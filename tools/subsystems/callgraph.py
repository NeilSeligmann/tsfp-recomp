# SPDX-License-Identifier: GPL-3.0-or-later
"""Optional, sound call-graph propagation of subsystems to UNKNOWN functions.

The graph is read from the XBE: every direct `call`/`jmp` whose immediate is the entry VA of a
function of the table is an edge (Capstone through `tools.name_candidates.Image`, about 10 s
for the whole image). Propagation is deliberately narrow:

* only UNKNOWN GAME functions receive a subsystem, only from GAME callers, never into or out
  of `library/*`;
* a function whose entry VA is also used as a value (an immediate operand, or an aligned dword
  in a non-code section such as a vtable or callback table) is ADDRESS TAKEN: it has callers
  the graph can not see, so it is never propagated;
* rule A: at least two distinct callers, all classified, all in the same subsystem path;
* rule B: exactly one caller, classified, and the function is at most `SMALL_BYTES` long;
* at most `ROUNDS` rounds; each round reads the state at its start (so the order of functions
  never matters); a non-UNKNOWN row is never overwritten.

The result is INFERRED with evidence `callgraph: all N callers in <path>` (rule A) or
`callgraph: 1 caller in <path>, function of S bytes` (rule B).
"""

from __future__ import annotations

import struct
from collections import defaultdict
from collections.abc import Collection, Mapping
from pathlib import Path

from tools.subsystems.rules import INFERRED, Decision
from tools.subsystems.tree import UNKNOWN_PATH

ROUNDS = 2
SMALL_BYTES = 64
MIN_CALLERS = 2


def build_graph(
    xbe: Path, functions_csv: Path, sizes: Mapping[int, int]
) -> tuple[dict[int, frozenset[int]], frozenset[int]]:
    """(callers of each function VA, address-taken VAs) from the XBE and the function table.

    `sizes` is the shared function table (entry VA -> size, so overrides and additions count);
    the Ghidra export `functions_csv` only supplies the body bounds it has.
    """
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_OP_IMM

    from tools.name_candidates import Image

    image = Image(xbe, functions_csv)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    callers: dict[int, set[int]] = defaultdict(set)
    taken: set[int] = set()
    for entry, size in sizes.items():
        for insn in image.body(md, entry, size).instructions:
            is_branch = insn.mnemonic in ("call", "jmp")
            for op in insn.operands:
                if op.type == X86_OP_IMM:
                    value = op.imm & 0xFFFFFFFF
                    if value in sizes and value != entry:
                        if is_branch:
                            callers[value].add(entry)
                        else:
                            taken.add(value)
    taken |= _data_pointers(image, frozenset(sizes))
    return {va: frozenset(found) for va, found in callers.items()}, frozenset(taken)


def _data_pointers(image: object, entries: frozenset[int]) -> set[int]:
    """Function entries stored as aligned dwords in any section that is not `.text`."""
    found: set[int] = set()
    xbe = image.xbe  # type: ignore[attr-defined]
    data = image.data  # type: ignore[attr-defined]
    for section in xbe.sections:
        if section.name == ".text" or section.raw_size < 4:
            continue
        blob = data[section.raw_addr : section.raw_addr + section.raw_size]
        usable = len(blob) - len(blob) % 4
        for (value,) in struct.iter_unpack("<I", blob[:usable]):
            if value in entries:
                found.add(value)
    return found


def propagate(
    decisions: Mapping[int, Decision],
    callers: Mapping[int, Collection[int]],
    sizes: Mapping[int, int],
    game_vas: Collection[int],
    address_taken: Collection[int] = (),
    *,
    rounds: int = ROUNDS,
    small_bytes: int = SMALL_BYTES,
    min_callers: int = MIN_CALLERS,
) -> dict[int, Decision]:
    """Return `decisions` plus the propagated rows. The input mapping is not modified."""
    result = dict(decisions)
    game = frozenset(game_vas)
    taken = frozenset(address_taken)
    for _ in range(rounds):
        gained: dict[int, Decision] = {}
        for va in sorted(game):
            if result[va].subsystem != UNKNOWN_PATH or va in taken:
                continue
            found = callers.get(va, ())
            if not found:
                continue
            paths = {result[c].subsystem if c in game else None for c in found}
            if len(paths) != 1:
                continue
            (path,) = paths
            if path is None or path == UNKNOWN_PATH or path.startswith("library"):
                continue
            many = len(found) >= min_callers
            if not many and not (len(found) == 1 and sizes[va] <= small_bytes):
                continue
            what = (
                f"all {len(found)} callers in {path}"
                if many
                else f"1 caller in {path}, function of {sizes[va]} bytes"
            )
            gained[va] = Decision(path, INFERRED, f"callgraph: {what}", "callgraph", "callgraph")
        if not gained:
            break
        result.update(gained)
    return result
