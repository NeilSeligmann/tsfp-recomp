# SPDX-License-Identifier: GPL-3.0-or-later
"""T1261 round 2 (0x00180000-0x00370000): function-pointer tables and call-graph propagation.

    python -m tools.name_tables_mid tables            # constant function-pointer runs
    python -m tools.name_tables_mid xrefs --va 0x4d0e10  # code that references a table
    python -m tools.name_tables_mid callers --va 0x001a0000
    python -m tools.name_tables_mid append SPEC       # merge 'va|name|evidence' lines

Read-only except `append`. Paths are relative to the repository root. Reuses the loaders of
tools.name_candidates (Image, load_known_names, game_vas, read_functions).
"""

from __future__ import annotations

import argparse
import csv
import struct
import sys
from collections import defaultdict
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from tools.name_candidates import Image, game_vas, load_known_names, read_functions
from tools.naming_body import EVIDENCE_NOTICE

LO = 0x00180000
HI = 0x00370000
DATA_SECTIONS = (".rdata", ".data", ".data1")


def function_sizes(root: Path) -> dict[int, int]:
    rows = read_functions(root / "generated/retail/functions.csv")
    return {int(r["entry_va"], 16): int(r["size_bytes"]) for r in rows}


def pointer_runs(image: Image, entries: set[int], min_slots: int) -> list[tuple[int, list[int]]]:
    """Runs of dwords that are function entries (zero slots allowed inside a run)."""
    runs: list[tuple[int, list[int]]] = []
    for section in image.xbe.sections:
        if section.name not in DATA_SECTIONS:
            continue
        raw = image.data[section.raw_addr : section.raw_addr + section.raw_size]
        count = len(raw) // 4
        values = struct.unpack(f"<{count}I", raw[: count * 4])
        index = 0
        while index < count:
            if values[index] not in entries:
                index += 1
                continue
            end = index
            while end < count and (values[end] in entries or values[end] == 0):
                end += 1
            slots = list(values[index:end])
            while slots and slots[-1] == 0:
                slots.pop()
            if sum(1 for v in slots if v) >= min_slots:
                runs.append((section.virtual_addr + 4 * index, slots))
            index = end
    return runs


