# SPDX-License-Identifier: GPL-3.0-or-later
"""T1227: count the on-screen triangles of live draws by their winding on the finished image.

Input: the JSON lines the host writes with `TSFP_LIVE_DRAW_DUMP_FULL=PATH` (optionally
`_SKIP=N`, `_COUNT=M`; src/gpu/live_vk_pipeline.c `dump_full`): the first culled triangle
strips of at least 200 vertices, each with the vertex program words, the 192 constant rows
and the attribute rows of the first expanded triangles (raw float32 words). Every vertex
runs through `tools.nv2a.interp` (the clean-room reference interpreter) and the window
position oPos is used as is. There x runs right and y runs DOWN (the title's c58.y is
negative), so a positive signed area is a triangle clockwise on the finished image.
Triangles with w <= 0, off screen by more than 100 pixels or of zero area are skipped.

Observation tool, no state. The numbers are MEASURED from our own interpretation of the
title's vertex programs, not from NV2A.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

from tools.nv2a import interp


def words_to_floats(words: list[int]) -> list[float]:
    return [struct.unpack("<f", struct.pack("<I", word))[0] for word in words]


def window_positions(draw: dict[str, object]) -> list[list[float]]:
    program = b"".join(struct.pack("<4I", *row) for row in draw["program"])  # type: ignore[union-attr]
    flat = words_to_floats(draw["constants"])  # type: ignore[arg-type]
    constants = [flat[row * 4 : row * 4 + 4] for row in range(192)]
    positions = []
    for vertex in draw["vertices"]:  # type: ignore[union-attr]
        floats = words_to_floats(vertex)
        inputs = [floats[slot * 4 : slot * 4 + 4] for slot in range(16)]
        positions.append(interp.run(program, inputs, constants).outputs[0])
    return positions


def count_windings(
    positions: list[list[float]], margin: float = 100.0, width: float = 640.0, height: float = 480.0
) -> tuple[int, int]:
    clockwise = counter = 0
    for index in range(len(positions) // 3):
        a, b, c = positions[3 * index : 3 * index + 3]
        if min(a[3], b[3], c[3]) <= 0.0:
            continue
        if not all(
            -margin < p[0] < width + margin and -margin < p[1] < height + margin for p in (a, b, c)
        ):
            continue
        area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])
        if abs(area) < 1e-6:
            continue
        if area > 0.0:
            clockwise += 1
        else:
            counter += 1
    return clockwise, counter


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("dump", type=Path, help="JSON lines written by TSFP_LIVE_DRAW_DUMP_FULL")
    args = parser.parse_args(argv)
    total_clockwise = total_counter = 0
    for line in args.dump.read_text().splitlines():
        if not line.strip():
            continue
        clockwise, counter = count_windings(window_positions(json.loads(line)))
        total_clockwise += clockwise
        total_counter += counter
        print(f"draw: clockwise {clockwise} counter-clockwise {counter}")
    print(
        f"TOTAL clockwise {total_clockwise} counter-clockwise {total_counter} "
        "(finished image, y down)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
