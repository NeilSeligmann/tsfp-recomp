# SPDX-License-Identifier: GPL-3.0-or-later
"""Attribute NV2A pushbuffer method headers to the D3D8 function that writes them.

WHAT THIS IS FOR. `.XTLID` names 11 of the 85 D3D entry points the title calls. The
other 74 are addresses and a call count, and a call count ranks work without
explaining it. But the Xbox has no user-mode graphics driver -- `D3D8.lib` is linked
into the title and *is* the driver -- so each of those entry points ends in writing
NV2A pushbuffer commands, and the commands it writes say what it means. This module
recovers that mapping: entry point -> set of methods written.

WHY IT DECODES THE BINARY AND NOT THE LIFTED C. The lifted C in `generated/lifted/`
is regenerated regularly and its line numbers move; the XBE does not. Decoding also
reaches the parts of the section the lifter has no function record for.

HOW A HEADER IS RECOGNISED, AND WHY THE FILTER IS FIELD CONSTRAINTS ONLY. Two shapes
account for almost every header this section emits:

  IMMEDIATE   `mov dword ptr [reg+disp], imm32` -- the header is a literal, which is
              what the compiler produces when the method and the count are both known
              at the call site. This is the common case by a wide margin.
  COMPUTED    the count is a variable, so the writer builds the header with a shift
              and an `or` against the bare method: `shl eax, 0x12` then
              `or eax, 0x0B80`. The shift of 18 is the count field's position, so the
              pair is recognisable, and the `or`'s immediate is the method.

`methods.is_plausible_header` gates the first shape on the header's field constraints
and nothing else -- no appeal to a name table -- so the scan cannot quietly become a
search for methods somebody already thought of. The COMPUTED shape has no such
constraints to lean on (`0x0B80` is just a small integer), so it is gated on a shift
by exactly 18 feeding the `or`, and is reported separately from the immediates rather
than merged into them, because it rests on a weaker premise.

WHAT THE FILTER COSTS. `is_plausible_header` accepts any 4-aligned value in the
method window with a non-zero count and the reserved and high bits clear, which a
non-header constant can satisfy by luck. `header_null` measures how often: it runs the
same predicate over every 4-aligned dword of a section's *data* neighbours, where no
pushbuffer writer exists, so the rate it reports is the rate of coincidence. Read any
single-hit attribution against that number. The attributions this project leans on are
multi-hit and ordered (a constant-load method followed by a constant-data method, a
begin followed by a draw followed by an end), and a coincidence does not produce an
ordered pair.

WHAT IT CANNOT SEE. A header assembled from a value loaded out of memory, or one whose
method comes from a table rather than an immediate, leaves no constant for either
shape to find. Such a function appears here with an empty method set, which means "not
recovered", never "writes nothing". `FunctionMethods.recovered` says which it is.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

import capstone

from tools.codediff.boundaries import Function
from tools.d3dscan.methods import (
    MethodHeader,
    decode_header,
    is_plausible_header,
    name_method,
)
from tools.gen_d3d8_surface import Image, decode_section
from tools.xbe import parse_xbe

#: The count field's bit position. A writer that computes a count shifts left by this.
COUNT_SHIFT = 18

#: Bare-method immediates below this are indistinguishable from ordinary small
#: constants, so the COMPUTED shape is not trusted for them.
MIN_COMPUTED_METHOD = 0x100


@dataclass(frozen=True)
class MethodWrite:
    """One recovered pushbuffer command header and where it was written from."""

    va: int
    header: MethodHeader
    #: "immediate" or "computed"; see the module docstring on why they stay apart.
    shape: str

    def describe(self, names: dict[int, str] | None = None) -> str:
        head = self.header
        flag = " non-inc" if head.non_incrementing else ""
        return (
            f"{self.va:#010x} {self.shape:9s} {head.raw:#010x} "
            f"{name_method(head.method, names)} count={head.count}{flag}"
        )


@dataclass
class FunctionMethods:
    """Every method header recovered inside one function's bounds."""

    entry_va: int
    end_va: int
    name: str
    writes: list[MethodWrite] = field(default_factory=list)

    @property
    def recovered(self) -> bool:
        """Did anything come back? False means "not recovered", never "writes nothing"."""
        return bool(self.writes)

    def methods(self) -> list[int]:
        """Distinct method numbers written, in first-write order.

        Order is kept because it carries the meaning: a constant-load method followed
        by a constant-data method is an upload, and the same two methods in the other
        order would not be.
        """
        seen: dict[int, None] = {}
        for write in self.writes:
            seen.setdefault(write.header.method, None)
        return list(seen)


