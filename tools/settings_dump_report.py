#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1709: report which setting bytes change on each button press while the owner walks Settings > Controls and
Settings > Audio/Video.

Input: a directory written by the host flag `--dump-on-button` (`buttons.jsonl` plus `buttons/guestdump.<label>`),
taken with the range set `SETTINGS_RANGES` (print the `--dump-guest-range` string with
`python -m tools.settings_dump_report ranges`). The parsing primitives come from tools.button_dump_report.

Per non-idle event the BEFORE dump is compared with the first usable AFTER dump (`--after K` picks the offset in
frames, `--after all` uses every after dump). From each dump the report decodes the page stack (page widget table),
the rows of the Controls or Audio/Video page (row node table) and the setting bytes: the UI copy B of the settings,
the profile object P and the audio globals. The event block lists the displayed row value before and after and EVERY
changed byte with its T1708 field name (docs/t1708-settings-block.md), then flags:

  NO-BYTE-CHANGE    a row index changed (or a LEFT/RIGHT press hit an option row that changed something else) but the
                    setting's own bytes in the UI block did not change (or, on an A/V row, no dumped byte changed)
  UNLABELLED-BYTE   a byte changed that no T1708 field maps to the row that changed (noise excluded)
  VALUE-MISMATCH    the value decoded from the bytes differs from the row's displayed option index, or the profile
                    differs from the UI block after Accept
  NO-CHANGE-ON-PRESS informational: a LEFT/RIGHT press on an option row changed nothing at all (lost press, clamp)
  PROFILE-ON-ACCEPT informational: the profile block changed on the Accept press and equals the UI block after it
  AV-CANDIDATE      a byte changed with an Audio/Video row (INFERRED storage discovery, the offsets are unlabelled)

NOISE: a byte that changed in an idle control event, or in at least 30 percent of all events (given at least 5
events), is noise and hidden unless `--show-noise`. A byte that belongs to the setting of the row under the cursor is
never hidden for that event (it is the signal).

Evidence class: xemu-level MEASURED observation of the unmodified game. The row label strings are NOT in the dump (the
string pointers are not dumped), so the displayed option label is DERIVED from the MEASURED label tables of T1708 and
docs/t-ui-framework.md: the owner must confirm what the screen showed. The Audio/Video storage report is INFERRED.

Exit codes: 0 ok, 1 --validate found manifest problems, 2 usage error or missing manifest, 3 every dump unreadable.

Usage: python -m tools.settings_dump_report DIR [--write] [--after K|first|all] [--show-noise] [--json] [--validate]
       python -m tools.settings_dump_report ranges   (the --dump-guest-range value, one line)
       python -m tools.settings_dump_report fields   (the field map tables)
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, NamedTuple

from tools.button_dump_report import (
    EXIT_INVALID,
    EXIT_OK,
    EXIT_UNREADABLE,
    EXIT_USAGE,
    MANIFEST_NAME,
    Event,
    RangeInfo,
    RangeSelector,
    _dump_path,
    as_float,
    check_dump_files,
    load_manifest,
    parse_dump,
    parse_range_spec,
    validate_manifest,
)

REPORT_NAME = "settings_report.md"
JSON_NAME = "settings_report.json"
OWNER_NOTES_NAME = "OWNER_NOTES.txt"

# --------------------------------------------------------------------------------------------------------------------
# Dump range set: the single source of truth for the host flag and for this report
# --------------------------------------------------------------------------------------------------------------------


class RangeDef(NamedTuple):
    key: str
    spec: str  # host grammar ADDR:LEN or *ADDR+OFF:LEN
    purpose: str


SETTINGS_RANGES: tuple[RangeDef, ...] = (
    RangeDef("mode", "0x79094C:4", "game mode word (0x65 front end, 0x64 preview, 0x66 editor)"),
    RangeDef(
        "ui",
        "0x7BA364:0x1E0",
        "UI copy B (viewport 0): B+0..0x7C settings block, B+0x7C working mapping W, B+0xD4 working cursor",
    ),
    RangeDef(
        "profile",
        "0x7BEDC4:0x1F50",
        "profile object P, player slot 0: P+0..0x7C settings block, the rest holds the other saved state",
    ),
    RangeDef(
        "audio",
        "0x5236A0:0x40",
        "audio menu globals (0x5236B4..0x5236BC audio settings, 0x5236C4 player count)",
    ),
    RangeDef("widgets", "0x6FEBE8:0x2CE0", "page widget table: 20 slots, stride 0x23C"),
    RangeDef(
        "rows",
        "*0x7BB1DC+0:0xD480",
        "row node table (pointer at 0x7BB1DC is the base), 0x10C bytes per node",
    ),
    RangeDef("player", "0x78AD34:4", "selected player"),
    RangeDef("viewport", "0x7BA9E0:4", "viewport"),
    RangeDef("node_count", "0x6FE920:4", "row node count"),
    RangeDef("pointer_mode", "0x7BA270:4", "pointer mode"),
)
MAX_RANGES = 16
UI_LENGTH = 0x1E0
PROFILE_LENGTH = 0x1F50
AUDIO_LENGTH = 0x40
AUDIO_BASE = 0x5236A0
PROFILE_BASE = 0x7BEDC4
WIDGET_STRIDE = 0x23C
WIDGET_SLOTS = 20
NODE_SIZE = 0x10C
HEAD_LEN = 0x7C
WORKING_MAPPING_OFFSET = 0x7C
WORKING_CURSOR_OFFSET = 0xD4
MAX_WALK = 64
NO_CURSOR = 0xFFFFFFFF
STATE_OPEN = 3
STATE_OPENING = 1
LIVE_STATES = (STATE_OPEN, STATE_OPENING)
NOISE_PERCENT = 30
NOISE_MIN_EVENTS = 5
ROW_TYPE_BUTTON = 7
ROW_KIND_CYCLER = 1
MAX_RUN_BYTES = 16
AUDIO_GLOBALS: dict[int, str] = {
    0x5236B4: "audio setting 0x5236B4",
    0x5236B8: "audio setting 0x5236B8",
    0x5236BC: "audio setting 0x5236BC",
    0x5236C4: "player count 0x5236C4",
}

SCREENS: dict[int, str] = {
    0x2D0BE0: "Main menu",
    0x2CBFC0: "Settings",
    0x2EA8E0: "Controls",
    0x2E9EC0: "Controls accept-changes",
    0x2E9A20: "Control Layout chooser",
    0x2C3E20: "Audio/Video",
    0x2F5520: "Pause menu",
}
CONTROLS_BUILDER = 0x2EA8E0
AV_BUILDER = 0x2C3E20
SETTINGS_BUILDER = 0x2CBFC0
ANALYSIS_BUILDERS = (CONTROLS_BUILDER, AV_BUILDER, SETTINGS_BUILDER)
SETTINGS_LIST_ROWS = ("Audio/Video Options", "Controls", "Copy Profile")
AV_ACCEPT_ID = 6
AV_CANCEL_ID = 7
KIND_CONTROLS = "controls"
KIND_AV = "av"
KIND_SETTINGS = "settings"
BUILDER_KIND = {
    CONTROLS_BUILDER: KIND_CONTROLS,
    AV_BUILDER: KIND_AV,
    SETTINGS_BUILDER: KIND_SETTINGS,
}


def ranges_argument() -> str:
    """The comma joined --dump-guest-range value for the fish script."""
    return ",".join(item.spec for item in SETTINGS_RANGES)


# --------------------------------------------------------------------------------------------------------------------
# T1708 field map of the 124 byte settings block (P and B share it)
# --------------------------------------------------------------------------------------------------------------------

MAPPING_NAMES: tuple[str, ...] = (
    "Fire",
    "Throw grenade",
    "Activate",
    "Manual reload",
    "Aim",
    "Crouch",
    "Zoom in",
    "Zoom out",
    "Prev. weapon",
    "Next weapon",
    "Prev. weapon 2nd",
    "Next weapon 2nd",
    "Change grenades",
    "Melee",
    "Run",
    "Side step",
    "Look",
    "Turn",
    "Run derived",
    "Side step derived",
    "Look derived",
    "Turn derived",
)
FLAGS_OFFSET = 2
# bit of the flag byte -> name
FLAG_BITS: dict[int, str] = {
    0: "Inverse Look (1=On)",
    1: "Aim Mode (1=Toggle)",
    2: "Crouch (1=Toggle)",
    3: "Auto Lookahead (1=On)",
    4: "Vibration enable",
    5: "Vibration kind bit5",
    6: "Vibration kind bit6",
    7: "Auto Aim (1=On)",
}
# setting key -> bits of the flag byte it owns
SETTING_MASKS: dict[str, int] = {
    "inverse_look": 0x01,
    "aim_mode": 0x02,
    "crouch": 0x04,
    "auto_lookahead": 0x08,
    "vibration": 0x70,
    "auto_aim": 0x80,
}
# vibration kind bits (flag byte & 0x70) -> option index, with bit 4 set
VIBRATION_INDEX: dict[int, int] = {0x50: 1, 0x30: 2, 0x70: 3}
VIBRATION_ENABLE = 0x10
WEAPON_CHANGE_OFFSET = 0x68
CROSSHAIR_OFFSET = 0x74
TURN_SPEED_OFFSET = 0x78


@dataclass(frozen=True)
class Field:
    offset: int
    size: int
    name: str
    kind: str  # u16 flags u8 u32 i32 f32 map
    setting: str | None
    confidence: str


