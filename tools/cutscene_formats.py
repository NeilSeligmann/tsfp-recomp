# SPDX-License-Identifier: GPL-3.0-or-later
"""Measured csinfo token grammar and ANRS cutscene animation envelope.

The CLI reads owner artifacts without extraction and emits only metadata/hashes.
See docs/t-csinfo-igcs-format.md for opaque regions and static-only limits.
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

ARITY = {"STREAM": 2, "FRAMES": 1, "CAMERA": 1, "CHR": 2, "PROP": 1, "ANIM": 3}
# Semantic names are INFERRED from original handler effects, not live traces.
STREAM_STATES = {
    1: "released",
    2: "reserve_async",
    4: "open_and_submit_read",
    8: "poll_register_and_advance",
    16: "loaded_idle",
    32: "load_synchronously",
}


@dataclass(frozen=True)
class Csinfo:
    records: tuple[tuple[str, tuple[str, ...]], ...]
    original: bytes

    def encode(self) -> bytes:
        """Canonical token serialization; whitespace spelling is not semantic."""
        return ("\n".join(" ".join((key, *args)) for key, args in self.records) + "\n").encode(
            "ascii"
        )


def parse_csinfo(data: bytes) -> Csinfo:
    # The guest uses sscanf %s; it has no quoted-string or comment grammar.
    body, _, tail = data.partition(b"\0")
    if tail.strip(b"\0"):
        raise ParseError("csinfo has nonzero bytes after terminator")
    try:
        tokens = body.decode("ascii").split()
    except UnicodeDecodeError as exc:
        raise ParseError("csinfo is not ASCII") from exc
    records = []
    cursor = 0
    while cursor < len(tokens):
        key = tokens[cursor]
        if key not in ARITY:
            raise ParseError("unsupported csinfo directive")
        count = ARITY[key]
        args = tuple(tokens[cursor + 1 : cursor + count + 1])
        if len(args) != count or any(arg in ARITY for arg in args):
            raise ParseError("truncated csinfo directive")
        if key == "FRAMES":
            try:
                frames = int(args[0], 10)
            except ValueError as exc:
                raise ParseError("invalid frame count") from exc
            if not 0 < frames <= 0x7FFFFFFF:
                raise ParseError("invalid frame count")
        records.append((key, args))
        cursor += count + 1
    if not records:
        raise ParseError("empty csinfo")
    return Csinfo(tuple(records), data)


@dataclass(frozen=True)
class AnimationEnvelope:
    header: tuple[int, ...]
    node_headers: tuple[tuple[int, tuple[int, ...]], ...]
    animation_headers: tuple[tuple[int, tuple[int, ...]], ...]
    original: bytes

    def encode(self) -> bytes:
        output = bytearray(self.original)
        struct.pack_into("<10I", output, 0, *self.header)
        for offset, words in self.node_headers + self.animation_headers:
            struct.pack_into(f"<{len(words)}I", output, offset, *words)
        return bytes(output)


def parse_animation_header(data: bytes) -> AnimationEnvelope:
    check_range(data, 0, 40, "ANRS header")
    header = struct.unpack_from("<10I", data)
    if data[:4] != b"ANRS" or not header[3] or not header[4] or not header[5] or not header[6]:
        raise ParseError("invalid ANRS header")
    for offset in (header[1], *header[7:]):
        check_range(data, offset, 1, "ANRS relative table")
    check_range(data, header[7], header[6] * 8, "ANRS block ranges")
    check_range(data, header[8], header[4] * 4, "ANRS per-frame table")
    cursor = 40
    nodes = []
    for _ in range(header[3]):
        check_range(data, cursor, 72, "ANRS node descriptor")
        words = struct.unpack_from("<18I", data, cursor)
        nodes.append((cursor, words))
        check_range(data, cursor + 72, words[5] * 8, "ANRS node links")
        cursor += 72 + words[5] * 8
    animations = []
    for index in range(header[3]):
        cursor = (cursor + 7) & ~7
        check_range(data, cursor, 56, "ANRS animation descriptor")
        words = struct.unpack_from("<14I", data, cursor)
        animations.append((cursor, words))
        if words[8]:
            check_range(data, words[8], 1, "ANRS animation relative pointer")
        cursor += 56
        if index:
            check_range(data, cursor, words[9] * 32 + words[4] * 4, "ANRS animation inline tables")
            cursor += words[9] * 32 + words[4] * 4
    return AnimationEnvelope(header, tuple(nodes), tuple(animations), data)


def validate_disc(iso: Path) -> dict:
    sample = "pak/igcs/igcs1/igcs_01.pak"
    root = "assets/ts/cutscenes/igcs_01/xbox/output/"
    with iso.open("rb") as stream:
        reader = XisoReader(stream)
        entry = next(e for e in reader.list_entries() if e.path == sample)
        if entry.size > 64 * 1024 * 1024:
            raise ParseError("sample exceeds read bound")
        archive_bytes = reader.read_file(sample)
        archive = parse_pak(archive_bytes)

        def lookup(path: str) -> bytes:
            matches = [e for e in archive.entries if e.key == pak_key(path)]
            if len(matches) != 1:
                raise ParseError("sample key absent or ambiguous")
            return archive.entry_data(archive_bytes, matches[0])

        cs_bytes = lookup(root + "csinfo.txt")
        cs = parse_csinfo(cs_bytes)
        if parse_csinfo(cs.encode()).records != cs.records:
            raise ParseError("csinfo semantic round trip differs")
        streams = [args for key, args in cs.records if key == "STREAM"]
        if len(streams) != 1:
            raise ParseError("sample requires one STREAM witness")
        anim_bytes = lookup(root + streams[0][1])
        anim = parse_animation_header(anim_bytes)
        if anim.encode() != anim_bytes:
            raise ParseError("ANRS envelope round trip differs")
        companion_path = "pak/igcs/igcs1/igcs_01s.pak"
        companion_entry = next(e for e in reader.list_entries() if e.path == companion_path)
        if companion_entry.size > 64 * 1024 * 1024:
            raise ParseError("companion exceeds read bound")
        companion_bytes = reader.read_file(companion_path)
        companion = parse_pak(companion_bytes)
        raw_matches = [e for e in companion.entries if e.key == pak_key(root + streams[0][0])]
        if len(raw_matches) != 1:
            raise ParseError("companion stream key absent or ambiguous")
        raw = companion.entry_data(companion_bytes, raw_matches[0])
        if len(raw) != anim.header[5] * anim.header[6]:
            raise ParseError("companion extent differs from block allocation")
        frames = [int(args[0]) for key, args in cs.records if key == "FRAMES"]
        if frames != [anim.header[4]]:
            raise ParseError("csinfo/ANRS frame counts differ")
        return {
            "archive": sample,
            "archive_sha256": hashlib.sha256(archive_bytes).hexdigest(),
            "csinfo_bytes": len(cs_bytes),
            "csinfo_sha256": hashlib.sha256(cs_bytes).hexdigest(),
            "directives": {key: sum(k == key for k, _ in cs.records) for key in ARITY},
            "frames": frames[0],
            "nodes": anim.header[3],
            "blocks": anim.header[6],
            "chunk_bytes": anim.header[5],
            "animation_header_bytes": len(anim_bytes),
            "animation_header_sha256": hashlib.sha256(anim_bytes).hexdigest(),
            "csinfo_semantic_roundtrip": True,
            "ANRS_byte_roundtrip": True,
            "stream_bytes": len(raw),
            "stream_sha256": hashlib.sha256(raw).hexdigest(),
            "stream_block_extent_matches": True,
        }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(validate_disc(args.iso), indent=2))


if __name__ == "__main__":
    main()
