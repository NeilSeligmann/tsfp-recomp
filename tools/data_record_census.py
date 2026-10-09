# SPDX-License-Identifier: GPL-3.0-or-later
"""Find initialized data dwords that name decoded function starts.

This is a candidate census, not a claim that every dword is a function
pointer or that every function start has been discovered.  Function starts
must come from a source-bound disassembly (or a separately documented,
hash-bound observed entry); interior instruction addresses are deliberately
not promoted to starts here.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from dataclasses import dataclass
from pathlib import Path

DATA_SECTION_NAMES = frozenset({".rdata", ".data", ".data1", ".idata", ".edata", ".reloc", ".tls"})


@dataclass(frozen=True)
class Section:
    name: str
    virtual_addr: int
    virtual_size: int
    raw_addr: int
    raw_size: int


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_dispatch(text: str) -> set[int]:
    """Read Xbox VAs from the generated ``g_recomp_table`` initializer."""
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL)
    match = re.search(r"g_recomp_table\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\};", text, re.DOTALL)
    if match is None:
        raise ValueError("dispatcher has no g_recomp_table initializer")
    return {int(value, 16) for value in re.findall(r"\{\s*(0x[0-9A-Fa-f]+)u,", match.group(1))}


def scan_data_records(
    image: bytes,
    sections: list[Section],
    function_starts: set[int],
    dispatch_starts: set[int],
    *,
    data_section_names: frozenset[str] = DATA_SECTION_NAMES,
) -> list[dict[str, int | str]]:
    """Return aligned file-backed data dwords naming an omitted function start.

    The scan follows virtual-address dword alignment, validates both virtual
    and raw extents, and requires exact membership in ``function_starts``.
    A value that merely points into a decoded body is not a function start.
    """
    hits: list[dict[str, int | str]] = []
    for section in sections:
        if section.name not in data_section_names:
            continue
        size = min(section.raw_size, section.virtual_size)
        if section.raw_addr < 0 or size < 0 or section.raw_addr + size > len(image):
            raise ValueError(f"invalid raw extent for {section.name}")
        first = (-section.virtual_addr) & 3
        for delta in range(first, size - 3, 4):
            value = struct.unpack_from("<I", image, section.raw_addr + delta)[0]
            if value in function_starts and value not in dispatch_starts:
                hits.append(
                    {
                        "section": section.name,
                        "dword_va": section.virtual_addr + delta,
                        "file_offset": section.raw_addr + delta,
                        "target_va": value,
                    }
                )
    return hits


def _int(value: str | int) -> int:
    return value if isinstance(value, int) else int(value, 16)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbe", type=Path)
    parser.add_argument("analysis_json", type=Path)
    parser.add_argument("functions_json", type=Path)
    parser.add_argument("dispatcher", type=Path)
    parser.add_argument("--expected-xbe-sha256", required=True)
    args = parser.parse_args()

    image = args.xbe.read_bytes()
    actual_hash = sha256(image)
    if actual_hash != args.expected_xbe_sha256.lower():
        parser.error(f"XBE SHA-256 mismatch: {actual_hash}")
    analysis = json.loads(args.analysis_json.read_text())
    cache_path = args.functions_json.parent / ".disasm_cache.json"
    cache = json.loads(cache_path.read_text())
    analysis_hash = sha256(args.analysis_json.read_bytes())
    if cache.get("xbe_hash") != actual_hash or cache.get("json_hash") != analysis_hash:
        parser.error("disassembly cache does not bind this XBE and analysis JSON")
    functions = json.loads(args.functions_json.read_text())
    starts = {_int(function["start"]) for function in functions}
    dispatcher_text = args.dispatcher.read_text()
    dispatch = parse_dispatch(dispatcher_text)
    sections = [
        Section(
            name=section["name"],
            virtual_addr=_int(section["virtual_addr"]),
            virtual_size=_int(section["virtual_size"]),
            raw_addr=_int(section["raw_addr"]),
            raw_size=_int(section["raw_size"]),
        )
        for section in analysis["sections"]
    ]
    hits = scan_data_records(image, sections, starts, dispatch)
    print(
        json.dumps(
            {
                "xbe_sha256": actual_hash,
                "analysis_json_sha256": analysis_hash,
                "functions_json_sha256": sha256(args.functions_json.read_bytes()),
                "dispatcher_sha256": sha256(args.dispatcher.read_bytes()),
                "decoded_function_starts": len(starts),
                "dispatcher_starts": len(dispatch),
                "data_sections_scanned": [
                    {"name": s.name, "va": s.virtual_addr, "raw_size": s.raw_size}
                    for s in sections
                    if s.name in DATA_SECTION_NAMES
                ],
                "missing_dispatch_candidates": hits,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
