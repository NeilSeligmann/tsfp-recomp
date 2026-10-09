# SPDX-License-Identifier: GPL-3.0-or-later
"""Address anchors: functions placed by an explicit address list in a tracked document.

An anchor beats every rule. MEASURED is possible ONLY here (and for a function_names.csv row
whose own confidence column is MEASURED, see `rules.classify_game`). An anchor whose address is
not in the function table is reported by `seed`, never silently kept.

MEASURED anchors come from `docs/t-ui-framework.md` (T1640): "MEASURED = observed in guest
memory of the recompiled title ... dumps at every button edge". The Controls page callbacks and
the gate/load/store trio are the addresses that document marks MEASURED (row `+0x38` and `+0x50`
of its row node table in 1.2, the MEASURED order of 2.2 and the UI block of section 3, confirmed
in section 8). All of them belong to the settings UI (the Controls page of the Settings menu),
so `ui/settings`.

The weapon and inventory functions of `docs/t-player-inventory.md` are NOT MEASURED anchors:
that document labels every one of its function roles INFERRED ("all INFERRED from the
disassembly unless marked"), and `docs/data/t-player-inventory-entries.csv` and
`docs/data/t-weapon-entry-table.csv` are tables of weapon ENTRIES (entry index 0x00..0x45),
not function addresses, so they carry no function MEASURED row. They are therefore INFERRED
anchors (an explicit address list in a document is still better than a name prefix).
"""

from __future__ import annotations

from dataclasses import dataclass

from tools.subsystems.draw import DRAW_SUBSYSTEMS
from tools.subsystems.objects import OBJECT_SUBSYSTEMS
from tools.subsystems.rules import INFERRED, MEASURED
from tools.subsystems.scene import SCENE_SUBSYSTEMS


@dataclass(frozen=True)
class Anchor:
    subsystem: str
    confidence: str
    source: str


_UI = "docs/t-ui-framework.md"

#: address -> (subsystem, source). MEASURED on the running game (T1640 / T1642 dumps).
_MEASURED_UI: dict[int, tuple[str, str]] = {
    # Section 2.2 / widget table rows +0x38 (activation callback) and +0x50 (draw callback).
    0x2E9E50: ("ui/settings", f"{_UI} sec 1.2 row +0x38: Controls Accept callback"),
    0x2E9EA0: ("ui/settings", f"{_UI} sec 1.2 row +0x38: Controls Cancel callback"),
    0x2EA210: ("ui/settings", f"{_UI} sec 1.2 row +0x38: Control Layout row callback"),
    0x2EA080: ("ui/settings", f"{_UI} sec 1.2 row +0x50: Control Layout label draw callback"),
    0x2EA250: ("ui/settings", f"{_UI} sec 1.2 row +0x50: Turn Speed highlight draw callback"),
    # Section 3 (per frame gate, load/store inverses), MEASURED against RIGHT presses.
    0x2EAB30: ("ui/settings", f"{_UI} sec 3: Controls page per-frame gate (MEASURED)"),
    0x2E9BC0: ("ui/settings", f"{_UI} sec 3: store row values into the UI block"),
    0x2E9A80: ("ui/settings", f"{_UI} sec 3: load UI block into the rows"),
    # Section 2.2 "MEASURED order": builder chain Settings row -> Controls page -> rows.
    0x2CBE50: ("ui/settings", f"{_UI} sec 2.2: Settings row Controls, copies profile into block"),
    0x2EA8E0: ("ui/settings", f"{_UI} sec 2.2: Controls page builder"),
    0x2EA3F0: ("ui/settings", f"{_UI} sec 2.2: Controls row builder"),
    0x2E9FC0: ("ui/settings", f"{_UI} sec 2.2: Controls page back handler (gate arg of 0x75B70)"),
}

_INV = "docs/t-player-inventory.md"

