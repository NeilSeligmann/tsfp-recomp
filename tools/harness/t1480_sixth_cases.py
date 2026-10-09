# SPDX-License-Identifier: GPL-3.0-or-later
"""T1480/T1476 sixth-batch amended domains (namespace 47), written after the default-domain
outcomes of 004098CE (coverage 0.640), 0008E920 (no verdicts), 00094390 (coverage 0.792)
and 00094410 (coverage 0.784) were seen. Additive to the seed stream, registers and the
unpatched argument words keep the seed stream values."""

from dataclasses import replace

from .model import Case
from .seeding import SCRATCH_BASE, SeedPolicy, make_case

SIXTH_INDEX_BASE = 47 << 40

# --- 004098CE (dest, source): tag word at source, valid record bytes -------------------
RECORD_DEST = SCRATCH_BASE + 0xA800
RECORD_SOURCE = SCRATCH_BASE + 0xA000
RECORD_TAGS = (1, 0x69, 0xFFFE, 0, 2, 0x68, 0x6A, 0xFFFF)
RECORD_HIGHS = (0, 0xABCD)
RECORD_VARIANTS = 4
RECORD_CASES = len(RECORD_TAGS) * len(RECORD_HIGHS) * RECORD_VARIANTS

# --- 0008E920 (object, a1, amount, a3, a4): valid object and counter chain ------------------
CHAIN_OBJECT = SCRATCH_BASE + 0xB000
CHAIN_BASE = SCRATCH_BASE + 0xB400
CHAIN_AMOUNTS = (0, 3, 0x7FFFFFFF, 0xFFFFFFFF)
CHAIN_PATTERNS = tuple((length, mask) for length in (1, 2, 3) for mask in range(1 << length))
#: Counter negative bit per entry in `mask`; 14 patterns, plus a negative pre-check case.
CHAIN_CASES = (len(CHAIN_PATTERNS) + 1) * len(CHAIN_AMOUNTS)

# --- 00094390 / 00094410: valid object, owner flags, table and bump allocator --------------
SNAP_OBJECT = SCRATCH_BASE + 0xC000
SNAP_FLAGS = SCRATCH_BASE + 0xC100
SNAP_OWNER = SCRATCH_BASE + 0xC200
SNAP_TABLE_10 = SCRATCH_BASE + 0xC400
SNAP_TABLE_14 = SCRATCH_BASE + 0xC800
SNAP_HEAP = SCRATCH_BASE + 0xE000
SNAP_FLAG_WORDS = (0, 0x10, 0xFFFFFFFF)
SNAP_EXTRAS = (0, 1, 3, 8)
#: 0x10 stride table lengths 0..4 (0 = negative first entry) times extras, plus a null table.
STRIDE10_CASES = (5 * len(SNAP_EXTRAS) + 1) * len(SNAP_FLAG_WORDS)
#: 0x30 stride table lengths 0..3, plus a null table.
STRIDE30_CASES = (4 + 1) * len(SNAP_FLAG_WORDS)

SIXTH_ROOTS = {
    0x4098CE: RECORD_CASES,
    0x8E920: CHAIN_CASES,
    0x94390: STRIDE10_CASES,
    0x94410: STRIDE30_CASES,
}


def sixth_case_count(va: int) -> int:
    return SIXTH_ROOTS.get(va, 0)


def _words(*values: int) -> bytes:
    return b"".join((value & 0xFFFFFFFF).to_bytes(4, "little") for value in values)


