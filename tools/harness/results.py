# SPDX-License-Identifier: GPL-3.0-or-later
"""Record every verdict to a CSV as it is reached, and summarise honestly.

Rows are flushed as they are produced rather than accumulated and written at the end.
A scaled run is long, and the previous attempt at one lost everything twice -- once to
a subprocess timeout and once to being stopped at ~85 minutes with no output. A run
that is interrupted must still leave behind exactly the evidence it had earned.

Skipped functions are written into the SAME file as executed cases, with the reason in
the diagnosis column. Keeping them in a separate place, or not writing them at all, is
how a reader ends up dividing agreements by a denominator that silently excludes
everything hard.

Output belongs in gitignored `generated/harness/`: the diagnosis column names addresses
in the user's own binary, and `tools/ci/check-no-disc-data.sh` rejects bulk per-address
dumps by content.
"""

from __future__ import annotations

import csv
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path
from types import TracebackType
from typing import Any, TextIO

from .model import (
    DIVERGENT_OUTCOMES,
    FLAG_BITS,
    VERDICT_OUTCOMES,
    CaseResult,
    Outcome,
    SkippedFunction,
)
from .provenance import Provenance

#: `gen_dir` and `tree_sha` come FIRST, before even the seed, and are on every row
#: including the skips. This is the whole point: the file used to record the seed and the
#: case index, which made any single case perfectly reproducible, while saying nothing
#: about WHICH LIFTED TREE the subject was built from. Three answers were published off
#: this file and retracted -- "0 DISAGREE" used as the gate for adopting upstream lifter
#: patches, then "384 DISAGREE, a real lifter defect", then "the defect was already
#: fixed; the subject predated it by ten hours". Per row rather than in a header comment
#: so that `grep DISAGREE results.csv`, a row pasted into a document, and two CSVs
#: concatenated all still carry it.
CSV_HEADER = (
    "gen_dir",
    "tree_sha",
    "seed",
    "case_index",
    "va",
    "size_bytes",
    "outcome",
    "oracle_fault",
    "subject_fault",
    "divergence_kinds",
    "stubbed",
    "insns",
    "body_insns",
    "covered_insns",
    "seh_ebp",
    "seh_ebp_drifted",
    "diagnosis",
    # Appended, not prepended, so a reader that indexes by position keeps working.
    # `subject_kind` says whether the subject under test was the lifted C ("lifted") or
    # hand-written replacements linked over it ("replacement"). `repl_sha` is the digest of
    # src/game when the replacement subject was built, `NONE` otherwise. `sampling` is the
    # random cases per function the run asked for, so a row quoted on its own still says
    # how deep the evidence behind it goes (counts at different depths are not comparable).
    "subject_kind",
    "repl_sha",
    "sampling",
    "passthrough_applied",
    # The EFLAGS channel (opt-in --compare-eflags runs). Hex masks; empty on a
    # run without the channel, so old and new CSVs stay column-compatible by
    # position for everything before them.
    "flags_compared_mask",
    "flags_disagree_mask",
    "flags_unmodeled_mask",
    "fault_diagnostic",
)

#: A case that executed fewer than this many instructions did little more than return.
#: Five covers a `mov eax, [esp+4]; test; jz; ret` early exit, which is the shape that
#: produces an AGREE meaning almost nothing.
SHALLOW_INSNS = 5

#: Union coverage below this, across ALL of a function's cases, means most of the body
#: was never executed by any case.
LOW_COVERAGE = 0.25

#: At or above this delegation ratio a stubbed function is essentially "marshal the
#: arguments, call, return the result", so a verdict with the callee stubbed out is close
#: to vacuous.
HIGH_DELEGATION = 0.7


