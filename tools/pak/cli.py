# SPDX-License-Identifier: GPL-3.0-or-later
"""List or extract a Free Radical PAK container."""

from __future__ import annotations

import argparse
from pathlib import Path

from tools.pak import parse_pak


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Read a Free Radical PAK archive.")
    parser.add_argument("pak", type=Path, help="path to the .pak file")
    parser.add_argument("--list", action="store_true", help="list entries")
    parser.add_argument("--extract", type=Path, metavar="DIR", help="extract entries here")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    data = args.pak.read_bytes()
    archive = parse_pak(data)

    packed = sum(1 for e in archive.entries if e.compressed)
    named = sum(1 for e in archive.entries if e.name)
    print(
        f"{archive.magic}, {len(archive.entries)} entries, {archive.entry_stride}-byte stride "
        f"({packed} gzip, {len(archive.entries) - packed} stored, {named} named)"
    )

    if args.list:
        for entry in archive.entries:
            label = entry.name if entry.name else f"<{entry.key:#010x}>"
            mark = "gz" if entry.compressed else "--"
            print(f"  {mark} {entry.uncompressed_size:>10d}  {label}")

    if args.extract is not None:
        args.extract.mkdir(parents=True, exist_ok=True)
        for index, entry in enumerate(archive.entries):
            stem = entry.name.replace("/", "_") if entry.name else f"{entry.key:08x}"
            target = args.extract / f"{index:04d}_{stem}"
            target.write_bytes(archive.entry_data(data, entry))
        print(f"extracted {len(archive.entries)} entries to {args.extract}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
