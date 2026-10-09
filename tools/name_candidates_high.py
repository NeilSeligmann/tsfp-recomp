# SPDX-License-Identifier: GPL-3.0-or-later
"""T1261: list candidate game functions in a VA range with their naming evidence.

For each function in generated/retail/functions.csv inside the range (default
0x00240000-0x00470000, game code only) disassemble the body with capstone and report:
strings referenced (immediates pointing at printable C strings in the XBE), kernel
imports called (thunk slot -> ordinal -> name via tools/kernel_ordinals.py), direct
callees that already carry a real name, and the size. Read-only; never prints owner
files beyond short strings that the function itself references.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import capstone

from tools.coverage import (
    DEFAULT_CLASS_OVERRIDES,
    REGION_GAME,
    REGION_LIBRARY,
    is_placeholder_name,
    load_class_overrides,
    read_flirt_addresses,
)
from tools.kernel_ordinals import KERNEL_ORDINALS
from tools.name_candidates import Image
from tools.naming_body import EVIDENCE_NOTICE
from tools.xbe.model import Xbe


def read_cstring(data: bytes, xbe: Xbe, va: int, limit: int = 80) -> str | None:
    for sec in xbe.sections:
        if sec.virtual_addr <= va < sec.virtual_addr + sec.virtual_size:
            off = sec.raw_addr + (va - sec.virtual_addr)
            raw = data[off : off + limit]
            end = raw.find(b"\0")
            if end < 4:
                return None
            text = raw[:end]
            if all(32 <= b < 127 or b in (9, 10) for b in text):
                return text.decode("ascii")
            return None
    return None


def va_bytes(data: bytes, xbe: Xbe, va: int, size: int) -> bytes:
    for sec in xbe.sections:
        if sec.virtual_addr <= va < sec.virtual_addr + sec.virtual_size:
            off = sec.raw_addr + (va - sec.virtual_addr)
            return data[off : off + size]
    return b""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    ap.add_argument("--functions", type=Path, default=Path("generated/retail/functions.csv"))
    ap.add_argument("--names", type=Path, default=Path("tools/data/function_names.csv"))
    ap.add_argument("--flirt", type=Path, default=Path("generated/retail/flirt_names.csv"))
    ap.add_argument("--lo", type=lambda s: int(s, 0), default=0x240000)
    ap.add_argument("--hi", type=lambda s: int(s, 0), default=0x470000)
    ap.add_argument("--max-size", type=int, default=100000)
    ap.add_argument(
        "--only-evidence",
        action="store_true",
        help="skip functions with no string/import/named-callee",
    )
    args = ap.parse_args()

    image = Image(args.xbe, args.functions)
    print(EVIDENCE_NOTICE, file=sys.stderr)
    data = image.data
    xbe = image.xbe
    slots = {xbe.kernel_thunk_addr + 4 * i: o for i, o in enumerate(xbe.kernel_import_ordinals)}
    funcs = []
    with args.functions.open() as handle:
        for row in csv.DictReader(handle):
            funcs.append((int(row["entry_va"], 16), int(row["size_bytes"]), row["name"]))
    names = {va: n for va, _, n in funcs if not is_placeholder_name(n)}
    library = set(names) | {entry.address for entry in xbe.xtlid}
    library |= read_flirt_addresses(args.flirt)
    overrides_path = Path(".") / DEFAULT_CLASS_OVERRIDES
    overrides = (
        load_class_overrides(overrides_path, frozenset(va for va, _, _ in funcs))
        if overrides_path.is_file()
        else {}
    )
    for va, region in overrides.items():
        if region == REGION_GAME:
            library.discard(va)
        elif region == REGION_LIBRARY:
            library.add(va)
    with args.names.open() as handle:
        for row in csv.DictReader(handle):
            names[int(row["entry_va"], 16)] = row["name"]
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    cs.detail = False
    for va, size, name in funcs:
        if not (args.lo <= va < args.hi) or size > args.max_size or va in library:
            continue
        strings, apis, callees = [], [], []
        body = image.body(cs, va, size)
        for ins in body.instructions:
            for tok in ins.op_str.replace("[", " ").replace("]", " ").replace(",", " ").split():
                if not tok.startswith("0x"):
                    continue
                try:
                    imm = int(tok, 16)
                except ValueError:
                    continue
                if imm in slots:
                    api = KERNEL_ORDINALS.get(slots[imm], f"ord{slots[imm]}")
                    if api not in apis:
                        apis.append(api)
                elif ins.mnemonic == "call" and imm in names and imm != va:
                    if names[imm] not in callees:
                        callees.append(names[imm])
                elif imm >= 0x370000 or imm >= 0x1000:
                    text = read_cstring(data, xbe, imm)
                    if text and imm not in names and text not in strings:
                        strings.append(text)
        if args.only_evidence and not (strings or apis or callees):
            continue
        print(
            f"{va:#010x} {size:5d} {name} evidence={body.label} "
            f"apis={apis} callees={callees} strings={strings}"
        )
    image.report_bodies()
    return 0


if __name__ == "__main__":
    sys.exit(main())