def _build_field_map() -> tuple[Field, ...]:
    fields = [
        Field(
            0x00, 2, "Control Layout selector", "u16", "layout", "MEASURED contract, INFERRED role"
        ),
        Field(0x02, 1, "Controls flag byte", "flags", None, "MEASURED"),
        Field(
            0x03,
            1,
            "Custom layout confirmation (bit0)",
            "u8",
            "layout",
            "MEASURED bit operations, INFERRED state",
        ),
    ]
    for index, name in enumerate(MAPPING_NAMES):
        fields.append(
            Field(
                4 + 4 * index,
                4,
                f"Mapping[{index}] {name}",
                "map",
                "layout",
                "MEASURED offset, INFERRED role",
            )
        )
    fields += [
        Field(0x5C, 4, "Unresolved 0x5C", "u32", None, "MEASURED write, unresolved"),
        Field(0x60, 4, "Unresolved 0x60 (round robin)", "u32", None, "MEASURED write, unresolved"),
        Field(0x64, 4, "Unresolved 0x64", "i32", None, "MEASURED write, unresolved"),
        Field(0x68, 4, "Weapon Change", "u32", "weapon_change", "MEASURED"),
        Field(
            0x6C,
            4,
            "Float table index (UI meaning unresolved)",
            "u32",
            None,
            "MEASURED initialization, INFERRED reader",
        ),
        Field(0x70, 4, "Unresolved 0x70", "u32", None, "MEASURED write, unresolved"),
        Field(0x74, 4, "Crosshair", "u32", "crosshair", "MEASURED"),
        Field(0x78, 4, "Turn Speed", "f32", "turn_speed", "MEASURED"),
    ]
    return tuple(fields)


FIELD_MAP: tuple[Field, ...] = _build_field_map()
_FIELD_AT: list[Field | None] = [None] * HEAD_LEN
for _item in FIELD_MAP:
    for _byte in range(_item.offset, _item.offset + _item.size):
        _FIELD_AT[_byte] = _item
WORKING_FIELDS: tuple[Field, ...] = tuple(
    Field(
        WORKING_MAPPING_OFFSET + 4 * index,
        4,
        f"W[{index}] {name} (working mapping)",
        "map",
        None,
        "INFERRED",
    )
    for index, name in enumerate(MAPPING_NAMES)
)
CURSOR_FIELD = Field(
    WORKING_CURSOR_OFFSET, 4, "Working cursor (Control Layout editor)", "u32", None, "INFERRED"
)


def field_at(block: str, offset: int) -> Field | None:
    """The T1708 field covering a byte of the UI block B (ui) or the profile object P (profile)."""
    if offset < HEAD_LEN:
        return _FIELD_AT[offset]
    if block == "ui":
        if WORKING_MAPPING_OFFSET <= offset < WORKING_CURSOR_OFFSET:
            return WORKING_FIELDS[(offset - WORKING_MAPPING_OFFSET) // 4]
        if WORKING_CURSOR_OFFSET <= offset < WORKING_CURSOR_OFFSET + 4:
            return CURSOR_FIELD
    return None


# --------------------------------------------------------------------------------------------------------------------
# Controls page rows (docs/t1708-settings-block.md and docs/t-ui-framework.md, MEASURED order and labels)
# --------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class ControlsRow:
    cursor: int
    row_id: int
    key: str
    label: str
    options: tuple[str, ...]


CONTROLS_ROWS: tuple[ControlsRow, ...] = (
    ControlsRow(0, 0, "layout", "Control Layout", ("preset name",)),
    ControlsRow(1, 1, "crosshair", "Crosshair", ("Off", "On and Moving", "On and Fixed")),
    ControlsRow(2, 3, "inverse_look", "Inverse Look", ("Off", "On")),
    ControlsRow(3, 2, "auto_aim", "Auto Aim", ("Off", "On")),
    ControlsRow(4, 4, "auto_lookahead", "Auto Lookahead", ("Off", "On")),
    ControlsRow(5, 5, "aim_mode", "Aim Mode", ("Toggle", "Hold")),
    ControlsRow(6, 7, "crouch", "Crouch", ("Toggle", "Hold")),
    ControlsRow(
        7,
        6,
        "weapon_change",
        "Weapon Change",
        ("Always", "Never", "Best", "If New", "If New and Best"),
    ),
    ControlsRow(8, 8, "vibration", "Vibration mode", ("Off", "Fire", "Hit", "Fire&Hit")),
    ControlsRow(9, 9, "turn_speed", "Turn Speed", ()),
    ControlsRow(10, 10, "accept", "Accept", ()),
    ControlsRow(11, 11, "cancel", "Cancel", ()),
)
CONTROLS_BY_ID: dict[int, ControlsRow] = {row.row_id: row for row in CONTROLS_ROWS}
CONTROLS_BY_KEY: dict[str, ControlsRow] = {row.key: row for row in CONTROLS_ROWS}
SETTING_KEYS: tuple[str, ...] = (
    "crosshair",
    "inverse_look",
    "auto_aim",
    "auto_lookahead",
    "aim_mode",
    "crouch",
    "weapon_change",
    "vibration",
    "turn_speed",
)
ACCEPT_ID = 10
CANCEL_ID = 11
LAYOUT_ID = 0
LR_BUTTONS = ("LEFT", "RIGHT")
MOVE_BUTTONS = ("UP", "DOWN")
TURN_SPEED_MIN_VALUES = 3


def _u32(block: bytes, offset: int) -> int:
    return int.from_bytes(block[offset : offset + 4], "little")


def decode_index(key: str, block: bytes | None) -> int | None:
    """Option index of a Controls setting decoded from the settings block bytes (None: bytes hold no valid value)."""
    if block is None or len(block) < HEAD_LEN:
        return None
    flags = block[FLAGS_OFFSET]
    if key == "inverse_look":
        return flags & 0x01
    if key == "auto_aim":
        return (flags >> 7) & 0x01
    if key == "auto_lookahead":
        return (flags >> 3) & 0x01
    if key == "aim_mode":
        return 0 if flags & 0x02 else 1
    if key == "crouch":
        return 0 if flags & 0x04 else 1
    if key == "vibration":
        if not flags & VIBRATION_ENABLE:
            return 0
        return VIBRATION_INDEX.get(flags & 0x70)
    if key == "weapon_change":
        value = _u32(block, WEAPON_CHANGE_OFFSET)
        return value if value < 5 else None
    if key == "crosshair":
        value = _u32(block, CROSSHAIR_OFFSET)
        return value if value < 3 else None
    return None


def turn_speed(block: bytes | None) -> float | None:
    if block is None or len(block) < HEAD_LEN:
        return None
    return as_float(_u32(block, TURN_SPEED_OFFSET))


def option_label(row: ControlsRow, index: int | None) -> str:
    if index is None:
        return "invalid"
    if 0 <= index < len(row.options):
        return row.options[index]
    return f"index {index}"


def value_text(key: str, index: int | None, block: bytes | None) -> str:
    """`label [index]` of a setting, Turn Speed as the float of the block."""
    if key == "turn_speed":
        value = turn_speed(block)
        return "unreadable" if value is None else f"{value:.4f}"
    row = CONTROLS_BY_KEY[key]
    return f"{option_label(row, index)} [{'?' if index is None else index}]"


def setting_bits_changed(key: str, old: bytes | None, new: bytes | None) -> bool:
    """True when a byte (or bit of the flag byte) that belongs to the setting differs between two blocks."""
    if old is None or new is None or len(old) < HEAD_LEN or len(new) < HEAD_LEN:
        return False
    if key in SETTING_MASKS:
        return bool((old[FLAGS_OFFSET] ^ new[FLAGS_OFFSET]) & SETTING_MASKS[key])
    spans = {
        "weapon_change": (WEAPON_CHANGE_OFFSET, 4),
        "crosshair": (CROSSHAIR_OFFSET, 4),
        "turn_speed": (TURN_SPEED_OFFSET, 4),
    }
    if key in spans:
        start, size = spans[key]
        return old[start : start + size] != new[start : start + size]
    return False


# --------------------------------------------------------------------------------------------------------------------
# Dump loading (one file at a time, only the dumped ranges keep their bytes)
# --------------------------------------------------------------------------------------------------------------------


def _selectors() -> list[tuple[str, RangeSelector, int]]:
    result = []
    for item in SETTINGS_RANGES:
        body, _, length = item.spec.partition(":")
        result.append((item.key, RangeSelector(spec=parse_range_spec(body)), int(length, 0)))
    return result


_SELECTORS = _selectors()


@dataclass
class Snapshot:
    path: Path | None
    status: str  # ok, missing, unreadable
    ranges: dict[str, bytes] = field(default_factory=dict)
    row_base: int | None = None
    reasons: list[str] = field(default_factory=list)

    @property
    def usable(self) -> bool:
        return self.status == "ok"


def _range_key(info: RangeInfo) -> str | None:
    for key, selector, _ in _SELECTORS:
        if selector.matches(info):
            return key
    return None


def load_snapshot(path: Path | None) -> Snapshot:
    """Parse one dump file and keep the bytes of the SETTINGS_RANGES it holds completely."""
    if path is None:
        return Snapshot(path, "missing", reasons=["no usable file name in the manifest"])
    try:
        parsed = parse_dump(path, lambda info: _range_key(info) is not None)
    except OSError as error:
        return Snapshot(path, "missing", reasons=[f"{path.name}: {error.strerror or error}"])
    snapshot = Snapshot(path, "unreadable")
    lengths = {key: length for key, _, length in _SELECTORS}
    for info in parsed.ranges:
        key = _range_key(info)
        if key is None or key in snapshot.ranges:
            continue
        data = bytes(info.data) if info.data is not None else b""
        if len(data) < lengths[key]:
            why = f" ({info.reason})" if info.reason else ""
            snapshot.reasons.append(
                f"{path.name}: range {key} partial or unreadable, {len(data)} of {lengths[key]} bytes{why}"
            )
            continue
        snapshot.ranges[key] = data[: lengths[key]]
        if key == "rows":
            snapshot.row_base = info.pointer if info.pointer is not None else info.address
    present = {key for key, _, _ in _SELECTORS} & set(snapshot.ranges)
    for key in sorted({key for key, _, _ in _SELECTORS} - present - {"rows"}):
        if not any(key in reason for reason in snapshot.reasons):
            snapshot.reasons.append(f"{path.name}: range {key} not in the dump")
    snapshot.status = "ok" if snapshot.ranges else "unreadable"
    return snapshot


# --------------------------------------------------------------------------------------------------------------------
# Decoding of the page stack and the rows
# --------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class RowNode:
    index: int
    node_type: int
    row_id: int
    flags: int
    kind: int
    options: int
    selected: int
    selected_id: int
    previous: int
    at_open: int
    next_ptr: int


@dataclass
class PageView:
    slot: int
    builder: int
    screen: str
    state: int
    child: int
    first_row: int
    row_count: int
    cursor: int
    cursor_id: int
    cursor_row: int
    rows: list[RowNode] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)

    @property
    def kind(self) -> str | None:
        return BUILDER_KIND.get(self.builder)


