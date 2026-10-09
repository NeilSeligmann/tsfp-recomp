# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce the finite T1734 accessor instruction witnesses (no generated tree)."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

from tools.name_candidates import Image

# Finite reviewed set: widget, scene, controller, effects, and the seven task anchors.
ENTRIES = (
    0x12E60,
    0x58670,
    0x58C50,
    0x58C70,
    0x70F20,
    0x70F50,
    0x71730,
    0x71740,
    0x71C70,
    0x71CD0,
    0x71CF0,
    0x746D0,
    0x74910,
    0x74950,
    0x772E0,
    0x77320,
    0x77360,
    0x94290,
    0x9E720,
    0xA1450,
    0xBE280,
    0xFF1B0,
    0xFF2B0,
    0xFF3D0,
    0xFF3F0,
    0xFF490,
    0x1001D0,
    0x15E830,
    0x183E50,
    0x1B62D0,
    0x1B6310,
    0x1BA130,
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--functions", type=Path, required=True)
    args = parser.parse_args()
    image = Image(args.xbe, args.functions)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    with args.functions.open() as source:
        rows = {int(row["entry_va"], 16): row for row in csv.DictReader(source)}
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(("entry_va", "span_source", "diagnostics", "skipped_bytes", "instructions"))
    for va in ENTRIES:
        body = image.body(md, va, int(rows[va]["size_bytes"]))
        writer.writerow(
            (
                f"0x{va:08x}",
                body.bounds.source,
                "|".join(body.diagnostics),
                body.skipped_bytes,
                " | ".join(
                    f"{ins.address:08x} {ins.mnemonic} {ins.op_str}".rstrip()
                    for ins in body.instructions
                ),
            )
        )


if __name__ == "__main__":
    main()
