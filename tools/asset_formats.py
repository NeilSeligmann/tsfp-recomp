# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded lossless retail Xbox asset envelopes; see docs/t-pak-format.md.

No extraction or file writes. Unknown fields/payloads remain opaque. This is
structural validation, not a geometry decoder or runtime relocation replacement.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path

from tools.errors import ParseError, check_range
from tools.names import pak_key
from tools.pak import parse_pak
from tools.xdvdfs.reader import XisoReader

XBR_ROOM_STRIDE = 0xC8
XBT_HEADER_SIZE = 0x80
# Direct room pointers rebased by 0x8df60 (nested graphs are outside this API).
ROOM_POINTER_FIELDS = (
    *range(0, 0x48, 4),
    0x60,
    0x64,
    0x68,
    0x6C,
    0x78,
    0x7C,
    0x80,
    0x84,
    0x88,
    0x8C,
    0xA8,
    0xAC,
    0xB0,
    0xB4,
    0xBC,
    0xC0,
    0xC4,
)


@dataclass(frozen=True)
class XbtEnvelope:
    """Nine interpreted dwords, preserved header remainder and opaque pixels."""

    words: tuple[int, ...]
    reserved: bytes
    pixels: bytes

    def encode(self) -> bytes:
        return struct.pack("<9I", *self.words) + self.reserved + self.pixels


def parse_xbt(data: bytes) -> XbtEnvelope:
    check_range(data, 0, XBT_HEADER_SIZE, "XBT header")
    words = struct.unpack_from("<9I", data)
    # Only format enum and dimensions used by 0x18390; no invented magic.
    if not all(0 < w <= 0x7FFFFFFF for w in words[:4]) or words[5] > 7:
        raise ParseError("unsupported XBT dimensions or format enum")
    return XbtEnvelope(words, data[36:XBT_HEADER_SIZE], data[XBT_HEADER_SIZE:])


@dataclass(frozen=True)
class RoomBlock:
    start: int
    header: int
    rooms: tuple[tuple[int, ...], ...]


@dataclass(frozen=True)
class XbrEnvelope:
    header_words: tuple[int, ...]
    blocks: tuple[RoomBlock, ...]
    fragments: tuple[bytes, ...]

    def encode(self) -> bytes:
        out = bytearray(struct.pack("<12I", *self.header_words))
        for fragment, block in zip(self.fragments, self.blocks, strict=False):
            out.extend(fragment)
            out.extend(b"".join(struct.pack("<50I", *room) for room in block.rooms))
        out.extend(self.fragments[-1])
        return bytes(out)

    @property
    def rooms(self) -> tuple[tuple[int, ...], ...]:
        return tuple(room for block in self.blocks for room in block.rooms)

    def relocated_rooms(self, base: int) -> tuple[tuple[int, ...], ...]:
        """Rebase direct nullable pointers; reject overflowing 32-bit addresses."""
        if not 0 <= base <= 0xFFFFFFFF:
            raise ParseError("invalid relocation base")
        out = []
        for room in self.rooms:
            words = list(room)
            for offset in ROOM_POINTER_FIELDS:
                index = offset // 4
                if words[index]:
                    if words[index] + base > 0xFFFFFFFF:
                        raise ParseError("relocation overflow")
                    words[index] += base
            out.append(tuple(words))
        return tuple(out)


