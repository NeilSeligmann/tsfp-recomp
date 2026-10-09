# SPDX-License-Identifier: GPL-3.0-or-later
"""The self-checks. Without these the alignment is unfalsifiable and therefore useless.

An alignment of an ordered name list against an ordered address list always succeeds:
there is no such thing as "no alignment", only a lower score, and a score has no
absolute meaning because the sequences share no content. So the output of `align` is
not evidence of anything on its own. These three checks are, because each one can fail
on data where the technique is wrong, and each is computed from something the alignment
did **not** optimise for.

CHECK 1 -- INTERNAL ADDRESS ORDER (`address_order_concordance`). The alignment pairs
donor subsystems with target functions in *call-site order*. Nothing in the aligner
looks at the target functions' own addresses. But the donor order is a *link* order, so
if the correspondence is real the paired target functions should also be in ascending
address order, because MSVC also emits a translation unit's code contiguously. This is
the cheapest check and it needs only one suffix group. It fails whenever the dispatcher
calls its subsystems in an order unrelated to link order, which is a perfectly possible
state of the world.

CHECK 2 -- CROSS-GROUP ORDER (`cross_group_concordance`). This is the one the invariant
was for. Align `Make` against one dispatcher, `End` against a different one. For the
subsystems in both donor groups that yields two independent assignments:
`sysMake -> va_m` and `sysEnd -> va_e`, pointing at *different* target functions found
from *different* sequences. Because the donor's `Make` and `End` orders are themselves
100% concordant, the two target address orders must be concordant too. They are
computed from disjoint evidence, so agreement is not arithmetically forced, and
disagreement is a hard refutation rather than a bad score.

CHECK 3 -- CO-LOCALITY (`colocality`). `sysMake` and `sysEnd` come from the same C file,
so in the target they should be *near each other* in `.text`. The check compares the
median gap between paired proposals against the median gap under a shifted pairing of
the same two assignments, which is the correct null: it holds both address populations
fixed and destroys only the pairing, so a low median gap cannot be earned by the
functions merely being clustered in the image.

ALL THREE REPORT THEIR DENOMINATOR. `order_concordance` can return `(0, 0)`, and 0/0
reported as a percentage is how a check that compared nothing comes to look like a
check that passed. Every rate here is `None` when its denominator is zero, and
`CrossCheck.verdict` refuses to pass on an empty comparison.
"""

from __future__ import annotations

import itertools
from dataclasses import dataclass
from typing import TYPE_CHECKING

from tools.gen_donor_symbols import order_concordance

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

#: Concordance below this is treated as refutation rather than noise. The donor's own
#: figure is 1.00 across all 15 pairs; chance alone gives 0.50 on random orders, so 0.90
#: sits far from both and does not need tuning to separate them.
CONCORDANCE_PASS = 0.90

#: Fewer shared ordered pairs than this and the rate is not interpretable: four shared
#: subsystems give 6 pairs, and 6 coin flips land on 6 heads 1.6% of the time.
MIN_CONCORDANCE_PAIRS = 6


@dataclass(frozen=True)
class GroupAssignment:
    """One suffix group's alignment result, as donor subsystem -> target function.

    `call_position` is the index of the target within the dispatcher's call sequence,
    retained because check 1 is precisely a comparison of `call_position` order against
    `target_va` order and conflating the two would make it vacuous.
    """

    suffix: str
    dispatcher_va: int
    assignment: dict[str, int]
    """`subsystem prefix -> proposed target entry VA`."""

    call_position: dict[str, int]
    """`subsystem prefix -> index in the dispatcher's call sequence`."""

    donor_order: tuple[str, ...]
    """The donor link order this group was aligned against, unmodified."""

    alignment_score: float
    gaps: int

    @property
    def matched(self) -> int:
        return len(self.assignment)


def _rate(agree: int, total: int) -> float | None:
    """`agree / total`, or None when nothing was compared."""
    if total <= 0:
        return None
    return agree / total


@dataclass(frozen=True)
class ConcordanceResult:
    """One concordance measurement, with the denominator always visible."""

    label: str
    agree: int
    total: int

    @property
    def rate(self) -> float | None:
        return _rate(self.agree, self.total)

    @property
    def interpretable(self) -> bool:
        return self.total >= MIN_CONCORDANCE_PAIRS

    @property
    def passed(self) -> bool:
        """True only when there was enough to compare *and* it agreed."""
        rate = self.rate
        return self.interpretable and rate is not None and rate >= CONCORDANCE_PASS


def address_order_concordance(group: GroupAssignment) -> ConcordanceResult:
    """Check 1: do call-site order and target address order agree?

    Built by sorting the assigned subsystems twice -- once by call position, once by
    target address -- and handing both orders to `order_concordance`, the same function
    that measured the donor invariant.
    """
    subsystems = sorted(group.assignment, key=lambda prefix: group.call_position[prefix])
    by_address = sorted(group.assignment, key=lambda prefix: group.assignment[prefix])
    agree, total = order_concordance(subsystems, by_address)
    return ConcordanceResult(
        label=f"{group.suffix}: call order vs address order", agree=agree, total=total
    )


def cross_group_concordance(left: GroupAssignment, right: GroupAssignment) -> ConcordanceResult:
    """Check 2: do two independently aligned groups agree on subsystem order?

    Only subsystems assigned in both groups participate. Each group contributes its own
    address order over those subsystems, and the two orders are compared. The donor's
    own orders for these two suffixes are 100% concordant, so under a correct
    correspondence the target's must be too.
    """
    shared = sorted(set(left.assignment) & set(right.assignment))
    left_order = sorted(shared, key=lambda prefix: left.assignment[prefix])
    right_order = sorted(shared, key=lambda prefix: right.assignment[prefix])
    agree, total = order_concordance(left_order, right_order)
    return ConcordanceResult(
        label=f"{left.suffix} vs {right.suffix}: target address order",
        agree=agree,
        total=total,
    )


