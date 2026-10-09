# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen paired CALL/RET and fresh/reused sequences; invoked only by future controller."""

from __future__ import annotations

from collections.abc import Callable
from contextlib import ExitStack
from dataclasses import asdict
from typing import TextIO

from .cli import run_case
from .model import Case, ExecResult
from .scoped_fixture_authority import canonical, case_digest
from .scoped_fixture_binding import Binding
from .seeding import make_case
from .t1550_runtime_domain import Domain, Paths, Provider, micro_domain
from .t1550_runtime_fixtures import PROGRAMS, make_provider_case
from .t1550_runtime_ordinary import ReferenceTrace, record_value

SEQUENCES = (
    ("r0", "r1", "r2", "r3"),
    ("r3", "r2", "r1", "r0"),
    ("p0", "r0", "p1", "r1"),
    ("p0", "p1", "p2", "r0"),
    ("p0", "p1", "p2", "p3", "p4", "r0"),
    ("p0", "p1", "p2", "p3", "p4", "r1", "r0", "r1"),
)


def sequence_cases(seed: int, sequence: tuple[str, ...]) -> tuple[tuple[Case, str], ...]:
    if sequence not in SEQUENCES:
        raise ValueError("unknown frozen isolation sequence")
    return tuple(
        (make_provider_case(seed, int(label[1:])), "t1550-synthetic-byte-table")
        if label[0] == "p"
        else (make_case(seed, int(label[1:]), 0x10000, len(PROGRAMS[0x10000])), "random")
        for label in sequence
    )


def bind(domain: Domain, provider: Provider, seed: int, paths: Paths) -> Binding:
    return Binding(domain, provider, seed, paths[0], overrides=paths[1], additions=paths[2])


def pair(
    case: Case,
    kind: str,
    binding: Binding,
    oracle: object,
    subject: object,
    observations: TextIO,
    rows: TextIO,
    label: str,
) -> tuple[ExecResult, ExecResult]:
    image = binding.domain.physical.source.image
    left = ReferenceTrace(oracle, image, "oracle", observations)
    right = ReferenceTrace(subject, image, "subject", observations)
    actual = run_case(left, right, case, scoped_fixture_binding=binding, scoped_fixture_kind=kind)
    row = dict(
        phase=label,
        index=case.index,
        case_sha256=case_digest(case),
        oracle=asdict(actual[0]),
        subject=asdict(actual[1]),
        independent_mismatches=left.mismatches + right.mismatches,
        fault_state_equality_claim=False,
    )
    rows.write(canonical(record_value(row)).decode() + "\n")
    rows.flush()
    if (
        left.calls != 1
        or right.calls != 1
        or any(
            value.fault in ("COUNT-LIMIT", "TIMEOUT", "SUBJECT-DIED", "NOFUNC", "NOT-RUN")
            for value in actual
        )
    ):
        raise ValueError("paired backend observation incomplete/infrastructure fault")
    if row["independent_mismatches"]:
        raise ValueError("paired returned-state/invalid-input independent reference mismatch")
    return actual


def run_micro(
    seed: int,
    domain: Domain,
    paths: Paths,
    oracle_factory: Callable[[], object],
    subject_factory: Callable[[], object],
    observations: TextIO,
    rows: TextIO,
) -> int:
    count = 0
    for root in (0x10080, 0x100F0):
        selected, provider = micro_domain(domain, root)
        binding = bind(selected, provider, seed, paths)
        oracle = oracle_factory()
        with subject_factory() as subject:
            for index in range(12):
                case = make_case(seed, index, root, len(PROGRAMS[root]))
                pair(
                    case, "random", binding, oracle, subject, observations, rows, f"micro-{root:x}"
                )
                count += 1
        binding.document()
    return count


def run_isolation(
    seed: int,
    domain: Domain,
    provider: Provider,
    paths: Paths,
    oracle_factory: Callable[[], object],
    subject_factory: Callable[[], object],
    observations: TextIO,
    rows: TextIO,
) -> int:
    count = 0
    for number, sequence in enumerate(SEQUENCES):
        fresh_binding = bind(domain, provider, seed, paths)
        reused_binding = bind(domain, provider, seed, paths)
        oracle = oracle_factory()
        with ExitStack() as stack:
            reused_subject = stack.enter_context(subject_factory())
            for case, kind in sequence_cases(seed, sequence):
                with subject_factory() as fresh_subject:
                    fresh = pair(
                        case,
                        kind,
                        fresh_binding,
                        oracle_factory(),
                        fresh_subject,
                        observations,
                        rows,
                        f"isolation-{number}-fresh",
                    )
                reused = pair(
                    case,
                    kind,
                    reused_binding,
                    oracle,
                    reused_subject,
                    observations,
                    rows,
                    f"isolation-{number}-reused",
                )
                # This compares each backend's reset against itself; it does NOT grant
                # fault equivalence between native and oracle or any proof credit.
                if fresh != reused:
                    raise ValueError("fresh/reused full observations differ")
                count += 2
        fresh_binding.document()
        reused_binding.document()
    return count
