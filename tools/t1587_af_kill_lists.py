# SPDX-License-Identifier: GPL-3.0-or-later
"""T1587: T1534 drafting screen with CURRENT recognizers for the 38 roots T1585 made
caller-eligible (AF definite kill). Static only, no proof, no API."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from tools import call_draft_lists as cdl
from tools import name_additions as na
from tools import pilot_lists as pilot
from tools import t1578_newly_reachable_lists as t1578

T1585_ROOTS = frozenset(
    int(v, 16)
    for v in (
        "251d0 29bb0 31fe0 32010 42640 613d0 9d7c0 12cad0 15c4e0 17bf70 18a950 1a0610 1bd440 "
        "312880 318b60 320f50 324e00 325610 32a620 351950 355f10 356130 35af40 35e670 376450 "
        "3861d4 38624e 386fcf 387d70 387da9 38839a 3884ee 3abe60 3acb50 3b3120 3beac0 3bfbe0 "
        "3c44c0"
    ).split()
)
assert len(T1585_ROOTS) == 38
t1578.SCAN_DIRS = (*t1578.SCAN_DIRS, "docs/data/t1583-reach-list001")
t1578.SCAN_GLOBS = (*t1578.SCAN_GLOBS, "docs/data/t1578-newly-reachable-lists/list-*.json")
t1578.SCAN_GLOBS = (*t1578.SCAN_GLOBS, "docs/data/t1578-newly-reachable-lists/excluded-*.json")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--private-root", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--elsewhere-out", type=Path, required=True)
    args = ap.parse_args()
    elsewhere = t1578.mentioned() & T1585_ROOTS
    args.elsewhere_out.write_text(json.dumps(sorted(elsewhere)) + "\n")
    original_rejected = pilot.rejected_vas
    world_init = na.World.__init__

    def init(self: Any, *a: Any, **k: Any) -> None:
        world_init(self, *a, **k)
        outside = {v for v in self.size if v not in T1585_ROOTS}
        pilot.rejected_vas = lambda: set(original_rejected()) | elsewhere | outside

    na.World.__init__ = init  # type: ignore[method-assign]
    return cdl.run(args.private_root.resolve(), args.out_dir)


if __name__ == "__main__":
    raise SystemExit(main())
