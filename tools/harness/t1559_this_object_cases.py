# SPDX-License-Identifier: GPL-3.0-or-later
"""Unactivated pure constructor input proposal; supplies no execution authority."""

from dataclasses import replace

from .model import Case
from .seeding import SCRATCH_BASE, SeedPolicy, make_case

NAMESPACE = 92
LABEL = "t1559-this-objects"
ROOT_SIZES = {0x3BEB10: 37, 0x3BEDF0: 51, 0x3BF680: 66}
OBJECT_OFFSETS = (0x2000, 0x3001, 0x4002, 0x5003, 0x6000, 0x7001, 0x8002, 0x9003)
BACKGROUNDS = (0x00, 0x01, 0x55, 0x7F, 0x80, 0xAA, 0xFE, 0xFF)
# Measured protected code prefix of the reviewed export (docs/evidence/t1559/resumed-engineering-
# predeclaration.md): guest code lives in [0x10000, 0x4EDF95). Object windows must stay clear.
CODE_LO = 0x10000
CODE_HI = 0x4EDF95
WINDOW_BELOW = 16
WINDOW_ABOVE = 144
PHYSICAL_FIELDS = ("scratch_base", "stack_base", "sentinel", "frame_args", "scratch_fill_bytes")


def window_clear_of_code(address: int, va: int, size: int) -> bool:
    """True when the patched window [address-16, address+144) misses all code and the root."""
    low, high = address - WINDOW_BELOW, address + WINDOW_ABOVE
    if low < 0 or high > 1 << 32:
        return False
    if low < CODE_HI and CODE_LO < high:
        return False
    return not (low < va + size and va < high)


def this_object_case_count(va: int) -> int:
    return 128 if va in ROOT_SIZES else 0


def make_this_object_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    """Append twenty owned-width proposals; retain every inherited Case field."""
    if va not in ROOT_SIZES or ROOT_SIZES[va] != size:
        raise ValueError("unsupported constructor body")
    if type(ordinal) is not int or not 0 <= ordinal < 128:
        raise ValueError("constructor ordinal outside frozen Cartesian domain")
    policy = policy or SeedPolicy()
    defaults = SeedPolicy()
    if any(getattr(policy, field) != getattr(defaults, field) for field in PHYSICAL_FIELDS):
        raise ValueError("relocated physical policy has no reviewed object geometry")
    case = make_case(seed, (NAMESPACE << 40) + ordinal, va, size, policy=policy)
    object_address = SCRATCH_BASE + OBJECT_OFFSETS[ordinal // 16]
    if not window_clear_of_code(object_address, va, size):
        raise ValueError("object window overlaps guest code: refused, never a verdict")
    background = BACKGROUNDS[(ordinal // 2) % 8]
    regs = list(case.regs)
    regs[1] = object_address
    patches = tuple((object_address - 16 + 8 * j, bytes([background]) * 8) for j in range(20))
    return replace(case, regs=tuple(regs), df=ordinal % 2, patches=case.patches + patches)
