"""Finite T1726 original-byte/query screen; emits metadata, never an XBE copy."""

import argparse
import hashlib
import json
from pathlib import Path

import capstone

from tools.codediff.boundaries import load_function_table
from tools.harness.image import build_guest_image
from tools.harness.selection import select
from tools.replace.audit import audit_all
from tools.replace.manifest import ManifestEntry
from tools.replace.straddle_corpus import data_ranges

ROOTS = (
    0x47030,
    0x47B30,
    0x47BD0,
    0x49200,
    0x4B310,
    0x58760,
    0x87630,
    0x88C60,
    0x913F0,
    0x91EC0,
    0x92360,
    0xC4DF0,
    0x128C60,
    0x165690,
    0x165760,
    0x1665A0,
    0x1665F0,
    0x167880,
    0x16C170,
    0x170500,
    0x170690,
    0x171560,
    0x171FB0,
    0x17F2F0,
    0x1806B0,
    0x2495B0,
)
LEAVES = {0x47030: 8, 0x47B30: 4, 0x47BD0: 2, 0x49200: 6}


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--xbe", type=Path, required=True)
    p.add_argument("--functions", type=Path, default=Path("generated/retail/functions.csv"))
    p.add_argument("--gen-dir", type=Path, default=Path("generated/lifted/gen"))
    a = p.parse_args()
    image = build_guest_image(a.xbe)
    functions = {f.entry_va: f for f in load_function_table(a.functions)}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    rows = []
    for va in ROOTS:
        size = functions[va].size_bytes
        code = image.code_at(va, size)
        instructions = list(md.disasm(code, va))
        rows.append(
            {
                "va": f"0x{va:08x}",
                "exported_size": size,
                "sha256": hashlib.sha256(code).hexdigest(),
                "decoded_size": sum(i.size for i in instructions),
                "final": (
                    f"{instructions[-1].address:#x}: "
                    f"{instructions[-1].mnemonic} {instructions[-1].op_str}"
                ),
                "calls": [
                    {"site": f"{i.address:#x}", "target": i.op_str}
                    for i in instructions
                    if i.mnemonic == "call"
                ],
                "globals": sorted(
                    {
                        hex(op.mem.disp)
                        for i in instructions
                        for op in i.operands
                        if op.type == capstone.x86_const.X86_OP_MEM
                        and not op.mem.base
                        and 0x74C414 <= op.mem.disp <= 0x74C714
                    }
                ),
            }
        )
    modes = {}
    for mode in ("default", "fp-scalar-v1"):
        selection = select(
            [(v, functions[v].size_bytes) for v in LEAVES],
            image.code_at,
            min_size=1,
            allow_x87=True,
            allow_inbody_jumps=True,
            vector_entries=frozenset(LEAVES) if mode != "default" else frozenset(),
            vector_mode=mode if mode != "default" else "",
        )
        modes[mode] = {
            "candidates": [hex(c.va) for c in selection.candidates],
            "skipped": [{"va": hex(s.va), "reason": s.reason} for s in selection.skipped],
        }
    audits = audit_all(
        [
            ManifestEntry(v, "prospective-exact", "cdecl", n, "eax", (), "screen")
            for v, n in LEAVES.items()
        ],
        a.gen_dir,
        image,
        data_ranges=[(lo, hi) for _, lo, hi in data_ranges(a.xbe)],
        functions=a.functions,
    )
    # World export stops before its real epilogue: metadata witness, no span edit.
    tail = [
        {"site": hex(i.address), "instruction": f"{i.mnemonic} {i.op_str}"}
        for i in md.disasm(image.code_at(0x17214F, 18), 0x17214F)
    ]
    print(
        json.dumps(
            {
                "xbe_sha256": hashlib.sha256(a.xbe.read_bytes()).hexdigest(),
                "functions_sha256": hashlib.sha256(a.functions.read_bytes()).hexdigest(),
                "capstone": capstone.__version__,
                "roots": rows,
                "selection": modes,
                "prospective_exact_audit": [
                    {
                        "va": hex(x.va),
                        "sites": x.direct_sites,
                        "safe": x.safe,
                        "unsafe": x.unsafe,
                        "unresolved": x.unresolved,
                        "eligible": x.eligible,
                    }
                    for x in audits
                ],
                "world_tail": tail,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
