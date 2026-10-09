# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace-61 amended domains for the T1475/T1477 continuation, batch 2.

Written after the default-domain outcomes of 0x606E0, 0x60740, 0x45E60 (zero verdicts, every
random case faults in the original), 0x17A190 and 0x6BB90 (coverage 0.485/0.489) and 0x6B4E0
(coverage 0.567, null-agree 0.914) were seen. Declaration: docs/t1475-t1477-continuation-batches.md,
"Batch 2 amendment A2". Additive to the seed stream: registers and unpatched words keep the
seed stream values. Every scenario builds valid guest tables so the original runs to its returns.
"""

from collections.abc import Callable
from dataclasses import replace

from .model import Case
from .seeding import SCRATCH_BASE, SeedPolicy, make_case

BATCH2_INDEX_BASE = 61 << 40
Patch = tuple[int, bytes]
Scenario = Callable[[int], tuple[tuple[int, ...], list[Patch]]]


def _w(address: int, *values: int) -> Patch:
    return (address, b"".join((value & 0xFFFFFFFF).to_bytes(4, "little") for value in values))


def _b(address: int, value: int) -> Patch:
    return (address, bytes((value & 0xFF,)))


def _base(offset: int) -> int:
    return SCRATCH_BASE + offset


# --- 0x606E0 / 0x60740 -------------------------------------------------------------------------
TABLE = _base(0x90100)
CALL_HEADER, CALL_SETS, CALL_ARRAY, CALL_VALUES = (_base(0xA0000 + 0x10000 * i) for i in range(4))
RECORD_COUNTS = (4, 1, 0)
TABLE_FIELDS = (0x40, -8, 0x41)
INDEXES = (0, 1, 3, 5, 0xFFFFFFFD, 0x80000000)
#: (4D19CC, 4D19D0 is index, 4D19D4 is index, other) cache states: hit1, hit2, miss, reset.
CACHE_STATES = ((0, True, False), (0, False, True), (0, False, False), (5, True, True))
CALL_INDEXES = (0, 1, 2, 3, 9, 0xFFFFFFFE)
TABLE_CASES = len(RECORD_COUNTS) * len(TABLE_FIELDS) * len(INDEXES)
CALL_CASES = len(CACHE_STATES) * len(CALL_INDEXES)
RECORD_ROOT_CASES = TABLE_CASES + CALL_CASES
DST_VARIANTS = (
    (1, 1, 1),
    (0, 1, 1),
    (1, 0, 1),
    (1, 1, 0),
    (0, 0, 0),
    (1, 0, 0),
    (0, 1, 0),
    (0, 0, 1),
)
COPY_ROOT_CASES = len(DST_VARIANTS) * 3 + 2 + 2
COPY_SOURCE = (0, 2, 3)  # indexes (table path, N=4) used by the copy root
DST16, DST12, DST4 = _base(0xE0000), _base(0xE0100), _base(0xE0200)


def _table_base(field: int) -> int:
    """Row base from the signed count field (cdq rounding, arithmetic shift)."""
    return TABLE + ((field + (3 if field < 0 else 0)) >> 2) * 4


def _table_patches(count: int, field: int) -> list[Patch]:
    base = _table_base(field)
    patches = [
        _w(0x7BB260, count),
        _w(0x7BB288, 0),
        _w(0x6D780C, TABLE),
        _w(TABLE + 8, field),
        _w(base, 2),
    ]
    for index in range(8):
        patches.append(_w(base + 4 + (32 + index) * 4, index + 1))
    return patches


def _call_patches(state: int) -> list[Patch]:
    cache, d0_is_index, d1_is_index = CACHE_STATES[state]
    del d0_is_index, d1_is_index
    patches = [
        _w(0x7BB260, 4),
        _w(0x7BB288, 1),
        _w(0x7DE530, 0x20),
        _w(0x7BB284, 4),
        _w(0x6D7818, CALL_HEADER),
        _w(CALL_HEADER + 4, 0x10),
        _w(CALL_HEADER + 0x14, 0x40),
        _w(0x6D7834, CALL_SETS),
        _w(0x6D7838, CALL_ARRAY),
        _w(0x6D783C, CALL_VALUES),
        _w(0x4D19CC, cache),
        _w(0x7DE540, 0, 1, 2, 3),
        _w(0x7DE520, 0, 1, 0, 0),
    ]
    for slot in range(4):
        patches.append(_w(CALL_SETS + slot * 8, slot * 2, 2))
    for index in range(10):
        patches.append(_w(CALL_ARRAY + index * 4, 0x100 * index))
    for index in range(64):
        patches.append(_w(CALL_VALUES + index * 4, 0x10 + index))
    return patches


def _call_cache_patches(state: int, index: int) -> list[Patch]:
    clamped = 0 if index >= 0x80000000 else min(index, 3)
    _, is_first, is_second = CACHE_STATES[state]
    return [
        _w(0x4D19D0, clamped if is_first else 0x77),
        _w(0x4D19D4, clamped if is_second else 0x78),
        _w(0x6D782C, 0xAAAA),
        _w(0x6D7830, 0xBBBB),
    ]


def record_scenario(ordinal: int) -> tuple[tuple[int, ...], list[Patch]]:
    """`(arguments, patches)` for 0x606E0 (the first four arguments are index and outputs)."""
    if ordinal >= TABLE_CASES:
        state, index_slot = divmod(ordinal - TABLE_CASES, len(CALL_INDEXES))
        index = CALL_INDEXES[index_slot]
        return (index,), [*_call_patches(state), *_call_cache_patches(state, index)]
    rest, index_slot = divmod(ordinal, len(INDEXES))
    count_slot, field_slot = divmod(rest, len(TABLE_FIELDS))
    return (INDEXES[index_slot],), _table_patches(
        RECORD_COUNTS[count_slot], TABLE_FIELDS[field_slot]
    )


def copy_scenario(ordinal: int) -> tuple[tuple[int, ...], list[Patch]]:
    """`(arguments, patches)` for 0x60740 (index, dst16, dst12, dst4)."""
    main = len(DST_VARIANTS) * len(COPY_SOURCE)
    if ordinal >= main + 2:
        # destinations overlap the source record: dst16 = record or record + 0xC, dst12 = + 0xC.
        index = COPY_SOURCE[1]
        record = _table_base(0x40) + 4 + 3 * 64
        patches = _table_patches(4, 0x40)
        patches.extend(_w(record + 4 * word, 0x1000 + word) for word in range(8))
        shift = 0 if ordinal == main + 2 else 0xC
        return (index, record + shift, record + 0xC, record), patches
    if ordinal >= main:
        index = COPY_SOURCE[1]
        state = 0 if ordinal == main else 2
        return (index, DST16, DST12, DST4), [
            *_call_patches(state),
            *_call_cache_patches(state, index),
        ]
    variant, source = divmod(ordinal, len(COPY_SOURCE))
    keep16, keep12, keep4 = DST_VARIANTS[variant]
    index = COPY_SOURCE[source]
    clamped = min(index, 3)
    record = _table_base(0x40) + 4 + (clamped + 1) * 64
    patches = _table_patches(4, 0x40)
    patches.extend(_w(record + 4 * word, 0x1000 * (clamped + 1) + word) for word in range(8))
    arguments = (index, DST16 if keep16 else 0, DST12 if keep12 else 0, DST4 if keep4 else 0)
    return arguments, patches


# --- 0x45E60 -----------------------------------------------------------------------------------
BLOCK = _base(0xC0000)
SKIPS = (0, 1, 3)
WORDS = (0x8001, 0x1234)
GATES = ("zero-gate", "mode-10", "call-path-entry-valid", "call-path-entry-minus-1")
LOOKUP_CASES = len(SKIPS) * len(WORDS) * len(GATES)
THIRD_VARIANTS = 2
LOOKUP_ROOT_CASES = LOOKUP_CASES + THIRD_VARIANTS


def _lookup_build(skip: int, word: int, gate: str) -> tuple[tuple[int, ...], list[Patch]]:
    patches = [_w(0x784014, BLOCK), _w(BLOCK, 0xC77667, 0x12D)]
    cursor = BLOCK + 8
    for _ in range(skip):
        patches.append(_w(cursor, 0xC77669, 0x10))
        cursor += 8 + 0x10
    patches.append(_w(cursor, 0xC77668, 0x20))
    record = cursor + 8
    patches.append((record + 0xE, (word & 0xFFFF).to_bytes(2, "little")))
    patches.append(_w(cursor + 8 + 0x20, 0xC7766A, 0))
    patches.append(_w(0x761404, 0 if gate == "zero-gate" else 1))
    patches.append(_b(0x7DE455, 10 if gate == "mode-10" else 5))
    patches.extend(
        [
            _w(0x76B108, 0),
            _w(0x79094C, 0x65),
            _w(0x7BEE40 + 0x114, 2),
            _w(2 * 8 + 0x537210, 0xFFFFFFFF if gate == "call-path-entry-minus-1" else 5),
        ]
    )
    return (), patches


def lookup_scenario(ordinal: int) -> tuple[tuple[int, ...], list[Patch]]:
    third = ordinal >= LOOKUP_CASES
    rest = ordinal if not third else 0
    rest, gate_slot = divmod(rest, len(GATES))
    skip_slot, word_slot = divmod(rest, len(WORDS))
    gate = (
        GATES[gate_slot]
        if not third
        else ("call-path-entry-valid", "mode-10")[ordinal - LOOKUP_CASES]
    )
    return _lookup_build(SKIPS[skip_slot], WORDS[word_slot], gate)


# --- 0x17A190 ----------------------------------------------------------------------------------
OBJECT, KEY_OBJECT, OWNERS, OWNER_RECORD = (_base(0xD0000 + 0x1000 * i) for i in range(4))
HANDLES = ("zero", "stale", "valid")
#: (entries, match position or None) for a valid handle.
LISTS = ((0, None), (1, None), (1, 0), (3, None), (3, 0), (3, 2))
INDEX_WORDS = (0xFFFFFFFF, 0, 1, 5)
HANDLE_VALUE = (1 << 22) | 0x11
OWNER_ROW = OWNERS + 1 * 0x2E8
OWNER_KEY = 0x77


def _owner_cases() -> list[tuple[str, int, int | None, int]]:
    cases: list[tuple[str, int, int | None, int]] = [("zero", 0, None, 0), ("stale", 0, None, 0)]
    for entries, match in LISTS:
        words = INDEX_WORDS if match is not None else (0,)
        cases.extend(("valid", entries, match, word) for word in words)
    cases.append(("null-record", 3, 0, 1))
    return cases


OWNER_CASE_TABLE = _owner_cases()
OWNER_CASES = len(OWNER_CASE_TABLE)


def owner_scenario(ordinal: int) -> tuple[tuple[int, ...], list[Patch]]:
    kind, entries, match, word = OWNER_CASE_TABLE[ordinal]
    handle = {"zero": 0, "stale": HANDLE_VALUE}.get(kind, HANDLE_VALUE)
    patches = [_w(OBJECT, KEY_OBJECT), _w(OBJECT + 0xC08, handle), _w(KEY_OBJECT + 0x2C, OWNER_KEY)]
    patches.append(_w(0x72F5E8, OWNERS))
    tag = HANDLE_VALUE if kind != "stale" else HANDLE_VALUE ^ 0x100
    patches.append(_w(OWNER_ROW + 0x2C, tag))
    patches.append(_w(OWNER_ROW + 0x7C, 0 if kind == "null-record" else OWNER_RECORD))
    patches.append(_w(OWNER_RECORD + 0x140, entries))
    patches.append(_w(OWNER_RECORD + 0x164, 3))
    for slot in range(max(entries, 1)):
        key = OWNER_KEY if match == slot else 0x99
        patches.append(_w(OWNER_RECORD + 0x70 + slot * 0x34, key))
        patches.append(_w(OWNER_RECORD + 0x70 + slot * 0x34 + 0x2C, word))
    return (OBJECT,), patches


# --- 0x6BB90 -----------------------------------------------------------------------------------
RECORD_SET, ROWS = _base(0xE1000), _base(0xE2000)
MODES = (0x50, 0x64, 0x70)
BYTES = (0xA, 5)
#: cache states: none (refresh), hit (cached pointer returned).
CACHES = ("refresh", "hit")
ROW_PATTERNS = ((1, 0, 1, 1, 0, 1), (0, 0, 0, 0), (1, 1, 1))
PAIRS = (0, 1, 2)
LIMITS = (0, 5, 0x80000000)
SCAN_MAIN = len(MODES) * len(BYTES) * len(CACHES) * len(ROW_PATTERNS) * len(PAIRS) * len(LIMITS)


def _scan_params(ordinal: int) -> tuple[int, int, str, tuple[int, ...], int, int]:
    rest, limit_slot = divmod(ordinal, len(LIMITS))
    rest, pair_slot = divmod(rest, len(PAIRS))
    rest, pattern_slot = divmod(rest, len(ROW_PATTERNS))
    rest, cache_slot = divmod(rest, len(CACHES))
    mode_slot, byte_slot = divmod(rest, len(BYTES))
    return (
        MODES[mode_slot],
        BYTES[byte_slot],
        CACHES[cache_slot],
        ROW_PATTERNS[pattern_slot],
        PAIRS[pair_slot],
        LIMITS[limit_slot],
    )


SCAN_CASES = SCAN_MAIN


def scan_scenario(ordinal: int) -> tuple[tuple[int, ...], list[Patch]]:
    mode, byte, cache, pattern, pair, limit = _scan_params(ordinal)
    patches = [
        _w(0x79094C, mode),
        _b(0x7DE455, byte),
        _w(0x7DE494, ordinal & 1),
        _w(0x515FFC, 1),
        _w(0x515FFC + 4, mode),
        _w(0x515FFC + 0x20, RECORD_SET),
        _w(0x515FFC + 0x24, RECORD_SET),
        _w(0x515FFC + 0x80, 0),
        _w(0x4D2CE4, mode if cache == "hit" else 0),
        _w(0x4D2CE8, byte if cache == "hit" else 0xFF),
        _w(0x6F32F0, RECORD_SET),
        _w(RECORD_SET + 0x24, ROWS),
        _w(RECORD_SET + 0x28, len(pattern)),
    ]
    for row, hit in enumerate(pattern):
        patches.append(_w(ROWS + row * 0x20, 0x01800000 if hit else 0x00400000))
    return (pair, limit), patches


# --- 0x6B4E0 -----------------------------------------------------------------------------------
PENDING = 0x6F1E70
OWNER_TABLE, PREDICATE_TABLE = (_base(0xF0000 + 0x10000 * i) for i in range(2))
MASKS = (0, 0xFF, 2)
PREDICATE_RESULTS = (True, False)
PENDING_CASES = len(MASKS) * len(PREDICATE_RESULTS) * 2


def _pending_build(
    mask: int, predicate: bool, flip: bool, offset: int
) -> tuple[tuple[int, ...], list[Patch]]:
    patches = [_w(0x72F5E8, OWNER_TABLE), _w(0x74C458, PREDICATE_TABLE)]
    # predicate object 1: 1656D0 reads [table + (1 << 9) + 0x1D0] and tests it > 0.
    patches.append(_w(PREDICATE_TABLE + 0x200 + 0x1D0, 5 if predicate else 0))
    # nine records: (owner row index, record word 4, flags, owner +0x80, owner +0x20, owner +0x28)
    layout = (
        (1, 0x1, 4, 1, 0, 0),
        (2, 0x1, 4, 0, 0, 0),
        (3, 0x1, 4, 0, 0x40, 2 if flip else 0),
        (4, 0x0, 5, 0, 0, 1),
        (5, 0x0, 4, 0, 0, 0),
        (6, 0x1, 4, 0, 0, 0),
        (0, 0x0, 4, 0, 0, 0),
        (7, 0x0, 0, 0, 0, 0),
        (8, 0x0, 1, 0, 0, 0),
    )
    for slot, (row, bits, flags, obj, kind, owner_flags) in enumerate(layout):
        record = PENDING + (slot + offset) * 0xC
        owner = OWNER_TABLE + row * 0x2E8
        handle = ((row << 22) | 0x11) if row else 0
        stale = slot == 5 and flip
        patches.append(_w(record, handle))
        patches.append(_w(record + 4, bits))
        patches.append(_w(record + 8, flags))
        patches.append(_w(owner + 0x2C, handle ^ (0x100 if stale else 0)))
        patches.append(_w(owner + 0x28, owner_flags))
        patches.append(_w(owner + 0x20, kind))
        patches.append(_w(owner + 0x80, 1 if obj else 0))
    return (mask,), patches


def pending_scenario(ordinal: int) -> tuple[tuple[int, ...], list[Patch]]:
    rest, flip = divmod(ordinal, 2)
    mask_slot, predicate_slot = divmod(rest, len(PREDICATE_RESULTS))
    return _pending_build(MASKS[mask_slot], PREDICATE_RESULTS[predicate_slot], bool(flip), 0)


# --- Amendment A3: extra cases APPENDED after the A2 ordinals (A2 ordinals are unchanged) -------
# Written after the A2 reruns of 0x606E0 (78 verdicts), 0x60740 (27), 0x45E60 (26) fell below the
# 100-verdict gate and 0x6B4E0 reached null-agree 0.9021 (> 0.9). Same builders, more values.
A2_COUNTS = {
    0x606E0: RECORD_ROOT_CASES,
    0x60740: COPY_ROOT_CASES,
    0x45E60: LOOKUP_ROOT_CASES,
    0x6B4E0: PENDING_CASES,
}
EXTRA_TABLE_INDEXES = (0x7FFFFFFF, 7, 0xFFFFFFF0, 0x40000000)
EXTRA_CALL_INDEXES = (4, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000)
RECORD_EXTRA_TABLE = len(RECORD_COUNTS) * len(TABLE_FIELDS) * len(EXTRA_TABLE_INDEXES)
RECORD_EXTRA = RECORD_EXTRA_TABLE + len(CACHE_STATES) * len(EXTRA_CALL_INDEXES)
COPY_EXTRA_SOURCES = (1, 9, 0xFFFFFFFD, 0x7FFFFFFF)
COPY_EXTRA = len(DST_VARIANTS) * len(COPY_EXTRA_SOURCES) * len(TABLE_FIELDS)
EXTRA_SKIPS = (0, 1, 2, 3)
EXTRA_WORDS = (0, 0xFFFF, 0x7FFF, 0x8000, 1, 0xFF00)
LOOKUP_EXTRA = len(EXTRA_SKIPS) * len(EXTRA_WORDS) * len(GATES)
EXTRA_MASKS = (1, 0x10, 0x8000)
PENDING_OFFSETS = (0, 100, 300)


def record_extra(ordinal: int) -> tuple[tuple[int, ...], list[Patch]]:
    if ordinal >= RECORD_EXTRA_TABLE:
        state, slot = divmod(ordinal - RECORD_EXTRA_TABLE, len(EXTRA_CALL_INDEXES))
        index = EXTRA_CALL_INDEXES[slot]
        return (index,), [*_call_patches(state), *_call_cache_patches(state, index)]
    rest, slot = divmod(ordinal, len(EXTRA_TABLE_INDEXES))
    count_slot, field_slot = divmod(rest, len(TABLE_FIELDS))
    return (EXTRA_TABLE_INDEXES[slot],), _table_patches(
        RECORD_COUNTS[count_slot], TABLE_FIELDS[field_slot]
    )


def copy_extra(ordinal: int) -> tuple[tuple[int, ...], list[Patch]]:
    rest, field_slot = divmod(ordinal, len(TABLE_FIELDS))
    variant, source = divmod(rest, len(COPY_EXTRA_SOURCES))
    keep16, keep12, keep4 = DST_VARIANTS[variant]
    index = COPY_EXTRA_SOURCES[source]
    field = TABLE_FIELDS[field_slot]
    clamped = 0 if index >= 0x80000000 else min(index, 3)
    record = _table_base(field) + 4 + (clamped + 1) * 64
    patches = _table_patches(4, field)
    patches.extend(_w(record + 4 * word, 0x1000 * (clamped + 1) + word) for word in range(8))
    arguments = (index, DST16 if keep16 else 0, DST12 if keep12 else 0, DST4 if keep4 else 0)
    return arguments, patches


def lookup_extra(ordinal: int) -> tuple[tuple[int, ...], list[Patch]]:
    rest, gate_slot = divmod(ordinal, len(GATES))
    skip_slot, word_slot = divmod(rest, len(EXTRA_WORDS))
    return _lookup_build(EXTRA_SKIPS[skip_slot], EXTRA_WORDS[word_slot], GATES[gate_slot])


def _pending_cases() -> list[tuple[int, bool, bool, int]]:
    """`(mask, predicate, flip, offset)`: new masks at every offset, A2 masks at the new offsets."""
    cases = [
        (mask, predicate, bool(flip), offset)
        for mask in EXTRA_MASKS
        for predicate in PREDICATE_RESULTS
        for flip in (0, 1)
        for offset in PENDING_OFFSETS
    ]
    cases += [
        (mask, predicate, bool(flip), offset)
        for mask in MASKS
        for predicate in PREDICATE_RESULTS
        for flip in (0, 1)
        for offset in PENDING_OFFSETS[1:]
    ]
    return cases


PENDING_EXTRA_TABLE = _pending_cases()
PENDING_EXTRA = len(PENDING_EXTRA_TABLE)


def pending_extra(ordinal: int) -> tuple[tuple[int, ...], list[Patch]]:
    mask, predicate, flip, offset = PENDING_EXTRA_TABLE[ordinal]
    return _pending_build(mask, predicate, flip, offset)


def _extended(base_count: int, base: Scenario, extra: Scenario) -> Scenario:
    def scenario(ordinal: int) -> tuple[tuple[int, ...], list[Patch]]:
        return base(ordinal) if ordinal < base_count else extra(ordinal - base_count)

    return scenario


ROOTS = {
    0x606E0: RECORD_ROOT_CASES + RECORD_EXTRA,
    0x60740: COPY_ROOT_CASES + COPY_EXTRA,
    0x45E60: LOOKUP_ROOT_CASES + LOOKUP_EXTRA,
    0x17A190: OWNER_CASES,
    0x6BB90: SCAN_CASES,
    0x6B4E0: PENDING_CASES + PENDING_EXTRA,
}
_SCENARIOS = {
    0x606E0: _extended(RECORD_ROOT_CASES, record_scenario, record_extra),
    0x60740: _extended(COPY_ROOT_CASES, copy_scenario, copy_extra),
    0x45E60: _extended(LOOKUP_ROOT_CASES, lookup_scenario, lookup_extra),
    0x17A190: owner_scenario,
    0x6BB90: scan_scenario,
    0x6B4E0: _extended(PENDING_CASES, pending_scenario, pending_extra),
}


def batch2_case_count(va: int) -> int:
    return ROOTS.get(va, 0)


def make_batch2_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < batch2_case_count(va):
        raise ValueError("unsupported T1475 batch 2 case")
    case = make_case(seed, BATCH2_INDEX_BASE + ordinal, va, size, policy=policy)
    arguments, patches = _SCENARIOS[va](ordinal)
    extra = [_w(case.esp + 4 + 4 * slot, value) for slot, value in enumerate(arguments)]
    return replace(case, patches=(*case.patches, *extra, *patches))