@dataclass
class FunctionReach:
    """Per-function reachability, accumulated across that function's cases.

    This exists because AGREE overstates coverage. Two mutations once survived the
    harness because the mutated line sat behind a branch gated on a guest global that
    loads as 0, so the code was dead on all 32 cases and every case agreed. Counting
    those agreements without saying how little ran is how a harness flatters itself.
    """

    body_insns: int = 0
    covered: set[int] = field(default_factory=set)
    insn_counts: list[int] = field(default_factory=list)
    agrees: int = 0
    verdicts: int = 0
    stubbed: bool = False
    delegation_ratio: float = 0.0
    stub_applied: int = 0

    @property
    def coverage(self) -> float:
        """Union coverage over every case, not the best single case."""
        if not self.body_insns:
            return 0.0
        return len(self.covered) / self.body_insns

    @property
    def max_insns(self) -> int:
        return max(self.insn_counts, default=0)

    @property
    def shallow(self) -> bool:
        """Every single case returned almost immediately."""
        return bool(self.insn_counts) and self.max_insns < SHALLOW_INSNS

    @property
    def low_coverage(self) -> bool:
        return bool(self.body_insns) and self.coverage < LOW_COVERAGE

    @property
    def delegating(self) -> bool:
        return self.stubbed and self.delegation_ratio >= HIGH_DELEGATION

    @property
    def near_vacuous(self) -> bool:
        """The function reached a verdict, but that verdict is weak evidence."""
        return self.shallow or self.low_coverage or self.delegating


