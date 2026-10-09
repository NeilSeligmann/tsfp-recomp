# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the title's own vertex-attribute-block builder and decode the 16 slots.

The title never makes a D3DVSD token stream (`docs/d3d8-usage.md` 5.1). It hands
`0x3D5630` a 0x40-dword block of 16 slots, 16 bytes each: dword 1 is the byte offset
of the attribute inside the vertex, dword 2 is the format byte, `size << 4 | type`.
Slot `n` is hardware input register `vn`. A slot with size 0 is unused. The builder
(`0x1E970`) is straight-line code that writes only that block, so it is run in Unicorn
from the image's bytes for a given flags word. Only counts and registers are printed.
"""

from __future__ import annotations

import struct
from collections.abc import Sequence
from dataclasses import dataclass

from unicorn import UC_ARCH_X86, UC_MODE_32, Uc
from unicorn.x86_const import UC_X86_REG_ESP

from tools.shaderscan.image import Image

SLOTS = 16
BLOCK_BYTES = SLOTS * 16
PAGE = 0x1000
CODE_BASE = 0x10000000
BLOCK_BASE = 0x20000000
STACK_TOP = 0x30001000
RETURN_ADDRESS = 0x40000000
TYPE_NAMES = {0: "UB_D3D", 1: "S1", 2: "F", 4: "UB_OGL", 5: "S32K", 6: "CMP"}


@dataclass(frozen=True)
class Slot:
    register: int
    offset: int
    format: int

    @property
    def size(self) -> int:
        return self.format >> 4

    @property
    def type(self) -> int:
        return self.format & 0xF

    @property
    def used(self) -> bool:
        return self.size != 0


def decode_block(block: bytes) -> list[Slot]:
    """Decode the 16 slots of a 0x100-byte attribute block."""
    if len(block) != BLOCK_BYTES:
        raise ValueError(f"block is {len(block)} bytes, expected {BLOCK_BYTES}")
    words = struct.unpack("<64I", block)
    return [Slot(n, words[n * 4 + 1], words[n * 4 + 2]) for n in range(SLOTS)]


def run_builder(code: bytes, flags: int) -> list[Slot]:
    """Execute `code` (a cdecl `builder(block, flags)`) and decode the block it filled."""
    if not code:
        raise ValueError("empty builder code")
    emulator = Uc(UC_ARCH_X86, UC_MODE_32)
    pages = (len(code) + PAGE - 1) // PAGE * PAGE
    emulator.mem_map(CODE_BASE, pages)
    emulator.mem_write(CODE_BASE, code)
    emulator.mem_map(BLOCK_BASE, PAGE)
    emulator.mem_map(STACK_TOP - PAGE, PAGE)
    emulator.mem_map(RETURN_ADDRESS, PAGE)
    stack = STACK_TOP - 0x100
    emulator.mem_write(stack, struct.pack("<III", RETURN_ADDRESS, BLOCK_BASE, flags))
    emulator.reg_write(UC_X86_REG_ESP, stack)
    emulator.emu_start(CODE_BASE, RETURN_ADDRESS, timeout=2_000_000, count=10_000)
    return decode_block(bytes(emulator.mem_read(BLOCK_BASE, BLOCK_BYTES)))


def builder_code(image: Image, address: int, length: int) -> bytes:
    code = image.read(address, length)
    if code is None:
        raise ValueError(f"0x{address:x}+{length:#x} is not initialised image bytes")
    return code


def used_registers(slots: Sequence[Slot]) -> dict[int, int]:
    """Register number -> format byte, for slots that carry an attribute."""
    return {slot.register: slot.format for slot in slots if slot.used}
