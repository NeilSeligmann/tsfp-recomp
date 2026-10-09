# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Mutations of the settings route generator (T1709): the open loop press shape, the nav positions, the kept prefix, the
refusals, the waits file and the invariant check. Run with `python tools/mutate/py_mutants.py --set settings_route` in a clean
checkout (the runner refuses an uncommitted target file).

Kill suite: `tests/test_t1709_settings_route.py`. Every `old` occurs exactly once (tests/test_mutation_anchors.py).
"""

MUTATIONS: list[dict] = []  # c_suites needs a native list, this set has none

_FILE = "tools/settings_route.py"
_TARGET = ["pytest: tests/test_t1709_settings_route.py"]


def _m(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"settings-route-{mutation_id}",
        "file": _FILE,
        "old": old,
        "new": new,
        "targets": _TARGET,
        "why": why,
    }


PYTHON_MUTATIONS: list[dict] = [
    _m(
        "down-hold",
        "DOWN_HOLD = 5",
        "DOWN_HOLD = 4",
        "the d-pad is held 4 polls, not the recorded 5",
    ),
    _m("down-gap", "DOWN_GAP = 5", "DOWN_GAP = 4", "the release between two presses is 4 polls"),
    _m("a-hold", "A_HOLD = 6", "A_HOLD = 5", "the A press is held 5 polls"),
    _m(
        "settle",
        "SETTLE_POLLS = 60",
        "SETTLE_POLLS = 59",
        "the rest between two menu steps changes",
    ),
    _m(
        "default-rest",
        "DEFAULT_REST_POLLS = 240",
        "DEFAULT_REST_POLLS = 239",
        "the default final rest changes",
    ),
    _m(
        "settings-row",
        'Hop(MAIN_MENU, "Settings", 6)',
        'Hop(MAIN_MENU, "Settings", 5)',
        "Settings is reached with 5 DOWN presses",
    ),
    _m(
        "controls-row",
        'Hop(SETTINGS_MENU, "Controls", 1)',
        'Hop(SETTINGS_MENU, "Controls", 0)',
        "Controls is selected on the first row",
    ),
    _m(
        "audio-video-row",
        '"Audio/Video Options", 0)',
        '"Audio/Video Options", 1)',
        "Audio/Video Options is selected on the second row",
    ),
    _m(
        "settings-page-goes-on",
        '"settings": (MAIN_TO_SETTINGS,),',
        '"settings": (MAIN_TO_SETTINGS, Hop(SETTINGS_MENU, "Controls", 1)),',
        "the settings page also opens Controls",
    ),
    _m(
        "rest-matches-prefix-only",
        'return re.fullmatch(r"[0-9]+", line.strip()) is not None',
        'return re.match(r"[0-9]+", line.strip()) is not None',
        "a run with buttons counts as a rest run",
    ),
    _m(
        "one-rest-line",
        "while index < len(lines) and is_rest(lines[index]):",
        "if index < len(lines) and is_rest(lines[index]):",
        "only the first rest line after the wait is kept",
    ),
    _m(
        "mark-info-not-consumed",
        'startswith(("# mark-info:", "# wait:"))',
        'startswith(("# wait:",))',
        "a mark-info line ends the kept prefix before the wait line",
    ),
    _m(
        "wait-last-wins",
        "has_wait = has_wait or WAIT_MARK1.match(lines[index]) is not None",
        "has_wait = WAIT_MARK1.match(lines[index]) is not None",
        "only the last of the mark lines is looked at for the wait",
    ),
    _m(
        "eol-always-lf",
        'eol = "\\r\\n" if lines[0].endswith("\\r") else "\\n"',
        'eol = "\\n"',
        "a CRLF source gets LF lines appended",
    ),
    _m(
        "prefix-one-line-too-many",
        '"\\n".join(lines[:index]) + "\\n"',
        '"\\n".join(lines[: index + 1]) + "\\n"',
        "the first line after the rest is kept too",
    ),
    _m(
        "rest-lower-bound",
        "if not 1 <= rest_polls <= route_trim.RUN_LIMIT:",
        "if not 0 <= rest_polls <= route_trim.RUN_LIMIT:",
        "0 rest polls is accepted",
    ),
    _m(
        "rest-upper-bound",
        "if not 1 <= rest_polls <= route_trim.RUN_LIMIT:",
        "if not 1 <= rest_polls < route_trim.RUN_LIMIT:",
        "the biggest run length is refused",
    ),
    _m(
        "wait-needed-ignored-force",
        "if not prefix.has_wait and not force:",
        "if not prefix.has_wait:",
        "--force does not waive the missing mark 1 wait",
    ),
    _m(
        "wait-never-needed",
        "if not prefix.has_wait and not force:",
        "if False:",
        "a missing mark 1 wait is never refused",
    ),
    _m(
        "rest-never-used",
        "settle = SETTLE_POLLS if number < len(hops) - 1 else rest_polls",
        "settle = SETTLE_POLLS",
        "--rest-polls is ignored",
    ),
    _m(
        "position-without-settle",
        "position += polls + settle",
        "position += polls",
        "the next step starts before the rest is counted",
    ),
    _m(
        "range-end-off-by-one",
        "to=position + polls,",
        "to=position + polls + 1,",
        "the nav range ends one poll late",
    ),
    _m(
        "select-by-index",
        'select_kind="name",',
        'select_kind="index",',
        "the row is selected by index 0",
    ),
    _m("no-activate", "activate=True,", "activate=False,", "the nav step does not press A"),
    _m(
        "check-wait-always",
        "if require_wait and 1 not in waits:",
        "if 1 not in waits:",
        "the check cannot waive the mark 1 wait",
    ),
    _m(
        "check-wait-optional",
        "if require_wait and 1 not in waits:",
        "if False:",
        "a missing mark 1 wait passes the check",
    ),
    _m(
        "check-wait-mark-bound",
        "any(mark > len(marks) for mark in waits)",
        "any(mark >= len(marks) for mark in waits)",
        "the wait of the last mark does not fit",
    ),
    _m(
        "check-duplicate-waits",
        "len(set(waits)) != len(waits)",
        "len(set(waits)) == len(waits)",
        "two waits for one mark are fine and one is not",
    ),
    _m(
        "check-steps-after-mark-edge",
        "if step.at >= marks[0]]",
        "if step.at > marks[0]]",
        "a nav step right at mark 1 is not an after-mark step",
    ),
    _m(
        "check-known-pages-inverted",
        "] not in [\n        [(hop.menu, hop.label)",
        "] in [\n        [(hop.menu, hop.label)",
        "only unknown step sequences pass",
    ),
    _m(
        "check-early-menu-step",
        "step.at < marks[0] for _, step in found.steps",
        "step.at > marks[0] for _, step in found.steps",
        "a main menu step before mark 1 is not noticed",
    ),
    _m(
        "check-fallback-skipped",
        "for number, step in after:\n        problems += check_fallback",
        "for number, step in after[:0]:\n        problems += check_fallback",
        "the open loop presses are never compared with the range",
    ),
    _m(
        "check-rest-after-last-step",
        "route.polls - after[-1][1].to < 1",
        "route.polls - after[-1][1].to < 0",
        "a nav range that runs to the end of the record passes",
    ),
    _m(
        "fallback-walks-past-range",
        "while covered < step.to - step.at and index < len(lines):",
        "while covered <= step.to - step.at and index < len(lines):",
        "the walk takes the rest run after the range as part of it",
    ),
    _m(
        "fallback-comment-not-a-stop",
        'if not line.strip() or line.startswith("#"):',
        "if not line.strip():",
        "a comment line in a range is read as a press count",
    ),
    _m(
        "fallback-a-press",
        'token.startswith("A=")',
        'token.startswith("B=")',
        "a range that ends with another button passes",
    ),
    _m(
        "waits-all-marks",
        'WAITS_MARK1 = re.compile(r"mark1:")',
        'WAITS_MARK1 = re.compile(r"mark")',
        "the specs of marks 2 and up are copied too",
    ),
    _m(
        "waits-missing-file",
        "if source_waits is not None and source_waits.is_file():",
        "if source_waits is not None:",
        "a missing source .waits file is an error",
    ),
    _m(
        "out-is-source",
        "if out.resolve() == source.resolve():",
        "if out.resolve() != source.resolve():",
        "the source guard is inverted",
    ),
    _m(
        "exists-needs-force-ignored",
        "if target.exists() and not args.force:",
        "if target.exists():",
        "--force does not overwrite",
    ),
    _m(
        "waits-target-not-checked",
        "for target in (out, waits):",
        "for target in (out,):",
        "an existing .waits file is overwritten silently",
    ),
    _m(
        "default-out-name",
        'route.stem + ".settings" + route.suffix',
        'route.stem + ".nav" + route.suffix',
        "the default output name changes",
    ),
    _m(
        "waits-option-ignored",
        "source_waits = args.waits or source.with_suffix",
        "source_waits = source.with_suffix",
        "--waits is ignored",
    ),
    _m(
        "menu-table-check",
        "for name in (MAIN_MENU, SETTINGS_MENU) if name not in menus]",
        "for name in (MAIN_MENU,) if name not in menus]",
        "a table without settings-menu is accepted",
    ),
    _m(
        "rest-report",
        "result.new_polls - result.steps[-1].to} rest polls",
        "result.new_polls} rest polls",
        "the report shows the whole record as the rest",
    ),
    _m("check-exit-status", "return 2 if problems else 0", "return 0", "--check never fails"),
]
