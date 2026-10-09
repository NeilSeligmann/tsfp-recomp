# SPDX-License-Identifier: GPL-3.0-or-later
"""Opcode and operand evidence read straight from the title's own assembler.

The title's `XGAssembleShader` turns Direct3D 8 `vs.1.1` tokens into NV2A microcode. A
source mnemonic whose meaning Direct3D documents, paired with the hardware opcode the
assembler emits for it, tells what that opcode does, from the BINARY and not from a
reference table. This module drives the emulated assembler (`tools/shaderscan/assemble.py`)
over a fixed set of experiments and reports the pairs, and it counts the opcodes the
title's programs actually use.

Only mnemonic pairs, opcode numbers and counts leave this module: those are hardware
facts and our own analysis, never program bytes (`tools/ci/check-no-disc-data.sh`).

Every experiment initialises each register component it reads and feeds its result to an
output, because the assembler rejects reads of uninitialised components and drops dead
code (both measured).
"""

from __future__ import annotations

import argparse
import json
from collections import Counter
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path

from tools.nv2a import d3dtokens, isa

#: Flags the title passes with its vertex token streams (measured at the call site).
VERTEX_FLAGS = 0x120

#: name -> source lines. Registers are chosen so that each operand file differs
#: (v input, c constant, r temporary), which exposes which hardware slot a source lands in.
EXPERIMENTS: dict[str, list[str]] = {
    "mov": ["mov r0, v0", "mov oPos, r0"],
    "mov_to_output": ["mov oPos, v0"],
    "add": ["mov r1, v1", "add r0, v0, r1", "mov oPos, r0"],
    "sub": ["mov r1, v1", "sub r0, v0, r1", "mov oPos, r0"],
    "mul": ["mov r1, v1", "mul r0, v0, r1", "mov oPos, r0"],
    "mad": ["mov r1, v1", "mad r0, v0, c1, r1", "mov oPos, r0"],
    "dp3": ["dp3 r0, v0, c1", "mov oPos, r0"],
    "dp4": ["dp4 r0, v0, c1", "mov oPos, r0"],
    "min": ["min r0, v0, c1", "mov oPos, r0"],
    "max": ["max r0, v0, c1", "mov oPos, r0"],
    "slt": ["slt r0, v0, c1", "mov oPos, r0"],
    "sge": ["sge r0, v0, c1", "mov oPos, r0"],
    "dst": ["dst r0, v0, c1", "mov oPos, r0"],
    "rcp": ["rcp r0, v0.w", "mov oPos, r0"],
    "rsq": ["rsq r0, v0.w", "mov oPos, r0"],
    "exp": ["exp r0, v0.w", "mov oPos, r0"],
    "log": ["log r0, v0.w", "mov oPos, r0"],
    "expp": ["expp r0, v0.w", "mov oPos, r0"],
    "logp": ["logp r0, v0.w", "mov oPos, r0"],
    "lit": ["lit r0, v0", "mov oPos, r0"],
    "frc": ["mov r0, v0", "frc r0.xy, r0", "mov oPos, r0"],
    "m4x4": ["m4x4 r0, v0, c0", "mov oPos, r0"],
    "m4x3": ["m4x3 r0, v0, c0", "mov oPos, r0"],
    "m3x4": ["m3x4 r0, v0, c0", "mov oPos, r0"],
    "m3x3": ["m3x3 r0, v0, c0", "mov oPos, r0"],
    "m3x2": ["m3x2 r0, v0, c0", "mov oPos, r0"],
    "address_load": ["mov a0.x, v0.x", "mov r0, c[a0.x+3]", "mov oPos, r0"],
    "output_registers": [
        "mov oD0.xyz, v0",
        "mov oD1, v1",
        "mov oPos, v2",
        "mov oFog.x, v3.x",
        "mov oPts.x, v3.y",
        "mov oT0, v4",
        "mov oT3.xy, v5",
    ],
    "lone_dot_to_output": [
        "dp4 oPos.x, v0, c0",
        "dp4 oPos.y, v0, c1",
        "dp4 oPos.z, v0, c2",
        "dp4 oPos.w, v0, c3",
    ],
}


