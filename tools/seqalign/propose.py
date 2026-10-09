# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn alignments into named proposals, with a confidence that can be low.

CONFIDENCE IS NOT A SCORE RESCALED. The alignment score is in arbitrary units and
cannot be converted into a probability, so nothing here tries. Confidence is built from
three things that are each independently checkable:

  1. RUN SUPPORT. The length of the contiguous run of matched columns the proposal sits
     in. A match flanked by matches on both sides is constrained by its neighbours; an
     isolated match between two gaps is the aligner picking something because it had to.
     This is the dominant term because it is the only per-proposal evidence that exists.
  2. GROUP SUPPORT. How many independent suffix groups proposed a name for this
     subsystem at all. One group is a hypothesis; two agreeing groups is the invariant
     doing its job.
  3. THE CROSS-CHECK GATE. If `CrossCheck.verdict()` is False, every confidence is
     capped at `UNVERIFIED_CEILING`. Not zeroed -- the proposals are still worth looking
     at by hand -- but hard-capped below any threshold anyone should apply names at, so
     a failed run cannot produce a confident-looking CSV.

THE FREE ACCURACY TEST, and what it actually tests on this target. 294 `.XTLID` names
are exact and 744 functions in the retail table already carry a name, so a proposal
landing on one of those is checkable for nothing. `check_known_names` does it. But read
`KnownNameCheck.note`: on retail TSFP every one of those known names is XDK library
code (D3D, DirectSound, winsock, `lstr*`), and the donor's names are all Free Radical
*game* subsystems. The two sets cannot agree by construction, so this is a ONE-SIDED
test -- it can only ever refute. A proposal of `soundMake` for a function already known
to be `D3DDevice_SetTexture` is a definite false positive; a proposal for an unnamed
function is untestable. Reported as a false-positive rate, never as an accuracy.
"""

from __future__ import annotations

import csv
from dataclasses import dataclass
from typing import TYPE_CHECKING

from tools.seqalign.crosscheck import CrossCheck

if TYPE_CHECKING:
    from collections.abc import Iterable, Mapping, Sequence
    from pathlib import Path

    from tools.codediff.boundaries import Function
    from tools.seqalign.align import Alignment
    from tools.seqalign.crosscheck import GroupAssignment

#: Header of the proposals CSV, in order.
PROPOSAL_CSV_COLUMNS = (
    "target_va",
    "proposed_name",
    "confidence",
    "supporting_groups",
    "evidence",
)

#: No proposal from a run whose cross-checks failed may exceed this, whatever its run
#: and group support. Chosen strictly below 0.20, the lowest band anyone would report,
#: so a failed run's proposals are visibly unusable rather than merely low.
UNVERIFIED_CEILING = 0.15

#: Matched-run length at which run support saturates. Six consecutive matched columns is
#: already a 6-element ordered correspondence, which is not something a gap-penalised
#: aligner stumbles into between unrelated sequences.
RUN_SATURATION = 6

#: Name prefixes Ghidra generates for an unnamed function. Anything else is a real name
#: and makes the function part of the known-name control.
GENERATED_PREFIXES = ("FUN_", "SUB_", "thunk_FUN_")


@dataclass(frozen=True)
class Proposal:
    """One proposed name for one target function."""

    target_va: int
    proposed_name: str
    confidence: float
    supporting_groups: tuple[str, ...]
    evidence: str

    def as_row(self) -> dict[str, str]:
        return {
            "target_va": f"0x{self.target_va:08x}",
            "proposed_name": self.proposed_name,
            "confidence": f"{self.confidence:.3f}",
            "supporting_groups": " ".join(self.supporting_groups),
            "evidence": self.evidence,
        }


def is_generated_name(name: str) -> bool:
    """Whether `name` is a Ghidra placeholder rather than a real symbol."""
    return name.startswith(GENERATED_PREFIXES)


def matched_run_lengths(alignment: Alignment) -> dict[int, int]:
    """`left_index -> length of the contiguous matched run it belongs to`.

    Contiguous in *alignment columns*, so one gap column breaks a run. That is the
    strict reading and the right one: a gap means the two sides disagree about what is
    there, and a match on the far side of a disagreement is not supported by the match
    on this side.
    """
    lengths: dict[int, int] = {}
    run: list[int] = []
    for pair in alignment.pairs:
        if pair.is_match and pair.left_index is not None:
            run.append(pair.left_index)
            continue
        for index in run:
            lengths[index] = len(run)
        run = []
    for index in run:
        lengths[index] = len(run)
    return lengths


def run_support(run_length: int, *, saturation: int = RUN_SATURATION) -> float:
    """Confidence contribution from matched-run length, in (0, 1].

    A run of 1 scores 1/saturation rather than 0, because an isolated match is weak
    evidence and not *no* evidence. Linear up to saturation then flat, because beyond a
    handful of consecutive matches the marginal information is small and a longer run
    should not be able to compensate for a failed cross-check.
    """
    if run_length <= 0:
        return 0.0
    return min(1.0, run_length / saturation)


def group_support(count: int) -> float:
    """Confidence contribution from how many suffix groups proposed a subsystem.

    0.55 for one group, 0.85 for two, 1.0 for three or more. A single group deliberately
    cannot reach high confidence however good its alignment looks, because a single
    group's alignment is exactly the thing that has no independent check.
    """
    if count <= 0:
        return 0.0
    if count == 1:
        return 0.55
    if count == 2:
        return 0.85
    return 1.0


def propose_names(
    groups: Mapping[str, GroupAssignment],
    alignments: Mapping[str, Alignment],
    checks: CrossCheck,
    *,
    unverified_ceiling: float = UNVERIFIED_CEILING,
) -> list[Proposal]:
    """Build the proposal list from per-group assignments and their alignments.

    One proposal per (target VA, suffix): `sysMake` and `sysEnd` are different functions
    and get separate rows. `supporting_groups` counts the groups that named the
    *subsystem*, which is what the invariant corroborates -- the groups cannot
    corroborate each other about an individual address, since they are looking at
    different addresses by construction.

    Sorted by descending confidence then ascending target VA, so the most defensible
    proposals are at the top of the CSV and the order is deterministic.
    """
    trustworthy, reason = checks.verdict()
    subsystem_groups: dict[str, list[str]] = {}
    for suffix in sorted(groups):
        for prefix in groups[suffix].assignment:
            subsystem_groups.setdefault(prefix, []).append(suffix)

    proposals: list[Proposal] = []
    for suffix in sorted(groups):
        group = groups[suffix]
        alignment = alignments.get(suffix)
        runs = matched_run_lengths(alignment) if alignment is not None else {}
        donor_index = {prefix: index for index, prefix in enumerate(group.donor_order)}
        for prefix, target_va in sorted(group.assignment.items(), key=lambda item: item[1]):
            run_length = runs.get(donor_index.get(prefix, -1), 1)
            supporters = tuple(subsystem_groups.get(prefix, (suffix,)))
            confidence = run_support(run_length) * group_support(len(supporters))
            if not trustworthy:
                confidence = min(confidence, unverified_ceiling)
            proposals.append(
                Proposal(
                    target_va=target_va,
                    proposed_name=prefix + suffix,
                    confidence=confidence,
                    supporting_groups=supporters,
                    evidence=(
                        f"group={suffix} dispatcher=0x{group.dispatcher_va:08x} "
                        f"call_index={group.call_position.get(prefix, -1)} "
                        f"matched_run={run_length} "
                        f"crosscheck={'pass' if trustworthy else 'FAIL'}: {reason}"
                    ),
                )
            )
    proposals.sort(key=lambda proposal: (-proposal.confidence, proposal.target_va))
    return proposals


@dataclass(frozen=True)
class KnownNameCheck:
    """The ground-truth control: proposals landing on already-named target functions."""

    proposals: int
    tested: int
    """Proposals whose target already has a real (non-generated) name."""

    agreed: int
    """Of those, where the proposed name equals the known name."""

    disagreed: int
    examples: tuple[tuple[int, str, str], ...]
    """`(target_va, proposed_name, known_name)` for up to `EXAMPLE_LIMIT` disagreements."""

    known_names_available: int
    """Real names in the target function table, i.e. the size of the control pool."""

    @property
    def false_positive_rate(self) -> float | None:
        """`disagreed / tested`, or None when no proposal landed on a named function."""
        if self.tested == 0:
            return None
        return self.disagreed / self.tested

    @property
    def note(self) -> str:
        if self.tested == 0:
            return (
                "no proposal landed on an already-named function, so the alignment is "
                "untested against ground truth"
            )
        return (
            f"{self.disagreed}/{self.tested} proposals contradict a name already known "
            "from .XTLID or prior analysis"
        )


#: How many contradictions to carry in `KnownNameCheck.examples`. Enough to inspect by
#: hand, not enough to bury the summary.
EXAMPLE_LIMIT = 20


def check_known_names(
    proposals: Sequence[Proposal], functions: Iterable[Function]
) -> KnownNameCheck:
    """Compare proposals against names the target already has.

    This is the most important measurement the package makes, because it is the only one
    with an external answer key. It is run over *all* proposals regardless of
    confidence: filtering to the confident ones first would hide exactly the failures
    worth knowing about.
    """
    known = {
        function.entry_va: function.name
        for function in functions
        if not is_generated_name(function.name)
    }
    tested = agreed = disagreed = 0
    examples: list[tuple[int, str, str]] = []
    for proposal in proposals:
        existing = known.get(proposal.target_va)
        if existing is None:
            continue
        tested += 1
        if existing == proposal.proposed_name:
            agreed += 1
            continue
        disagreed += 1
        if len(examples) < EXAMPLE_LIMIT:
            examples.append((proposal.target_va, proposal.proposed_name, existing))
    return KnownNameCheck(
        proposals=len(proposals),
        tested=tested,
        agreed=agreed,
        disagreed=disagreed,
        examples=tuple(examples),
        known_names_available=len(known),
    )


def write_proposals(path: Path, proposals: Iterable[Proposal]) -> int:
    """Write the proposals CSV and return the row count.

    Columns are `PROPOSAL_CSV_COLUMNS`. The `evidence` column carries each row's own
    provenance -- which group, which dispatcher, which call index, which matched-run
    length, and whether the cross-check passed -- so no row has to be taken on trust.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    rows = list(proposals)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(PROPOSAL_CSV_COLUMNS))
        writer.writeheader()
        for proposal in rows:
            writer.writerow(proposal.as_row())
    return len(rows)
