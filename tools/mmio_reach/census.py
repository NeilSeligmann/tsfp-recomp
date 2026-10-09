# SPDX-License-Identifier: GPL-3.0-or-later
"""Static half of the probe: NV2A register constants in lifted code, and who calls them.

A constant scan is a LOWER BOUND. D3D keeps the register base in the device structure
(`MEM32(ecx) = 0xFD000000` in the CDevice initialiser) and every later access goes
through it, so only the function that stores the base shows a constant. That is why
the measured half (`instrument`, `boot`) exists.

The lifter writes an address above 0x7FFFFFFF as a NEGATIVE decimal inside `MEM32(...)`
(`MEM32(-44039872)` is `0xFD600140`), so the scan reads both spellings.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

from tools.initmap.liftparse import FunctionIndex

#: The NV2A register aperture on a retail console.
NV2A_WINDOW = range(0xFD000000, 0xFE000000)

_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_NUMBER = r"(?:0x[0-9A-Fa-f]{1,8}u?|-\d{1,10})"
_LITERAL = re.compile(rf"(?<![\w.])({_NUMBER})(?![\w.])")
#: An access whose whole address operand is one constant: `MEM32(0xFD001804u)`. A mask
#: such as `eax & 0xFDFFFFFFu` is a constant in the window and not a register access.
_DIRECT = re.compile(rf"\bMEM\w*\(\s*({_NUMBER})\s*\)")
_MANUAL_DISPATCH = re.compile(r"xdk_thunk_dispatch_at\(0x([0-9A-Fa-f]{8})u\)")

#: Where a path upward from a finding stops without an intercepted boundary.
END_GAME = "game"
END_ROOT = "no_caller"
END_UNKNOWN = "unknown_site"
GAME_SECTION = ".text"


def literal_value(text: str) -> int:
    """A lifter integer literal as an unsigned 32-bit value."""
    if text.startswith("-"):
        return int(text, 10) & 0xFFFFFFFF
    return int(text.rstrip("u"), 16)


@dataclass(frozen=True)
class Finding:
    entry: int
    #: Every literal inside the window, masks included.
    constants: tuple[int, ...]
    #: The subset used as a bare memory address.
    direct: tuple[int, ...]


def scan_function(
    body: str, window: range = NV2A_WINDOW
) -> tuple[tuple[int, ...], tuple[int, ...]]:
    text = _COMMENT.sub("", body)
    constants = sorted(
        {v for v in (literal_value(m.group(1)) for m in _LITERAL.finditer(text)) if v in window}
    )
    direct = sorted(
        {v for v in (literal_value(m.group(1)) for m in _DIRECT.finditer(text)) if v in window}
    )
    return tuple(constants), tuple(direct)


def scan(index: FunctionIndex, window: range = NV2A_WINDOW) -> list[Finding]:
    """Every lifted function with a literal in `window`, in address order."""
    found: list[Finding] = []
    for entry in sorted(index.spans):
        constants, direct = scan_function(index.body(entry), window)
        if constants:
            found.append(Finding(entry, constants, direct))
    return found


def read_manual(path: Path) -> frozenset[int]:
    """Addresses whose body the manual lift replaced by a dispatch into the HLE."""
    text = path.read_text(encoding="utf-8", errors="replace")
    found = frozenset(int(m.group(1), 16) for m in _MANUAL_DISPATCH.finditer(text))
    if not found:
        raise ValueError(f"no xdk_thunk_dispatch_at rows in {path}; the file shape changed")
    return found


@dataclass(frozen=True)
class FunctionRecord:
    start: int
    end: int
    name: str
    section: str
    #: Entry addresses of the calling functions. The table records the caller's start, not
    #: the call instruction (MEASURED: all 46183 entries in the shipped table are function
    #: starts), so a site resolved through `containing` is that caller itself.
    called_by: tuple[int, ...]


def read_functions(path: Path) -> list[FunctionRecord]:
    """The disassembler's function table, which covers suppressed bodies too."""
    raw = json.loads(path.read_text(encoding="utf-8"))
    records = [
        FunctionRecord(
            start=int(item["start"], 16),
            end=int(item["end"], 16),
            name=item["name"],
            section=item["section"],
            called_by=tuple(int(site, 16) for site in item["called_by"]),
        )
        for item in raw
    ]
    if not records:
        raise ValueError(f"no functions in {path}")
    records.sort(key=lambda r: r.start)
    return records