@dataclass(frozen=True)
class Emitted:
    """What one source program turned into: the operations that are not NOP-only."""

    experiment: str
    instructions: tuple[isa.Decoded, ...]

    @property
    def operations(self) -> tuple[tuple[str, str], ...]:
        return tuple(
            (d.mac_name, d.ilu_name) for d in self.instructions if d.mac != 0 or d.ilu != 0
        )


def output_writes(emitted: Emitted) -> tuple[tuple[int, int], ...]:
    """`(output address, write mask)` of each instruction that writes an output register."""
    return tuple(
        (d.out_address, d.out_mask) for d in emitted.instructions if d.out_mask and d.out_to_output
    )


def assemble_lines(
    assemble: Callable[[bytes, int], object], lines: Iterable[str]
) -> tuple[isa.Decoded, ...] | None:
    """Assemble `vs.1.1` lines with `assemble(source, flags)` (the emulator's method).

    None when the assembler rejected the source or returned nothing.
    """
    result = assemble(d3dtokens.program(list(lines)), VERTEX_FLAGS)
    if result is None or getattr(result, "kernel_call", None) is not None:
        return None
    data = getattr(result, "data", b"")
    if getattr(result, "status", 1) != 0 or not data:
        return None
    return tuple(isa.decode_program(data))


def run_experiments(assemble: Callable[[bytes, int], object]) -> dict[str, Emitted | None]:
    return {
        name: (
            Emitted(name, instructions)
            if (instructions := assemble_lines(assemble, lines)) is not None
            else None
        )
        for name, lines in EXPERIMENTS.items()
    }


def opcode_sources(emitted: Iterable[Emitted | None]) -> dict[str, set[str]]:
    """Hardware operation name (`MAC:DP4`, `ILU:RCP`) -> source experiments that emit it."""
    table: dict[str, set[str]] = {}
    for item in emitted:
        if item is None:
            continue
        for mac, ilu in item.operations:
            if mac != "NOP":
                table.setdefault(f"MAC:{mac}", set()).add(item.experiment)
            if ilu != "NOP":
                table.setdefault(f"ILU:{ilu}", set()).add(item.experiment)
    return table


def opcode_histogram(programs: Iterable[bytes]) -> Counter[str]:
    """Occurrences of every `MAC:` and `ILU:` operation over a set of programs, counting
    instructions that contain no operation as `NOP-ONLY`."""
    histogram: Counter[str] = Counter()
    for data in programs:
        for decoded in isa.decode_program(data):
            if decoded.mac == 0 and decoded.ilu == 0:
                histogram["NOP-ONLY"] += 1
            if decoded.mac:
                histogram[f"MAC:{decoded.mac_name}"] += 1
            if decoded.ilu:
                histogram[f"ILU:{decoded.ilu_name}"] += 1
    return histogram


def main(argv: list[str] | None = None) -> int:
    from tools.shaderscan.assemble import AssemblerEmulator, AssemblerSpec
    from tools.shaderscan.image import Image

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbe", type=Path)
    parser.add_argument("--assembler", type=lambda t: int(t, 0), default=0x3EE2B3)
    parser.add_argument("--heap", default="0x383678:0x383df3", help="ALLOC:FREE of this build")
    parser.add_argument("--json", type=Path, help="also write the table here")
    args = parser.parse_args(argv)
    alloc, free = (int(part, 0) for part in args.heap.split(":"))
    emulator = AssemblerEmulator(Image.load(args.xbe), AssemblerSpec(args.assembler, alloc, free))
    results = run_experiments(emulator.assemble)
    for name, item in results.items():
        shown = "REJECTED" if item is None else " ; ".join(f"{m}/{i}" for m, i in item.operations)
        print(f"{name:20} {shown}")
    table = opcode_sources(results.values())
    print()
    for operation, sources in sorted(table.items()):
        print(f"{operation:10} emitted for {sorted(sources)}")
    if args.json:
        args.json.write_text(
            json.dumps({k: sorted(v) for k, v in sorted(table.items())}, indent=1) + "\n"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
