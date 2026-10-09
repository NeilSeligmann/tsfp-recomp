# SPDX-License-Identifier: GPL-3.0-or-later
"""Parser for Xbox executables (XBE).

Layout constants were measured directly from the retail TimeSplitters: Future
Perfect XBE. See docs/superpowers/plans/2026-09-30-analysis-toolchain.md for the
format reference.
"""

from __future__ import annotations

import struct

from tools.errors import BadMagicError, check_range
from tools.xbe.model import Xbe, XbeCertificate, XbeSection, XtlidEntry

XBE_MAGIC = b"XBEH"
SECTION_HEADER_SIZE = 0x38

OFF_BASE_ADDRESS = 0x104
OFF_SIZE_OF_HEADERS = 0x108
OFF_SIZE_OF_IMAGE = 0x10C
OFF_CERTIFICATE_ADDR = 0x118
OFF_NUMBER_OF_SECTIONS = 0x11C
OFF_SECTION_HEADERS_ADDR = 0x120
OFF_ENTRY_POINT = 0x128
OFF_KERNEL_THUNK_ADDR = 0x158
HEADER_MIN_SIZE = 0x17C

ENTRY_XOR_RETAIL = 0xA8FC57AB
ENTRY_XOR_DEBUG = 0x94859D4B
THUNK_XOR_RETAIL = 0x5B6D40B6
THUNK_XOR_DEBUG = 0xEFB1F152

MAX_KERNEL_IMPORTS = 512

# Certificate field offsets, relative to the certificate start.
CERT_OFF_TIME_DATE = 0x04
CERT_OFF_TITLE_ID = 0x08
CERT_OFF_TITLE_NAME = 0x0C
CERT_TITLE_NAME_BYTES = 80
CERT_OFF_ALLOWED_MEDIA = 0x9C
CERT_OFF_GAME_REGION = 0xA0
CERT_OFF_GAME_RATINGS = 0xA4
CERT_OFF_DISK_NUMBER = 0xA8
CERT_OFF_VERSION = 0xAC
CERT_MIN_SIZE = 0xB0

XTLID_SECTION_NAME = ".XTLID"
XTLID_HEADER_SIZE = 8
XTLID_RECORD_SIZE = 8


def _u32(data: bytes, offset: int, what: str) -> int:
    check_range(data, offset, 4, what)
    return int(struct.unpack_from("<I", data, offset)[0])


def _cstring(data: bytes, offset: int, what: str, limit: int = 256) -> str:
    check_range(data, offset, 1, what)
    end = data.find(b"\0", offset, min(offset + limit, len(data)))
    if end < 0:
        end = min(offset + limit, len(data))
    return data[offset:end].decode("latin-1")


def _parse_certificate(data: bytes, offset: int) -> XbeCertificate:
    check_range(data, offset, CERT_MIN_SIZE, "certificate")
    raw_name = data[
        offset + CERT_OFF_TITLE_NAME : offset + CERT_OFF_TITLE_NAME + CERT_TITLE_NAME_BYTES
    ]
    title_name = raw_name.decode("utf-16-le", errors="replace").split("\0", 1)[0]
    return XbeCertificate(
        title_id=_u32(data, offset + CERT_OFF_TITLE_ID, "cert.title_id"),
        title_name=title_name,
        time_date=_u32(data, offset + CERT_OFF_TIME_DATE, "cert.time_date"),
        allowed_media=_u32(data, offset + CERT_OFF_ALLOWED_MEDIA, "cert.allowed_media"),
        game_region=_u32(data, offset + CERT_OFF_GAME_REGION, "cert.game_region"),
        game_ratings=_u32(data, offset + CERT_OFF_GAME_RATINGS, "cert.game_ratings"),
        disk_number=_u32(data, offset + CERT_OFF_DISK_NUMBER, "cert.disk_number"),
        version=_u32(data, offset + CERT_OFF_VERSION, "cert.version"),
    )


def _decode_xored(
    raw: int, retail_key: int, debug_key: int, base: int, size: int
) -> tuple[int, bool]:
    """Try the retail key then the debug key; accept whichever lands inside the image.

    Returns (value, is_retail). Falls back to the retail decode if neither fits, so
    callers still get a deterministic answer for a malformed image.
    """
    retail = raw ^ retail_key
    if base <= retail < base + size:
        return retail, True
    debug = raw ^ debug_key
    if base <= debug < base + size:
        return debug, False
    return retail, True


