# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared namespace 55 domains for T1479's fourth bounded batch (ten session/table roots).

Contracts and ordinal layouts: docs/evidence/t1479/fourth-batch-predeclaration.md (written and
committed before any audit, native run or proof outcome). Every ordinal's global/table state
and stack frame derive from the declared tuples below, never from observed results. Registers
and unpatched argument words keep the seed stream values.
"""

import hashlib

from .model import Case
from .seeding import SeedPolicy, make_case
from .t1479_batch3_cases import _Builder, _digits

BATCH4_INDEX_BASE = 55 << 40

_MODE = 0x79094C
_NET = 0x76B0FC  # 42870 network flag (not the 76B108 session flag)
_FLAG_TABLE = 0x761C48
_FLAGS = 0xD70200  # flag words of 2E4C10 / 2E4CB0, stride 0x40
_A = 0xD70000
_B = 0xD70100
_ROW_BASE = 0x521A18
_ROW_STRIDE = 0x30

_G458 = (0, 0x20000000, 0x40, 0x20000040)
_INDEX_OUT = (0xFFFFFFFF, 4, 0x7FFFFFFF, 0x80000000, 0x80000001, 5, 100, 0xFFFFFFFC)

# 2BE5E0 table rows patched by this domain: row -> (key, tag byte).
_KEYS = {0: (0xAB000000, 3), 1: (0xAB000001, 0xFE), 7: (0xAB000007, 1), 8: (0xAB000007, 2)}
_KEYS[146] = (0xAB000092, 5)
_QUERY_KEYS = (0xAB000000, 0xAB000001, 0xAB000007, 0xAB000092, 0x7FFF1234)
_QUERY_TAG = {0xAB000000: 3, 0xAB000001: 0xFE, 0xAB000007: 1, 0xAB000092: 5, 0x7FFF1234: 0}
_QUERY_B = (0xFFFFFFFF, 0x80000000, None, 100, 2, 1)

_SPEC = {
    0x2E4C10: (1, 384 + len(_INDEX_OUT) + 2),
    0x2E4470: (0, 3 * 5 * 3 * 4 + 1 + 220),
    0x2CB1E0: (0, 5 * 2 * 2 * 4 * 4),
    0x2D74B0: (1, 5 * 2 * 3 * 8 + 200),
    0x361950: (3, 6 * 2 * 3 + 4),
    0x36EBE0: (5, 6 * 3 * 3 + 14 + 120),
    0x2BE5E0: (2, 5 * len(_QUERY_B)),
    0x2875F0: (1, 8 * 3 * 3 + 160),
    0x2E4CB0: (0, 4 * 4 * 2 * 2 * 3),
    0x3381D0: (1, 8 * 6),
}


def _stream(tag: str, ordinal: int) -> bytes:
    """Amendment bytes: SHA-256 of `<tag>:<ordinal>:<block>` for blocks 0 and 1 (64 bytes)."""
    return b"".join(
        hashlib.sha256(f"{tag}:{ordinal}:{block}".encode()).digest() for block in range(2)
    )


def _hash_word(stream: bytes, slot: int) -> int:
    return int.from_bytes(stream[4 * slot : 4 * slot + 4], "little")


def batch4_case_count(va: int) -> int:
    return _SPEC.get(va, (0, 0))[1]


def _string(length: int, salt: int) -> bytes:
    """Nonzero printable bytes then a terminator (omitted past 0x28 bytes)."""
    body = bytes(0x41 + (salt + index * 7) % 26 for index in range(length))
    return body + b"\x00" if length <= 0x28 else body


def make_batch4_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    arguments, total = _SPEC.get(va, (0, 0))
    if not 0 <= ordinal < total:
        raise ValueError("unsupported T1479 fourth-batch case")
    case = make_case(seed, BATCH4_INDEX_BASE + ordinal, va, size, policy=policy)
    builder = _Builder(case, arguments)
    word, byte = builder.word, builder.byte

    if va == 0x2E4C10:
        grid = 384
        alias = None
        mode, flag_word, g458, live, state, index = 0x64, 0, 0, 1, 5, 0
        if ordinal < grid:
            index_pick, mode_index, flag_index, g_index, live_index, state_index = _digits(
                ordinal, 2, 2, 4, 4, 2, 3
            )
            index = (0, 3)[index_pick]
            mode = (0x64, 0x65)[mode_index]
            flag_word = (0, 1, 0x200, 0x201)[flag_index]
            g458 = _G458[g_index]
            live = (1, 0)[live_index]
            state = (5, 2, 3)[state_index]
        elif ordinal < grid + len(_INDEX_OUT):
            index = _INDEX_OUT[ordinal - grid]
            flag_word = 0x201
        else:
            alias = ordinal - grid - len(_INDEX_OUT)
            index, flag_word, g458, live, state = 1, 1, 0x40, 1, 5
        for slot in range(4):
            word(_FLAG_TABLE + 4 * slot, _FLAGS + 0x40 * slot)
            word(_FLAGS + 0x40 * slot, flag_word if slot == index else 0x55)
        word(_MODE, mode)
        word(0x7DE458, g458)
        word(0x774024, live)
        word(0x6B98A4, state)
        builder.argument(0, index)
        if alias is not None:
            builder.move_entry((0x77402C, 0x6B98AC)[alias], index)
    elif va == 0x2E4470:
        if ordinal < 180:
            b455, limit, probe, g458 = _digits(ordinal, 3, 5, 3, 4)
            byte(0x7DE455, (0xA, 9, 0)[b455])
            word(0x790950, (2, 1, 0, 0xFFFFFFFF, 5)[limit])
            word(0x7330D8, (0, 1, 0xFFFFFFFF)[probe])
            word(0x7DE458, (0, 0x200, 0x100, 0xFFFFFDFF)[g458])
        elif ordinal == 180:
            byte(0x7DE455, 0xA)
            word(0x790950, 2)
            word(0x7DE458, 0)
            builder.move_entry(0x7330D8 + 4)
        else:
            # Amendment A3 (written AFTER null-agree 0.9866): active-state answer-1 ordinals.
            stream = _stream("t1479-batch4-a3", ordinal)
            probe = _hash_word(stream, 1)
            byte(0x7DE455, 0xA)
            word(0x790950, 2 + _hash_word(stream, 0) % 4)
            word(0x7330D8, probe)
            word(0x7DE458, _hash_word(stream, 2) | (0x200 if probe == 0 else 0))
    elif va == 0x2CB1E0:
        index, bit, player, limit, field = _digits(ordinal, 5, 2, 2, 4, 4)
        idx_value = (0, 1, 2, 5, 0xFFFFFFFF)[index]
        word(0x78AD40, idx_value)
        word(0x7DE458, (0, 0x800000)[bit])
        word(0x7BEDC0, player)
        word(0x761260, (0xFFFFFFFB, 7, 0x80000000, 0)[limit])
        base = 0x6F11C0 if (bit or not player) else 0x7C0050
        word(base + idx_value * 24 + 0x490 + 0xC, (0xFFFFFFFB, 7, 8, 0x7FFFFFFF)[field])
    elif va == 0x2D74B0:
        if ordinal >= 240:
            # Amendment A2 (written AFTER null-agree 0.9870): answer-1 ordinals, network flag
            # clear, a connected pad whose buttons overlap the mask.
            stream = _stream("t1479-batch4-a2", ordinal)
            pattern = 1 + _hash_word(stream, 0) % 15
            hit = next(pad for pad in range(4) if (pattern >> pad) & 1)
            word(0x6B9888, 0)
            for pad in range(4):
                word(0x6B93D4 + pad * 0x3C, (pattern >> pad) & 1)
                word(0x6B93E0 + pad * 0x3C, _hash_word(stream, 1 + pad) | (4 if pad == hit else 0))
            builder.argument(0, 4 | (_hash_word(stream, 5) & 0xF0000000))
            return builder.build()
        connected, g9888, buttons, mask = _digits(ordinal, 5, 2, 3, 8)
        pattern = (0b0000, 0b0001, 0b0110, 0b1111, 0b1000)[connected]
        sets = ((0, 0, 0, 0), (1, 2, 4, 8), (0x10, 0x20, 0xFFFF0000, 0x80000000))[buttons]
        word(0x6B9888, g9888)
        for pad in range(4):
            word(0x6B93D4 + pad * 0x3C, (1, 0xFFFF0000)[pad & 1] if (pattern >> pad) & 1 else 0)
            word(0x6B93E0 + pad * 0x3C, sets[pad])
        builder.argument(0, (0, 1, 2, 4, 8, 0x10, 0xFFFFFFFF, 3)[mask])
    elif va == 0x361950:
        main = 6 * 2 * 3
        string_pointer, struct_pointer = _A, _B
        if ordinal < main:
            length_index, struct_index, value_index = _digits(ordinal, 6, 2, 3)
            length = (0, 1, 7, 0x27, 0x28, 0x30)[length_index]
            struct_words = (
                (0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666),
                (0xFFFFFFFF, 0, 0x80000000, 1, 0x7FFFFFFF, 0xDEADBEEF),
            )[struct_index]
            value = (0, 0x12345678, 0xFFFFFFFF)[value_index]
        else:
            alias = ordinal - main
            length, value = 7, 0x12345678
            struct_words = (1, 2, 3, 4, 5, 6)
            if alias == 0:
                struct_pointer = 0x773EE8
            elif alias == 1:
                string_pointer = 0x773F00
            elif alias == 2:
                struct_pointer = 0x773F00
            else:
                struct_pointer = 0x773F10
        builder.patches.append((0x773F00, b"\xee" * 0x2C))
        builder.patches.append((_A, _string(length, 3)))
        builder.patches.append((0x773EE8, b"\xdd" * 0x18))
        if string_pointer == 0x773F00:
            builder.patches.append((0x773F00, _string(7, 5)))
        for index, struct_word in enumerate(struct_words):
            word(struct_pointer + 4 * index, struct_word)
        builder.argument(0, string_pointer)
        builder.argument(1, struct_pointer)
        builder.argument(2, value)
    elif va == 0x36EBE0:
        main = 6 * 3 * 3
        for index in range(10):
            word(0x4D2D64 + index * 24, 0x1000 + index)
        source, dest = _B, _A
        a4 = 0
        a2, a3 = 0x11223344, 0x55667788
        stream = b""
        if ordinal >= main + 14:
            # Amendment A4 (written AFTER 76 / 71 verdicts): disjoint objects, hash words.
            stream = _stream("t1479-batch4-a4", ordinal)
            a2, a3, a4 = _hash_word(stream, 0), _hash_word(stream, 1), _hash_word(stream, 2) % 10
            dest = 0xD71000
            for index in range(0x30 // 4):
                word(dest + 4 * index, 0xA0A00000 + index)
        elif ordinal < main:
            index, a2_index, a3_index = _digits(ordinal, 6, 3, 3)
            a4 = (0, 1, 2, 5, 7, 9)[index]
            a2 = (0, 0x11223344, 0xFFFFFFFF)[a2_index]
            a3 = (0, 0x55667788, 0xFFFFFFFF)[a3_index]
        elif ordinal < main + 14:
            alias = ordinal - main
            a4 = (0, 5)[alias // 7]
            dest = (source + (0, 4, 8, 0xC, 0xFFFFFFFC, 0xFFFFFFF8, 0x10)[alias % 7]) & 0xFFFFFFFF
        for index in range(0x30 // 4):
            word(_A + 4 * index, 0xA0A00000 + index)
        for index in range(4):
            word(source + 4 * index, 0xC0DE0001 * (index + 1))
            if stream:
                word(source + 4 * index, _hash_word(stream, 4 + index))
        builder.argument(0, dest)
        builder.argument(1, source)
        builder.argument(2, a2)
        builder.argument(3, a3)
        builder.argument(4, a4)
    elif va == 0x2BE5E0:
        key_index, b_index = divmod(ordinal, len(_QUERY_B))
        key = _QUERY_KEYS[key_index]
        for row, (row_key, tag) in _KEYS.items():
            word(_ROW_BASE + 4 + _ROW_STRIDE * row, row_key)
            byte(_ROW_BASE + 1 + _ROW_STRIDE * row, tag)
        tag = _QUERY_TAG[key]
        b_value = _QUERY_B[b_index]
        if b_value is None:
            b_value = tag | (0xFFFFFF00 if tag & 0x80 else 0)
        builder.argument(0, key)
        builder.argument(1, b_value)
    elif va == 0x2875F0:
        word(_A + 4, _B)
        if ordinal >= 72:
            # Amendment A1 (written AFTER null-agree 0.9483 / 0.9470): matching tag, hash payload.
            stream = _stream("t1479-batch4-a1", ordinal)
            word(_MODE, (1, 0x11)[ordinal % 2])
            word(_B + 0xD4, (0x19C, 0x23B)[ordinal % 2])
            word(_B + 0x124, (_hash_word(stream, 0) | 0x00010000) & 0x7FFFFFFF)
        else:
            mode_index, tag_index, payload_index = _digits(ordinal, 8, 3, 3)
            word(_MODE, (1, 0x11, 0, 2, 0x10, 0x12, 0xFFFFFFFF, 0x7FFFFFFF)[mode_index])
            word(_B + 0xD4, (0x19C, 0x23B, 0x100)[tag_index])
            word(_B + 0x124, (0x11111111, 0, 0xFFFFFFFF)[payload_index])
        builder.argument(0, _A)
    elif va == 0x2E4CB0:
        x_index, slot_index, flag, network, value = _digits(ordinal, 4, 4, 2, 2, 3)
        word(0x7B0C7C, _A)
        word(_A, x_index)
        word(_A + 4, slot_index)
        for slot in range(4):
            word(_FLAG_TABLE + 4 * slot, _FLAGS + 0x40 * slot)
            word(_FLAGS + 0x40 * slot, flag if slot == slot_index else 0x54)
        word(_NET, network)
        for entry in range(4):
            word(0x6B93C0 + 4 * entry, (entry + 1) % 4)
        word(0x7B0CC0, (0, 0x100, 0xFFFFFFE0)[value])
    else:
        count, table_value = divmod(ordinal, 6)
        n = (1, 2, 3, 4, 5, 0, 0x7FFFFFFF, 0x80000000)[count]
        value = (0x02000000, 0, 0xFDFFFFFF, 0xFFFFFFFF, 0x01000000, 0x03000000)[table_value]
        word(_A + 0xF54, n)
        word(0x703460 + (((n - 1) * 4) & 0xFFFFFFFF), value)
        builder.argument(0, _A)
    return builder.build()
