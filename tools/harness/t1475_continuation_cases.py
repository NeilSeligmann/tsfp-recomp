# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace-60 amended domains for the T1475/T1477 continuation, batch 1.

Written after the default-domain outcomes of 0x61EE0 (every random case faulted in the
original, zero verdicts) and 0x406A0 (coverage 0.838 below the 0.9 gate) were seen. The
declaration is docs/t1475-t1477-continuation-batches.md, "Batch 1 amendment A1". Additive to
the seed stream: registers and unpatched words keep the seed stream values.
"""

from dataclasses import replace

from .model import Case
from .seeding import SCRATCH_BASE, SeedPolicy, make_case

CONTINUATION_INDEX_BASE = 60 << 40

# --- 0x61EE0 (slot): gate bits, slot table, tag table, two mask groups ----------------------
GATE_WORD = 0x7DE458
SLOT_TABLE_GLOBAL = 0x7356D8
TAG_TABLE_GLOBAL = 0x7B0C48
GROUP_BASE_WORDS = (0x6D7844, 0x6D7848)
GROUP_COUNT_WORDS = (0x6D784C, 0x6D7850)
SLOT_TABLE = SCRATCH_BASE + 0x50000
TAG_TABLE = SCRATCH_BASE + 0x60000
GROUPS = (SCRATCH_BASE + 0x70000, SCRATCH_BASE + 0x71000)
#: Entry stride 0x18, value at +0, mask at +0xC.
GROUP_ENTRIES = (
    ((0x1000, 0x00000001), (0x1001, 0x00000020), (0x1002, 0x80000000)),
    ((0x2000, 0x00000010), (0x2001, 0x00000040)),
)
GATES = (0, 0x200, 0x20000000, 0x20000200, 0x1)
SLOTS = (0, 1, 2)
#: Slot record word at +0x678: -1 is "unused".
SLOT_WORDS = (0x55, 0xFFFFFFFF)
#: tag -> expected family: group0 bits 0/5/31, group1 bits 4/6, absent bit 7, wrapped shifts.
TAGS = (0, 5, 31, 4, 6, 7, 0x25, 0x1FF)
#: (group0 count, group1 count) overrides appended after the main grid.
COUNT_QUIRKS = ((0, 2), (0xFFFFFFFF, 2), (3, 0), (3, 0xFFFFFFFF))
SLOT_MAIN = len(GATES) * len(SLOTS) * len(SLOT_WORDS) * len(TAGS)
SLOT_CASES = SLOT_MAIN + len(COUNT_QUIRKS) * len(TAGS)

# --- 0x406A0 (text, limit): comma scanner with digit-parsing callee -------------------------
SCAN_FLAG_GLOBAL = 0x6B93A8
TEXT_BASE = SCRATCH_BASE + 0x80000
TEXTS = (
    b"",
    b"abc",
    b"1,2,3",
    b",,,",
    b"12,34,56,78",
    b"a,b",
    b"7,99999,x",
    b",12345678,9",
    b"1,2,3,4,5,6,7,8,9,0",
)
LIMITS = (0, 1, 2, 5, 100, 0xFFFFFFFF, 0x80000000)
#: (flag, text index, limit): flag-zero shortcut cases come last.
SCAN_MAIN = len(TEXTS) * len(LIMITS)
SCAN_ZERO_FLAG = 4
SCAN_CASES = SCAN_MAIN + SCAN_ZERO_FLAG

CONTINUATION_ROOTS = {0x61EE0: SLOT_CASES, 0x406A0: SCAN_CASES}


def continuation_case_count(va: int) -> int:
    return CONTINUATION_ROOTS.get(va, 0)


def _words(*values: int) -> bytes:
    return b"".join((value & 0xFFFFFFFF).to_bytes(4, "little") for value in values)


def slot_parameters(ordinal: int) -> tuple[int, int, int, int, tuple[int, int]]:
    """`(gate, slot, slot_word, tag, group counts)` for a 0x61EE0 ordinal."""
    if ordinal >= SLOT_MAIN:
        quirk, tag_index = divmod(ordinal - SLOT_MAIN, len(TAGS))
        return 0, 1, SLOT_WORDS[0], TAGS[tag_index], COUNT_QUIRKS[quirk]
    rest, tag_index = divmod(ordinal, len(TAGS))
    rest, word_index = divmod(rest, len(SLOT_WORDS))
    gate_index, slot_index = divmod(rest, len(SLOTS))
    counts = (len(GROUP_ENTRIES[0]), len(GROUP_ENTRIES[1]))
    return GATES[gate_index], SLOTS[slot_index], SLOT_WORDS[word_index], TAGS[tag_index], counts


def _slot_patches(ordinal: int) -> list[tuple[int, bytes]]:
    gate, slot, slot_word, tag, counts = slot_parameters(ordinal)
    patches = [
        (GATE_WORD, _words(gate)),
        (SLOT_TABLE_GLOBAL, _words(SLOT_TABLE)),
        (TAG_TABLE_GLOBAL, _words(TAG_TABLE)),
        (GROUP_BASE_WORDS[0], _words(GROUPS[0])),
        (GROUP_BASE_WORDS[1], _words(GROUPS[1])),
        (GROUP_COUNT_WORDS[0], _words(counts[0])),
        (GROUP_COUNT_WORDS[1], _words(counts[1])),
        (SLOT_TABLE + slot * 0xEA0 + 0x678, _words(slot_word)),
        (TAG_TABLE + slot * 0x1584 + 0x1568, _words(tag)),
    ]
    for base, entries in zip(GROUPS, GROUP_ENTRIES, strict=True):
        for index, (value, mask) in enumerate(entries):
            patches.append((base + index * 0x18, _words(value)))
            patches.append((base + index * 0x18 + 0xC, _words(mask)))
    return patches


def scan_parameters(ordinal: int) -> tuple[int, bytes, int]:
    """`(flag, text, limit)` for a 0x406A0 ordinal."""
    if ordinal >= SCAN_MAIN:
        return 0, TEXTS[2], LIMITS[4 + (ordinal - SCAN_MAIN) % 2]
    text_index, limit_index = divmod(ordinal, len(LIMITS))
    return 1, TEXTS[text_index], LIMITS[limit_index]


def make_continuation_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < continuation_case_count(va):
        raise ValueError("unsupported T1475 continuation case")
    case = make_case(seed, CONTINUATION_INDEX_BASE + ordinal, va, size, policy=policy)
    if va == 0x61EE0:
        slot = slot_parameters(ordinal)[1]
        extra = [(case.esp + 4, _words(slot)), *_slot_patches(ordinal)]
    else:
        flag, text, limit = scan_parameters(ordinal)
        extra = [
            (case.esp + 4, _words(TEXT_BASE, limit)),
            (SCAN_FLAG_GLOBAL, _words(flag)),
            (TEXT_BASE, text + b"\0" * 8),
        ]
    return replace(case, patches=(*case.patches, *extra))