@dataclass
class Tally:
    """Running counts. `total` is every row written, so nothing can go missing."""

    outcomes: dict[Outcome, int] = field(default_factory=dict)
    skip_reasons: dict[str, int] = field(default_factory=dict)
    divergent_functions: dict[int, list[str]] = field(default_factory=dict)
    functions_executed: set[int] = field(default_factory=set)
    reach: dict[int, FunctionReach] = field(default_factory=dict)

    #: The EFLAGS channel (opt-in). Per-flag counts over the cases where BOTH
    #: sides supplied a flags word. `flags_unmodeled` is the honest remainder:
    #: TRACKED bits the executed exit's publish mask did not claim (AF always),
    #: counted rather than silently skipped. `flags_unpublished_cases` counts
    #: flags-observed cases whose exit published nothing at all (mask 0).
    flags_observed_cases: int = 0
    flags_unpublished_cases: int = 0
    flags_compared: dict[str, int] = field(default_factory=dict)
    flags_disagree: dict[str, int] = field(default_factory=dict)
    flags_unmodeled: dict[str, int] = field(default_factory=dict)

    #: Cases where the subject left `g_seh_ebp` disagreeing with its own final `ebp`.
    #: An observation with no oracle counterpart, so it is counted and reported and
    #: never allowed to change an outcome. See `compare._seh_ebp_drifted`.
    seh_ebp_drifts: int = 0
    #: Cases where the subject reported a `g_seh_ebp` at all, i.e. the denominator the
    #: drift count is meaningful against. A subject too old to report it gives zero, and
    #: zero drifts out of zero observations must not read as "never stale".
    seh_ebp_observed: int = 0

    def add_case(self, result: CaseResult) -> None:
        self.outcomes[result.outcome] = self.outcomes.get(result.outcome, 0) + 1
        self.functions_executed.add(result.va)
        if result.flags_observed:
            self.flags_observed_cases += 1
            if result.flags_unpublished:
                self.flags_unpublished_cases += 1
            for bit, label in FLAG_BITS:
                if result.flags_compared_mask & bit:
                    self.flags_compared[label] = self.flags_compared.get(label, 0) + 1
                    if result.flags_disagree_mask & bit:
                        self.flags_disagree[label] = self.flags_disagree.get(label, 0) + 1
                if result.flags_unmodeled_mask & bit:
                    self.flags_unmodeled[label] = self.flags_unmodeled.get(label, 0) + 1
        if result.subject_seh_ebp is not None:
            self.seh_ebp_observed += 1
            if result.seh_ebp_drifted:
                self.seh_ebp_drifts += 1
        self._add_reach(result)
        if result.outcome in DIVERGENT_OUTCOMES:
            self.divergent_functions.setdefault(result.va, []).append(result.diagnosis)
        if result.outcome is Outcome.ORACLE_FAULTED and result.oracle_fault:
            key = f"oracle:{result.oracle_fault}"
            self.skip_reasons[key] = self.skip_reasons.get(key, 0) + 1
        elif result.outcome is Outcome.SKIPPED_UNSUPPORTED:
            key = f"subject:{result.subject_fault or 'unsupported'}"
            self.skip_reasons[key] = self.skip_reasons.get(key, 0) + 1

    def _add_reach(self, result: CaseResult) -> None:
        """Fold one case into its function's reachability record.

        Faulted and no-verdict cases are folded in too: they still say how far the
        function got, and a function whose cases all fault after two instructions has not
        been tested either.
        """
        if result.outcome is Outcome.SKIPPED_UNSUPPORTED:
            return
        entry = self.reach.setdefault(result.va, FunctionReach())
        if result.body_insns:
            entry.body_insns = result.body_insns
        entry.stubbed = entry.stubbed or result.stubbed
        entry.delegation_ratio = max(entry.delegation_ratio, result.delegation_ratio)
        entry.covered |= result.reach.covered_vas
        entry.insn_counts.append(result.reach.insns)
        entry.stub_applied += result.reach.stub_applied
        if result.outcome is Outcome.AGREE:
            entry.agrees += 1
        if result.outcome in VERDICT_OUTCOMES:
            entry.verdicts += 1

    def add_skip(self, skipped: SkippedFunction) -> None:
        self.outcomes[Outcome.SKIPPED_UNSUPPORTED] = (
            self.outcomes.get(Outcome.SKIPPED_UNSUPPORTED, 0) + 1
        )
        self.skip_reasons[skipped.reason] = self.skip_reasons.get(skipped.reason, 0) + 1

    @property
    def total(self) -> int:
        return sum(self.outcomes.values())

    @property
    def cases_run(self) -> int:
        """Rows that represent an actual execution, i.e. everything but the skips."""
        return self.total - self.outcomes.get(Outcome.SKIPPED_UNSUPPORTED, 0)

    @property
    def verdicts(self) -> int:
        """Cases where a verdict was possible: AGREE plus the two divergent outcomes."""
        return sum(self.outcomes.get(outcome, 0) for outcome in VERDICT_OUTCOMES)

    @property
    def verdict_functions(self) -> list[FunctionReach]:
        """Reachability records for functions that reached at least one verdict.

        Functions with no verdict are excluded deliberately: a reachability claim about a
        function whose every case faulted would be describing faults, not coverage.
        """
        return [entry for entry in self.reach.values() if entry.verdicts]

    @property
    def near_vacuous_functions(self) -> list[FunctionReach]:
        return [entry for entry in self.verdict_functions if entry.near_vacuous]

    @property
    def near_vacuous_agrees(self) -> int:
        """AGREEs that rest on a function barely exercised. Reported, not subtracted.

        They are not wrong, they are weak. Silently dropping them would be as misleading
        as silently counting them as full evidence.
        """
        return sum(entry.agrees for entry in self.near_vacuous_functions)

    @property
    def stubbed_verdicts(self) -> int:
        """Verdicts whose function had its callees stubbed, so they cover the caller only."""
        return sum(entry.verdicts for entry in self.verdict_functions if entry.stubbed)


