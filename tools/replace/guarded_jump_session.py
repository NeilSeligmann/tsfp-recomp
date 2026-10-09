# SPDX-License-Identifier: GPL-3.0-or-later
"""T1576 per-case guarded-jump evidence inside the replacement proof loop (opt-in only).

Created by `tools.harness.cli` when the guarded-jump contract is selected and the root has a
jump table or an out-of-body tail jmp. For every case the proof loop already ran (oracle,
subject, judge) it

1. takes the ORIGINAL arm and tail events from the oracle trace (dispatch slot, JA default,
   executed tail JMP),
2. runs the same Case on a second subject built from the same draft at O0 under clang source
   coverage (the WITNESS, driver.c HARNESS_WITNESS) and reads which `case`/`default` regions of
   the draft's entry switch ran and whether the typed tail adapter ran,
3. checks per-case hit-set equality (original coverage-derived set vs replacement region set,
   both expanded to groups sharing a body, arm_witness.arm_parity), per-case tail-taken equality,
   and that the witness and the shipped subject returned the same state, and
4. accumulates campaign coverage: every declared slot, the default and every tail site must have
   been DISPATCHED/TAKEN (oracle events) on a case that reached a genuine AGREE verdict.

Nothing is written outside the returned document. A missing profile, a witness death, a
refusal from `arm_witness.refusals` or an empty stream all count as failures, never as skips.
"""

from __future__ import annotations

import hashlib
import os
from collections.abc import Iterable
from pathlib import Path
from typing import Any

from tools.harness.guarded_tables import GuardedJumpTable
from tools.harness.model import Case, ExecResult
from tools.harness.subject import SubjectError, SubjectProcess
from tools.replace import arm_witness as aw
from tools.replace.tail_adapter import TailSite

PROFILE_ENV = "HARNESS_WITNESS_FILE"
PROTOCOL = "t1576-arm-witness-v1"
TAIL_ADAPTER = "game_guest_tail"


def original_table(table: GuardedJumpTable) -> aw.OriginalTable:
    """The ORIGINAL table as the witness sees it: slot n is case n (identity, mutation tested)."""
    return aw.OriginalTable(table.targets, table.default_target, tuple(range(len(table.targets))))


def event_hits(table: GuardedJumpTable, events: Iterable[tuple]) -> frozenset[str]:
    """Dispatched arms from the oracle trace of ONE case for ONE table."""
    out: set[str] = set()
    for kind, site, slot, _target in events:
        if kind == "slot" and site == table.site:
            out.add(f"case:{slot}")
        elif kind == "default" and site == table.site:
            out.add("default")
    return frozenset(out)


