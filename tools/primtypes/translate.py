# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the title's own primitive-kind translators and read the NV2A operation they pick.

The XDK library writes the primitive number it is given into `SET_BEGIN_END` (0x17FC)
UNCHANGED (`DrawVertices` 0x3D4FB0 and `DrawIndexedVertices` 0x3D5050, checked by
`library_pushes`). So the hardware operation is decided by the TITLE, in three
translators that turn a game-level kind into the number (MEASURED, T84c):

    batcher   0x18EA0  (kind, vertex_format, key, count)  kinds 1..8, 1-based, jump table 0x18FC8
    indexed   0x18FF0  (kind, key, count)                 kinds 1..8, 1-based, jump table 0x190DC
    mesh      0x1E770  eax = kind, 0-based 0..7, jump table 0x1E7A4, returns the operation

The batcher and indexed translators carry on into state-dependent flushing after the
mapping, so they are run to the instruction where the mapping is complete and the
register is read (`esi` / `edi`). The third is a plain function and is run to its `ret`.
Nothing here is Direct3D's own enumeration: the numbers are the hardware's.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

from unicorn.x86_const import (
    UC_X86_REG_EAX,
    UC_X86_REG_EBX,
    UC_X86_REG_EDI,
    UC_X86_REG_ESI,
    UC_X86_REG_ESP,
)

from tools.d3dscan.oracle import SENTINEL, STACK_TOP, Oracle

BATCHER = 0x18EA0
BATCHER_DONE = 0x18EFE
INDEXED = 0x18FF0
INDEXED_DONE = 0x19049
MESH = 0x1E770
GLOBAL_PRIMITIVE = 0x4B85D8
DRAW_VERTICES = 0x3D4FB0
DRAW_INDEXED_VERTICES = 0x3D5050
INSTRUCTION_BUDGET = 1000
SET_BEGIN_END_HEADER = 0x000417FC

#: The NV2A operations, from the nxdk and xemu register headers (two derivations that agree).
OPERATION_NAMES = {
    0: "END",
    1: "POINTS",
    2: "LINES",
    3: "LINE_LOOP",
    4: "LINE_STRIP",
    5: "TRIANGLES",
    6: "TRIANGLE_STRIP",
    7: "TRIANGLE_FAN",
    8: "QUADS",
    9: "QUAD_STRIP",
    10: "POLYGON",
}


@dataclass(frozen=True)
class Conversion:
    """What one translator produced for one input."""

    operation: int
    #: The translator marked the draw as unbatchable (`bl = 1`: strips and loops force a flush).
    flush: bool = False


def _stop_at(oracle: Oracle, entry: int, stop: int, arguments: list[int]) -> None:
    esp = STACK_TOP - 0x1000
    for value in reversed(arguments):
        esp -= 4
        oracle.write32(esp, value)
    esp -= 4
    oracle.write32(esp, SENTINEL)
    emulator = oracle.emulator
    emulator.reg_write(UC_X86_REG_ESP, esp)
    emulator.reg_write(UC_X86_REG_EBX, 0)
    emulator.emu_start(entry, stop, count=INSTRUCTION_BUDGET)


def batcher(oracle: Oracle, kind: int, count: int) -> Conversion:
    """0x18EA0: game kind (1-based) and vertex count to the operation held in `esi`."""
    _stop_at(oracle, BATCHER, BATCHER_DONE, [kind, 0, 0, count])
    return Conversion(
        operation=oracle.emulator.reg_read(UC_X86_REG_ESI),
        flush=bool(oracle.emulator.reg_read(UC_X86_REG_EBX) & 0xFF),
    )


def indexed(oracle: Oracle, kind: int, key: int) -> Conversion:
    """0x18FF0: game kind (1-based) to the operation held in `edi`. `key` is argument 2."""
    _stop_at(oracle, INDEXED, INDEXED_DONE, [kind, key, 0])
    return Conversion(
        operation=oracle.emulator.reg_read(UC_X86_REG_EDI),
        flush=bool(oracle.emulator.reg_read(UC_X86_REG_EBX) & 0xFF),
    )


