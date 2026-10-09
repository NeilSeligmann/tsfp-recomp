# SPDX-License-Identifier: GPL-3.0-or-later
"""Finite original-image audit of named debug/no-op bodies; no dead-code inference."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import struct
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM

from tools.codediff.boundaries import function_table_rows
from tools.orphan_reach_report import CERTIFIED
from tools.xbe import parse_xbe


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    raw = args.xbe.read_bytes()
    if hashlib.sha256(raw).hexdigest() != CERTIFIED:
        raise SystemExit("retail XBE mismatch")
    image = parse_xbe(raw)
    names = {
        int(r["entry_va"], 16): r
        for r in csv.DictReader(Path("tools/data/function_names.csv").open())
    }
    selected = {
        v: r for v, r in names.items() if any(t in r["name"].split("_") for t in ("debug", "noop"))
    }
    functions = function_table_rows(args.generated_dir / "functions.csv")
    cs = Cs(CS_ARCH_X86, CS_MODE_32)
    cs.detail = True
    edges = {v: [] for v in selected}
    pointers = {v: [] for v in selected}
    bodies = {}
    for row in functions:
        start = int(row["entry_va"], 16)
        off = image.va_to_offset(start)
        if off is None:
            continue
        body = raw[off : off + int(row["size_bytes"])]
        insns = list(cs.disasm(body, start))
        if start in selected:
            bodies[start] = "; ".join(f"{i.address:#x}: {i.mnemonic} {i.op_str}" for i in insns)
        for ins in insns:
            for op in ins.operands:
                target = op.imm & 0xFFFFFFFF if op.type == X86_OP_IMM else None
                if target in selected and target != start:
                    edges[target].append(
                        dict(
                            owner=f"{start:#x}",
                            site=f"{ins.address:#x}",
                            instruction=f"{ins.mnemonic} {ins.op_str}",
                        )
                    )
    for section in image.sections:
        if section.name == ".text":
            continue
        for pos in range(0, section.raw_size - 3, 4):
            target = struct.unpack_from("<I", raw, section.raw_addr + pos)[0]
            if target in selected:
                pointers[target].append(f"{section.virtual_addr + pos:#x}")
    args.out.write_text(
        json.dumps(
            dict(
                xbe_sha256=CERTIFIED,
                rows=[
                    dict(
                        va=f"{v:#x}",
                        name=r["name"],
                        naming_evidence=r["evidence"],
                        body=bodies.get(v, "not in function table"),
                        code_references=edges[v],
                        aligned_nontext_references=pointers[v],
                    )
                    for v, r in sorted(selected.items())
                ],
            ),
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
