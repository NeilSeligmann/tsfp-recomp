# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Mutations of tools/route_splice.py (T1639): the cut validation, the position remap, the rest gap, the mark and nav remap,
the self-check and the file safety. Run with `python tools/mutate/py_mutants.py --set route_splice` in a clean worktree.

Every `old` occurs exactly once (tests/test_mutation_anchors.py).
"""

MUTATIONS: list[dict] = []  # c_suites needs a native list, this set has none

_FILE = "tools/route_splice.py"
_TARGET = ["pytest: tests/test_t1639_route_splice.py"]


def _m(mutation_id: str, old: str, new: str, why: str, file: str = _FILE) -> dict:
    return {
        "id": f"route-splice-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": _TARGET,
        "why": why,
    }


PYTHON_MUTATIONS: list[dict] = [
    _m(
        "cut-empty-allowed",
        "if not 0 <= start < end <= polls:",
        "if not 0 <= start <= end <= polls:",
        "an empty cut is accepted",
    ),
    _m(
        "cut-past-end",
        "if not 0 <= start < end <= polls:",
        "if not 0 <= start < end <= polls + 1:",
        "a cut past the end of the record is accepted",
    ),
    _m(
        "cut-overlap-allowed",
        '        if start < end:\n            raise RouteError(f"cuts overlap',
        '        if start < end - 1:\n            raise RouteError(f"cuts overlap',
        "two cuts that overlap by one poll are accepted",
    ),
    _m(
        "cut-adjacent-refused",
        '        if start < end:\n            raise RouteError(f"cuts overlap',
        '        if start <= end:\n            raise RouteError(f"cuts overlap',
        "two adjacent cuts are refused",
    ),
    _m(
        "remap-after-cut",
        "        if position >= end:\n            shift += (end - start) - rest",
        "        if position > end:\n            shift += (end - start) - rest",
        "a position exactly at the cut end is not shifted",
    ),
    _m(
        "remap-rest-forgotten",
        "shift += (end - start) - rest",
        "shift += end - start",
        "later marks ignore the rest gap and drift by its length",
    ),
    _m(
        "remap-inside-cut",
        "            return position - shift - (position - start)",
        "            return position - shift",
        "a position inside a cut keeps its old index instead of the cut start",
    ),
    _m(
        "rest-gap-missing",
        '                add("", rest)',
        '                add("", 0)',
        "no rest gap at the join, a held input leaks across it",
    ),
    _m(
        "mark-before-rest",
        "            close_cuts(item.at)\n            flush()",
        "            flush()",
        "a mark at the cut end is written before the rest gap",
    ),
    _m(
        "nav-not-remapped",
        "            if item.text.strip().startswith(NAV_PREFIX):",
        "            if False and item.text.strip().startswith(NAV_PREFIX):",
        "nav lines keep their old poll indices",
    ),
    _m(
        "nav-edge-split",
        "        if start_p < end and end_p > start:  # the span touches the cut interior",
        "        if start_p < end and end_p >= start:  # the span touches the cut interior",
        "a nav span ending exactly at the cut start is treated as split",
    ),
    _m(
        "nav-partial-dropped",
        "            if start_p >= start and end_p <= end:\n                return None",
        "            if start_p >= start:\n                return None",
        "a nav span only partly inside a cut is silently dropped",
    ),
    _m(
        "nav-split-allowed",
        '            raise RouteError(\n                f"nav span at=',
        '            return line\n            raise RouteError(\n                f"nav span at=',
        "a span split by a cut is kept with wrong indices",
    ),
    _m(
        "verify-mark-count",
        "    if len(spliced.marks()) != len(original.marks()):",
        "    if False:",
        "the self-check no longer notices a changed mark count",
    ),
    _m(
        "verify-mark-order",
        "    if spliced.marks() != sorted(spliced.marks()):",
        "    if False:",
        "the self-check no longer notices descending marks",
    ),
    _m(
        "verify-states",
        "    if expand(spliced) != expected:",
        "    if False:",
        "the self-check no longer compares the poll states",
    ),
    _m(
        "default-rest",
        '        default=20,\n        help="pad-at-rest polls inserted at each join (default 20)",',
        '        default=0,\n        help="pad-at-rest polls inserted at each join (default 20)",',
        "the default rest gap is lost",
    ),
    _m(
        "out-is-input",
        "    if out.resolve() == path.resolve():",
        "    if False:",
        "--out naming the input overwrites the original with --force",
    ),
    _m(
        "waits-copied-verbatim",
        'line.replace(route_path.name, out.name) if line.lstrip().startswith("#") else line',
        "line",
        "the copied waits file still names the original route",
    ),
    _m(
        "events-recycled-pointer-proposed",
        'if spec.startswith("*") and event.new in ("0x0", "0"):',
        'if False and event.new in ("0x0", "0"):',
        "a wait on the recycled editor list is proposed for the preview mark",
        "tools/route_events.py",
    ),
    _m(
        "events-nonzero-pointer-dropped",
        'if spec.startswith("*") and event.new in ("0x0", "0"):',
        'if spec.startswith("*"):',
        "the editor list readiness sentinel is no longer proposed for the editor mark",
        "tools/route_events.py",
    ),
]
