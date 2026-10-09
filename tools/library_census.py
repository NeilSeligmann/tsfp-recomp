# SPDX-License-Identifier: GPL-3.0-or-later
"""Census of the library-classified functions by region and evidence (T1506).

    /workspace/.venv/bin/python -m tools.library_census [--root .] [--strings]

Read-only measurement. Region = the XBE section holding the entry VA; inside `.text` the
region is split into import thunks (a single `jmp [mem]`), the evidence kinds (xtlid,
flirt, named) and the rest. Prints counts only, never addresses or bytes.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

from tools.coverage import (
    REGION_LIBRARY,
    build_report,
    discover_artifacts,
    load_inputs,
    parse_xbe,
)
from tools.xbe.model import Xbe

_STRING = re.compile(rb"[\x20-\x7e]{6,}")


def section_of(xbe: Xbe, va: int) -> str:
    for section in xbe.sections:
        if section.virtual_addr <= va < section.virtual_addr + section.virtual_size:
            return section.name
    return "?"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."), help="repository root")
    parser.add_argument("--xbe", type=Path, default=None, help="XBE path (default: discovered)")
    parser.add_argument("--strings", action="store_true", help="count strings per section")
    args = parser.parse_args(argv)
    artifacts = discover_artifacts(args.root, xbe=args.xbe)
    inputs = load_inputs(artifacts, root=args.root, collect_tests=False)
    report = build_report(inputs)
    data = artifacts.xbe.read_bytes()
    xbe = parse_xbe(data)
    table: Counter[tuple[str, str, str, bool]] = Counter()
    for item in report.classified:
        if item.region != REGION_LIBRARY:
            continue
        va = item.function.entry_va
        offset = xbe.va_to_offset(va)
        kind = "other"
        if (
            offset is not None
            and item.function.size_bytes == 6
            and data[offset : offset + 2] == b"\xff\x25"
        ):
            kind = "import-thunk"
        table[(section_of(xbe, va), item.evidence, kind, item.is_named)] += 1
    print("section evidence kind named count")
    for key, count in sorted(table.items()):
        print(*key, count)
    xtlid = inputs.xtlid_addresses or frozenset()
    flirt = inputs.flirt_addresses or frozenset()
    per: dict[str, Counter[str]] = {}
    for item in report.classified:
        if item.region != REGION_LIBRARY:
            continue
        va = item.function.entry_va
        row = per.setdefault(section_of(xbe, va), Counter())
        row["total"] += 1
        row["named" if item.is_named else "unnamed"] += 1
        row["xtlid-known"] += va in xtlid
        row["flirt-matched"] += va in flirt
        if not item.is_named:
            size = item.function.size_bytes
            row[
                "unnamed<=16B" if size <= 16 else "unnamed<=64B" if size <= 64 else "unnamed>64B"
            ] += 1
    print("per-section summary")
    for name, row in sorted(per.items()):
        print(name, dict(row))
    total = sum(table.values())
    named = sum(c for k, c in table.items() if k[3])
    print(f"library total {total} named {named}", file=sys.stderr)
    if args.strings:
        for section in xbe.sections:
            blob = data[section.raw_addr : section.raw_addr + section.raw_size]
            print(section.name, len(_STRING.findall(blob)), "strings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