def parse_xtlid(data: bytes) -> tuple[int, list[XtlidEntry]]:
    """Decode a .XTLID section body.

    Layout: {u32 zero, u32 xdk_build} then {u16 func_id, u16 lib_id, u32 address}
    records to the end of the section. A trailing partial record is ignored.
    """
    check_range(data, 0, XTLID_HEADER_SIZE, "xtlid header")
    _, xdk_build = struct.unpack_from("<II", data, 0)
    entries: list[XtlidEntry] = []
    offset = XTLID_HEADER_SIZE
    while offset + XTLID_RECORD_SIZE <= len(data):
        func_id, lib_id, address = struct.unpack_from("<HHI", data, offset)
        entries.append(XtlidEntry(func_id=func_id, lib_id=lib_id, address=address))
        offset += XTLID_RECORD_SIZE
    return int(xdk_build), entries


def _va_to_offset(sections: list[XbeSection], va: int) -> int | None:
    for section in sections:
        if section.virtual_addr <= va < section.virtual_addr + section.virtual_size:
            delta = va - section.virtual_addr
            if delta < section.raw_size:
                return section.raw_addr + delta
            return None
    return None


def _read_kernel_ordinals(data: bytes, sections: list[XbeSection], thunk_va: int) -> list[int]:
    """Read the NUL-terminated kernel thunk table. Each entry is ordinal | 0x80000000."""
    offset = _va_to_offset(sections, thunk_va)
    if offset is None:
        return []
    ordinals: list[int] = []
    while len(ordinals) < MAX_KERNEL_IMPORTS:
        if offset + 4 > len(data):
            break
        (value,) = struct.unpack_from("<I", data, offset)
        if value == 0:
            break
        ordinals.append(value & 0xFFFF)
        offset += 4
    return ordinals


def parse_xbe(data: bytes) -> Xbe:
    """Parse an XBE image from its raw bytes."""
    check_range(data, 0, HEADER_MIN_SIZE, "xbe header")
    if data[0:4] != XBE_MAGIC:
        raise BadMagicError(f"expected XBE magic {XBE_MAGIC!r}, found {data[0:4]!r}")

    base = _u32(data, OFF_BASE_ADDRESS, "base_address")
    size_of_headers = _u32(data, OFF_SIZE_OF_HEADERS, "size_of_headers")
    size_of_image = _u32(data, OFF_SIZE_OF_IMAGE, "size_of_image")
    n_sections = _u32(data, OFF_NUMBER_OF_SECTIONS, "number_of_sections")
    sec_hdr_va = _u32(data, OFF_SECTION_HEADERS_ADDR, "section_headers_addr")
    cert_va = _u32(data, OFF_CERTIFICATE_ADDR, "certificate_addr")

    # Header-resident structures are addressed virtually but live at
    # (virtual address - base address) within the header region.
    sec_hdr_off = sec_hdr_va - base
    check_range(data, sec_hdr_off, SECTION_HEADER_SIZE * n_sections, "section headers")

    sections: list[XbeSection] = []
    for i in range(n_sections):
        off = sec_hdr_off + SECTION_HEADER_SIZE * i
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<6I", data, off)
        name = _cstring(data, name_va - base, f"section[{i}].name") if name_va else ""
        sections.append(
            XbeSection(
                name=name,
                flags=flags,
                virtual_addr=va,
                virtual_size=vsize,
                raw_addr=raw,
                raw_size=rsize,
            )
        )

    raw_entry = _u32(data, OFF_ENTRY_POINT, "entry_point")
    raw_thunk = _u32(data, OFF_KERNEL_THUNK_ADDR, "kernel_thunk_addr")
    entry_point, entry_retail = _decode_xored(
        raw_entry, ENTRY_XOR_RETAIL, ENTRY_XOR_DEBUG, base, size_of_image
    )
    thunk_addr, _ = _decode_xored(raw_thunk, THUNK_XOR_RETAIL, THUNK_XOR_DEBUG, base, size_of_image)

    ordinals = _read_kernel_ordinals(data, sections, thunk_addr)

    xdk_build: int | None = None
    xtlid: list[XtlidEntry] = []
    for section in sections:
        if section.name != XTLID_SECTION_NAME:
            continue
        start, end = section.raw_addr, section.raw_addr + section.raw_size
        if end <= len(data):
            xdk_build, xtlid = parse_xtlid(data[start:end])
        break

    return Xbe(
        base_address=base,
        size_of_image=size_of_image,
        size_of_headers=size_of_headers,
        sections=sections,
        certificate=_parse_certificate(data, cert_va - base),
        entry_point=entry_point,
        kernel_thunk_addr=thunk_addr,
        is_retail=entry_retail,
        kernel_import_ordinals=ordinals,
        xdk_build=xdk_build,
        xtlid=xtlid,
    )