def node_index(pointer: int, base: int | None, count: int) -> int | None:
    """Row node index of an absolute guest pointer into the table at base (None: not a node of the table)."""
    if base is None or pointer == 0:
        return None
    delta = pointer - base
    if delta < 0 or delta % NODE_SIZE:
        return None
    index = delta // NODE_SIZE
    return index if index < count else None


def decode_node(rows: bytes, index: int) -> RowNode:
    start = index * NODE_SIZE
    return RowNode(
        index=index,
        node_type=_u32(rows, start + 0x00),
        row_id=_u32(rows, start + 0x14),
        flags=_u32(rows, start + 0x28),
        kind=_u32(rows, start + 0xB4),
        options=_u32(rows, start + 0xC4),
        selected=_u32(rows, start + 0xC8),
        selected_id=_u32(rows, start + 0xCC),
        previous=_u32(rows, start + 0xD0),
        at_open=_u32(rows, start + 0xD4),
        next_ptr=_u32(rows, start + 0x04),
    )


def walk_rows(page: PageView, rows: bytes, base: int | None, count: int) -> None:
    """Fill page.rows: follow the creation chain from the first row pointer, keep the button and cycler rows."""
    seen: set[int] = set()
    pointer = page.first_row
    nodes: list[RowNode] = []
    while pointer and len(nodes) < MAX_WALK:
        index = node_index(pointer, base, count)
        if index is None:
            page.problems.append(f"row pointer 0x{pointer:X} is not a node of the table")
            break
        if index in seen:
            page.problems.append(f"row chain loops at node {index}")
            break
        seen.add(index)
        node = decode_node(rows, index)
        nodes.append(node)
        pointer = node.next_ptr
    else:
        if pointer and len(nodes) >= MAX_WALK:
            page.problems.append(f"row chain longer than {MAX_WALK} nodes, cut")
    picked = [
        node for node in nodes if node.node_type == ROW_TYPE_BUTTON or node.kind == ROW_KIND_CYCLER
    ]
    if 0 < page.row_count < len(picked):
        picked = picked[: page.row_count]
    page.rows = picked


def decode_page(widgets: bytes, slot: int) -> PageView | None:
    start = slot * WIDGET_STRIDE
    builder = _u32(widgets, start + 0x08)
    if builder == 0:
        return None
    return PageView(
        slot=slot,
        builder=builder,
        screen=SCREENS.get(builder, f"builder 0x{builder:X}"),
        state=_u32(widgets, start + 0x1B0),
        child=_u32(widgets, start + 0x170),
        first_row=_u32(widgets, start + 0x178),
        row_count=_u32(widgets, start + 0x180),
        cursor=_u32(widgets, start + 0x184),
        cursor_id=_u32(widgets, start + 0x18C),
        cursor_row=_u32(widgets, start + 0x198),
    )


def _choose(pool: list[PageView]) -> PageView | None:
    """Prefer open pages (state 3) over opening ones (state 1), then a page without a child page, then the highest slot."""
    pool = [page for page in pool if page.state == STATE_OPEN] or pool
    pool = [page for page in pool if page.child == 0] or pool
    return max(pool, key=lambda page: page.slot) if pool else None


def pick_top(pages: list[PageView]) -> PageView | None:
    """The top page of the stack: a live (open or opening) known screen. Closed pages stay in the table as stale entries."""
    return _choose(
        [page for page in pages if page.builder in SCREENS and page.state in LIVE_STATES]
    )


def pick_settings_page(pages: list[PageView]) -> PageView | None:
    """The live Controls, Audio/Video or Settings list page that has rows."""
    return _choose(
        [
            page
            for page in pages
            if page.builder in ANALYSIS_BUILDERS
            and page.state in LIVE_STATES
            and page.row_count > 0
        ]
    )


@dataclass
class Decoded:
    ok: bool = False
    mode: int | None = None
    top: PageView | None = None
    page: PageView | None = None
    stack: list[str] = field(default_factory=list)
    ui: bytes | None = None
    profile: bytes | None = None
    audio: bytes | None = None
    player: int | None = None
    viewport: int | None = None
    node_count: int | None = None
    pointer_mode: int | None = None
    problems: list[str] = field(default_factory=list)


def _word(raw: dict[str, bytes], key: str) -> int | None:
    data = raw.get(key)
    return None if data is None else _u32(data, 0)


def decode_snapshot(raw: dict[str, bytes], row_base: int | None) -> Decoded:
    """Decode one dump (the `ranges` of a Snapshot) into mode, page stack, Controls or A/V page rows and setting blocks."""
    decoded = Decoded(ok=bool(raw))
    decoded.mode = _word(raw, "mode")
    decoded.player = _word(raw, "player")
    decoded.viewport = _word(raw, "viewport")
    decoded.node_count = _word(raw, "node_count")
    decoded.pointer_mode = _word(raw, "pointer_mode")
    decoded.ui = raw.get("ui")
    decoded.profile = raw.get("profile")
    decoded.audio = raw.get("audio")
    widgets = raw.get("widgets")
    if widgets is None:
        return decoded
    pages = [
        page for slot in range(WIDGET_SLOTS) if (page := decode_page(widgets, slot)) is not None
    ]
    decoded.stack = [
        page.screen for page in pages if page.builder in SCREENS and page.state in LIVE_STATES
    ]
    decoded.top = pick_top(pages)
    decoded.page = pick_settings_page(pages)
    rows = raw.get("rows")
    if decoded.page is not None and decoded.page.kind != KIND_SETTINGS:
        if rows is None:
            decoded.problems.append("row node table not in the dump, rows not decoded")
        else:
            count = len(rows) // NODE_SIZE
            if decoded.node_count:
                count = min(count, decoded.node_count)
            walk_rows(decoded.page, rows, row_base, count)
            decoded.problems += decoded.page.problems
    return decoded


# --------------------------------------------------------------------------------------------------------------------
# Byte diffs and their field level description
# --------------------------------------------------------------------------------------------------------------------

BlockDiff = list[tuple[int, int, int]]  # (offset, old, new)


def diff_block(before: bytes | None, after: bytes | None) -> BlockDiff:
    if before is None or after is None or before == after:
        return []
    return [
        (offset, old, new)
        for offset, (old, new) in enumerate(zip(before, after, strict=False))
        if old != new
    ]


def _flag_bit_text(old: int, new: int) -> str:
    parts = []
    changed = old ^ new
    for bit in range(8):
        if changed >> bit & 1:
            parts.append(f"bit{bit} {FLAG_BITS[bit]} {old >> bit & 1}>{new >> bit & 1}")
    return ", ".join(parts)


def _vibration_text(flags: int) -> str:
    index = decode_index("vibration", bytes([0, 0, flags]) + bytes(HEAD_LEN - 3))
    return option_label(CONTROLS_BY_KEY["vibration"], index)


def _field_values(item: Field, before: bytes, after: bytes) -> tuple[str, str, str]:
    old = int.from_bytes(before[item.offset : item.offset + item.size], "little")
    new = int.from_bytes(after[item.offset : item.offset + item.size], "little")
    if item.kind == "f32":
        return f"{as_float(old):.4f}", f"{as_float(new):.4f}", f"0x{old:08X} -> 0x{new:08X}"
    if item.kind == "flags":
        detail = _flag_bit_text(old, new)
        if (old ^ new) & 0x70:
            detail += f", vibration {_vibration_text(old)} -> {_vibration_text(new)}"
        return f"0x{old:02X}", f"0x{new:02X}", detail
    if item.kind == "i32" and old >= 0x80000000:
        old -= 1 << 32
    if item.kind == "i32" and new >= 0x80000000:
        new -= 1 << 32
    width = item.size * 2
    if item.kind in ("map", "u16", "u8"):
        return f"0x{old:0{width}X}", f"0x{new:0{width}X}", ""
    return str(old), str(new), f"0x{old & 0xFFFFFFFF:08X} -> 0x{new & 0xFFFFFFFF:08X}"


@dataclass
class Change:
    block: str  # ui, profile_head, profile_tail, audio
    start: int
    size: int
    name: str
    old: str
    new: str
    detail: str = ""
    offsets: list[int] = field(default_factory=list)
    labelled: bool = False
    noise: bool = False
    candidate: bool = False
    info: str = ""

    @property
    def title(self) -> str:
        end = "" if self.size <= 1 else f"..+0x{self.start + self.size - 1:02X}"
        return f"+0x{self.start:02X}{end}"


def _hex_run(data: bytes | None, start: int, size: int) -> str:
    if data is None:
        return "?"
    shown = data[start : start + min(size, MAX_RUN_BYTES)]
    text = " ".join(f"{byte:02X}" for byte in shown)
    return text + (" ..." if size > MAX_RUN_BYTES else "")


def _run_name(block: str, start: int) -> str:
    if block == "audio":
        address = AUDIO_BASE + start
        return AUDIO_GLOBALS.get(address & ~3, f"audio globals 0x{address:X}")
    if block == "profile_tail":
        return "profile object, unlabelled"
    return "unlabelled"


