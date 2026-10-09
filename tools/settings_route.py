# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1709: a short route that ends on the Settings > Controls page (front end mode 0x65), made from a recorded route.

  python -m tools.settings_route ROUTE [--out FILE] [--page controls|settings|audio-video] [--rest-polls N] [--force]
  python -m tools.settings_route --check ROUTE

The settings dump session needs the title at the Settings page with the pad handed over there, not in the Map Maker preview.
The first part of any owner route is the same: title, profile select, mark 1 (main menu, profile loaded, the `# wait: mark1:`
line). This keeps the record up to and including mark 1, its `# mark-info:` / `# wait:` lines and the rest polls that follow,
drops everything after, and appends the menu steps main menu -> Settings [-> Controls | Audio/Video Options] as closed loop
`# nav:` steps (docs/t1640-nav-tools.md) with the open loop presses of the same polls as the fallback, then a rest of
`--rest-polls` so the page is ready when the pad is handed over, and the `# polls:` trailer. The kept text is byte exact, so the
header (xbe-sha256, flags-sha256, flags, budgets) and with it the T1618 route flag identity stay those of the source.
The `.waits` file next to the output carries only the mark 1 specs of the source's `.waits`. The source is never modified.

Fallback press shape: DOWN held 5 polls, released 5, then A held 6 (`A=255`), the shape of the recorded main menu steps of the
owner routes. The cursor starts on row 0 of both menus (main menu: Settings is item id 6, the 7th row; Settings: Audio/Video
Options row 0, Controls row 1, MEASURED in tools/data/menu_nav.txt). FABRICATED input: it only drives the menus.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

from tools import route_trim
from tools.play import route_nav as grammar
from tools.play import route_wait

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MENUS = ROOT / grammar.DEFAULT_MENU_FILE
DOWN_HOLD = 5
DOWN_GAP = 5
A_HOLD = 6
SETTLE_POLLS = 60  # rest between two menu steps, a page is open within ~50 polls
DEFAULT_REST_POLLS = 240
MAIN_MENU = "main-menu"
SETTINGS_MENU = "settings-menu"
MARK_LINE = re.compile(r"# mark:\s*at=(\d+)\s*")
WAIT_MARK1 = re.compile(r"# wait:\s*mark1:")
WAITS_MARK1 = re.compile(r"mark1:")


class ToolError(ValueError):
    """The request is refused (exit 2)."""


@dataclass(frozen=True)
class Hop:
    """One menu step: select the row `label` of `menu` with `downs` presses of DOWN from the top row, then A."""

    menu: str
    label: str
    downs: int


MAIN_TO_SETTINGS = Hop(MAIN_MENU, "Settings", 6)
PAGES: dict[str, tuple[Hop, ...]] = {
    "settings": (MAIN_TO_SETTINGS,),
    "controls": (MAIN_TO_SETTINGS, Hop(SETTINGS_MENU, "Controls", 1)),
    "audio-video": (MAIN_TO_SETTINGS, Hop(SETTINGS_MENU, "Audio/Video Options", 0)),
}


@dataclass(frozen=True)
class Prefix:
    text: str  # the kept lines, byte exact, ending with a newline
    polls: int  # polls inside the kept text
    has_wait: bool  # a `# wait: mark1:` line is kept
    eol: str


@dataclass(frozen=True)
class Generated:
    text: str
    source_polls: int
    kept_polls: int
    new_polls: int
    steps: tuple[grammar.NavStep, ...]


def is_rest(line: str) -> bool:
    return re.fullmatch(r"[0-9]+", line.strip()) is not None


def split_prefix(text: str) -> tuple[Prefix, int]:
    """The part of a record this tool keeps and the source's poll count. Raises ToolError."""
    try:
        route = route_trim.parse_route(text)
    except route_trim.RouteError as error:
        raise ToolError(f"the source is not a usable record: {error}") from None
    if not route.marks():
        raise ToolError("the source has no mark 1 (no '# mark: at=N' line)")
    lines = text.split("\n")
    position = 0
    mark_index = -1
    for index, raw in enumerate(lines):
        line = raw.rstrip("\r")
        match = MARK_LINE.fullmatch(line)
        if match:
            mark_index = index
            break
        if line.strip() and not line.startswith("#"):
            position += int(line.split()[0])
    if mark_index < 0:
        raise ToolError("the source has no mark 1 (no '# mark: at=N' line)")
    index = mark_index + 1
    has_wait = False
    while index < len(lines) and lines[index].startswith(("# mark-info:", "# wait:")):
        has_wait = has_wait or WAIT_MARK1.match(lines[index]) is not None
        index += 1
    polls = position
    while index < len(lines) and is_rest(lines[index]):
        polls += int(lines[index].strip())
        index += 1
    eol = "\r\n" if lines[0].endswith("\r") else "\n"
    return Prefix("\n".join(lines[:index]) + "\n", polls, has_wait, eol), route.polls


