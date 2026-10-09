# SPDX-License-Identifier: GPL-3.0-or-later
"""Section-aware access to an XBE image by virtual address.

The one distinction this package needs that `tools.xbe.Xbe.va_to_offset` already
encodes but does not name: a virtual address inside a section's `virtual_size` but past
its `raw_size` is BSS. It holds zeros at load and whatever the program writes after,
so a pointer into it is a pointer to a RUNTIME buffer, never to static data. The
title's shader-source buffer at `0x5608d0` is exactly that.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

from tools.xbe import Xbe, XbeSection, parse_xbe

#: A pointer-looking immediate must land inside the image. Below the base address
#: nothing is mapped, so a small immediate is a count or a flag, not a pointer.
POINTER_FLOOR = 0x10000


@dataclass(frozen=True)
class Location:
    """Where a virtual address falls."""

    section: str
    #: False when the address is past the section's initialised bytes (BSS).
    initialised: bool
    writable: bool

    def label(self) -> str:
        return f"{self.section}:{'init' if self.initialised else 'bss'}"


class Image:
    """An XBE plus its raw bytes, addressable by virtual address."""

    def __init__(self, raw: bytes, xbe: Xbe) -> None:
        self.raw = raw
        self.xbe = xbe

    @classmethod
    def load(cls, path: Path) -> Image:
        raw = path.read_bytes()
        return cls(raw, parse_xbe(raw))

    def section_at(self, va: int) -> XbeSection | None:
        for section in self.xbe.sections:
            if section.virtual_addr <= va < section.virtual_addr + section.virtual_size:
                return section
        return None

    def locate(self, va: int) -> Location | None:
        section = self.section_at(va)
        if section is None:
            return None
        return Location(
            section=section.name,
            initialised=va - section.virtual_addr < section.raw_size,
            writable=section.writable,
        )

    def read(self, va: int, length: int) -> bytes | None:
        """`length` initialised bytes at `va`, or None if any of them is BSS or unmapped."""
        section = self.section_at(va)
        if section is None:
            return None
        delta = va - section.virtual_addr
        if delta + length > section.raw_size:
            return None
        start = section.raw_addr + delta
        if start + length > len(self.raw):
            return None
        return self.raw[start : start + length]

    def u32(self, va: int) -> int | None:
        data = self.read(va, 4)
        return None if data is None else int(struct.unpack("<I", data)[0])

    def cstring_length(self, va: int, limit: int = 4096) -> int | None:
        """Length of the NUL-terminated string at `va`, or None if it runs off the data."""
        section = self.section_at(va)
        if section is None:
            return None
        delta = va - section.virtual_addr
        start = section.raw_addr + delta
        end = min(section.raw_addr + section.raw_size, start + limit, len(self.raw))
        stop = self.raw.find(b"\0", start, end)
        return None if stop < 0 else stop - start

    def looks_like_pointer(self, value: int) -> bool:
        return value >= POINTER_FLOOR and self.section_at(value) is not None
