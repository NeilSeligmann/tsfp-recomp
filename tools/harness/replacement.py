# SPDX-License-Identifier: GPL-3.0-or-later
"""Judging hand-written replacements: the per-function evidence a proof is made of.

The differential harness normally asks "does the lifted C behave like the original
machine code?". In replacement mode the subject is a binary with hand-written C linked
over the lifted functions (see docs/decompilation-workflow.md) and the question is the
same one asked of the hand-written code. This module holds what is specific to that:

* WHICH REGISTERS ARE COMPARED. All eight for a lifted function. A replacement declares
  the registers its convention makes caller-saved scratch (ecx and edx, plus eax when it
  returns nothing) and those are not compared, because a readable C function cannot
  reproduce whatever the original left in them. That is sound only if no caller reads
  them, which `tools/replace audit` checks over every direct call site.
* WHICH WRITES ARE COMPARED. All of them, except those into the callee's own dead stack
  frame (a page below the entry `esp`): the original's `push edi ... pop edi` leaves the saved
  value in memory below `esp`, a readable C function does not, and nothing may read it.
* THE NULL MODEL. Every verdict is also computed for two functions that do nothing: one
  returning 0, one returning 1, writing no memory and preserving every register. The
  fraction of verdict cases a do-nothing function would also have passed is
  `null_agree_rate`. A high rate means the inputs cannot tell a correct replacement from
  an empty one, whatever the AGREE count says.
* THE PROOF FILE. One JSON document per run: seed, sampling depth, edge cases, the three
  provenance digests, and for each registered function its counts, union coverage over
  the ORIGINAL bytes, null rate, whether the dispatch table was seen to reach the adapter,
  and the registers ignored. `tools/coverage.py` reads it and applies the gates.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from tools.replace.input_abi import input_contract
from tools.replace.manifest import ManifestEntry
from tools.replace.proof_contract import VECTOR_SCHEMA, validate_closure, vector_enabled

from .compare import compare
from .fixture_selection import selection
from .model import REG_NAMES, VERDICT_OUTCOMES, Case, CaseResult, ExecResult, Outcome
from .named_global_objects import ROW_FIELD, attach_context
from .scoped_fixture_proof import attach_context as attach_custom_fixture_context
from .synth_domain import LABEL as SYNTH_DOMAIN_LABEL

PROOF_SCHEMA = 1
#: The two constants a do-nothing function returns. Both are tried because a function that
#: returns 1 on its common path would be flattered by a null that returns 0.
NULL_RETURN_VALUES = (0, 1)
SUBJECT_FAULT_NOT_REPLACED = "NOT-REPLACED"
#: How far below the entry `esp` a callee's own frame is assumed to reach. Everything the
#: original pushes or spills lives there and is dead the moment it returns, so a readable C
#: function cannot and need not reproduce it. A leaf's frame is a few words, and a frame
#: past a page needs a stack probe the original would show, so a page is ample.
DEAD_FRAME_BYTES = 0x1000
REGISTER_ARGUMENTS = {"cdecl": 0, "stdcall": 0, "thiscall": 1, "fastcall": 2}
CALLEE_POPS = {"cdecl": False, "stdcall": True, "thiscall": True, "fastcall": True}


def register_arguments(entry: ManifestEntry) -> int:
    return len(register_argument_names(entry))


def register_argument_names(entry: ManifestEntry) -> tuple[str, ...]:
    if entry.register_inputs is not None:
        return entry.register_inputs
    return {"cdecl": (), "stdcall": (), "thiscall": ("ecx",), "fastcall": ("ecx", "edx")}[
        entry.convention
    ]


def ignored_registers(entry: ManifestEntry) -> frozenset[str]:
    """Registers a replacement does not promise to match, by name."""
    return frozenset(entry.scratch)


def popped_bytes(entry: ManifestEntry) -> int:
    """Bytes the original's `ret` pops beyond nothing: the return address and, for a
    callee-cleanup convention, the stack arguments."""
    return 4 + (4 * entry.stack_args if CALLEE_POPS[entry.convention] else 0)


def null_result(case: Case, entry: ManifestEntry, eax: int) -> ExecResult:
    """What a function that does nothing would leave: registers kept, `ret` performed."""
    regs = list(case.regs)
    esp = REG_NAMES.index("esp")
    regs[esp] = (regs[esp] + popped_bytes(entry)) & 0xFFFFFFFF
    regs[REG_NAMES.index("eax")] = eax
    return ExecResult(
        regs=tuple(regs),
        writes={},
        xmm=case.xmm,
        mxcsr=case.mxcsr if case.xmm is not None else None,
    )


@dataclass
class FunctionEvidence:
    """Everything one registered function accumulated over its cases."""

    entry: ManifestEntry
    cases: int = 0
    #: Cases by how they were made: "random", "edge", "feedback".
    by_kind: dict[str, int] = field(default_factory=dict)
    #: Verdict cases (AGREE, DISAGREE, SUBJECT_FAULTED) by the same kinds.
    verdicts_by_kind: dict[str, int] = field(default_factory=dict)
    agree: int = 0
    disagree: int = 0
    subject_faulted: int = 0
    oracle_faulted: int = 0
    replaced_seen: int = 0
    not_replaced: int = 0
    null_agree: list[int] = field(default_factory=lambda: [0] * len(NULL_RETURN_VALUES))
    #: The most oracle write bytes any one case put in the dead stack frame below the entry
    #: `esp`, which are dropped from the write-set comparison. Reported so the exemption is
    #: visible: zero means the original never spilled, 4 means it saved one register.
    dead_frame_bytes_ignored: int = 0
    #: Why the harness could not run this function at all, when it could not.
    unjudgeable: str | None = None
    #: Distinct first divergences, kept short, for the summary.
    divergence_examples: list[str] = field(default_factory=list)
    named_global_object_contract: dict[str, object] | None = None
    named_global_code_safety: dict[str, object] | None = None
    scoped_fixture_contract: dict[str, object] | None = None
    scoped_fixture_runtime: dict[str, object] | None = None
    #: T1773 synthesized-domain identity for this root. None (and never serialised) by default.
    synth_domain: dict[str, object] | None = None

    @property
    def verdicts(self) -> int:
        return self.agree + self.disagree + self.subject_faulted

    @property
    def null_agree_rate(self) -> float:
        if not self.verdicts:
            return 1.0
        return max(self.null_agree) / self.verdicts

    @property
    def discriminating_cases(self) -> int:
        """Verdict cases a do-nothing function would have FAILED, the better of the two.

        This, not the rate, is what says how many cases could have caught an empty or
        constant replacement. A rate falls as random cases are added, which would reward
        padding a proof with cases that cannot tell anything apart.
        """
        return self.verdicts - (max(self.null_agree) if self.verdicts else 0)

    @property
    def replaced_confirmed(self) -> bool:
        return self.replaced_seen > 0 and self.not_replaced == 0


class ReplacementJudge:
    """Scores cases for the registered replacements and writes the proof file."""

    def __init__(self, entries: list[ManifestEntry], *, require_fault_parity: bool = False) -> None:
        self.evidence = {entry.va: FunctionEvidence(entry) for entry in entries}
        #: T1576 guarded-jump contract only: matched faults need the same guest address.
        self.require_fault_parity = require_fault_parity

    def mark_unjudgeable(self, va: int, reason: str) -> None:
        self.evidence[va].unjudgeable = reason

    def judge(
        self,
        case: Case,
        oracle: ExecResult,
        subject: ExecResult,
        *,
        body_insns: int,
        kind: str,
    ) -> CaseResult:
        """Compare one case and fold it into its function's evidence."""
        evidence = self.evidence[case.va]
        entry = evidence.entry
        ignored = ignored_registers(entry)
        dead_frame = ((case.esp - DEAD_FRAME_BYTES, case.esp),)
        result = compare(
            case,
            oracle,
            subject,
            body_insns=body_insns,
            ignored_registers=ignored,
            ignored_write_ranges=dead_frame,
            require_fault_parity=self.require_fault_parity,
        )
        ignored_bytes = sum(1 for a in oracle.writes if dead_frame[0][0] <= a < dead_frame[0][1])
        evidence.dead_frame_bytes_ignored = max(evidence.dead_frame_bytes_ignored, ignored_bytes)
        evidence.cases += 1
        evidence.by_kind[kind] = evidence.by_kind.get(kind, 0) + 1
        if subject.fault == SUBJECT_FAULT_NOT_REPLACED:
            evidence.not_replaced += 1
        elif subject.replaced:
            evidence.replaced_seen += 1
        if result.outcome in (Outcome.AGREE, Outcome.DISAGREE, Outcome.SUBJECT_FAULTED):
            evidence.verdicts_by_kind[kind] = evidence.verdicts_by_kind.get(kind, 0) + 1
        if result.outcome is Outcome.AGREE:
            evidence.agree += 1
        elif result.outcome is Outcome.DISAGREE:
            evidence.disagree += 1
        elif result.outcome is Outcome.SUBJECT_FAULTED:
            evidence.subject_faulted += 1
        elif result.outcome is Outcome.ORACLE_FAULTED:
            evidence.oracle_faulted += 1
        if result.outcome in (Outcome.DISAGREE, Outcome.SUBJECT_FAULTED) and (
            len(evidence.divergence_examples) < 3
        ):
            evidence.divergence_examples.append(f"case {case.index}: {result.diagnosis}")
        if result.outcome in VERDICT_OUTCOMES:
            for slot, value in enumerate(NULL_RETURN_VALUES):
                null = compare(
                    case,
                    oracle,
                    null_result(case, entry, value),
                    body_insns=body_insns,
                    ignored_registers=ignored,
                    ignored_write_ranges=dead_frame,
                )
                if null.outcome is Outcome.AGREE:
                    evidence.null_agree[slot] += 1
        return result

    @property
    def not_replaced_functions(self) -> list[int]:
        return [va for va, evidence in self.evidence.items() if evidence.not_replaced]

    def document(
        self,
        *,
        manifest_sha: str,
        seed: int,
        cases_per_function: int,
        edge_cases: bool,
        subject: dict[str, str],
        reach: dict[int, tuple[float, int, bool]],
        live_call_closure: dict[str, object] | None = None,
        fixture_provider_selection: tuple[str, ...] | None = None,
        synth_domain: bool = False,
    ) -> dict[str, object]:
        """The proof JSON. `reach[va]` is `(coverage, body_insns, near_vacuous)`."""
        fixture_provider_selection = selection(fixture_provider_selection)
        vector = vector_enabled(live_call_closure)
        if vector:
            assert live_call_closure is not None
            validate_closure(live_call_closure, live_call_closure["root"])
        functions = []
        for va in sorted(self.evidence):
            evidence = self.evidence[va]
            coverage, body_insns, near_vacuous = reach.get(va, (0.0, 0, True))
            functions.append(
                {
                    "va": f"0x{va:08x}",
                    "name": evidence.entry.name,
                    "cases": evidence.cases,
                    "random_cases": evidence.by_kind.get("random", 0),
                    "edge_cases": evidence.by_kind.get("edge", 0),
                    "string_cases": evidence.by_kind.get("string", 0),
                    "tls_cases": evidence.by_kind.get("tls", 0),
                    "table_cases": evidence.by_kind.get("table", 0),
                    "feedback_cases": evidence.by_kind.get("feedback", 0),
                    "verdicts": evidence.verdicts,
                    "agree": evidence.agree,
                    "disagree": evidence.disagree,
                    "subject_faulted": evidence.subject_faulted,
                    "oracle_faulted": evidence.oracle_faulted,
                    "coverage": round(coverage, 4),
                    "body_insns": body_insns,
                    "near_vacuous": near_vacuous,
                    "null_agree_rate": round(evidence.null_agree_rate, 4),
                    "discriminating_cases": evidence.discriminating_cases,
                    "replaced_confirmed": evidence.replaced_confirmed,
                    "regs_ignored": sorted(evidence.entry.scratch),
                    "dead_frame_bytes_ignored": evidence.dead_frame_bytes_ignored,
                    "unjudgeable": evidence.unjudgeable,
                    "divergence_examples": evidence.divergence_examples,
                }
            )
        for row in functions:
            evidence = self.evidence[int(row["va"], 16)]
            entry = evidence.entry
            if evidence.named_global_object_contract is not None:
                row[ROW_FIELD] = evidence.named_global_object_contract
            if evidence.named_global_code_safety is not None:
                row["named_global_code_safety"] = evidence.named_global_code_safety
            if evidence.scoped_fixture_contract is not None:
                row["scoped_fixture_contract"] = evidence.scoped_fixture_contract
                row["scoped_fixture_entry"] = entry.as_json()
            if evidence.scoped_fixture_runtime is not None:
                row["scoped_fixture_runtime"] = evidence.scoped_fixture_runtime
            if evidence.synth_domain is not None:
                row["synth_domain"] = {
                    **evidence.synth_domain,
                    "cases": evidence.by_kind.get(SYNTH_DOMAIN_LABEL, 0),
                    "verdicts": evidence.verdicts_by_kind.get(SYNTH_DOMAIN_LABEL, 0),
                }
            contract = input_contract(entry.register_inputs, entry.convention, entry.stack_args)
            if contract is not None:
                row["input_contract"] = contract
        if fixture_provider_selection is not None:
            for row in functions:
                row["fixture_provider_selection"] = list(fixture_provider_selection)
        if vector:
            for row in functions:
                selected = row["va"] == live_call_closure["root"]
                row["state_contract"] = live_call_closure["state_contract"] if selected else None
                row["source_closure"] = live_call_closure if selected else None
                row["source_seed"] = seed if selected else None
        document = {
            "schema": VECTOR_SCHEMA if vector else PROOF_SCHEMA,
            "kind": "replacement-proof",
            "manifest_sha": manifest_sha,
            "seed": seed,
            "cases_per_function": cases_per_function,
            "edge_cases": edge_cases,
            "subject": subject,
            "live_call_closure": live_call_closure,
            "functions": functions,
        }
        if fixture_provider_selection is not None:
            document["fixture_provider_selection"] = list(fixture_provider_selection)
        if synth_domain:
            document["synth_domain"] = True
        attach_context(document)
        attach_custom_fixture_context(document)
        if vector:
            document["state_contract_schema"] = 1
        return document


def write_proof(path: Path, document: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