#: address -> (subsystem, source). INFERRED from the disassembly, per the document.
#: Left out on purpose: `0x25EAD0` ("save of the inventory to the profile record" in the document).
#: The function table names that address `game_hud_message_post_by_mode_flags_with_text_id`, so
#: the document's address is off by one function or the name is wrong, and neither is verified.
_INFERRED_INVENTORY: dict[int, tuple[str, str]] = {
    0x1B6B40: ("gameplay/inventory", f"{_INV} functions: numbers the entry groups at boot"),
    0x1B9D30: ("gameplay/inventory", f"{_INV} functions: fills the group order bytes"),
    0xD7BC0: ("gameplay/inventory", f"{_INV} functions: reset of the ext block at spawn"),
    0x1BFAA0: ("gameplay/inventory", f"{_INV} functions: grant"),
    0x1BFCA0: ("gameplay/inventory", f"{_INV} functions: collect"),
    0x1BFC00: ("gameplay/inventory", f"{_INV} functions: release"),
    0x1BA130: ("gameplay/inventory", f"{_INV} functions: select"),
    0x1BA410: ("gameplay/inventory", f"{_INV} functions: selectable predicate"),
    0x1B68F0: ("gameplay/inventory", f"{_INV} functions: group contains an owned x2 entry"),
    0x1BA7B0: ("gameplay/inventory", f"{_INV} functions: auto-switch on pickup"),
    0x1BA220: ("gameplay/inventory", f"{_INV} functions: reserve ammo add"),
    0x1C1D90: ("gameplay/inventory", f"{_INV} functions: default loadout amounts"),
    0xCEF30: ("gameplay/inventory", f"{_INV} functions: initial loadout"),
    0x25EC70: ("gameplay/inventory", f"{_INV} functions: restore inventory from the profile"),
    0xC0310: ("gameplay/weapons", f"{_INV} functions: world pickup of a weapon"),
}

_BOUNDARY = "decomp-tree boundary decision (supervisor, 2026-10-09)"

#: address -> (subsystem, source). Named by the boundary decision, INFERRED.
_INFERRED_BOUNDARY: dict[int, tuple[str, str]] = {
    0x30890: ("network/syslink", f"{_BOUNDARY}: LAN/UDP messenger socket recvfrom dispatch step"),
    0x313D0: ("network/syslink", f"{_BOUNDARY}: LAN/UDP messenger socket recvfrom and dispatch"),
    0x31790: ("network/syslink", f"{_BOUNDARY}: LAN/UDP messenger client sockets open and bind"),
}

