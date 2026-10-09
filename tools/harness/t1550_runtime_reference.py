# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent input-to-output equations for the frozen T1550 scalar microprograms.

This module neither executes instructions nor generates guest faults. Invalid input
reads return None as a reference classification; only real CPU backends report faults.
"""

from __future__ import annotations

from .image import GuestImage
from .model import Case


def expected(case: Case, image: GuestImage) -> tuple[tuple[int, ...], dict[int, int]] | None:
    overlay = {
        address + offset: byte for address, raw in case.patches for offset, byte in enumerate(raw)
    }

    def read(address: int, width: int) -> int | None:
        if not image.base <= address < address + width <= image.base + len(image.data):
            return None
        return int.from_bytes(
            bytes(
                overlay.get(at, image.data[at - image.base])
                for at in range(address, address + width)
            ),
            "little",
        )

    regs = list(case.regs)
    output = {}

    def store(address: int, value: int) -> None:
        for offset, byte in enumerate(value.to_bytes(4, "little")):
            at = address + offset
            if read(at, 1) != byte:
                output[at] = byte

    if case.va == 0x10000:
        byte = read(0x20000, 1)
        pointer = read(0x20004, 4)
        assert byte is not None and pointer is not None
        value = read(pointer, 4)
        if value is None:
            return None
        regs[0] = read(0x20010 + 4 * byte, 4)
        regs[1] = pointer
        regs[2] = value
        store(0xDC0004, value)
        pop = 4
    elif case.va == 0x10080:
        pointer = read(0x20004, 4)
        assert pointer is not None
        value = read(pointer, 4)
        if value is None:
            return None
        regs[0] = read(0x20018, 4)
        regs[1] = pointer
        regs[2] = value
        regs[3] = read(0x2000C, 4)
        regs[6] = read(0x20008, 4)
        # A real guest CALL leaves its consumed return word below the root frame.
        store((case.esp - 4) & 0xFFFFFFFF, 0x10085)
        store(0xDC0008, regs[3])
        pop = 4
    elif case.va == 0x100F0:
        regs[0] = read((case.esp + 4) & 0xFFFFFFFF, 4)
        regs[1] = read((case.esp + 8) & 0xFFFFFFFF, 4)
        if regs[0] is None or regs[1] is None:
            return None
        store(0xDC000C, regs[0])
        pop = 12
    else:
        raise ValueError("unsupported T1550 independent reference root")
    regs[4] = (regs[4] + pop) & 0xFFFFFFFF
    if any(type(value) is not int for value in regs):
        raise ValueError("independent reference encountered unmapped fixed data")
    return tuple(regs), output