def parse_xbr(data: bytes) -> XbrEnvelope:
    """Level XBR envelope, distinct from a streamed room-block envelope."""
    check_range(data, 0, 0x30, "level XBR header")
    words = struct.unpack_from("<12I", data)
    table = words[1]
    if table < 0x30:
        raise ParseError("XBR block table overlaps envelope")
    check_range(data, table, 0xB4, "XBR block table")
    blocks = []
    index = 1  # Entry zero is special; 0x8a453 starts at table+0xb4.
    while True:
        slot = table + index * 0xB4
        check_range(data, slot, 4, "XBR block table sentinel")
        header = struct.unpack_from("<I", data, slot)[0]
        if not header:
            break
        check_range(data, header, 0x48, "XBR room header")
        count = struct.unpack_from("<I", data, header)[0]
        start = header - count * XBR_ROOM_STRIDE
        if start < 0x30:
            raise ParseError("XBR rooms precede envelope")
        rooms = tuple(
            struct.unpack_from("<50I", data, start + i * XBR_ROOM_STRIDE) for i in range(count)
        )
        for room in rooms:
            for offset in ROOM_POINTER_FIELDS:
                pointer = room[offset // 4]
                if pointer:
                    check_range(data, pointer, 1, "XBR direct room pointer")
        blocks.append(RoomBlock(start, header, rooms))
        index += 1
    blocks.sort(key=lambda block: block.start)
    cursor = 0x30
    fragments = []
    for block in blocks:
        if block.start < cursor:
            raise ParseError("overlapping XBR room tables")
        fragments.append(data[cursor : block.start])
        cursor = block.header
    fragments.append(data[cursor:])
    return XbrEnvelope(words, tuple(blocks), tuple(fragments))


def rebuild_p5ck(data: bytes) -> bytes:
    """Reserialize the 12-byte header and TOC, preserving stored payload/gaps.

    Inflation is separately checked; recompressing gzip need not reproduce its
    original encoder metadata or bitstream.
    """
    check_range(data, 0, 12, "P5CK header")
    toc, size = struct.unpack_from("<II", data, 4)
    if size % 16:
        raise ParseError("unaligned P5CK TOC size")
    archive = parse_pak(data)
    if archive.magic != "P5CK":
        raise ParseError("round trip supports retail P5CK only")
    toc, size = struct.unpack_from("<II", data, 4)
    if size % 16 or (size and (toc < 12 or toc + size != len(data))):
        raise ParseError("P5CK TOC must be aligned in size and at EOF")
    for entry in archive.entries:
        if entry.on_disc_size and (
            entry.data_offset < 12 or entry.data_offset + entry.on_disc_size > toc
        ):
            raise ParseError("P5CK payload overlaps header or TOC")
        archive.entry_data(data, entry)
    header = struct.pack("<4sII", b"P5CK", toc, size)
    if not size:
        return header + data[12:]
    table = b"".join(
        struct.pack("<IIII", e.key, e.data_offset, e.uncompressed_size, e.stored_size)
        for e in archive.entries
    )
    return header + data[12:toc] + table


def _receipt(kind: str, original: bytes, rebuilt: bytes) -> dict:
    if original != rebuilt:
        raise ParseError(f"{kind} round trip differs")
    return {
        "kind": kind,
        "bytes": len(original),
        "sha256": hashlib.sha256(original).hexdigest(),
        "round_trip": True,
    }


def validate_disc(iso: Path) -> list[dict]:
    """Read two bounded archives and named entries; report metadata only."""
    receipts = []
    samples = (
        ("pak/story/l_1_st.pak", "assets/ts/models/level1/level1/xbox/output/level1.xbr", "XBR"),
        ("pak/chr.pak", "textures/cc95c8.xbt", "XBT"),
    )
    with iso.open("rb") as stream:
        reader = XisoReader(stream)
        for pak_path, asset_path, kind in samples:
            entry = next(e for e in reader.list_entries() if e.path == pak_path)
            if entry.size > 64 * 1024 * 1024:
                raise ParseError("sample archive exceeds 64 MiB bound")
            data = reader.read_file(pak_path)
            receipts.append(_receipt("PAK", data, rebuild_p5ck(data)))
            archive = parse_pak(data)
            matches = [e for e in archive.entries if e.key == pak_key(asset_path)]
            if len(matches) != 1:
                raise ParseError("sample asset key missing or ambiguous")
            payload = archive.entry_data(data, matches[0])
            parsed = parse_xbr(payload) if kind == "XBR" else parse_xbt(payload)
            receipt = _receipt(kind, payload, parsed.encode())
            if isinstance(parsed, XbrEnvelope):
                if not parsed.rooms:
                    raise ParseError("sample XBR has no room witness")
                base = 0x10000000
                relocated = parsed.relocated_rooms(base)
                restored = []
                for room in relocated:
                    words = list(room)
                    for offset in ROOM_POINTER_FIELDS:
                        index = offset // 4
                        if words[index]:
                            words[index] -= base
                    restored.append(tuple(words))
                if tuple(restored) != parsed.rooms:
                    raise ParseError("XBR direct relocation inverse differs")
                receipt["direct_relocation_inverse"] = True
                receipt["rooms"] = len(parsed.rooms)
            receipts.append(receipt)
    return receipts


def census_disc(iso: Path) -> dict:
    """Read only the 403 archive headers/TOCs, never large payloads."""
    archives = entries = compressed = tail_tables = duplicates = 0
    with iso.open("rb") as stream:
        reader = XisoReader(stream)
        files = [e for e in reader.list_entries() if not e.is_dir]
        for entry in files:
            if not entry.path.endswith(".pak"):
                continue
            base = entry.start_sector * 2048
            header = reader._read_at(base, 12)
            magic, offset, size = struct.unpack("<4sII", header)
            if magic != b"P5CK" or size % 16 or size > 0x80000:
                raise ParseError("unsupported disc PAK table")
            if offset + size > entry.size:
                raise ParseError("disc PAK table exceeds file")
            table = reader._read_at(base + offset, size)
            rows = list(struct.iter_unpack("<4I", table))
            archives += 1
            entries += len(rows)
            compressed += sum(row[3] != 0 for row in rows)
            duplicates += len(rows) - len({row[0] for row in rows})
            tail_tables += offset + size == entry.size
    return {
        "files": len(files),
        "p5ck_archives": archives,
        "entries": entries,
        "nonzero_stored_size": compressed,
        "toc_at_eof": tail_tables,
        "duplicate_key_entries": duplicates,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--census", action="store_true", help="header/TOC-only disc census")
    args = parser.parse_args()
    print(json.dumps(census_disc(args.iso) if args.census else validate_disc(args.iso), indent=2))


if __name__ == "__main__":
    main()