def mesh(oracle: Oracle, kind: int) -> Conversion:
    """0x1E770: game kind (0-based, in eax) to the operation it returns (-1 when out of range)."""
    _stop_at_return(oracle, MESH, kind)
    return Conversion(operation=oracle.emulator.reg_read(UC_X86_REG_EAX))


def _stop_at_return(oracle: Oracle, entry: int, eax: int) -> None:
    esp = STACK_TOP - 0x1000 - 4
    oracle.write32(esp, SENTINEL)
    oracle.emulator.reg_write(UC_X86_REG_ESP, esp)
    oracle.emulator.reg_write(UC_X86_REG_EAX, eax)
    oracle.emulator.emu_start(entry, SENTINEL, count=INSTRUCTION_BUDGET)


def prepare_indexed_state(oracle: Oracle) -> int:
    """Give `DrawIndexedVertices` the guest state it reads (as tests/test_d3dscan_indexed.py does).

    Maps a scratch range, aligns the cursor, points two vertex streams at fake buffer headers
    and fills the declaration. Returns the address of 16-bit indices to pass as the third
    argument. Nothing here affects the primitive, which is what `library_pushes` reads.
    """
    from tools.d3dscan.oracle import DEVICE_BASE

    oracle.emulator.mem_map(0xA01000, 0xF000)
    start = oracle.read32(DEVICE_BASE)
    oracle.write32(DEVICE_BASE, start + ((0 - start) & 31))
    for offset in (8, 0x1C, 0x20):
        oracle.write32(DEVICE_BASE + offset, 0)
    oracle.write32(0x3E3AB8, 0)
    declaration = oracle.read32(DEVICE_BASE + 0x794)
    oracle.write32(declaration + 4, 0)
    oracle.write32(DEVICE_BASE + 0x784, 0x3E4884)
    oracle.write32(DEVICE_BASE + 0x788, 0x1C80)
    oracle.write32(DEVICE_BASE + 0x78C, 0x100)
    oracle.write32(DEVICE_BASE + 0x790, 1)
    oracle.write32(DEVICE_BASE + 0x1928, 1)
    for address in (0x3E3E30, 0x3E3E58, 0x3E3E5C):
        oracle.write32(address, 0)
    for address in range(DEVICE_BASE + 0x7AC, DEVICE_BASE + 0x7D4, 4):
        oracle.write32(address, 0)
    for slot in range(16):
        oracle.write32(declaration + slot * 16 + 0x14, slot % 2)
        oracle.write32(declaration + slot * 16 + 0x18, slot * 4)
        oracle.write32(declaration + slot * 16 + 0x1C, 2 if slot % 3 == 0 else 0x12)
    for stream in range(2):
        row = 0x3E2BA8 + stream * 12
        oracle.write32(row, 20 if stream == 0 else 12)
        oracle.write32(row + 4, stream * 24)
        oracle.write32(row + 8, 0xA10000 + stream * 12)
    oracle.write_bytes(
        0xA10000,
        struct.pack("<6I", 0x1000001, 0x1002000, 0, 0x1000001, 0x2003000, 0) + bytes(232),
    )
    oracle.write_bytes(0xA00000, bytes((i * 37 ^ i >> 8) & 255 for i in range(0x1000)))
    return 0xA00000


def library_pushes(oracle: Oracle, entry: int, arguments: list[int]) -> int:
    """Run the original library draw at `entry` and return the data of its `SET_BEGIN_END` begin.

    Both draws emit `0x000417FC, primitive` first (after any deferred state flush), so a
    measured equality with the argument means the library applies no conversion. The
    scan takes the first non-zero begin pair: the closing pair carries 0.
    """
    from tools.d3dscan.oracle import DEVICE_BASE

    start = oracle.read32(DEVICE_BASE)
    oracle.run(entry, arguments)
    end = oracle.read32(DEVICE_BASE)
    for cursor in range(start, end, 4):
        if oracle.read32(cursor) == SET_BEGIN_END_HEADER and oracle.read32(cursor + 4) != 0:
            return oracle.read32(cursor + 4)
    raise ValueError(f"0x{entry:x} wrote no SET_BEGIN_END begin between {start:#x} and {end:#x}")


def load_oracle(xbe: Path) -> Oracle:
    """The image in an emulator; `instant_gpu` so a draw's kick does not wait on the ring."""
    return Oracle(xbe, instant_gpu=True)
