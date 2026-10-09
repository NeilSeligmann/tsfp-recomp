# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Mutations of the settings dump report (T1709): flag byte masks and polarity, field offsets, option label tables, the
page and row pointer math, the noise rule, the flag conditions and the coverage status. Run with
`python tools/mutate/py_mutants.py --set settings_dump` in a clean worktree.

Kill suite: `tests/test_t1709_settings_dump_report.py`. Every `old` occurs exactly once (tests/test_mutation_anchors.py).
"""

MUTATIONS: list[dict] = []  # c_suites needs a native list, this set has none

_REPORT = "tools/settings_dump_report.py"
_TARGET = ["pytest: tests/test_t1709_settings_dump_report.py"]


def _m(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"settings-dump-{mutation_id}",
        "file": _REPORT,
        "old": old,
        "new": new,
        "targets": _TARGET,
        "why": why,
    }


PYTHON_MUTATIONS: list[dict] = [
    _m(
        "inverse-look-bit",
        "        return flags & 0x01\n",
        "        return flags & 0x02\n",
        "Inverse Look reads bit 1 instead of bit 0",
    ),
    _m(
        "auto-aim-bit",
        "return (flags >> 7) & 0x01",
        "return (flags >> 6) & 0x01",
        "Auto Aim reads bit 6 instead of bit 7",
    ),
    _m(
        "auto-lookahead-bit",
        "return (flags >> 3) & 0x01",
        "return (flags >> 2) & 0x01",
        "Auto Lookahead reads bit 2 instead of bit 3",
    ),
    _m(
        "aim-mode-polarity",
        "return 0 if flags & 0x02 else 1",
        "return 1 if flags & 0x02 else 0",
        "Aim Mode: the SET bit is read as Hold",
    ),
    _m(
        "crouch-polarity",
        "return 0 if flags & 0x04 else 1",
        "return 1 if flags & 0x04 else 0",
        "Crouch: the SET bit is read as Hold",
    ),
    _m(
        "crouch-bit",
        "return 0 if flags & 0x04 else 1",
        "return 0 if flags & 0x08 else 1",
        "Crouch reads the Auto Lookahead bit",
    ),
    _m(
        "vibration-enable-inverted",
        "if not flags & VIBRATION_ENABLE:",
        "if flags & VIBRATION_ENABLE:",
        "bit 4 clear is read as vibration enabled",
    ),
    _m(
        "vibration-enable-bit",
        "VIBRATION_ENABLE = 0x10",
        "VIBRATION_ENABLE = 0x20",
        "vibration enable is read from bit 5",
    ),
    _m(
        "vibration-fire-hit-swapped",
        "VIBRATION_INDEX: dict[int, int] = {0x50: 1, 0x30: 2, 0x70: 3}",
        "VIBRATION_INDEX: dict[int, int] = {0x50: 2, 0x30: 1, 0x70: 3}",
        "Fire and Hit swap",
    ),
    _m(
        "vibration-kind-mask",
        "return VIBRATION_INDEX.get(flags & 0x70)",
        "return VIBRATION_INDEX.get(flags & 0x60)",
        "the enable bit is dropped from the kind lookup",
    ),
    _m(
        "weapon-change-bound",
        "return value if value < 5 else None",
        "return value if value < 6 else None",
        "Weapon Change index 5 is accepted",
    ),
    _m(
        "crosshair-bound",
        "return value if value < 3 else None",
        "return value if value < 4 else None",
        "Crosshair index 3 is accepted",
    ),
    _m(
        "weapon-change-offset",
        "WEAPON_CHANGE_OFFSET = 0x68",
        "WEAPON_CHANGE_OFFSET = 0x6C",
        "Weapon Change is read at +0x6C",
    ),
    _m(
        "crosshair-offset",
        "CROSSHAIR_OFFSET = 0x74",
        "CROSSHAIR_OFFSET = 0x70",
        "Crosshair is read at +0x70",
    ),
    _m(
        "turn-speed-offset",
        "TURN_SPEED_OFFSET = 0x78",
        "TURN_SPEED_OFFSET = 0x74",
        "Turn Speed is read at +0x74",
    ),
    _m("flags-offset", "FLAGS_OFFSET = 2", "FLAGS_OFFSET = 3", "the flag byte is read at +3"),
    _m(
        "tail-boundary",
        "if item[0] >= HEAD_LEN]\n    head_changes",
        "if item[0] > HEAD_LEN]\n    head_changes",
        "profile byte 0x7C is in neither the settings block nor the rest of the object",
    ),
    _m(
        "weapon-label-swap",
        '("Always", "Never", "Best", "If New", "If New and Best")',
        '("Always", "Best", "Never", "If New", "If New and Best")',
        "Weapon Change labels Never and Best swap",
    ),
    _m(
        "crosshair-label-swap",
        '("Off", "On and Moving", "On and Fixed")',
        '("Off", "On and Fixed", "On and Moving")',
        "Crosshair labels swap",
    ),
    _m(
        "vibration-label-swap",
        '("Off", "Fire", "Hit", "Fire&Hit")',
        '("Off", "Hit", "Fire", "Fire&Hit")',
        "Vibration labels Fire and Hit swap",
    ),
    _m(
        "aim-mode-label-swap",
        'ControlsRow(5, 5, "aim_mode", "Aim Mode", ("Toggle", "Hold"))',
        'ControlsRow(5, 5, "aim_mode", "Aim Mode", ("Hold", "Toggle"))',
        "Aim Mode labels swap",
    ),
    _m(
        "row-id-swap",
        'ControlsRow(2, 3, "inverse_look"',
        'ControlsRow(2, 2, "inverse_look"',
        "Inverse Look gets the Auto Aim row id",
    ),
    _m(
        "accept-row-id",
        'ControlsRow(10, 10, "accept"',
        'ControlsRow(10, 11, "accept"',
        "Accept gets the Cancel row id",
    ),
    _m("node-size", "NODE_SIZE = 0x10C", "NODE_SIZE = 0x108", "row nodes are 0x108 bytes"),
    _m(
        "node-misaligned",
        "if delta < 0 or delta % NODE_SIZE:",
        "if delta < 0:",
        "a pointer into the middle of a node is accepted",
    ),
    _m(
        "node-bound",
        "return index if index < count else None",
        "return index if index <= count else None",
        "a pointer one node past the table is accepted",
    ),
    _m(
        "widget-stride",
        "WIDGET_STRIDE = 0x23C",
        "WIDGET_STRIDE = 0x238",
        "page widgets are 0x238 bytes",
    ),
    _m("loop-guard-off", "if index in seen:", "if False:", "a looping row chain is not detected"),
    _m(
        "walk-limit", "MAX_WALK = 64", "MAX_WALK = 1000", "the row chain walk limit is far too high"
    ),
    _m(
        "row-count-cut",
        "if 0 < page.row_count < len(picked):",
        "if False:",
        "the page row count no longer cuts the chain",
    ),
    _m("row-type", "ROW_TYPE_BUTTON = 7", "ROW_TYPE_BUTTON = 8", "button rows are node type 8"),
    _m("state-open", "STATE_OPEN = 3", "STATE_OPEN = 2", "an open page has state 2"),
    _m(
        "top-ignores-child",
        "pool = [page for page in pool if page.child == 0] or pool",
        "pool = pool",
        "a page with an open child page can be the top",
    ),
    _m(
        "noise-percent",
        "NOISE_PERCENT = 30",
        "NOISE_PERCENT = 40",
        "frequency noise needs 40 percent",
    ),
    _m(
        "noise-percent-low",
        "NOISE_PERCENT = 30",
        "NOISE_PERCENT = 20",
        "frequency noise needs only 20 percent",
    ),
    _m(
        "noise-boundary",
        "if count * 100 >= NOISE_PERCENT * total and position not in noise:",
        "if count * 100 > NOISE_PERCENT * total and position not in noise:",
        "exactly 30 percent is not noise",
    ),
    _m(
        "noise-min-events",
        "NOISE_MIN_EVENTS = 5",
        "NOISE_MIN_EVENTS = 4",
        "frequency noise applies from 4 events",
    ),
    _m(
        "noise-min-boundary",
        "if total >= NOISE_MIN_EVENTS:",
        "if total > NOISE_MIN_EVENTS:",
        "frequency noise needs more than 5 events",
    ),
    _m(
        "idle-noise-dropped",
        "idle |= positions",
        "idle |= set()",
        "idle events no longer define noise",
    ),
    _m(
        "noise-block-key",
        'return "profile" if item.block.startswith("profile") else item.block',
        "return item.block",
        "profile noise is looked up under the wrong block name",
    ),
    _m(
        "labelled-never-noise",
        "item.noise = (not item.labelled) and all(",
        "item.noise = all(",
        "the setting's own bytes can be hidden as noise",
    ),
    _m(
        "flag-byte-any-bit-labelled",
        "return changed & ~SETTING_MASKS.get(owner, 0) == 0",
        "return True",
        "any bit of the flag byte is labelled for the row under the cursor",
    ),
    _m(
        "no-byte-change-inverted",
        "if report.index_changed and not report.field_changed:",
        "if report.index_changed and report.field_changed:",
        "NO-BYTE-CHANGE fires when the byte did change",
    ),
    _m(
        "no-change-on-press-anything",
        "        if anything:\n",
        "        if not anything:\n",
        "a LEFT/RIGHT press that changed nothing is NO-BYTE-CHANGE and vice versa",
    ),
    _m(
        "mismatch-inverted",
        "if report.new_index is not None and decoded != report.new_index:",
        "if report.new_index is not None and decoded == report.new_index:",
        "VALUE-MISMATCH fires on matching values",
    ),
    _m(
        "turn-speed-range",
        "if value is None or not 0.0 <= value < 1.0:",
        "if value is None or not 0.0 <= value < 2.0:",
        "a Turn Speed of 1.5 is accepted",
    ),
    _m(
        "accept-row",
        "row_before is not None and row_before.row_id == ACCEPT_ID",
        "row_before is not None and row_before.row_id == CANCEL_ID",
        "Accept is detected on the Cancel row",
    ),
    _m(
        "layout-row",
        "row_before is not None and row_before.row_id == LAYOUT_ID",
        "row_before is not None and row_before.row_id == ACCEPT_ID",
        "the layout press is detected on the Accept row",
    ),
    _m(
        "accept-equal-inverted",
        "report.accept_equal = after.profile[:HEAD_LEN] == after.ui[:HEAD_LEN]",
        "report.accept_equal = after.profile[:HEAD_LEN] != after.ui[:HEAD_LEN]",
        "PROFILE-ON-ACCEPT needs a profile that differs from the UI block",
    ),
    _m(
        "accept-flag-inverted",
        "        if report.accept_equal:\n",
        "        if not report.accept_equal:\n",
        "the Accept note and mismatch swap",
    ),
    _m(
        "cancel-head-unlabelled",
        "if (report.accept or report.cancel) and head_changes:",
        "if report.accept and head_changes:",
        "a profile change on Cancel is flagged twice",
    ),
    _m(
        "cursor-move-owns-bytes",
        "owner = key if key in SETTING_KEYS and not moved else None",
        "owner = key if key in SETTING_KEYS else None",
        "a cursor move owns the setting bytes",
    ),
    _m(
        "av-branch-off",
        "    if report.kind == KIND_AV:\n",
        "    if report.kind == KIND_AV and False:\n",
        "Audio/Video events are treated as Controls events",
    ),
    _m(
        "av-no-byte-change-off",
        "        elif report.row_changes:\n            names",
        "        elif False:\n            names",
        "an A/V row change without bytes raises no flag",
    ),
    _m(
        "run-contiguity",
        "if offset == last + 1 and runs:",
        "if offset == last and runs:",
        "adjacent unlabelled bytes are separate ranges",
    ),
    _m(
        "head-boundary",
        "if item[0] < HEAD_LEN]\n    tail_diffs",
        "if item[0] <= HEAD_LEN]\n    tail_diffs",
        "profile byte 0x7C is read as part of the settings block",
    ),
    _m(
        "audio-base",
        "AUDIO_BASE = 0x5236A0",
        "AUDIO_BASE = 0x5236A4",
        "audio global names are looked up at the wrong address",
    ),
    _m(
        "after-first-stops",
        'if after_mode != "all":',
        'if after_mode == "all":',
        "--after all stops after the first dump and first uses every dump",
    ),
    _m(
        "after-offset-filter",
        "entries = [item for item in entries if _tag_offset(item) == only_offset]",
        "entries = [item for item in entries if _tag_offset(item) != only_offset]",
        "--after K picks every dump but K",
    ),
    _m(
        "final-report-first",
        "last[report.index] = report",
        "last.setdefault(report.index, report)",
        "coverage uses the first after dump instead of the last",
    ),
    _m(
        "coverage-mismatch-off",
        'if flag_tags["VALUE-MISMATCH"]:',
        "if False:",
        "VALUE-MISMATCH never reaches the coverage status",
    ),
    _m(
        "coverage-partial-boundary",
        "if len(cov.values) < needed:",
        "if len(cov.values) <= needed:",
        "a fully exercised setting is PARTIAL",
    ),
    _m(
        "turn-speed-values",
        "TURN_SPEED_MIN_VALUES = 3",
        "TURN_SPEED_MIN_VALUES = 2",
        "two Turn Speed values are enough",
    ),
    _m(
        "exit-unreadable",
        'f"every dump ({analysis.counters.attempted}) was unreadable", EXIT_UNREADABLE',
        'f"every dump ({analysis.counters.attempted}) was unreadable", EXIT_USAGE',
        "all dumps unreadable exits 2",
    ),
    _m(
        "av-cursor-text",
        'return f"row index {page.cursor} (row id {page.cursor_id})"',
        'return f"row index {page.cursor_id} (row id {page.cursor})"',
        "the A/V cursor row swaps index and id",
    ),
    _m(
        "top-ignores-stale",
        "[page for page in pages if page.builder in SCREENS and page.state in LIVE_STATES]",
        "[page for page in pages if page.builder in SCREENS]",
        "a closed stale page can be the top screen",
    ),
    _m(
        "analysis-ignores-state",
        "            and page.state in LIVE_STATES\n            and page.row_count > 0",
        "            and page.row_count > 0",
        "the stale closed Controls slot is picked while the Settings list is on screen",
    ),
    _m(
        "analysis-ignores-empty",
        "            and page.state in LIVE_STATES\n            and page.row_count > 0",
        "            and page.state in LIVE_STATES",
        "a live page without rows is picked as the settings page",
    ),
    _m(
        "opening-state",
        "STATE_OPENING = 1",
        "STATE_OPENING = 0",
        "an opening page (state 1) is not live",
    ),
    _m(
        "open-preferred",
        "pool = [page for page in pool if page.state == STATE_OPEN] or pool",
        "pool = pool",
        "an opening page wins over an open one",
    ),
    _m(
        "event-kind-first",
        "        if kind in (KIND_CONTROLS, KIND_AV):\n            return kind",
        "        if kind == KIND_AV:\n            return kind",
        "a Controls page is not recognised as the event kind",
    ),
    _m(
        "ui-fill-not-detected",
        "and not any(before.ui) and any(after.ui)",
        "and any(before.ui) and any(after.ui)",
        "the first fill of the UI block is not detected",
    ),
    _m(
        "page-open-detection",
        "and (page_before is None or page_before.kind != page_after.kind)",
        "and page_before is None",
        "a page that opens over the Settings list is not an opened page",
    ),
    _m(
        "page-open-blocks",
        'opened = [item for item in visible if item.block in ("ui", "audio")]',
        'opened = [item for item in visible if item.block in ("ui",)]',
        "audio globals filled at page open are not absorbed",
    ),
    _m(
        "page-open-everything",
        'opened = [item for item in visible if item.block in ("ui", "audio")]',
        "opened = list(visible)",
        "profile changes at page open are absorbed too",
    ),
    _m(
        "profile-load-screens",
        "if visible and report.kind in (None, KIND_SETTINGS):",
        "if visible and report.kind is None:",
        "changes on the Settings list are not profile load",
    ),
    _m(
        "profile-load-everywhere",
        "if visible and report.kind in (None, KIND_SETTINGS):",
        "if visible and report.kind in (None, KIND_SETTINGS, KIND_CONTROLS):",
        "unlabelled changes on the Controls page are profile load",
    ),
    _m(
        "cycle-press-counted-as-noise",
        'return event.edge in ("press", "mixed") and bool(set(event.buttons) & set(LR_BUTTONS))',
        "return False",
        "the bytes of a LEFT/RIGHT press count towards the frequency noise",
    ),
    _m(
        "av-accept-row",
        'report.av_accept = page_before.cursor_id == AV_ACCEPT_ID and "A" in buttons and pressed',
        'report.av_accept = page_before.cursor_id == AV_CANCEL_ID and "A" in buttons and pressed',
        "the A/V Accept press is detected on Cancel",
    ),
    _m(
        "av-skip-accept-rows",
        "if report.cursor_row_id in (AV_ACCEPT_ID, AV_CANCEL_ID):",
        "if report.cursor_row_id in (AV_CANCEL_ID,):",
        "presses on A/V Accept are listed as a settings row",
    ),
    _m(
        "av-slider-detection",
        "slider = not report.cursor_options",
        "slider = False",
        "slider rows are listed as option rows",
    ),
    _m(
        "av-pair-value-check",
        "changed.get(profile_offset) == audio[audio_offset]",
        "changed.get(profile_offset) is not None",
        "a profile byte with a different value is paired",
    ),
    _m(
        "profile-base",
        "PROFILE_BASE = 0x7BEDC4",
        "PROFILE_BASE = 0x7BEDC0",
        "profile addresses are shifted",
    ),
    _m(
        "settings-list-labels",
        'SETTINGS_LIST_ROWS = ("Audio/Video Options", "Controls", "Copy Profile")',
        'SETTINGS_LIST_ROWS = ("Controls", "Audio/Video Options", "Copy Profile")',
        "Settings list rows swap",
    ),
]
