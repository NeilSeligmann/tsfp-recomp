# SPDX-License-Identifier: GPL-3.0-or-later
"""T1576 batch A fixture domains for guarded-jump roots; namespace 120, additive only.

The random streams never build the states a jump-table root needs (a stack argument that selects
the arm, a pointer that is mapped, a global that holds the mode), so each root below gets a
frozen Cartesian domain derived from the ORIGINAL bytes (table bound plus two, the compared
constants, the documented state words), never from the replacement's switch. Predeclared in
docs/evidence/t1576/batch-a.md before any final tuple was run. Arena addresses live in the
mapped window outside the scratch arena (0xD00000..0xD20000), the thunk targets and the stack.
"""

from collections.abc import Callable
from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

NAMESPACE = 120
LABEL = "t1576-batch-a"
MASK = 0xFFFFFFFF

Word = Callable[[int, int], None]
Data = Callable[[int, bytes], None]

# --- 0x0003DFA0 / 0x0003E0B0: the arena allocators (docs/t1364-arena-allocator-naming.md) -------
ARENA_REMAINING = 0x6B8384  # R, signed
ARENA_TOP = 0x6B8388  # T, kind 0 descending
ARENA_CURSOR_1 = 0x6B837C
ARENA_CURSOR_2 = 0x6B8370
ARENA_BUDGET_2 = 0x6B8378
ARENA_CURSOR_4 = 0x6B8390
ARENA_TOP_BASE = 0xD80000
ARENA_C1_BASE = 0xD60000
ARENA_C2_BASE = 0xD68000
ARENA_C4_BASE = 0xD70000
KINDS = (0, 1, 2, 3, 4, 5, 0xFFFFFFFF)  # table bound is 4: bound + 2 and a negative kind
SIZES_3DFA0 = (0, 1, 0xF, 0x10, 0x11, 0x100, 0x1001, 0xFFFFFFF1, 0x7FFFFFF0, 0x80000000)
STATES = ("ample", "exact", "short", "negative", "odd")
ALIGNMENTS = (1, 2, 4, 0x10, 0x20, 0, 3)  # valid powers of two plus the documented invalid ones
SIZES_3E0B0 = (0, 1, 0x11, 0x100)
STATES_3E0B0 = ("ample", "odd")


def _arena_words(state: str, kind: int, rounded: int) -> dict[int, int]:
    """Arena state words of one named state; `rounded` is the byte count the arm will debit."""
    exact = (rounded + 0x10) & MASK if kind == 0 else rounded
    remaining = {
        "ample": 0x00100000,
        "exact": exact,
        "short": (exact - 1) & MASK,
        "negative": 0xFFFFFFF0,
        "odd": 0x00100000,
    }[state]
    odd = 1 if state == "odd" else 0
    return {
        ARENA_REMAINING: remaining,
        ARENA_TOP: ARENA_TOP_BASE + odd * 7,
        ARENA_CURSOR_1: ARENA_C1_BASE + odd,
        ARENA_CURSOR_2: ARENA_C2_BASE + odd * 3,
        ARENA_BUDGET_2: 0x00080000,
        ARENA_CURSOR_4: ARENA_C4_BASE + odd * 5,
    }


def _count_3dfa0() -> int:
    return len(KINDS) * len(SIZES_3DFA0) * len(STATES)


