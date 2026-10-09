# SPDX-License-Identifier: GPL-3.0-or-later
"""T1644 structural EH owner audit of the private original image. Run from repo root."""

import argparse
import csv
import json
import struct
from collections.abc import Iterator
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

from tools.codediff.boundaries import function_table_rows
from tools.xbe.parser import parse_xbe

p = argparse.ArgumentParser(
    description="Audit original-XBE EH funclet ownership; no SDK libraries used."
)
p.add_argument("--xbe", type=Path, required=True)
p.add_argument("--out", type=Path, required=True)
p.add_argument("--check", action="store_true", help="Fail on missing or nonunique ownership chains")
args = p.parse_args()
b = args.xbe.read_bytes()
x = parse_xbe(b)
cs = Cs(CS_ARCH_X86, CS_MODE_32)


def read(v: int, n: int) -> bytes:
    o = x.va_to_offset(v)
    return b[o : o + n] if o is not None else b""


def u(v: int) -> int:
    return struct.unpack("<I", read(v, 4))[0]


def hits(p: bytes) -> Iterator[int]:
    for s in x.sections:
        blob = b[s.raw_addr : s.raw_addr + s.raw_size]
        pos = blob.find(p)
        while pos >= 0:
            yield s.virtual_addr + pos
            pos = blob.find(p, pos + 1)


funcs = {
    int(r["entry_va"], 16): r for r in function_table_rows(Path("generated/retail/functions.csv"))
}
names = {
    int(r["entry_va"], 16): r["name"] for r in csv.DictReader(open("tools/data/function_names.csv"))
}
infos = []
for fi in hits(struct.pack("<I", 0x19930520)):
    try:
        n = u(fi + 4)
        um = u(fi + 8)
        if not 0 < n < 300:
            continue
        actions = [
            (i, struct.unpack("<i", read(um + i * 8, 4))[0], u(um + i * 8 + 4)) for i in range(n)
        ]
        if not any(0x3D1920 <= a <= 0x3D1FBF for i, t, a in actions):
            continue
        handlers = list(hits(b"\xb8" + struct.pack("<I", fi) + b"\xe9"))
        owners = []
        for h in handlers:
            if h + 10 + struct.unpack("<i", read(h + 6, 4))[0] != 0x3C90D7:
                continue
            for ref in hits(struct.pack("<I", h)):
                if read(ref - 1, 1) == b"\xb8" and read(ref + 4, 1) == b"\xe8":
                    target = ref + 9 + struct.unpack("<i", read(ref + 5, 4))[0]
                    if target == 0x3CAAB4:
                        owners.append(
                            {
                                "handler": h,
                                "owner": ref - 1,
                                "form": "eax-frame-helper",
                                "section": next(
                                    s.name
                                    for s in x.sections
                                    if s.virtual_addr <= ref < s.virtual_addr + s.virtual_size
                                ),
                            }
                        )
                elif read(ref - 1, 1) == b"\x68":
                    for delta in range(1, 25):
                        v = ref - 1 - delta
                        if (
                            read(v, 6) == b"\x64\xa1\x00\x00\x00\x00"
                            and read(v + 6, 2) == b"\x6a\xff"
                            and ref - 1 == v + 8
                            and read(ref + 4, 8) == b"\x50\x64\x89\x25\x00\x00\x00\x00"
                        ):
                            owners.append(
                                {
                                    "handler": h,
                                    "owner": v,
                                    "form": "inline-fs-registration",
                                    "section": next(
                                        s.name
                                        for s in x.sections
                                        if s.virtual_addr <= ref < s.virtual_addr + s.virtual_size
                                    ),
                                }
                            )
        infos.append({"fi": fi, "map": um, "owners": owners, "actions": actions})
    except (struct.error, TypeError):
        pass
out = []
for v, r in sorted(funcs.items()):
    if not 0x3D1920 <= v <= 0x3D1FBF:
        continue
    ins = list(cs.disasm(read(v, min(int(r["size_bytes"]), 256)), v))
    text = []
    for i in ins:
        text.append(i.mnemonic + " " + i.op_str)
        if i.mnemonic in ("jmp", "ret"):
            break
    out.append(
        {
            "va": v,
            "name": names.get(v),
            "base_name": r["name"],
            "body": "; ".join(text),
            "chains": [
                dict(fi=f["fi"], map=f["map"], owners=f["owners"], state=i, to=t)
                for f in infos
                for i, t, a in f["actions"]
                if a == v
            ],
        }
    )
args.out.write_text(json.dumps(out, indent=2))
print(
    "rows",
    len(out),
    "no chains",
    sum(not r["chains"] for r in out),
    "no owners",
    sum(not c["owners"] for r in out for c in r["chains"]),
)
print(
    "owners",
    sorted(
        {(hex(o["owner"]), o["section"]) for r in out for c in r["chains"] for o in c["owners"]}
    ),
)

if args.check and (
    not out or any(len(r["chains"]) != 1 or not r["chains"][0]["owners"] for r in out)
):
    raise SystemExit("Incomplete or ambiguous funclet ownership")
