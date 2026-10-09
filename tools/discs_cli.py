# SPDX-License-Identifier: GPL-3.0-or-later
"""Report which game discs are present and whether they verify."""

from __future__ import annotations

import argparse

from tools.discs import REGISTRY, DiscMismatch, disc_dir, find_disc, verify_disc


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Check the local disc collection.")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="also verify SHA-256 (slow: hashes several gigabytes)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    print(f"disc directory: {disc_dir()}\n")

    missing = 0
    for key, spec in REGISTRY.items():
        path = find_disc(key)
        if path is None:
            missing += 1
            print(f"  [ ] {spec.filename:16s}  MISSING   {spec.title} ({spec.role})")
            continue
        try:
            verify_disc(path, spec, strict=args.strict)
        except DiscMismatch as exc:
            print(f"  [!] {spec.filename:16s}  MISMATCH  {exc}")
        else:
            note = "verified" if args.strict and spec.sha256 else "size ok"
            print(f"  [x] {spec.filename:16s}  {note:8s}  {spec.title}")

    print(f"\n{len(REGISTRY) - missing}/{len(REGISTRY)} present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
