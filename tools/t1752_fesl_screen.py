"""Reproduce T1752's finite, original-backed screening without gate changes."""

import argparse
import json
from pathlib import Path

from tools.codediff.boundaries import load_function_table
from tools.harness.image import build_guest_image
from tools.harness.selection import select
from tools.replace.audit import audit_all
from tools.replace.manifest import ManifestEntry
from tools.replace.straddle_corpus import data_ranges

ROOTS = (
    0x3B9870,
    0x3BA270,
    0x3BA3C0,
    0x3BB9C0,
    0x3BC270,
    0x3BC2A0,
    0x3BC300,
    0x3BDE60,
    0x3BF2D0,
    0x3C09D0,
    0x3C6730,
    0x3C6890,
    0x3C6900,
    0x3C6950,
    0x3C6B30,
    0x3C6B70,
    0x3C6C30,
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--gen-dir", type=Path, default=Path("generated/lifted/gen"))
    parser.add_argument("--functions", type=Path, default=Path("generated/retail/functions.csv"))
    args = parser.parse_args()
    image = build_guest_image(args.xbe)
    functions = load_function_table(args.functions)
    # This is a prospective exact-output caller screen, not a linked manifest or proof.
    audits = audit_all(
        [ManifestEntry(va, "prospective-exact", "cdecl", 0, "eax", (), "screen") for va in ROOTS],
        args.gen_dir,
        image,
        data_ranges=[(low, high) for _, low, high in data_ranges(args.xbe)],
        functions=args.functions,
    )
    selected = select(
        [(f.entry_va, f.size_bytes) for f in functions if not f.is_thunk],
        image.code_at,
        min_size=1,
        allow_calls=True,
        allow_x87=True,
        allow_inbody_jumps=True,
    )
    skipped = {f.va: f.reason for f in selected.skipped}
    rows = []
    for audit in audits:
        rows.append(
            {
                "va": f"0x{audit.va:08x}",
                "sites": audit.direct_sites,
                "safe": audit.safe,
                "unsafe": audit.unsafe,
                "unresolved": audit.unresolved,
                "eligible": audit.eligible,
                "selection": skipped.get(audit.va, "candidate-not-a-proof"),
                "flags": [
                    {"return_va": f"0x{v.return_va:08x}", "verdict": v.verdict, "reason": v.reason}
                    for v in audit.flags
                    if v.verdict != "safe"
                ],
            }
        )
    print(json.dumps(rows, indent=2))


if __name__ == "__main__":
    main()
