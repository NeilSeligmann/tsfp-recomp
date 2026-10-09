# SPDX-License-Identifier: GPL-3.0-or-later
"""Find the x86 function that looks like a lifecycle dispatcher.

THIS IS THE STEP THAT CAN KILL THE WHOLE TECHNIQUE, so it comes with its own report
rather than quietly returning a best guess. The alignment needs an ordered target
sequence, and the only thing that supplies one is a function whose body is mostly a
run of `call rel32` to subsystem entry points. If retail TSFP has no such function --
because the dispatch is through a table of function pointers, or inlined, or split
across several functions -- then there is nothing to align and that is the answer.

WHAT A LIFECYCLE DISPATCHER SHOULD LOOK LIKE, stated before measuring so the criteria
are not fitted to whatever was found:

  1. MANY DIRECT CALLS. At least as many as the donor suffix group has members, minus
     room for subsystems TSFP dropped.
  2. MOSTLY DISTINCT TARGETS. A subsystem is initialised once. A function calling the
     same two helpers thirty times is a loop, not a dispatcher.
  3. LOW FAN-IN TARGETS. A subsystem's `Make` is called by its dispatcher and almost
     nothing else. A function whose targets are all called from forty places is calling
     utilities (`memset`, string helpers), not subsystem entry points.
  4. LITTLE ELSE IN THE BODY. `calls_per_byte` high means the body is nearly all calls.
     A 4,000-byte function with 20 calls is doing work between them and is more likely
     a level loader than a dispatcher.
  5. FEW INDIRECT CALLS. Recorded but *not* penalised, because a high indirect count is
     itself the interesting negative finding -- see the module docstring in
     `sequences.py`.

`dispatcher_score` combines 2, 3 and 4 multiplicatively over a length floor, so a
candidate has to satisfy all of them rather than buy its way in on one. The score is
for *ranking only*; it is not a probability and is not used as a confidence anywhere.
"""

from __future__ import annotations

import itertools
import random
from dataclasses import dataclass
from typing import TYPE_CHECKING

from tools.seqalign.sequences import first_occurrences

if TYPE_CHECKING:
    from collections.abc import Iterable, Mapping, Sequence

    from tools.seqalign.sequences import CallSequence

#: Targets called from more distinct functions than this are treated as shared
#: utilities rather than subsystem entry points. 3 allows a subsystem entry point to be
#: reached from its dispatcher plus a retry path or a debug menu, while excluding the
#: genuinely common helpers, which in retail `.text` have fan-in in the hundreds.
MAX_SUBSYSTEM_FAN_IN = 3

#: A candidate shorter than this cannot be distinguished from an ordinary function that
#: happens to make a few calls, and cannot support a meaningful alignment either.
MIN_DISPATCHER_CALLS = 8


@dataclass(frozen=True)
class DispatcherCandidate:
    """One target function scored as a possible lifecycle dispatcher."""

    entry_va: int
    name: str
    call_count: int
    distinct_targets: int
    low_fan_in_targets: int
    """Distinct targets whose global fan-in is <= `MAX_SUBSYSTEM_FAN_IN`."""

    indirect_calls: int
    size_bytes: int
    score: float

    @property
    def distinct_fraction(self) -> float:
        if self.call_count == 0:
            return 0.0
        return self.distinct_targets / self.call_count

    @property
    def low_fan_in_fraction(self) -> float:
        if self.distinct_targets == 0:
            return 0.0
        return self.low_fan_in_targets / self.distinct_targets

    @property
    def calls_per_byte(self) -> float:
        if self.size_bytes == 0:
            return 0.0
        return self.call_count / self.size_bytes


def dispatcher_score(
    call_count: int,
    distinct_targets: int,
    low_fan_in_targets: int,
    size_bytes: int,
    *,
    min_calls: int = MIN_DISPATCHER_CALLS,
) -> float:
    """Rank score for a dispatcher candidate. 0.0 for anything below `min_calls`.

    The product of the three fractions times the call count: length is what makes a
    candidate useful, the fractions are what make it credible, and multiplying means a
    candidate failing any one of them scores near zero however long it is.

    A `call rel32` is 5 bytes, so `calls_per_byte` cannot exceed 0.2; it is rescaled by
    5 so a body that is nothing but calls scores 1.0 on that factor and the three
    factors are commensurable.
    """
    if call_count < min_calls or distinct_targets == 0 or size_bytes <= 0:
        return 0.0
    distinct_fraction = distinct_targets / call_count
    low_fan_in_fraction = low_fan_in_targets / distinct_targets
    density = min(1.0, (call_count / size_bytes) * 5.0)
    return call_count * distinct_fraction * low_fan_in_fraction * density


