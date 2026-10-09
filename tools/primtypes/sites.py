# SPDX-License-Identifier: GPL-3.0-or-later
"""Census of the title's primitive call sites by the NV2A operation each reaches.

Every draw the title issues takes its operation from the global at 0x4B85D8, which only
the batcher (0x18EA0) and the indexed batcher (0x18FF0) write (`writers`). The mesh
renderer reaches `DrawIndexedVertices` through the translator 0x1E770 instead. Each call
site's kind is traced with `tools.shaderscan.callargs` and converted by RUNNING the
original translator, so no mapping in this file is typed from memory.
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass

import capstone
from capstone import x86 as cs_x86

from tools.d3dscan.oracle import Oracle
from tools.gen_d3d8_surface import decode_section
from tools.primtypes import translate
from tools.shaderscan import callargs
from tools.shaderscan.image import Image

MESH_DRAW = 0x1E7D0
#: Counts that exercise every branch of the batcher (it only compares the count with 2 and 3).
BATCHER_COUNTS = (1, 2, 3, 4, 6, 16)
DYNAMIC = "dynamic"


@dataclass(frozen=True)
class SiteResult:
    """One call site of a translator and the operations it can reach."""

    va: int
    translator: str
    kind: int | None
    count: int | None
    #: Empty when the kind is not a literal (decided at run time, not derived here).
    operations: frozenset[int]


def decode_text(image: Image) -> list[capstone.CsInsn]:
    section = image.xbe.section_by_name(".text")
    if section is None:
        raise ValueError("image has no .text section")
    body = image.raw[section.raw_addr : section.raw_addr + section.raw_size]
    instructions, _ = decode_section(body, section.virtual_addr)
    return instructions


def writers(instructions: Sequence[capstone.CsInsn]) -> list[int]:
    """Addresses of every instruction in .text that stores to the primitive global."""
    found = []
    for insn in instructions:
        if insn.mnemonic != "mov" or len(insn.operands) != 2:
            continue
        destination = insn.operands[0]
        if (
            destination.type == cs_x86.X86_OP_MEM
            and destination.mem.base == 0
            and destination.mem.index == 0
            and destination.mem.disp & 0xFFFFFFFF == translate.GLOBAL_PRIMITIVE
        ):
            found.append(insn.address)
    return found


def _literal(value: callargs.ArgValue) -> int | None:
    return value.value if value.kind == "imm" else None


def census(oracle: Oracle, instructions: Sequence[capstone.CsInsn]) -> list[SiteResult]:
    results: list[SiteResult] = []
    for site in callargs.find_sites(instructions, {translate.BATCHER}, 4):
        kind, count = _literal(site.args[0]), _literal(site.args[3])
        counts = (count,) if count is not None else BATCHER_COUNTS
        operations = (
            frozenset(translate.batcher(oracle, kind, each).operation for each in counts)
            if kind is not None
            else frozenset()
        )
        results.append(SiteResult(site.va, "batcher", kind, count, operations))
    for site in callargs.find_sites(instructions, {translate.INDEXED}, 3):
        kind = _literal(site.args[0])
        operations = (
            frozenset({translate.indexed(oracle, kind, 0).operation})
            if kind is not None
            else frozenset()
        )
        results.append(SiteResult(site.va, "indexed", kind, None, operations))
    for site in callargs.find_sites(instructions, {MESH_DRAW}, 3):
        kind = _literal(site.args[0])
        operations = (
            frozenset({translate.mesh(oracle, kind).operation}) if kind is not None else frozenset()
        )
        results.append(SiteResult(site.va, "mesh", kind, None, operations))
    return results


def by_operation(results: Sequence[SiteResult]) -> Counter[str]:
    """Sites per reachable-operation set, as `NAME` or `NAME|NAME`, or `dynamic`."""
    tally: Counter[str] = Counter()
    for result in results:
        if not result.operations:
            tally[DYNAMIC] += 1
        else:
            names = (translate.OPERATION_NAMES[each] for each in sorted(result.operations))
            tally["|".join(names)] += 1
    return tally
