# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Mutations of the T1770 dead time work: the nav aware cuts of tools/route_trim.py, the tuned file-idle of tools/route_events.py,
the timeline classification of tools/route_timeline.py and the C ready counter of the nav machine. Run with
`python tools/mutate/py_mutants.py --set deadtime` in a clean worktree (the C mutant needs `c_suites.py --set deadtime`).

Every `old` occurs exactly once (tests/test_mutation_anchors.py).
"""

MUTATIONS: list[dict] = [
    {
        "id": "deadtime-nav-ready-polls-not-recorded",
        "file": "src/input/xinput_route_nav.c",
        "old": "        machine->ready_polls = machine->polls;",
        "new": "        machine->ready_polls = 0u;",
        "suites": ["test_route_nav"],
        "why": "the 'menu first ready after N polls' measurement always says 0",
    },
    {
        "id": "deadtime-nav-ready-seen-never-latched",
        "file": "src/input/xinput_route_nav.c",
        "old": "    if (!machine->ready_seen) {\n        machine->ready_seen = true;",
        "new": "    if (true) {\n        machine->ready_seen = true;",
        "suites": ["test_route_nav"],
        "why": "the ready poll is overwritten by every later poll instead of the first",
    },
]

_TRIM = "tools/route_trim.py"
_EVENTS = "tools/route_events.py"
_TIMELINE = "tools/route_timeline.py"
_T = ["pytest: tests/test_t1770_deadtime.py"]


def _m(mutation_id: str, file: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"deadtime-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": _T,
        "why": why,
    }


PYTHON_MUTATIONS: list[dict] = [
    _m(
        "trim-nav-pre-disabled",
        _TRIM,
        'if kind in ("lead-in", "interior") and ends_at_gated(right):',
        "if False:",
        "no idle before a gated nav step is cut",
    ),
    _m(
        "trim-nav-pre-margin",
        _TRIM,
        "                        gap.cut = (left, right - options.nav_margin)",
        "                        gap.cut = (left, right)",
        "no margin is kept before a nav step",
    ),
    _m(
        "trim-nav-pre-min-gap",
        _TRIM,
        "elif length >= options.nav_min_gap and length - options.nav_margin > 0:",
        "elif length - options.nav_margin > 0:",
        "a gap below the nav min gap is cut",
    ),
    _m(
        "trim-nav-pre-lead-in-always",
        _TRIM,
        'if kind == "lead-in" and options.lead_in != "nav":',
        'if kind == "lead-in" and False:',
        "the guest boot lead-in is cut before a nav step without --lead-in nav",
    ),
    _m(
        "trim-post-mark-nav-margin",
        _TRIM,
        "                if ends_at_gated(right):\n                    margin, min_gap = options.nav_margin, options.nav_min_gap",
        "                if False:\n                    margin, min_gap = options.nav_margin, options.nav_min_gap",
        "the post-mark margin stays 60 before a nav step",
    ),
    _m(
        "trim-tail-needs-guard",
        _TRIM,
        "elif tail and steps and spec is not None and is_guard(spec):",
        "elif tail and steps and spec is not None:",
        "an unguarded mark (min= only) gets the short tail margin",
    ),
    _m(
        "trim-nav-post-needs-nav-step",
        _TRIM,
        "                    and starts_at_gated(left)\n",
        "                    and True\n",
        "every pre-mark gap after any press is treated as after a nav step",
    ),
    _m(
        "trim-nav-post-needs-guarded-follower",
        _TRIM,
        "                    and (ends_at_gated(end) or end == route.polls)\n",
        "                    and True\n",
        "a pre-mark gap before an open-loop input after the mark is cut to the nav margin",
    ),
    _m(
        "trim-nav-post-follower-record-end",
        _TRIM,
        "                    and (ends_at_gated(end) or end == route.polls)\n",
        "                    and ends_at_gated(end)\n",
        "the end of the record does not count as guarded after the last mark",
    ),
    _m(
        "trim-nav-post-margin",
        _TRIM,
        '                    Options(**{**options.__dict__, "margin": options.nav_margin})',
        "                    Options(**{**options.__dict__})",
        "the pre-mark gap after a nav step keeps the long margin",
    ),
    _m(
        "trim-pressed-includes-dpad-zero",
        _TRIM,
        "for at, to in pressed)",
        "for at, to in gated)",
        "the idle before a stick driven (dpad=0) nav step is cut",
    ),
    _m(
        "trim-gated-needs-ready",
        _TRIM,
        "        if isinstance(menu, route_nav.Menu) and menu.page_builder is not None and menu.ready\n",
        "        if isinstance(menu, route_nav.Menu) and menu.page_builder is not None\n",
        "a menu without a ready= condition counts as a gate",
    ),
    _m(
        "trim-gated-needs-page",
        _TRIM,
        "        if isinstance(menu, route_nav.Menu) and menu.page_builder is not None and menu.ready\n",
        "        if isinstance(menu, route_nav.Menu) and menu.ready\n",
        "a menu without a page builder counts as a gate",
    ),
    _m(
        "trim-pressed-needs-dpad",
        _TRIM,
        "        and menu.dpad\n",
        "        and True\n",
        "dpad=0 menus count as pressed",
    ),
    _m(
        "trim-auto-off-without-navs",
        _TRIM,
        'options.nav_aware = args.nav_aware == "on" or (args.nav_aware == "auto" and has_navs)',
        'options.nav_aware = args.nav_aware != "off"',
        "auto turns nav aware on for routes without nav steps",
    ),
    _m(
        "trim-table-hides-nav-cuts",
        _TRIM,
        "if gap.length < min_gap and not show_all and gap.cut is None:",
        "if gap.length < min_gap and not show_all:",
        "short nav cuts are hidden from the table",
    ),
    _m(
        "events-tune-cap",
        _EVENTS,
        "    value = max(floor_ms, min(cap_ms, base))",
        "    value = max(floor_ms, base)",
        "no cap on the tuned file-idle",
    ),
    _m(
        "events-tune-floor",
        _EVENTS,
        "    value = max(floor_ms, min(cap_ms, base))",
        "    value = min(cap_ms, base)",
        "no floor on the tuned file-idle",
    ),
    _m(
        "events-tune-only-shortens",
        _EVENTS,
        "        value = min(value, old_ms)",
        "        value = value",
        "tuning can lengthen an existing wait",
    ),
    _m(
        "events-tune-rounding",
        _EVENTS,
        "        base = -(-half // TUNE_STEP_MS) * TUNE_STEP_MS",
        "        base = half",
        "the half lag is not rounded up to the step",
    ),
    _m(
        "events-tune-no-evidence-cap",
        _EVENTS,
        "    base = cap_ms\n    if lag_ms is not None:",
        "    base = floor_ms\n    if lag_ms is not None:",
        "no lag evidence tunes to the floor instead of the cap",
    ),
    _m(
        "events-lag-after-io",
        _EVENTS,
        "        if nav.t_ms is None or nav.t_ms <= last_ms or not nav.ready:",
        "        if nav.t_ms is None or nav.t_ms < last_ms or not nav.ready:",
        "a ready line at the same ms as the I/O counts",
    ),
    _m(
        "events-lag-needs-ready",
        _EVENTS,
        "        if nav.t_ms is None or nav.t_ms <= last_ms or not nav.ready:",
        "        if nav.t_ms is None or nav.t_ms <= last_ms:",
        "a not-ready menu line ends the lag",
    ),
    _m(
        "events-lag-needs-rows",
        _EVENTS,
        "        if not nav.cursor.isdigit():",
        "        if False:",
        "a rowless page ends the lag",
    ),
    _m(
        "events-scope-negation",
        _EVENTS,
        '    if wanted.startswith("!"):\n        return wanted[1:] not in lowered',
        '    if wanted.startswith("!"):\n        return wanted[1:] in lowered',
        "the !SUBSTR filter is inverted",
    ),
    _m(
        "events-scope-case",
        _EVENTS,
        "    lowered = path.lower()\n    if wanted.startswith",
        "    lowered = path\n    if wanted.startswith",
        "the filter is case sensitive",
    ),
    _m(
        "events-last-io-is-latest",
        _EVENTS,
        "    last = max(timed, key=lambda event: event.t_ms or 0)",
        "    last = timed[0]",
        "the first instead of the last I/O is used",
    ),
    _m(
        "events-tune-wait-keeps-rest",
        _EVENTS,
        '    new_spec = spec[: found.start()] + f"file-idle={new_ms}{scope}" + spec[found.end() :]',
        '    new_spec = spec[: found.start()] + f"file-idle={new_ms}{scope}"',
        "the fields after file-idle are dropped",
    ),
    _m(
        "events-tune-only-wait-lines",
        _EVENTS,
        '        if line.startswith("# wait:"):\n            body',
        '        if line.startswith("#"):\n            body',
        "other comment lines are parsed as waits",
    ),
    _m(
        "events-proposal-tuned",
        _EVENTS,
        "        if tune:\n            lag = settle_lag(segment, scope)",
        "        if False:\n            lag = settle_lag(segment, scope)",
        "--tune does not change the proposals",
    ),
    _m(
        "events-out-never-original",
        _EVENTS,
        "            if args.out.resolve() == record.resolve():",
        "            if False:",
        "--out may be the input record",
    ),
    _m(
        "timeline-stall-start",
        _TIMELINE,
        "            stall_start_t = anchor.t_ms - anchor.stalled_ms",
        "            stall_start_t = anchor.t_ms",
        "the wait stall is attributed to the idle before the mark",
    ),
    _m(
        "timeline-ready-counter",
        _TIMELINE,
        '                f"menu input-ready after {anchor.ready_polls} of {anchor.step_polls} step polls"\n                if anchor.ready_polls is not None',
        '                f"menu input-ready after {anchor.ready_polls} of {anchor.step_polls} step polls"\n                if False',
        "the ready counter is never reported",
    ),
    _m(
        "timeline-mark-dedupe",
        _TIMELINE,
        "        if found and not repeated:",
        "        if found:",
        "a mark with a wait is counted twice",
    ),
]
