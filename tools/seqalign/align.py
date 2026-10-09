# SPDX-License-Identifier: GPL-3.0-or-later
"""Global sequence alignment with affine gaps (Gotoh), and the scoring model for it.

WHY GLOBAL AND NOT GREEDY. A greedy nearest-match walk over two ordered lists commits
to its first plausible pair and can never undo it, so a single insertion early in the
sequence corrupts everything after it. Both sides here are *complete* lists -- the
donor's whole suffix group, the dispatcher's whole call list -- so the right shape is a
global alignment (Needleman-Wunsch) rather than a local one (Smith-Waterman), which
would be free to discard the sequence ends and would therefore quietly stop testing
the hypothesis that the ends correspond at all.

WHY AFFINE GAPS AND NOT LINEAR. The expected differences between TS2 and TSFP are
*whole subsystems* added or removed, and because both sides are in link order a whole
subsystem's absence is a contiguous block of gaps, not scattered single gaps. A linear
gap penalty charges the same for one gap of length 8 as for 8 gaps of length 1, which
is backwards: the first is one plausible edit, the second is eight independent
coincidences. Affine gaps (`gap_open` once, `gap_extend` per element) encode that, and
it is the difference between an alignment that explains a missing module and one that
shreds the sequence to chase matches.

THE SUBSTITUTION SCORE IS THE HONEST WEAK POINT. The donor side is a *name* and the
target side is an *address*; they share no content, because the donor is MIPS and the
target x86. Position is therefore almost the only signal, and an aligner whose
substitution score is a constant is degenerate -- every order-preserving alignment with
the same number of matches scores identically, so it produces an arbitrary pick among
exponentially many equal-scoring answers. `ScoringModel` adds the one weak numeric
signal that does cross an ISA boundary: function *size*, compared as a log ratio
against the median ratio of the pairing under consideration, which cancels the
platform's constant factor. `uniform_substitution` exists so a caller can measure
exactly how much that signal is worth by turning it off.

DETERMINISM IS A REQUIREMENT, NOT A DETAIL. Ties are broken in a fixed priority
(diagonal, then gap-in-right, then gap-in-left) and the fill order is fixed, so the
same inputs always produce byte-identical output. A naming pipeline whose answer moves
between runs cannot be reviewed.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from enum import Enum
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence

#: Stand-in for negative infinity in the dynamic-programming tables. A real `-inf`
#: works but propagates `nan` through `-inf + -inf`, which is reachable in the
#: first row/column, so a large finite sentinel is used instead.
NEG_INF = -1e18


class Step(Enum):
    """Which matrix a traceback step came from."""

    DIAGONAL = "diagonal"
    GAP_IN_RIGHT = "gap_in_right"
    """Left element consumed, right side gapped."""

    GAP_IN_LEFT = "gap_in_left"
    """Right element consumed, left side gapped."""


@dataclass(frozen=True)
class AlignedPair:
    """One column of an alignment. Exactly one index is None in a gap column."""

    left_index: int | None
    right_index: int | None

    @property
    def is_match(self) -> bool:
        return self.left_index is not None and self.right_index is not None


@dataclass(frozen=True)
class Alignment:
    """An alignment of two sequences, with the score it achieved."""

    pairs: tuple[AlignedPair, ...]
    score: float

    @property
    def matched_pairs(self) -> tuple[AlignedPair, ...]:
        return tuple(pair for pair in self.pairs if pair.is_match)

    @property
    def matches(self) -> int:
        return len(self.matched_pairs)

    @property
    def gaps(self) -> int:
        return len(self.pairs) - self.matches

    @property
    def gap_runs(self) -> int:
        """Maximal contiguous gap blocks, the quantity `gap_open` is charged per."""
        runs = 0
        in_gap = False
        for pair in self.pairs:
            if pair.is_match:
                in_gap = False
            else:
                if not in_gap:
                    runs += 1
                in_gap = True
        return runs

    def mapping(self) -> dict[int, int]:
        """`left_index -> right_index` for matched columns only."""
        return {
            pair.left_index: pair.right_index
            for pair in self.pairs
            if pair.left_index is not None and pair.right_index is not None
        }


@dataclass(frozen=True)
class ScoringModel:
    """Scores for a lifecycle-order alignment.

    `match` is the reward for pairing any donor subsystem with any target call, before
    the size term. It is positive and larger in magnitude than `gap_extend` so that
    pairing is preferred to gapping both sides, which is what makes the alignment
    find correspondences at all.

    `gap_open` and `gap_extend` are both negative and a gap of length L costs
    `gap_open + L * gap_extend`. The defaults make a single isolated gap (-2.2) cost
    noticeably more than one element of an existing gap (-0.2), which is the "a whole
    missing subsystem is one edit" bias described in the module docstring.

    `size_weight` scales the size term, which lies in [-1, 0]: 0 when the pair's size
    ratio equals the median ratio for the alignment, -1 once it is off by
    `size_tolerance` octaves or more. With `size_weight = 0` the model is uniform and
    the alignment becomes degenerate on purpose, for measuring the signal's worth.
    """

    match: float = 1.0
    gap_open: float = -2.0
    gap_extend: float = -0.2
    size_weight: float = 0.75
    size_tolerance: float = 2.0

    def __post_init__(self) -> None:
        if self.gap_open > 0 or self.gap_extend > 0:
            raise ValueError("gap_open and gap_extend must be <= 0")
        if self.match <= 0:
            raise ValueError("match must be > 0")
        if self.size_tolerance <= 0:
            raise ValueError("size_tolerance must be > 0")
        if self.size_weight < 0:
            raise ValueError("size_weight must be >= 0")

    def gap_cost(self, length: int) -> float:
        """Total penalty for one contiguous gap of `length` elements."""
        if length <= 0:
            return 0.0
        return self.gap_open + length * self.gap_extend

    def pair_score(self, left_size: int | None, right_size: int | None, scale: float) -> float:
        """Score for pairing two functions whose sizes are known, or `match` if not.

        `scale` is the expected `log2(right_size / left_size)` for a correct pair, i.e.
        the platform's constant factor. Absent either size the size term is dropped
        rather than guessed, so a missing size neither rewards nor punishes a pair.
        """
        if self.size_weight == 0 or not left_size or not right_size:
            return self.match
        deviation = abs(math.log2(right_size / left_size) - scale)
        normalised = min(1.0, deviation / self.size_tolerance)
        return self.match - self.size_weight * normalised


def median(values: Sequence[float]) -> float:
    """Median of `values`, 0.0 when empty.

    Written out rather than imported from `statistics` so the empty case is an explicit
    0.0 instead of an exception a caller has to guard at every site.
    """
    if not values:
        return 0.0
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2 == 1:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2.0


def size_scale(left_sizes: Sequence[int | None], right_sizes: Sequence[int | None]) -> float:
    """The median `log2` size ratio between two populations.

    Taken over the *populations* rather than over any proposed pairing, so the scale
    does not depend on the alignment it is used to compute -- which would be circular,
    letting a wrong alignment justify itself by redefining what a normal ratio is.
    """
    left = [math.log2(size) for size in left_sizes if size]
    right = [math.log2(size) for size in right_sizes if size]
    if not left or not right:
        return 0.0
    return median(right) - median(left)


def uniform_substitution(model: ScoringModel) -> Callable[[int, int], float]:
    """A substitution function that ignores its arguments. The degenerate control."""

    def score(left_index: int, right_index: int) -> float:  # noqa: ARG001
        return model.match

    return score


def size_substitution(
    model: ScoringModel,
    left_sizes: Sequence[int | None],
    right_sizes: Sequence[int | None],
) -> Callable[[int, int], float]:
    """A substitution function scoring pairs by size agreement."""
    scale = size_scale(left_sizes, right_sizes)

    def score(left_index: int, right_index: int) -> float:
        return model.pair_score(left_sizes[left_index], right_sizes[right_index], scale)

    return score


def align_sequences(
    left_length: int,
    right_length: int,
    substitution: Callable[[int, int], float],
    model: ScoringModel,
) -> Alignment:
    """Gotoh affine-gap global alignment of two sequences given only their lengths.

    The sequences themselves are never touched: `substitution(i, j)` is the sole source
    of content-dependent score. That keeps the aligner reusable and, more importantly,
    keeps it testable against hand-computed answers without constructing a donor.

    Three tables are filled -- `best_match`, `gap_in_right` (left element against a
    gap) and `gap_in_left` -- and a gap of length L is charged `gap_open + L *
    gap_extend` exactly once, which is what distinguishes this from a linear-penalty
    Needleman-Wunsch. Runtime and memory are O(left * right).
    """
    if left_length < 0 or right_length < 0:
        raise ValueError("sequence lengths must be non-negative")
    rows = left_length + 1
    columns = right_length + 1

    open_extend = model.gap_open + model.gap_extend

    best_match = [[NEG_INF] * columns for _ in range(rows)]
    gap_in_right = [[NEG_INF] * columns for _ in range(rows)]
    gap_in_left = [[NEG_INF] * columns for _ in range(rows)]
    from_match = [[Step.DIAGONAL] * columns for _ in range(rows)]
    from_right_gap = [[Step.DIAGONAL] * columns for _ in range(rows)]
    from_left_gap = [[Step.DIAGONAL] * columns for _ in range(rows)]

    best_match[0][0] = 0.0
    for row in range(1, rows):
        gap_in_right[row][0] = model.gap_cost(row)
        from_right_gap[row][0] = Step.DIAGONAL if row == 1 else Step.GAP_IN_RIGHT
    for column in range(1, columns):
        gap_in_left[0][column] = model.gap_cost(column)
        from_left_gap[0][column] = Step.DIAGONAL if column == 1 else Step.GAP_IN_LEFT

    for row in range(1, rows):
        for column in range(1, columns):
            diagonal_best, diagonal_from = _best(
                best_match[row - 1][column - 1],
                gap_in_right[row - 1][column - 1],
                gap_in_left[row - 1][column - 1],
            )
            best_match[row][column] = diagonal_best + substitution(row - 1, column - 1)
            from_match[row][column] = diagonal_from

            gap_in_right[row][column], from_right_gap[row][column] = _best(
                best_match[row - 1][column] + open_extend,
                gap_in_right[row - 1][column] + model.gap_extend,
                gap_in_left[row - 1][column] + open_extend,
            )
            gap_in_left[row][column], from_left_gap[row][column] = _best(
                best_match[row][column - 1] + open_extend,
                gap_in_right[row][column - 1] + open_extend,
                gap_in_left[row][column - 1] + model.gap_extend,
                prefer_left_gap=True,
            )

    score, state = _best(
        best_match[left_length][right_length],
        gap_in_right[left_length][right_length],
        gap_in_left[left_length][right_length],
    )

    pairs: list[AlignedPair] = []
    row, column = left_length, right_length
    while row > 0 or column > 0:
        if state is Step.DIAGONAL:
            if row == 0:
                state = Step.GAP_IN_LEFT
                continue
            if column == 0:
                state = Step.GAP_IN_RIGHT
                continue
            pairs.append(AlignedPair(left_index=row - 1, right_index=column - 1))
            state = from_match[row][column]
            row -= 1
            column -= 1
        elif state is Step.GAP_IN_RIGHT:
            pairs.append(AlignedPair(left_index=row - 1, right_index=None))
            state = from_right_gap[row][column]
            row -= 1
        else:
            pairs.append(AlignedPair(left_index=None, right_index=column - 1))
            state = from_left_gap[row][column]
            column -= 1

    pairs.reverse()
    return Alignment(pairs=tuple(pairs), score=score)


def _best(
    diagonal: float,
    right_gap: float,
    left_gap: float,
    *,
    prefer_left_gap: bool = False,
) -> tuple[float, Step]:
    """Max of three candidates with a fixed tie-break, so results are reproducible.

    Default priority is diagonal, then gap-in-right, then gap-in-left. The
    `gap_in_left` table flips the last two, because inside that table "continue the
    existing gap" is the `left_gap` argument and preferring it keeps a gap contiguous
    rather than splitting it into two charged runs of equal total score.
    """
    order = (
        ((diagonal, Step.DIAGONAL), (left_gap, Step.GAP_IN_LEFT), (right_gap, Step.GAP_IN_RIGHT))
        if prefer_left_gap
        else (
            (diagonal, Step.DIAGONAL),
            (right_gap, Step.GAP_IN_RIGHT),
            (left_gap, Step.GAP_IN_LEFT),
        )
    )
    best_value, best_step = order[0]
    for value, step in order[1:]:
        if value > best_value:
            best_value, best_step = value, step
    return best_value, best_step
