# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure a libsig match CSV against the surface table, `.XTLID` and FLIRT.

    ./.venv/bin/python -m tools.libsig.report --matches tmp/libsig/names-5849.csv \\
        --xbe tmp/oxm-extract/retail/default.xbe --surface src/xbox/xdk_surface.c \\
        --xtlid-db tmp/flirt-work/xtlid.xml --flirt generated/retail/flirt_names.csv

Prints counts and the full list of disagreements (never just a count).
"""

from __future__ import annotations

import argparse
import csv
from collections import Counter, defaultdict
from pathlib import Path

from tools.flirt.cli import normalise_symbol
from tools.libsig.cli import load_surface
from tools.xbe import parse_xbe
from tools.xtlid import parse_xtlid_db, resolve_xtlid


def read_matches(path: Path) -> dict[int, dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return {int(row["address"], 16): row for row in csv.DictReader(handle)}


def names_of(row: dict[str, str]) -> set[str]:
    if row.get("reason") == "xtlid":
        return {row["symbol"]}  # `.XTLID` chose the member, so it cannot be its own check
    return set(row["alias_set"].split("|")) if row["alias_set"] else {row["symbol"]}


def compare(label: str, truth: dict[int, str], rows: dict[int, dict[str, str]]) -> None:
    tally: Counter[str] = Counter()
    bad = []
    for address, name in sorted(truth.items()):
        row = rows.get(address)
        if row is None:
            tally["unmatched"] += 1
            continue
        candidates = {normalise_symbol(n) for n in names_of(row)}
        kind = row["confidence"]
        if normalise_symbol(name) in candidates:
            tally[f"agree/{kind}"] += 1
        else:
            tally[f"DISAGREE/{kind}"] += 1
            bad.append((address, name, kind, sorted(names_of(row))))
    print(f"{label}: {len(truth)} names -> {dict(tally)}")
    for address, name, kind, got in bad:
        print(f"  DISAGREE 0x{address:08x} truth={name} [{kind}] got={got}")


def surface_summary(
    rows: dict[int, dict[str, str]], surface: dict[int, tuple[str, str | None]]
) -> Counter[str]:
    """Surface addresses by outcome, keeping the two kinds of unnamed apart.

    `unique` is high or resolved. `alias_group` is identical masked bodies under several names (the
    bytes cannot decide, whatever the evidence). `ambiguous` is several bodies that more evidence
    might tell apart. The rest are weak, propagated and no row.
    """
    tally: Counter[str] = Counter()
    for address in surface:
        row = rows.get(address)
        kind = row["confidence"] if row else "none"
        tally["unique" if kind in ("high", "resolved") else kind] += 1
    return tally


def multiplicity(rows: dict[int, dict[str, str]]) -> None:
    """A real library function is linked once, so one name at many addresses is noise.

    Buckets unique-name rows by body length and prints the share whose symbol also
    names another address. Evidence for the weak-length threshold.
    """
    by_symbol: Counter[str] = Counter(r["symbol"] for r in rows.values() if r["symbol"])
    buckets: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    for row in rows.values():
        if not row["symbol"] or row["confidence"] == "propagated":
            continue
        length = int(row["body_len"])
        key = "<8" if length < 8 else "8-15" if length < 16 else "16-31" if length < 32 else "32+"
        buckets[key][0] += 1
        buckets[key][1] += by_symbol[row["symbol"]] > 1
    print(
        "duplicate-name rate by body_len (rows, duplicated):",
        {k: tuple(buckets[k]) for k in ("<8", "8-15", "16-31", "32+") if k in buckets},
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--matches", type=Path, required=True)
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--surface", type=Path, required=True)
    parser.add_argument("--xtlid-db", type=Path, default=None)
    parser.add_argument("--flirt", type=Path, default=None)
    args = parser.parse_args()

    rows = read_matches(args.matches)
    xbe = parse_xbe(args.xbe.read_bytes())
    text = xbe.section_by_name(".text")
    in_text = {
        a for a in rows if text and text.virtual_addr <= a < text.virtual_addr + text.virtual_size
    }
    print("all rows:", dict(Counter(r["confidence"] for r in rows.values())))
    print(".text rows:", len(in_text), dict(Counter(rows[a]["confidence"] for a in in_text)))
    if args.flirt:
        with args.flirt.open(newline="", encoding="utf-8") as handle:
            flirt = {int(r["entry_va"], 16) for r in csv.DictReader(handle)}
        mine = {
            a
            for a, r in rows.items()
            if r["confidence"] in ("high", "propagated", "weak", "resolved")
        }
        print(
            f"FLIRT {len(flirt)} addresses; libsig named(high/weak/propagated) {len(mine)}; "
            f"both {len(flirt & mine)}; libsig-only {len(mine - flirt)}; "
            f"FLIRT-only {len(flirt - mine)}"
        )

    multiplicity(rows)
    surface = load_surface(args.surface)
    per_family: dict[str, Counter[str]] = defaultdict(Counter)
    for address, (family, _) in surface.items():
        row = rows.get(address)
        per_family[family]["total"] += 1
        per_family[family][row["confidence"] if row else "none"] += 1
    print(f"surface {len(surface)} addresses by family:")
    for family in sorted(per_family):
        print(f"  {family:8s} {dict(per_family[family])}")
    summary = surface_summary(rows, surface)
    print(f"surface summary: {dict(summary)}")
    named = sum(1 for a in surface if a in rows)
    print(f"surface addresses with any libsig row: {named}/{len(surface)}")
    compare("surface-table names", {a: n for a, (_, n) in surface.items() if n}, rows)

    if args.xtlid_db:
        database = parse_xtlid_db(args.xtlid_db.read_text(encoding="utf-8", errors="replace"))
        resolved, _ = resolve_xtlid(xbe.xtlid, database)
        compare(".XTLID names", {a: n.name for a, n in resolved.items()}, rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