@dataclass(frozen=True)
class Colocality:
    """Check 3: are a subsystem's lifecycle functions near each other in `.text`?"""

    label: str
    shared: int
    median_gap: float | None
    """Median |va difference| for subsystems assigned in both groups."""

    median_null_gap: float | None
    """The same statistic under a rotated pairing of the same two address sets."""

    @property
    def enrichment(self) -> float | None:
        """`median_null_gap / median_gap`. >1 means paired functions really are closer."""
        if not self.median_gap or self.median_null_gap is None:
            return None
        return self.median_null_gap / self.median_gap


def _median(values: Sequence[float]) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2 == 1:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2.0


def colocality(left: GroupAssignment, right: GroupAssignment) -> Colocality:
    """Compare paired lifecycle-function distance against a rotated-pairing null.

    The null rotates the right-hand assignment by one position over the shared
    subsystems. That keeps both address multisets exactly as they are and changes only
    which address is paired with which, so any difference is attributable to the pairing
    and not to how the addresses are distributed.
    """
    shared = sorted(set(left.assignment) & set(right.assignment))
    label = f"{left.suffix}/{right.suffix}: lifecycle co-locality"
    if len(shared) < 2:
        return Colocality(label=label, shared=len(shared), median_gap=None, median_null_gap=None)
    paired = [abs(left.assignment[prefix] - right.assignment[prefix]) for prefix in shared]
    rotated = [
        abs(left.assignment[prefix] - right.assignment[shared[(index + 1) % len(shared)]])
        for index, prefix in enumerate(shared)
    ]
    return Colocality(
        label=label,
        shared=len(shared),
        median_gap=_median(paired),
        median_null_gap=_median(rotated),
    )


def parallel_dispatchers(left_targets: Sequence[int], right_targets: Sequence[int]) -> Colocality:
    """Check 4, and it needs no donor: do two candidate dispatchers walk the same list?

    If `left` is the `Make` dispatcher and `right` the `End` dispatcher over the same
    subsystems in the same order, then their i-th targets come from the same translation
    unit and must be close together in `.text`. That prediction involves no donor, no
    alignment and no names -- it is purely a property of two call sequences -- so it
    tests the technique's premise from a second direction, and it can be run over every
    pair of candidates before any naming is attempted.

    The null again rotates the right-hand sequence by one, holding both address sets
    fixed and destroying only the pairing. Enrichment near 1.0 means the two sequences
    are not parallel, and therefore that no pair of dispatchers is walking one shared
    subsystem order.
    """
    length = min(len(left_targets), len(right_targets))
    label = "parallel dispatchers"
    if length < 2:
        return Colocality(label=label, shared=length, median_gap=None, median_null_gap=None)
    paired = [abs(left_targets[index] - right_targets[index]) for index in range(length)]
    rotated = [
        abs(left_targets[index] - right_targets[(index + 1) % length]) for index in range(length)
    ]
    return Colocality(
        label=label,
        shared=length,
        median_gap=_median(paired),
        median_null_gap=_median(rotated),
    )


@dataclass(frozen=True)
class CrossCheck:
    """Every check, over every group and group pair."""

    internal: tuple[ConcordanceResult, ...]
    cross_group: tuple[ConcordanceResult, ...]
    colocalities: tuple[Colocality, ...]

    def interpretable_cross_group(self) -> tuple[ConcordanceResult, ...]:
        return tuple(result for result in self.cross_group if result.interpretable)

    @property
    def cross_group_rate(self) -> float | None:
        """Pooled agreement over every interpretable cross-group comparison."""
        usable = self.interpretable_cross_group()
        return _rate(sum(r.agree for r in usable), sum(r.total for r in usable))

    @property
    def internal_rate(self) -> float | None:
        usable = [result for result in self.internal if result.interpretable]
        return _rate(sum(r.agree for r in usable), sum(r.total for r in usable))

    def verdict(self) -> tuple[bool, str]:
        """`(trustworthy, reason)`. False whenever a check failed or could not be run.

        Fails closed. "Not enough data to check" and "checked and disagreed" are both
        reasons not to emit names, and collapsing them into a pass is how an
        unverifiable guess gets committed and then trusted.
        """
        usable = self.interpretable_cross_group()
        if not usable:
            return False, "no cross-group comparison had enough shared subsystems to interpret"
        rate = self.cross_group_rate
        if rate is None or rate < CONCORDANCE_PASS:
            return False, f"cross-group order concordance {rate:.3f} < {CONCORDANCE_PASS}"
        internal = self.internal_rate
        if internal is None or internal < CONCORDANCE_PASS:
            shown = "none" if internal is None else f"{internal:.3f}"
            return False, f"internal call-order vs address-order concordance {shown}"
        return True, f"cross-group {rate:.3f}, internal {internal:.3f}, both >= {CONCORDANCE_PASS}"


def run_cross_checks(groups: Mapping[str, GroupAssignment]) -> CrossCheck:
    """Every check over `groups`, in a deterministic order.

    Group pairs are taken in sorted-suffix order so the report is stable run to run.
    """
    ordered = [groups[suffix] for suffix in sorted(groups)]
    internal = tuple(address_order_concordance(group) for group in ordered)
    cross = tuple(
        cross_group_concordance(left, right) for left, right in itertools.combinations(ordered, 2)
    )
    local = tuple(colocality(left, right) for left, right in itertools.combinations(ordered, 2))
    return CrossCheck(internal=internal, cross_group=cross, colocalities=local)
