# SPDX-License-Identifier: GPL-3.0-or-later
"""Scan a game disc for vertex programs. Counts and file paths only, never content.

The question: do the title's precompiled vertex programs live on the disc (a resource
file) or inside the XBE? `vsh.find_headed_programs` finds a program by its header dword
and the structure of the instructions that follow, so the same predicate can be run
over every byte the disc holds and the two answers compared.

WHAT IS SCANNED. Every file in the XDVDFS image. A `.pak` is parsed with
`tools.pak` and each entry is inflated when it is gzip, because 38.8% of real entries
are compressed and a scan of the raw bytes would not see inside them. A pak that does not
parse, and any other file, is scanned as raw bytes.

WHAT A MISS DOES NOT PROVE. The scan finds programs in the XBE's own encoding. A program
stored in a different container (another compression, an XOR layer, a different header)
would not be found, and a pak entry that fails to inflate is counted in `entry_failures`
rather than skipped silently. The control is the XBE itself: the same predicate must find
the 48 programs there, which `tests/test_shaderscan.py` pins on a synthetic image and the
CLI reports on the real one.
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path

from tools.errors import ParseError
from tools.pak import parse_pak
from tools.shaderscan.vsh import find_headed_programs
from tools.xdvdfs.reader import XisoReader


@dataclass
class DiscScan:
    """Totals for one scan. Holds paths and counts, no program bytes."""

    files: int = 0
    bytes_scanned: int = 0
    pak_files: int = 0
    pak_entries: int = 0
    pak_parse_failures: int = 0
    entry_failures: int = 0
    #: path -> programs found in it (any entry)
    hits_by_path: Counter[str] = field(default_factory=Counter)
    instructions: int = 0

    @property
    def programs(self) -> int:
        return sum(self.hits_by_path.values())


def _scan_buffer(scan: DiscScan, path: str, data: bytes) -> None:
    scan.bytes_scanned += len(data)
    for program in find_headed_programs(data):
        scan.hits_by_path[path] += 1
        scan.instructions += program.instructions


def _payloads(scan: DiscScan, data: bytes) -> Iterator[bytes]:
    """Every entry payload of a pak, inflated. Failures are counted, not hidden."""
    archive = parse_pak(data)
    for entry in archive.entries:
        scan.pak_entries += 1
        try:
            payload = archive.entry_data(data, entry)
        except ParseError:
            scan.entry_failures += 1
            continue
        yield payload


def scan_disc(iso: Path) -> DiscScan:
    """Scan every file on `iso`. Reads each file whole, one at a time."""
    scan = DiscScan()
    with iso.open("rb") as stream:
        reader = XisoReader(stream)
        for entry in reader.list_entries():
            if entry.is_dir:
                continue
            scan.files += 1
            data = reader.read_file(entry.path)
            if entry.path.lower().endswith(".pak"):
                scan.pak_files += 1
                try:
                    for payload in _payloads(scan, data):
                        _scan_buffer(scan, entry.path, payload)
                    continue
                except ParseError:
                    scan.pak_parse_failures += 1
            _scan_buffer(scan, entry.path, data)
    return scan
