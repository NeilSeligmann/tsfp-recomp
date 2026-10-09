# SPDX-License-Identifier: GPL-3.0-or-later
"""Wire donor order + target call sequence -> alignment -> assignment.

Separated from `cli.py` so the whole pipeline is testable without an XBE on disk. The
only thing `cli.py` adds is reading files and printing.

ONE DECISION WORTH STATING. Each suffix group is aligned against a *separately chosen*
dispatcher. It would be simpler to pick one dispatcher and align every group against it,
and it would also be wrong: `sysMake` and `sysEnd` are called from different places in
any plausible engine (startup and shutdown), so forcing them through one sequence would
make the groups non-independent and destroy the cross-check that is the entire point.
`select_dispatchers` therefore assigns distinct dispatchers to distinct groups.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

from tools.seqalign.align import Alignment, ScoringModel, align_sequences, size_substitution
from tools.seqalign.crosscheck import GroupAssignment

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

    from tools.seqalign.dispatcher import DispatcherCandidate
    from tools.seqalign.donor import DonorOrder
    from tools.seqalign.sequences import CallSequence


@dataclass(frozen=True)
class GroupInput:
    """What one suffix group needs to be aligned."""

    suffix: str
    donor_order: tuple[str, ...]
    donor_sizes: tuple[int | None, ...]
    dispatcher_va: int
    targets: tuple[int, ...]
    target_sizes: tuple[int | None, ...]


def build_group_input(
    suffix: str,
    donor: DonorOrder,
    sequence: CallSequence,
    target_sizes: Mapping[int, int],
) -> GroupInput:
    """Assemble a `GroupInput` from a donor suffix group and a target call sequence.

    Target sizes are looked up by entry VA. A target with no entry in the function table
    gets `None`, which `ScoringModel.pair_score` treats as "no size signal" rather than
    as size zero -- the difference matters, because size zero would make every such pair
    maximally implausible and systematically push the alignment away from calls into
    functions Ghidra never bounded.
    """
    order = donor.link_order[suffix]
    return GroupInput(
        suffix=suffix,
        donor_order=order,
        donor_sizes=tuple(donor.size_of(prefix, suffix) for prefix in order),
        dispatcher_va=sequence.entry_va,
        targets=sequence.targets,
        target_sizes=tuple(target_sizes.get(target) for target in sequence.targets),
    )


def align_group(
    group: GroupInput, model: ScoringModel | None = None
) -> tuple[GroupAssignment, Alignment]:
    """Align one group and return `(assignment, alignment)`.

    The alignment object is returned alongside because `propose.matched_run_lengths`
    needs the column structure, which an assignment dict has thrown away.
    """
    scoring = model if model is not None else ScoringModel()
    substitution = size_substitution(scoring, group.donor_sizes, group.target_sizes)
    alignment = align_sequences(len(group.donor_order), len(group.targets), substitution, scoring)

    assignment: dict[str, int] = {}
    call_position: dict[str, int] = {}
    for pair in alignment.pairs:
        if pair.left_index is None or pair.right_index is None:
            continue
        prefix = group.donor_order[pair.left_index]
        assignment[prefix] = group.targets[pair.right_index]
        call_position[prefix] = pair.right_index

    return (
        GroupAssignment(
            suffix=group.suffix,
            dispatcher_va=group.dispatcher_va,
            assignment=assignment,
            call_position=call_position,
            donor_order=group.donor_order,
            alignment_score=alignment.score,
            gaps=alignment.gaps,
        ),
        alignment,
    )


def select_dispatchers(
    candidates: Sequence[DispatcherCandidate],
    suffixes: Sequence[str],
) -> dict[str, DispatcherCandidate]:
    """Give each suffix its own dispatcher candidate, best candidate to largest group.

    Pairing is positional: `suffixes` arrives largest-group-first from
    `DonorOrder.groups`, `candidates` arrives best-score-first, and they are zipped. That
    is a *heuristic and nothing more* -- it assumes the subsystem list's length ranking
    matches the dispatcher candidates' score ranking, which there is no reason to
    believe. It exists so the pipeline can run end to end and be measured; any real use
    would pin each suffix to a dispatcher a human identified, which `cli.py` exposes via
    `--pin`.

    Suffixes beyond the supply of candidates are simply absent from the result, so a
    caller sees a short dict rather than groups silently sharing a dispatcher.
    """
    return {suffix: candidate for suffix, candidate in zip(suffixes, candidates, strict=False)}
