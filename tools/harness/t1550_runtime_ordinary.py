# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual ordinary custom producer/full-reader route for the frozen synthetic root.

Execution is opt-in from the reviewed future controller; importing does no CPU work.
"""

from __future__ import annotations

from dataclasses import asdict, replace
from types import SimpleNamespace
from typing import TextIO

from tools.replace.manifest import ManifestEntry, manifest_sha
from tools.replace.proof_contract import validate_document

from . import cli
from .model import Case, CaseResult, ExecResult, Outcome
from .replacement import ReplacementJudge
from .scoped_fixture_authority import canonical, case_digest
from .seeding import SeedPolicy
from .selection import select
from .t1550_runtime_domain import Domain, Paths, Provider
from .t1550_runtime_reference import expected


def record_value(value: object) -> object:
    if isinstance(value, dict):
        return {key: record_value(item) for key, item in value.items()}
    if isinstance(value, tuple | list):
        return [record_value(item) for item in value]
    if isinstance(value, frozenset | set):
        return [record_value(item) for item in sorted(value)]
    return value


class ReferenceTrace:
    """Delegate to the real backend; independently check only returned full state.

    Faults remain real backend faults/nonverdicts. A reference None is never emitted
    as an ExecResult fault. Both-fault register/write equality is never asserted.
    """

    def __init__(self, backend: object, image: object, side: str, stream: TextIO) -> None:
        self.backend, self.image, self.side, self.stream = backend, image, side, stream
        self.calls = 0
        self.mismatches = []

    def __getattr__(self, name: str) -> object:
        return getattr(self.backend, name)

    def run(self, case: Case, *args: object, **kwargs: object) -> ExecResult:
        result = self.backend.run(case, *args, **kwargs)
        reference = expected(case, self.image)
        mismatch = None
        if reference is None:
            if result.fault is None:
                mismatch = "invalid-input-read-returned-clean"
        elif result.fault is not None:
            mismatch = "valid-input-faulted"
        elif (result.regs, result.writes) != reference:
            mismatch = "returned-full-GPR/memory-reference-difference"
        self.calls += 1
        row = dict(
            side=self.side,
            index=case.index,
            case_sha256=case_digest(case),
            fault=result.fault,
            reference_invalid=reference is None,
            mismatch=mismatch,
            observed=asdict(result),
            returned_state_checked=result.fault is None,
            fault_state_equality_claim=False,
        )
        self.stream.write(canonical(record_value(row)).decode() + "\n")
        self.stream.flush()
        if mismatch:
            self.mismatches.append(row)
        return result


def run_ordinary(
    *,
    seed: int,
    image: object,
    domain: Domain,
    provider: Provider,
    paths: Paths,
    entries: list[ManifestEntry],
    source_digest: str,
    oracle: object,
    subject: object,
    rows: TextIO,
    observations: TextIO,
) -> dict[str, object]:
    linked = [entry for entry in entries if entry.va == 0x10000]
    if len(linked) != 1 or (
        linked[0].name,
        linked[0].convention,
        linked[0].stack_args,
        linked[0].returns,
        linked[0].scratch,
        linked[0].register_inputs,
    ) != ("custom_byte_table", "cdecl", 0, "eax", (), None):
        raise ValueError("actual linked synthetic scalar ABI differs from frozen contract")
    domain = replace(domain, entry=linked[0])
    provider = replace(provider, custom_contracts=(domain,))
    judge = ReplacementJudge(linked)
    selection = select([(0x10000, 29)], image.code_at)
    if selection.skipped or len(selection.candidates) != 1:
        raise ValueError("frozen scalar root selection refused")
    recorded = []

    def record(result: CaseResult) -> None:
        recorded.append(result)
        rows.write(canonical(record_value(asdict(result))).decode() + "\n")
        rows.flush()

    traced_oracle = ReferenceTrace(oracle, image, "oracle", observations)
    traced_subject = ReferenceTrace(subject, image, "subject", observations)
    settings = SimpleNamespace(
        seed=seed,
        cases_per_function=600,
        edge_cases=True,
        live_call_closure=False,
        live_vector_state=False,
        no_fixture_providers=False,
        fixture_provider_selection=None,
        functions=paths[0],
        overrides=paths[1],
        additions=paths[2],
    )
    ran, aborted = cli.drive_replacement(
        settings,
        SeedPolicy(),
        image,
        traced_oracle,
        traced_subject,
        judge,
        SimpleNamespace(write_case=record),
        selection.candidates[0],
        0,
        registry=(provider,),
    )
    covered = set().union(*(result.reach.covered_vas for result in recorded))
    proof = judge.document(
        manifest_sha=manifest_sha(entries, source_digest),
        seed=seed,
        cases_per_function=600,
        edge_cases=True,
        subject=asdict(subject.provenance),
        reach={0x10000: (len(covered) / 6, 6, len(covered) < 5)},
    )
    reader_error = None
    try:
        validate_document(proof)
    except ValueError as error:
        reader_error = str(error)
    result = dict(
        ran=ran,
        aborted=aborted,
        proof=proof,
        reader_refusal=reader_error,
        oracle_calls=traced_oracle.calls,
        subject_calls=traced_subject.calls,
        independent_mismatches=traced_oracle.mismatches + traced_subject.mismatches,
        outcomes={
            str(outcome): sum(row.outcome == outcome for row in recorded) for outcome in Outcome
        },
        covered_vas=sorted(covered),
    )
    # CapFALSE is intentional: metric success cannot be accepted evidence.
    if reader_error is None or "unvalidated local custom" not in reader_error:
        result["infrastructure_failure"] = "missing/wrong required local-capability reader refusal"
    if ran != 776 or aborted or len(recorded) != 776:
        result["infrastructure_failure"] = "frozen ordinary count/termination mismatch"
    return result
