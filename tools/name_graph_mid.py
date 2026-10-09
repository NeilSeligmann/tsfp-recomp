# SPDX-License-Identifier: GPL-3.0-or-later
"""T1261 round 3 (0x00180000-0x00370000): call-graph survey and name merge.

    python -m tools.name_graph_mid survey --min-size 41 --max-size 400
    python -m tools.name_graph_mid dis --va 0x001a0000
    python -m tools.name_graph_mid gaps  # data pointers missing from functions.csv
    python -m tools.name_graph_mid append SPEC          # merge 'va|name|evidence' lines
    python -m tools.name_graph_mid rename SPEC          # edit existing rows 'va|name|evidence'

Read-only except append/rename. Paths are relative to the repository root.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import struct
import sys
from collections import Counter, defaultdict
from collections.abc import Iterator
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from tools.name_candidates import Image, game_vas, load_known_names
from tools.name_tables_mid import DATA_SECTIONS, HI, LO, call_graph, function_sizes
from tools.naming_body import EVIDENCE_NOTICE

DOC = "docs/t1261-naming-round4-mid.md"


def short(name: str) -> str:
    return name[5:] if name.startswith("game_") else name


def summarise(image: Image, entry: int, size: int, known: dict[int, str]) -> dict:
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    calls: list[str] = []
    globs: list[int] = []
    strs: list[str] = []
    kernel: list[str] = []
    body = image.body(md, entry, size)
    for insn in body.instructions:
        if insn.mnemonic in ("call", "jmp") and insn.operands:
            op = insn.operands[0]
            if op.type == X86_OP_IMM:
                tgt = op.imm & 0xFFFFFFFF
                calls.append(known.get(tgt, f"{tgt:x}"))
            elif op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                k = image.kernel_name(op.mem.disp & 0xFFFFFFFF)
                if k:
                    kernel.append(k)
            continue
        for op in insn.operands:
            val = None
            if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                val = op.mem.disp & 0xFFFFFFFF
            elif op.type == X86_OP_IMM and insn.mnemonic in ("push", "mov", "lea"):
                val = op.imm & 0xFFFFFFFF
            if val and val >= 0x400000:
                text = image.cstring(val, 60)
                if text:
                    strs.append(text[:40])
                elif val not in globs:
                    globs.append(val)
    return {
        "calls": calls,
        "globs": globs,
        "strs": list(dict.fromkeys(strs)),
        "kernel": kernel,
        "body_evidence": body.label,
    }


def cmd_survey(args: argparse.Namespace) -> int:
    root = args.root
    image = Image(root / args.xbe, root / "generated/retail/functions.csv")
    known = load_known_names(root)
    game = game_vas(root)
    sizes = function_sizes(root)
    _, callers = call_graph(root, image)
    for entry in sorted(sizes):
        if not (args.lo <= entry < args.hi and entry in game and entry not in known):
            continue
        if not args.min_size <= sizes[entry] <= args.max_size:
            continue
        info = summarise(image, entry, sizes[entry], known)
        named = [c for c in info["calls"] if not all(ch in "0123456789abcdef" for ch in c)]
        if args.named_only and not (named or info["strs"]):
            continue
        cname = [short(known[c]) for c in callers.get(entry, []) if c in known]
        print(
            f"{entry:08x} {sizes[entry]:4d} n{len(callers.get(entry, []))} "
            f"calls={','.join(short(c) for c in info['calls'])[:300]} "
            f"g={','.join(f'{g:x}' for g in info['globs'][:6])} "
            f"s={info['strs'][:2]} k={sorted(set(info['kernel']))} by={cname[:3]} "
            f"evidence={info['body_evidence']}"
        )
    image.report_bodies()
    return 0


def cmd_dis(args: argparse.Namespace) -> int:
    root = args.root
    image = Image(root / args.xbe, root / "generated/retail/functions.csv")
    known = load_known_names(root)
    sizes = function_sizes(root)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    for va in args.va:
        print(f"== {va:08x} size={sizes.get(va, args.len)} {known.get(va, '-')}")
        body = image.body(md, va, sizes.get(va, args.len))
        print(body.label)
        for insn in body.instructions:
            note = ""
            if insn.mnemonic in ("call", "jmp") and insn.op_str.startswith("0x"):
                note = f" ; {known.get(int(insn.op_str, 16), '')}"
            print(f"  {insn.address:08x} {insn.mnemonic} {insn.op_str}{note}")
    image.report_bodies()
    return 0


def cmd_gaps(args: argparse.Namespace) -> int:
    """Data dwords pointing into the code range at addresses that are not listed entries."""
    root = args.root
    image = Image(root / args.xbe, root / "generated/retail/functions.csv")
    sizes = function_sizes(root)
    entries = sorted(sizes)
    listed = set(entries)
    starts = entries
    exact_bounds = [b for b in image.bounds.values() if b.membership == "exact-exported-ranges"]
    hits: dict[int, list[int]] = defaultdict(list)
    for section in image.xbe.sections:
        if section.name not in DATA_SECTIONS:
            continue
        raw = image.data[section.raw_addr : section.raw_addr + section.raw_size]
        for off in range(0, len(raw) - 3, 4):
            (val,) = struct.unpack_from("<I", raw, off)
            if args.lo <= val < args.hi and val not in listed:
                hits[val].append(section.virtual_addr + off)
    for val in sorted(hits):
        idx = bisect.bisect_right(starts, val) - 1
        bounds = image.bounds.get(starts[idx]) if idx >= 0 else None
        exact_owner = next(
            (b for b in exact_bounds if any(lo <= val <= hi for lo, hi in b.ranges)), None
        )
        if exact_owner is not None:
            bounds = exact_owner  # Lower fragments can precede the owning entry.
        inside = (
            bounds is not None
            and not bounds.error
            and bounds.lower <= val < bounds.lower + bounds.span
        )
        # Suppress exact members; legacy fragmented spans cannot identify membership.
        exact_member = bounds is not None and any(lo <= val <= hi for lo, hi in bounds.ranges)
        contiguous = inside and bounds is not None and bounds.byte_count == bounds.span
        if exact_member and not args.include_inside:
            continue
        if contiguous and not args.include_inside:
            continue
        membership = bounds.membership if bounds is not None else "unverified"
        head = image.read(val, 1) or b""
        print(
            f"{val:08x} refs={len(hits[val])} first_ref={hits[val][0]:08x} "
            f"inside_listed_span={'Y' if inside else 'N'} membership={membership} "
            f"first_byte={head.hex()}"
        )
    image.report_bodies()
    return 0


def _load(path: Path) -> tuple[list[str], list[list[str]]]:
    with path.open(newline="") as handle:
        rows = list(csv.reader(handle))
    return rows[0], rows[1:]


def _save(path: Path, header: list[str], body: list[list[str]]) -> None:
    body.sort(key=lambda r: int(r[0], 16))
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(body)


def _specs(path: Path) -> Iterator[tuple[str, int, str, str]]:
    for line in path.read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            va_text, name, evidence = (p.strip() for p in line.split("|", 2))
            yield va_text, int(va_text, 16), name, evidence


def cmd_append(args: argparse.Namespace) -> int:
    root = args.root
    overlay = root / "tools/data/function_names.csv"
    header, body = _load(overlay)
    taken_vas = {int(r[0], 16) for r in body}
    taken_names = {r[1] for r in body}
    functions = set(function_sizes(root))
    game = game_vas(root)
    added = 0
    for va_text, va, name, evidence in _specs(args.spec):
        problems = []
        if not LO <= va < HI:
            problems.append("outside range")
        if va not in functions:
            problems.append("not a function entry")
        elif va not in game:
            problems.append("not game region")
        if va in taken_vas:
            problems.append("VA already named")
        if name in taken_names:
            problems.append("duplicate name")
        if not name.startswith("game_"):
            problems.append("no game_ prefix")
        if not evidence:
            problems.append("empty evidence")
        if problems:
            print(f"SKIP {va_text} {name}: {', '.join(problems)}", file=sys.stderr)
            continue
        taken_vas.add(va)
        taken_names.add(name)
        body.append([f"0x{va:08x}", name, args.confidence, f"{evidence}; {DOC}"])
        added += 1
    _save(overlay, header, body)
    print(f"added {added} rows, overlay now {len(body)}")
    return 0


def cmd_rename(args: argparse.Namespace) -> int:
    overlay = args.root / "tools/data/function_names.csv"
    header, body = _load(overlay)
    by_va = {int(r[0], 16): r for r in body}
    names = Counter(r[1] for r in body)
    done = 0
    for va_text, va, name, evidence in _specs(args.spec):
        row = by_va.get(va)
        if row is None or not name.startswith("game_") or not evidence:
            print(f"SKIP {va_text}: missing row, prefix or evidence", file=sys.stderr)
            continue
        if names[name] and name != row[1]:
            print(f"SKIP {va_text}: duplicate name {name}", file=sys.stderr)
            continue
        names[row[1]] -= 1
        names[name] += 1
        row[1], row[3] = name, f"{evidence}; revised in round 4 (VA kept); {DOC}"
        done += 1
    _save(overlay, header, body)
    print(f"renamed {done}")
    return 0


def hex_int(text: str) -> int:
    return int(text, 16)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    sub = parser.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("survey")
    s.add_argument("--lo", type=hex_int, default=LO)
    s.add_argument("--hi", type=hex_int, default=HI)
    s.add_argument("--min-size", type=int, default=0)
    s.add_argument("--max-size", type=int, default=100000)
    s.add_argument("--named-only", action="store_true")
    s.set_defaults(func=cmd_survey)
    d = sub.add_parser("dis")
    d.add_argument("--va", type=hex_int, action="append", required=True)
    d.add_argument("--len", type=int, default=96)
    d.set_defaults(func=cmd_dis)
    g = sub.add_parser("gaps")
    g.add_argument("--lo", type=hex_int, default=0x00011000)
    g.add_argument("--hi", type=hex_int, default=0x00470000)
    g.add_argument("--include-inside", action="store_true")
    g.set_defaults(func=cmd_gaps)
    for cmd, func in (("append", cmd_append), ("rename", cmd_rename)):
        p = sub.add_parser(cmd)
        p.add_argument("spec", type=Path)
        p.add_argument("--confidence", default="INFERRED")
        p.set_defaults(func=func)
    args = parser.parse_args()
    print(EVIDENCE_NOTICE, file=sys.stderr)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
