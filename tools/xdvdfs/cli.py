# SPDX-License-Identifier: GPL-3.0-or-later
"""List or extract the contents of an Xbox ISO."""

from __future__ import annotations

import argparse
from pathlib import Path

from tools.xdvdfs import XisoReader


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Read an Xbox ISO (XDVDFS).")
    parser.add_argument("iso", type=Path, help="path to the ISO image")
    parser.add_argument("--list", action="store_true", help="list every entry")
    parser.add_argument(
        "--extract", nargs="*", metavar="PATH", help="extract these paths (all if empty)"
    )
    parser.add_argument("--dest", type=Path, default=Path("extracted"), help="extraction directory")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not args.list and args.extract is None:
        parser.error("give --list or --extract")

    with args.iso.open("rb") as stream:
        reader = XisoReader(stream)
        if args.list:
            entries = reader.list_entries()
            files = [e for e in entries if not e.is_dir]
            for entry in sorted(entries, key=lambda e: e.path):
                kind = "d" if entry.is_dir else "-"
                print(f"{kind} {entry.size:>12d}  {entry.path}")
            total = sum(e.size for e in files)
            print(f"\n{len(files)} files, {len(entries) - len(files)} dirs, {total} bytes")
        if args.extract is not None:
            only = args.extract or None
            written = reader.extract(args.dest, only=only)
            print(f"extracted {len(written)} files to {args.dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
