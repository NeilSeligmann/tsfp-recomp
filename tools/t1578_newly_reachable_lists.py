# SPDX-License-Identifier: GPL-3.0-or-later
"""T1578: rerun the T1534 drafting screen with the CURRENT recognizers, restricted to roots
refused in T1534 and not proven/registered/drafted/rejected anywhere. Static only, no API."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any

from tools import call_draft_lists as cdl
from tools import name_additions as na
from tools import pilot_lists as pilot

SCAN_DIRS = (
    "docs/data/t1534-call-draft-lists",
    "docs/data/t-pilot-lists",
    "docs/data/t1541-list002",
    "docs/evidence/t1543",
)
SCAN_GLOBS = (
    "docs/t-call-draft-list00*.md",
    "docs/t-cheap-model-pilot-list*.md",
    "docs/data/t1544*",
)
T1534_POPULATION_FILES = {
    "pre-audit-dispositions.json",
    "report.json",
    "pins-before.json",
    "pins-after.json",
}
HEX = re.compile(r"(?:0x|sub_)([0-9a-fA-F]{6,8})\b")


def mentioned() -> set[int]:
    """VAs named by any listed artefact, except T1534's own refusal population files."""
    files: list[Path] = []
    for d in SCAN_DIRS:
        files += [p for p in Path(d).rglob("*") if p.is_file()]
    for g in SCAN_GLOBS:
        for p in Path(".").glob(g):
            files += [q for q in p.rglob("*") if q.is_file()] if p.is_dir() else [p]
    found: set[int] = set()
    for p in files:
        if p.name in T1534_POPULATION_FILES and "t1534" in str(p):
            continue
        found |= {int(m, 16) for m in HEX.findall(p.read_text(errors="ignore"))}
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--private-root", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--elsewhere-out", type=Path, required=True)
    args = ap.parse_args()
    rows = json.loads(Path("docs/data/t1534-call-draft-lists/report.json").read_text())[
        "dispositions"
    ]
    population = {int(r["va"], 16) for r in rows if r["status"] == "refused"}
    elsewhere = mentioned() & population
    args.elsewhere_out.write_text(json.dumps(sorted(elsewhere)) + "\n")
    original_rejected = pilot.rejected_vas
    world_init = na.World.__init__

    def init(self: Any, *a: Any, **k: Any) -> None:
        world_init(self, *a, **k)
        outside = {v for v in self.size if v not in population}
        pilot.rejected_vas = lambda: set(original_rejected()) | elsewhere | outside

    na.World.__init__ = init  # type: ignore[method-assign]
    return cdl.run(args.private_root.resolve(), args.out_dir)


if __name__ == "__main__":
    raise SystemExit(main())
