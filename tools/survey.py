# SPDX-License-Identifier: GPL-3.0-or-later
"""Triage a disc image for symbol-bearing executables.

Demo, review and pre-release discs sometimes ship builds that retail does not:
unstripped symbol tables, populated debug sections, or debug-keyed executables
with asserts still live. Those are worth far more than any amount of inference,
so every new disc should be surveyed before anything else is done with it.

What this looks for, in descending order of value:

1. **A populated `.mdebug` (PS2)** — stabs type information. This is what makes
   the TS2 Demo 53 build valuable: 7.3 MB of it, against size-zero on retail.
2. **A `.symtab`** — function and object names with addresses.
3. **A debug-keyed XBE (Xbox)** — if the entry point decodes with the *debug* XOR
   key rather than the retail one, asserts are plausibly compiled in.
4. **Assert `__FILE__` strings** — the engine's source tree, even with no symbols.
5. **`host0:` dev-host paths** — the asset tree as the developers saw it.

MEASURED, on retail `default.xbe` (6,270,976 bytes) and the OXM 46 demo
`tsdemo_cd.xbe` (5,750,784 bytes): neither has 1-3. Retail has 33 source paths
(14 of them `d:/build/...`) and 21 `host0:` paths; the demo has 26 and 17.

TEMPERED EXPECTATION FOR 3. This module used to assert that Free Radical's
assert macro passes `__func__`. Both real binaries disprove it -- the only
assert format either contains is `assert '%.128s' at %.128s:%ld failed`, an
expression/__FILE__/__LINE__ triplet with no function argument. So a debug-keyed
build would still yield file and line, not function names. Worth finding, worth
less than previously claimed.
"""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

from tools.xbe.parser import ENTRY_XOR_DEBUG, ENTRY_XOR_RETAIL

ELF_MAGIC = b"\x7fELF"
XBE_MAGIC = b"XBEH"

RE_SOURCE_PATH = re.compile(
    rb"[A-Za-z]:[\\/][ -~]{3,200}?\.(?:cpp|c|h|hpp|inl)|"
    rb"[A-Za-z0-9_][A-Za-z0-9_./\\-]{2,160}\.(?:cpp|c|h|hpp|inl)"
)
RE_HOST0 = re.compile(rb"host0:[ -~]{3,160}")
RE_ASSERT_FUNC = re.compile(rb"Function: %s|ASSERT FAILED|assert\(%s\)")


