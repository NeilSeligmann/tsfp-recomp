# SPDX-License-Identifier: GPL-3.0-or-later
"""T1728 actual-original bounded controls; no callee stubs or native verdicts."""

import argparse
import hashlib
import struct
from pathlib import Path

from unicorn import UC_ARCH_X86, UC_MODE_32, Uc
from unicorn.x86_const import UC_X86_REG_ESP

from tools.name_candidates import Image

IMAGE_SHA = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
BODIES = {
    0x1C8540: (1111, "953e9a52e2592363cdf1403441f6af12ee5bd42bbf5231a42a5a7602bd362262"),
    0x1C9610: (21, "78153ad03f595b55b13a31853805e636d3df608a3f0373f8a279ce46f9328ca6"),
    0x5A360: (591, "2919caf8b47fef41281f7e1bf43c2d4fa7fdb289153b24465a4a02edfb00b832"),
}


def run(image_path: Path) -> None:
    assert hashlib.sha256(image_path.read_bytes()).hexdigest() == IMAGE_SHA
    image = Image(image_path)
    for va, (size, digest) in BODIES.items():
        actual = hashlib.sha256(image.read(va, size)).hexdigest()
        if digest:
            assert actual == digest
        print(f"0x{va:08x} span={size} sha256={actual}")

    def fresh() -> Uc:
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc.mem_map(0x10000, 0xA00000)
        for va, (size, _) in BODIES.items():
            uc.mem_write(va, image.read(va, size))
        return uc

    def word(uc: Uc, address: int, value: int) -> None:
        uc.mem_write(address, struct.pack("<I", value))

    uc = fresh()
    word(uc, 0x79094C, 0x66)
    word(uc, 0x900000, 0xA00000)
    uc.reg_write(UC_X86_REG_ESP, 0x900000)
    uc.emu_start(0x1C8540, 0xA00000, count=1000)
    assert uc.reg_read(UC_X86_REG_ESP) == 0x900004
    print("PASS opener mode102 early return, no loader executed")

    uc = fresh()
    word(uc, 0x900000, 0xA00000)
    uc.reg_write(UC_X86_REG_ESP, 0x900000)
    uc.emu_start(0x1C9610, 0x1C961C, count=100)
    assert bytes(uc.mem_read(0x8FFFF4, 12)) == struct.pack("<III", 0x49C690, 0x48C198, 1)
    print("PASS set-loader prefix arguments; stopped BEFORE original callee")

    for second_flags in (0, 0xFFFFFFFF):
        uc = fresh()
        # Entry args: clip id, transition scalar, track scalar, sample scale,
        # controller, previous record, current record.
        uc.mem_write(
            0x900000,
            struct.pack(
                "<8I", 0xA00000, 1, 0x3F800000, 0, 0x3F800000, 0x805000, 0x803000, 0x802000
            ),
        )
        uc.reg_write(UC_X86_REG_ESP, 0x900000)
        uc.mem_write(0x7E03C2, struct.pack("<h", 2))
        word(uc, 0x802000, 3)
        word(uc, 0x7DE660 + 16, 0x201)
        word(uc, 0x7DE664 + 16, second_flags)
        before = bytes(uc.mem_read(0x802000, 0x100))
        uc.emu_start(0x5A360, 0xA00000, count=1000)
        assert uc.reg_read(UC_X86_REG_ESP) == 0x900004
        assert bytes(uc.mem_read(0x802000, 0x100)) == before
        assert bytes(uc.mem_read(0x803000, 0x100)) == bytes(0x100)
    print("PASS two track-switch veto returns; no decode/interpolation executed")
    print("4 bounded original controls; native proofs NOT RUN")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, required=True)
    run(parser.parse_args().xbe)