def group_changes(
    block: str, diffs: BlockDiff, before: bytes | None, after: bytes | None, base: int = 0
) -> list[Change]:
    """Group changed bytes by T1708 field (head) or contiguous run (unlabelled)."""
    changes: list[Change] = []
    fields: dict[int, list[int]] = {}
    runs: list[list[int]] = []
    last = -2
    for offset, _, _ in diffs:
        item = (
            field_at("ui" if block == "ui" else "profile", offset)
            if block in ("ui", "profile_head")
            else None
        )
        if item is not None:
            fields.setdefault(item.offset, []).append(offset)
            continue
        if offset == last + 1 and runs:
            runs[-1].append(offset)
        else:
            runs.append([offset])
        last = offset
    for start, offsets in fields.items():
        item = field_at("ui" if block == "ui" else "profile", start)
        assert item is not None and before is not None and after is not None
        old, new, detail = _field_values(item, before, after)
        changes.append(Change(block, start, item.size, item.name, old, new, detail, offsets))
    for run in runs:
        size = run[-1] - run[0] + 1
        start = run[0] + base
        name = _run_name(block, run[0])
        changes.append(
            Change(
                block,
                start,
                size,
                name,
                _hex_run(before, run[0], size),
                _hex_run(after, run[0], size),
                "",
                [offset + base for offset in run],
            )
        )
    changes.sort(key=lambda item: item.start)
    return changes


# --------------------------------------------------------------------------------------------------------------------
# Event analysis
# --------------------------------------------------------------------------------------------------------------------

FLAG_TAGS: tuple[str, ...] = (
    "NO-BYTE-CHANGE",
    "UNLABELLED-BYTE",
    "VALUE-MISMATCH",
    "NO-CHANGE-ON-PRESS",
    "PROFILE-ON-ACCEPT",
    "AV-CANDIDATE",
    "PAGE-OPEN",
    "PROFILE-LOAD",
)
FLAG_MEANING: dict[str, str] = {
    "NO-BYTE-CHANGE": "row value changed but the setting's bytes did not",
    "UNLABELLED-BYTE": "a changed byte no T1708 field maps to the changed row",
    "VALUE-MISMATCH": "decoded byte value differs from the displayed row index",
    "NO-CHANGE-ON-PRESS": "LEFT/RIGHT on an option row changed nothing (info)",
    "PROFILE-ON-ACCEPT": "profile block changed on Accept and equals the UI block (info)",
    "AV-CANDIDATE": "byte changed with an Audio/Video row (INFERRED storage)",
    "PAGE-OPEN": "UI block or audio globals filled when a Controls or Audio/Video page opened (info)",
    "PROFILE-LOAD": "profile or settings data changed on a screen that is not Controls or Audio/Video (info)",
}


@dataclass
class RowChange:
    key: str
    name: str
    position: int
    row_id: int
    old_index: int
    new_index: int
    old_label: str
    new_label: str
    primary: bool = False


@dataclass
class Flag:
    tag: str
    text: str
    setting: str | None = None


@dataclass
class EventReport:
    index: int
    poll: int | None
    edge: str
    buttons: list[str]
    after_offset: int | None
    kind: str | None = None
    mode_before: int | None = None
    mode_after: int | None = None
    screen_before: str = "unknown"
    screen_after: str = "unknown"
    cursor_before: str = "none"
    cursor_after: str = "none"
    cursor_key: str | None = None
    row_changes: list[RowChange] = field(default_factory=list)
    changes: list[Change] = field(default_factory=list)
    flags: list[Flag] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    value_before: str = ""
    value_after: str = ""
    hidden_noise: int = 0
    accept: bool = False
    cancel: bool = False
    layout_press: bool = False
    lr_press: bool = False
    field_changed: bool = False
    index_changed: bool = False
    accept_keys: list[str] = field(default_factory=list)
    accept_equal: bool | None = None
    turn_before: float | None = None
    turn_after: float | None = None
    old_index: int | None = None
    new_index: int | None = None
    layout_before: int | None = None
    layout_after: int | None = None
    cursor_index: int | None = None
    cursor_row_id: int | None = None
    cursor_options: int | None = None
    value_index_before: int | None = None
    value_index_after: int | None = None
    av_accept: bool = False
    profile_diffs: list[tuple[int, int, int]] = field(default_factory=list)
    audio_after_hex: str = ""

    def has(self, tag: str) -> bool:
        return any(item.tag == tag for item in self.flags)


@dataclass
class Unit:
    """One (event, after dump) comparison before the noise rule."""

    event: Event
    after_offset: int | None
    before: Decoded
    after: Decoded
    diffs: dict[str, BlockDiff]

    def positions(self) -> set[tuple[str, int]]:
        return {(block, offset) for block, diffs in self.diffs.items() for offset, _, _ in diffs}


def cursor_text(page: PageView | None) -> str:
    if page is None:
        return "no settings page"
    if page.cursor == NO_CURSOR:
        return "none"
    if page.kind == KIND_SETTINGS:
        label = SETTINGS_LIST_ROWS[page.cursor] if page.cursor < len(SETTINGS_LIST_ROWS) else "row"
        return f"Settings list: {label} (cursor {page.cursor})"
    if page.kind == KIND_CONTROLS:
        row = CONTROLS_BY_ID.get(page.cursor_id)
        if row is not None:
            return f"{row.label} (row id {row.row_id}, cursor {page.cursor})"
        return f"row id {page.cursor_id} (cursor {page.cursor})"
    return f"row index {page.cursor} (row id {page.cursor_id})"


def _controls_key(page: PageView | None) -> str | None:
    if page is None or page.kind != KIND_CONTROLS or page.cursor == NO_CURSOR:
        return None
    row = CONTROLS_BY_ID.get(page.cursor_id)
    return row.key if row is not None else None


def _rows_by_key(page: PageView) -> dict[int, RowNode]:
    if page.kind == KIND_CONTROLS:
        return {node.row_id: node for node in page.rows}
    return dict(enumerate(page.rows))


def _row_label(row: ControlsRow | None, index: int) -> str:
    if row is None:
        return str(index)
    if row.options:
        return option_label(row, index)
    return f"slider {index}" if row.key == "turn_speed" else str(index)


def compute_row_changes(before: PageView, after: PageView) -> list[RowChange]:
    changes = []
    old_rows, new_rows = _rows_by_key(before), _rows_by_key(after)
    for key, old in old_rows.items():
        new = new_rows.get(key)
        if new is None or new.selected == old.selected:
            continue
        if before.kind == KIND_CONTROLS:
            row = CONTROLS_BY_ID.get(old.row_id)
            name = row.label if row else f"row id {old.row_id}"
            setting = row.key if row else f"id{old.row_id}"
            old_label = _row_label(row, old.selected)
            new_label = _row_label(row, new.selected)
            position = old.index
        else:
            name = f"row index {key}"
            setting = f"av{key}"
            old_label, new_label = f"{old.selected}", f"{new.selected}"
            position = key
        changes.append(
            RowChange(
                setting,
                name,
                position,
                old.row_id,
                old.selected,
                new.selected,
                old_label,
                new_label,
            )
        )
    return changes


def _node_at(page: PageView | None, index: int) -> RowNode | None:
    if page is None or page.kind != KIND_AV or not 0 <= index < len(page.rows):
        return None
    return page.rows[index]


def _page_opened(
    before: Decoded, after: Decoded, page_before: PageView | None, page_after: PageView | None
) -> bool:
    """The UI block was first filled (all zero before) or a Controls or Audio/Video page appeared."""
    filled = before.ui is not None and after.ui is not None and not any(before.ui) and any(after.ui)
    opened = (
        page_after is not None
        and page_after.kind in (KIND_CONTROLS, KIND_AV)
        and (page_before is None or page_before.kind != page_after.kind)
    )
    return filled or opened


def event_kind(page_before: PageView | None, page_after: PageView | None) -> str | None:
    """Controls or Audio/Video if either side shows that page (before first), else the Settings list or None."""
    kinds = [page.kind for page in (page_before, page_after) if page is not None]
    for kind in kinds:
        if kind in (KIND_CONTROLS, KIND_AV):
            return kind
    return kinds[0] if kinds else None