def _record_patches(ordinal: int) -> list[tuple[int, bytes]]:
    pair, variant = divmod(ordinal, RECORD_VARIANTS)
    tag = RECORD_TAGS[pair // len(RECORD_HIGHS)]
    high = RECORD_HIGHS[pair % len(RECORD_HIGHS)]
    source = RECORD_SOURCE + 0x80 * variant
    body = bytes((variant * 37 + index * 5 + 1) & 0xFF for index in range(0x40))
    record = (tag | high << 16).to_bytes(4, "little") + body[4:]
    return [(source, record), (RECORD_DEST, bytes(0x40))]


def record_arguments(ordinal: int) -> tuple[int, int]:
    return RECORD_DEST, RECORD_SOURCE + 0x80 * (ordinal % RECORD_VARIANTS)


def chain_parameters(ordinal: int) -> tuple[int, int, int]:
    """`(length, negative_mask, amount)`; length 0 is the negative pre-check case."""
    pattern, amount = divmod(ordinal, len(CHAIN_AMOUNTS))
    if pattern == len(CHAIN_PATTERNS):
        return 0, 0, CHAIN_AMOUNTS[amount]
    length, mask = CHAIN_PATTERNS[pattern]
    return length, mask, CHAIN_AMOUNTS[amount]


def _chain_patches(ordinal: int) -> list[tuple[int, bytes]]:
    length, mask, _ = chain_parameters(ordinal)
    block = bytearray(0xC8)
    block[0x4B] = 0xFF
    block[0x4C] = 0xFF
    block[0x14:0x18] = CHAIN_BASE.to_bytes(4, "little")
    chain = bytearray(0x30 * 5)
    if length == 0:
        chain[0x10:0x14] = _words(0xFFFFFFFF)
    for entry in range(length):
        counter = 0x80000005 if mask >> entry & 1 else 5
        chain[0x30 * entry : 0x30 * entry + 4] = _words(counter)
        chain[0x30 * entry + 0x10 : 0x30 * entry + 0x14] = _words(1)
    if length:
        chain[0x30 * length + 0x10 : 0x30 * length + 0x14] = _words(0xFFFFFFFF)
    return [(CHAIN_OBJECT, bytes(block)), (CHAIN_BASE, bytes(chain))]


def snapshot_parameters_10(ordinal: int) -> tuple[int, int, int, bool]:
    """`(table_length, extra, flag_word, null_table)` for a 00094390 ordinal."""
    group, flag = divmod(ordinal, len(SNAP_FLAG_WORDS))
    if group == 5 * len(SNAP_EXTRAS):
        return 0, 0, SNAP_FLAG_WORDS[flag], True
    length, extra = divmod(group, len(SNAP_EXTRAS))
    return length, SNAP_EXTRAS[extra], SNAP_FLAG_WORDS[flag], False


def snapshot_parameters_14(ordinal: int) -> tuple[int, int, bool]:
    """`(table_length, flag_word, null_table)` for a 00094410 ordinal."""
    group, flag = divmod(ordinal, len(SNAP_FLAG_WORDS))
    return group if group < 4 else 0, SNAP_FLAG_WORDS[flag], group == 4


def _snapshot_common(
    slot: int, table_slot: int, table: int, flag_word: int
) -> list[tuple[int, bytes]]:
    obj = bytearray(0x200)
    obj[0:4] = _words(SNAP_FLAGS)
    obj[4:8] = _words(SNAP_OWNER)
    obj[slot : slot + 4] = _words(0x5A5A5A5A)
    owner = bytearray(0x20)
    owner[table_slot : table_slot + 4] = _words(table)
    return [
        (SNAP_OBJECT, bytes(obj)),
        (SNAP_FLAGS, bytes(0x18) + _words(flag_word)),
        (SNAP_OWNER, bytes(owner)),
        (0x6B839C, _words(SNAP_HEAP)),
        (0x6B83B8, _words(0x1000)),
    ]


def _snapshot_patches_10(ordinal: int) -> list[tuple[int, bytes]]:
    length, _, flag_word, null_table = snapshot_parameters_10(ordinal)
    patches = _snapshot_common(0x1EC, 0x10, 0 if null_table else SNAP_TABLE_10, flag_word)
    table = bytearray(0x10 * 8)
    if length == 0:
        table[0:4] = _words(0xFFFFFFFF)
    else:
        table[0:4] = _words(1)
        for entry in range(1, length):
            table[0x10 * entry : 0x10 * entry + 4] = _words(1)
        table[0x10 * length : 0x10 * length + 4] = _words(0xFFFFFFFF)
    return [*patches, (SNAP_TABLE_10, bytes(table))]


def _snapshot_patches_14(ordinal: int) -> list[tuple[int, bytes]]:
    length, flag_word, null_table = snapshot_parameters_14(ordinal)
    patches = _snapshot_common(0x1F0, 0x14, 0 if null_table else SNAP_TABLE_14, flag_word)
    table = bytearray(0x10 + 0x30 * 5)
    if length == 0:
        table[0x10:0x14] = _words(0xFFFFFFFF)
    else:
        table[0x10:0x14] = _words(1)
        for entry in range(1, length):
            table[0x10 + 0x30 * entry : 0x14 + 0x30 * entry] = _words(1)
        table[0x10 + 0x30 * length : 0x14 + 0x30 * length] = _words(0xFFFFFFFF)
    return [*patches, (SNAP_TABLE_14, bytes(table))]


def make_sixth_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < sixth_case_count(va):
        raise ValueError("unsupported T1480 sixth-batch case")
    case = make_case(seed, SIXTH_INDEX_BASE + ordinal, va, size, policy=policy)
    if va == 0x4098CE:
        dest, source = record_arguments(ordinal)
        extra = [(case.esp + 4, _words(dest, source)), *_record_patches(ordinal)]
    elif va == 0x8E920:
        _, _, amount = chain_parameters(ordinal)
        extra = [
            (case.esp + 4, _words(CHAIN_OBJECT)),
            (case.esp + 12, _words(amount)),
            *_chain_patches(ordinal),
        ]
    elif va == 0x94390:
        _, extra_slots, _, _ = snapshot_parameters_10(ordinal)
        extra = [(case.esp + 4, _words(SNAP_OBJECT, extra_slots)), *_snapshot_patches_10(ordinal)]
    else:
        extra = [(case.esp + 4, _words(SNAP_OBJECT)), *_snapshot_patches_14(ordinal)]
    return replace(case, patches=(*case.patches, *extra))
