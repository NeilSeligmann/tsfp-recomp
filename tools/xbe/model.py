# SPDX-License-Identifier: GPL-3.0-or-later
"""Data types describing an Xbox executable. Data only; parsing lives in parser.py."""

from __future__ import annotations

from dataclasses import dataclass, field

SECTION_FLAG_WRITABLE = 0x01
SECTION_FLAG_PRELOAD = 0x02
SECTION_FLAG_EXECUTABLE = 0x04
SECTION_FLAG_INSERTED_FILE = 0x08


@dataclass(frozen=True)
class XbeSection:
    name: str
    flags: int
    virtual_addr: int
    virtual_size: int
    raw_addr: int
    raw_size: int

    @property
    def writable(self) -> bool:
        return bool(self.flags & SECTION_FLAG_WRITABLE)

    @property
    def preload(self) -> bool:
        return bool(self.flags & SECTION_FLAG_PRELOAD)

    @property
    def executable(self) -> bool:
        return bool(self.flags & SECTION_FLAG_EXECUTABLE)

    @property
    def inserted_file(self) -> bool:
        return bool(self.flags & SECTION_FLAG_INSERTED_FILE)


@dataclass(frozen=True)
class XbeCertificate:
    title_id: int
    title_name: str
    time_date: int
    allowed_media: int
    game_region: int
    game_ratings: int
    disk_number: int
    version: int


@dataclass(frozen=True)
class XtlidEntry:
    """One (XDK library function id, address) pair from the .XTLID section."""

    func_id: int
    lib_id: int
    address: int


@dataclass(frozen=True)
class Xbe:
    base_address: int
    size_of_image: int
    size_of_headers: int
    sections: list[XbeSection]
    certificate: XbeCertificate
    entry_point: int = 0
    kernel_thunk_addr: int = 0
    is_retail: bool = True
    kernel_import_ordinals: list[int] = field(default_factory=list)
    xdk_build: int | None = None
    xtlid: list[XtlidEntry] = field(default_factory=list)

    def section_by_name(self, name: str) -> XbeSection | None:
        for section in self.sections:
            if section.name == name:
                return section
        return None

    def va_to_offset(self, va: int) -> int | None:
        """Map a virtual address to a file offset, or None if it is in no section."""
        for section in self.sections:
            if section.virtual_addr <= va < section.virtual_addr + section.virtual_size:
                delta = va - section.virtual_addr
                if delta < section.raw_size:
                    return section.raw_addr + delta
                return None
        return None