def analyze_unit(unit: Unit, noise: dict[tuple[str, int], str]) -> EventReport:
    """Build the event report: decode, label the changes, apply the noise rule, raise the flags."""
    event, before, after = unit.event, unit.before, unit.after
    report = EventReport(
        event.index, event.poll, event.edge, list(event.buttons), unit.after_offset
    )
    report.mode_before, report.mode_after = before.mode, after.mode
    report.screen_before = before.top.screen if before.top else "unknown"
    report.screen_after = after.top.screen if after.top else "unknown"
    page_before, page_after = before.page, after.page
    report.kind = event_kind(page_before, page_after)
    report.cursor_before, report.cursor_after = cursor_text(page_before), cursor_text(page_after)
    report.cursor_key = _controls_key(page_before)
    pressed = event.edge in ("press", "mixed")
    buttons = set(event.buttons)
    key = report.cursor_key
    row_before = (
        CONTROLS_BY_ID.get(page_before.cursor_id)
        if page_before and page_before.kind == KIND_CONTROLS
        else None
    )
    report.accept = (
        row_before is not None and row_before.row_id == ACCEPT_ID and "A" in buttons and pressed
    )
    report.cancel = (
        row_before is not None and row_before.row_id == CANCEL_ID and "A" in buttons and pressed
    )
    report.layout_press = (
        row_before is not None and row_before.row_id == LAYOUT_ID and "A" in buttons and pressed
    )
    report.lr_press = pressed and bool(buttons & set(LR_BUTTONS)) and key in SETTING_KEYS
    if page_before is not None and page_before.kind == KIND_AV and page_before.cursor != NO_CURSOR:
        report.cursor_index, report.cursor_row_id = page_before.cursor, page_before.cursor_id
        node_before = _node_at(page_before, page_before.cursor)
        node_after = _node_at(page_after, page_before.cursor) if page_after else None
        report.cursor_options = node_before.options if node_before else None
        report.value_index_before = node_before.selected if node_before else None
        report.value_index_after = node_after.selected if node_after else None
        report.av_accept = page_before.cursor_id == AV_ACCEPT_ID and "A" in buttons and pressed
        report.audio_after_hex = after.audio.hex() if after.audio else ""
        if report.av_accept:
            report.profile_diffs = list(unit.diffs.get("profile", []))
    if (
        page_before
        and page_after
        and page_before.kind == page_after.kind
        and page_before.kind in (KIND_CONTROLS, KIND_AV)
    ):
        report.row_changes = compute_row_changes(page_before, page_after)
    moved = bool(buttons & set(MOVE_BUTTONS))
    owner = key if key in SETTING_KEYS and not moved else None
    owner_row = None
    if page_before and report.row_changes:
        for item in report.row_changes:
            same = (
                item.row_id == page_before.cursor_id
                if page_before.kind == KIND_CONTROLS
                else item.position == page_before.cursor
            )
            item.primary = same
        owner_row = next((item for item in report.row_changes if item.primary), None)
    # --- value text
    if key in SETTING_KEYS:
        row = CONTROLS_BY_KEY[key]
        if key == "turn_speed":
            report.turn_before, report.turn_after = turn_speed(before.ui), turn_speed(after.ui)
            report.value_before = value_text(key, None, before.ui)
            report.value_after = value_text(key, None, after.ui)
        else:
            found = {node.row_id: node for node in page_before.rows} if page_before else {}
            node_old = found.get(row.row_id)
            node_new = (
                {node.row_id: node for node in page_after.rows}.get(row.row_id)
                if page_after
                else None
            )
            report.old_index = node_old.selected if node_old else None
            report.new_index = node_new.selected if node_new else None
            report.value_before = (
                f"{option_label(row, report.old_index)} [{report.old_index}]"
                if node_old
                else "row not decoded"
            )
            report.value_after = (
                f"{option_label(row, report.new_index)} [{report.new_index}]"
                if node_new
                else "row not decoded"
            )
    elif owner_row is not None and report.kind == KIND_AV:
        report.value_before, report.value_after = owner_row.old_label, owner_row.new_label
    report.layout_before = int.from_bytes(before.ui[0:2], "little") if before.ui else None
    report.layout_after = int.from_bytes(after.ui[0:2], "little") if after.ui else None
    # --- labelled changes
    ui_changes = group_changes("ui", unit.diffs.get("ui", []), before.ui, after.ui)
    head_diffs = [item for item in unit.diffs.get("profile", []) if item[0] < HEAD_LEN]
    tail_diffs = [item for item in unit.diffs.get("profile", []) if item[0] >= HEAD_LEN]
    head_changes = group_changes("profile_head", head_diffs, before.profile, after.profile)
    tail_changes = group_changes("profile_tail", tail_diffs, before.profile, after.profile)
    audio_changes = group_changes("audio", unit.diffs.get("audio", []), before.audio, after.audio)
    for item in ui_changes:
        item.labelled = _ui_labelled(item, owner, unit)
    report.accept_keys = []
    if (report.accept or report.cancel) and head_changes:
        for item in head_changes:
            item.labelled = True
    if report.accept and head_changes:
        if before.profile and after.profile and after.ui:
            report.accept_equal = after.profile[:HEAD_LEN] == after.ui[:HEAD_LEN]
        for setting in SETTING_KEYS:
            if setting_bits_changed(setting, before.profile, after.profile):
                report.accept_keys.append(setting)
    everything = ui_changes + head_changes + tail_changes + audio_changes
    for item in everything:
        item.noise = (not item.labelled) and all(
            (_block_of(item), offset) in noise for offset in item.offsets
        )
        report.hidden_noise += item.noise
    report.changes = everything
    # --- setting level flags
    if owner is not None and report.kind == KIND_CONTROLS:
        _setting_flags(report, key, owner_row, before, after)
    # --- unlabelled and A/V
    visible = [item for item in everything if not item.labelled and not item.noise]
    if visible and _page_opened(before, after, page_before, page_after):
        opened = [item for item in visible if item.block in ("ui", "audio")]
        for item in opened:
            item.info = "PAGE-OPEN"
        if opened:
            where = ", ".join(f"{_block_of(item)} {item.title}" for item in opened[:8])
            report.flags.append(
                Flag(
                    "PAGE-OPEN", f"{len(opened)} range(s) filled as the page opened: {where}", None
                )
            )
        visible = [item for item in visible if not item.info]
    if visible and report.kind in (None, KIND_SETTINGS):
        for item in visible:
            item.info = "PROFILE-LOAD"
        where = ", ".join(f"{_block_of(item)} {item.title}" for item in visible[:8])
        report.flags.append(
            Flag(
                "PROFILE-LOAD",
                f"{len(visible)} range(s) changed off the settings pages: {where}",
                None,
            )
        )
        visible = []
    if report.kind == KIND_AV:
        if visible:
            for item in visible:
                item.candidate = True
            rows = ", ".join(
                f"{item.name} {item.old_label}>{item.new_label}" for item in report.row_changes
            ) or cursor_text(page_before)
            report.flags.append(
                Flag("AV-CANDIDATE", f"{len(visible)} changed range(s) with {rows}", None)
            )
        elif report.row_changes:
            names = ", ".join(item.name for item in report.row_changes)
            report.flags.append(
                Flag(
                    "NO-BYTE-CHANGE",
                    f"{names} changed but no dumped byte changed (storage outside the dumped ranges)",
                    None,
                )
            )
    elif visible:
        where = ", ".join(sorted({f"{_block_of(item)} {item.title}" for item in visible}))
        report.flags.append(
            Flag("UNLABELLED-BYTE", f"no T1708 field maps to the changed row: {where}", key)
        )
    # --- accept
    if report.accept and head_changes:
        if report.accept_equal:
            report.flags.append(
                Flag(
                    "PROFILE-ON-ACCEPT",
                    "profile block bytes changed on Accept and equal the UI block after it",
                    "accept",
                )
            )
        else:
            report.flags.append(
                Flag(
                    "VALUE-MISMATCH",
                    "profile block differs from the UI block after Accept",
                    "accept",
                )
            )
    if report.cancel and head_changes:
        report.flags.append(Flag("VALUE-MISMATCH", "profile block changed on Cancel", "cancel"))
    report.notes = _notes(report, before, after)
    return report


def _block_of(item: Change) -> str:
    return "profile" if item.block.startswith("profile") else item.block


def _ui_labelled(item: Change, owner: str | None, unit: Unit) -> bool:
    if owner is None:
        return False
    found = field_at("ui", item.start)
    if found is None:
        return False
    if found.kind == "flags":
        changed = 0
        for offset, old, new in unit.diffs.get("ui", []):
            if offset == FLAGS_OFFSET:
                changed = old ^ new
        return changed & ~SETTING_MASKS.get(owner, 0) == 0
    return found.setting == owner


def _setting_flags(
    report: EventReport, key: str, owner_row: RowChange | None, before: Decoded, after: Decoded
) -> None:
    report.field_changed = setting_bits_changed(key, before.ui, after.ui)
    report.index_changed = owner_row is not None and key != "turn_speed"
    anything = bool(report.changes) or bool(report.row_changes)
    if report.index_changed and not report.field_changed:
        report.flags.append(
            Flag(
                "NO-BYTE-CHANGE",
                f"{CONTROLS_BY_KEY[key].label} row index changed {owner_row.old_label} -> {owner_row.new_label} but its UI block bytes did not",
                key,
            )
        )
    elif report.lr_press and not report.field_changed:
        if anything:
            report.flags.append(
                Flag(
                    "NO-BYTE-CHANGE",
                    f"LEFT/RIGHT on {CONTROLS_BY_KEY[key].label} changed other data but not the setting's UI block bytes",
                    key,
                )
            )
        else:
            report.flags.append(
                Flag(
                    "NO-CHANGE-ON-PRESS",
                    f"LEFT/RIGHT on {CONTROLS_BY_KEY[key].label} changed nothing in the dumped ranges",
                    key,
                )
            )
    if report.field_changed and key != "turn_speed":
        decoded = decode_index(key, after.ui)
        if report.new_index is not None and decoded != report.new_index:
            shown = (
                "invalid bytes"
                if decoded is None
                else f"{option_label(CONTROLS_BY_KEY[key], decoded)} [{decoded}]"
            )
            report.flags.append(
                Flag(
                    "VALUE-MISMATCH",
                    f"{CONTROLS_BY_KEY[key].label}: row shows [{report.new_index}] but the bytes decode to {shown}",
                    key,
                )
            )
    if report.field_changed and key == "turn_speed":
        value = report.turn_after
        if value is None or not 0.0 <= value < 1.0:
            report.flags.append(
                Flag(
                    "VALUE-MISMATCH", f"Turn Speed bytes decode to {value}, outside 0..0.9999", key
                )
            )


def _notes(report: EventReport, before: Decoded, after: Decoded) -> list[str]:
    notes = []
    if report.screen_before != report.screen_after:
        notes.append(f"screen changed {report.screen_before} -> {report.screen_after}")
    if report.mode_before != report.mode_after:
        notes.append(f"mode word changed {_hex(report.mode_before)} -> {_hex(report.mode_after)}")
    if report.accept and not report.changes:
        notes.append(
            "Accept press: the profile block did not change (profile already equal to the UI block)"
        )
    for problem in sorted(set(before.problems + after.problems)):
        notes.append(f"decode: {problem}")
    return notes


def _hex(value: int | None) -> str:
    return "?" if value is None else f"0x{value:X}"


# --------------------------------------------------------------------------------------------------------------------
# Noise
# --------------------------------------------------------------------------------------------------------------------


