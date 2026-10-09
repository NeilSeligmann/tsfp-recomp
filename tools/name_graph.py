# SPDX-License-Identifier: GPL-3.0-or-later
"""Call-graph and global-ownership summaries for unnamed game functions (T1261 round 3).

    python -m tools.name_graph summary --lo 0x12000 --hi 0x180000 --min-size 101
    python -m tools.name_graph show --va 0x00012380 [--disasm]

`summary` prints one line per unnamed game function: size, named callees, named callers,
kernel imports, short strings, and the named functions that share the globals it touches.
`show` prints the same for chosen VAs and optionally the disassembly. Read-only,
repo-relative paths, never writes anything. Output is for a human reader.
"""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path
from typing import TypeAlias

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from tools.name_candidates import Image, game_vas, load_known_names, read_functions
from tools.naming_body import EVIDENCE_NOTICE

GENERIC = {"game_ftol_adjust", "__floor_default", "__ceil_default", "_sprintf"}


FunctionInfo: TypeAlias = tuple[list[int], set[int], set[str], list[str]]
GraphResult: TypeAlias = tuple[
    Image,
    dict[int, int],
    dict[int, str],
    set[int],
    dict[int, FunctionInfo],
    defaultdict[int, set[int]],
    defaultdict[int, set[int]],
    Cs,
]


def build(root: Path, xbe: Path) -> GraphResult:
    image = Image(root / xbe, root / "generated/retail/functions.csv")
    funcs = {
        int(r["entry_va"], 16): int(r["size_bytes"])
        for r in read_functions(root / "generated/retail/functions.csv")
    }
    known = load_known_names(root)
    game = game_vas(root)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    info = {}
    callers = defaultdict(set)
    owners = defaultdict(set)
    for entry, size in funcs.items():
        calls, globs, kern, strs = [], set(), set(), []
        for insn in image.body(md, entry, size).instructions:
            if insn.mnemonic in ("call", "jmp") and insn.operands:
                op = insn.operands[0]
                if op.type == X86_OP_IMM and op.imm in funcs and op.imm != entry:
                    calls.append(op.imm)
                    callers[op.imm].add(entry)
                elif op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                    k = image.kernel_name(op.mem.disp & 0xFFFFFFFF)
                    if k:
                        kern.add(k)
            for op in insn.operands:
                val = None
                if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                    val = op.mem.disp & 0xFFFFFFFF
                elif op.type == X86_OP_IMM and insn.mnemonic in ("push", "mov", "lea"):
                    val = op.imm & 0xFFFFFFFF
                if val is None or insn.mnemonic in ("call", "jmp"):
                    continue
                if 0x4A0000 <= val < 0x800000:
                    text = image.cstring(val)
                    if text and val < 0x5F0000:
                        if text[:50] not in strs:
                            strs.append(text[:50])
                    else:
                        globs.add(val)
                        owners[val].add(entry)
        info[entry] = (calls, globs, kern, strs)
    return image, funcs, known, game, info, callers, owners, md


def line(
    va: int,
    funcs: dict[int, int],
    known: dict[int, str],
    info: dict[int, FunctionInfo],
    callers: dict[int, set[int]],
    owners: dict[int, set[int]],
) -> str:
    calls, globs, kern, strs = info[va]
    nc = sorted({known[c] for c in calls if c in known and known[c] not in GENERIC})
    up = sorted({known[c] for c in callers.get(va, ()) if c in known})
    own = defaultdict(list)
    for g in globs:
        for o in owners[g]:
            if o != va and o in known:
                own[g].append(known[o])
    parts = [f"{va:08x} sz={funcs[va]} nup={len(callers.get(va, ()))} ncall={len(set(calls))}"]
    if nc:
        parts.append("C:" + ",".join(nc[:8]))
    if up:
        parts.append("U:" + ",".join(up[:4]))
    if kern:
        parts.append("K:" + ",".join(sorted(kern)))
    if strs:
        parts.append("S:" + "|".join(strs[:3]))
    if own:
        parts.append(
            "G:"
            + " ".join(f"{g:x}={'/'.join(sorted(set(v))[:2])}" for g, v in sorted(own.items())[:4])
        )
    return "  ".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    sub = parser.add_subparsers(dest="cmd", required=True)

    def hexint(s: str) -> int:
        return int(s, 16)

    s = sub.add_parser("summary")
    s.add_argument("--lo", type=hexint, default=0x12000)
    s.add_argument("--hi", type=hexint, default=0x180000)
    s.add_argument("--min-size", type=int, default=101)
    s.add_argument("--max-size", type=int, default=1 << 30)
    w = sub.add_parser("show")
    w.add_argument("--va", type=hexint, action="append", required=True)
    w.add_argument("--disasm", action="store_true")
    w.add_argument("--max-insn", type=int, default=100000)
    args = parser.parse_args()
    print(EVIDENCE_NOTICE, file=sys.stderr)
    image, funcs, known, game, info, callers, owners, md = build(args.root, args.xbe)
    if args.cmd == "summary":
        for va in sorted(funcs):
            if (
                va in game
                and va not in known
                and args.lo <= va < args.hi
                and args.min_size <= funcs[va] <= args.max_size
            ):
                print(line(va, funcs, known, info, callers, owners), image.body_reports[va].label)
    else:
        for va in args.va:
            print(line(va, funcs, known, info, callers, owners), image.body_reports[va].label)
            if args.disasm:
                for index, insn in enumerate(image.body(md, va, funcs[va]).instructions):
                    if index >= args.max_insn:
                        print("   ...")
                        break
                    nm = ""
                    if (
                        insn.mnemonic == "call"
                        and insn.operands
                        and insn.operands[0].type == X86_OP_IMM
                    ):
                        nm = "  ; " + known.get(insn.operands[0].imm, f"{insn.operands[0].imm:x}")
                    print(f"   {insn.address:08x} {insn.mnemonic} {insn.op_str}{nm}")
    image.report_bodies()
    return 0


if __name__ == "__main__":
    sys.exit(main())
