# SPDX-License-Identifier: GPL-3.0-or-later
"""Seed the function to subsystem table from classified functions (pure, deterministic).

Order of precedence per function: an anchor (`anchors.py`), then the region of
`tools.coverage` (library region -> `rules.classify_library`, game region ->
`rules.classify_game`), then `place_library_runs` for classification-row library functions
whose reason names no library (T1641 position rows, T1643 candidates), then, when a call graph
is supplied, `callgraph.propagate` over the remaining UNKNOWN game functions. Output rows are
sorted by address.
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Collection, Iterable, Mapping
from dataclasses import dataclass, field

from tools.coverage import EVIDENCE_OVERRIDE, REGION_GAME
from tools.subsystems.anchors import ANCHORS, Anchor
from tools.subsystems.callgraph import propagate
from tools.subsystems.rules import (
    INFERRED,
    MEASURED,
    Decision,
    classify_game,
    classify_library,
)
from tools.subsystems.table import Row
from tools.subsystems.tree import Tree


@dataclass(frozen=True)
class Facts:
    """Everything the rules may look at for one function."""

    va: int
    size: int
    name: str
    region: str
    evidence: str
    section: str = ""
    #: Reason text of the function_classification.csv row (library overrides).
    reason: str = ""
    #: Confidence and evidence text of the function_names.csv row, when the name has one.
    name_confidence: str = ""
    name_evidence: str = ""

    @property
    def is_game(self) -> bool:
        return self.region == REGION_GAME


@dataclass
class SeedResult:
    rows: list[Row]
    decisions: dict[int, Decision]
    #: Anchor addresses that are not functions of the table (reported, never kept).
    anchors_missing: list[int] = field(default_factory=list)
    callgraph_used: bool = False


def anchor_problems(
    facts: Iterable[Facts], tree: Tree, anchors: Mapping[int, Anchor] = ANCHORS
) -> tuple[list[int], list[str]]:
    """(anchor addresses missing from the table, errors that make seeding refuse)."""
    by_va = {fact.va: fact for fact in facts}
    missing: list[int] = []
    errors: list[str] = []
    for va, anchor in sorted(anchors.items()):
        if anchor.subsystem not in tree:
            errors.append(f"anchor 0x{va:08x}: subsystem {anchor.subsystem} is not in the tree")
        fact = by_va.get(va)
        if fact is None:
            missing.append(va)
        elif not fact.is_game:
            errors.append(f"anchor 0x{va:08x}: {fact.name} is library code, anchors are game only")
    return missing, errors


def decide(fact: Facts, anchors: Mapping[int, Anchor] = ANCHORS) -> Decision:
    anchor = anchors.get(fact.va)
    if anchor is not None and fact.is_game:
        return Decision(
            anchor.subsystem, anchor.confidence, f"anchor: {anchor.source}", "anchor", "anchor"
        )
    if not fact.is_game:
        library = classify_library(
            fact.name, evidence=fact.evidence, section=fact.section, reason=fact.reason
        )
        return Decision(
            library.subsystem,
            library.confidence,
            library.evidence,
            f"library:{fact.evidence}",
            "library",
        )
    return classify_game(
        fact.name,
        evidence_text=fact.name_evidence,
        name_measured=fact.name_confidence == MEASURED,
        va=fact.va,
    )


def place_library_runs(
    decisions: Mapping[int, Decision], facts: Iterable[Facts]
) -> dict[int, Decision]:
    """Place unplaced library classification rows by their library neighbours (INFERRED).

    A library function with a classification row whose reason names no library (a T1641
    position row, a T1643 candidate) takes the leaf of the nearest placed library function
    before and after it in the same section when both agree and no game function lies
    between. Neighbours are read from `decisions` as given, so the result does not depend on
    the order rows are visited. The input mapping is not modified.
    """
    ordered = sorted(facts, key=lambda f: f.va)
    result = dict(decisions)
    for index, fact in enumerate(ordered):
        if fact.is_game or fact.evidence != EVIDENCE_OVERRIDE or decisions[fact.va].classified:
            continue
        ends: list[tuple[str, int]] = []
        for step in (-1, 1):
            cursor = index + step
            while 0 <= cursor < len(ordered):
                other = ordered[cursor]
                if other.is_game or other.section != fact.section:
                    break
                if decisions[other.va].classified:
                    ends.append((decisions[other.va].subsystem, other.va))
                    break
                cursor += step
        if len(ends) != 2 or ends[0][0] != ends[1][0]:
            continue
        path = ends[0][0]
        if not path.startswith("library"):
            continue
        result[fact.va] = Decision(
            path,
            INFERRED,
            f"library: classification row between {path} functions 0x{ends[0][1]:08x} and "
            f"0x{ends[1][1]:08x}, no game function between",
            "library-neighbours",
            "library",
        )
    return result


def seed(
    facts: Iterable[Facts],
    tree: Tree,
    *,
    anchors: Mapping[int, Anchor] = ANCHORS,
    callers: Mapping[int, Collection[int]] | None = None,
    address_taken: Collection[int] = (),
) -> SeedResult:
    """Build the table. `callers` None disables call graph propagation."""
    ordered = sorted(facts, key=lambda f: f.va)
    missing, errors = anchor_problems(ordered, tree, anchors)
    if errors:
        raise ValueError("; ".join(errors))
    decisions = {fact.va: decide(fact, anchors) for fact in ordered}
    decisions = place_library_runs(decisions, ordered)
    if callers is not None:
        decisions = propagate(
            decisions,
            callers,
            {fact.va: fact.size for fact in ordered},
            [fact.va for fact in ordered if fact.is_game],
            address_taken,
        )
    rows = [
        Row(va, decision.subsystem, decision.confidence, decision.evidence)
        for va, decision in sorted(decisions.items())
    ]
    return SeedResult(rows, decisions, missing, callers is not None)


def rule_hits(decisions: Mapping[int, Decision]) -> Counter[tuple[str, str, str]]:
    """Rows decided by each (group, rule label, subsystem)."""
    hits: Counter[tuple[str, str, str]] = Counter()
    for decision in decisions.values():
        hits[(decision.group, decision.rule, decision.subsystem)] += 1
    return hits