def hop_runs(hop: Hop) -> list[str]:
    """The open loop presses of a hop: DOWN x downs (hold, release), then A."""
    runs: list[str] = []
    for _ in range(hop.downs):
        runs += [f"{DOWN_HOLD} DOWN", f"{DOWN_GAP}"]
    runs.append(f"{A_HOLD} A=255")
    return runs


def hop_polls(hop: Hop) -> int:
    return hop.downs * (DOWN_HOLD + DOWN_GAP) + A_HOLD


def generate(text: str, page: str, rest_polls: int, force: bool = False) -> Generated:
    """The settings route for a source record text. Raises ToolError."""
    if page not in PAGES:
        raise ToolError(f"unknown page {page!r} (want {', '.join(PAGES)})")
    if not 1 <= rest_polls <= route_trim.RUN_LIMIT:
        raise ToolError(f"--rest-polls {rest_polls}: want 1..{route_trim.RUN_LIMIT}")
    prefix, source_polls = split_prefix(text)
    if not prefix.has_wait and not force:
        raise ToolError(
            "the source has no '# wait: mark1:' line right at mark 1: the replay would not wait for the profile to load (--force to write it anyway)"
        )
    hops = PAGES[page]
    out: list[str] = []
    steps: list[grammar.NavStep] = []
    position = prefix.polls
    for number, hop in enumerate(hops):
        polls = hop_polls(hop)
        step = grammar.NavStep(
            at=position,
            to=position + polls,
            menu=hop.menu,
            select_kind="name",
            select_name=hop.label.encode("ascii"),
            activate=True,
        )
        steps.append(step)
        out.append(step.format())
        out += hop_runs(hop)
        settle = SETTLE_POLLS if number < len(hops) - 1 else rest_polls
        out.append(str(settle))
        position += polls + settle
    out.append(f"# polls: {position}")
    body = prefix.eol.join(out) + prefix.eol
    return Generated(prefix.text + body, source_polls, prefix.polls, position, tuple(steps))


def check_text(
    text: str, menus: Mapping[str, grammar.Menu] | None, require_wait: bool = True
) -> list[str]:
    """The invariants of a generated route, as one line per violation (empty = fine). `require_wait` False waives the mark 1 wait line."""
    try:
        route = route_trim.parse_route(text)
    except route_trim.RouteError as error:
        return [f"not a valid record (trailer, marks, runs): {error}"]
    problems: list[str] = []
    marks = route.marks()
    if not marks:
        return ["no mark 1"]
    waits: list[int] = []
    for raw in text.split("\n"):
        line = raw.rstrip("\r")
        if not line.startswith("# wait:"):
            continue
        try:
            waits.append(route_wait.parse_wait(line[7:].strip(), event=True).mark)
        except route_wait.WaitError as error:
            problems.append(f"wait line {line[:60]!r}: {error}")
    if require_wait and 1 not in waits:
        problems.append("no '# wait: mark1:' line")
    if len(set(waits)) != len(waits) or any(mark > len(marks) for mark in waits):
        problems.append(f"wait lines for marks {waits} do not fit {len(marks)} mark(s)")
    problems += grammar.validate_route(text, menus)
    found = grammar.scan_record(text)
    after = [(number, step) for number, step in found.steps if step.at >= marks[0]]
    if not after:
        problems.append("no nav step after mark 1")
    elif [(step.menu, step.select_name.decode("latin-1")) for _, step in after] not in [
        [(hop.menu, hop.label) for hop in hops] for hops in PAGES.values()
    ]:
        problems.append(
            "the nav steps after mark 1 are not one of the known pages: "
            + ", ".join(f"{step.menu}/{step.select_text}" for _, step in after)
        )
    if any(
        step.at < marks[0] for _, step in found.steps if step.menu in (MAIN_MENU, SETTINGS_MENU)
    ):
        problems.append("a main/settings menu step lies before mark 1")
    body = [line.rstrip("\r") for line in text.split("\n") if line.strip()]
    for number, step in after:
        problems += check_fallback(text.split("\n"), number, step)
    last_run = [line for line in body if not line.startswith("#")][-1:]
    if not last_run or not is_rest(last_run[0]):
        problems.append("the route does not end with a rest run")
    elif after and route.polls - after[-1][1].to < 1:
        problems.append("no rest poll after the last nav step")
    return problems


