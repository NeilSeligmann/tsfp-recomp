# SPDX-License-Identifier: GPL-3.0-or-later
"""Seeded random sample of the name overlay for the independent precision audit (T1513).

Reads tools/data/function_names.csv and keeps game rows that are neither seeded from src/game
replacements nor MEASURED, whose VA lies in --lo/--hi. Each row is tagged with the last task id
named in its evidence text (the naming pass that wrote it). It draws --n rows with a fixed seed,
stratified by that tag so large passes do not drown small ones. Output is a CSV with the VA, name,
confidence, tag and the first words of the evidence. Deterministic for a given overlay and seed.
"""

from __future__ import annotations

import argparse
import csv
import random
import re
import sys
from collections import defaultdict
from pathlib import Path

NAMES = Path("tools/data/function_names.csv")
TASK = re.compile(r"\bT1[0-9]{3}\b")


def pass_tag(evidence: str) -> str:
    """The last task id in the evidence (the latest pass that touched the row), else 'untagged'."""
    tasks = TASK.findall(evidence)
    return tasks[-1] if tasks else "untagged"


def load(lo: int, hi: int) -> list[dict[str, str]]:
    rows = []
    with NAMES.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            va = int(row["entry_va"], 16)
            if not lo <= va < hi:
                continue
            if "seeded from src/game" in row["evidence"] or row["confidence"] == "MEASURED":
                continue
            row["tag"] = pass_tag(row["evidence"])
            rows.append(row)
    return rows


def sample(rows: list[dict[str, str]], n: int, seed: int) -> list[dict[str, str]]:
    """Stratified draw: every tag with rows gets at least one, the rest in proportion to size."""
    rng = random.Random(seed)
    groups: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        groups[row["tag"]].append(row)
    total = sum(len(g) for g in groups.values())
    chosen: list[dict[str, str]] = []
    for tag in sorted(groups):
        group = sorted(groups[tag], key=lambda r: r["entry_va"])
        quota = max(1, round(n * len(group) / total))
        chosen += rng.sample(group, min(quota, len(group)))
    rng.shuffle(chosen)
    return chosen[:n] if len(chosen) > n else chosen


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lo", type=lambda s: int(s, 0), required=True)
    parser.add_argument("--hi", type=lambda s: int(s, 0), required=True)
    parser.add_argument("--n", type=int, default=120)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--out", type=Path, help="write the CSV here (default stdout)")
    args = parser.parse_args()
    rows = load(args.lo, args.hi)
    picked = sample(rows, args.n, args.seed)
    out = args.out.open("w", newline="", encoding="utf-8") if args.out else sys.stdout
    writer = csv.writer(out)
    writer.writerow(["entry_va", "name", "confidence", "tag", "evidence_head"])
    for row in sorted(picked, key=lambda r: r["entry_va"]):
        writer.writerow(
            [row["entry_va"], row["name"], row["confidence"], row["tag"], row["evidence"][:120]]
        )
    print(f"{len(picked)} of {len(rows)} rows in range, seed {args.seed}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
