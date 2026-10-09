# SPDX-License-Identifier: GPL-3.0-or-later
"""Original saved-EDI alias controls; no campaign proof tuple is run."""

import json
import struct
import subprocess
from pathlib import Path

from unicorn import UC_ARCH_X86, UC_MODE_32, Uc
from unicorn.x86_const import (
    UC_X86_REG_EAX,
    UC_X86_REG_EBP,
    UC_X86_REG_EBX,
    UC_X86_REG_ECX,
    UC_X86_REG_EDI,
    UC_X86_REG_EDX,
    UC_X86_REG_EFLAGS,
    UC_X86_REG_ESI,
    UC_X86_REG_ESP,
)

from tools.harness.image import build_guest_image

image = build_guest_image(Path("build/default.xbe"))
expected = []
for df in (0, 1):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(image.base, len(image.data))
    uc.mem_write(image.base, image.data)
    for address, value in ((0x200008, 0x200000), (0x20003C, 0x300000), (0x300000, 0xBAD)):
        uc.mem_write(address, struct.pack("<I", value))
    for register, value in (
        (UC_X86_REG_EAX, 0x11223344),
        (UC_X86_REG_ECX, 0x55667788),
        (UC_X86_REG_EDI, 0x87654321),
        (UC_X86_REG_ESP, 0x200004),
        (UC_X86_REG_EFLAGS, 0x202 | (df << 10)),
    ):
        uc.reg_write(register, value)
    # Stop immediately before RET: DF=0 clearing also aliases the return slot.
    # Native adapter adds its documented four-byte return pop, without fetching RET.
    uc.emu_start(0x698D0, 0x698F5, count=1000)
    row = [
        df,
        uc.reg_read(UC_X86_REG_EAX),
        uc.reg_read(UC_X86_REG_ECX),
        uc.reg_read(UC_X86_REG_EDI),
        uc.reg_read(UC_X86_REG_ESP) + 4,
        struct.unpack("<I", uc.mem_read(0x300000, 4))[0],
    ]
    assert row == [df, 0, 0, 0, 0x200008, 0]
    expected.append(row)
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(image.base, len(image.data))
uc.mem_write(image.base, image.data)
for address, value in (
    (0x200008, 0x400000),
    (0x400000, 0x500000),
    (0x400014, 1),
    (0x40002C, 100),
    (0x5000B8, 0x1FFFF0),
    (0x1FFFF0, 4),
):
    uc.mem_write(address, struct.pack("<I", value))
for register, value in (
    (UC_X86_REG_EAX, 0x11223344),
    (UC_X86_REG_ECX, 0x55667788),
    (UC_X86_REG_EDX, 0x99AABBCC),
    (UC_X86_REG_ESI, 0x12345678),
    (UC_X86_REG_EDI, 0x87654321),
    (UC_X86_REG_EBX, 0x76543210),
    (UC_X86_REG_EBP, 0xABCDEF98),
    (UC_X86_REG_ESP, 0x200004),
    (UC_X86_REG_EFLAGS, 0x202),
):
    uc.reg_write(register, value)
uc.emu_start(0x85830, 0x85876, count=1000)
room_expected = [
    uc.reg_read(r)
    for r in (
        UC_X86_REG_EAX,
        UC_X86_REG_ECX,
        UC_X86_REG_EDX,
        UC_X86_REG_ESI,
        UC_X86_REG_EDI,
        UC_X86_REG_EBX,
        UC_X86_REG_EBP,
    )
]
room_expected.append(uc.reg_read(UC_X86_REG_ESP) + 4)
assert room_expected == [
    1,
    0x400000,
    0,
    0x12345678 + 100,
    0x87654321 + 100,
    0x76543210 + 100,
    0xABCDEF98 + 100,
    0x200008,
]
records = []
for compiler in ("gcc", "clang"):
    for opt in (0, 2, 3):
        binary = Path(f"tmp/batch2-controls/original-{compiler}-{opt}")
        run = subprocess.run([str(binary.resolve())], capture_output=True, text=True, check=True)
        observed = [
            [int(w) for w in line.split()[1:]]
            for line in run.stdout.splitlines()
            if line.startswith("ALIAS ")
        ]
        assert observed == expected, (compiler, opt, observed)
        room_observed = [
            [int(w) for w in line.split()[1:]]
            for line in run.stdout.splitlines()
            if line.startswith("ROOMALIAS ")
        ]
        assert room_observed == [room_expected]
        records.append(
            {
                "va": "0x00085830",
                "compiler": compiler,
                "opt": opt,
                "original_before_ret_plus_adapter_pop": room_expected,
                "native": room_observed[0],
                "match": True,
            }
        )
        records.append(
            {
                "compiler": compiler,
                "opt": opt,
                "original_before_ret_plus_adapter_pop": expected,
                "native": observed,
                "match": True,
            }
        )
Path("docs/data/t1786-batch2/pop-alias-original.json").write_text(
    json.dumps(records, indent=2) + "\n"
)
print(
    "Original save/room-list alias states match all six native builds; save RET target not claimed"
)
