# SPDX-License-Identifier: GPL-3.0-or-later
"""Facts about the XBE and the XDK surface that the walker needs.

Everything here is read from the user's own files at run time. Nothing is embedded, and
nothing that is read is written to any output except as a count, an address or a
section name. String literals are available through `Image.string_at` for local
inspection only: the CLI never prints them unless asked to, and never to a tracked path.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from tools.kernel_ordinals import KERNEL_ORDINALS
from tools.xbe.model import Xbe
from tools.xbe.parser import parse_xbe

_SURFACE_ROW = re.compile(r'\{0x([0-9a-fA-F]{8}), "([A-Z0-9_]+)", (NULL|"[^"]*"), (\d+)\}')
_ABI_ROW = re.compile(
    r"\{0x([0-9A-F]{8})u, XDK_CC_([A-Z]+), (\d+)u, (\d+)u, XDK_ABI_FROM_([A-Z_]+), (\d+)u\}"
)
_PRINTABLE = re.compile(rb"^[\x20-\x7e\t\r\n]{4,}$")

#: Name the generated table gives a row it could not name.
UNNAMED = None


@dataclass(frozen=True)
class SurfaceRow:
    address: int
    section: str
    #: The `.XTLID` name when the image carries one for this address, else None.
    name: str | None
    #: Call sites measured by the surface generator, game `.text` origins only.
    sites: int


@dataclass(frozen=True)
class AbiRow:
    address: int
    convention: str
    stack_args: int
    register_args: int
    #: `CALLEE_RET` (a `ret imm16` was read) or `CALLER_VOTES` (counted from call sites).
    evidence: str


def read_surface(path: Path) -> dict[int, SurfaceRow]:
    """Parse the generated surface table. An empty parse is an error, not an empty map."""
    text = path.read_text(encoding="utf-8", errors="replace")
    rows: dict[int, SurfaceRow] = {}
    for match in _SURFACE_ROW.finditer(text):
        address = int(match.group(1), 16)
        raw_name = match.group(3)
        rows[address] = SurfaceRow(
            address=address,
            section=match.group(2),
            name=None if raw_name == "NULL" else raw_name.strip('"'),
            sites=int(match.group(4)),
        )
    if not rows:
        raise ValueError(f"no surface rows parsed from {path}; the table shape changed")
    return rows


_ORACLE_DATA = re.compile(r"\{(\d+)u, KERNEL_ARITY_ORACLE_DATA")


def read_data_ordinals(path: Path) -> frozenset[int]:
    """Kernel exports that are data, not functions, per the arity oracle's own table."""
    text = path.read_text(encoding="utf-8", errors="replace")
    return frozenset(int(match.group(1)) for match in _ORACLE_DATA.finditer(text))


def read_abi(path: Path) -> dict[int, AbiRow]:
    """Parse the generated ABI table. Rows the measurement refused are simply absent."""
    text = path.read_text(encoding="utf-8", errors="replace")
    rows: dict[int, AbiRow] = {}
    for match in _ABI_ROW.finditer(text):
        address = int(match.group(1), 16)
        rows[address] = AbiRow(
            address=address,
            convention=match.group(2).lower(),
            stack_args=int(match.group(3)),
            register_args=int(match.group(4)),
            evidence=match.group(5),
        )
    return rows


@dataclass
class Image:
    """The XBE bytes plus the parsed header, with the lookups the walker uses."""

    xbe: Xbe
    data: bytes

    @classmethod
    def load(cls, path: Path) -> Image:
        data = path.read_bytes()
        return cls(xbe=parse_xbe(data), data=data)

    @property
    def thunk_base(self) -> int:
        return self.xbe.kernel_thunk_addr

    @property
    def thunk_slots(self) -> range:
        count = len(self.xbe.kernel_import_ordinals)
        return range(self.thunk_base, self.thunk_base + 4 * count, 4)

    def slot_ordinal(self, slot: int) -> int | None:
        """Ordinal behind a kernel thunk slot address, or None when it is not one."""
        if slot not in self.thunk_slots:
            return None
        return self.xbe.kernel_import_ordinals[(slot - self.thunk_base) // 4]

    def read_u32(self, va: int) -> int | None:
        offset = self.xbe.va_to_offset(va)
        if offset is None or offset + 4 > len(self.data):
            return None
        return int.from_bytes(self.data[offset : offset + 4], "little")

    def string_at(self, va: int, limit: int = 160) -> str | None:
        """A printable NUL-terminated string at `va`, or None. For local inspection only."""
        offset = self.xbe.va_to_offset(va)
        if offset is None:
            return None
        end = self.data.find(b"\0", offset, offset + limit)
        if end < 0:
            return None
        raw = self.data[offset:end]
        return raw.decode("ascii") if _PRINTABLE.match(raw) else None

    def section_name(self, va: int) -> str | None:
        for section in self.xbe.sections:
            if section.virtual_addr <= va < section.virtual_addr + section.virtual_size:
                return section.name
        return None


def ordinal_name(ordinal: int) -> str:
    """Kernel export name for an ordinal, or a marker when the table has none."""
    return KERNEL_ORDINALS.get(ordinal, f"ordinal_{ordinal}")
