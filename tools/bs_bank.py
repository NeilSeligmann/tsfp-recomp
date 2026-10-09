# SPDX-License-Identifier: GPL-3.0-or-later
"""Lossless .bs offset-bank codec. Counts come from the loader, not the file."""

import struct
from dataclasses import dataclass


@dataclass(frozen=True)
class Bank:
    offsets: tuple[int, ...]
    payload: bytes

    def encode(self) -> bytes:
        return struct.pack(f"<{len(self.offsets)}I", *self.offsets) + self.payload

    def string_bytes(self, index: int) -> bytes:
        if not 0 <= index < len(self.offsets):
            raise IndexError(index)
        offset = self.offsets[index] - 4 * len(self.offsets)
        return self.payload[offset : self.payload.index(b"\0", offset)]


def decode(data: bytes, count: int) -> Bank:
    if count <= 0 or len(data) < 4 * count:
        raise ValueError("truncated offset table or invalid count")
    offsets = struct.unpack_from(f"<{count}I", data)
    for offset in offsets:
        if offset < 4 * count or offset >= len(data):
            raise ValueError("offset outside payload")
        if b"\0" not in data[offset:]:
            raise ValueError("unterminated string")
    return Bank(offsets, data[4 * count :])


def main() -> None:
    """Measure owner-supplied English banks without printing text or writing assets."""
    import argparse
    import hashlib
    from pathlib import Path

    from tools.frontend_labels import FIRST_TABLE_ENTRIES, SECOND_TABLE_ENTRIES, load

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    args = parser.parse_args()
    labels = load(args.iso)
    for slot, data, count in (
        ("first", labels.first, FIRST_TABLE_ENTRIES),
        ("story", labels.second, SECOND_TABLE_ENTRIES),
    ):
        bank = decode(data, count)
        if bank.encode() != data:
            raise ValueError("bank roundtrip differs")
        print(
            slot,
            "bytes",
            len(data),
            "entries",
            count,
            "unique_offsets",
            len(set(bank.offsets)),
            "sha256",
            hashlib.sha256(data).hexdigest(),
            "roundtrip=equal",
        )


if __name__ == "__main__":
    main()