def containing(records: list[FunctionRecord], address: int) -> FunctionRecord | None:
    """The function whose [start, end) holds `address`, by bisection."""
    low, high = 0, len(records)
    while low < high:
        mid = (low + high) // 2
        if records[mid].start <= address:
            low = mid + 1
        else:
            high = mid
    if low == 0:
        return None
    candidate = records[low - 1]
    return candidate if address < candidate.end else None


@dataclass(frozen=True)
class Link:
    """`caller` reaches `callee`; `site` is the table entry that named the caller."""

    callee: int
    caller: int | None
    site: int
    #: The caller's body is replaced by an HLE dispatch, so nothing above it can run it.
    caller_intercepted: bool


def callers_upward(
    records: list[FunctionRecord],
    target: int,
    intercepted: frozenset[int],
    limit: int = 10_000,
) -> list[Link]:
    """Every call edge on the way up from `target`, stopping at an intercepted caller.

    An intercepted caller is a boundary: its original body never runs, so the edges above
    it are not paths to `target` in the current build. A site outside every known
    function is returned with caller None, never dropped.
    """
    by_start = {r.start: r for r in records}
    if target not in by_start:
        raise KeyError(f"{target:#010x} is not a function entry in the table")
    links: list[Link] = []
    seen = {target}
    queue = [target]
    while queue:
        callee = queue.pop(0)
        for site in by_start[callee].called_by:
            owner = containing(records, site)
            caller = owner.start if owner else None
            stop = caller is not None and caller in intercepted
            links.append(Link(callee, caller, site, stop))
            game = owner is not None and owner.section == GAME_SECTION
            if caller is not None and not stop and not game and caller not in seen:
                seen.add(caller)
                queue.append(caller)
                if len(seen) > limit:
                    raise RuntimeError("caller walk exceeded its bound")
    return links


@dataclass(frozen=True)
class Reach:
    """How one function is reached from outside the library, from a callers walk."""

    target: int
    #: Intercepted functions every path runs into first. Their bodies never run.
    blockers: tuple[int, ...]
    #: (kind, address) for each path end that is NOT an intercepted function.
    open_ends: tuple[tuple[str, int], ...]

    @property
    def blocked(self) -> bool:
        return bool(self.blockers) and not self.open_ends


def classify_reach(
    records: list[FunctionRecord], target: int, intercepted: frozenset[int]
) -> Reach:
    """Blocked when every path up from `target` hits an intercepted function."""
    links = callers_upward(records, target, intercepted)
    by_start = {r.start: r for r in records}
    blockers: set[int] = {target} if target in intercepted else set()
    open_ends: set[tuple[str, int]] = set()
    if by_start[target].section == GAME_SECTION:
        open_ends.add((END_GAME, target))
    visited = {target}
    for link in links:
        if link.caller is not None:
            visited.add(link.caller)
        if link.caller is None:
            open_ends.add((END_UNKNOWN, link.site))
        elif link.caller_intercepted:
            blockers.add(link.caller)
        elif by_start[link.caller].section == GAME_SECTION:
            open_ends.add((END_GAME, link.caller))
    for entry in visited:
        record = by_start.get(entry)
        if (
            record is not None
            and not record.called_by
            and entry not in intercepted
            and record.section != GAME_SECTION
        ):
            open_ends.add((END_ROOT, entry))
    return Reach(target, tuple(sorted(blockers)), tuple(sorted(open_ends)))
