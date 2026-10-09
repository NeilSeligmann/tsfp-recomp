# SPDX-License-Identifier: GPL-3.0-or-later
"""Sample overlay rows whose VA is listed in function_additions.csv (T1513)."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from tools import precision_sample as ps


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--n", type=int, default=40)
    parser.add_argument("--seed", type=int, default=1304)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    with Path("tools/data/function_additions.csv").open(newline="", encoding="utf-8") as handle:
        header = next(csv.reader(handle))
    with Path("tools/data/function_additions.csv").open(newline="", encoding="utf-8") as handle:
        vas = {int(r[header[0]], 16) for r in csv.DictReader(handle)}
    rows = [r for r in ps.load(0, 1 << 32) if int(r["entry_va"], 16) in vas]
    out = ps.sample(rows, args.n, args.seed)
    with Path(args.out).open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=["entry_va", "name", "confidence", "tag", "evidence"],
            extrasaction="ignore",
        )
        writer.writeheader()
        writer.writerows(out)
    print(len(rows), "candidates", len(out), "sampled")


if __name__ == "__main__":
    main()
