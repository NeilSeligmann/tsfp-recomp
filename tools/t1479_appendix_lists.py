# SPDX-License-Identifier: GPL-3.0-or-later
"""T1479 appendix: write the fixed-matrix batch lists from the frozen drafts and the census rows.

The group of each root is read from the header of its draft ("T1479 appendix draft 0xVA (<group>").
Groups: plain, legacy (live vector state), fp-scalar (fp-scalar-v1), call (live call closure).
Every row keeps its census recipe and gets the fixed matrix: seeds 20261001/20261006, O0/O2/O3,
600 cases.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

DATA = Path("docs/data/t1479-appendix-census")
LIST = Path("docs/data/t1772-replacement-census/band-T1479/appendix-previously-mentioned.json")
STATUS = (
    "static eligibility only (T1772 census) after triage; "
    "fixed matrix seeds 20261001/20261006 x O0/O2/O3, count 600"
)


def draft_group(path: Path) -> str:
    match = re.search(r"draft 0x[0-9A-Fa-f]{8} \((plain|legacy|fp-scalar|call)\b", path.read_text())
    if not match:
        raise SystemExit(f"{path}: no group in the header")
    return match.group(1)


def build(group: str, vas: list[str], rows: dict[str, dict]) -> dict:
    functions = []
    for va in vas:
        row = json.loads(json.dumps(rows[va]))
        row["band"] = "T1479"
        recipe = row["recipe"]
        recipe["optimizations"] = [0, 2, 3]
        recipe["seeds"] = [20261001, 20261006]
        recipe["count"] = 600
        if group in ("legacy", "fp-scalar"):
            recipe["live_call_closure"] = True
            recipe["live_vector_state"] = True
            recipe["vector_mode"] = "legacy" if group == "legacy" else "fp-scalar-v1"
        functions.append(row)
    document = {
        "task": "T1479",
        "band": "T1479",
        "kind": "appendix",
        "group": group,
        "count": len(functions),
        "status": STATUS,
    }
    if group == "legacy":
        document["vector_mode"] = "legacy"
    if group == "fp-scalar":
        document["vector_mode"] = "fp-scalar-v1"
    document["functions"] = functions
    return document


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", action="append", default=[], help="restrict to these draft VAs")
    parser.add_argument("--suffix", default="", help="suffix of the written list file name")
    args = parser.parse_args()
    rows = {r["va"][2:].lower(): r for r in json.loads(LIST.read_text())}
    groups: dict[str, list[str]] = {}
    for draft in sorted((DATA / "drafts").glob("*.c.txt")):
        va = draft.name[:8]
        if args.only and va not in {v.lower() for v in args.only}:
            continue
        groups.setdefault(draft_group(draft), []).append(va)
    for group, vas in sorted(groups.items()):
        document = build(group, vas, rows)
        out = DATA / f"{group}-six-tuples{args.suffix}.json"
        out.write_text(json.dumps(document, indent=2) + "\n")
        print(out, len(vas), " ".join(vas))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
