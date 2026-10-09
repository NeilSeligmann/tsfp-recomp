# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace 77 nested record lists and signed profile-table domains.

Pre-outcome contract: docs/evidence/t1479/pointer-list-domains.md.
"""

from itertools import product

from .model import Case
from .seeding import SeedPolicy
from .t1478_pointer_fixture import PointerFixture

NAMESPACE = 77
SUPPORTED_VAS = frozenset({0x2FE520})
_PRIMARY_TARGETS = tuple(
    (groups, length, target)
    for groups in range(4)
    for length in range(4)
    for target in (None, *((g, i) for g in range(groups) for i in range(length)))
)
_PRIMARY = tuple(product(_PRIMARY_TARGETS, (-32768, -1, 0, 32767)))
_SECONDARY_TARGETS = tuple(
    (length, target) for length in (0, 1, 3) for target in (None, *range(length))
)
_SECONDARY = tuple(
    product(_SECONDARY_TARGETS, (-32768, -1, 0, 32767, 32768), (0x46, 0xFFFFFFFF, 1), (0, 1))
)


def profile_list_case_count(va: int) -> int:
    return len(_PRIMARY) + len(_SECONDARY) + 5 if va in SUPPORTED_VAS else 0


def make_profile_list_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < profile_list_case_count(va):
        raise ValueError("unsupported T1479 profile/list case")
    fixture = PointerFixture(NAMESPACE, seed, ordinal, va, size, policy)
    control = ordinal - len(_PRIMARY) - len(_SECONDARY)
    groups, length, target, count, selected, mode, same = 0, 0, None, 0, None, 0xFFFFFFFF, 0
    key = -1
    if ordinal < len(_PRIMARY):
        (groups, length, target), key = _PRIMARY[ordinal]
    elif control < 0:
        (count, selected), key, mode, same = _SECONDARY[ordinal - len(_PRIMARY)]
    profile = fixture.esp - 20 - 0x2F34 if control == 4 else 0xD00000
    head = 0xD90000
    fixture.word(
        0x78400C, 0x90000000 if control == 1 else head if groups or control in (2, 3) else 0
    )
    for group in range(max(groups, 1 if control in (2, 3) else 0)):
        node = head + group * 0x100
        first = 0xDA0000 + group * 0x1000
        fixture.word(node + 0x40, head + (group + 1) * 0x100 if group + 1 < groups else 0)
        fixture.word(node + 0x3C, 0x90000000 if control == 2 else first if length else 0)
        for item in range(length):
            entry = first + item * 0x100
            fixture.word(entry + 4, key if target == (group, item) else key ^ 0x10000)
            fixture.word(entry + 0x14, entry + 0x100 if item + 1 < length else 0)
    if control == 3:
        fixture.word(head + 0x40, head)
    fixture.word(0x7844A8, 0x90000000 if control == 0 else profile)
    fixture.word(profile + 0x2F34, mode)
    fixture.word(profile + 0x2F38, key if same else key ^ 0x10000)
    fixture.half(profile + 0x306E, count)
    for item in range(count):
        fixture.half(profile + 0x307A + item * 12, key if item == selected else key ^ 0x4000)
    fixture.word(fixture.esp + 4, key)
    return fixture.finish()
