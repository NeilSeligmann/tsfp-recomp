# SPDX-License-Identifier: GPL-3.0-or-later
"""Reader for XDVDFS, the Xbox DVD filesystem.

The volume descriptor sits at sector 32. Directories are binary trees whose
subtree offsets are expressed in 4-byte units relative to the directory's own
data. Nothing in the image is trusted: offsets are bounds-checked, the tree walk
guards against cycles, and extraction refuses paths that escape the destination.
"""

from __future__ import annotations

import struct
from pathlib import Path
from typing import BinaryIO

from tools.errors import BadMagicError, ParseError
from tools.xdvdfs.model import XisoEntry

SECTOR_SIZE = 2048
XISO_MAGIC = b"MICROSOFT*XBOX*MEDIA"
DESCRIPTOR_SECTOR = 32
ATTR_DIRECTORY = 0x10
DIRENT_FIXED_SIZE = 14
MAX_DEPTH = 32


def safe_join(dest: Path, relative: str) -> Path:
    """Join `relative` onto `dest`, refusing anything that escapes it."""
    normalised = relative.replace("\\", "/")
    if normalised.startswith("/"):
        raise ParseError(f"absolute path in image: {relative!r}")
    candidate = (dest / normalised).resolve()
    root = dest.resolve()
    if candidate != root and root not in candidate.parents:
        raise ParseError(f"path escapes destination: {relative!r}")
    return candidate


class XisoReader:
    """Reads entries and file data out of an Xbox ISO."""

    def __init__(self, stream: BinaryIO) -> None:
        self._stream = stream
        self._entries: list[XisoEntry] | None = None

    def _read_at(self, offset: int, length: int) -> bytes:
        if offset < 0 or length < 0:
            raise ParseError(f"bad read: offset={offset} length={length}")
        self._stream.seek(offset)
        data = self._stream.read(length)
        if len(data) != length:
            raise ParseError(f"short read at {offset}: wanted {length}, got {len(data)}")
        return data

    def _read_descriptor(self) -> tuple[int, int]:
        descriptor = self._read_at(DESCRIPTOR_SECTOR * SECTOR_SIZE, SECTOR_SIZE)
        if descriptor[: len(XISO_MAGIC)] != XISO_MAGIC:
            raise BadMagicError(
                f"expected {XISO_MAGIC!r} at sector {DESCRIPTOR_SECTOR}, "
                f"found {descriptor[: len(XISO_MAGIC)]!r}"
            )
        root_sector, root_size = struct.unpack_from("<II", descriptor, len(XISO_MAGIC))
        return int(root_sector), int(root_size)

    def _walk_directory(
        self, sector: int, size: int, prefix: str, depth: int, out: list[XisoEntry]
    ) -> None:
        if depth > MAX_DEPTH or size == 0:
            return
        data = self._read_at(sector * SECTOR_SIZE, size)
        pending: list[int] = [0]
        seen: set[int] = set()
        subdirs: list[tuple[int, int, str]] = []

        while pending:
            offset = pending.pop()
            if offset in seen or offset + DIRENT_FIXED_SIZE > len(data):
                continue
            seen.add(offset)
            left, right, start, length, attrs, name_len = struct.unpack_from(
                "<HHIIBB", data, offset
            )
            name_end = offset + DIRENT_FIXED_SIZE + name_len
            if name_end > len(data):
                continue
            name = data[offset + DIRENT_FIXED_SIZE : name_end].decode("latin-1")
            is_dir = bool(attrs & ATTR_DIRECTORY)
            path = f"{prefix}{name}"
            out.append(XisoEntry(path=path, start_sector=start, size=length, is_dir=is_dir))
            if is_dir:
                subdirs.append((start, length, f"{path}/"))

            if left != 0xFFFF:
                pending.append(left * 4)
            if right != 0xFFFF:
                pending.append(right * 4)
            # A flat directory lays entries out consecutively with no subtree links.
            if left == 0xFFFF and right == 0xFFFF:
                nxt = name_end + (-name_end % 4)
                if nxt > offset:
                    pending.append(nxt)

        for start, length, sub_prefix in subdirs:
            self._walk_directory(start, length, sub_prefix, depth + 1, out)

    def list_entries(self) -> list[XisoEntry]:
        """Return every entry in the image, recursively. Cached after the first call."""
        if self._entries is None:
            root_sector, root_size = self._read_descriptor()
            entries: list[XisoEntry] = []
            self._walk_directory(root_sector, root_size, "", 0, entries)
            self._entries = entries
        return self._entries

    def read_file(self, path: str) -> bytes:
        wanted = path.replace("\\", "/").lstrip("/").lower()
        for entry in self.list_entries():
            if not entry.is_dir and entry.path.lower() == wanted:
                if entry.size == 0:
                    return b""
                return self._read_at(entry.start_sector * SECTOR_SIZE, entry.size)
        raise ParseError(f"not found in image: {path!r}")

    def extract(self, dest: Path, only: list[str] | None = None) -> list[Path]:
        """Extract files to `dest`. Returns the paths written."""
        wanted = None
        if only is not None:
            wanted = {p.replace("\\", "/").lstrip("/").lower() for p in only}
        written: list[Path] = []
        for entry in self.list_entries():
            if entry.is_dir:
                continue
            if wanted is not None and entry.path.lower() not in wanted:
                continue
            target = safe_join(dest, entry.path)
            target.parent.mkdir(parents=True, exist_ok=True)
            payload = (
                b""
                if entry.size == 0
                else self._read_at(entry.start_sector * SECTOR_SIZE, entry.size)
            )
            target.write_bytes(payload)
            written.append(target)
        return written
