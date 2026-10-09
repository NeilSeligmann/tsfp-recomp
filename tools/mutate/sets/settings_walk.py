# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Mutations of the settings walk route builder (T1709): the trailer check, press shape, label positions, count and rest
handling, the refusals. Run with `python tools/mutate/py_mutants.py --set settings_walk` in a clean checkout (the runner
refuses an uncommitted target file).

Kill suite: `tests/test_t1709_settings_walk.py`. Every `old` occurs exactly once (tests/test_mutation_anchors.py).
"""

MUTATIONS: list[dict] = []  # c_suites needs a native list, this set has none

_FILE = "tools/settings_walk.py"
_TARGET = ["pytest: tests/test_t1709_settings_walk.py"]


def _m(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"settings-walk-{mutation_id}",
        "file": _FILE,
        "old": old,
        "new": new,
        "targets": _TARGET,
        "why": why,
    }


PYTHON_MUTATIONS: list[dict] = [
    _m(
        "comment-lines",
        'if not line.strip() or line.startswith("#"):',
        "if not line.strip():",
        "comment lines of the route are parsed as poll counts",
    ),
    _m(
        "polls-first-token",
        "total += int(line.split()[0])",
        "total += int(line.split()[-1])",
        "the poll count is read from the last token",
    ),
    _m(
        "walk-comment",
        'if not line or line.startswith("//"):',
        "if not line:",
        "walk comments are not skipped",
    ),
    _m(
        "unknown-button",
        'or match.group("button") not in BUTTONS:',
        "or False:",
        "an unknown button name is accepted",
    ),
    _m(
        "count-default",
        'count = int(match.group("count") or 1)',
        'count = int(match.group("count") or 2)',
        "a press without xN is made twice",
    ),
    _m("count-zero", "if count < 1:", "if count < 0:", "x0 is accepted"),
    _m(
        "rest-default",
        'if match.group("rest") is not None else default_rest',
        'if match.group("rest") is not None else default_rest + 1',
        "the default rest is one poll too long",
    ),
    _m("empty-walk", "if not entries:", "if False:", "an empty walk is accepted"),
    _m(
        "trailer-missing",
        "if not lines or not lines[-1].startswith(POLLS_PREFIX):",
        "if not lines:",
        "a base route without trailer is accepted",
    ),
    _m(
        "trailer-mismatch",
        "if declared != route_polls(body):",
        "if False:",
        "a base route whose trailer lies is accepted",
    ),
    _m(
        "direction-token",
        'token = button if button in DIRECTIONS else f"{button}=255"',
        "token = button",
        "face buttons are written without the pressure value",
    ),
    _m(
        "label-poll",
        'labels.append({"route_poll": total, "button": button, "label": label})',
        'labels.append({"route_poll": total + hold, "button": button, "label": label})',
        "the label poll is after the hold",
    ),
    _m(
        "hold-written",
        'body.append(f"{hold} {token}")',
        'body.append(f"{hold + 1} {token}")',
        "the press is held one poll too long",
    ),
    _m(
        "rest-written",
        "body.append(str(rest_polls))",
        "body.append(str(rest))",
        "the per press rest is ignored",
    ),
    _m(
        "total-advance",
        "total += hold + rest_polls",
        "total += hold",
        "the label positions forget the rest",
    ),
    _m(
        "final-rest",
        "body.append(str(final_rest))",
        "body.append(str(final_rest + 1))",
        "the final rest is one poll too long",
    ),
    _m(
        "trailer-written",
        'body.append(f"{POLLS_PREFIX} {route_polls(body)}")',
        'body.append(f"{POLLS_PREFIX} {declared}")',
        "the old trailer is kept",
    ),
    _m(
        "exists-guard",
        "if args.out.exists() and not args.force:",
        "if False:",
        "an existing output is replaced without --force",
    ),
    _m(
        "same-file",
        "if args.out.resolve() == args.base.resolve():",
        "if False:",
        "the base route may be overwritten",
    ),
    _m("positive", "if value < 1:", "if value < 0:", "zero hold is accepted"),
]
