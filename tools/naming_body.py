# SPDX-License-Identifier: GPL-3.0-or-later
"""Conservative direct-reachable naming evidence with optional exact body membership.

size_bytes is an address count. Legacy min/max spans have unverified membership;
validated exported ranges constrain membership without proving CFG completeness.
Calls are not descended into; indirect transfers remain unresolved.
"""

from __future__ import annotations

import heapq
import re
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_GRP_INT, CS_GRP_IRET, CS_GRP_JUMP, CS_GRP_RET, Cs, CsInsn
from capstone.x86 import X86_OP_IMM

from tools.codediff.boundaries import function_table_rows

MAX_NAMING_SPAN = 1024 * 1024
EVIDENCE_NOTICE = (
    "Naming evidence: direct-reachable instructions in metadata address spans; "
    "membership exact only with validated exported ranges, indirect CFG unresolved; "
    "size/thresholds remain exported address counts."
)


@dataclass(frozen=True)
class BodyBounds:
    entry: int
    byte_count: int
    span: int
    source: str
    error: str = ""
    minimum: int | None = None
    ranges: tuple[tuple[int, int], ...] = ()

    @property
    def lower(self) -> int:
        return self.entry if self.minimum is None else self.minimum

    @property
    def membership(self) -> str:
        return "exact-exported-ranges" if self.ranges and not self.error else "unverified"

    @classmethod
    def from_row(cls, row: Mapping[str, str]) -> BodyBounds:
        try:
            entry = int(row["entry_va"], 16)
        except (KeyError, TypeError, ValueError):
            raise ValueError("invalid or missing entry_va") from None
        try:
            count = int(row["size_bytes"])
            raw_max = row.get("body_max_va", "")
            source = "body_max_va" if raw_max else "legacy-bytecount-window"
            maximum = int(raw_max, 16) if raw_max else entry + count - 1
            raw_min = row.get("body_min_va", "")
            raw_ranges = row.get("body_ranges", "")
            minimum = int(raw_min, 16) if raw_min else entry
            span = maximum - minimum + 1
            if not 0 <= minimum <= entry <= maximum <= 0xFFFFFFFF:
                raise ValueError("invalid VA ordering/range")
            if count <= 0 or span > MAX_NAMING_SPAN:
                raise ValueError("invalid count/span or span exceeds naming limit")
            if raw_ranges:
                if not raw_min or not raw_max:
                    raise ValueError("exact ranges require explicit min and max")
                ranges = _parse_ranges(raw_ranges)
                if ranges[0][0] != minimum or ranges[-1][1] != maximum:
                    raise ValueError("range min/max contradict metadata")
                if sum(hi - lo + 1 for lo, hi in ranges) != count:
                    raise ValueError("range address count contradicts size_bytes")
                if not any(lo <= entry <= hi for lo, hi in ranges):
                    raise ValueError("entry is outside exact body membership")
                return cls(entry, count, span, "exact-body-ranges", "", minimum, ranges)
            if raw_min:
                if not raw_max:
                    raise ValueError("body minimum requires explicit maximum")
                if count > span:
                    raise ValueError("count exceeds explicit min/max span")
                return cls(entry, count, span, "body_min_max_va", "", minimum)
            if count > span:
                return cls(
                    entry,
                    count,
                    span,
                    "unsupported-body-extent",
                    "exported count exceeds entry-to-max span; lower ranges unavailable",
                )
            return cls(entry, count, span, source)
        except (KeyError, ValueError, TypeError) as error:
            return cls(entry, 0, 0, "invalid-metadata", str(error))


def _parse_ranges(raw: str) -> tuple[tuple[int, int], ...]:
    ranges: list[tuple[int, int]] = []
    for item in raw.split(";"):
        if not re.fullmatch(r"0x[0-9a-fA-F]+-0x[0-9a-fA-F]+", item):
            raise ValueError("malformed exact range")
        lo, hi = (int(value, 16) for value in item.split("-"))
        if not 0 <= lo <= hi <= 0xFFFFFFFF:
            raise ValueError("invalid exact range ordering/width")
        if ranges and lo <= ranges[-1][1]:
            raise ValueError("overlapping or unsorted exact ranges")
        # Adjacent exported intervals have an identical union; merge without
        # adding a single address, so instructions may cross that boundary.
        if ranges and lo == ranges[-1][1] + 1:
            ranges[-1] = (ranges[-1][0], hi)
        else:
            ranges.append((lo, hi))
    return tuple(ranges)


def load_bounds(path: Path) -> dict[int, BodyBounds]:
    result: dict[int, BodyBounds] = {}
    for line, row in enumerate(function_table_rows(path), start=2):
        try:
            bounds = BodyBounds.from_row(row)
        except ValueError as error:
            raise ValueError(f"{path}:{line}: {error}") from None
        if bounds.entry in result:
            bounds = BodyBounds(bounds.entry, 0, 0, "invalid-metadata", "duplicate entry")
        result[bounds.entry] = bounds
    return result