def check_fallback(lines: list[str], number: int, step: grammar.NavStep) -> list[str]:
    """The recorded presses right after a nav line cover exactly [at, to) and end with the A press."""
    covered = 0
    last = ""
    index = number  # lines are 1 based, so this is the line after the nav line
    while covered < step.to - step.at and index < len(lines):
        line = lines[index].rstrip("\r")
        if not line.strip() or line.startswith("#"):
            break
        covered += int(line.split()[0])
        last = line
        index += 1
    if covered != step.to - step.at:
        return [
            f"line {number}: the open loop presses cover {covered} polls, the step spans {step.to - step.at}"
        ]
    if not any(token.startswith("A=") for token in last.split()[1:]):
        return [f"line {number}: the open loop presses do not end with A"]
    return []


def waits_text(source_waits: Path | None, out_name: str) -> str:
    """The `.waits` text: a comment and the mark 1 specs of the source's `.waits` (nothing for marks 2 and up)."""
    specs: list[str] = []
    if source_waits is not None and source_waits.is_file():
        for raw in source_waits.read_text(encoding="utf-8", errors="replace").splitlines():
            if WAITS_MARK1.match(raw):
                specs.append(raw)
    header = f"# --route-wait specs for {out_name}, one per line (docs/input-replay.md, Route replay). Edit freely."
    return "\n".join([header, *specs]) + "\n"


def default_out(route: Path) -> Path:
    return route.with_name(route.stem + ".settings" + route.suffix)


def load_menus(path: Path) -> dict[str, grammar.Menu]:
    if not path.is_file():
        raise ToolError(f"menu table {path} does not exist (give --menus FILE)")
    try:
        menus = grammar.load_menu_table(path)
    except grammar.NavError as error:
        raise ToolError(f"{path}: {error}") from None
    missing = [name for name in (MAIN_MENU, SETTINGS_MENU) if name not in menus]
    if missing:
        raise ToolError(f"{path}: the menu table has no {', '.join(missing)}")
    return menus


def read_text(path: Path) -> str:
    try:
        return path.read_bytes().decode("utf-8", errors="surrogateescape")
    except OSError as error:
        raise ToolError(f"cannot read {path}: {error}") from None


def describe(result: Generated, source: Path, out: Path, waits: Path) -> str:
    lines = [
        f"settings_route: {source} -> {out}",
        f"  source polls {result.source_polls}, kept polls {result.kept_polls}, new polls {result.new_polls}",
    ]
    for step in result.steps:
        lines.append(f"  nav at={step.at} to={step.to} menu={step.menu} select={step.select_text}")
    lines.append(f"  ends with {result.new_polls - result.steps[-1].to} rest polls")
    lines.append(f"  waits file {waits}")
    return "\n".join(lines)


def command_generate(args: argparse.Namespace) -> int:
    source: Path = args.route
    menus = load_menus(args.menus)
    result = generate(read_text(source), args.page, args.rest_polls, args.force)
    out: Path = args.out or default_out(source)
    waits = out.with_suffix(".waits")
    if out.resolve() == source.resolve():
        raise ToolError("--out is the source route, the source is never overwritten")
    for target in (out, waits):
        if target.exists() and not args.force:
            raise ToolError(f"{target} exists (use --force)")
    problems = check_text(result.text, menus, require_wait=not args.force)
    if problems:
        raise ToolError("the generated route fails its own check: " + "; ".join(problems))
    source_waits = args.waits or source.with_suffix(".waits")
    out.write_bytes(result.text.encode("utf-8", errors="surrogateescape"))
    waits.write_text(waits_text(source_waits, out.name), encoding="utf-8")
    print(describe(result, source, out, waits))
    return 0


def command_check(args: argparse.Namespace) -> int:
    menus = load_menus(args.menus)
    problems = check_text(read_text(args.route), menus)
    for problem in problems:
        print(f"settings_route: {problem}", file=sys.stderr)
    print(f"{args.route}: {len(problems)} problem(s)")
    return 2 if problems else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Short route ending on the Settings > Controls page (T1709), from a recorded route.",
    )
    parser.add_argument(
        "route", type=Path, help="the recorded route (the route to verify with --check)"
    )
    parser.add_argument(
        "--out", type=Path, help="output route (default ROUTE.settings.txt next to the source)"
    )
    parser.add_argument(
        "--page",
        choices=sorted(PAGES),
        default="controls",
        help="the page to stop on (default controls)",
    )
    parser.add_argument(
        "--rest-polls",
        type=int,
        default=DEFAULT_REST_POLLS,
        help=f"rest polls after the last step (default {DEFAULT_REST_POLLS})",
    )
    parser.add_argument(
        "--menus",
        type=Path,
        default=DEFAULT_MENUS,
        help="menu table (default tools/data/menu_nav.txt)",
    )
    parser.add_argument(
        "--waits", type=Path, help="source wait specs (default the sibling ROUTE.waits)"
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="overwrite existing outputs, accept a source with no '# wait: mark1:' line",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="only verify the invariants of an already generated ROUTE",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return command_check(args) if args.check else command_generate(args)
    except ToolError as error:
        print(f"settings_route: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
