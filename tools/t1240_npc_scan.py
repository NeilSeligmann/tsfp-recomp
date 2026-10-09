#!/usr/bin/env python3
"""T1240: find NPC position words (float x, y, z triples) in two read-only guest snapshots (`mem` command of tools.t1240_drive).

A triple qualifies when it lies inside a box around the player in snapshot A, is not the player's own position, and is
UNCHANGED between A and B (B taken after the player moved, NPCs standing).  Output: address, x y z, and the values in B.
"""

# ruff: noqa: ANN201, E501

import argparse
import sys
from pathlib import Path

import numpy as np

from tools.guest_mem_probe import load_snapshot


def triples(
    snapshot: dict[int, bytes], center: tuple[float, float, float], radius: float, height: float
):
    """Yield (address, x, y, z) for every aligned float triple inside the box around `center`."""
    cx, cy, cz = center
    for start, data in snapshot.items():
        usable = len(data) // 4 * 4
        words = np.frombuffer(data[:usable], dtype="<f4")
        if len(words) < 3:
            continue
        with np.errstate(invalid="ignore"):
            hit = (
                (np.abs(words[:-2] - cx) < radius)
                & (np.abs(words[1:-1] - cy) < height)
                & (np.abs(words[2:] - cz) < radius)
                & (np.abs(words[:-2]) > 0.5)
                & (np.abs(words[1:-1]) > 0.05)
                & (np.abs(words[2:]) > 0.5)
            )
        for index in np.nonzero(hit)[0]:
            yield (
                start + int(index) * 4,
                float(words[index]),
                float(words[index + 1]),
                float(words[index + 2]),
            )


def value_at(snapshot: dict[int, bytes], address: int) -> tuple[float, float, float] | None:
    for start, data in snapshot.items():
        if start <= address and address + 12 <= start + len(data):
            return tuple(
                float(v)
                for v in np.frombuffer(data[address - start : address - start + 12], dtype="<f4")
            )
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot_a")
    parser.add_argument("snapshot_b")
    parser.add_argument(
        "--center", nargs=3, type=float, required=True, help="player x y z in snapshot A"
    )
    parser.add_argument("--radius", type=float, default=60.0)
    parser.add_argument("--height", type=float, default=12.0)
    parser.add_argument(
        "--min-distance", type=float, default=3.0, help="skip the player's own copies"
    )
    args = parser.parse_args()
    a = load_snapshot(Path(args.snapshot_a))
    b = load_snapshot(Path(args.snapshot_b))
    kept = 0
    for address, x, y, z in triples(a, tuple(args.center), args.radius, args.height):
        if (x - args.center[0]) ** 2 + (z - args.center[2]) ** 2 < args.min_distance**2:
            continue
        other = value_at(b, address)
        if other is None or other != (x, y, z):
            continue
        print(f"0x{address:08X} {x:.2f} {y:.2f} {z:.2f}")
        kept += 1
    print(f"# {kept} unchanged triples", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
