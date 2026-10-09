# SPDX-License-Identifier: GPL-3.0-or-later
"""Table-role evidence for the T1266 function-table additions, and the overlay merge (T1472).

    python -m tools.name_additions scan --out tmp/t1472/scan.json
    python -m tools.name_additions table 0x004766f0 [...]
    python -m tools.name_additions show 0x0002ffa0 [...]
    python -m tools.name_additions count [--names tools/data/function_names.csv]
    python -m tools.name_additions merge SPEC [--confidence INFERRED]

`scan` loads the shared function table (tools.codediff.boundaries.load_function_table:
export + overrides + additions), classifies each entry game or library with the
tools.coverage rules, and for every addition collects: the data dwords that point at it
(table run, slot index, neighbours), code references (call/jmp/push/mov immediates and
table-base displacements), and caller/callee names. `merge` appends `va|name|evidence`
lines to tools/data/function_names.csv applying the tools.coverage loader rules (valid
VA in the table, game region, unique name and VA, identifier, evidence present).
Read-only on the XBE. Repo-relative paths.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from tools.codediff.boundaries import load_function_table
from tools.coverage import (
    NAME_OVERLAY_COLUMNS,
    is_placeholder_name,
    read_flirt_addresses,
    text_range,
)
from tools.xbe.parser import parse_xbe

DATA_SECTIONS = (".rdata", ".data", ".data1")


class World:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.data = (root / "build/default.xbe").read_bytes()
        self.xbe = parse_xbe(self.data)
        self.text_lo, self.text_hi = text_range(self.xbe)
        xtlid = {e.address for e in self.xbe.xtlid}
        flirt = read_flirt_addresses(root / "generated/retail/flirt_names.csv")
        self.table = load_function_table(
            root / "generated/retail/functions.csv",
            root / "tools/data/function_overrides.csv",
            root / "tools/data/function_additions.csv",
        )
        self.size = {f.entry_va: f.size_bytes for f in self.table}
        self.names: dict[int, str] = {}
        for f in self.table:
            if not is_placeholder_name(f.name):
                self.names[f.entry_va] = f.name
        with (root / "generated/retail/flirt_names.csv").open(newline="") as h:
            for r in csv.DictReader(h):
                self.names.setdefault(int(r["entry_va"], 16), r["proposed_name"])
        with (root / "tools/data/function_names.csv").open(newline="") as h:
            for r in csv.DictReader(h):
                self.names[int(r["entry_va"], 16)] = r["name"]
        self.library: set[int] = set()
        for f in self.table:
            e = f.entry_va
            if (
                not (self.text_lo <= e < self.text_hi)
                or e in xtlid
                or e in flirt
                or not is_placeholder_name(f.name)
            ):
                self.library.add(e)
        self.entries = sorted(self.size)
        with (root / "tools/data/function_additions.csv").open(newline="") as h:
            self.additions = [int(r["entry_va"], 16) for r in csv.DictReader(h)]

    def read(self, va: int, size: int) -> bytes:
        off = self.xbe.va_to_offset(va)
        return self.data[off : off + size] if off is not None else b""

    def owner(self, va: int) -> int | None:
        i = bisect.bisect_right(self.entries, va) - 1
        if i < 0:
            return None
        e = self.entries[i]
        return e if va < e + max(self.size[e], 1) else None

    def label(self, va: int) -> str:
        n = self.names.get(va)
        return n if n else f"FUN_{va:08x}"


def data_runs(world: World) -> list[tuple[int, list[int]]]:
    """Runs of consecutive dwords that are function entries (>= 1 member)."""
    funcs = set(world.size)
    runs: list[tuple[int, list[int]]] = []
    for name in DATA_SECTIONS:
        sec = world.xbe.section_by_name(name)
        if sec is None:
            continue
        blob = world.read(sec.virtual_addr, sec.raw_size)
        count = len(blob) // 4
        words = struct.unpack_from(f"<{count}I", blob)
        i = 0
        while i < count:
            if words[i] in funcs:
                j = i
                while j < count and words[j] in funcs:
                    j += 1
                runs.append((sec.virtual_addr + 4 * i, list(words[i:j])))
                i = j
            else:
                i += 1
    return runs


def scan(world: World) -> dict[str, object]:
    adds = set(world.additions)
    runs = data_runs(world)
    slots: dict[int, list[dict[str, object]]] = defaultdict(list)
    for base, members in runs:
        for idx, m in enumerate(members):
            if m in adds:
                slots[m].append({"table": base, "slot": idx, "len": len(members)})
    # data dwords outside runs (struct fields): single-pointer refs
    # (run of length 1 already covered above)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    calls: dict[int, set[int]] = defaultdict(set)
    callees: dict[int, set[int]] = defaultdict(set)
    imm_refs: dict[int, set[int]] = defaultdict(set)
    table_users: dict[int, set[int]] = defaultdict(set)
    spans = [(b, b + 4 * len(m)) for b, m in runs]
    span_starts = [s for s, _ in spans]
    for f in world.table:
        e = f.entry_va
        code = world.read(e, max(f.size_bytes, 1))
        for insn in md.disasm(code, e):
            for op in insn.operands:
                val = None
                if op.type == X86_OP_IMM:
                    val = op.imm & 0xFFFFFFFF
                    if insn.mnemonic in ("call", "jmp"):
                        if val in world.size and val != e:
                            calls[val].add(e)
                            callees[e].add(val)
                        continue
                elif op.type == X86_OP_MEM and op.mem.base == 0:
                    val = op.mem.disp & 0xFFFFFFFF
                if val is None:
                    continue
                if val in adds:
                    imm_refs[val].add(e)
                k = bisect.bisect_right(span_starts, val) - 1
                if k >= 0 and spans[k][0] <= val < spans[k][1]:
                    table_users[spans[k][0]].add(e)
    out: dict[str, object] = {
        "runs": {f"{b:08x}": m for b, m in runs},
        "slots": {f"{k:08x}": v for k, v in slots.items()},
        "callers": {f"{k:08x}": sorted(v) for k, v in calls.items()},
        "callees": {f"{k:08x}": sorted(v) for k, v in callees.items()},
        "imm_refs": {f"{k:08x}": sorted(v) for k, v in imm_refs.items()},
        "table_users": {f"{k:08x}": sorted(v) for k, v in table_users.items()},
    }
    return out


def cmd_scan(args: argparse.Namespace) -> int:
    world = World(Path("."))
    data = scan(world)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(data))
    game = [a for a in world.additions if a not in world.library]
    print(
        f"additions {len(world.additions)} game {len(game)} "
        f"library {len(world.additions) - len(game)}"
    )
    return 0


def cmd_merge(args: argparse.Namespace) -> int:
    root = Path(".")
    world = World(root)
    overlay = root / "tools/data/function_names.csv"
    with overlay.open(newline="") as h:
        rows = list(csv.reader(h))
    header, body = rows[0], rows[1:]
    assert tuple(header) == NAME_OVERLAY_COLUMNS
    vas = {int(r[0], 16) for r in body}
    names = {r[1] for r in body} | {n for n in world.names.values()}
    added = 0
    for line in args.spec.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        va_text, name, evidence = (p.strip() for p in line.split("|", 2))
        va = int(va_text, 16)
        problems = []
        if va not in world.size:
            problems.append("not in function table")
        elif va in world.library:
            problems.append("library region")
        if va in vas:
            problems.append("VA already named")
        if name in names:
            problems.append("duplicate name")
        if not name.startswith("game_") or not name.replace("_", "").isalnum():
            problems.append("bad identifier")
        if not evidence:
            problems.append("empty evidence")
        if problems:
            print(f"SKIP {va_text} {name}: {', '.join(problems)}", file=sys.stderr)
            continue
        vas.add(va)
        names.add(name)
        body.append([f"0x{va:08x}", name, args.confidence, f"{evidence}{args.evidence_suffix}"])
        added += 1
    body.sort(key=lambda r: int(r[0], 16))
    with overlay.open("w", newline="") as h:
        w = csv.writer(h, lineterminator="\n")
        w.writerow(header)
        w.writerows(body)
    print(f"added {added} rows, overlay now {len(body)}")
    return 0


def cmd_table(args: argparse.Namespace) -> int:
    """Dump the data run that starts at (or contains) each address: slot, entry, name."""
    world = World(Path("."))
    adds = set(world.additions)
    runs = data_runs(world)
    for text in args.addresses:
        want = int(text, 16)
        for base, members in runs:
            if base <= want < base + 4 * len(members):
                slot_of = (want - base) // 4
                print(f"run 0x{base:08x} slots {len(members)} (0x{want:08x} is slot {slot_of})")
                for slot, member in enumerate(members):
                    mark = "ADD" if member in adds else "   "
                    size = world.size[member]
                    print(f"  [{slot}] {member:08x} {mark} size={size} {world.label(member)}")
    return 0


def cmd_show(args: argparse.Namespace) -> int:
    """Disassemble functions with call and jump targets replaced by their names."""
    world = World(Path("."))
    cs = Cs(CS_ARCH_X86, CS_MODE_32)
    for text in args.addresses:
        va = int(text, 16)
        print(f"== {va:08x} {world.label(va)} size={world.size.get(va)}")
        code = world.read(va, min(world.size.get(va, 64), args.max_bytes))
        for insn in cs.disasm(code, va):
            ops = insn.op_str
            if insn.mnemonic in ("call", "jmp") and ops.startswith("0x"):
                target = int(ops, 16)
                if target in world.size:
                    ops = f"{ops} <{world.label(target)}>"
            print(f"  {insn.address:08x} {insn.mnemonic} {ops}")
    return 0


def cmd_count(args: argparse.Namespace) -> int:
    """Game-named count through the tools.coverage loader (overlay applied to the table)."""
    from tools.coverage import (
        apply_name_overlay,
        class_overrides_for_root,
        classify_functions,
        load_function_names,
        naming_split,
    )

    root = Path(".")
    world = World(root)
    flirt = read_flirt_addresses(root / "generated/retail/flirt_names.csv")
    xtlid = frozenset(e.address for e in world.xbe.xtlid)
    classified = classify_functions(
        world.table,
        text_lo=world.text_lo,
        text_hi=world.text_hi,
        xtlid_addresses=xtlid,
        flirt_addresses=flirt,
        class_overrides=class_overrides_for_root(root, world.table),
    )
    overlay = load_function_names(args.names, frozenset(world.size))
    applied = apply_name_overlay(classified, overlay)
    split = naming_split(applied.classified)
    print(
        f"game named {split.game_named} / {split.game_total}, library named "
        f"{split.library_named} / {split.library_total}, overlay rows {len(overlay.rows)}, "
        f"rejections {len(overlay.rejections)}, ignored_library {len(applied.ignored_library)}, "
        f"collisions {len(applied.collisions)}"
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    count = sub.add_parser("count")
    count.add_argument("--names", type=Path, default=Path("tools/data/function_names.csv"))
    count.set_defaults(func=cmd_count)
    t = sub.add_parser("table")
    t.add_argument("addresses", nargs="+")
    t.set_defaults(func=cmd_table)
    sh = sub.add_parser("show")
    sh.add_argument("addresses", nargs="+")
    sh.add_argument("--max-bytes", type=int, default=400)
    sh.set_defaults(func=cmd_show)
    p = sub.add_parser("scan")
    p.add_argument("--out", type=Path, default=Path("tmp/t1472/scan.json"))
    p.set_defaults(func=cmd_scan)
    m = sub.add_parser("merge")
    m.add_argument("spec", type=Path)
    m.add_argument("--confidence", default="INFERRED")
    m.add_argument("--evidence-suffix", default="; docs/t1472-function-addition-names.md")
    m.set_defaults(func=cmd_merge)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
