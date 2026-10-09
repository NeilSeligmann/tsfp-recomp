# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce the bounded T1746 tables; does not generate lifted code."""

import argparse
import csv
import hashlib
import io
import struct
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM

from tools.name_candidates import Image


def render(fields: list[str], rows: list[dict[str, str | int]]) -> str:
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    return stream.getvalue()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--functions", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    image = Image(args.xbe, args.functions)
    names = list(csv.DictReader((root / "tools/data/function_names.csv").open()))
    family = {int(r["entry_va"], 16): r for r in names if r["name"].startswith("game_table_737bd4")}
    assert len(family) == 88, "Review scope if the historical family changes"
    sizes = {
        int(r["entry_va"], 16): int(r["size_bytes"]) for r in csv.DictReader(args.functions.open())
    }
    for r in csv.DictReader((root / "tools/data/function_additions.csv").open()):
        sizes.setdefault(int(r["entry_va"], 16), int(r["size_bytes"]))

    def u32(va: int) -> int:
        return struct.unpack("<I", image.read(va, 4))[0]

    def hx(va: int) -> str:
        return f"0x{va:08x}" if va else "0"

    modes = []
    owners = {t: [] for t in range(54)}
    for record in range(66):
        base = 0x4EEF74 + record * 0x29C
        for mode in range(2):
            block = base + 0x68 + mode * 0x7C
            flags, raw_type = u32(block), u32(block + 4)
            typ = raw_type if raw_type != 0xFFFFFFFF else -1
            active = not bool(flags & 0x80000000)
            assert not active or typ == -1 or typ in owners
            if active and typ >= 0:
                owners[typ].append(f"R{record}:M{mode}")
            modes.append(
                dict(
                    record=record,
                    mode=mode,
                    block_va=hx(block),
                    flags=hx(flags),
                    active=int(active),
                    projectile_type=typ,
                    record_callback=hx(u32(base + 0x294)),
                )
            )
    types, slots = [], {}
    for typ in range(54):
        base = 0x4DEB54 + typ * 0x40
        callbacks = {
            slot: u32(base + offset)
            for slot, offset in [("init", 4), ("update", 8), ("aux", 12), ("draw", 16)]
        }
        for slot, va in callbacks.items():
            if va:
                slots.setdefault(va, []).append(f"T0x{typ:02x}:{slot}")
        types.append(
            dict(
                type=f"0x{typ:02x}",
                flags=hx(u32(base)),
                descriptor_va=hx(base),
                **{k: hx(v) for k, v in callbacks.items()},
                record_modes=";".join(owners[typ]),
                confidence="MEASURED bytes; INFERRED semantics",
            )
        )
    # Named-family edges only. They express possible shared-helper reachability,
    # never an assertion that a branch executed in an archived census.
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    edges, diagnostics = {}, {}
    for va in family.keys() | slots.keys():
        if va not in sizes:
            diagnostics[va] = "No exported or addition body bound; edges unresolved"
            edges[va] = set()
            continue
        body = image.body(md, va, sizes[va])
        diagnostics[va] = ";".join(str(d) for d in body.diagnostics)
        edges[va] = set()
        for insn in body.instructions:
            if (
                insn.mnemonic in ("call", "jmp")
                and insn.operands
                and insn.operands[0].type == X86_OP_IMM
            ):
                target = insn.operands[0].imm & 0xFFFFFFFF
                if target in family and target != va:
                    edges[va].add(target)
    reach = {va: set() for va in family}
    for entry, type_slots in slots.items():
        seen, todo = set(), [entry]
        while todo:
            va = todo.pop()
            if va in seen:
                continue
            seen.add(va)
            if va in reach:
                reach[va].update(type_slots)
            todo.extend(edges.get(va, ()))
    members = []
    for va, r in sorted(family.items()):
        direct = slots.get(va, [])
        callers = sorted(src for src, targets in edges.items() if va in targets)
        members.append(
            dict(
                entry_va=hx(va),
                name=r["name"],
                direct_type_slots=";".join(direct),
                possible_type_roots=";".join(sorted(reach[va])),
                direct_callers=";".join(map(hx, callers)),
                attribution="descriptor callback"
                if direct
                else "shared helper path"
                if reach[va]
                else "pool infrastructure or unresolved",
                confidence=r["confidence"],
                body_diagnostics=diagnostics[va],
                evidence=r["evidence"],
            )
        )
    outputs = {
        "t1746-projectile-types.csv": render(list(types[0]), types),
        "t1746-record-projectiles.csv": render(list(modes[0]), modes),
        "t1746-projectile-functions.csv": render(list(members[0]), members),
    }
    for name, content in outputs.items():
        path = root / "docs/data" / name
        if args.check:
            assert path.read_text() == content, f"{name} differs"
        else:
            path.write_text(content)
    print(
        "88 family functions; 54 types; 132 record modes; "
        f"{sum(bool(r['direct_type_slots']) for r in members)} direct callback members"
    )
    print("XBE sha256=" + hashlib.sha256(image.data).hexdigest())


if __name__ == "__main__":
    main()
