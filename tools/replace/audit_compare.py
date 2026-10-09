"""Compare two `tools.replace audit` JSON files per function (T1252 before/after helper).

Reports the functions whose eligibility changed in either direction and the per-function
safe/unresolved/unsafe site counts that changed, so a rule change can be checked for
regressions (a previously eligible function becoming ineligible must never happen).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load(path: Path) -> dict[str, dict[str, object]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    return {item["va"]: item for item in document["functions"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    args = parser.parse_args()
    before, after = load(args.before), load(args.after)
    if before.keys() != after.keys():
        print("function sets differ", sorted(before.keys() ^ after.keys()))
        return 1
    fixed, broken, moved = [], [], []
    for va in sorted(before):
        old, new = before[va], after[va]
        if old["eligible"] != new["eligible"]:
            (fixed if new["eligible"] else broken).append(va)
        counts_old = tuple(old.get(key) for key in ("safe", "unresolved", "unsafe"))
        counts_new = tuple(new.get(key) for key in ("safe", "unresolved", "unsafe"))
        if counts_old != counts_new:
            moved.append((va, counts_old, counts_new))
    eligible_before = sum(1 for item in before.values() if item["eligible"])
    eligible_after = sum(1 for item in after.values() if item["eligible"])
    print(f"functions {len(before)} eligible {eligible_before} -> {eligible_after}")
    print("newly eligible:", fixed)
    print("newly INELIGIBLE (regressions):", broken)
    print("site count changes:", moved)
    return 1 if broken else 0


if __name__ == "__main__":
    raise SystemExit(main())
