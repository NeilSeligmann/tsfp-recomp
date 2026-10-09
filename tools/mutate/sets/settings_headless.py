# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Mutations of the pure parts of the headless settings dump run (T1709): the flag set, the header rewrite, the stop poll,
the host command, the timeout wrapper and the environment, the six assertions of `evaluate`, the newest route choice.
Run with `python tools/mutate/py_mutants.py --set settings_headless` in a clean checkout (the runner refuses an uncommitted
target file).

Kill suite: `tests/test_t1709_settings_headless.py`. Every `old` occurs exactly once (tests/test_mutation_anchors.py).
"""

MUTATIONS: list[dict] = []  # c_suites needs a native list, this set has none

_FILE = "tools/settings_headless.py"
_TARGET = ["pytest: tests/test_t1709_settings_headless.py"]


def _m(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"settings-headless-{mutation_id}",
        "file": _FILE,
        "old": old,
        "new": new,
        "targets": _TARGET,
        "why": why,
    }


PYTHON_MUTATIONS: list[dict] = [
    _m(
        "flag-dropped",
        '        "--overlay-consume",\n',
        "",
        "the headless flag set loses its last flag",
    ),
    _m(
        "flag-gpu",
        '        "--skip-intro",\n',
        '        "--skip-intro",\n        "--gpu-live",\n',
        "the headless set keeps a gpu flag",
    ),
    _m(
        "header-line-only-when-body",
        'if stripped and not stripped.startswith("#"):\n            break',
        'if stripped and not stripped.startswith("#"):\n            continue',
        "a flags comment after the header is rewritten too",
    ),
    _m(
        "header-prefix",
        "if raw.startswith(FLAGS_PREFIX):",
        "if FLAGS_PREFIX in raw:",
        "any line that mentions flags is replaced",
    ),
    _m(
        "header-ending",
        'ending = raw[len(raw.rstrip("\\r\\n")) :]',
        'ending = "\\n"',
        "the line ending of the flags line is normalised",
    ),
    _m(
        "header-join",
        'return "".join(lines)',
        'return "\\n".join(lines)',
        "the rewritten route gets extra newlines",
    ),
    _m(
        "header-missing",
        "raise InputError(\"the route has no '# flags:' header line\")",
        "return text",
        "a route without flags line is accepted",
    ),
    _m(
        "stop-trailer",
        "return polls + mark_shift + slack",
        "return polls + mark_shift",
        "the stop poll forgets the slack",
    ),
    _m(
        "stop-shift",
        "return polls + mark_shift + slack",
        "return polls + slack",
        "the stop poll forgets the mark wait shift",
    ),
    _m(
        "stop-last-trailer",
        "int(trailers[-1][len(POLLS_PREFIX) :].strip())",
        "int(trailers[0][len(POLLS_PREFIX) :].strip())",
        "the first trailer is read instead of the last",
    ),
    _m(
        "stop-no-trailer",
        "if not trailers:",
        "if False:",
        "a route without trailer gives a crash instead of a refusal",
    ),
    _m(
        "stop-unreadable",
        "except ValueError:\n        raise InputError(",
        "except KeyError:\n        raise InputError(",
        "an unreadable trailer escapes as a ValueError",
    ),
    _m(
        "mark-shift",
        "MARK_WAIT_SHIFT = 1500",
        "MARK_WAIT_SHIFT = 1470",
        "the default mark wait shift changes",
    ),
    _m(
        "slack",
        "STOP_SLACK = 300",
        "STOP_SLACK = 30",
        "the default slack changes",
    ),
    _m(
        "command-synthetic-pad",
        '        "--synthetic-pad",\n',
        "",
        "the host runs without the synthetic pad",
    ),
    _m(
        "command-nav-mode",
        '        "strict",\n',
        '        "lenient",\n',
        "the route navigation is not strict",
    ),
    _m(
        "command-stop-value",
        "        str(stop),\n",
        "        str(stop + 1),\n",
        "the stop poll is passed off by one",
    ),
    _m(
        "command-frames",
        'DUMP_AFTER_FRAMES = "2,30"',
        'DUMP_AFTER_FRAMES = "2"',
        "only one after dump per press",
    ),
    _m(
        "command-max-dumps",
        "MAX_DUMPS = 3000",
        "MAX_DUMPS = 300",
        "the dump budget is too small for the walk",
    ),
    _m(
        "command-hdd-swapped",
        '        "--hdd",\n        hdd,\n',
        '        "--hdd",\n        xbe,\n',
        "the XBE is passed as the HDD",
    ),
    _m(
        "command-route-swapped",
        '        "--replay-input",\n        route,\n',
        '        "--replay-input",\n        event_log,\n',
        "the event log is replayed as the input",
    ),
    _m(
        "command-dump-on-button",
        '        "--dump-on-button",\n',
        "",
        "the host does not dump on button presses",
    ),
    _m(
        "command-waits",
        "OWNER_WAITS = 1000000",
        "OWNER_WAITS = 1000",
        "the vblank owner waits budget is the recorded one",
    ),
    _m(
        "timeout-kill",
        'f"--kill-after={KILL_AFTER}"',
        '"--preserve-status"',
        "the timeout never hard kills",
    ),
    _m(
        "timeout-seconds",
        'return ["timeout", f"--kill-after={KILL_AFTER}", str(seconds), *command]',
        'return ["timeout", f"--kill-after={KILL_AFTER}", str(HOST_TIMEOUT_SECONDS), *command]',
        "the timeout argument is ignored",
    ),
    _m(
        "env-audio",
        'env["SDL_AUDIODRIVER"] = "dummy"',
        'env["SDL_AUDIODRIVER"] = "alsa"',
        "the host opens a real audio device",
    ),
    _m(
        "env-runtime",
        'env["XDG_RUNTIME_DIR"] = runtime_dir',
        'env["XDG_RUNTIME_DIR"] = env.get("XDG_RUNTIME_DIR", runtime_dir)',
        "the runtime dir of the caller is used",
    ),
    _m(
        "press-kind",
        'return report.get("kind") is not None and report.get("edge") in PRESS_EDGES',
        'return report.get("edge") in PRESS_EDGES',
        "navigation presses before the Settings pages count",
    ),
    _m(
        "press-edges",
        'PRESS_EDGES = ("press", "mixed")',
        'PRESS_EDGES = ("press", "mixed", "release")',
        "release events count as presses",
    ),
    _m(
        "press-edges-mixed",
        'PRESS_EDGES = ("press", "mixed")',
        'PRESS_EDGES = ("press",)',
        "coalesced press and release events do not count",
    ),
    _m(
        "press-threshold",
        "count >= MIN_WALK_PRESSES,",
        "count > MIN_WALK_PRESSES,",
        "exactly the minimum number of presses fails",
    ),
    _m(
        "coverage-status",
        'if status.get(key) != "OK"',
        "if status.get(key) is None",
        "any status of a walked setting passes",
    ),
    _m(
        "coverage-ok",
        "        not bad,\n",
        "        True,\n",
        "the coverage assertion cannot fail",
    ),
    _m(
        "accept-only",
        '        if entry.get("accept")\n',
        "",
        "a change on a press that is not an Accept satisfies the assertion",
    ),
    _m(
        "accept-offset",
        'and item.get("start") == ACCEPT_OFFSET',
        "and True",
        "any offset of the profile block satisfies the assertion",
    ),
    _m(
        "accept-old",
        'and item.get("old") == ACCEPT_OLD',
        "and True",
        "any old value satisfies the assertion",
    ),
    _m(
        "accept-new",
        'and item.get("new") == ACCEPT_NEW',
        "and True",
        "any new value satisfies the assertion",
    ),
    _m(
        "accept-block",
        'if item.get("block") == ACCEPT_BLOCK',
        "if True",
        "a change of any block satisfies the assertion",
    ),
    _m(
        "flag-count",
        'return (f"no {tag} flags", count == 0, f"{tag} count {count}")',
        'return (f"no {tag} flags", count <= 1, f"{tag} count {count}")',
        "one forbidden flag is tolerated",
    ),
    _m(
        "flag-tags",
        'FORBIDDEN_FLAGS = ("UNLABELLED-BYTE", "VALUE-MISMATCH")',
        'FORBIDDEN_FLAGS = ("UNLABELLED-BYTE",)',
        "the value mismatch flag is not asserted",
    ),
    _m(
        "av-rows",
        "ok = len(rows) >= MIN_AV_ROWS and",
        "ok = len(rows) >= 0 and",
        "no A/V rows are accepted",
    ),
    _m(
        "av-pairs",
        "and len(pairs) >= MIN_AV_PAIRS",
        "and len(pairs) >= 0",
        "no storage pairing is accepted",
    ),
    _m(
        "av-unpaired",
        "and unpaired == 0",
        "and True",
        "a pairing without a row is accepted",
    ),
    _m(
        "evaluate-drops-av",
        "        _check_av(report),\n",
        "",
        "the A/V assertion is not evaluated",
    ),
    _m(
        "newest-order",
        "return max(found)[2] if found else None",
        "return min(found)[2] if found else None",
        "the oldest route is chosen",
    ),
    _m(
        "newest-wait",
        'if re.search(r"^# wait: mark1:", text, re.MULTILINE):',
        "if True:",
        "a route without a mark 1 wait is chosen",
    ),
    _m(
        "tree-size",
        "if item.is_file() and not item.is_symlink()",
        "if item.is_dir()",
        "directories are counted as bytes",
    ),
    _m(
        "prepare-out-guard",
        "if any(out == item or out in item.parents for item in protected):",
        "if False:",
        "a folder holding a checkout can be removed",
    ),
    _m(
        "out-guard",
        "if out.is_dir() and any(out.iterdir()) and not args.force:",
        "if False:",
        "a non-empty output folder is replaced without --force",
    ),
    _m(
        "stop-validation",
        "if args.stop_at_poll is not None and args.stop_at_poll < 1:",
        "if False:",
        "a stop poll of zero is accepted",
    ),
]
