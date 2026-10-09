# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual future execution routes for frozen guard controls 10 through 13.

Importing this module creates no backend. Fatal safety errors are retained as
campaign failures, never converted to guest faults or passing comparisons.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import asdict, replace
from typing import TextIO

from .cli import run_case
from .code_safety import NamedGlobalCodeSafetyError
from .model import ExecResult
from .scoped_fixture_authority import canonical, case_digest
from .seeding import SENTINEL, make_case
from .t1550_runtime_domain import Domain, Paths, micro_domain
from .t1550_runtime_guard_capture import capture
from .t1550_runtime_ordinary import record_value
from .t1550_runtime_pairs import bind


class SubjectMustNotRun:
    def __init__(self) -> None:
        self.calls = 0

    def run(self, *args: object, **kwargs: object) -> ExecResult:
        self.calls += 1
        raise AssertionError("fatal actual guard reached subject invocation")


def run_fatal(
    control: int,
    seed: int,
    base: Domain,
    paths: Paths,
    oracle_factory: Callable[[], object],
    rows: TextIO,
) -> dict[str, object]:
    roots = {10: 0x10200, 11: 0x10220, 12: 0x10240}
    reasons = {10: "bytes changed", 11: "return target mismatch", 12: "return slot ESP mismatch"}
    if control not in roots:
        raise ValueError("unknown frozen fatal execution control")
    root = roots[control]
    domain, provider = micro_domain(base, root)
    binding = bind(domain, provider, seed, paths)
    case = make_case(seed, 0, root, domain.size)
    if control == 12:
        # Preserve the frozen directed decoy target. This is an ordinary baseline
        # frame patch, never a provider-stream substitution or removed case.
        patches = list(case.patches)
        address, frame = patches[0]
        if address != case.esp or len(frame) < 8:
            raise ValueError("frozen directed frame shape differs")
        patches[0] = (address, frame[:4] + SENTINEL.to_bytes(4, "little") + frame[8:])
        case = replace(case, patches=tuple(patches))
    oracle, subject = oracle_factory(), SubjectMustNotRun()
    row = dict(
        control=control, case_sha256=case_digest(case), expected="campaign-fatal", passed=False
    )
    try:
        run_case(
            oracle, subject, case, scoped_fixture_binding=binding, scoped_fixture_kind="random"
        )
    except NamedGlobalCodeSafetyError as error:
        row["fatal"] = capture(oracle, error) if control == 10 else dict(violation=error.observed)
        if reasons[control] not in str(error) or subject.calls:
            raise ValueError("wrong fatal control reason or subject invoked") from error
        row["passed"] = True
    except BaseException as error:
        row["infrastructure_error"] = repr(error)
        raise
    finally:
        row["subject_invocations"] = subject.calls
        rows.write(canonical(record_value(row)).decode() + "\n")
        rows.flush()
    if not row["passed"]:
        raise ValueError("expected actual guard fatal was not observed")
    return row


def run_unreadable_ret(
    seed: int,
    base: Domain,
    paths: Paths,
    oracle_factory: Callable[[], object],
    subject_factory: Callable[[], object],
    rows: TextIO,
) -> dict[str, object]:
    domain, provider = micro_domain(base, 0x10260)
    binding = bind(domain, provider, seed, paths)
    case = make_case(seed, 0, domain.entry.va, domain.size)
    row = dict(control=13, case_sha256=case_digest(case), proof_credit=False, passed=False)
    try:
        with subject_factory() as subject:
            results = run_case(
                oracle_factory(),
                subject,
                case,
                scoped_fixture_binding=binding,
                scoped_fixture_kind="random",
            )
        row["observed"] = [asdict(result) for result in results]
        if any(result.fault is None for result in results) or any(
            result.fault in ("COUNT-LIMIT", "TIMEOUT", "SUBJECT-DIED", "NOFUNC", "NOT-RUN")
            for result in results
        ):
            raise ValueError("unreadable RET did not retain two real backend faults")
        row["passed"] = True
    except BaseException as error:
        row["infrastructure_error"] = repr(error)
        raise
    finally:
        rows.write(canonical(record_value(row)).decode() + "\n")
        rows.flush()
    return row
