# SPDX-License-Identifier: GPL-3.0-or-later
"""The retail image as the XPP tools see it: sections, bytes by virtual address, the surface.

Nothing here writes a byte of the executable anywhere. The XBE is read, decoded in memory and
summarised as counts and addresses, which is all `docs/provenance.md` lets a committed file carry.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

from tools.kernel_ordinals import KERNEL_ORDINALS
from tools.xbe.model import Xbe, XbeSection
from tools.xbe.parser import parse_xbe

DEFAULT_XBE = Path("tmp/oxm-extract/retail/default.xbe")

XPP_SECTION = "XPP"


@dataclass(frozen=True)
class SurfaceFunction:
    """One function the title can call into the XPP section, from `.XTLID`."""

    address: int
    name: str
    # The title's own site count (calls from game `.text`), as measured by the surface generator.
    sites: int


# The fifteen named code symbols. Names and addresses are `.XTLID`, the same table
# `src/input/xinput_hle.c` compiles in, and `tests/test_xppscan_static.py` cross-checks the two.
SURFACE: tuple[SurfaceFunction, ...] = (
    SurfaceFunction(0x0046DBD2, "XGetDevices", 4),
    SurfaceFunction(0x0046DBF4, "XGetDeviceChanges", 3),
    SurfaceFunction(0x0046DB91, "XPeekDevices", 2),
    SurfaceFunction(0x0046E189, "XInputClose", 2),
    SurfaceFunction(0x004754FD, "XVoiceCreateMediaObjectEx", 2),
    SurfaceFunction(0x0046D7C4, "XMountMUA", 1),
    SurfaceFunction(0x0046D8F6, "XUnmountMU", 1),
    SurfaceFunction(0x0046DBCD, "XInitDevices", 1),
    SurfaceFunction(0x0046E133, "XInputOpen", 1),
    SurfaceFunction(0x0046E195, "XInputGetCapabilities", 1),
    SurfaceFunction(0x0046E36D, "XInputGetState", 1),
    SurfaceFunction(0x0046E3E0, "XInputSetState", 1),
    SurfaceFunction(0x0046F3A8, "XGetDeviceEnumerationStatus", 1),
    SurfaceFunction(0x0046DA04, "XReadMUMetaData", 0),
    SurfaceFunction(0x004752C6, "XVoiceCreateMediaObject", 0),
)

# The device-type descriptor tables the title passes by address. Each is three guest dwords the
# library's enumeration owns (see docs/xpp-init.md), followed by library-private fields.
DATA_TABLES: dict[str, int] = {
    "XDEVICE_TYPE_MEMORY_UNIT_TABLE": 0x0046C6E0,
    "XDEVICE_TYPE_GAMEPAD_TABLE": 0x0046C75C,
    "XDEVICE_TYPE_IR_REMOTE_TABLE": 0x0046C7C0,
    "XDEVICE_TYPE_VOICE_MICROPHONE_TABLE": 0x0046C894,
    "XDEVICE_TYPE_VOICE_HEADPHONE_TABLE": 0x0046C8A0,
    "XDEVICE_TYPE_HIGHFIDELITY_MICROPHONE_TABLE": 0x0046C8AC,
}


class Image:
    """A parsed XBE and a read-only view of its bytes by guest virtual address."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.raw = path.read_bytes()
        self.xbe: Xbe = parse_xbe(self.raw)
        self.sections: dict[str, XbeSection] = {s.name: s for s in self.xbe.sections}
        self.kernel_slots: dict[int, int] = {
            self.xbe.kernel_thunk_addr + 4 * index: ordinal
            for index, ordinal in enumerate(self.xbe.kernel_import_ordinals)
        }

    @property
    def xpp(self) -> XbeSection:
        return self.sections[XPP_SECTION]

    def section_of(self, address: int) -> str | None:
        for section in self.xbe.sections:
            if section.virtual_addr <= address < section.virtual_addr + section.virtual_size:
                return section.name
        return None

    def read(self, address: int, length: int) -> bytes:
        """Bytes at a guest address. Zero-filled past a section's file bytes (BSS), short at the
        end of the mapped image."""
        for section in self.xbe.sections:
            begin = section.virtual_addr
            if begin <= address < begin + section.virtual_size:
                available = begin + section.virtual_size - address
                take = min(length, available)
                offset = address - begin
                file_part = b""
                if offset < section.raw_size:
                    start = section.raw_addr + offset
                    file_part = self.raw[start : start + min(take, section.raw_size - offset)]
                return file_part + b"\x00" * (take - len(file_part))
        return b""

    def u32(self, address: int) -> int:
        return struct.unpack("<I", self.read(address, 4))[0]

    def in_xpp(self, address: int) -> bool:
        begin = self.xpp.virtual_addr
        return begin <= address < begin + self.xpp.virtual_size

    def kernel_name(self, ordinal: int) -> str:
        return KERNEL_ORDINALS.get(ordinal, f"ordinal{ordinal}")