class ResultWriter:
    """Append-as-you-go CSV writer with a tally alongside.

    `provenance` is a REQUIRED positional argument, not a keyword with a default. A
    default would let a new call site produce a provenance-free CSV silently, which is
    the exact failure this class was changed to prevent.
    """

    def __init__(
        self,
        path: Path,
        provenance: Provenance,
        *,
        flush_every: int = 1,
        subject_kind: str = "lifted",
        sampling: int | None = None,
    ) -> None:
        self.path = Path(path)
        self.provenance = provenance
        self.subject_kind = subject_kind
        self.sampling = sampling
        self.tally = Tally()
        self._flush_every = max(1, flush_every)
        self._since_flush = 0
        self._stream: TextIO | None = None
        self._writer: Any | None = None

    def __enter__(self) -> ResultWriter:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._stream = self.path.open("w", encoding="utf-8", newline="")
        self._writer = csv.writer(self._stream, lineterminator="\n")
        self._writer.writerow(CSV_HEADER)
        self._stream.flush()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        if self._stream is not None:
            self._stream.flush()
            self._stream.close()
            self._stream = None

    def write_case(self, result: CaseResult) -> None:
        self._row(
            (
                result.seed,
                result.index,
                f"{result.va:#010x}",
                result.size,
                str(result.outcome),
                result.oracle_fault or "",
                result.subject_fault or "",
                result.kinds,
                1 if result.stubbed else 0,
                result.reach.insns,
                result.body_insns,
                result.reach.covered,
                "" if result.subject_seh_ebp is None else f"{result.subject_seh_ebp:#010x}",
                1 if result.seh_ebp_drifted else 0,
                result.diagnosis,
            ),
            fault_diagnostic=result.fault_diagnostic,
            passthrough_applied=result.reach.passthrough_applied,
            flags_cols=(
                (
                    f"{result.flags_compared_mask:#x}",
                    f"{result.flags_disagree_mask:#x}",
                    f"{result.flags_unmodeled_mask:#x}",
                )
                if result.flags_observed
                else ("", "", "")
            ),
        )
        self.tally.add_case(result)

    def write_skip(self, skipped: SkippedFunction, seed: int) -> None:
        self._row(
            (
                seed,
                "",
                f"{skipped.va:#010x}",
                skipped.size,
                str(Outcome.SKIPPED_UNSUPPORTED),
                "",
                "",
                "",
                0,
                "",
                "",
                "",
                "",
                0,
                skipped.reason,
            )
        )
        self.tally.add_skip(skipped)

    def _row(
        self,
        values: tuple[object, ...],
        *,
        passthrough_applied: int = 0,
        flags_cols: tuple[str, str, str] = ("", "", ""),
        fault_diagnostic: str = "",
    ) -> None:
        if self._writer is None or self._stream is None:
            raise RuntimeError("ResultWriter used outside its context manager")
        # Prepended here, in the one place every row passes through, so no future row
        # type can be added without it.
        self._writer.writerow(
            (self.provenance.gen_dir, self.provenance.tree_sha)
            + tuple(values)
            + (
                self.subject_kind,
                self.provenance.repl_sha,
                "" if self.sampling is None else self.sampling,
                passthrough_applied,
            )
            + flags_cols
            + (fault_diagnostic,)
        )
        self._since_flush += 1
        if self._since_flush >= self._flush_every:
            self._stream.flush()
            self._since_flush = 0