class GuardedSession:
    def __init__(
        self,
        *,
        root: int,
        tables: tuple[GuardedJumpTable, ...],
        tails: tuple[TailSite, ...] = (),
        draft_path: Path,
        entry_function: str,
        source_function: str,
        witness_binary: Path,
        image_path: Path,
        work_dir: Path,
    ) -> None:
        if len(tables) > 1:
            raise aw.WitnessRefusal(f"at most one root jump table supported, got {len(tables)}")
        if not tables and not tails:
            raise aw.WitnessRefusal("no root jump table or tail: nothing to witness")
        self.root = root
        self.table = tables[0] if tables else None
        self.original = original_table(self.table) if self.table else None
        self.tails = tails
        self.draft_path = draft_path
        self.source = draft_path.read_text(encoding="utf-8")
        self.entry_function = entry_function
        self.source_function = source_function
        self.binary = witness_binary
        self.work = work_dir
        self.work.mkdir(parents=True, exist_ok=True)
        self.profile = self.work / "case.profraw"
        os.environ[PROFILE_ENV] = str(self.profile)
        os.environ["LLVM_PROFILE_FILE"] = os.devnull
        os.environ["HARNESS_FAULT_ADDR"] = "1"
        self.subject = SubjectProcess(witness_binary, image_path)
        self.failures: list[dict[str, Any]] = []
        self.cases = 0
        self.compared = 0
        self.reached: set[str] = set()
        self.dispatched: set[str] = set()
        self.tail_taken: set[int] = set()
        self.groups: list[frozenset[str]] = []
        self.refusals: list[str] = []
        self.faulted_cases = 0

    def attach(self, oracle: Any) -> None:
        """Tell the oracle which JMP sites are tails so its trace records them."""
        oracle.tail_sites.update({site.site: site.target for site in self.tails})

    def _fail(self, case: Case, kind: str, detail: str) -> None:
        self.failures.append({"index": case.index, "kind": kind, "detail": detail[:300]})

    def _witness(self, case: Case) -> tuple[ExecResult, frozenset[str] | None, bool | None]:
        """(witness result, replacement arm hit set or None, tail adapter ran or None)."""
        self.profile.unlink(missing_ok=True)
        try:
            result = self.subject.run(case)
        except SubjectError as error:
            return ExecResult(fault=f"witness-refusal:{error}"), None, None
        if not self.profile.exists():
            if result.fault is None:
                self._fail(case, "witness-no-profile", "driver wrote no profile")
            return result, None, None
        functions = aw.export_all(self.binary, self.profile)
        hits: frozenset[str] | None = None
        if self.table is not None:
            entry = aw.find_function(functions, self.entry_function)
            labels = aw.static_labels(self.source, entry, filename=self.draft_path.name)
            if not self.groups:
                self.refusals = aw.refusals(self.source, labels, self.source_function)
                self.groups = aw.groups(self.source, labels)
            hits = aw.reached_labels(labels, self.groups)
        tail_ran: bool | None = None
        if self.tails:
            tail_ran = (
                sum(
                    entry["count"]
                    for name, entry in functions.items()
                    if name == TAIL_ADAPTER or name.endswith(f":{TAIL_ADAPTER}")
                )
                > 0
            )
        return result, hits, tail_ran

    def observe(
        self,
        case: Case,
        oracle: Any,
        oracle_result: ExecResult,
        subject_result: ExecResult,
        agree: bool,
    ) -> None:
        self.cases += 1
        witness_result, replacement, tail_ran = self._witness(case)
        if oracle_result.fault is not None or subject_result.fault is not None:
            self.faulted_cases += 1
            # Faults never count as coverage. The shipped subject and the witness must still
            # fault the same way (same label), so a witness build cannot hide a fault.
            if witness_result.fault != subject_result.fault:
                self._fail(case, "witness-fault-differs", f"{witness_result.fault!r}")
            return
        if (self.table is not None and replacement is None) or (self.tails and tail_ran is None):
            self._fail(case, "witness-unavailable", str(witness_result.fault))
            return
        for field_name in ("regs", "writes", "xmm", "mxcsr"):
            if getattr(witness_result, field_name) != getattr(subject_result, field_name):
                self._fail(case, "witness-not-faithful", field_name)
                break
        self.compared += 1
        if self.table is not None and self.original is not None and replacement is not None:
            covered = oracle_result.reach.covered_vas
            original = aw.expand_original(
                aw.original_arm_hits(self.original, covered), self.original
            )
            if original != replacement:
                self._fail(
                    case,
                    "arm-hit-set",
                    f"original={sorted(original)} replacement={sorted(replacement)}",
                )
            if agree:
                self.reached |= replacement
                self.dispatched |= event_hits(self.table, oracle.arm_events)
        if self.tails:
            taken = {event[1] for event in oracle.arm_events if event[0] == "tail"}
            if bool(taken) != bool(tail_ran):
                self._fail(case, "tail-taken", f"original={sorted(taken)} replacement={tail_ran}")
            if agree:
                self.tail_taken |= taken

    def close(self) -> None:
        self.subject.close()

    def document(self) -> dict[str, Any]:
        failures: list[dict[str, Any]] = []  # campaign-level first, so truncation keeps them
        missing: list[str] = []
        if self.table is not None:
            declared = {f"case:{n}" for n in range(len(self.table.targets))} | {"default"}
            missing = sorted(declared - self.dispatched)
        if not self.compared:
            failures.append({"index": -1, "kind": "empty-stream", "detail": "no compared case"})
        for reason in self.refusals:
            failures.append({"index": -1, "kind": "draft-refusal", "detail": reason})
        if missing:
            failures.append(
                {"index": -1, "kind": "arm-not-dispatched", "detail": ",".join(missing)}
            )
        untaken = sorted(site.site for site in self.tails if site.site not in self.tail_taken)
        if untaken:
            failures.append(
                {
                    "index": -1,
                    "kind": "tail-not-taken",
                    "detail": ",".join(f"0x{v:08x}" for v in untaken),
                }
            )
        failures.extend(self.failures)
        document: dict[str, Any] = {
            "protocol": PROTOCOL,
            "root": f"0x{self.root:08x}",
            "draft_sha256": hashlib.sha256(self.source.encode("utf-8")).hexdigest(),
            "witness_binary_sha256": hashlib.sha256(self.binary.read_bytes()).hexdigest(),
            "entry_function": self.entry_function,
            "cases": self.cases,
            "compared_cases": self.compared,
            "faulted_cases": self.faulted_cases,
            "dispatched_arms": sorted(self.dispatched),
            "reached_arms": sorted(self.reached),
            "undispatched_arms": missing,
            "failures": failures[:50],
            "failure_count": len(failures),
            "passed": not failures,
        }
        if self.table is not None:
            document["table"] = self.table.document()
        if self.tails:
            document["tail_sites"] = [site.document() for site in self.tails]
            document["tails_taken"] = [f"0x{v:08x}" for v in sorted(self.tail_taken)]
        return document


def entry_function_name(registration: Any) -> str:
    """The function the compiler maps: the EXACT body is the static `game_body_<VA>`."""
    if registration.exact_registers:
        return f"game_body_{registration.va:08X}"
    return str(registration.function)
