# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen T1535 menu indexed-flag domain, namespace79 additive root only.

Pre-outcome contract: docs/evidence/t1535/menu-index-plan.md.
No original execution or validated unlock follows from fixture construction.
"""

from itertools import product

from .model import Case
from .seeding import SeedPolicy
from .t1478_pointer_fixture import PointerFixture

NAMESPACE = 79
ROOT = 0x002D2BE0
SUPPORTED_VAS = frozenset({ROOT})
MODE_ADDRESS = 0x007DE455
TABLE = 0x004D1F88
MODES = (0, 9, 10, 11, 255)
CALLER_INDICES = (0, 1, 7, 19, 20, 127, 255)
WRAP_INDICES = (0x08000000, 0x80000000, 0xFFFFFFFF)
VALUES = (0, 1, 0x80000000, 0xFFFFFFFF, 0x12345678)
DECOYS = (0, 0xDEADBEEF)
UNMAPPED_INDEX = 0x04800000
ORDINARY = tuple(product(MODES, CALLER_INDICES + WRAP_INDICES, VALUES, DECOYS))
FAULT_CONTROLS = tuple(product(MODES, DECOYS))


def menu_index_case_count(va: int) -> int:
    return len(ORDINARY) + len(FAULT_CONTROLS) if va == ROOT else 0


def make_menu_index_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != ROOT or type(ordinal) is not int or not 0 <= ordinal < menu_index_case_count(va):
        raise ValueError("unsupported T1535 menu-index root/ordinal")
    fixture = PointerFixture(NAMESPACE, seed, ordinal, va, size, policy)
    if ordinal < len(ORDINARY):
        mode, index, value, decoy = ORDINARY[ordinal]
        address = (TABLE + ((index << 5) & 0xFFFFFFFF)) & 0xFFFFFFFF
        fixture.word(address, value)
        # Adjacent words are decoys: the original reads exactly one aligned dword.
        fixture.word(address - 4, value ^ 0xA5A5A5A5)
        fixture.word(address + 4, value ^ 0x5A5A5A5A)
    else:
        mode, decoy = FAULT_CONTROLS[ordinal - len(ORDINARY)]
        index = UNMAPPED_INDEX
        # No patch attempts an unmapped address; actual load must retain its fault.
    fixture.byte(MODE_ADDRESS, mode)
    fixture.word(fixture.esp + 4, decoy)
    fixture.word(fixture.esp + 8, index)
    return fixture.finish()
