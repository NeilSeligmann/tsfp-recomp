# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1636: remove the owner's dead time from a recorded route without desyncing the loads.

  python -m tools.route_trim tmp/owner-profiles/routes/route-20261008-191017.txt --dry-run
  python -m tools.route_trim ROUTE [--out FILE] [--margin-polls 60] [--min-gap-polls 90]
                                   [--pre-mark keep|guarded|all] [--lead-in keep|trim]

A route (docs/input-replay.md, T1074/T1616) is a poll indexed pad script: `<frames> TOKEN ...` lines, `# mark: at=N` lines and a
`# polls: N` trailer. The replay clock is the title's XInputGetState poll index, never wall time. Cutting polls therefore moves
every later input EARLIER relative to the guest, which is exactly what a slow load must not suffer. This tool only cuts polls
that are provably (or, by the owner's mark semantics, by construction) not a guest wait.

Idle = a maximal run of polls in which no button is held (digital, analog above --analog-quiet) and both sticks stay inside the
XInput dead zone (7849 left, 8689 right, or --stick-quiet N for both). Each idle region is split at the marks inside it:

  post-mark   mark K -> next input / end of the record. The owner placed the mark when the screen had finished, then reached for the
              pad: reaction time. Trimmed down to --margin-polls. Safe because the replay's wait for the mark still stalls first and
              the polls BEFORE the mark (the load) are kept.
  pre-mark    last input -> mark K. Load time PLUS the chord gesture (hold >= 0.5 s). Trimmed only when mark K has a settle guard
              (a `# wait:` / `.waits` / --waits spec with a field other than min, min-ms, max, timeout: a sentinel or an event) or
              the record's `# mark-info:` proves the HDD was quiet (`hdd-last-io-ago-ms`), and then only the proven quiet part.
              `--pre-mark all` trims every pre-mark gap (the owner accepts the risk), `keep` none.
  between     mark K -> mark J inside one idle region: limited by the pre-mark rule of J.
  lead-in     before the first input of a region that starts at poll 0 and has no mark: guest boot, kept unless --lead-in trim.
  interior    idle between two inputs with no mark: could be a load or the owner hesitating, always KEPT (reported).

T1770 nav aware mode (default `auto`: on when the route has `# nav:` steps). A nav step replaces the recorded presses of [at, to) by
a closed loop that waits for its menu page to be on screen, input-ready (state 3) and then presses, so the idle polls NEXT TO a nav
step are redundant: whatever the guest was doing there, the step waits for the page again. Therefore, only for a step whose menu
has a page builder and a `ready=` condition in the menu table (a real gate, `--menus`):

  nav-pre     idle (post-mark, or between two inputs with no mark) that ends exactly at a nav step's `at`: cut down to
              --nav-margin-polls (6), whatever --margin-polls and --min-gap-polls say (gaps below --nav-min-gap-polls stay).
  nav-post    pre-mark idle that starts at a nav step's end and has a guarded mark, when the next input after the mark is again a gated
              nav step or the record ends (an open-loop input after the mark relies on the mark's guard alone, so its pre-mark idle stays
              at the T1636 rule): same, the mark's own wait holds.
  nav-tail    post-mark idle at the END of the record after a guarded mark: handover to the live pad sooner, same margin.

A genuine guest load next to a nav step is NOT cut by this: the step (or the mark's wait) simply waits as long as before, the
saving is only the recorded idle that the replay would have waited out ON TOP of the load. The lead-in stays kept unless
`--lead-in trim` (or `nav`: cut it only before a gated nav step). `--tune-waits` also shortens the record's `# wait:` file-idle
(route_events.tune_record_text, evidence from the recording's `.events`), see docs/t1636-route-trim.md "T1770".

Cut polls are removed whole, marks keep their ordinal (nothing is dropped or merged, so `# wait:` / `# mark-info:` lines, which are
keyed by mark ordinal, stay valid), `# mark: at=N` and `# polls:` are rewritten, header lines are copied verbatim (the flag identity,
XBE hash and budgets do not depend on the poll count). The original file is never written.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from tools import route_events
from tools.play import route_nav, route_wait

RUN_LIMIT = 1_000_000  # xinput_source.c: frame count 1..1000000 per line
MAX_MARKS = 16
DIGITAL = {"UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LTHUMB", "RTHUMB"}
ANALOG = {"A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT"}
STICKS = {"LX": 7849, "LY": 7849, "RX": 8689, "RY": 8689}  # XINPUT_GAMEPAD_*_THUMB_DEADZONE
ANALOG_QUIET = 30  # XINPUT_GAMEPAD_TRIGGER_THRESHOLD
NON_GUARD_FIELDS = {"min", "min-ms", "max", "timeout"}
DEFAULT_POLL_RATE = 60.0
# Conservative polls per second used to turn a quiet period in ms into polls (the measured rate is 50..60, docs/t1636-route-trim.md)
EVIDENCE_POLL_RATE = 30.0
# A cut gap may only hold stick noise: the spread of every axis over the whole gap must stay below this (a deliberate move
# inside the dead zone, for example a slow cursor nudge, is a spread of thousands)
STICK_DRIFT = 2500
# T1770: polls kept next to a nav step (its own readiness wait replaces the idle) and the shortest gap worth cutting there
NAV_MARGIN = 6
NAV_MIN_GAP = 12
DEFAULT_MENUS = Path("tools/data/menu_nav.txt")


class RouteError(ValueError):
    """The file is not a complete route record."""


@dataclass
class Item:
    kind: str  # run mark text
    frames: int = 0
    state: str = ""  # run: the token text after the frame count, verbatim
    at: int = 0  # mark
    text: str = ""  # text: a comment line, verbatim


@dataclass
class Route:
    items: list[Item] = field(default_factory=list)
    polls: int = 0  # the `# polls:` trailer
    trailer_line: str = ""

    def marks(self) -> list[int]:
        return [item.at for item in self.items if item.kind == "mark"]

    def runs(self) -> list[Item]:
        return [item for item in self.items if item.kind == "run"]


def parse_route(text: str) -> Route:
    """Parse a v1 record. Refuses what the host refuses: bad run, missing/duplicate/early trailer, mark not at the poll position."""
    route = Route()
    position = 0
    seen_trailer = False
    for number, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip("\r")
        if number == 1 and not line.startswith("# tsfp-input v"):
            raise RouteError("line 1: not an input record (want '# tsfp-input v1')")
        stripped = line.strip()
        if not stripped:
            continue
        if seen_trailer and not stripped.startswith("#"):
            raise RouteError(f"line {number}: body after the '# polls:' trailer")
        if stripped.startswith("# polls:"):
            if seen_trailer:
                raise RouteError(f"line {number}: second '# polls:' trailer")
            match = re.fullmatch(r"# polls:\s*(\d+)", stripped)
            if not match:
                raise RouteError(f"line {number}: malformed trailer {stripped!r}")
            route.polls = int(match.group(1))
            route.trailer_line = stripped
            seen_trailer = True
            continue
        if stripped.startswith("# mark:"):
            match = re.fullmatch(r"# mark:\s*at=(\d+)", stripped)
            if not match:
                raise RouteError(f"line {number}: malformed mark {stripped!r}")
            at = int(match.group(1))
            if at != position:
                raise RouteError(f"line {number}: mark at={at} but the poll position is {position}")
            route.items.append(Item("mark", at=at))
            continue
        if stripped.startswith("#"):
            route.items.append(Item("text", text=line))
            continue
        parts = stripped.split()
        if not re.fullmatch(r"[0-9]+", parts[0]) or not 1 <= int(parts[0]) <= RUN_LIMIT:
            raise RouteError(f"line {number}: frame count not 1..{RUN_LIMIT}: {parts[0]!r}")
        frames = int(parts[0])
        route.items.append(Item("run", frames=frames, state=" ".join(parts[1:])))
        position += frames
    if not seen_trailer:
        raise RouteError("no '# polls:' trailer (an incomplete record)")
    if route.polls != position:
        raise RouteError(f"trailer says {route.polls} polls but the body has {position}")
    if len(route.marks()) > MAX_MARKS:
        raise RouteError(f"{len(route.marks())} marks, a replay accepts at most {MAX_MARKS}")
    return route


def read_header_values(path: Path) -> dict[str, str]:
    """The leading `# key: value` lines of a route (xbe-sha256, flags-sha256, flags, budgets)."""
    values: dict[str, str] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines()):
        if not raw.startswith("#"):
            break
        key, separator, value = raw[2:].partition(":")
        if number and separator:
            values.setdefault(key, value.strip())
    return values


# ---- quiet / active ----


def parse_state(state: str) -> dict[str, int] | None:
    """Token text to {name: value}; None when a token is unknown (treated as active)."""
    values: dict[str, int] = {}
    for token in state.split():
        name, separator, value = token.partition("=")
        if not separator:
            if name not in DIGITAL:
                return None
            values[name] = 1
            continue
        if name not in ANALOG and name not in STICKS:
            return None
        try:
            values[name] = int(value)
        except ValueError:
            return None
    return values


def is_quiet(state: str, stick_quiet: int | None = None, analog_quiet: int = ANALOG_QUIET) -> bool:
    values = parse_state(state)
    if values is None:
        return False
    for name, value in values.items():
        if name in DIGITAL:
            return False
        if name in ANALOG and value > analog_quiet:
            return False
        if name in STICKS and abs(value) > (STICKS[name] if stick_quiet is None else stick_quiet):
            return False
    return True


# ---- waits and mark-info ----


def is_guard(spec: str) -> bool:
    """A wait is a settle guard when it has a field other than min/min-ms/max/timeout (a sentinel or an event)."""
    _, _, fields = spec.partition(":")
    keys = {item.strip().split("=", 1)[0].strip() for item in fields.split(",") if item.strip()}
    return bool(keys - NON_GUARD_FIELDS)


def parse_waits(lines: list[str]) -> dict[int, str]:
    """`markK:fields` specs (one per line, `#` comments) to {K: spec}."""
    waits: dict[int, str] = {}
    for raw in lines:
        line = raw.strip()
        if line.startswith("# wait:"):
            line = line[len("# wait:") :].strip()
        elif not line or line.startswith("#"):
            continue
        match = re.match(r"mark(\d+):", line)
        if match:
            waits[int(match.group(1))] = line
    return waits


def route_waits(route: Route, waits_file: Path | None) -> dict[int, str]:
    """Waits of the record's own `# wait:` lines, which win over the sibling `.waits` file (T1633)."""
    waits: dict[int, str] = {}
    if waits_file is not None and waits_file.is_file():
        waits.update(
            parse_waits(waits_file.read_text(encoding="utf-8", errors="replace").splitlines())
        )
    waits.update(
        parse_waits(
            [
                item.text
                for item in route.items
                if item.kind == "text" and item.text.startswith("# wait:")
            ]
        )
    )
    return waits


def hdd_quiet_ms(route: Route) -> dict[int, int]:
    """{mark K: hdd-last-io-ago-ms} from the record's `# mark-info:` lines (T1633 hosts). The all-files `last-io-ago-ms` is NOT used
    (it counts disc streaming, which is almost never quiet in a menu, T1633 note)."""
    quiet: dict[int, int] = {}
    for item in route.items:
        if item.kind != "text":
            continue
        match = re.match(r"# mark-info:\s*mark(\d+)\s+(.*)", item.text.strip())
        if not match:
            continue
        found = re.search(r"(?:^|\s)hdd-last-io-ago-ms=(\d+)", match.group(2))
        if found:
            quiet[int(match.group(1))] = int(found.group(1))
    return quiet


def mark_rates(hotkey_dir: Path | None, marks: list[int]) -> list[float | None]:
    """Polls per second of each segment between marks (segment i ends at mark i+1), from `hotkey.N` files (`N mark poll=P ms=M`)."""
    if hotkey_dir is None or not hotkey_dir.is_dir():
        return []
    stamps: dict[int, int] = {}
    for path in hotkey_dir.glob("hotkey.*"):
        match = re.fullmatch(
            r"\d+\s+mark\s+poll=(\d+)\s+ms=(\d+)", path.read_text(errors="replace").strip()
        )
        if match:
            stamps[int(match.group(1))] = int(match.group(2))
    rates: list[float | None] = []
    previous: tuple[int, int] | None = None
    for at in marks:
        current = (at, stamps[at]) if at in stamps else None
        if previous and current and current[1] > previous[1] and current[0] > previous[0]:
            rates.append((current[0] - previous[0]) * 1000.0 / (current[1] - previous[1]))
        else:
            rates.append(None)
        previous = current if current else previous
    return rates


# ---- planning ----


@dataclass
class Gap:
    start: int  # old poll range [start, end)
    end: int
    kind: str  # post-mark pre-mark between lead-in interior tail
    marks: tuple[int, ...]  # mark ordinals (1 based) that bound it
    cut: tuple[int, int] | None = None  # old polls removed
    reason: str = ""

    @property
    def length(self) -> int:
        return self.end - self.start

    @property
    def removed(self) -> int:
        return 0 if self.cut is None else self.cut[1] - self.cut[0]


@dataclass
class Options:
    margin: int = 60
    min_gap: int = 90
    pre_mark: str = "guarded"  # keep guarded all
    lead_in: str = "keep"  # keep trim
    stick_quiet: int | None = None
    analog_quiet: int = ANALOG_QUIET
    stick_drift: int = STICK_DRIFT
    # T1770 nav aware mode
    nav_aware: bool = False
    nav_margin: int = NAV_MARGIN
    nav_min_gap: int = NAV_MIN_GAP
    gated_menus: frozenset[str] | None = (
        None  # None: every menu counts as gated (tests); a set: only these menu ids
    )
    # menus a nav step presses the d-pad on (not dpad=0): only for those the idle BEFORE the step is redundant. A dpad=0 editor menu
    # follows the recorded stick/pointer motion, whose travel and easing take game time, so the idle before it stays.
    pressed_menus: frozenset[str] | None = None


def quiet_regions(route: Route, options: Options) -> list[tuple[int, int]]:
    regions: list[tuple[int, int]] = []
    position = 0
    start: int | None = None
    for item in route.runs():
        if is_quiet(item.state, options.stick_quiet, options.analog_quiet):
            if start is None:
                start = position
        elif start is not None:
            regions.append((start, position))
            start = None
        position += item.frames
    if start is not None:
        regions.append((start, position))
    return regions


def pre_mark_allowance(
    gap_length: int, mark: int, options: Options, waits: dict[int, str], quiet_ms: dict[int, int]
) -> tuple[int, str]:
    """(polls that may be cut from a pre-mark gap of this length, reason)."""
    full = max(0, gap_length - options.margin)
    if options.pre_mark == "keep":
        return 0, "pre-mark idle kept (--pre-mark keep)"
    if options.pre_mark == "all":
        return full, "pre-mark idle cut by --pre-mark all (load time not excluded, owner's risk)"
    spec = waits.get(mark)
    if spec is not None and is_guard(spec):
        return full, f"mark {mark} has a settle guard ({spec}), the replay stalls until it holds"
    if mark in quiet_ms:
        proven = int(quiet_ms[mark] * EVIDENCE_POLL_RATE / 1000.0)
        allowed = min(full, max(0, proven - options.margin))
        return (
            allowed,
            f"mark-info: HDD quiet {quiet_ms[mark]} ms before mark {mark} (~{proven} polls at {EVIDENCE_POLL_RATE:g}/s)",
        )
    shape = f"wait is {spec}" if spec else "no wait"
    return 0, f"KEPT: unguarded ({shape}), the gap may hold the load that mark {mark} waited for"


def stick_spread(route: Route, start: int, end: int) -> int:
    """Largest max-min of any stick axis over the old polls [start, end) (an axis a run does not name is 0)."""
    states: list[dict[str, int]] = []
    position = 0
    for item in route.runs():
        if position + item.frames > start and position < end:
            states.append(parse_state(item.state) or {})
        position += item.frames
    spread = 0
    for name in STICKS:
        values = [state.get(name, 0) for state in states]
        if values:
            spread = max(spread, max(values) - min(values))
    return spread


def plan(route: Route, options: Options, waits: dict[int, str] | None = None) -> list[Gap]:
    """The idle gaps of the route, each with the cut (if any) and the reason."""
    waits = waits or {}
    quiet_ms = hdd_quiet_ms(route)
    marks = route.marks()
    navs = nav_ranges(route)
    steps = nav_steps(route) if options.nav_aware else []
    gated = [(step.at, step.to) for step in steps if is_gated(step, options)]
    pressed = [(step.at, step.to) for step in steps if is_pressed(step, options)]

    def ends_at_gated(position: int) -> bool:
        """An idle gap ending here is followed by a gated nav step: the position is in [at, to) of one."""
        return any(at <= position < to or at == position == to for at, to in pressed)

    def starts_at_gated(position: int) -> bool:
        """An idle gap starting here follows a gated nav step: the position is in (at, to] of one (its presses are replaced)."""
        return any(at < position <= to for at, to in gated)

    gaps: list[Gap] = []
    for start, end in quiet_regions(route, options):
        inside = [(index + 1, at) for index, at in enumerate(marks) if start <= at <= end]
        cuts_at = [(0, start)] + [(k, at) for k, at in inside] + [(0, end)]
        # anchors: (mark ordinal or 0, poll). First anchor is the region start, last the region end.
        for index in range(len(cuts_at) - 1):
            (left_mark, left), (right_mark, right) = cuts_at[index], cuts_at[index + 1]
            if right <= left:
                continue
            length = right - left
            if not inside:
                kind = "lead-in" if start == 0 and end < route.polls else "interior"
                if end == route.polls and start != 0:
                    kind = "tail"
                gap = Gap(left, right, kind, ())
                if kind in ("lead-in", "interior") and ends_at_gated(right):
                    # T1770 nav-pre: the nav step that follows waits for its page again
                    if kind == "lead-in" and options.lead_in != "nav":
                        gap.reason = "KEPT: before the first input, nothing guards the guest boot (--lead-in nav would cut it before a gated nav step)"
                    elif length >= options.nav_min_gap and length - options.nav_margin > 0:
                        gap.cut = (left, right - options.nav_margin)
                        gap.reason = f"nav-pre: the {kind} idle ends at a gated nav step, whose own wait for the page replaces it (kept {options.nav_margin})"
                    else:
                        gap.reason = f"KEPT: {kind} idle of {length} polls is not above the nav margin {options.nav_margin} / nav min-gap {options.nav_min_gap}"
                elif kind == "lead-in" and options.lead_in == "trim" and length >= options.min_gap:
                    gap.cut = (left, right - options.margin)
                    gap.reason = (
                        "lead-in cut by --lead-in trim (guest boot not excluded, owner's risk)"
                    )
                elif gap.reason:
                    pass
                elif kind == "lead-in":
                    gap.reason = "KEPT: before the first input, nothing guards the guest boot"
                elif kind == "tail":
                    gap.reason = "KEPT: idle at the end of the record with no mark (may be the load of the last input)"
                else:
                    gap.reason = "KEPT: idle between two inputs with no mark (a load or the owner hesitating, undecidable)"
                gaps.append(gap)
                continue
            if left_mark and right_mark:
                gap = Gap(left, right, "between", (left_mark, right_mark))
                allowed, why = pre_mark_allowance(length, right_mark, options, waits, quiet_ms)
                cut_length = min(allowed, max(0, length - options.margin))
                gap.reason = f"between marks {left_mark} and {right_mark}: {why}"
                if cut_length > 0 and length >= options.min_gap:
                    gap.cut = (
                        right - cut_length,
                        right,
                    )  # keep the polls right after the left mark
            elif right_mark:
                gap = Gap(left, right, "pre-mark", (right_mark,))
                # T1770 nav-post: the polls right after a nav step are not the owner's, the guarded mark wait holds
                # nav-post also needs everything AFTER the mark to be guarded again: the region ends at a gated nav step or at the end of
                # the record. An open-loop input after the mark (the owner's stick motion at mark 3) relies on the mark's guard alone, and
                # that guard may fire earlier than the screen takes input (MEASURED, docs/t1636-route-trim.md T1770), so its idle stays.
                nav_post = (
                    left_mark == 0
                    and starts_at_gated(left)
                    and (ends_at_gated(end) or end == route.polls)
                )
                limits = (
                    Options(**{**options.__dict__, "margin": options.nav_margin})
                    if nav_post
                    else options
                )
                allowed, gap.reason = pre_mark_allowance(
                    length, right_mark, limits, waits, quiet_ms
                )
                if nav_post and allowed > 0:
                    gap.reason = f"nav-post: {gap.reason}; the idle starts at a gated nav step's end (kept {options.nav_margin})"
                if allowed > 0 and length >= (options.nav_min_gap if nav_post else options.min_gap):
                    gap.cut = (right - allowed, right)  # keep the polls right after the last input
            else:
                gap = Gap(left, right, "post-mark", (left_mark,))
                gap.reason = f"owner reaction after mark {left_mark} (the screen had finished at the mark, its wait still stalls first)"
                margin, min_gap = options.margin, options.min_gap
                tail = right == route.polls  # `steps` below is empty unless nav aware
                spec = waits.get(left_mark)
                if ends_at_gated(right):
                    margin, min_gap = options.nav_margin, options.nav_min_gap
                    gap.reason += f"; nav-pre: a gated nav step follows, its page wait replaces the idle (kept {margin})"
                elif tail and steps and spec is not None and is_guard(spec):
                    margin, min_gap = options.nav_margin, options.nav_min_gap
                    gap.reason += f"; nav-tail: mark {left_mark} has a settle guard ({spec}), the hand over to the live pad need not wait (kept {margin})"
                if length - margin > 0 and length >= min_gap:
                    if tail:
                        gap.cut = (
                            left + margin,
                            right,
                        )  # end of the record: keep the polls next to the mark
                    else:
                        gap.cut = (
                            left,
                            right - margin,
                        )  # keep the polls right before the next input
            if gap.cut is not None and gap.cut[1] <= gap.cut[0]:
                gap.cut = None
            if gap.cut is not None and stick_spread(route, left, right) > options.stick_drift:
                gap.reason = f"KEPT: a stick moves {stick_spread(route, left, right)} inside the dead zone here (limit {options.stick_drift}), maybe deliberate"
                gap.cut = None
            if gap.cut is not None and navs:
                gap.cut, gap.reason = clip_to_navs(gap.cut, navs, gap.reason)
            if gap.cut is None and not gap.reason.startswith("KEPT"):
                gap.reason = f"KEPT: {gap.reason} (gap {length} polls is not above margin {options.margin} / min-gap {options.min_gap})"
            gaps.append(gap)
    return gaps


# ---- nav steps (T1640) ----


def nav_ranges(route: Route) -> list[tuple[int, int]]:
    """(at, to) of every well formed `# nav:` line: the polls the replay itself drives (a closed loop on the menu state)."""
    ranges: list[tuple[int, int]] = []
    for item in route.items:
        if item.kind == "text" and item.text.startswith(route_nav.NAV_PREFIX):
            try:
                step = route_nav.parse_nav_line(item.text)
            except route_nav.NavError:
                continue
            ranges.append((step.at, step.to))
    return ranges


def nav_steps(route: Route) -> list[route_nav.NavStep]:
    steps: list[route_nav.NavStep] = []
    for item in route.items:
        if item.kind == "text" and item.text.startswith(route_nav.NAV_PREFIX):
            try:
                steps.append(route_nav.parse_nav_line(item.text))
            except route_nav.NavError:
                continue
    return steps


def pressed_menu_ids(menus: route_nav.MenuTable) -> frozenset[str]:
    """Gated menus the nav step drives with the d-pad (table key dpad=0 excluded: stick/pointer driven editor menus)."""
    return frozenset(
        name
        for name, menu in menus.items()
        if isinstance(menu, route_nav.Menu)
        and menu.page_builder is not None
        and menu.ready
        and menu.dpad
    )


def gated_menu_ids(menus: route_nav.MenuTable) -> frozenset[str]:
    """Menus whose nav step really waits for the screen: a page builder (the page must be on screen) AND a ready condition (state 3,
    no child page), the rule that decides when the host presses (docs/t1640-nav-state-machine.md)."""
    return frozenset(
        name
        for name, menu in menus.items()
        if isinstance(menu, route_nav.Menu) and menu.page_builder is not None and menu.ready
    )


def is_gated(step: route_nav.NavStep, options: Options) -> bool:
    return options.gated_menus is None or step.menu in options.gated_menus


def is_pressed(step: route_nav.NavStep, options: Options) -> bool:
    return is_gated(step, options) and (
        options.pressed_menus is None or step.menu in options.pressed_menus
    )


def clip_to_navs(
    cut: tuple[int, int], navs: list[tuple[int, int]], reason: str
) -> tuple[tuple[int, int] | None, str]:
    """A cut never enters a nav range [at, to] (the replay waits for the menu there, the recorded presses are replaced): the
    parts of the cut outside every range are what is left, the longest one is kept (a gap holds one cut). A range that is a
    point (at == to) guards that poll the same way. Cut ends AT `at` or START at `to` are fine."""
    pieces = [cut]
    for at, to in navs:
        shrunk: list[tuple[int, int]] = []
        for start, end in pieces:
            if end <= at or start >= to:
                shrunk.append((start, end))
                continue
            if start < at:
                shrunk.append((start, at))
            if end > to:
                shrunk.append((to, end))
        pieces = shrunk
    if pieces == [cut]:
        return cut, reason
    if not pieces:
        return (
            None,
            "KEPT: the whole cut lies inside a nav step (the replay drives those polls from the menu state)",
        )
    best = max(pieces, key=lambda piece: piece[1] - piece[0])
    return best, f"{reason} (clipped to {best[0]}..{best[1]} so no nav step is cut into)"


def remap_nav_text(text: str, cuts: list[tuple[int, int]]) -> str:
    """A `# nav:` comment with `at=` and `to=` moved by the same function as the marks; every other text unchanged."""
    if not text.startswith(route_nav.NAV_PREFIX):
        return text
    parts = re.split(r"(\s+)", text)
    done: set[str] = set()
    for index, part in enumerate(parts):
        key, separator, value = part.partition("=")
        if separator and key in ("at", "to") and key not in done and value.isdigit():
            done.add(key)
            parts[index] = f"{key}={new_position(int(value), cuts)}"
    return "".join(parts)


# ---- applying ----


def cut_ranges(gaps: list[Gap]) -> list[tuple[int, int]]:
    return sorted(gap.cut for gap in gaps if gap.cut is not None)


def new_position(position: int, cuts: list[tuple[int, int]]) -> int:
    """Old poll index (or boundary) to the new one: minus the cut polls before it."""
    removed = 0
    for start, end in cuts:
        removed += max(0, min(end, position) - start)
    return position - removed


def apply_cuts(route: Route, cuts: list[tuple[int, int]]) -> str:
    out: list[str] = []
    position = 0
    pending: tuple[str, int] | None = (
        None  # (state, frames) run waiting to merge with the next piece
    )

    def flush() -> None:
        nonlocal pending
        if pending is None:
            return
        state, frames = pending
        while frames > 0:
            piece = min(frames, RUN_LIMIT)
            out.append(f"{piece} {state}".rstrip())
            frames -= piece
        pending = None

    for item in route.items:
        if item.kind == "text":
            flush()
            out.append(remap_nav_text(item.text, cuts))
        elif item.kind == "mark":
            flush()
            out.append(f"# mark: at={new_position(item.at, cuts)}")
        else:
            kept: list[tuple[int, int]] = []
            cursor = position
            end = position + item.frames
            for start_cut, end_cut in cuts:
                if end_cut <= cursor or start_cut >= end:
                    continue
                if start_cut > cursor:
                    kept.append((cursor, start_cut))
                cursor = max(cursor, end_cut)
            if cursor < end:
                kept.append((cursor, end))
            for piece_start, piece_end in kept:
                frames = piece_end - piece_start
                if pending is not None and pending[0] == item.state:
                    pending = (item.state, pending[1] + frames)
                else:
                    flush()
                    pending = (item.state, frames)
            position = end
    flush()
    total = route.polls - sum(end - start for start, end in cuts)
    out.append(f"# polls: {total}")
    return "\n".join(out) + "\n"


def expand(route: Route) -> list[str]:
    states: list[str] = []
    for item in route.runs():
        states.extend([item.state] * item.frames)
    return states


def verify(original: Route, trimmed: Route, cuts: list[tuple[int, int]]) -> list[str]:
    """Problems found comparing the trimmed route with the original and the cut list. Empty = sound."""
    problems: list[str] = []
    old_states = expand(original)
    removed: set[int] = set()
    for start, end in cuts:
        removed.update(range(start, end))
    expected = [state for index, state in enumerate(old_states) if index not in removed]
    if expand(trimmed) != expected:
        problems.append("the trimmed poll states are not the original states minus the cut polls")
    if trimmed.polls != len(expected):
        problems.append(f"trailer {trimmed.polls} != {len(expected)} polls")
    if trimmed.marks() != [new_position(at, cuts) for at in original.marks()]:
        problems.append("marks were not remapped consistently")
    if len(trimmed.marks()) != len(original.marks()):
        problems.append("the number of marks changed")
    old_text = [remap_nav_text(item.text, cuts) for item in original.items if item.kind == "text"]
    new_text = [item.text for item in trimmed.items if item.kind == "text"]
    if old_text != new_text:
        problems.append("comment lines (header, # wait:, # mark-info:) changed")
    for at, to in nav_ranges(original):
        if any(start < to and end > at for start, end in cuts):
            problems.append(f"a cut enters the nav range {at}..{to}")
    if nav_ranges(trimmed) != [
        (new_position(at, cuts), new_position(to, cuts)) for at, to in nav_ranges(original)
    ]:
        problems.append("nav ranges were not remapped consistently")
    quiet_cut = [index for index in removed if not is_quiet(old_states[index])]
    if quiet_cut:
        problems.append(
            f"{len(quiet_cut)} cut polls were not idle (first old poll {min(quiet_cut)})"
        )
    return problems


# ---- reporting ----


def rate_at(position: int, marks: list[int], rates: list[float | None], default: float) -> float:
    """Poll rate of the segment holding `position` (segment i ends at mark i+1); the nearest known one, else the default."""
    known = [(index, rate) for index, rate in enumerate(rates) if rate]
    if not known:
        return default
    segment = sum(1 for at in marks if at < position)
    best = min(known, key=lambda pair: abs(pair[0] - segment))
    return best[1]


def describe(gap: Gap) -> str:
    where = {
        "post-mark": f"after mark {gap.marks[0]}" if gap.marks else "",
        "pre-mark": f"before mark {gap.marks[0]}" if gap.marks else "",
        "between": f"marks {gap.marks[0]}..{gap.marks[1]}" if len(gap.marks) == 2 else "",
    }.get(gap.kind, "")
    return f"{gap.kind} {where}".strip()


def table(
    gaps: list[Gap],
    marks: list[int],
    rates: list[float | None],
    rate_default: float,
    before: int,
    after: int,
    min_gap: int,
    show_all: bool,
) -> str:
    rows = [("gap polls", "length", "cut polls", "cut s", "what", "decision")]
    hidden = 0
    seconds_total = 0.0
    for gap in gaps:
        rate = rate_at((gap.start + gap.end) // 2, marks, rates, rate_default)
        seconds_total += gap.removed / rate
        if gap.length < min_gap and not show_all and gap.cut is None:
            hidden += 1
            continue
        cut = f"{gap.cut[0]}..{gap.cut[1]}" if gap.cut else "-"
        seconds = f"{gap.removed / rate:.1f}" if gap.cut else "-"
        rows.append(
            (f"{gap.start}..{gap.end}", str(gap.length), cut, seconds, describe(gap), gap.reason)
        )
    widths = [max(len(row[col]) for row in rows) for col in range(5)]
    lines = [
        "  ".join(row[col].ljust(widths[col]) for col in range(5)) + "  " + row[5] for row in rows
    ]
    if hidden:
        lines.append(
            f"({hidden} idle gaps shorter than --min-gap-polls {min_gap} and not cut are normal spacing between presses, kept, --all-gaps lists them)"
        )
    lines.append(
        f"total: {before} polls -> {after} polls, cut {before - after} polls, about {seconds_total:.1f} s "
        f"(each gap at the poll rate measured from the .hotkeys timing of its segment, else {rate_default:g} polls/s)"
    )
    return "\n".join(lines)


def default_out(route_path: Path) -> Path:
    return route_path.with_name(route_path.stem + ".trim" + route_path.suffix)


def copy_waits(waits_file: Path | None, route_path: Path, out: Path, force: bool) -> Path | None:
    """The `.waits` file of the trimmed route: same specs (they are keyed by mark ordinal), the route name in comments updated."""
    if waits_file is None or not waits_file.is_file():
        return None
    target = out.with_suffix(".waits")
    if target.exists() and not force:
        raise RouteError(f"{target} exists (use --force)")
    text = waits_file.read_text(encoding="utf-8", errors="replace")
    lines = [
        line.replace(route_path.name, out.name) if line.lstrip().startswith("#") else line
        for line in text.splitlines()
    ]
    target.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return target


def run(args: argparse.Namespace) -> int:
    route_path: Path = args.route
    try:
        text = route_path.read_text(encoding="utf-8", errors="replace")
        route = parse_route(text)
    except OSError as error:
        print(f"route_trim: cannot read {route_path}: {error}", file=sys.stderr)
        return 2
    except RouteError as error:
        print(f"route_trim: {route_path}: {error}", file=sys.stderr)
        return 2
    out = args.out or default_out(route_path)
    if out.resolve() == route_path.resolve():
        print(
            "route_trim: --out is the input file, the original is never overwritten",
            file=sys.stderr,
        )
        return 2
    if out.exists() and not args.force and not args.dry_run:
        print(
            f"route_trim: {out} exists (use --force to replace a previous trim, never the original)",
            file=sys.stderr,
        )
        return 2
    options = Options(
        margin=args.margin_polls,
        min_gap=args.min_gap_polls,
        pre_mark=args.pre_mark,
        lead_in=args.lead_in,
        stick_quiet=args.stick_quiet,
        analog_quiet=args.analog_quiet,
        stick_drift=args.stick_drift,
        nav_margin=args.nav_margin_polls,
        nav_min_gap=args.nav_min_gap_polls,
    )
    has_navs = bool(nav_ranges(route))
    options.nav_aware = args.nav_aware == "on" or (args.nav_aware == "auto" and has_navs)
    if options.nav_aware:
        try:
            menu_table = route_nav.load_menu_table(args.menus)
            options.gated_menus = gated_menu_ids(menu_table)
            options.pressed_menus = pressed_menu_ids(menu_table)
        except (OSError, route_nav.NavError) as error:
            print(f"route_trim: cannot read the menu table {args.menus}: {error}", file=sys.stderr)
            return 2
    tuned: list[route_events.TunedWait] = []
    tune_on = args.tune_waits == "on" or (args.tune_waits == "auto" and options.nav_aware)
    events_path = args.events if args.events else route_path.with_suffix(".events")
    if tune_on and events_path.is_file():
        info = route_wait.read_record(route_path)
        segments = route_events.build_segments(
            info.marks, route_events.parse_event_log(events_path), info.infos, {}
        )
        text, tuned = route_events.tune_record_text(
            text, segments, args.idle_cap_ms, args.idle_floor_ms
        )
        route = parse_route(text)
    elif tune_on and args.tune_waits == "on":
        print(f"route_trim: --tune-waits on needs the event log {events_path}", file=sys.stderr)
        return 2
    waits_file = args.waits if args.waits else route_path.with_suffix(".waits")
    waits = route_waits(route, waits_file)
    gaps = plan(route, options, waits)
    cuts = cut_ranges(gaps)
    trimmed_text = apply_cuts(route, cuts)
    trimmed = parse_route(trimmed_text)
    problems = verify(route, trimmed, cuts)
    if problems:
        print("route_trim: INTERNAL CHECK FAILED, nothing written:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 3
    marks = route.marks()
    rates = mark_rates(route_path.with_suffix(".hotkeys"), marks)
    print(
        f"route_trim T1636: {route_path} ({route.polls} polls, {len(marks)} marks), margin {options.margin}, min-gap {options.min_gap}, "
        f"pre-mark {options.pre_mark}, lead-in {options.lead_in}"
        + (
            f", nav aware (margin {options.nav_margin}, min-gap {options.nav_min_gap}, {len(options.gated_menus or ())} gated menus)"
            if options.nav_aware
            else ", nav aware off"
        )
    )
    for item in tuned:
        if item.changed:
            print(f"tuned wait: {item.note}")
            print(f"  was {item.old}")
            print(f"  now {item.new}")
    if waits:
        print(
            "waits: "
            + "; ".join(
                f"mark{k}: {v.partition(':')[2]} ({'guard' if is_guard(v) else 'time only'})"
                for k, v in sorted(waits.items())
            )
        )
    else:
        print("waits: none found (every pre-mark gap counts as unguarded)")
    print(
        table(
            gaps,
            marks,
            rates,
            args.poll_rate,
            route.polls,
            trimmed.polls,
            options.min_gap,
            args.all_gaps,
        )
    )
    print("new marks: " + ", ".join(f"{at} -> {new_position(at, cuts)}" for at in marks))
    if args.dry_run:
        print("dry run: nothing written")
        return 0
    out.write_text(trimmed_text, encoding="utf-8")
    print(f"wrote {out}")
    target = copy_waits(waits_file, route_path, out, args.force)
    if target is not None:
        print(
            f"wrote {target} (same specs: they are keyed by mark ordinal and min= is a stall length)"
        )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.route_trim",
        description="Remove the owner's idle polls (reaction and chord time) from a recorded route, keeping loads (T1636).",
    )
    parser.add_argument("route", type=Path, help="the recorded route (.txt); it is never modified")
    parser.add_argument(
        "--out",
        type=Path,
        help="output file (default ROUTE.trim.txt next to it); refused if it is the input",
    )
    parser.add_argument(
        "--margin-polls",
        type=int,
        default=60,
        help="idle polls kept next to an input or mark (default 60, about 1 s)",
    )
    parser.add_argument(
        "--min-gap-polls",
        type=int,
        default=90,
        help="idle gaps shorter than this are normal human spacing and never touched (default 90)",
    )
    parser.add_argument(
        "--pre-mark",
        choices=["keep", "guarded", "all"],
        default="guarded",
        help="idle before a mark: keep, cut only when a settle guard or mark-info proves it (default), or cut always",
    )
    parser.add_argument(
        "--lead-in",
        choices=["keep", "trim", "nav"],
        default="keep",
        help="idle before the first input (guest boot): keep (default), trim, or nav (cut it only before a gated nav step)",
    )
    parser.add_argument(
        "--stick-quiet",
        type=int,
        help="stick magnitude counted as at rest (default the XInput dead zones 7849/8689)",
    )
    parser.add_argument(
        "--analog-quiet",
        type=int,
        default=ANALOG_QUIET,
        help="analog button/trigger value counted as at rest (default 30)",
    )
    parser.add_argument(
        "--stick-drift",
        type=int,
        default=STICK_DRIFT,
        help="a gap whose stick axes spread more than this is kept as possibly deliberate (default 2500)",
    )
    parser.add_argument(
        "--nav-aware",
        choices=["auto", "on", "off"],
        default="auto",
        help="T1770: cut the idle next to a gated nav step down to --nav-margin-polls (auto = on when the route has '# nav:' lines)",
    )
    parser.add_argument(
        "--nav-margin-polls",
        type=int,
        default=NAV_MARGIN,
        help=f"idle polls kept next to a gated nav step (default {NAV_MARGIN})",
    )
    parser.add_argument(
        "--nav-min-gap-polls",
        type=int,
        default=NAV_MIN_GAP,
        help=f"shortest idle gap cut next to a gated nav step (default {NAV_MIN_GAP})",
    )
    parser.add_argument(
        "--menus",
        type=Path,
        default=DEFAULT_MENUS,
        help="menu table that says which nav menus gate on their page (default tools/data/menu_nav.txt)",
    )
    parser.add_argument(
        "--tune-waits",
        choices=["auto", "on", "off"],
        default="auto",
        help="T1770: shorten the file-idle of the record's '# wait:' lines from the recording's .events (auto = with nav aware, if the log exists)",
    )
    parser.add_argument(
        "--events",
        type=Path,
        help="event log of the RECORDING for --tune-waits (default ROUTE.events beside the route)",
    )
    parser.add_argument(
        "--idle-cap-ms",
        type=int,
        default=route_events.TUNE_MAX_IDLE_MS,
        help="--tune-waits: largest file-idle (default 500)",
    )
    parser.add_argument(
        "--idle-floor-ms",
        type=int,
        default=route_events.TUNE_MIN_IDLE_MS,
        help="--tune-waits: smallest file-idle (default 300)",
    )
    parser.add_argument("--all-gaps", action="store_true", help="list the short gaps too")
    parser.add_argument(
        "--waits",
        type=Path,
        help="wait specs file (default the sibling ROUTE.waits); the record's own '# wait:' lines win",
    )
    parser.add_argument(
        "--poll-rate",
        type=float,
        default=DEFAULT_POLL_RATE,
        help="polls per second used for the seconds column when no .hotkeys timing exists",
    )
    parser.add_argument("--dry-run", action="store_true", help="print the table, write nothing")
    parser.add_argument(
        "--force",
        action="store_true",
        help="replace an existing trimmed output (never the original)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if (
        args.margin_polls < 0
        or args.min_gap_polls < 0
        or args.poll_rate <= 0
        or args.nav_margin_polls < 0
        or args.nav_min_gap_polls < 0
        or args.idle_cap_ms < args.idle_floor_ms
        or args.idle_floor_ms < 0
    ):
        print(
            "route_trim: --margin-polls, --min-gap-polls, --nav-margin-polls and --nav-min-gap-polls must be >= 0, --poll-rate > 0 and 0 <= --idle-floor-ms <= --idle-cap-ms",
            file=sys.stderr,
        )
        return 2
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
