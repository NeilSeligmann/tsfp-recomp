# SPDX-License-Identifier: GPL-3.0-or-later
"""Print the measured input-register assignment for each flags word.

python -m tools.vtxdecl.cli XBE --builder 0x1e970 --length 0x182 --flags 0xd 0x2d
python -m tools.vtxdecl.cli XBE --builder 0x1e970 --length 0x182 --single-bits 18
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

from tools.shaderscan.image import Image
from tools.vtxdecl.block import TYPE_NAMES, builder_code, run_builder, used_registers


def describe(flags: int, registers: dict[int, int]) -> str:
    parts = [
        f"v{reg}={format_byte:#04x}({format_byte >> 4}x{TYPE_NAMES.get(format_byte & 15, '?')})"
        for reg, format_byte in sorted(registers.items())
    ]
    return f"flags {flags:#07x}: " + (" ".join(parts) or "no attributes")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbe", type=Path, help="path to default.xbe, relative")
    parser.add_argument("--builder", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--length", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--flags", type=lambda v: int(v, 0), nargs="*", default=[])
    parser.add_argument("--single-bits", type=int, default=0, help="also run bits 0..N-1")
    args = parser.parse_args(argv)
    code = builder_code(Image.load(args.xbe), args.builder, args.length)
    keys = list(args.flags) + [1 << bit for bit in range(args.single_bits)]
    if not keys:
        print("nothing to run: pass --flags or --single-bits", file=sys.stderr)
        return 1
    for key in keys:
        print(describe(key, used_registers(run_builder(code, key))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
