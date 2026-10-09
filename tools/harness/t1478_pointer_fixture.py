# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch helpers for T1478's predeclared pointer-table domains (namespaces72-74)."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case


class PointerFixture:
    def __init__(
        self, namespace: int, seed: int, ordinal: int, va: int, size: int, policy: SeedPolicy | None
    ) -> None:
        self.case = make_case(seed, (namespace << 40) + ordinal, va, size, policy=policy)
        self.patches = list(self.case.patches)
        self.regs = list(self.case.regs)
        self.esp = self.case.esp

    def word(self, address: int, value: int) -> None:
        self.patches.append((address & 0xFFFFFFFF, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    def half(self, address: int, value: int) -> None:
        self.patches.append((address & 0xFFFFFFFF, (value & 0xFFFF).to_bytes(2, "little")))

    def byte(self, address: int, value: int) -> None:
        self.patches.append((address & 0xFFFFFFFF, bytes([value & 0xFF])))

    def move_frame(self, address: int) -> None:
        frame = next(data for pointer, data in self.case.patches if pointer == self.case.esp)
        self.patches.append((address, frame))
        self.esp = address
        self.regs[4] = address

    def finish(self) -> Case:
        return replace(self.case, patches=tuple(self.patches), regs=tuple(self.regs))
