# SPDX-License-Identifier: GPL-3.0-or-later
"""Load the function facts the rules need, reusing the badges' loaders (`tools.coverage`).

`tools.coverage_by_subsystem.load_classified` returns the same classified and overlaid function
table the badges and the coverage-by-subsystem report use, so the denominator is shared. This
module adds what the rules want on top: the XBE section of each function, the classification
reason text and the function_names.csv confidence/evidence of each game name.
"""

from __future__ import annotations

import csv
import hashlib
from dataclasses import dataclass
from pathlib import Path

from tools.coverage import XBE_SEARCH, Classified, Inputs
from tools.coverage_by_subsystem import load_classified, proven_classes
from tools.subsystems.seed import Facts
from tools.xbe.parser import XbeSection, parse_xbe

DEFAULT_GENERATED = Path("generated/retail")
DEFAULT_PROOF_SNAPSHOT = Path("docs/data/replace-proof-snapshot.json")
CLASSIFICATION_CSV = Path("tools/data/function_classification.csv")


class MissingInputs(SystemExit):
    """The XBE or the generated function table is not available."""


@dataclass(frozen=True)
class Loaded:
    facts: tuple[Facts, ...]
    inputs: Inputs
    classified: tuple[Classified, ...]
    xbe_sha256: str
    functions_csv: Path
    xbe: Path


def find_xbe(root: Path, explicit: str | None) -> Path | None:
    """The XBE to read: `--xbe` when given, else the first existing `XBE_SEARCH` path."""
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise MissingInputs(f"--xbe {path}: no such file")
        return path
    for relative in XBE_SEARCH:
        if (root / relative).is_file():
            return root / relative
    return None


def section_of(sections: list[XbeSection], va: int) -> str:
    for section in sections:
        if section.virtual_addr <= va < section.virtual_addr + section.virtual_size:
            return section.name
    return ""


def read_reasons(path: Path) -> dict[int, str]:
    """entry VA -> reason of the classification rows (empty when the file is absent)."""
    if not path.is_file():
        return {}
    with path.open(encoding="utf-8", newline="") as handle:
        return {int(row["entry_va"], 16): row["reason"] for row in csv.DictReader(handle)}


def load_facts(
    root: Path,
    xbe: Path,
    generated_dir: Path,
    *,
    proof_snapshot: Path | None = None,
) -> Loaded:
    functions_csv = generated_dir / "functions.csv"
    if not functions_csv.is_file():
        raise MissingInputs(
            f"{functions_csv}: not found. Pass --generated-dir with functions.csv and "
            "flirt_names.csv (the badges' generated/retail export)"
        )
    inputs, classified = load_classified(root, xbe, generated_dir, proof_snapshot=proof_snapshot)
    data = xbe.read_bytes()
    sections = parse_xbe(data).sections
    reasons = read_reasons(root / CLASSIFICATION_CSV)
    names = inputs.name_overlay.rows if inputs.name_overlay is not None else {}
    facts = []
    for item in classified:
        function = item.function
        row = names.get(function.entry_va) if item.region == "game" else None
        facts.append(
            Facts(
                va=function.entry_va,
                size=function.size_bytes,
                name=function.name,
                region=item.region,
                evidence=item.evidence,
                section=section_of(sections, function.entry_va),
                reason=reasons.get(function.entry_va, ""),
                name_confidence=row.confidence if row else "",
                name_evidence=row.evidence if row else "",
            )
        )
    return Loaded(
        tuple(facts), inputs, classified, hashlib.sha256(data).hexdigest(), functions_csv, xbe
    )


def load_proven(loaded: Loaded) -> tuple[frozenset[int], str]:
    """(VAs the replayed proof snapshot counts as replaced and proven, why unavailable or "").

    A stale or unusable snapshot (the badges report `unknown` proof then) gives an empty set and
    the reason, so no number is invented.
    """
    try:
        _, proven_class = proven_classes(loaded.inputs, loaded.classified)
    except SystemExit as error:
        return frozenset(), str(error).split(". ", 1)[0]
    return frozenset(proven_class), ""


def load_registered(loaded: Loaded) -> tuple[frozenset[int], str]:
    """(VAs with a registration in `src/game`, why unavailable or "")."""
    tree = loaded.inputs.game_tree
    if tree is None:
        return frozenset(), loaded.inputs.game_tree_error or "no src/game tree"
    return frozenset(tree.registrations), ""
