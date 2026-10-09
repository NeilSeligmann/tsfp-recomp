# SPDX-License-Identifier: GPL-3.0-or-later
"""The retail image as bytes, sections and a decode that cannot silently stop short.

capstone's `disasm` stops at the first byte it cannot decode and says nothing, so a naive loop
reports a function shorter than it is. `decode` here steps one byte past a failure, counts it, and
the caller can require decoded plus skipped bytes to equal the range (`Decoded.complete`).

Nothing here writes a string or byte run read from the image to any output the repository keeps.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

import capstone
from capstone import x86

from tools.kernel_ordinals import KERNEL_ORDINALS
from tools.xbe import XbeSection
from tools.xbe.parser import parse_xbe

DSOUND_SECTION = "DSOUND"


@dataclass(frozen=True)
class Decoded:
    """One linear decode of a byte range."""

    instructions: tuple[capstone.CsInsn, ...]
    skipped: int
    length: int

    @property
    def complete(self) -> bool:
        """True when every byte of the range was either decoded or counted as skipped."""
        return sum(item.size for item in self.instructions) + self.skipped == self.length


class Image:
    """The XBE parsed once, with virtual-address reads and a verified decoder."""

    def __init__(self, path: Path) -> None:
        self.raw = path.read_bytes()
        self.xbe = parse_xbe(self.raw)
        self.sections: tuple[XbeSection, ...] = tuple(self.xbe.sections)
        self._decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self._decoder.detail = True

    def section(self, name: str) -> XbeSection:
        for section in self.sections:
            if section.name == name:
                return section
        raise KeyError(name)

    def section_at(self, address: int) -> XbeSection | None:
        for section in self.sections:
            if section.virtual_addr <= address < section.virtual_addr + section.virtual_size:
                return section
        return None

    def offset(self, address: int) -> int | None:
        section = self.section_at(address)
        if section is None or address - section.virtual_addr >= section.raw_size:
            return None
        return section.raw_addr + address - section.virtual_addr

    def read(self, address: int, length: int) -> bytes:
        """Bytes at a virtual address, zero-filled past a section's raw size (its BSS tail)."""
        out = bytearray()
        for index in range(length):
            where = self.offset(address + index)
            out.append(0 if where is None else self.raw[where])
        return bytes(out)

    def read_u32(self, address: int) -> int:
        return int(struct.unpack("<I", self.read(address, 4))[0])

    @property
    def dsound_range(self) -> tuple[int, int]:
        """[low, high) of the DSOUND section, BSS tail included."""
        section = self.section(DSOUND_SECTION)
        return section.virtual_addr, section.virtual_addr + section.virtual_size

    def kernel_slot(self, address: int) -> tuple[int, str] | None:
        """(ordinal, name) when `address` is a kernel import thunk slot."""
        base = self.xbe.kernel_thunk_addr
        ordinals = self.xbe.kernel_import_ordinals
        if base <= address < base + 4 * len(ordinals) and (address - base) % 4 == 0:
            ordinal = ordinals[(address - base) // 4]
            return ordinal, KERNEL_ORDINALS.get(ordinal, "?")
        return None

    def decode(self, low: int, high: int) -> Decoded:
        """Linear decode of [low, high) with resynchronisation and an exact byte account."""
        data = self.read(low, high - low)
        instructions: list[capstone.CsInsn] = []
        skipped = 0
        position = 0
        while position < len(data):
            progressed = False
            for item in self._decoder.disasm(data[position:], low + position):
                instructions.append(item)
                position += item.size
                progressed = True
            if position < len(data) and not progressed:
                position += 1
                skipped += 1
        return Decoded(tuple(instructions), skipped, len(data))

    def function_extent(self, start: int, reach: int = 0x4000) -> dict[int, capstone.CsInsn]:
        """Instructions reachable from `start` by fall-through and in-range jumps, keyed by address.

        A function's extent as control flow sees it: stops at `ret`, follows conditional and
        unconditional jumps that land within `reach` bytes, and does not follow calls.
        """
        seen: dict[int, capstone.CsInsn] = {}
        work = [start]
        while work:
            address = work.pop()
            while address not in seen:
                found = list(self._decoder.disasm(self.read(address, 16), address, 1))
                if not found:
                    break
                item = found[0]
                seen[address] = item
                if item.mnemonic.startswith("ret"):
                    break
                if item.mnemonic.startswith("j") and item.operands[0].type == x86.X86_OP_IMM:
                    target = item.operands[0].imm & 0xFFFFFFFF
                    if abs(target - start) < reach and target not in seen:
                        work.append(target)
                if item.mnemonic == "jmp":
                    break
                address += item.size
        return seen