@dataclass(frozen=True)
class BodyEvidence:
    bounds: BodyBounds
    instructions: tuple[CsInsn, ...]
    diagnostics: tuple[str, ...]
    skipped_bytes: int

    @property
    def label(self) -> str:
        status = ",".join(self.diagnostics) or "direct-paths-decoded"
        return (
            f"span={self.bounds.span} source={self.bounds.source} "
            f"membership={self.bounds.membership} skipped={self.skipped_bytes} cfg={status}"
        )


def decode_body(
    read: Callable[[int, int], bytes | None], bounds: BodyBounds, md: Cs
) -> BodyEvidence:
    """Read the available span, follow direct control flow, refuse overlapping decodes.

    Invalid metadata, short reads and overlapping instruction boundaries return
    no evidence. A decoded span is never advertised as a complete indirect CFG.
    Skipped bytes may be data, padding, disconnected code or unvisited targets.
    """
    if bounds.error:
        return BodyEvidence(bounds, (), (bounds.source + ":" + bounds.error,), max(0, bounds.span))
    ranges = bounds.ranges or ((bounds.lower, bounds.lower + bounds.span - 1),)
    chunks: list[tuple[int, int, bytes]] = []
    for lo, hi in ranges:
        code = read(lo, hi - lo + 1) or b""
        if len(code) != hi - lo + 1:
            return BodyEvidence(bounds, (), ("short-read",), bounds.span)
        chunks.append((lo, hi, code))

    def chunk_at(address: int) -> tuple[int, int, bytes] | None:
        return next((chunk for chunk in chunks if chunk[0] <= address <= chunk[1]), None)

    md.detail = True
    pending = [bounds.entry]
    visited: set[int] = set()
    occupied: dict[int, int] = {}
    instructions: dict[int, CsInsn] = {}
    diagnostics: set[str] = set()
    if bounds.source == "legacy-bytecount-window":
        diagnostics.add("legacy-window-not-full-extent")

    def enqueue(address: int) -> None:
        if chunk_at(address) is not None:
            if address not in visited:
                heapq.heappush(pending, address)
        else:
            diagnostics.add("outside-exact-membership" if bounds.ranges else "out-of-span-transfer")

    while pending:
        address = heapq.heappop(pending)
        if address in visited:
            continue
        visited.add(address)
        if address in occupied and occupied[address] != address:
            return BodyEvidence(bounds, (), ("overlapping-instruction-target",), bounds.span)
        chunk = chunk_at(address)
        assert chunk is not None
        lo, hi, code = chunk
        offset = address - lo
        insn = next(md.disasm(code[offset : offset + 15], address, count=1), None)
        if insn is None:
            diagnostics.add("undecodable-reachable-address")
            continue
        addresses = range(address, address + insn.size)
        if any(a in occupied and occupied[a] != address for a in addresses):
            return BodyEvidence(bounds, (), ("overlapping-instruction-target",), bounds.span)
        for a in addresses:
            occupied[a] = address
        instructions[address] = insn
        if insn.group(CS_GRP_RET) or insn.group(CS_GRP_IRET):
            continue
        if insn.group(CS_GRP_INT) or insn.mnemonic in ("ud2", "hlt"):
            diagnostics.add("trap-or-halt")
            continue
        if insn.mnemonic in ("ljmp", "lcall"):
            diagnostics.add("unsupported-far-transfer")
            if insn.mnemonic == "ljmp":
                continue
        if insn.group(CS_GRP_JUMP) or insn.mnemonic in ("loop", "loope", "loopne"):
            if len(insn.operands) == 1 and insn.operands[0].type == X86_OP_IMM:
                enqueue(insn.operands[0].imm & 0xFFFFFFFF)
            else:
                diagnostics.add("unresolved-indirect-jump")
            if insn.mnemonic in ("jmp", "ljmp"):
                continue
        # Calls potentially return. Their callee bytes are never attributed here.
        if insn.mnemonic == "call" and (not insn.operands or insn.operands[0].type != X86_OP_IMM):
            diagnostics.add("unresolved-indirect-call")
        if address + insn.size == hi + 1:
            diagnostics.add(
                "fallthrough-at-range-end" if bounds.ranges else "fallthrough-at-span-end"
            )
        else:
            enqueue(address + insn.size)
    if bounds.source != "legacy-bytecount-window" and len(occupied) != bounds.byte_count:
        diagnostics.add("reachable-bytecount-mismatch")
    ordered = tuple(instructions[a] for a in sorted(instructions))
    return BodyEvidence(bounds, ordered, tuple(sorted(diagnostics)), bounds.span - len(occupied))
