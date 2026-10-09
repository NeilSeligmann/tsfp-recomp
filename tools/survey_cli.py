# SPDX-License-Identifier: GPL-3.0-or-later
"""Triage disc images for symbol-bearing executables, ranked by value."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

from tools.survey import looks_like_executable, survey_executable


def list_iso(iso: Path) -> list[tuple[str, int]]:
    """List (path, size) for an ISO9660 image using 7z."""
    result = subprocess.run(["7z", "l", str(iso)], capture_output=True, text=True, check=False)
    entries: list[tuple[str, int]] = []
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 6 or not parts[0][:4].isdigit():
            continue
        if "D" in parts[2]:
            continue
        try:
            size = int(parts[3])
        except ValueError:
            continue
        entries.append((parts[-1], size))
    return entries


def extract(iso: Path, member: str) -> bytes:
    return subprocess.run(
        ["7z", "e", "-so", str(iso), member], capture_output=True, check=True
    ).stdout


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Find symbol-bearing executables inside disc images."
    )
    parser.add_argument(
        "images", type=Path, nargs="+", help="disc images, or loose executables, to survey"
    )
    parser.add_argument(
        "--all", action="store_true", help="report every executable, not just scoring ones"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    for image in args.images:
        if not image.is_file():
            print(f"{image}: not a file")
            continue
        print(f"\n########## {image.name} ##########")

        # A loose executable, not a container. Demo discs sometimes arrive already
        # extracted, and refusing to survey a bare XBE would be perverse.
        head = image.open("rb").read(4)
        if head in (b"XBEH", b"\x7fELF"):
            print(survey_executable(image.name, image.read_bytes()).render())
            continue

        candidates = [
            (path, size) for path, size in list_iso(image) if looks_like_executable(path, size)
        ]
        if not candidates:
            print("  (no executables found)")
            continue
        reports = []
        for path, _size in candidates:
            try:
                data = extract(image, path)
            except subprocess.CalledProcessError:
                continue
            reports.append(survey_executable(path, data))
        for report in sorted(reports, key=lambda r: -r.score):
            if report.score == 0 and not args.all:
                continue
            print(report.render())
        scoring = sum(1 for r in reports if r.score > 0)
        print(f"  -- {len(reports)} executables, {scoring} with something of interest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
