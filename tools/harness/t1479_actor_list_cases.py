# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace75 reciprocal actor-parent/child lists for 3158C0 and 2FBF70.

Frozen before original outcomes in docs/evidence/t1479/pointer-list-domains.md.
"""

from itertools import product

from .model import Case
from .seeding import SeedPolicy
from .t1478_pointer_fixture import PointerFixture

NAMESPACE = 75
SUPPORTED_VAS = frozenset({0x3158C0, 0x2FBF70})
_STATUS = tuple(product(range(4), range(4), (0, 1), (0, 4), (0, 0x10, 0x20, 0x30)))
_LIST_TARGETS = tuple((length, target) for length in range(4) for target in (None, *range(length)))
_GUARD = tuple(
    product(_LIST_TARGETS, range(4), (0, 0x80, 0x20, 8, 0x16004008), (0, 4), (0, 1), (0, 1))
)
_LATE = tuple(product(range(6), (0, 0x80)))
_CONTROLS = 4


def actor_list_case_count(va: int) -> int:
    if va == 0x3158C0:
        return len(_STATUS) + _CONTROLS
    return len(_GUARD) + len(_LATE) + _CONTROLS if va == 0x2FBF70 else 0


def make_actor_list_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < actor_list_case_count(va):
        raise ValueError("unsupported T1479 actor-list case")
    fixture = PointerFixture(NAMESPACE, seed, ordinal, va, size, policy)
    count = actor_list_case_count(va) - _CONTROLS
    control = ordinal - count
    profile_flags, selected, equality, active, late = 0, None, 1, 0, 0
    if va == 0x3158C0:
        length, topology, active, state, byte_flags = (
            _STATUS[ordinal] if control < 0 else (2, 0, 0, 4, 0x20)
        )
    elif ordinal < len(_GUARD):
        (length, selected), topology, profile_flags, byte_flags, equality, active = _GUARD[ordinal]
        state = 4
    elif control < 0:
        late, profile_flags = _LATE[ordinal - len(_GUARD)]
        length, selected, topology, byte_flags, state = 1, 0, 0, 0, 4
    else:
        length, selected, topology, byte_flags, state = 2, 1, 0, 0, 4
    profile, base = 0xD00000, 0xD90000
    nodes = [base + 0x400 * index for index in range(length)]
    final = nodes[-1] if nodes else 0
    for index, node in enumerate(nodes):
        fixture.word(node, 1 if index == selected else 0)
        fixture.word(node + 0x2C, nodes[index - 1] if index else 0)
        fixture.word(node + 0x170, nodes[index + 1] if index + 1 < length else 0)
        fixture.word(node + 0x1B0, state if node == final else state ^ 4)
        fixture.byte(node + 0xC4, byte_flags if node == final else byte_flags ^ 0x30)
        fixture.byte(node + 0x218, byte_flags)
    parent, ancestor = base + 0x2000, base + 0x2400
    for node in (parent, ancestor):
        fixture.word(node, 0)
        fixture.word(node + 0x170, 0)
        fixture.word(node + 0x2C, 0)
        fixture.byte(node + 0x218, 0)
    if nodes and topology:
        fixture.word(nodes[0] + 0x2C, parent)
        fixture.word(nodes[0], 3 if topology == 2 else 0)
        if topology == 3:
            fixture.word(parent + 0x2C, ancestor)
    index = (-1, 0, 1)[ordinal % 3]
    fixture.word(0x7BA9E0, 0x01000000 if control == 2 else index)
    fixture.word(0x7BA26C + index * 0x1E0, nodes[0] if nodes else 0)
    fixture.word(0x783B94, active)
    fixture.word(0x7844A8, 0x90000000 if control == 0 and va == 0x2FBF70 else profile)
    fixture.word(0x7842A8, final if equality else base + 0x3000)
    fixture.word(profile + 0x25C, profile_flags)
    fixture.word(profile + 0x2F34, 0 if late == 1 else 0xFFFFFFFF)
    fixture.half(profile + 0x2F10, 0 if late == 2 else 0xFFFF)
    fixture.word(profile + 0x2F7C, 1 if late == 3 else 0)
    fixture.word(profile + 0x1185C, 0 if late == 4 else 0xFFFFFFFF)
    if late == 5:
        fixture.word(0x783B94, 1)
    if control == 0 and va == 0x3158C0:
        fixture.word(0x7BA26C + index * 0x1E0, 0x90000000)
    if control == 1 and nodes:
        fixture.word(nodes[0] + 0x2C, 0x90000000)
    if control == 3:
        fixture.move_frame(0x7BA9E4)
    return fixture.finish()
