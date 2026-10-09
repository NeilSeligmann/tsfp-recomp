# SPDX-License-Identifier: GPL-3.0-or-later
"""Parser for Free Radical PAK containers.

Three magics, and the layout is NOT determined by the magic alone.

  P5CK  16-byte entries. Key is zlib.crc32(path) with case preserved.
  P4CK  TWO layouts under one magic, measured on real discs:
          TS2 / PS2        60-byte entries, plaintext char[48] name inline
          TSFP / GameCube  16-byte entries + a trailing string table
  P8CK  TS2 Xbox/GameCube. Reported to differ again; untested here.

Because the magic is ambiguous, the entry stride is detected by probing: a
candidate stride must divide toc_size exactly AND every entry it yields must
validate. A "count the plausible-looking names" heuristic is not sufficient — it
scores false positives on the 60-byte layout by reading string-table bytes as
struct fields.

The fourth entry field is the ON-DISC length, not reserved. Zero means stored;
non-zero means gzip, and you read that many bytes and inflate.
"""

from __future__ import annotations

import struct

from tools.errors import BadMagicError, check_range
from tools.pak.model import PakArchive, PakEntry

MAGIC_HASHED = b"P5CK"
MAGIC_PLAINTEXT = b"P4CK"
MAGIC_NAMETABLE = b"P8CK"
KNOWN_MAGICS = (MAGIC_HASHED, MAGIC_PLAINTEXT, MAGIC_NAMETABLE)

HEADER_SIZE = 16
#: magic + toc_offset + toc_size, without the P4CK/P8CK string-table size.
SHORT_HEADER_SIZE = 12
STRIDE_SHORT = 16
STRIDE_INLINE_NAME = 60
INLINE_NAME_BYTES = 48
MAX_NAME_LENGTH = 256


def _is_printable(raw: bytes) -> bool:
    return bool(raw) and all(32 <= byte < 127 for byte in raw)


def _read_strtab_name(data: bytes, toc_offset: int, key: int) -> str | None:
    """Read a NUL-terminated name at toc_offset + key, or None if implausible."""
    start = toc_offset + key
    if start < 0 or start >= len(data):
        return None
    end = data.find(b"\0", start, min(start + MAX_NAME_LENGTH, len(data)))
    if end < 0:
        return None
    raw = data[start:end]
    return raw.decode("ascii") if _is_printable(raw) else None


def _parse_inline_name_entries(data: bytes, toc_offset: int, toc_size: int) -> list[PakEntry]:
    """Parse 60-byte entries whose name is an inline NUL-padded char[48]."""
    entries: list[PakEntry] = []
    for offset in range(0, toc_size, STRIDE_INLINE_NAME):
        base = toc_offset + offset
        raw_name = data[base : base + INLINE_NAME_BYTES].split(b"\0", 1)[0]
        if not _is_printable(raw_name):
            return []
        data_offset, uncompressed, stored = struct.unpack_from(
            "<III", data, base + INLINE_NAME_BYTES
        )
        entries.append(
            PakEntry(
                key=offset // STRIDE_INLINE_NAME,
                data_offset=data_offset,
                uncompressed_size=uncompressed,
                stored_size=stored,
                name=raw_name.decode("ascii"),
            )
        )
    return entries


def _parse_short_entries(
    data: bytes, toc_offset: int, toc_size: int, *, resolve_names: bool
) -> list[PakEntry]:
    """Parse 16-byte entries. Keys are CRC32 hashes, or string-table offsets."""
    entries: list[PakEntry] = []
    for offset in range(0, toc_size, STRIDE_SHORT):
        key, data_offset, uncompressed, stored = struct.unpack_from(
            "<IIII", data, toc_offset + offset
        )
        name = _read_strtab_name(data, toc_offset, key) if resolve_names else None
        if resolve_names and name is None:
            return []
        entries.append(
            PakEntry(
                key=key,
                data_offset=data_offset,
                uncompressed_size=uncompressed,
                stored_size=stored,
                name=name,
            )
        )
    return entries


def parse_pak(data: bytes) -> PakArchive:
    """Parse a PAK container, detecting its entry layout by probing."""
    # The fourth field is a string-table size, which only P4CK/P8CK need: measured
    # 0 in all 772 P5CK archives across PS2, GameCube and Xbox. An empty P5CK
    # archive therefore omits it and is only 12 bytes long -- seven ship on the
    # GameCube disc (l_2_vst.pak and friends). Those are legitimately empty, not
    # corrupt, so require only the fields a given magic actually uses.
    check_range(data, 0, SHORT_HEADER_SIZE, "pak header")
    magic = data[0:4]
    if magic not in KNOWN_MAGICS:
        raise BadMagicError(
            f"expected one of {[m.decode() for m in KNOWN_MAGICS]}, found {magic!r}"
        )
    if magic != MAGIC_HASHED:
        check_range(data, 0, HEADER_SIZE, "pak header")

    toc_offset, toc_size = struct.unpack_from("<II", data, 4)
    # An empty archive still declares where its TOC would have been, so the offset
    # can point past the end of the file: the seven empty GameCube archives say
    # "TOC at 16, zero entries" inside 12 bytes. Reading nothing from anywhere is
    # vacuously in range, so only bounds-check a TOC that has entries.
    if toc_size:
        check_range(data, toc_offset, toc_size, "pak toc")

    if magic == MAGIC_HASHED:
        return PakArchive(
            magic=magic.decode("ascii"),
            entries=_parse_short_entries(data, toc_offset, toc_size, resolve_names=False),
            entry_stride=STRIDE_SHORT,
        )

    # P4CK and P8CK are ambiguous: probe each stride that divides toc_size exactly
    # and require every entry to validate.
    for stride, parse in (
        (STRIDE_INLINE_NAME, lambda: _parse_inline_name_entries(data, toc_offset, toc_size)),
        (
            STRIDE_SHORT,
            lambda: _parse_short_entries(data, toc_offset, toc_size, resolve_names=True),
        ),
    ):
        if toc_size % stride:
            continue
        entries = parse()
        if entries and len(entries) == toc_size // stride:
            return PakArchive(magic=magic.decode("ascii"), entries=entries, entry_stride=stride)

    # Neither layout validated. Fall back to the stride-16 shape without names so
    # the caller still gets offsets rather than an exception.
    return PakArchive(
        magic=magic.decode("ascii"),
        entries=_parse_short_entries(data, toc_offset, toc_size, resolve_names=False),
        entry_stride=STRIDE_SHORT,
    )
