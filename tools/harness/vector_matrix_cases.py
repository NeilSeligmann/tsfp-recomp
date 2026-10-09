# SPDX-License-Identifier: GPL-3.0-or-later
"""Additive valid/masked/overlapping/invalid domains for retail167AB0→1531D0."""

from __future__ import annotations

import hashlib
from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

MATRIX_CALL_VA = 0x167AB0
MATRIX_CALL_CASES = 256
MATRIX_CALL_INDEX_BASE = 24 << 40


def make_matrix_call_case(
    seed: int,
    ordinal: int,
    va: int,
    size: int,
    *,
    policy: SeedPolicy | None = None,
) -> Case:
    if va != MATRIX_CALL_VA or not 0 <= ordinal < MATRIX_CALL_CASES:
        raise ValueError("unsupported matrix-call address or ordinal")
    c = make_case(seed, MATRIX_CALL_INDEX_BASE + ordinal, va, size, policy=policy)
    row = ordinal % 4
    index = row | ((ordinal // 4 & 0xFFFF) << 16)
    base = 0x600000 + (ordinal % 16)
    source = base + row * 512 + 0x80
    destination = 0x630000 + ordinal % 16
    mode = ordinal % 16
    if mode in (4, 5, 6, 7, 8):
        destination = source + (0, 1, -1, 4, 16)[mode - 4]
    if mode == 14:
        base = 0xFFFFFFF0  # mapped source is intentionally absent; preserve fault negative.
    if mode == 15:
        destination = 0xFFFFE0  # partial destination crosses guest end.
    payload = b"".join(
        hashlib.sha256(f"matrix-v1:{seed}:{ordinal}:{i}".encode()).digest() for i in range(2)
    )
    patches = list(c.patches)
    patches.extend(
        (
            (0x74C458, base.to_bytes(4, "little")),
            (c.esp + 4, index.to_bytes(4, "little")),
            (c.esp + 8, destination.to_bytes(4, "little")),
            (source, payload),
        )
    )
    # Source initialized before destination to make overlap and prestate observable.
    if destination < 0xFFFFE0:
        patches.append((destination, bytes([ordinal ^ 0xA5]) * 64))
    return replace(c, patches=tuple(patches))