def render_summary(
    tally: Tally,
    *,
    provenance: Provenance,
    seed: int,
    csv_path: Path,
    functions_selected: int,
    restarts: int = 0,
    timeouts: int = 0,
    deaths: int = 0,
    flags_compared: bool = False,
    adversarial_inputs: bool = False,
    elapsed_seconds: float | None = None,
    max_divergences_shown: int = 10,
    subject_note: str | None = None,
    cases_per_function: int | None = None,
    live_call_closure: bool = False,
    static_jump_tables: bool = False,
) -> str:
    """The stdout report. States what was measured and refuses to extrapolate past it.

    `provenance` is keyword-REQUIRED and printed above the counts rather than below the
    caveats, because the counts are what gets quoted and the provenance is what makes
    them mean anything. `subject_note` carries the reason a run against a non-shipped
    tree was permitted, so a deliberate control says so in its own output.
    """
    lines: list[str] = ["", "=" * 72, "DIFFERENTIAL HARNESS RESULTS", "=" * 72]
    lines.append("SUBJECT PROVENANCE  (what these numbers measured)")
    lines.append(f"  lifted tree   {provenance.gen_dir}")
    lines.append(f"  tree_sha      {provenance.tree_sha}")
    if subject_note:
        lines.append(f"  NOTE          {subject_note}")
    lines.append("")
    lines.append(f"seed            {seed}  (every case is reproducible from seed+case_index)")
    if cases_per_function is not None:
        # The SAMPLING DEPTH is part of what a baseline means. A run at 6 cases per function found
        # 19 DISAGREE over 4 functions where one at 32 found 92 over 3, and the two were nearly
        # compared as if they were the same measurement. Counts at different depths are not
        # comparable, so the depth is printed beside them.
        lines.append(
            f"cases/function  {cases_per_function}  "
            "(counts are NOT comparable across different depths)"
        )
    lines.append(f"results CSV     {csv_path}")
    if elapsed_seconds is not None:
        lines.append(f"wall clock      {elapsed_seconds:,.1f} s")
    lines.append("")
    lines.append(f"functions selected for execution : {functions_selected:,}")
    lines.append(f"functions actually executed      : {len(tally.functions_executed):,}")
    lines.append(f"cases run                        : {tally.cases_run:,}")
    lines.append("")

    for outcome in Outcome:
        count = tally.outcomes.get(outcome, 0)
        lines.append(f"  {str(outcome):<22}{count:>10,}")
    lines.append("")
    lines.append(
        f"  verdicts reached     {tally.verdicts:>10,}   (AGREE + DISAGREE + SUBJECT-FAULTED)"
    )
    divergent = tally.outcomes.get(Outcome.DISAGREE, 0) + tally.outcomes.get(
        Outcome.SUBJECT_FAULTED, 0
    )
    lines.append(
        f"  divergent            {divergent:>10,}   "
        f"over {len(tally.divergent_functions):,} function(s)"
    )

    lines.extend(_reachability_lines(tally))
    lines.extend(_eflags_lines(tally))
    lines.extend(_seh_ebp_lines(tally))

    if tally.skip_reasons:
        lines.append("")
        lines.append("skip and no-verdict reasons (nothing is dropped unreported)")
        for reason, count in sorted(tally.skip_reasons.items(), key=lambda kv: (-kv[1], kv[0])):
            lines.append(f"  {reason:<40}{count:>10,}")

    if restarts or timeouts or deaths:
        lines.append("")
        # Deaths and timeouts are reported apart because they are different failures:
        # pooling them let every crash hide inside the timeout figure.
        lines.append(
            f"subject restarts {restarts}, case timeouts {timeouts}, "
            f"subject deaths {deaths} (all counted as failures)"
        )

    if tally.divergent_functions:
        lines.append("")
        lines.append("DIVERGENCES")
        shown = list(tally.divergent_functions.items())[:max_divergences_shown]
        for va, messages in shown:
            lines.append(f"  {va:#010x}  ({len(messages)} case(s))")
            for message in messages[:2]:
                lines.append(f"      {message}")
        remaining = len(tally.divergent_functions) - len(shown)
        if remaining > 0:
            lines.append(f"  ... and {remaining:,} more function(s); see the CSV")

    lines.append("")
    lines.append("WHAT THIS RUN DOES NOT ESTABLISH")
    for caveat in _caveats(
        flags_compared,
        stubbed_verdicts=tally.stubbed_verdicts,
        adversarial_inputs=adversarial_inputs,
        live_call_closure=live_call_closure,
        static_jump_tables=static_jump_tables,
    ):
        lines.append(f"  - {caveat}")
    lines.append("=" * 72)
    return "\n".join(lines)


