# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the finite T1725 descriptor initializers and draw jump table; never lift code."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import struct
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

from tools.name_candidates import Image

FIELDS = (
    "index",
    "draw_descriptor",
    "emitter_descriptor",
    "init_call",
    "emitter_init_call",
    "draw_kind",
    "blend_selector",
    "draw_flags",
    "shape",
    "texture_id",
    "init",
    "update",
    "draw",
    "primary_draw",
)


def extract(root: Path, xbe: Path) -> str:
    image = Image(xbe)
    assert (
        hashlib.sha256(image.data).hexdigest()
        == "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
    ), "unsupported XBE build (initializer ABI is pinned)"
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    pushes: list[int] = []
    draws, emitters = [], []
    # Fixed straight-line initializer, ends before the next function at 0xab050.
    for ins in md.disasm(image.read(0xA9430, 7194), 0xA9430):
        if ins.mnemonic == "push":
            pushes.append(int(ins.op_str, 0))
        elif ins.mnemonic == "call":
            target = int(ins.op_str, 0)
            if target in (0x1115F0, 0x1113E0):
                n = 8 if target == 0x1115F0 else 7
                args = list(reversed(pushes[-n:]))
                assert len(args) == n
                (draws if n == 8 else emitters).append((ins.address, args))
            pushes.clear()
    assert len(draws) == len(emitters) == 35
    cases = struct.unpack("<7I", image.read(0x1120FC, 28))
    handlers = []
    for case in cases:
        # Kind 1 intentionally targets the shared continuation directly.
        calls = (
            [
                int(i.op_str, 0)
                for i in md.disasm(image.read(case, 12), case)
                if i.mnemonic == "call"
            ]
            if case != 0x1120C3
            else []
        )
        assert len(calls) <= 1
        handlers.append(calls[0] if calls else None)
    assert handlers == [0x1108D0, None, 0x110BD0, 0x10F880, 0x1108D0, 0x10FD70, 0x1101A0]
    with (root / "tools/data/function_names.csv").open() as f:
        names = {int(r["entry_va"], 16): r["name"] for r in csv.DictReader(f)}
    out = io.StringIO()
    writer = csv.DictWriter(out, fieldnames=FIELDS, lineterminator="\n")
    writer.writeheader()
    for index, ((call, d), (ecall, e)) in enumerate(zip(draws, emitters, strict=True)):
        assert d[0] == 0x72F738 + index * 0xCC
        assert e[0] == 0x7313F0 + index * 0x84 and e[1] == d[0]
        kind = d[1]
        assert kind <= 6
        writer.writerow(
            dict(
                index=index,
                draw_descriptor=hex(d[0]),
                emitter_descriptor=hex(e[0]),
                init_call=hex(call),
                emitter_init_call=hex(ecall),
                draw_kind=kind,
                blend_selector=d[2],
                draw_flags=hex(d[3]),
                shape=e[2],
                texture_id=hex(d[5]),
                init=names[0x10C220],
                update=names[0x10D890],
                draw=names[0x111FC0],
                primary_draw=names[handlers[kind]]
                if handlers[kind]
                else "none: kind 1 goes to 0x1120c3 (optional buffer pass remains)",
            )
        )
    return out.getvalue()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    parser.add_argument("--check", type=Path)
    args = parser.parse_args()
    result = extract(Path.cwd(), args.xbe)
    if args.check:
        assert args.check.read_text() == result, (
            "descriptor record differs from original instructions/names"
        )
        print("35 paired descriptors and all seven draw branches verified")
    else:
        print(result, end="")


if __name__ == "__main__":
    main()
