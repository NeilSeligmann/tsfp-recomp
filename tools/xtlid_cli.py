# SPDX-License-Identifier: GPL-3.0-or-later
"""Name an XBE's .XTLID library functions using the XboxDev/xtlid database."""

from __future__ import annotations

import argparse
from pathlib import Path

from tools.xbe import parse_xbe
from tools.xtlid import XTLID_DB_URL, parse_xtlid_db, resolve_xtlid, to_ghidra_symbols


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Resolve an XBE's .XTLID records to XDK function names.",
        epilog=f"Fetch the database from {XTLID_DB_URL}",
    )
    parser.add_argument("xbe", type=Path, help="path to the XBE file")
    parser.add_argument("--db", type=Path, required=True, help="path to xtlid.xml")
    parser.add_argument("--out", type=Path, help="write an 'address name' table here")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    xbe = parse_xbe(args.xbe.read_bytes())
    database = parse_xtlid_db(args.db.read_text(encoding="utf-8", errors="replace"))
    resolved, unresolved = resolve_xtlid(xbe.xtlid, database)

    print(f"database        {len(database)} named XDK functions")
    print(f"XDK build       {xbe.xdk_build}")
    print(f".XTLID records  {len(xbe.xtlid)}")
    print(f"resolved        {len(resolved)}")
    print(f"unresolved      {len(unresolved)}")

    by_library: dict[str, int] = {}
    for entry in resolved.values():
        by_library[entry.library] = by_library.get(entry.library, 0) + 1
    if by_library:
        print("\nby library:")
        for library, count in sorted(by_library.items(), key=lambda kv: -kv[1]):
            print(f"  {library:12s} {count}")

    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(to_ghidra_symbols(resolved), encoding="utf-8")
        print(f"\nwrote {len(resolved)} names to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
