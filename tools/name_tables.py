# SPDX-License-Identifier: GPL-3.0-or-later
"""Function-pointer table scan and propagation helpers for T1261 round 2.

    python -m tools.name_tables scan --min-run 2
    python -m tools.name_tables sole --lo 0x12000 --hi 0x180000

`scan` finds dword-aligned runs of pointers to function entries in the data
sections of build/default.xbe (vtables, handler tables), reports each run with the
code sites that reference the table address and the member names already known.
`sole` lists unnamed game functions that are the sole caller of a named function or
that call a named function nobody else calls (propagation candidates, to be read by a
human, never named automatically). Read-only, repo-relative paths.
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import defaultdict
from collections.abc import Iterator
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from tools.name_candidates import Image, game_vas, load_known_names, read_functions
from tools.naming_body import EVIDENCE_NOTICE

DATA_SECTIONS = (".rdata", ".data")


def load(root: Path, xbe: Path) -> tuple[Image, dict[int, int], set[int], dict[int, str]]:
    image = Image(root / xbe, root / "generated/retail/functions.csv")
    funcs = {
        int(r["entry_va"], 16): int(r["size_bytes"])
        for r in read_functions(root / "generated/retail/functions.csv")
    }
    return image, funcs, game_vas(root), load_known_names(root)


def scan_tables(
    image: Image, funcs: dict[int, int], min_run: int
) -> Iterator[tuple[int, list[int]]]:
    """Yield (table_va, [member_va...]) for runs of function-entry pointers."""
    for name in DATA_SECTIONS:
        sec = image.xbe.section_by_name(name)
        blob = image.read(sec.virtual_addr, sec.raw_size) or b""
        count = len(blob) // 4
        words = struct.unpack_from(f"<{count}I", blob)
        i = 0
        while i < count:
            j = i
            while j < count and words[j] in funcs:
                j += 1
            if j - i >= min_run:
                yield sec.virtual_addr + 4 * i, list(words[i:j])
                i = j
            else:
                i = max(j, i + 1)


def code_refs(image: Image, funcs: dict[int, int], targets: set[int]) -> dict[int, list[int]]:
    """Map table address -> function entries whose code has an immediate/disp inside the table."""
    refs: dict[int, list[int]] = defaultdict(list)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    for entry, size in funcs.items():
        for insn in image.body(md, entry, size).instructions:
            for op in insn.operands:
                val = None
                if op.type == X86_OP_IMM:
                    val = op.imm & 0xFFFFFFFF
                elif op.type == X86_OP_MEM and op.mem.base == 0:
                    val = op.mem.disp & 0xFFFFFFFF
                if val in targets and entry not in refs[val]:
                    refs[val].append(entry)
    return refs


def cmd_scan(args: argparse.Namespace, root: Path) -> int:
    image, funcs, game, known = load(root, args.xbe)
    tables = list(scan_tables(image, funcs, args.min_run))
    refs = code_refs(image, funcs, {t for t, _ in tables})
    for table, members in tables:
        in_range = [m for m in members if m in game and args.lo <= m < args.hi]
        if len(in_range) < args.min_game:
            continue
        users = [f"{u:08x}({known.get(u, '-')})" for u in refs.get(table, [])]
        print(f"table 0x{table:08x} slots={len(members)} game={len(in_range)} users={users}")
        for slot, m in enumerate(members):
            print(f"   [{slot}] {m:08x} {'G' if m in game else 'L'} {known.get(m, '-')}")
    image.report_bodies()
    return 0


def callgraph(
    image: Image, funcs: dict[int, int]
) -> tuple[dict[int, set[int]], dict[int, set[int]]]:
    callers: dict[int, set[int]] = defaultdict(set)
    callees: dict[int, set[int]] = defaultdict(set)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    for entry, size in funcs.items():
        for insn in image.body(md, entry, size).instructions:
            if (
                insn.mnemonic in ("call", "jmp")
                and insn.operands
                and insn.operands[0].type == X86_OP_IMM
            ):
                tgt = insn.operands[0].imm
                if tgt in funcs and tgt != entry:
                    callers[tgt].add(entry)
                    callees[entry].add(tgt)
    return callers, callees


def cmd_sole(args: argparse.Namespace, root: Path) -> int:
    image, funcs, game, known = load(root, args.xbe)
    callers, callees = callgraph(image, funcs)
    for va in sorted(funcs):
        if va not in game or va in known or not args.lo <= va < args.hi:
            continue
        up = callers.get(va, set())
        down = callees.get(va, set())
        named_up = [c for c in up if c in known]
        sole_caller = len(up) == 1 and named_up
        sole_callee = [c for c in down if c in known and len(callers[c]) == 1]
        if sole_caller or sole_callee:
            caller_text = [f"{c:08x}:{known.get(c, '-')}" for c in sorted(up)][:4]
            print(
                f"{va:08x} size={funcs[va]} callers={caller_text} "
                f"sole_callee_of={[known[c] for c in sole_callee]} ncallees={len(down)}"
            )
    image.report_bodies()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name in ("scan", "sole"):
        p = sub.add_parser(name)
        p.add_argument("--lo", type=lambda s: int(s, 16), default=0x0)
        p.add_argument("--hi", type=lambda s: int(s, 16), default=0x180000)
        p.add_argument("--min-run", type=int, default=2)
        p.add_argument("--min-game", type=int, default=1)
    args = parser.parse_args()
    print(EVIDENCE_NOTICE, file=sys.stderr)
    return {"scan": cmd_scan, "sole": cmd_sole}[args.cmd](args, args.root)


if __name__ == "__main__":
    sys.exit(main())
