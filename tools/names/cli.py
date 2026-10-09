# SPDX-License-Identifier: GPL-3.0-or-later
"""Build an asset-name dictionary, or resolve a hashed archive against one."""

from __future__ import annotations

import argparse
from pathlib import Path

from tools.errors import ParseError
from tools.names import build_dictionary, harvest_plaintext, load_c2n_dir, resolve
from tools.pak import PakArchive, parse_pak


def _load_archives(root: Path) -> list[PakArchive]:
    archives: list[PakArchive] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.stat().st_size < 16:
            continue
        try:
            archives.append(parse_pak(path.read_bytes()))
        except ParseError:
            continue
    return archives


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Recover plaintext asset names.")
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build-dictionary", help="collect names into a word list")
    build.add_argument("--disc-root", type=Path, required=True, help="extracted disc tree to scan")
    build.add_argument("--out", type=Path, required=True, help="output word list")

    res = sub.add_parser("resolve", help="name the entries of a hashed archive")
    res.add_argument("--pak", type=Path, required=True, help="a P5CK archive")
    res.add_argument("--dictionary", type=Path, required=True, help="candidate path word list")

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.command == "build-dictionary":
        from_c2n = load_c2n_dir(args.disc_root)
        from_paks = harvest_plaintext(_load_archives(args.disc_root))
        names = sorted(set(from_c2n.values()) | set(from_paks))
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text("\n".join(names) + "\n", encoding="utf-8")
        print(
            f"{len(from_c2n)} names from .c2n indexes, {len(set(from_paks))} from plaintext "
            f"archives -> {len(names)} distinct, written to {args.out}"
        )
        return 0

    candidates = [
        line.strip()
        for line in args.dictionary.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    dictionary = build_dictionary(candidates)
    archive = parse_pak(args.pak.read_bytes())
    resolved, unresolved = resolve([e.key for e in archive.entries], dictionary)
    total = len(archive.entries)
    pct = 100.0 * len(resolved) / total if total else 0.0
    print(f"{args.pak}: {len(resolved)}/{total} named ({pct:.1f}%), {len(unresolved)} unresolved")
    for key, name in sorted(resolved.items(), key=lambda kv: kv[1]):
        print(f"  {key:#010x}  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