def is_cycle_press(event: Event) -> bool:
    """A LEFT or RIGHT press: the press that changes a setting, so its own bytes must not count as noise."""
    return event.edge in ("press", "mixed") and bool(set(event.buttons) & set(LR_BUTTONS))


def compute_noise(units: list[Unit]) -> tuple[dict[tuple[str, int], str], int]:
    """(position -> reason, number of events). Idle events, or >= 30 percent of >= 5 events.

    The frequency rule counts the events that are not LEFT/RIGHT presses: a byte that only moves when a setting is
    cycled is the signal, a byte that moves on cursor moves and releases too is noise."""
    per_event: dict[int, set[tuple[str, int]]] = {}
    idle: set[tuple[str, int]] = set()
    for unit in units:
        positions = unit.positions()
        if not is_cycle_press(unit.event):
            per_event.setdefault(unit.event.index, set()).update(positions)
        if unit.event.idle:
            idle |= positions
    counts: Counter[tuple[str, int]] = Counter()
    for positions in per_event.values():
        counts.update(positions)
    total = len(per_event)
    noise: dict[tuple[str, int], str] = {position: "changed in an idle event" for position in idle}
    if total >= NOISE_MIN_EVENTS:
        for position, count in counts.items():
            if count * 100 >= NOISE_PERCENT * total and position not in noise:
                noise[position] = f"changed in {count} of {total} events"
    return noise, total


# --------------------------------------------------------------------------------------------------------------------
# Whole run
# --------------------------------------------------------------------------------------------------------------------


@dataclass
class Counters:
    attempted: int = 0
    usable: int = 0


@dataclass
class Analysis:
    events: int = 0
    idle_events: int = 0
    reports: list[EventReport] = field(default_factory=list)
    noise: dict[tuple[str, int], str] = field(default_factory=dict)
    noise_events: int = 0
    warnings: list[str] = field(default_factory=list)
    counters: Counters = field(default_factory=Counters)
    after: str = "first"


def _tag_offset(entry: dict[str, Any]) -> int:
    offset = entry.get("offset")
    if isinstance(offset, int) and not isinstance(offset, bool):
        return offset
    digits = "".join(ch for ch in str(entry.get("tag", ""))[-6:] if ch.isdigit())
    return int(digits) if digits else 0


def _decode(snapshot: Snapshot) -> Decoded:
    return decode_snapshot(snapshot.ranges, snapshot.row_base)


def analyze_directory(
    directory: Path, events: list[Event], after_mode: str = "first"
) -> tuple[list[Unit], Analysis]:
    """Pass one: load each event's dumps one file at a time and keep decoded states and raw diffs."""
    analysis = Analysis(after=after_mode)
    units: list[Unit] = []
    only_offset = None if after_mode in ("first", "all") else int(after_mode)
    for event in events:
        analysis.idle_events += event.idle
        analysis.events += 1
        before_entry = next(
            (
                item
                for item in event.dumps
                if str(item.get("kind", "")).endswith("before") or item.get("tag") == "before"
            ),
            None,
        )
        if before_entry is None:
            analysis.warnings.append(f"event {event.index}: no before dump listed")
            continue
        analysis.counters.attempted += 1
        snapshot = load_snapshot(_dump_path(directory, before_entry))
        for reason in snapshot.reasons:
            analysis.warnings.append(f"event {event.index} before: {reason}")
        if not snapshot.usable:
            continue
        analysis.counters.usable += 1
        before = _decode(snapshot)
        entries = sorted(
            (item for item in event.dumps if item is not before_entry), key=_tag_offset
        )
        if only_offset is not None:
            entries = [item for item in entries if _tag_offset(item) == only_offset]
        for entry in entries:
            analysis.counters.attempted += 1
            after_snapshot = load_snapshot(_dump_path(directory, entry))
            for reason in after_snapshot.reasons:
                analysis.warnings.append(
                    f"event {event.index} {entry.get('tag', 'after')}: {reason}"
                )
            if not after_snapshot.usable:
                continue
            analysis.counters.usable += 1
            after = _decode(after_snapshot)
            diffs = {
                "ui": diff_block(before.ui, after.ui),
                "profile": diff_block(before.profile, after.profile),
                "audio": diff_block(before.audio, after.audio),
            }
            units.append(Unit(event, _tag_offset(entry), before, after, diffs))
            if after_mode != "all":
                break
        else:
            if not entries:
                analysis.warnings.append(f"event {event.index}: no after dump to compare")
    return units, analysis


def run_analysis(directory: Path, events: list[Event], after_mode: str = "first") -> Analysis:
    """Both passes: units, noise, per event reports (idle events only feed the noise rule)."""
    units, analysis = analyze_directory(directory, events, after_mode)
    analysis.noise, analysis.noise_events = compute_noise(units)
    for unit in units:
        if not unit.event.idle:
            analysis.reports.append(analyze_unit(unit, analysis.noise))
    return analysis


# --------------------------------------------------------------------------------------------------------------------
# Coverage tables
# --------------------------------------------------------------------------------------------------------------------


@dataclass
class Coverage:
    key: str
    name: str
    presses: int = 0
    values: list[str] = field(default_factory=list)
    byte_changes: str = "n/a"
    profile: str = "n/a"
    status: str = "NOT-EXERCISED"
    flags: list[str] = field(default_factory=list)


def final_reports(reports: list[EventReport]) -> list[EventReport]:
    """One report per event (the last one when --after all produced several)."""
    last: dict[int, EventReport] = {}
    for report in reports:
        last[report.index] = report
    return [last[index] for index in sorted(last)]


def build_coverage(reports: list[EventReport]) -> list[Coverage]:
    rows = []
    reports = final_reports(reports)
    accepts = [item for item in reports if item.accept]
    for setting in (*SETTING_KEYS, "layout", "accept", "cancel"):
        row = CONTROLS_BY_KEY[setting]
        cov = Coverage(setting, row.label)
        flag_tags: Counter[str] = Counter()
        if setting in SETTING_KEYS:
            observed = [
                item
                for item in reports
                if item.cursor_key == setting
                and (item.index_changed or item.lr_press or item.field_changed)
            ]
            cov.presses = len(observed)
            values: list[str] = []
            for item in observed:
                if setting == "turn_speed":
                    candidates = [
                        f"{value:.4f}"
                        for value in (item.turn_before, item.turn_after)
                        if value is not None
                    ]
                else:
                    candidates = [
                        f"{option_label(row, index)}"
                        for index in (item.old_index, item.new_index)
                        if index is not None
                    ]
                for text in candidates:
                    if text not in values:
                        values.append(text)
            cov.values = values
            changed = [item for item in observed if item.index_changed or item.field_changed]
            cov.byte_changes = (
                f"{sum(item.field_changed for item in changed)}/{len(changed)}"
                if changed
                else "n/a"
            )
            if accepts:
                moved = sum(setting in item.accept_keys for item in accepts)
                cov.profile = f"{moved}/{len(accepts)} Accept(s) changed it"
            for item in observed:
                for flag in item.flags:
                    if flag.setting == setting:
                        flag_tags[flag.tag] += 1
            cov.flags = sorted(flag_tags)
            cov.status = _setting_status(setting, row, cov, flag_tags)
        elif setting == "layout":
            observed = [item for item in reports if item.layout_press]
            cov.presses = len(observed)
            seen = [
                f"layout {value}"
                for item in observed
                for value in (item.layout_before, item.layout_after)
                if value is not None
            ]
            cov.values = list(dict.fromkeys(seen))
            cov.status = "OK" if observed else "NOT-EXERCISED"
        elif setting == "accept":
            cov.presses = len(accepts)
            cov.profile = (
                f"{sum(bool(item.changes) for item in accepts)}/{len(accepts)} changed the profile"
                if accepts
                else "n/a"
            )
            mismatched = any(item.has("VALUE-MISMATCH") for item in accepts)
            cov.flags = sorted({flag.tag for item in accepts for flag in item.flags})
            cov.status = "NOT-EXERCISED" if not accepts else "MISMATCH" if mismatched else "OK"
        else:
            cancels = [item for item in reports if item.cancel]
            cov.presses = len(cancels)
            cov.flags = sorted({flag.tag for item in cancels for flag in item.flags})
            cov.status = (
                "NOT-EXERCISED"
                if not cancels
                else "MISMATCH"
                if any(item.has("VALUE-MISMATCH") for item in cancels)
                else "OK"
            )
        rows.append(cov)
    return rows


def _setting_status(setting: str, row: ControlsRow, cov: Coverage, flag_tags: Counter[str]) -> str:
    if cov.presses == 0:
        return "NOT-EXERCISED"
    if flag_tags["VALUE-MISMATCH"]:
        return "MISMATCH"
    if flag_tags["NO-BYTE-CHANGE"]:
        return "NO-BYTE-CHANGE"
    needed = TURN_SPEED_MIN_VALUES if setting == "turn_speed" else len(row.options)
    if len(cov.values) < needed:
        return "PARTIAL"
    return "OK"


@dataclass
class AvRow:
    name: str
    presses: int = 0
    values: list[str] = field(default_factory=list)
    ui: Counter[str] = field(default_factory=Counter)
    profile: Counter[str] = field(default_factory=Counter)
    audio: Counter[str] = field(default_factory=Counter)


def _audio_text(change: Change) -> str:
    return f"{change.title} (0x{AUDIO_BASE + change.start:X}) {change.old}>{change.new}"


def _audio_values(report: EventReport, change: Change) -> list[str]:
    """`0xADDR=0xVV` for the bytes of an audio change, from the audio globals after the press."""
    texts = []
    for offset in change.offsets:
        digits = report.audio_after_hex[2 * offset : 2 * offset + 2]
        if digits:
            texts.append(f"0x{AUDIO_BASE + offset:X}=0x{digits.upper()}")
    return texts