def rank_dispatchers(
    sequences: Iterable[CallSequence],
    fan_in: Mapping[int, int],
    *,
    min_calls: int = MIN_DISPATCHER_CALLS,
    max_fan_in: int = MAX_SUBSYSTEM_FAN_IN,
) -> list[DispatcherCandidate]:
    """Score every sequence and return candidates best-first.

    Ties break on `entry_va` ascending, so the ranking is deterministic even when two
    functions are structurally identical. Candidates scoring 0.0 are omitted: they
    failed the length floor and carry no information worth ranking.
    """
    candidates: list[DispatcherCandidate] = []
    for sequence in sequences:
        if sequence.fragmented:
            # The decoded span covers bytes Ghidra attributes to another function, so
            # the sequence may contain that function's calls. Excluded rather than
            # scored, because a contaminated sequence is worse than a missing one.
            continue
        distinct = set(sequence.targets)
        low_fan_in = sum(1 for target in distinct if fan_in.get(target, 0) <= max_fan_in)
        score = dispatcher_score(
            len(sequence.targets),
            len(distinct),
            low_fan_in,
            sequence.size_bytes,
            min_calls=min_calls,
        )
        if score <= 0.0:
            continue
        candidates.append(
            DispatcherCandidate(
                entry_va=sequence.entry_va,
                name=sequence.name,
                call_count=len(sequence.targets),
                distinct_targets=len(distinct),
                low_fan_in_targets=low_fan_in,
                indirect_calls=sequence.indirect_calls,
                size_bytes=sequence.size_bytes,
                score=score,
            )
        )
    candidates.sort(key=lambda candidate: (-candidate.score, candidate.entry_va))
    return candidates


@dataclass(frozen=True)
class PremiseTest:
    """Does a candidate call its targets in *address* order? The technique's premise.

    THIS IS THE TEST THAT MATTERS MOST AND IT NEEDS NO DONOR AT ALL. The donor invariant
    is a statement about *link order*: the suffix groups are mutually order-concordant
    because the linker emitted each translation unit once, in one order. Turning that
    into a naming method requires one further step -- that a dispatcher *calls* its
    subsystems in that same order -- and that step is an assumption, not a measurement,
    because `.mdebug` stabs contain no call graph and so no TS2 dispatcher's call order
    was ever observed.

    On the target the assumption is directly testable: take a candidate's distinct call
    targets in call order, and ask how often an earlier call goes to a lower address. A
    dispatcher walking link order scores near 1.0. Random order scores 0.5.
    `shuffled_null` is that null computed from the candidate's *own* addresses, so the
    comparison is not against a theoretical 0.5 but against the same multiset of
    addresses in a random order -- which is the only honest null when the addresses are
    not uniformly distributed.
    """

    entry_va: int
    distinct_targets: int
    agree: int
    total: int
    """Ordered pairs compared. `n * (n - 1) / 2` for n distinct targets."""

    shuffled_null: float | None
    """Concordance of the same addresses in a seeded random order."""

    @property
    def concordance(self) -> float | None:
        if self.total <= 0:
            return None
        return self.agree / self.total

    @property
    def lift(self) -> float | None:
        """Observed concordance minus its own shuffled null. ~0.0 means no signal."""
        observed = self.concordance
        if observed is None or self.shuffled_null is None:
            return None
        return observed - self.shuffled_null


def _ascending_concordance(addresses: Sequence[int]) -> tuple[int, int]:
    """`(pairs in ascending order, pairs compared)` for a sequence of addresses."""
    agree = 0
    total = 0
    for first, second in itertools.combinations(range(len(addresses)), 2):
        total += 1
        if addresses[first] < addresses[second]:
            agree += 1
    return agree, total


def measure_premise(sequence: CallSequence, *, seed: int = 0) -> PremiseTest:
    """Measure whether one candidate calls its targets in ascending address order.

    `seed` makes the shuffled null reproducible. It is derived from the candidate's own
    entry VA as well, so two candidates do not share one permutation and the nulls are
    independent across a survey while still being deterministic run to run.
    """
    addresses = first_occurrences(sequence.targets)
    agree, total = _ascending_concordance(addresses)
    null: float | None = None
    if total > 0:
        shuffled = list(addresses)
        # A string seed is hashed deterministically by CPython (sha512), unlike
        # `hash()`, which is salted per process and would make the null irreproducible.
        random.Random(f"{seed}:{sequence.entry_va}").shuffle(shuffled)
        null_agree, null_total = _ascending_concordance(shuffled)
        null = null_agree / null_total
    return PremiseTest(
        entry_va=sequence.entry_va,
        distinct_targets=len(addresses),
        agree=agree,
        total=total,
        shuffled_null=null,
    )


def survey_premise(
    sequences: Iterable[CallSequence],
    *,
    min_distinct: int = 15,
    seed: int = 0,
) -> list[PremiseTest]:
    """Run `measure_premise` over every long-enough sequence, best concordance first.

    `min_distinct` defaults to 15 (105 ordered pairs) because below that a high
    concordance is reachable by chance: the shuffled null's own spread over short
    sequences is wide, and a survey that includes them reports a high maximum that means
    nothing. Ties break on `entry_va` so the ordering is deterministic.
    """
    results = [
        measure_premise(sequence, seed=seed)
        for sequence in sequences
        if len(set(sequence.targets)) >= min_distinct
    ]
    results.sort(key=lambda result: (-(result.concordance or 0.0), result.entry_va))
    return results
