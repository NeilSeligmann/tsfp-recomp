# SPDX-License-Identifier: GPL-3.0-or-later
"""T1620: compare two `tools.replace prove` outputs verdict field by verdict field.

Used for the "no change to default behaviour" evidence: the same root proved on the base commit
and on the head commit must give an identical `proof.json` function entry (every field) and an
identical `results.csv` (every per-case outcome, byte for byte).

    python -m tools.replace.proof_diff --va 0x00021740 BASE_PARTIAL_DIR HEAD_PARTIAL_DIR
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


def entry(directory: Path, va: str) -> dict[str, object]:
    document = json.loads((directory / "proof.json").read_text(encoding="utf-8"))
    rows = [row for row in document["functions"] if row.get("va") == va]
    if len(rows) != 1:
        raise SystemExit(f"{directory}: expected exactly one entry for {va}, found {len(rows)}")
    return rows[0]


def compare(base: Path, head: Path, va: str) -> dict[str, object]:
    """Every differing proof field plus the results.csv digests of both sides."""
    left, right = entry(base, va), entry(head, va)
    differing = {
        key: {"base": left.get(key), "head": right.get(key)}
        for key in sorted(set(left) | set(right))
        if left.get(key) != right.get(key)
    }
    digests = {
        side: hashlib.sha256((directory / "results.csv").read_bytes()).hexdigest()
        for side, directory in (("base", base), ("head", head))
    }
    return {
        "va": va,
        "fields_compared": len(set(left) | set(right)),
        "differing_fields": differing,
        "results_csv_sha256": digests,
        "results_csv_identical": digests["base"] == digests["head"],
        "identical": not differing and digests["base"] == digests["head"],
        "verdicts": left.get("verdicts"),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("base", type=Path, help="base partial/ directory (proof.json, results.csv)")
    parser.add_argument("head", type=Path, help="head partial/ directory")
    parser.add_argument(
        "--va", required=True, help="canonical 8-digit hex root address, e.g. 0x00021740"
    )
    args = parser.parse_args(argv)
    result = compare(args.base, args.head, args.va)
    print(json.dumps(result, sort_keys=True))
    return 0 if result["identical"] else 1


if __name__ == "__main__":
    sys.exit(main())
