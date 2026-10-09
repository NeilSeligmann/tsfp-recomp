# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""The snapshot command-line flags of `tools.play` (T1153), kept apart so the play CLI stays small."""

import argparse
import re
from pathlib import Path

#: flags that take one value, then flags that take none
VALUE_FLAGS = (
    "--snapshot-at-poll",
    "--snapshot-before-stop",
    "--snapshot-margin",
    "--snapshot-dir",
    "--resume-snapshot",
    "--resume-timeout",
)
SWITCH_FLAGS = ("--snapshot-continue", "--snapshot-gzip", "--snapshot-fast-verify")
DEFAULT_MARGIN = 30
INNER_ENV = "TSFP_SNAPSHOT_INNER"
CONSUMED = re.compile(r"input replay: consumed (\d+) of \d+ recorded polls")


def add_arguments(parser: argparse.ArgumentParser) -> None:
    group = parser.add_argument_group(
        "snapshot (T1153, docs/state-snapshot.md)",
        "whole-process DMTCP snapshot of the run at a port 0 poll index, and resume of it. "
        "Valid only for the exact host binary and libraries it was taken on.",
    )
    group.add_argument(
        "--snapshot-at-poll",
        type=int,
        metavar="N",
        help="take a snapshot when port 0 has been polled N times (the title polls once per frame), "
        "then end the run unless --snapshot-continue. Needs --host or --no-build",
    )
    group.add_argument(
        "--snapshot-before-stop",
        type=Path,
        metavar="RUN_DIR",
        help="snapshot at (polls consumed by the earlier run RUN_DIR) minus --snapshot-margin",
    )
    group.add_argument(
        "--snapshot-margin",
        type=int,
        default=DEFAULT_MARGIN,
        metavar="K",
        help=f"polls before the earlier stop (default {DEFAULT_MARGIN})",
    )
    group.add_argument(
        "--snapshot-dir",
        type=Path,
        metavar="DIR",
        help="snapshot directory (default tmp/snapshots/<time>), holds manifest.json",
    )
    group.add_argument(
        "--snapshot-continue",
        action="store_true",
        help="let the run that took the snapshot continue to its own stop",
    )
    group.add_argument(
        "--snapshot-gzip",
        action="store_true",
        help="compress the images (smaller, seconds slower)",
    )
    group.add_argument(
        "--resume-snapshot",
        type=Path,
        metavar="DIR",
        help="resume the snapshot in DIR and run to the stop; needs no --disc. Refuses loudly when "
        "the host binary, a mapped library, the XBE, the record or an image differs",
    )
    group.add_argument(
        "--resume-timeout",
        type=int,
        default=600,
        metavar="S",
        help="hard kill of a resumed run after S seconds (default 600)",
    )
    group.add_argument(
        "--snapshot-fast-verify",
        action="store_true",
        help="resume: check image sizes only instead of sha256 (library hashes are always checked)",
    )


def wants_snapshot(args: argparse.Namespace) -> bool:
    return args.snapshot_at_poll is not None or args.snapshot_before_stop is not None


def strip_flags(argv: list[str]) -> list[str]:
    """`argv` without every snapshot flag and its value, for the inner tools.play."""
    kept: list[str] = []
    skip = False
    for token in argv:
        if skip:
            skip = False
            continue
        name = token.split("=", 1)[0]
        if name in VALUE_FLAGS:
            skip = "=" not in token
            continue
        if name in SWITCH_FLAGS:
            continue
        kept.append(token)
    return kept


def polls_consumed(run_dir: Path) -> int | None:
    """Polls the earlier run consumed (host.out line `input replay: consumed N of M recorded polls`)."""
    path = run_dir / "host.out"
    if not path.is_file():
        return None
    found = CONSUMED.findall(path.read_text(errors="replace"))
    return int(found[-1]) if found else None


def resolve_poll(args: argparse.Namespace) -> int:
    """The poll index to snapshot at, from --snapshot-at-poll or --snapshot-before-stop."""
    if args.snapshot_at_poll is not None:
        if args.snapshot_at_poll < 1:
            raise SystemExit("snapshot: --snapshot-at-poll needs N >= 1")
        return int(args.snapshot_at_poll)
    consumed = polls_consumed(args.snapshot_before_stop)
    if consumed is None:
        raise SystemExit(
            f"snapshot: {args.snapshot_before_stop}/host.out has no 'input replay: consumed N of M "
            "recorded polls' line, that run did not replay a record to its stop"
        )
    return max(1, consumed - args.snapshot_margin)
