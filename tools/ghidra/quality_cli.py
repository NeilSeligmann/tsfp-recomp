# SPDX-License-Identifier: GPL-3.0-or-later
"""Report decompilation health markers across an export."""

from __future__ import annotations

import argparse
from pathlib import Path

from tools.ghidra.quality import score_export


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Score a decompilation export.")
    parser.add_argument("export", type=Path, help="export directory (scanned recursively for *.c)")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.export.is_dir():
        raise SystemExit(f"not a directory: {args.export}")
    print(score_export(args.export).render())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
