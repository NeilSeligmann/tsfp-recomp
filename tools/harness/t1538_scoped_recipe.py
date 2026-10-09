"""Prospective HUD custom-binding recipe, inactive until core T1550 is ratified.

Independent equations for the unchanged426case namespace79 domain. No contract
registration, core callback, permission claim or native/original execution here.
"""

from dataclasses import replace

from .model import Case
from .seeding import make_case

ROOT = 0x139160
SIZE = 53
NAMESPACE = 79
LABEL = "t1479-signed-table"
COUNT = 426
SEEDS = (20261001, 20261006)
# (role, kind, start, end-exclusive, exact patch width): disjoint physical owners.
REGIONS = (
    ("hud-state-minus2", "source", 0x7A3264, 0x7A3270, 4),
    ("hud-state-minus1", "source", 0x7A49E0, 0x7A49EC, 4),
    ("hud-state-zero", "source", 0x7A615C, 0x7A6168, 4),
    ("hud-state-one", "source", 0x7A78D8, 0x7A78E4, 4),
    ("hud-state-three", "source", 0x7AA7D0, 0x7AA7DC, 4),
    ("hud-player-pointer", "source", 0x7B0C7C, 0x7B0C80, 4),
    ("hud-player0-slot", "guest", 0xDC0004, 0xDC0008, 4),
    ("hud-player0-flag", "guest", 0xDC000C, 0xDC000D, 1),
    ("hud-player1-slot", "guest", 0xDC0104, 0xDC0108, 4),
    ("hud-player1-flag", "guest", 0xDC010C, 0xDC010D, 1),
    ("hud-boundary-slot", "guest", 0xFFFFFC, 0x1000000, 4),
)


def expected_case(seed: int, ordinal: int) -> Case:
    """Independent finite stream, never calls HUD or namespace79 factories."""
    if seed not in SEEDS or type(ordinal) is not int or not 0 <= ordinal < COUNT:
        raise ValueError("unsupported frozen HUD seed/ordinal")
    base = make_case(seed, (79 << 40) + ordinal, 0x139160, 53)
    patches = list(base.patches)

    def add(at: int, value: int, width: int = 4) -> None:
        patches.append((at, (value & ((1 << (8 * width)) - 1)).to_bytes(width, "little")))

    if ordinal < 420:
        player = (0xDC0000, 0xDC0100)[ordinal // 210]
        slot = (-2, -1, 0, 1, 3, 0x40000000, 0x7FFFFFFF)[ordinal // 30 % 7]
        flag = (0, 1, 0x40, 0x41, 0x80, 0xFF)[ordinal // 5 % 6]
        state = (0, 12, 17, 1, 0xFFFFFFFF)[ordinal % 5]
        add(player + 4, slot)
        add(player + 12, flag, 1)
        at = (0x7A6160 + slot * 6012) & 0xFFFFFFFF
        add(at, state)
        add(at - 4, state ^ 0xA5A5A5A5)
        add(at + 4, state ^ 0x5A5A5A5A)
    elif ordinal < 422:
        player = 0 if ordinal == 420 else 0x90000000
    elif ordinal < 424:
        player = 0xDC0000
        add(player + 4, 0x10000)
        add(player + 12, 0 if ordinal == 422 else 0x40, 1)
    else:
        player = 0xFFFFF8
        add(player + 4, -1 if ordinal == 424 else 0)
    add(0x7B0C7C, player)
    return replace(base, patches=tuple(patches))


def expected_roles(case: Case) -> tuple[str, ...]:
    """Recipe roles for expected-stream freezing; core must independently derive its own."""
    roles = ["baseline-frame", "baseline-scratch"]
    for at, raw in case.patches[2:]:
        matches = [
            role
            for role, _, lo, hi, width in REGIONS
            if lo <= at < at + len(raw) <= hi and len(raw) == width and (at - lo) % width == 0
        ]
        if len(matches) != 1:
            raise ValueError("patch lacks unique declared HUD region")
        roles.append(matches[0])
    return tuple(roles)