@dataclass
class ExecutableReport:
    """What one executable inside a disc turned out to contain."""

    path: str
    size: int
    kind: str  # "elf", "xbe", "dol" or "unknown"
    sections: dict[str, int] = field(default_factory=dict)
    symtab_entries: int = 0
    mdebug_bytes: int = 0
    is_debug_build: bool | None = None
    source_paths: list[str] = field(default_factory=list)
    #: Distinct paths found, which exceeds len(source_paths) when truncated by
    #: max_paths. Reported separately so a figure that is really a cap cannot be
    #: mistaken for a count.
    source_path_total: int = 0
    host0_paths: int = 0
    has_assert_func_macro: bool = False

    @property
    def score(self) -> int:
        """Rough value ranking, so a survey can be sorted by what matters."""
        value = 0
        if self.mdebug_bytes > 1000:
            value += 1000
        if self.symtab_entries > 0:
            value += 500
        if self.is_debug_build:
            value += 400
        if self.has_assert_func_macro:
            value += 200
        value += min(max(self.source_path_total, len(self.source_paths)), 100)
        value += min(self.host0_paths // 10, 50)
        return value

    def render(self) -> str:
        bits = [f"{self.path}  ({self.size} bytes, {self.kind})"]
        if self.mdebug_bytes:
            bits.append(f"    .mdebug      {self.mdebug_bytes} bytes  <-- TYPE INFORMATION")
        if self.symtab_entries:
            bits.append(f"    .symtab      {self.symtab_entries} symbols  <-- NAMES")
        if self.is_debug_build is not None:
            label = "DEBUG BUILD  <-- asserts may be live" if self.is_debug_build else "retail"
            bits.append(f"    build        {label}")
        if self.has_assert_func_macro:
            bits.append("    asserts      carry __func__  <-- names every asserting function")
        if self.source_paths:
            shown = len(self.source_paths)
            total = max(self.source_path_total, shown)
            count = f"{total}" if total == shown else f"{shown} of {total} kept"
            bits.append(f"    source paths {count}")
            for path in self.source_paths[:5]:
                bits.append(f"       {path}")
        if self.host0_paths:
            bits.append(f"    host0: paths {self.host0_paths}")
        if self.score == 0:
            bits.append("    (nothing of interest)")
        return "\n".join(bits)


def _elf_sections(data: bytes) -> dict[str, int]:
    """Section name -> size for a 32-bit little-endian ELF. Empty on malformed input."""
    if len(data) < 0x34 or data[:4] != ELF_MAGIC:
        return {}
    try:
        shoff = struct.unpack_from("<I", data, 0x20)[0]
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
        if shnum == 0 or shoff + shentsize * shnum > len(data):
            return {}
        strtab_hdr = struct.unpack_from("<10I", data, shoff + shstrndx * shentsize)
        strtab = data[strtab_hdr[4] : strtab_hdr[4] + strtab_hdr[5]]
        out: dict[str, int] = {}
        for index in range(shnum):
            fields = struct.unpack_from("<10I", data, shoff + index * shentsize)
            end = strtab.find(b"\0", fields[0])
            if end < 0:
                continue
            out[strtab[fields[0] : end].decode("latin-1")] = fields[5]
        return out
    except struct.error:
        return {}


def _symtab_entries(sections: dict[str, int]) -> int:
    """Symbol count from .symtab's size. ELF32 symbols are 16 bytes each."""
    return sections.get(".symtab", 0) // 16


def _xbe_is_debug(data: bytes) -> bool | None:
    """True when the entry point decodes with the debug XOR key, not retail.

    A debug-keyed build is the single most valuable Xbox find: Free Radical's
    assert macro passes __func__, so a build with asserts live names every
    function containing one.
    """
    if len(data) < 0x17C or data[:4] != XBE_MAGIC:
        return None
    try:
        base = struct.unpack_from("<I", data, 0x104)[0]
        size = struct.unpack_from("<I", data, 0x10C)[0]
        raw = struct.unpack_from("<I", data, 0x128)[0]
    except struct.error:
        return None
    if base <= (raw ^ ENTRY_XOR_RETAIL) < base + size:
        return False
    if base <= (raw ^ ENTRY_XOR_DEBUG) < base + size:
        return True
    return None


def survey_executable(path: str, data: bytes, *, max_paths: int = 400) -> ExecutableReport:
    """Classify one executable and report every symbol signal it carries."""
    if data[:4] == ELF_MAGIC:
        kind = "elf"
    elif data[:4] == XBE_MAGIC:
        kind = "xbe"
    elif len(data) > 0xE4 and struct.unpack_from(">I", data, 0xE0)[0] & 0x80000000:
        kind = "dol"
    else:
        kind = "unknown"

    sections = _elf_sections(data) if kind == "elf" else {}
    paths = sorted({m.group(0).decode("latin-1") for m in RE_SOURCE_PATH.finditer(data)})

    return ExecutableReport(
        path=path,
        size=len(data),
        kind=kind,
        sections=sections,
        symtab_entries=_symtab_entries(sections),
        mdebug_bytes=sections.get(".mdebug", 0),
        is_debug_build=_xbe_is_debug(data) if kind == "xbe" else None,
        source_paths=paths[:max_paths],
        source_path_total=len(paths),
        host0_paths=len(RE_HOST0.findall(data)),
        has_assert_func_macro=bool(RE_ASSERT_FUNC.search(data)),
    )


def looks_like_executable(name: str, size: int) -> bool:
    """Cheap filter so a survey does not read every asset on a 4 GB disc."""
    if size < 0x1000 or size > 64 * 1024 * 1024:
        return False
    lowered = name.lower()
    if lowered.endswith((".elf", ".xbe", ".dol", ".irx", ".rel")):
        return True
    stem = Path(lowered).name
    # PS2 boot executables are named by serial, e.g. SLUS_211.48, SLUS_999.99.
    return bool(re.match(r"^(slus|sles|sled|slps|scus|sces|sced)_\d", stem))
