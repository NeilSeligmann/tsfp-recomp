# SPDX-License-Identifier: GPL-3.0-or-later
"""Print the measured primitive tables and the per-operation call-site census.

python -m tools.primtypes.cli tmp/oxm-extract/retail/default.xbe
"""

from __future__ import annotations

import argparse
from collections.abc import Sequence
from pathlib import Path

from tools.d3dscan.oracle import create_device
from tools.primtypes import sites, translate
from tools.shaderscan.image import Image

KINDS = range(0, 12)
INDICES = -1
DRAWS = (
    (translate.DRAW_VERTICES, "DrawVertices", [0, 0, 3]),
    (translate.DRAW_INDEXED_VERTICES, "DrawIndexedVertices", [0, 3, INDICES]),
)


def name(operation: int) -> str:
    return translate.OPERATION_NAMES.get(operation, f"{operation:#x}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbe", type=Path, help="path to default.xbe, relative")
    args = parser.parse_args(argv)
    oracle = translate.load_oracle(args.xbe)
    print("batcher 0x18EA0, kind -> operation by vertex count (1, 2, 3, 4):")
    for kind in KINDS:
        row = [name(translate.batcher(oracle, kind, count).operation) for count in (1, 2, 3, 4)]
        print(f"  {kind:2d}: {' '.join(row)}")
    print("indexed 0x18FF0, kind -> operation (key 0x1234):")
    for kind in KINDS:
        print(f"  {kind:2d}: {name(translate.indexed(oracle, kind, 0x1234).operation)}")
    print("mesh 0x1E770, kind (0-based) -> operation:")
    for kind in KINDS:
        print(f"  {kind:2d}: {name(translate.mesh(oracle, kind).operation)}")
    if not create_device(oracle).succeeded:
        print("CreateDevice failed in the emulator")
        return 1
    indices = translate.prepare_indexed_state(oracle)
    for entry, label, arguments in DRAWS:
        arguments = [indices if each == INDICES else each for each in arguments]
        unchanged = True
        for primitive in range(1, 11):
            arguments[0] = primitive
            unchanged &= translate.library_pushes(oracle, entry, arguments) == primitive
        print(f"{label} 0x{entry:X} pushes its primitive argument unchanged: {unchanged}")
    instructions = sites.decode_text(Image.load(args.xbe))
    print("writers of the primitive global:", [hex(each) for each in sites.writers(instructions)])
    for label, count in sorted(sites.by_operation(sites.census(oracle, instructions)).items()):
        print(f"  {count:3d} sites: {label}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