def _immediate_header(insn: capstone.CsInsn) -> int | None:
    """The header literal in a `mov dword ptr [...], imm32`, or None."""
    if insn.mnemonic != "mov" or len(insn.operands) != 2:
        return None
    dest, src = insn.operands
    if dest.type != capstone.x86.X86_OP_MEM or src.type != capstone.x86.X86_OP_IMM:
        return None
    if dest.size != 4:
        return None
    value = src.imm & 0xFFFFFFFF
    return value if is_plausible_header(value) else None


def _computed_method(insn: capstone.CsInsn, shifted: set[int]) -> int | None:
    """The bare method in an `or reg, imm32` whose register was just shifted by 18."""
    if insn.mnemonic != "or" or len(insn.operands) != 2:
        return None
    dest, src = insn.operands
    if dest.type != capstone.x86.X86_OP_REG or src.type != capstone.x86.X86_OP_IMM:
        return None
    if dest.reg not in shifted:
        return None
    method = src.imm & 0xFFFFFFFF
    if method & 0x03 or not MIN_COMPUTED_METHOD <= method <= 0x1FFF:
        return None
    return method


def _count_shift_target(insn: capstone.CsInsn) -> int | None:
    """The register of a `shl reg, 18`, which is a count being moved into position."""
    if insn.mnemonic not in ("shl", "sal") or len(insn.operands) != 2:
        return None
    dest, src = insn.operands
    if dest.type != capstone.x86.X86_OP_REG or src.type != capstone.x86.X86_OP_IMM:
        return None
    return dest.reg if src.imm == COUNT_SHIFT else None


def scan_section(data: bytes, base_va: int) -> list[MethodWrite]:
    """Recover every method header written by the code in `data`.

    `base_va` is the virtual address of `data[0]`. Returns writes in address order.
    """
    instructions, _ = decode_section(data, base_va)
    writes: list[MethodWrite] = []
    shifted: set[int] = set()
    for insn in instructions:
        value = _immediate_header(insn)
        if value is not None:
            header = decode_header(value)
            if header is not None:
                writes.append(MethodWrite(va=insn.address, header=header, shape="immediate"))

        method = _computed_method(insn, shifted)
        if method is not None:
            # The count is a variable, so it is unknown here; record it as zero-width
            # rather than invent a value, and let `shape` mark why.
            writes.append(
                MethodWrite(
                    va=insn.address,
                    header=MethodHeader(
                        raw=method,
                        method=method,
                        subchannel=0,
                        count=0,
                        non_incrementing=False,
                    ),
                    shape="computed",
                )
            )

        target = _count_shift_target(insn)
        if target is not None:
            shifted = {target}
        elif insn.mnemonic not in ("or", "nop"):
            # A shift's result must reach the `or` directly. Anything else in between
            # breaks the pairing rather than being assumed harmless.
            shifted.clear()
    return writes


def attribute(
    writes: list[MethodWrite],
    functions: list[Function],
    lo_va: int,
    hi_va: int,
) -> list[FunctionMethods]:
    """Bucket `writes` into the function whose bounds contain them.

    Functions outside `[lo_va, hi_va)` are dropped. A write inside no function's bounds
    is dropped too and the caller is expected to notice via the returned totals, since
    a section's function table is never complete.
    """
    inside = sorted(
        (
            FunctionMethods(
                entry_va=fn.entry_va,
                end_va=max(fn.entry_va + fn.size_bytes, fn.body_max_va + 1),
                name=fn.name,
            )
            for fn in functions
            if lo_va <= fn.entry_va < hi_va
        ),
        key=lambda item: item.entry_va,
    )
    if not inside:
        return []
    for write in writes:
        slot = _containing(inside, write.va)
        if slot is not None:
            slot.writes.append(write)
    return inside


def _containing(items: list[FunctionMethods], va: int) -> FunctionMethods | None:
    """Binary search for the function whose `[entry_va, end_va)` holds `va`."""
    low, high = 0, len(items) - 1
    while low <= high:
        mid = (low + high) // 2
        item = items[mid]
        if va < item.entry_va:
            high = mid - 1
        elif va >= item.end_va:
            low = mid + 1
        else:
            return item
    return None


def header_null(data: bytes) -> tuple[int, int]:
    """How often `is_plausible_header` fires on data that is not a header.

    Returns `(hits, dwords)` over every 4-aligned dword of `data`, exhaustively, so
    there is no sample and no seed. Run it on a section with no pushbuffer writer in
    it and the ratio is the coincidence rate against which a single-hit attribution
    has to be read.
    """
    dwords = len(data) // 4
    hits = 0
    for offset in range(0, dwords * 4, 4):
        (value,) = struct.unpack_from("<I", data, offset)
        if is_plausible_header(value):
            hits += 1
    return hits, dwords


def load_image(path: Path) -> Image:
    """Read an XBE into an `Image`, reusing `tools.gen_d3d8_surface`'s accessor."""
    data = path.read_bytes()
    return Image(data, parse_xbe(data))