def code_refs(image: Image, sizes: dict[int, int], lo: int, hi: int) -> dict[int, list[int]]:
    """Immediate/absolute-memory operands -> list of referencing function entries."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    refs: dict[int, list[int]] = defaultdict(list)
    for entry, size in sizes.items():
        if not lo <= entry < hi:
            continue
        for insn in image.body(md, entry, size).instructions:
            for op in insn.operands:
                value = None
                if (
                    op.type == X86_OP_IMM
                    and insn.mnemonic not in ("call", "jmp")
                    and not insn.mnemonic.startswith("j")
                ):
                    value = op.imm & 0xFFFFFFFF
                elif op.type == X86_OP_MEM:
                    value = op.mem.disp & 0xFFFFFFFF
                if value and value >= 0x400000:
                    refs[value].append(entry)
    return refs


def cmd_tables(args: argparse.Namespace) -> int:
    root = args.root
    image = Image(root / args.xbe, root / "generated/retail/functions.csv")
    game = game_vas(root)
    known = load_known_names(root)
    entries = set(function_sizes(root))
    sizes = function_sizes(root)
    runs = pointer_runs(image, entries, args.min_slots)
    refs = code_refs(image, sizes, 0x10000, 0x470000) if args.xrefs else {}
    for va, slots in runs:
        mid = [v for v in slots if LO <= v < HI and v in game]
        if not mid:
            continue
        users = sorted(set(refs.get(va, [])))
        print(
            f"table 0x{va:08x} slots={len(slots)} mid_game={len(mid)} "
            f"users={[hex(u) for u in users]}"
        )
        if args.verbose:
            for i, v in enumerate(slots):
                print(f"   [{i}] 0x{v:08x} {known.get(v, '')}")
    image.report_bodies()
    return 0


def cmd_taken(args: argparse.Namespace) -> int:
    """Every dword in data sections that equals a mid-range game function entry."""
    root = args.root
    image = Image(root / args.xbe, root / "generated/retail/functions.csv")
    game = game_vas(root)
    known = load_known_names(root)
    wanted = {v for v in function_sizes(root) if LO <= v < HI and v in game}
    for section in image.xbe.sections:
        if section.name not in DATA_SECTIONS:
            continue
        raw = image.data[section.raw_addr : section.raw_addr + section.raw_size]
        for offset in range(0, len(raw) - 3, 4):
            (value,) = struct.unpack_from("<I", raw, offset)
            if value in wanted:
                va = section.virtual_addr + offset
                around = struct.unpack_from("<4I", raw, max(0, offset - 8)) if offset >= 8 else ()
                print(
                    f"data 0x{va:08x} -> 0x{value:08x} {known.get(value, '')} "
                    f"prev={[hex(a) for a in around[:2]]}"
                )
    image.report_bodies()
    return 0


def cmd_dump(args: argparse.Namespace) -> int:
    """Dump dwords at a data VA, annotating function entries, strings and small ints."""
    root = args.root
    image = Image(root / args.xbe, root / "generated/retail/functions.csv")
    known = load_known_names(root)
    entries = set(function_sizes(root))
    for index in range(args.count):
        va = args.va + 4 * index
        blob = image.read(va, 4)
        if blob is None or len(blob) < 4:
            break
        (value,) = struct.unpack("<I", blob)
        note = ""
        if value in entries:
            note = f"FUNC {known.get(value, '')}"
        else:
            text = image.cstring(value, args.max_string) if value >= 0x400000 else None
            if text:
                note = f"STR {text[: args.max_string]!r}"
        print(f"0x{va:08x}: 0x{value:08x} {note}")
    image.report_bodies()
    return 0


def cmd_small(args: argparse.Namespace) -> int:
    """One line of disassembly per unnamed mid-range game function up to --max-size bytes."""
    root = args.root
    image = Image(root / args.xbe, root / "generated/retail/functions.csv")
    known = load_known_names(root)
    game = game_vas(root)
    sizes = function_sizes(root)
    _, callers = call_graph(root, image)
    taken = set()
    for section in image.xbe.sections:
        if section.name in DATA_SECTIONS:
            raw = image.data[section.raw_addr : section.raw_addr + section.raw_size]
            taken.update(struct.unpack_from("<I", raw, o)[0] for o in range(0, len(raw) - 3, 4))
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    for entry in sorted(sizes):
        if not (args.lo <= entry < args.hi and entry in game and entry not in known):
            continue
        if not args.min_size <= sizes[entry] <= args.max_size:
            continue
        parts = []
        body = image.body(md, entry, sizes[entry])
        for insn in body.instructions:
            text = f"{insn.mnemonic} {insn.op_str}".strip()
            if insn.mnemonic in ("call", "jmp") and insn.op_str.startswith("0x"):
                target = int(insn.op_str, 16)
                text += f"<{known[target]}>" if target in known else ""
            parts.append(text)
        flag = "T" if entry in taken else "-"
        print(
            f"{entry:08x} {sizes[entry]:3d} c{len(callers.get(entry, []))}{flag} "
            f"evidence={body.label} | " + "; ".join(parts)
        )
    image.report_bodies()
    return 0


def cmd_dis(args: argparse.Namespace) -> int:
    """Disassemble each --va (function size from functions.csv, else --len bytes)."""
    root = args.root
    image = Image(root / args.xbe, root / "generated/retail/functions.csv")
    known = load_known_names(root)
    sizes = function_sizes(root)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    for va in args.va:
        size = sizes.get(va, args.len)
        print(f"== 0x{va:08x} size={size} name={known.get(va, '-')} in_functions_csv={va in sizes}")
        body = image.body(md, va, size)
        print(body.label)
        for insn in body.instructions:
            note = ""
            if insn.mnemonic in ("call", "jmp") and insn.op_str.startswith("0x"):
                note = f"  ; {known.get(int(insn.op_str, 16), '')}"
            for op in insn.operands:
                if (
                    op.type == X86_OP_IMM
                    and insn.mnemonic in ("push", "mov", "lea")
                    and op.imm >= 0x400000
                ):
                    text = image.cstring(op.imm & 0xFFFFFFFF, 60)
                    if text:
                        note += f"  ; str {text!r}"
            print(f"  {insn.address:08x}  {insn.mnemonic} {insn.op_str}{note}")
    image.report_bodies()
    return 0


def cmd_xrefs(args: argparse.Namespace) -> int:
    root = args.root
    image = Image(root / args.xbe, root / "generated/retail/functions.csv")
    refs = code_refs(image, function_sizes(root), 0x10000, 0x470000)
    for va in args.va:
        print(hex(va), [hex(u) for u in sorted(set(refs.get(va, [])))])
    image.report_bodies()
    return 0


def call_graph(root: Path, image: Image) -> tuple[dict[int, list[int]], dict[int, list[int]]]:
    sizes = function_sizes(root)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    callees: dict[int, list[int]] = {}
    callers: dict[int, list[int]] = defaultdict(list)
    for entry, size in sizes.items():
        out: list[int] = []
        for insn in image.body(md, entry, size).instructions:
            if (
                insn.mnemonic in ("call", "jmp")
                and insn.operands
                and insn.operands[0].type == X86_OP_IMM
            ):
                target = insn.operands[0].imm
                if target in sizes and target != entry:
                    out.append(target)
        callees[entry] = out
        for target in set(out):
            callers[target].append(entry)
    return callees, callers


def cmd_callers(args: argparse.Namespace) -> int:
    root = args.root
    image = Image(root / args.xbe, root / "generated/retail/functions.csv")
    known = load_known_names(root)
    callees, callers = call_graph(root, image)
    for va in args.va:
        print(f"0x{va:08x} {known.get(va, '-')}")
        print("  callers:", [f"{c:08x}:{known.get(c, '-')}" for c in callers.get(va, [])])
        print(
            "  callees:",
            [f"{c:08x}:{known.get(c, '-')}" for c in dict.fromkeys(callees.get(va, []))],
        )
    image.report_bodies()
    return 0


def cmd_append(args: argparse.Namespace) -> int:
    root = args.root
    overlay = root / "tools/data/function_names.csv"
    with overlay.open(newline="") as handle:
        rows = list(csv.reader(handle))
    header, body = rows[0], rows[1:]
    taken_vas = {int(r[0], 16) for r in body}
    taken_names = {r[1] for r in body}
    functions = set(function_sizes(root))
    game = game_vas(root)
    added = 0
    for line in args.spec.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        va_text, name, evidence = (part.strip() for part in line.split("|", 2))
        va = int(va_text, 16)
        problems = []
        if not LO <= va < HI:
            problems.append("outside 0x00180000-0x00370000")
        if va not in functions:
            problems.append("not a function entry")
        elif va not in game:
            problems.append("not game region")
        if va in taken_vas:
            problems.append("VA already named")
        if name in taken_names:
            problems.append("duplicate name")
        if not name.startswith("game_"):
            problems.append("name lacks game_ prefix")
        if not evidence:
            problems.append("empty evidence")
        if problems:
            print(f"SKIP {va_text} {name}: {', '.join(problems)}", file=sys.stderr)
            continue
        taken_vas.add(va)
        taken_names.add(name)
        body.append(
            [f"0x{va:08x}", name, args.confidence, f"{evidence}; docs/t1261-naming-round2-mid.md"]
        )
        added += 1
    body.sort(key=lambda r: int(r[0], 16))
    with overlay.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(body)
    print(f"added {added} rows, overlay now {len(body)}")
    return 0


def hex_int(text: str) -> int:
    return int(text, 16)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    sub = parser.add_subparsers(dest="cmd", required=True)
    tables = sub.add_parser("tables")
    tables.add_argument("--min-slots", type=int, default=2)
    tables.add_argument("--xrefs", action="store_true")
    tables.add_argument("--verbose", action="store_true")
    tables.set_defaults(func=cmd_tables)
    taken = sub.add_parser("taken")
    taken.set_defaults(func=cmd_taken)
    dump = sub.add_parser("dump")
    dump.add_argument("--va", type=hex_int, required=True)
    dump.add_argument("--count", type=int, default=32)
    dump.add_argument("--max-string", type=int, default=40)
    dump.set_defaults(func=cmd_dump)
    small = sub.add_parser("small")
    small.add_argument("--lo", type=hex_int, default=LO)
    small.add_argument("--hi", type=hex_int, default=HI)
    small.add_argument("--min-size", type=int, default=0)
    small.add_argument("--max-size", type=int, default=48)
    small.set_defaults(func=cmd_small)
    dis = sub.add_parser("dis")
    dis.add_argument("--va", type=hex_int, action="append", required=True)
    dis.add_argument("--len", type=int, default=96)
    dis.set_defaults(func=cmd_dis)
    xrefs = sub.add_parser("xrefs")
    xrefs.add_argument("--va", type=hex_int, action="append", required=True)
    xrefs.set_defaults(func=cmd_xrefs)
    callers = sub.add_parser("callers")
    callers.add_argument("--va", type=hex_int, action="append", required=True)
    callers.set_defaults(func=cmd_callers)
    append = sub.add_parser("append")
    append.add_argument("spec", type=Path)
    append.add_argument("--confidence", default="INFERRED")
    append.set_defaults(func=cmd_append)
    args = parser.parse_args()
    print(EVIDENCE_NOTICE, file=sys.stderr)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