def _build_3dfa0(ordinal: int, word: Word, arg: Word, data: Data) -> None:
    kind = KINDS[ordinal % len(KINDS)]
    size = SIZES_3DFA0[ordinal // len(KINDS) % len(SIZES_3DFA0)]
    state = STATES[ordinal // (len(KINDS) * len(SIZES_3DFA0))]
    rounded = (size + 15) & 0xFFFFFFF0 & MASK
    for address, value in _arena_words(state, kind, rounded).items():
        word(address, value)
    arg(0, size)
    arg(1, kind)


def _count_3e0b0() -> int:
    return len(ALIGNMENTS) * len(SIZES_3E0B0) * len(KINDS) * len(STATES_3E0B0)


def _build_3e0b0(ordinal: int, word: Word, arg: Word, data: Data) -> None:
    alignment = ALIGNMENTS[ordinal % len(ALIGNMENTS)]
    size = SIZES_3E0B0[ordinal // len(ALIGNMENTS) % len(SIZES_3E0B0)]
    kind = KINDS[ordinal // (len(ALIGNMENTS) * len(SIZES_3E0B0)) % len(KINDS)]
    state = STATES_3E0B0[ordinal // (len(ALIGNMENTS) * len(SIZES_3E0B0) * len(KINDS))]
    rounded = (size + alignment + 0x1F) & 0xFFFFFFF0 & MASK
    for address, value in _arena_words(state, kind, rounded).items():
        word(address, value)
    arg(0, alignment)
    arg(1, size)
    arg(2, kind)


# --- 0x0008E380: release texture entries, tail jmp to 389B0 ------------------------------------
TABLE_PTR_GLOBAL = 0x4B85C8  # -> pointer to the 0x34-byte record array (flag dword at +0x18)
SLOT_COUNT_GLOBAL = 0x54EF10  # 18300 slot table: count here, 16-byte slots from 0x54EF30
SLOT_BASE = 0x54EF30
RECORD_POINTER_CELL = 0xD90000
RECORD_ARRAY = 0xD90100
BLOB = 0xDA0000
BLOB_ENTRIES = 0xDA0100
BLOB_KEYS = (1, 2, 3)
BLOB_VARIANTS = 2 + 4 * 8 * 4  # null blob, null entries, then (records, flag mask, slot table)
SLOT_TABLES = (
    (0, (0, 0)),  # count 0: 18300 returns at once
    (2, (1, 2)),  # slots match the first two keys
    (2, (7, 8)),  # no slot matches
    (1, (2, 9)),  # only the second key matches, and only one slot is counted
)


def _count_8e380() -> int:
    return BLOB_VARIANTS


def _build_8e380(ordinal: int, word: Word, arg: Word, data: Data) -> None:
    if ordinal == 0:
        arg(0, 0)
        return
    word(BLOB + 0x10, 0 if ordinal == 1 else BLOB_ENTRIES)
    arg(0, BLOB)
    if ordinal == 1:
        return
    ordinal -= 2
    records = ordinal % 4
    flags = ordinal // 4 % 8
    count, entries = SLOT_TABLES[ordinal // 32]
    word(TABLE_PTR_GLOBAL, RECORD_POINTER_CELL)
    word(RECORD_POINTER_CELL, RECORD_ARRAY)
    for index, key in enumerate(BLOB_KEYS):
        word(RECORD_ARRAY + key * 0x34 + 0x18, 0x08000000 if flags >> index & 1 else 0x00000040)
        word(BLOB_ENTRIES + 16 * index, key if index < records else MASK)
    word(BLOB_ENTRIES + 16 * records, MASK)
    word(SLOT_COUNT_GLOBAL, count)
    for slot, key in enumerate(entries):
        word(SLOT_BASE + 16 * slot, key)


# --- 0x000379C0: feedback reason text ids (single-level table, 10 slots) --------------------------
TEXT_TABLE_GLOBAL = 0x6B7B08  # 3D7B0 reads [this + id * 4 - 0x361C]
TEXT_BASE = 0xDB0000
TEXT_IDS = (
    0x1C25,
    0x1C26,
    0x1C28,
    0x1C27,
    0x1C29,
    0x1C2A,
    0x1C2B,
    0x1C2C,
    0x1C2D,
    0x1C2E,
    0x1C2F,
    0x1C8B,
)
FEEDBACK_REQUESTS = (
    *((0, kind) for kind in (*range(12), 0x7FFFFFFF, 0xFFFFFFFF)),
    *((1, kind) for kind in (0, 9, 10, 11)),
    *((2, kind) for kind in (0, 10, 11)),
    *((flag, kind) for flag in (3, 0xFFFFFFFF) for kind in (0, 10)),
)


FEEDBACK_REPEATS = 16  # same request, different random registers and scratch (null-agree gate)


def _count_379c0() -> int:
    return len(FEEDBACK_REQUESTS) * 2 * FEEDBACK_REPEATS


def _build_379c0(ordinal: int, word: Word, arg: Word, data: Data) -> None:
    flag, kind = FEEDBACK_REQUESTS[ordinal % len(FEEDBACK_REQUESTS)]
    arg(0, kind)
    arg(1, flag)
    if ordinal // len(FEEDBACK_REQUESTS) % 2:
        word(TEXT_TABLE_GLOBAL, TEXT_BASE)
        for text_id in TEXT_IDS:
            word(TEXT_BASE + text_id * 4 - 0x361C, 0x12340000 + text_id)
    else:
        word(TEXT_TABLE_GLOBAL, 0)


# --- 0x000BE860 / 0x000C0E60: object type entry address (two-level table, 7 slots) --------------
TYPE_MASKS = (
    *range(10), 0xF, 0x10, 0x11, 0x20, 0x21, 0x3F, 0x40, 0x41, 0x7F, 0x80, 0x81, 0x100, 0x101,
    0x200, 0x300, 0x400, 0x401, 0x800, 0x801, 0x1000, 0x1001, 0xFFFFFFFF, 0x80000000,
)  # fmt: skip
TYPE_INDEXES = (0, 1, 5, 0x2B, 0x45, 0xFFFFFFFF)
WEAPON_SLOT_TABLE = 0x4FA138  # 1B99B0: [index * 0x3C + 4FA138] = weapon id, valid 0..0x41
WEAPON_POOL_GLOBAL = 0x4F9BAC  # [pool + id * 0x29C + 0x14] = weapon slot returned
WEAPON_POOL = 0xDC0000
WEAPON_STATES = ((1, 5), (0x41, 0x2B), (1, 0x2C), (0x42, 5))


def _weapon_state(word: Word, index: int, state: int) -> None:
    weapon, slot = WEAPON_STATES[state]
    word(WEAPON_POOL_GLOBAL, WEAPON_POOL)
    if index < 0x46:
        word(WEAPON_SLOT_TABLE + index * 0x3C, weapon)
    if weapon < 0x42:
        word(WEAPON_POOL + weapon * 0x29C + 0x14, slot)


def _be860_requests() -> list[tuple[int, int, int]]:
    plain = [(mask, index, 0) for mask in TYPE_MASKS for index in TYPE_INDEXES]
    weapon = [(0x40, index, state) for index in TYPE_INDEXES for state in range(1, 4)]
    return plain + weapon


BE860_REQUESTS = _be860_requests()


def _count_be860() -> int:
    return len(BE860_REQUESTS)


def _build_be860(ordinal: int, word: Word, arg: Word, data: Data) -> None:
    mask, index, state = BE860_REQUESTS[ordinal]
    arg(0, mask)
    arg(1, index)
    _weapon_state(word, index, state)


C0E60_TYPES = (0x40, 1, 2, 3, 8, 0x10, 0x20, 0x41, 0x80, 0x100, 0x200, 0x400, 0x800, 0x1000, 0)
C0E60_INDEXES = (0, 5, 0x45, 0x46, 0xFFFFFFFF)
C0E60_TAILS = ((0, 5), (0, 0), (1, 5), (0, 0x2C), (0, 0xFFFFFFFF))


def _c0e60_requests() -> list[tuple[int, int, int, int, int]]:
    out = []
    for kind in C0E60_TYPES:
        for index in C0E60_INDEXES:
            for hint, weapon in C0E60_TAILS:
                out.append((kind, index, hint, weapon, 0))
                if kind == 0x40:
                    out.extend((kind, index, hint, weapon, state) for state in range(1, 4))
    return out


C0E60_REQUESTS = _c0e60_requests()


def _count_c0e60() -> int:
    return len(C0E60_REQUESTS)


def _build_c0e60(ordinal: int, word: Word, arg: Word, data: Data) -> None:
    kind, index, hint, weapon, state = C0E60_REQUESTS[ordinal]
    arg(0, kind)
    arg(1, index)
    arg(3, hint)
    arg(4, weapon)
    _weapon_state(word, index, state)


# --- 0x000D6ED0: character type id (byte map over id - 0x12 + two-slot table in D6DD0) ----------
D6ED0_IDS = (*range(0xC0), 0x100, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)


def _count_d6ed0() -> int:
    return len(D6ED0_IDS)


def _build_d6ed0(ordinal: int, word: Word, arg: Word, data: Data) -> None:
    arg(0, D6ED0_IDS[ordinal])


# --- 0x00045A70: music volume flag through 1CEB00 (byte map over mode - 8, two-slot table) ------
MODE_GLOBAL = 0x79094C
MODE_CACHE = 0x4FCE6C
MODE_FLAG = 0x75AF10
SETTING_NEAR = 0x5236B4
SETTING_FAR = 0x5236B8
MODES = (*range(0x5A), 0xFFFFFFFF, 0x80000000)
FLAG_PAIRS = ((0, 0), (0, 1), (1, 0), (1, 1))


def _count_45a70() -> int:
    return len(MODES) * 3 * len(FLAG_PAIRS)


def _build_45a70(ordinal: int, word: Word, arg: Word, data: Data) -> None:
    mode = MODES[ordinal % len(MODES)]
    variant = ordinal // len(MODES) % 3  # 0 cache miss, 1 hit with flag 0, 2 hit with flag 1
    near, far = FLAG_PAIRS[ordinal // (len(MODES) * 3)]
    word(MODE_GLOBAL, mode)
    word(MODE_CACHE, mode if variant else (~mode) & MASK)
    word(MODE_FLAG, 1 if variant == 2 else 0)
    word(SETTING_NEAR, near)
    word(SETTING_FAR, far)


# --- 0x000E05B0: entries of table 737BD4 with +0x158 == owner, E0540 increments counters --------
ENTRY_TABLE_GLOBAL = 0x737BD4
ENTRY_FIRST_GLOBAL = 0x736680
ENTRY_LAST_GLOBAL = 0x737E44
ENTRY_TABLE = 0xDD0000
ENTRY_STRIDE = 0x22C
RESULT_BLOCK = 0xDE0000
OWNER = 0x1234
ENTRY_TYPES = (
    0xE, 0xF, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x1F, 0x20, 0x21, 0x22, 0x40, MASK, 0,
)  # fmt: skip
ENTRY_RANGES = ((0, MASK), (0, 0), (0, 2), (0, 7), (2, 5), (3, 3), (5, 2), (1, 6))


def _count_e05b0() -> int:
    return len(ENTRY_RANGES) * 32


def _build_e05b0(ordinal: int, word: Word, arg: Word, data: Data) -> None:
    first, last = ENTRY_RANGES[ordinal % len(ENTRY_RANGES)]
    pattern = ordinal // len(ENTRY_RANGES)
    arg(0, OWNER)
    arg(1, RESULT_BLOCK)
    word(ENTRY_TABLE_GLOBAL, ENTRY_TABLE)
    word(ENTRY_FIRST_GLOBAL, first)
    word(ENTRY_LAST_GLOBAL, last)
    for index in range(8):
        entry = ENTRY_TABLE + index * ENTRY_STRIDE
        word(entry, ENTRY_TYPES[(pattern * 3 + index * 7 + ordinal) % len(ENTRY_TYPES)])
        word(entry + 0x158, OWNER if pattern >> (index % 5) & 1 else OWNER + 1)
    for offset in range(0, 0x1C, 4):
        word(RESULT_BLOCK + offset, 0xA5A5A5A5)


# --- 0x00061FD0: pak slot allocation with a name copy and a caller-chosen arena kind ------------
PAK_COUNT = 0x6D7880
PAK_SLOTS = 0x6D7900
NAME_BASE = 0xDF0000
NAMES = (b"\x00", b"a\x00", b"abcd\x00")
PAK_COUNTS = (0, 1, 3)
PAK_FREE = (None, 0, 2)  # slot that holds the -1 free marker (None: every slot is in use)
PAK_KINDS = (0, 1, 2, 3, 4, 5, 0xFFFFFFFF)


def _count_61fd0() -> int:
    return len(PAK_COUNTS) * len(PAK_FREE) * len(NAMES) * len(PAK_KINDS) * 2


def _build_61fd0(ordinal: int, word: Word, arg: Word, data: Data) -> None:
    kind = PAK_KINDS[ordinal % len(PAK_KINDS)]
    name = NAMES[ordinal // len(PAK_KINDS) % len(NAMES)]
    free = PAK_FREE[ordinal // (len(PAK_KINDS) * len(NAMES)) % len(PAK_FREE)]
    count = PAK_COUNTS[ordinal // (len(PAK_KINDS) * len(NAMES) * len(PAK_FREE)) % len(PAK_COUNTS)]
    state = (
        "odd"
        if ordinal // (len(PAK_KINDS) * len(NAMES) * len(PAK_FREE) * len(PAK_COUNTS))
        else "ample"
    )
    rounded = (len(name) + 15) & 0xFFFFFFF0
    for address, value in _arena_words(state, kind, rounded).items():
        word(address, value)
    word(PAK_COUNT, count)
    for slot in range(4):
        word(PAK_SLOTS + slot * 0x1C + 0x18, MASK if free == slot and slot < count else 0)
    data(NAME_BASE, name)
    arg(0, NAME_BASE)
    arg(1, kind)


#: va -> (case count, builder). A builder gets word(address, value), arg(index, value) and
#: data(address, bytes).
DOMAINS: dict[int, tuple[Callable[[], int], Callable[..., None]]] = {
    0x3DFA0: (_count_3dfa0, _build_3dfa0),
    0x3E0B0: (_count_3e0b0, _build_3e0b0),
    0x8E380: (_count_8e380, _build_8e380),
    0x379C0: (_count_379c0, _build_379c0),
    0xBE860: (_count_be860, _build_be860),
    0xC0E60: (_count_c0e60, _build_c0e60),
    0xD6ED0: (_count_d6ed0, _build_d6ed0),
    0x45A70: (_count_45a70, _build_45a70),
    0xE05B0: (_count_e05b0, _build_e05b0),
    0x61FD0: (_count_61fd0, _build_61fd0),
}
SUPPORTED_VAS = tuple(DOMAINS)


def guarded_case_count(va: int) -> int:
    return DOMAINS[va][0]() if va in DOMAINS else 0


def make_guarded_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va not in DOMAINS or not 0 <= ordinal < guarded_case_count(va):
        raise ValueError("unsupported T1576 batch A case")
    case = make_case(seed, (NAMESPACE << 40) + ordinal, va, size, policy=policy)
    patches = list(case.patches)

    def word(address: int, value: int) -> None:
        patches.append((address, (value & MASK).to_bytes(4, "little")))

    def arg(index: int, value: int) -> None:
        word(case.esp + 4 + 4 * index, value)

    def data(address: int, content: bytes) -> None:
        patches.append((address, content))

    DOMAINS[va][1](ordinal, word, arg, data)
    return replace(case, patches=tuple(patches))
