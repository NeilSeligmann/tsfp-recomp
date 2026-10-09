# SPDX-License-Identifier: GPL-3.0-or-later
"""Data types for Free Radical PAK containers."""

from __future__ import annotations

import gzip
import zlib
from dataclasses import dataclass

from tools.errors import ParseError, check_range


@dataclass(frozen=True)
class PakEntry:
    key: int
    data_offset: int
    uncompressed_size: int
    stored_size: int
    name: str | None = None

    @property
    def compressed(self) -> bool:
        """True when the payload is gzip. A zero stored_size means stored."""
        return self.stored_size != 0

    @property
    def on_disc_size(self) -> int:
        """Bytes actually occupied on disc."""
        return self.stored_size if self.compressed else self.uncompressed_size


@dataclass(frozen=True)
class PakArchive:
    magic: str
    entries: list[PakEntry]
    entry_stride: int = 16

    @property
    def hashed(self) -> bool:
        """True when keys are CRC32 hashes (P5CK) rather than names or offsets."""
        return self.magic == "P5CK"

    def entry_data(self, data: bytes, entry: PakEntry) -> bytes:
        """Return the entry's payload, inflating it when it is gzip.

        38.8% of real entries are gzip-compressed, so skipping this returns
        truncated garbage rather than failing loudly.
        """
        what = f"entry payload key={entry.key:#x}"
        check_range(data, entry.data_offset, entry.on_disc_size, what)
        raw = data[entry.data_offset : entry.data_offset + entry.on_disc_size]
        if not entry.compressed:
            return raw
        try:
            payload = gzip.decompress(raw)
        except (OSError, EOFError, zlib.error) as exc:
            raise ParseError(f"{what}: gzip inflate failed: {exc}") from exc
        if len(payload) != entry.uncompressed_size:
            raise ParseError(
                f"{what}: inflated to {len(payload)} bytes, expected {entry.uncompressed_size}"
            )
        return payload
