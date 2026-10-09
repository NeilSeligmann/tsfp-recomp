# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""`python -m tools.snapshot`: build-dmtcp, locate, verify DIR, roundtrip (T1153, docs/state-snapshot.md)."""

import argparse
import json
import sys
from pathlib import Path

from tools.snapshot import dmtcp, manifest, roundtrip


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="python -m tools.snapshot", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("build-dmtcp", help="build the pinned, patched DMTCP into tmp/dmtcp")
    sub.add_parser("locate", help="print the DMTCP binaries and plugin in use")
    verify = sub.add_parser(
        "verify", help="check a snapshot against the current host and libraries"
    )
    verify.add_argument("snapshot", type=Path)
    verify.add_argument("--host", type=Path, help="the host you expect to continue")
    verify.add_argument("--fast", action="store_true", help="image sizes only, not sha256")
    roundtrip.add_arguments(sub.add_parser("roundtrip", help="straight run vs resumed runs"))
    return parser


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    if args.command == "build-dmtcp":
        dmtcp.build()
        return 0
    if args.command == "locate":
        print(json.dumps({key: str(value) for key, value in dmtcp.locate().items()}, indent=2))
        return 0
    if args.command == "verify":
        snapshot = args.snapshot.resolve()
        data = manifest.load(snapshot)
        problems = manifest.verify(data, snapshot, host_override=args.host, fast=args.fast)
        for problem in problems:
            print(f"snapshot: REFUSED: {problem}")
        print("snapshot: valid" if not problems else f"snapshot: {len(problems)} problem(s)")
        return 0 if not problems else 2
    return roundtrip.run_roundtrip(args)


if __name__ == "__main__":
    sys.exit(main())
