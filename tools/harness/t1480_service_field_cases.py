# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen amended service-object domain for417ADA; namespace70 delegation only."""

from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import GUEST_HI, SCRATCH_BASE, SeedPolicy, make_case

ROOT = 0x417ADA
INDEX_BASE = 70 << 40
OBJECT = SCRATCH_BASE + 0xF000
IDS = (0, 1, 0x80000000, 0xFFFFFFFF)
STATUSES = (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)
PAYLOADS = (0, 0x10203040, 0x80000000, 0xFFFFFFFF)
HITS = tuple(product(range(16), IDS, STATUSES, PAYLOADS))
BAD_READS = tuple(product((0xFFFFFFFF, GUEST_HI - 0xC8), IDS))


def service_field_case_count(va: int) -> int:
    return 1308 if va == ROOT else 0


def _word(value: int) -> bytes:
    return (value & 0xFFFFFFFF).to_bytes(4, "little")


def _object(
    request: int,
    *,
    online: int,
    hit: int = -1,
    status: int = 0,
    payload: int = 0,
    duplicate: bool = False,
) -> bytes:
    data = bytearray(0x214)
    data[0xC8:0xCC] = _word(online)
    for index in range(16):
        offset = 0xD4 + 20 * index
        matching = index == hit or (duplicate and index == 15)
        words = (
            request if matching else request ^ 0xFFFFFFFF,
            0xFFFFFFFF if duplicate and index == 15 else payload if matching else 0,
            0xA5000000 + index,
            status if matching else 0,
            0x5A000000 + index,
        )
        data[offset : offset + 20] = b"".join(_word(word) for word in words)
    return bytes(data)


def make_service_field_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < service_field_case_count(va):
        raise ValueError("unsupported T1480 service-field case")
    case = make_case(seed, INDEX_BASE + ordinal, va, size, policy=policy)
    extra: list[tuple[int, bytes]] = []
    pointer = OBJECT
    if ordinal < 1280:
        hit, request, status, payload = HITS[ordinal]
        pointer += hit % 2
        extra.append(
            (
                pointer,
                _object(
                    request,
                    online=1 if hit % 2 == 0 else 0xFFFFFFFF,
                    hit=hit,
                    status=status,
                    payload=payload,
                ),
            )
        )
    elif ordinal < 1284:
        request = IDS[ordinal - 1280]
        pointer = 0
    elif ordinal < 1288:
        request = IDS[ordinal - 1284]
        extra.append((pointer, _object(request, online=0, hit=0, status=1, payload=0x10203040)))
    elif ordinal < 1292:
        request = IDS[ordinal - 1288]
        extra.append((pointer, _object(request, online=1)))
    elif ordinal < 1300:
        pointer, request = BAD_READS[ordinal - 1292]
    elif ordinal < 1304:
        request = IDS[ordinal - 1300]
        pointer = GUEST_HI - 0xD4
        extra.append((GUEST_HI - 12, _word(1)))
    else:
        request = IDS[ordinal - 1304]
        extra.append(
            (pointer, _object(request, online=1, hit=0, payload=0x10203040, duplicate=True))
        )
    regs = list(case.regs)
    regs[1] = pointer
    return replace(
        case, regs=tuple(regs), patches=(*case.patches, (case.esp + 4, _word(request)), *extra)
    )