def _reachability_lines(tally: Tally) -> list[str]:
    """How much of each function actually ran, and how many AGREEs that undermines.

    Printed next to the outcome counts rather than in a separate report, because the
    AGREE total immediately above it is the number a reader will otherwise quote.
    """
    verdict_functions = tally.verdict_functions
    if not verdict_functions:
        return []

    shallow = [entry for entry in verdict_functions if entry.shallow]
    low = [entry for entry in verdict_functions if entry.low_coverage]
    delegating = [entry for entry in verdict_functions if entry.delegating]
    vacuous = tally.near_vacuous_functions
    covered = [entry.coverage for entry in verdict_functions if entry.body_insns]
    agrees = tally.outcomes.get(Outcome.AGREE, 0)

    lines = ["", "REACHABILITY (an AGREE on a body that never ran is weak evidence)"]
    lines.append(f"  functions reaching a verdict        : {len(verdict_functions):,}")
    if covered:
        median = sorted(covered)[len(covered) // 2]
        full = sum(1 for ratio in covered if ratio >= 1.0)
        lines.append(f"  median body coverage (union/function): {median:.0%}")
        lines.append(f"  functions fully covered             : {full:,}")
    lines.append(
        f"  every case under {SHALLOW_INSNS} instructions   : {len(shallow):,}"
        "   (all cases returned early)"
    )
    lines.append(f"  union coverage under {LOW_COVERAGE:.0%}            : {len(low):,}")
    if delegating:
        lines.append(
            f"  stubbed and delegating >= {HIGH_DELEGATION:.0%}        : {len(delegating):,}"
            "   (body is mostly just the call)"
        )
    lines.append(
        f"  NEAR-VACUOUS functions              : {len(vacuous):,} of {len(verdict_functions):,}"
    )
    lines.append(
        f"  AGREEs resting on them              : {tally.near_vacuous_agrees:,}"
        f" of {agrees:,}"
        "   (counted above, but weak)"
    )
    if tally.stubbed_verdicts:
        lines.append(
            f"  verdicts with callees stubbed       : {tally.stubbed_verdicts:,}"
            "   (about the caller only)"
        )
    return lines


def _eflags_lines(tally: Tally) -> list[str]:
    """Per-flag agreement for the opt-in EFLAGS channel, unmodelled bits included.

    Empty on a run without the channel. The denominator is per FLAG: a flag is
    compared only on cases whose executed exit's publish mask claimed it, so CF's
    count and ZF's count legitimately differ. The unmodelled column is the honest
    remainder -- AF on every case (the lifter does not model AF anywhere), plus
    each flag on the cases whose exit could not answer it.
    """
    if not tally.flags_observed_cases:
        return []
    lines = ["", "EFLAGS (opt-in channel: subject's published flags vs the oracle's real ones)"]
    lines.append(f"  cases with both flag words          : {tally.flags_observed_cases:,}")
    lines.append(
        f"  exits that published nothing        : {tally.flags_unpublished_cases:,}"
        "   (mask 0: stubbed tail exit or no publish executed; not compared)"
    )
    lines.append(f"  {'flag':<6}{'compared':>10}{'agree':>10}{'DISAGREE':>10}{'unmodeled':>11}")
    for _bit, label in FLAG_BITS:
        compared = tally.flags_compared.get(label, 0)
        disagree = tally.flags_disagree.get(label, 0)
        unmodeled = tally.flags_unmodeled.get(label, 0)
        if not compared and not unmodeled:
            continue
        lines.append(
            f"  {label:<6}{compared:>10,}{compared - disagree:>10,}{disagree:>10,}{unmodeled:>11,}"
        )
    lines.append(
        "  AF is unmodelled BY the lifter everywhere, so its row is all unmodelled: "
        "that is a reported gap, not a pass."
    )
    return lines


def _seh_ebp_lines(tally: Tally) -> list[str]:
    """The second published frame pointer, reported rather than compared.

    Says the denominator out loud. Zero drifts out of zero observations is a subject that
    cannot report the value, not a lifter that never leaves it stale, and the two must not
    print the same way.
    """
    if not tally.seh_ebp_observed:
        return [
            "",
            "g_seh_ebp: NOT OBSERVED on any case. This subject does not report it, so this "
            "run says nothing about whether it is ever left stale.",
        ]
    lines = ["", "g_seh_ebp (the lifter's second frame pointer; OBSERVED, never compared)"]
    lines.append(f"  cases where it was reported         : {tally.seh_ebp_observed:,}")
    lines.append(
        f"  left disagreeing with the final ebp : {tally.seh_ebp_drifts:,}"
        f" of {tally.seh_ebp_observed:,}"
    )
    lines.append(
        "  It has no hardware counterpart, so a difference here is NOT a divergence. The "
        "driver seeds it equal to ebp at entry, so this measures only whether the function "
        "under test changed one and not the other."
    )
    return lines


def _caveats(
    flags_compared: bool,
    *,
    stubbed_verdicts: int = 0,
    adversarial_inputs: bool = False,
    live_call_closure: bool = False,
    static_jump_tables: bool = False,
) -> Iterable[str]:
    if live_call_closure:
        yield (
            "This run seeded and compared the x87 stack and control word; only TOP "
            "bits (0x3800) of the status word are modelled and compared. SSE and the "
            "other x87 status bits remain outside this proof."
        )
    else:
        yield (
            "x87 and SSE are excluded BY DESIGN, not untested by accident: the lifter "
            "computes x87 in C double where the hardware is 80-bit extended."
        )
    if not flags_compared:
        yield (
            "EFLAGS was not compared directly. The lifter stores no flags word, so flag "
            "errors are caught only through their effect on a register or a write."
        )
    else:
        yield (
            "EFLAGS was compared only on the bits each exit's publish mask claimed. "
            "AF is never modelled and never compared; the per-flag table above counts "
            "every unmodelled bit rather than scoring it."
        )
    if adversarial_inputs:
        yield (
            "Inputs include forced overflow (0x7FFFFFFF/0x80000000/0xFFFFFFFF) and "
            "shift-count (0/1/31/32/33) boundaries, but only at the sampled shares; "
            "they are directed noise, not an exhaustive sweep."
        )
    else:
        yield (
            "Inputs are random, not adversarial on overflow or shift-count boundaries, "
            "which is exactly where the known flag-model gaps live."
        )
    yield (
        "Only functions in the executed set are covered. These numbers say nothing "
        "about functions that were skipped or never selected."
    )
    yield (
        "An AGREE is per-case evidence, not per-function proof. See the reachability "
        "block for how many of them rest on a body that barely executed."
    )
    if stubbed_verdicts:
        yield (
            f"{stubbed_verdicts:,} verdict(s) had ordinary callees replaced by a stub "
            "returning a fixed value and touching no memory. They establish nothing "
            "about those callees, and they do not exercise any caller behaviour that "
            "depends on what a real callee returns or writes."
        )
        yield (
            "Indirect calls remain out of scope: the target VA is only known at run "
            "time, so the two sides could stub different callees."
        )
    elif live_call_closure:
        yield (
            "The explicitly fingerprinted direct-call closure ran live on both sides "
            "without synthetic callees. Any indirect-call body is an explicit opaque "
            "boundary in the closure document; descendants beyond it are unproved."
        )
    elif static_jump_tables:
        yield (
            "Only original-validated bounded intraprocedural jump tables were executed; "
            "modified body/table identity or writes produce no verdict. Calls remain excluded."
        )
    else:
        yield "Calls, indirect jumps and privileged instructions were never executed."
    if static_jump_tables:
        yield "Other indirect control flow, privileged instructions, x87 and SSE remain excluded."
    elif not live_call_closure:
        yield ("Indirect jumps, privileged instructions, x87 and SSE were never executed at all.")
    else:
        yield "Indirect jumps and privileged instructions were never executed."
    yield (
        "The g_seh_ebp figures above cannot answer the question that matters for it. It is "
        "a handoff channel between a caller and its callee, and this harness seeds it "
        "fresh and runs ONE function per case, so a value left stale for the NEXT function "
        "is unreachable here by construction."
    )
