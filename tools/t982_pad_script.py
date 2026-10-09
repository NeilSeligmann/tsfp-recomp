#!/usr/bin/env python3
# ruff: noqa: E501
"""T982 combat pad script: the recorded Story input followed by a scripted engage tail (FABRICATED input).

The tail repeats a cycle that walks forward, sweeps the aim left and right over the level's open ground and holds the
fire trigger (RT) through the sweeps, so any enemy inside the sweep arc is shot at.  The script is open loop (no
feedback from the game), so a hit is evidence only when frames show one.

    python -m tools.t982_pad_script --record tmp/recorded-input-story-mode --cycles 30 --out tmp/t982/engage.pad
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

CYCLE = (
    "90 LY=32767 RX=-9000 RT=255",
    "90 LY=32767 RX=9000 RT=255",
    "40 RT=255",
    "20",
    "120 LY=20000 RX=-14000 RT=255",
    "120 LY=20000 RX=14000 RT=255",
    "60 LX=-32767",
    "60 LX=32767",
    "30 RX=26000",
    "30 RX=-26000",
    "30 A=255",
    "20",
)


# T1222 calibration tail: one stick axis at a time with rest in between, so frames show the turn rate and the pitch limits.
CALIBRATE = (
    "120",
    "240 RY=-32767",
    "180",
    "90 RY=32767",
    "180",
    "90 RY=32767",
    "180",
    "90 RX=32767",
    "180",
    "90 RX=-32767",
    "180",
)

TAILS = {"engage": CYCLE, "calibrate": CALIBRATE}


def build(record: Path, cycles: int, tail: tuple[str, ...] = CYCLE) -> tuple[str, int]:
    body = [
        line
        for line in record.read_text().splitlines()
        if line.strip() and not line.startswith("#")
    ]
    lines = [
        "# T982 engage script (FABRICATED scripted pad): recorded Story input + engage tail",
        f"# record: {record.name}, cycles: {cycles}",
    ]
    lines += body
    for _ in range(cycles):
        lines += tail
    polls = sum(int(line.split()[0]) for line in lines if not line.startswith("#"))
    lines.insert(2, f"# polls: {polls}")
    return "\n".join(lines) + "\n", polls


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--record", required=True, help="recorded input file (tsfp-input v1)")
    parser.add_argument(
        "--cycles", type=int, default=30, help="engage cycles appended after the record"
    )
    parser.add_argument("--out", required=True, help="pad script to write")
    parser.add_argument(
        "--tail",
        choices=sorted(TAILS),
        default="engage",
        help="tail cycle appended after the record",
    )
    args = parser.parse_args()
    text, polls = build(Path(args.record), args.cycles, TAILS[args.tail])
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(text)
    print(f"{args.out}: {polls} polls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
