# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic populated keyed-table cases, judged against original instructions.

Default pointer-shaped arguments cannot initialize a title-owned global table.
These cases supply the same explicit initial bytes to oracle and subject, while
retaining the ordinary random/edge fault cases. They are tests, never live state.
"""

from __future__ import annotations

from dataclasses import replace
from random import Random

from .model import REG_NAMES, Case
from .seeding import SeedPolicy, derive_seed, make_case

TABLE_CASES = 512
TABLE_INDEX_BASE = 6 << 40
TABLE_ORDINAL_BITS = 11
TABLE_FUNCTIONS = frozenset({0x001CF640, 0x002FDF50, 0x00311C00})


def has_table_cases(va: int) -> bool:
    return va in TABLE_FUNCTIONS


def _dword(value: int) -> bytes:
    return (value & 0xFFFFFFFF).to_bytes(4, "little")


def make_table_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    """Build empty/matching/missing rows, wrap inputs and genuine frame aliases."""
    if va not in TABLE_FUNCTIONS or not 0 <= ordinal < 1 << TABLE_ORDINAL_BITS:
        raise ValueError("unsupported table address or ordinal")
    index = TABLE_INDEX_BASE + (va << TABLE_ORDINAL_BITS) + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    if va == 0x002FDF50:
        return _make_layer_case(case, seed, index, ordinal, policy)
    if va == 0x00311C00:
        return _make_region_case(case, seed, index, ordinal, policy)
    rng = Random(derive_seed(seed, index) ^ 0x5441424C45)
    scratch = (policy or SeedPolicy()).scratch_base
    rows = scratch + 0x2000
    keys = scratch + 0x3000
    scale = rng.choice((0, 1, 3, 0xFFFFFFFD, 0xFFFFFFFF))
    factor = rng.getrandbits(32)
    row = ordinal % 8
    count = (ordinal // 8) % 9
    wanted = 0x10000000 + rng.randrange(count) if count else 0xFFFFFFFF
    if ordinal % 3 == 0:
        wanted = 0xFFFFFFFF  # missing-key fallback, including nonempty rows.
    payload = rng.getrandbits(32)
    regs = list(case.regs)
    patches: list[tuple[int, bytes]] = []
    for key in range(9):
        patches.append((keys + key * 8, _dword(0x10000000 + key) + _dword(rng.getrandbits(32))))
    patches.append((keys - 4, _dword(0xDEADC0DE)))
    shape = ordinal % 32
    if shape in (24, 25, 26):
        # Empty fallback can overwrite saved EBX before its actual pop, or a
        # save already restored by the original; sentinel itself is untouched.
        count = 0
        keys = case.esp + (-4, -8, -12)[shape - 24] + 4
    elif shape == 27:
        # Matching key is the saved incoming EBP. Value overwrites saved EBX.
        count = 1
        keys = case.esp - 8
        wanted = regs[REG_NAMES.index("ebp")]
    elif shape == 28:
        # The original's frame writes construct the row header before lookup.
        row = 0
        rows = case.esp - 12
        count = 1
        regs[REG_NAMES.index("esi")] = 1
        regs[REG_NAMES.index("ebp")] = keys
        wanted = 0x10000000
    elif shape == 29:
        # A deliberate invalid original input stays a no-verdict fault.
        rows = 0xFFFFFFFF
    regs[REG_NAMES.index("ecx")] = (row - scale * factor) & 0xFFFFFFFF
    patches.extend(
        (
            (0x007087C4, _dword(scale)),
            (0x0075AF1C, _dword(rows)),
            (case.esp + 4, _dword(factor) + _dword(wanted) + _dword(payload)),
        )
    )
    if shape not in (28, 29):
        patches.append((rows + row * 8, _dword(count) + _dword(keys)))
    return replace(case, regs=tuple(regs), patches=(*case.patches, *patches))


def _make_layer_case(
    case: Case, seed: int, index: int, ordinal: int, policy: SeedPolicy | None
) -> Case:
    """Populate genuine 16-bit cell layouts for signed and linked-layer paths."""
    rng = Random(derive_seed(seed, index) ^ 0x4C41594552)
    scratch = (policy or SeedPolicy()).scratch_base
    row = ordinal % 5
    record = 0x00529510 + row * 320
    width = (ordinal // 5) % 6
    height = (ordinal // 30) % 5
    layers = (ordinal // 150) % 4 + 1
    if ordinal % 31 == 0:
        width = 0xFFFFFFFF
    if ordinal % 37 == 0:
        height = 0x80000000
    if ordinal % 41 == 0:
        layers = 0xFFFFFFFF
    patches: list[tuple[int, bytes]] = [
        (case.esp + 4, _dword(row)),
        (record + 20, _dword(width) + _dword(height) + _dword(layers)),
    ]
    for layer in range(4):
        address = scratch + 0x2000 + layer * 0x1000
        cells = bytearray()
        for cell in range(40):
            flags = rng.randrange(16) | (rng.randrange(16) << 8)
            if (ordinal + layer + cell) % 3:
                flags |= 0x1000
            cells.extend(_dword((rng.randrange(65536) << 16) | flags))
        patches.append((address, bytes(cells)))
        if ordinal % 32 in (24, 25, 26) and layer == ordinal % 3:
            # Read actual saved registers/count/locals as cells; loops stay finite.
            width = height = 1
            patches.append((record + 20, _dword(width) + _dword(height)))
            address = case.esp + (-36, -20, -4)[ordinal % 32 - 24]
        patches.append((record + 32 + layer * 4, _dword(address)))
    return replace(case, patches=(*case.patches, *patches))


def _make_region_case(
    case: Case, seed: int, index: int, ordinal: int, policy: SeedPolicy | None
) -> Case:
    """Initialize actual six-byte primary and eight-byte secondary record layouts."""
    rng = Random(derive_seed(seed, index) ^ 0x524547494F4E)
    row = ordinal % 5
    record = 0x00529510 + row * 320
    primary = ordinal % 11
    secondary = (ordinal // 11) % 9
    if ordinal % 37 == 0:
        primary = 0x80000000
    if ordinal % 41 == 0:
        secondary = 0xFFFFFFFF
    patches: list[tuple[int, bytes]] = [
        (record + 20, _dword(rng.getrandbits(32)) + _dword(rng.getrandbits(32))),
        (record + 68, _dword(primary)),
        (record + 264, _dword(secondary)),
    ]
    components = bytearray()
    for _ in range(24):
        components.extend(
            bytes(
                (
                    rng.randrange(9),
                    rng.randrange(256),
                    rng.randrange(256),
                    rng.randrange(256),
                    rng.randrange(12),
                    rng.randrange(5),
                )
            )
        )
    patches.append((record + 72, bytes(components)))
    for component in range(9):
        patches.append(
            (
                record + 268 + component * 8,
                _dword(rng.choice((0, 0, 1, 0xFFFFFFFF))) + _dword(rng.randrange(9)),
            )
        )
    regs = list(case.regs)
    shape = ordinal % 32
    if shape in (24, 25, 26):
        # Saves overwrite primary bytes, including a count already loaded.
        # Real bounded count/read chronology decides the result on BOTH sides.
        regs[REG_NAMES.index("esp")] = record + (70, 74, 86)[shape - 24]
        for register in ("ebx", "esi", "edi", "ebp"):
            regs[REG_NAMES.index(register)] = 0x00030003
        if shape == 24:
            regs[REG_NAMES.index("ebp")] = 0
        elif shape == 25:
            regs[REG_NAMES.index("ebp")] = 0x00050000
        patches.append((record + 68, _dword(3)))
    esp = regs[REG_NAMES.index("esp")]
    patches.append((esp, _dword((policy or SeedPolicy()).sentinel) + _dword(row)))
    return replace(case, regs=tuple(regs), patches=(*case.patches, *patches))
