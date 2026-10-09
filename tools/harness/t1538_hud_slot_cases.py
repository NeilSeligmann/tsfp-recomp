# SPDX-License-Identifier: GPL-3.0-or-later
"""T1538 frozen HUD player-slot domain; namespace79 disjoint root delegation.

No original execution or validated unlock follows from fixture construction.
"""

from itertools import product

from .model import Case
from .seeding import SeedPolicy
from .t1478_pointer_fixture import PointerFixture

NAMESPACE = 79
ROOT = 0x00139160
SUPPORTED_VAS = frozenset({ROOT})
PLAYER_POINTER = 0x007B0C7C
STATE_BASE = 0x007A6160
STRIDE = 0x177C
PLAYERS = (0x00DC0000, 0x00DC0100)
SLOTS = (-2, -1, 0, 1, 3, 0x40000000, 0x7FFFFFFF)
FLAGS = (0, 1, 0x40, 0x41, 0x80, 0xFF)
STATES = (0, 0xC, 0x11, 1, 0xFFFFFFFF)
ORDINARY = tuple(product(PLAYERS, SLOTS, FLAGS, STATES))
CONTROLS = (
    "null-player",
    "unmapped-player",
    "unmapped-state",
    "flag-skips-unmapped-state",
    "negative-skips-unmapped-flag",
    "nonnegative-unmapped-flag",
)


def hud_slot_case_count(va: int) -> int:
    return len(ORDINARY) + len(CONTROLS) if va == ROOT else 0


def make_hud_slot_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != ROOT or type(ordinal) is not int or not 0 <= ordinal < hud_slot_case_count(va):
        raise ValueError("unsupported T1538 HUD-slot root/ordinal")
    fixture = PointerFixture(NAMESPACE, seed, ordinal, va, size, policy)
    if ordinal < len(ORDINARY):
        player, slot, flag, state = ORDINARY[ordinal]
        fixture.word(player + 4, slot)
        fixture.byte(player + 0xC, flag)
        # Negative-slot cases still carry mapped state decoys; original must skip reads.
        address = (STATE_BASE + slot * STRIDE) & 0xFFFFFFFF
        fixture.word(address, state)
        fixture.word(address - 4, state ^ 0xA5A5A5A5)
        fixture.word(address + 4, state ^ 0x5A5A5A5A)
    else:
        control = CONTROLS[ordinal - len(ORDINARY)]
        player = PLAYERS[0]
        if control in ("null-player", "unmapped-player"):
            player = 0 if control == "null-player" else 0x90000000
        elif control in ("unmapped-state", "flag-skips-unmapped-state"):
            fixture.word(player + 4, 0x10000)
            fixture.byte(player + 0xC, 0x40 if control.startswith("flag-") else 0)
            # No patch to the unmapped arithmetic state address.
        else:
            player = 0x00FFFFF8
            fixture.word(player + 4, -1 if control.startswith("negative-") else 0)
            # player+4 is mapped; player+C is not. No patch to the latter.
    fixture.word(PLAYER_POINTER, player)
    return fixture.finish()