def build_av_rows(reports: list[EventReport]) -> list[AvRow]:
    """One row per Audio/Video cursor row that was pressed, sliders included (their value is not in +0xC8)."""
    table: dict[int, AvRow] = {}
    for report in final_reports(reports):
        if report.kind != KIND_AV or report.cursor_index is None:
            continue
        if report.cursor_row_id in (AV_ACCEPT_ID, AV_CANCEL_ID):
            continue
        changes = [
            item
            for item in report.changes
            if not item.labelled and not item.noise and not item.info
        ]
        pressed = report.edge in ("press", "mixed") and bool(set(report.buttons) & set(LR_BUTTONS))
        if not (pressed or changes or report.row_changes):
            continue
        slider = not report.cursor_options
        shape = "slider" if slider else f"{report.cursor_options} options"
        row = table.setdefault(
            report.cursor_index,
            AvRow(f"row index {report.cursor_index} (id {report.cursor_row_id}, {shape})"),
        )
        row.presses += 1
        seen = [] if slider else [report.value_index_before, report.value_index_after]
        for change in changes:
            if change.block == "audio" and slider:
                seen += _audio_values(report, change)
        for value in seen:
            text = str(value)
            if value is not None and text not in row.values:
                row.values.append(text)
        for change in changes:
            if change.block == "audio":
                row.audio[_audio_text(change)] += 1
            elif change.block == "ui":
                row.ui[f"{change.title} {change.old}>{change.new}"] += 1
            else:
                row.profile[f"{change.title} {change.old}>{change.new}"] += 1
    return [table[index] for index in sorted(table)]


@dataclass
class AvPair:
    audio_offset: int
    profile_offset: int
    value: int
    rows: list[str]


@dataclass
class AvStorage:
    """INFERRED pairing of the audio globals with the profile bytes copied at the A/V Accept press."""

    delta: int | None = None
    pairs: list[AvPair] = field(default_factory=list)
    unpaired: list[tuple[int, int, int]] = field(default_factory=list)
    accepts: int = 0


def pair_av_storage(reports: list[EventReport]) -> AvStorage:
    """Find the profile offset = audio offset + delta that takes the audio value at the A/V Accept, from the data."""
    final = final_reports(reports)
    tracked: dict[int, list[str]] = {}
    for report in final:
        if report.kind != KIND_AV or report.cursor_index is None:
            continue
        for change in report.changes:
            if change.block == "audio" and not change.labelled and not change.noise:
                for offset in change.offsets:
                    rows = tracked.setdefault(offset, [])
                    label = f"row index {report.cursor_index} (id {report.cursor_row_id})"
                    if label not in rows:
                        rows.append(label)
    accepts = [
        item for item in final if item.av_accept and item.profile_diffs and item.audio_after_hex
    ]
    storage = AvStorage(accepts=len(accepts))
    if not accepts or not tracked:
        storage.unpaired = [diff for item in accepts for diff in item.profile_diffs]
        return storage
    best = _best_delta(accepts, tracked)
    storage.delta = best
    if best is None:
        storage.unpaired = [diff for item in accepts for diff in item.profile_diffs]
        return storage
    paired: set[int] = set()
    for audio_offset, rows in sorted(tracked.items()):
        for item in accepts:
            audio = bytes.fromhex(item.audio_after_hex)
            changed = {offset: new for offset, _, new in item.profile_diffs}
            profile_offset = audio_offset + best
            if audio_offset < len(audio) and changed.get(profile_offset) == audio[audio_offset]:
                storage.pairs.append(
                    AvPair(audio_offset, profile_offset, audio[audio_offset], rows)
                )
                paired.add(profile_offset)
                break
    storage.unpaired = [
        diff for item in accepts for diff in item.profile_diffs if diff[0] not in paired
    ]
    return storage


def _best_delta(accepts: list[EventReport], tracked: dict[int, list[str]]) -> int | None:
    """The delta with the most value-equal pairs minus the pairs whose profile byte changed to another value."""
    candidates: set[int] = set()
    for item in accepts:
        for profile_offset, _, _ in item.profile_diffs:
            candidates.update(profile_offset - audio_offset for audio_offset in tracked)
    best: tuple[int, int] | None = None
    best_delta: int | None = None
    for delta in sorted(candidates, key=lambda value: (abs(value), value)):
        good = bad = 0
        for item in accepts:
            audio = bytes.fromhex(item.audio_after_hex)
            changed = {offset: new for offset, _, new in item.profile_diffs}
            for audio_offset in tracked:
                if audio_offset >= len(audio) or audio_offset + delta not in changed:
                    continue
                if changed[audio_offset + delta] == audio[audio_offset]:
                    good += 1
                else:
                    bad += 1
        score = (good - bad, good)
        if good and (best is None or score > best):
            best, best_delta = score, delta
    return best_delta


# --------------------------------------------------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------------------------------------------------


def _table(headers: list[str], rows: list[list[str]]) -> list[str]:
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join("---" for _ in headers) + " |"]
    lines += ["| " + " | ".join(cell.replace("|", "/") for cell in row) + " |" for row in rows]
    return lines


def flag_counts(reports: list[EventReport]) -> Counter[str]:
    counts: Counter[str] = Counter()
    for report in reports:
        for tag in {item.tag for item in report.flags}:
            counts[tag] += 1
    return counts


def _change_line(item: Change) -> str:
    block = {
        "ui": "UI block B",
        "profile_head": "profile P",
        "profile_tail": "profile P",
        "audio": "audio globals",
    }[item.block]
    prefix = f"{block} {item.title}"
    text = f"{prefix} {item.name}: {item.old} -> {item.new}"
    if item.detail:
        text += f" ({item.detail})"
    if item.size > 1 and item.offsets and item.block in ("ui", "profile_head"):
        text += " bytes " + " ".join(f"+0x{offset:02X}" for offset in item.offsets)
    if not item.labelled:
        text += " [unlabelled]"
    if item.candidate:
        text += " [AV-CANDIDATE]"
    if item.info:
        text += f" [{item.info}]"
    if item.noise:
        text += " [noise]"
    return text


def render_event(report: EventReport, show_noise: bool) -> list[str]:
    lines = [
        f"### Event {report.index}: {report.edge} {'-'.join(report.buttons) or '-'} (poll {report.poll if report.poll is not None else '?'}"
        + (f", after +{report.after_offset}" if report.after_offset is not None else "")
        + ")"
    ]
    lines.append(f"- mode word: {_hex(report.mode_before)} -> {_hex(report.mode_after)}")
    screens = (
        report.screen_before
        if report.screen_before == report.screen_after
        else f"{report.screen_before} -> {report.screen_after}"
    )
    lines.append(f"- screen: {screens}")
    cursor = (
        report.cursor_before
        if report.cursor_before == report.cursor_after
        else f"{report.cursor_before} -> {report.cursor_after}"
    )
    lines.append(f"- cursor row: {cursor}")
    if (report.value_before or report.value_after) and (
        report.value_before != report.value_after or report.lr_press
    ):
        name = CONTROLS_BY_KEY[report.cursor_key].label if report.cursor_key else "row"
        lines.append(f"- displayed value ({name}): {report.value_before} -> {report.value_after}")
    for item in report.row_changes:
        mark = "" if item.primary else " (other row)"
        lines.append(
            f"- row changed{mark}: {item.name} (id {item.row_id}) {item.old_label} [{item.old_index}] -> {item.new_label} [{item.new_index}]"
        )
    shown = [item for item in report.changes if show_noise or not item.noise]
    if shown:
        for item in shown:
            lines.append(f"- {_change_line(item)}")
    elif not report.row_changes:
        lines.append("- no setting byte or row index changed")
    if report.hidden_noise and not show_noise:
        lines.append(f"- {report.hidden_noise} noise change(s) hidden (--show-noise)")
    if report.flags:
        lines.append("- flags: " + " ".join(sorted({item.tag for item in report.flags})))
        lines += [f"  - {item.tag}: {item.text}" for item in report.flags]
    lines += [f"- note: {note}" for note in report.notes]
    lines.append("")
    return lines


def render_markdown(
    directory: Path,
    analysis: Analysis,
    show_noise: bool,
    notes_text: str | None,
    manifest_warnings: list[str],
) -> str:
    reports = analysis.reports
    final = final_reports(reports)
    lines = ["# Settings dump report (T1709)", ""]
    lines.append(
        f"Source: `{directory}`, {analysis.events} event(s) ({analysis.idle_events} idle), after dump: {analysis.after}."
    )
    lines.append(
        "Evidence: xemu-level MEASURED bytes of the unmodified game. The row label strings are not in the dump, so the displayed "
        "option labels are DERIVED from the MEASURED label tables (T1708): the owner must confirm what the screen showed. "
        "The Audio/Video storage section is INFERRED."
    )
    lines.append("")
    warnings = manifest_warnings + analysis.warnings
    if warnings:
        lines += ["## Warnings", ""]
        lines += [f"- {text}" for text in warnings[:30]]
        if len(warnings) > 30:
            lines.append(f"- ... {len(warnings) - 30} more")
        lines.append("")
    counts = flag_counts(reports)
    lines += ["## Flag summary", ""]
    lines += _table(
        ["flag", "events", "meaning"],
        [[tag, str(counts[tag]), FLAG_MEANING[tag]] for tag in FLAG_TAGS],
    )
    lines.append("")
    lines += ["## Coverage of the Controls settings", ""]
    coverage = build_coverage(reports)
    lines += _table(
        [
            "setting",
            "presses",
            "values seen",
            "UI byte changed",
            "profile after Accept",
            "flags",
            "status",
        ],
        [
            [
                item.name,
                str(item.presses),
                ", ".join(item.values) or "-",
                item.byte_changes,
                item.profile,
                " ".join(item.flags) or "-",
                item.status,
            ]
            for item in coverage
        ],
    )
    lines.append("")
    av_rows = build_av_rows(reports)
    lines += ["## Audio/Video rows (INFERRED storage discovery)", ""]
    if av_rows:
        lines += _table(
            [
                "row",
                "changes",
                "values seen",
                "UI block bytes",
                "profile bytes",
                "audio globals bytes",
            ],
            [
                [
                    item.name,
                    str(item.presses),
                    ", ".join(item.values),
                    _counter_text(item.ui),
                    _counter_text(item.profile),
                    _counter_text(item.audio),
                ]
                for item in av_rows
            ],
        )
        lines.append("")
    else:
        lines += ["No Audio/Video row change was observed.", ""]
    lines += render_av_storage(pair_av_storage(reports))
    lines += ["## Noise", ""]
    if analysis.noise:
        rows = [
            [block, f"+0x{offset:02X}", reason]
            for (block, offset), reason in sorted(analysis.noise.items())
        ]
        lines += _table(["block", "offset", "reason"], rows[:60])
        if len(rows) > 60:
            lines.append(f"... {len(rows) - 60} more")
        lines += [
            "",
            "A noise byte that belongs to the setting under the cursor is still shown for that press.",
        ]
        lines.append("")
    else:
        lines += [f"No noise byte ({analysis.noise_events} event(s) considered).", ""]
    lines += [f"## Events ({len(final) if analysis.after != 'all' else len(reports)} reported)", ""]
    for report in reports:
        lines += render_event(report, show_noise)
    if notes_text is not None:
        lines += ["## Owner notes", "", notes_text.rstrip(), ""]
    return "\n".join(lines).rstrip() + "\n"