# T1735: finite reviewed families, not an unbounded prefix decision.
_INFERRED_INPUT: dict[int, tuple[str, str]] = {
    0x246D10: ("data/missions", "docs/t-guest-input-chain.md: reviewed family row"),
    0x246D50: ("data/missions", "docs/t-guest-input-chain.md: reviewed family row"),
    0x246DC0: ("data/missions", "docs/t-guest-input-chain.md: reviewed family row"),
    0x246E40: ("data/missions", "docs/t-guest-input-chain.md: reviewed family row"),
    0x246EB0: ("data/missions", "docs/t-guest-input-chain.md: reviewed family row"),
    0x246F50: ("data/missions", "docs/t-guest-input-chain.md: reviewed family row"),
    0x247190: ("data/missions", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2474E0: ("data/missions", "docs/t-guest-input-chain.md: reviewed family row"),
    0x247550: ("data/missions", "docs/t-guest-input-chain.md: reviewed family row"),
    0x247640: ("data/missions", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2586C0: ("engine/input", "docs/t-guest-input-chain.md: reviewed family row"),
    0x258870: ("engine/input", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2588B0: ("engine/input", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E7CA0: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E7F10: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E7FD0: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E80A0: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E8150: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E83A0: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E8520: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E86A0: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E8760: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E87B0: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E8840: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E8C00: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E8D60: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
    0x2E8E40: ("ui/settings", "docs/t-guest-input-chain.md: reviewed family row"),
}

# T1738: original allocator/ledger operations, not generic append/release rules.
_INFERRED_MEMORY: dict[int, tuple[str, str]] = {
    0x3E440: ("engine/memory", "docs/t-engine-memory.md: primary ledger allocation (T1420/T1030)"),
    0x3E4E0: ("engine/memory", "docs/t-engine-memory.md: primary ledger release (T1003)"),
    0x3E560: ("engine/memory", "docs/t-engine-memory.md: secondary ledger append (T1037)"),
    0x3E5A0: ("engine/memory", "docs/t-engine-memory.md: secondary ledger release (T1037)"),
    0x3E970: ("engine/memory", "docs/t-engine-memory.md: bump allocator source contract"),
}

# T1702 finite rule-state and result presentation boundaries; never sweep the AI band.
_INFERRED_MULTIPLAYER: dict[int, tuple[str, str]] = {
    0xC09B0: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0xC0C50: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0xC11A0: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0xCB240: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0xD8A00: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0xD8B10: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x2E4470: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x274330: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x274350: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x274380: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x2743A0: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x2743D0: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x2744F0: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x274590: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x274620: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x274650: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x275640: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x2F2050: ("gameplay/modes/multiplayer", "docs/t-multiplayer-rules.md: function catalogue"),
    0x2F1C80: ("ui/menu", "docs/t-multiplayer-rules.md: function catalogue"),
    0x2F2200: ("ui/menu", "docs/t-multiplayer-rules.md: function catalogue"),
    0x2F6160: ("ui/menu", "docs/t-multiplayer-rules.md: function catalogue"),
    0x2F7610: ("ui/menu", "docs/t-multiplayer-rules.md: function catalogue"),
}

# T1703: finite mission controllers; shared load/script/checkpoint engines stay shared.
_INFERRED_STORY: dict[int, tuple[str, str]] = {
    0x1E9CC0: ("gameplay/modes/story", "docs/t-story-mode-flow.md: lv4 mission initialization"),
    0x1E8BD0: ("gameplay/modes/story", "docs/t-story-mode-flow.md: lv4 tagged checkpoint state"),
    0x1E4A10: (
        "gameplay/modes/story",
        "docs/t-story-mode-flow.md: lv4 checkpoint object and slot state",
    ),
    0x1E4AD0: ("gameplay/modes/story", "docs/t-story-mode-flow.md: lv4 checkpoint slot presence"),
    0x2532D0: ("data/missions", "docs/t-story-mode-flow.md: shared sequence checkpoint state"),
    0x70410: ("ui/menu", "docs/t-story-mode-flow.md: boot state0x67 language/movie policy"),
}

ANCHORS: dict[int, Anchor] = {
    **{
        va: Anchor(
            path,
            INFERRED,
            f"docs/t1734-object-layout.md; docs/data/t1734-record-subsystems.csv row 0x{va:08x}",
        )
        for va, path in OBJECT_SUBSYSTEMS.items()
    },
    **{
        va: Anchor(path, INFERRED, f"docs/t1722-draw-subsystems.md row 0x{va:08x}")
        for va, path in DRAW_SUBSYSTEMS.items()
    },
    **{
        va: Anchor(
            path,
            INFERRED,
            f"docs/t1723-scene-subsystems.md; docs/data/t1723-scene-subsystems.csv row 0x{va:08x}",
        )
        for va, path in SCENE_SUBSYSTEMS.items()
    },
    **{va: Anchor(path, INFERRED, source) for va, (path, source) in _INFERRED_STORY.items()},
    **{va: Anchor(path, MEASURED, source) for va, (path, source) in _MEASURED_UI.items()},
    **{va: Anchor(path, INFERRED, source) for va, (path, source) in _INFERRED_INVENTORY.items()},
    **{va: Anchor(path, INFERRED, source) for va, (path, source) in _INFERRED_BOUNDARY.items()},
    **{va: Anchor(path, INFERRED, source) for va, (path, source) in _INFERRED_INPUT.items()},
    **{va: Anchor(path, INFERRED, source) for va, (path, source) in _INFERRED_MEMORY.items()},
    **{va: Anchor(path, INFERRED, source) for va, (path, source) in _INFERRED_MULTIPLAYER.items()},
}