def _ranges_of(diffs: list[tuple[int, int, int]]) -> list[str]:
    runs: list[list[tuple[int, int, int]]] = []
    for diff in sorted(diffs):
        if runs and diff[0] == runs[-1][-1][0] + 1:
            runs[-1].append(diff)
        else:
            runs.append([diff])
    return [
        f"P+0x{run[0][0]:X}"
        + (f"..+0x{run[-1][0]:X}" if len(run) > 1 else "")
        + ": "
        + " ".join(f"{old:02X}" for _, old, _ in run)
        + " -> "
        + " ".join(f"{new:02X}" for _, _, new in run)
        for run in runs
    ]


def render_av_storage(storage: AvStorage) -> list[str]:
    lines = ["## Audio/Video storage pairing (INFERRED)", ""]
    if not storage.accepts:
        return [*lines, "No Audio/Video Accept press that changed the profile was observed.", ""]
    if storage.delta is None:
        lines.append(
            f"{storage.accepts} Accept press(es) changed the profile but no audio global byte matches a changed profile byte."
        )
    else:
        lines.append(
            f"Profile offset = audio globals offset + 0x{storage.delta:X} (derived from the value equal pairs at {storage.accepts} Accept press(es), "
            f"profile address = 0x{PROFILE_BASE:X} + offset)."
        )
        lines.append("")
        lines += _table(
            ["audio global", "profile byte", "value at Accept", "changed by rows"],
            [
                [
                    f"0x{AUDIO_BASE + pair.audio_offset:X} (+0x{pair.audio_offset:02X})",
                    f"0x{PROFILE_BASE + pair.profile_offset:X} (P+0x{pair.profile_offset:X})",
                    f"0x{pair.value:02X}",
                    ", ".join(pair.rows),
                ]
                for pair in storage.pairs
            ],
        )
    lines.append("")
    if storage.unpaired:
        lines.append("Profile bytes changed at Accept without an audio global partner:")
        lines.append("")
        lines += [f"- {text}" for text in _ranges_of(storage.unpaired)]
        lines.append("")
    return lines


def _counter_text(counter: Counter[str]) -> str:
    return "; ".join(f"{text} x{count}" for text, count in counter.most_common(8)) or "-"


def to_json(directory: Path, analysis: Analysis, notes_text: str | None) -> dict[str, Any]:
    return {
        "source": str(directory),
        "events": analysis.events,
        "idle_events": analysis.idle_events,
        "after": analysis.after,
        "warnings": analysis.warnings,
        "flags": dict(flag_counts(analysis.reports)),
        "noise": [
            {"block": block, "offset": offset, "reason": reason}
            for (block, offset), reason in sorted(analysis.noise.items())
        ],
        "coverage": [asdict(item) for item in build_coverage(analysis.reports)],
        "av_rows": [
            {
                "row": item.name,
                "changes": item.presses,
                "values": item.values,
                "ui": dict(item.ui),
                "profile": dict(item.profile),
                "audio": dict(item.audio),
            }
            for item in build_av_rows(analysis.reports)
        ],
        "av_storage": asdict(pair_av_storage(analysis.reports)),
        "reports": [asdict(item) for item in analysis.reports],
        "owner_notes": notes_text,
    }


def fields_markdown() -> str:
    lines = ["## Settings block fields (P and B, offsets relative, T1708)", ""]
    lines += _table(
        ["offset", "width", "name", "encoding", "confidence"],
        [
            [f"+0x{item.offset:02X}", str(item.size), item.name, item.kind, item.confidence]
            for item in FIELD_MAP
        ],
    )
    lines += ["", "Flag byte +0x02 bits:", ""]
    lines += _table(
        ["bit", "meaning"], [[str(bit), name] for bit, name in sorted(FLAG_BITS.items())]
    )
    lines += ["", "## Controls page rows (cursor order)", ""]
    lines += _table(
        ["cursor", "row id", "label", "setting key", "options by index"],
        [
            [
                str(row.cursor),
                str(row.row_id),
                row.label,
                row.key,
                ", ".join(f"{i}={o}" for i, o in enumerate(row.options)) or "-",
            ]
            for row in CONTROLS_ROWS
        ],
    )
    lines += ["", "## Screens (page widget +0x08 builder)", ""]
    lines += _table(
        ["builder", "screen"], [[f"0x{builder:X}", name] for builder, name in SCREENS.items()]
    )
    lines += ["", "## Dump ranges", ""]
    lines += _table(
        ["key", "spec", "purpose"],
        [[item.key, item.spec, item.purpose] for item in SETTINGS_RANGES],
    )
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------------------------------------


def _after_arg(text: str) -> str:
    if text in ("first", "all"):
        return text
    try:
        value = int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(
            "--after takes first, all or an after offset in frames such as 2"
        ) from None
    if value < 1:
        raise argparse.ArgumentTypeError("--after offset must be >= 1")
    return str(value)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.settings_dump_report",
        description="Report which setting bytes change per button press (T1709). Subcommands: `ranges` prints the "
        "--dump-guest-range value, `fields` prints the field map (use ./ranges for a directory of that name).",
    )
    parser.add_argument(
        "directory", help="session directory with buttons.jsonl and buttons/guestdump.*"
    )
    parser.add_argument(
        "--write", action="store_true", help=f"write DIR/{REPORT_NAME} instead of printing"
    )
    parser.add_argument(
        "--after",
        type=_after_arg,
        default="first",
        help="first (default), all, or the after offset in frames",
    )
    parser.add_argument(
        "--show-noise", action="store_true", help="show the changes classified as noise"
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help=f"JSON instead of markdown (with --write also DIR/{JSON_NAME})",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="check buttons.jsonl against the T1629 spec and exit",
    )
    parser.add_argument(
        "--owner-notes", action="store_true", help=f"append DIR/{OWNER_NOTES_NAME} to the report"
    )
    return parser


def _fail(message: str, code: int = EXIT_USAGE) -> int:
    print(f"error: {message}", file=sys.stderr)
    return code


def run(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest_path = directory / MANIFEST_NAME
    if not directory.is_dir():
        return _fail(f"{directory} is not a directory")
    if not manifest_path.is_file():
        return _fail(f"{manifest_path} not found (was the run started with --dump-on-button?)")
    try:
        if args.validate:
            text_lines = manifest_path.read_text(encoding="utf-8", errors="replace").splitlines()
            problems = validate_manifest(text_lines) + check_dump_files(
                directory, load_manifest(manifest_path)
            )
            if problems:
                for problem in problems:
                    print(f"problem: {problem}")
                print(f"{len(problems)} manifest problem(s) in {manifest_path}")
                return EXIT_INVALID
            print(f"manifest OK: {len(text_lines)} line(s) in {manifest_path}")
            return EXIT_OK
        manifest = load_manifest(manifest_path)
        analysis = run_analysis(directory, manifest.events, args.after)
        if analysis.counters.attempted and not analysis.counters.usable:
            for text in analysis.warnings[:30]:
                print(f"warning: {text}", file=sys.stderr)
            return _fail(
                f"every dump ({analysis.counters.attempted}) was unreadable", EXIT_UNREADABLE
            )
        notes_path = directory / OWNER_NOTES_NAME
        notes_text = (
            notes_path.read_text(encoding="utf-8", errors="replace")
            if args.owner_notes and notes_path.is_file()
            else None
        )
        markdown = render_markdown(
            directory, analysis, args.show_noise, notes_text, manifest.warnings
        )
        if args.write:
            (directory / REPORT_NAME).write_text(markdown, encoding="utf-8")
            if args.json:
                (directory / JSON_NAME).write_text(
                    json.dumps(to_json(directory, analysis, notes_text), indent=1) + "\n",
                    encoding="utf-8",
                )
            print(f"wrote {directory / REPORT_NAME}")
        elif args.json:
            print(json.dumps(to_json(directory, analysis, notes_text), indent=1))
        else:
            print(markdown, end="")
        return EXIT_OK
    except OSError as error:
        return _fail(f"{error.filename or directory}: {error.strerror or error}")


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments[:1] == ["ranges"] and len(arguments) == 1:
        print(ranges_argument())
        return EXIT_OK
    if arguments[:1] == ["fields"] and len(arguments) == 1:
        print(fields_markdown(), end="")
        return EXIT_OK
    return run(build_parser().parse_args(arguments))


if __name__ == "__main__":
    sys.exit(main())
