# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1640: nav steps for a recorded route (`# nav:` comment lines), converter included.

  python -m tools.route_nav convert ROUTE [--events LOG] [--menus tools/data/menu_nav.txt] [--out FILE] [--force] [--dry-run]
  python -m tools.route_nav show ROUTE [--menus FILE]
  python -m tools.route_nav suggest-memlog [--menus FILE]
  python -m tools.route_nav insert ROUTE --at P --to Q --menu ID --select index:N|name:TEXT [--activate] [--expect FIELDS]
  python -m tools.route_nav encode-name TEXT

A nav step (grammar in `tools/play/route_nav.py`, docs/t1640-nav-tools.md) replaces the recorded presses of the polls [at, to) by
a closed loop on the menu state: the host reads the cursor from guest memory, presses the d-pad until it is on the target,
presses A and continues at `to`. The recorded pad states are NEVER changed, a nav line is a comment an old host ignores.

`convert` finds the A presses of an existing recording, asks the `--route-event-log` memory samples (`--route-log-mem`, T1633)
which menu was active and where its cursor stood at that poll, finds the run of d-pad presses that led there, and writes
`ROUTE.nav.txt` with one `# nav:` line per activation it could attribute. Each skipped activation is reported with the reason.
The convert output differs from the input by the added comment lines only (checked before writing).
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from tools import route_events, route_trim
from tools.play import route_nav as grammar
from tools.route_nav_state import (
    NO_ROW,
    DumpSource,
    HostNavLine,
    HostNavSkip,
    Lookup,
    MemTimeline,
    MenuState,
    NavEvents,
    NavLogSource,
    SampleSource,
    StateSource,
    read_dump_manifest,
    read_nav_events,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MENUS = ROOT / grammar.DEFAULT_MENU_FILE
DPAD = ("UP", "DOWN", "LEFT", "RIGHT")
AXIS_DIRECTIONS = {"v": frozenset({"UP", "DOWN"}), "h": frozenset({"LEFT", "RIGHT"})}
MAX_MEMLOG_PER_RUN = 8
# menus whose item ids repeat (MEASURED by agent R, docs/t1640-menu-state-findings.md): a select by id would hit the first row with that id
REPEATED_ID_MENUS = frozenset({"editor-map-settings", "editor-tools"})
DEFAULT_MAX_GAP = (
    90  # polls between a d-pad release and the next press that still count as one run of presses
)


class ToolError(ValueError):
    """The request is refused (exit 2)."""


# ---- pad timeline ----


@dataclass(frozen=True)
class Press:
    edge: int  # first poll of the press
    release: int  # first poll after it
    dirs: frozenset[str]


@dataclass
class PadTimeline:
    polls: int
    a_edges: list[tuple[int, int]]  # (edge, release) of every press of A
    dpad: list[Press]
    quiet: list[bool]  # per poll: no button, sticks inside the dead zone


def build_pad(route: route_trim.Route, button: str = "A") -> PadTimeline:
    states = route_trim.expand(route)
    quiet: list[bool] = []
    down: list[bool] = []
    dirs: list[frozenset[str]] = []
    for state in states:
        values = route_trim.parse_state(state)
        quiet.append(route_trim.is_quiet(state))
        if values is None:
            down.append(False)
            dirs.append(frozenset())
            continue
        if button in route_trim.DIGITAL:
            down.append(button in values)
        else:
            down.append(values.get(button, 0) > route_trim.ANALOG_QUIET)
        dirs.append(frozenset(name for name in DPAD if name in values))

    def runs(flags: list[bool]) -> list[tuple[int, int]]:
        found: list[tuple[int, int]] = []
        start: int | None = None
        for index, flag in enumerate(flags):
            if flag and start is None:
                start = index
            elif not flag and start is not None:
                found.append((start, index))
                start = None
        if start is not None:
            found.append((start, len(flags)))
        return found

    presses = [
        Press(start, end, frozenset().union(*dirs[start:end]))
        for start, end in runs([bool(item) for item in dirs])
    ]
    return PadTimeline(len(states), runs(down), presses, quiet)


# ---- the conversion ----


@dataclass
class Outcome:
    poll: int  # the A press edge
    step: grammar.NavStep | None
    reason: str  # why it was skipped, or a short note when converted
    notes: list[str] = field(default_factory=list)


@dataclass
class Options:
    max_gap: int = DEFAULT_MAX_GAP
    select_by: str = "auto"  # auto index id name
    button: str = "A"
    avoid_id: frozenset[str] = REPEATED_ID_MENUS


def read_names(path: Path) -> dict[tuple[str, int], bytes]:
    """`MENU INDEX TEXT...` lines (`#` comments) to {(menu, index): label bytes}."""
    names: dict[tuple[str, int], bytes] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"(\S+)\s+(\d+)\s+(.+)", line)
        if not match:
            raise ToolError(f"{path}:{number}: want 'MENU INDEX TEXT'")
        names[(match.group(1), int(match.group(2)))] = match.group(3).strip().encode("utf-8")
    return names


def existing_steps(text: str) -> list[tuple[int, grammar.NavStep]]:
    return grammar.scan_record(text).steps


def first_lookup(sources: list[StateSource], poll: int, menus: grammar.MenuTable) -> Lookup:
    """The first source that knows the menu state at `poll`; else why none did (with the source names when several were asked)."""
    reasons: list[str] = []
    for source in sources:
        found = source.lookup(poll, menus)
        if found.state is not None:
            return found
        reasons.append(found.reason)
    if len(sources) == 1:
        return Lookup(None, reasons[0], sources[0].name)
    return Lookup(
        None,
        "; ".join(
            f"{source.name}: {reason}" for source, reason in zip(sources, reasons, strict=True)
        ),
    )


def label_of(
    menu: grammar.Menu, state: MenuState, names: dict[tuple[str, int], bytes]
) -> bytes | None:
    return state.label if state.label is not None else names.get((menu.id, state.cursor))


def can_use_id(menu: grammar.Menu, state: MenuState, options: Options) -> str | None:
    """None when `select=id:` is safe, else why not."""
    if menu.items is None or menu.items.id_off is None:
        return "the menu has no items= with id="
    if state.item_id is None or state.item_id == NO_ROW:
        return "the row has no item id (0xFFFFFFFF) or none was recorded"
    if menu.id in options.avoid_id:
        return "the item ids of this menu repeat (--avoid-id)"
    if state.ids_unique is False:
        return "another row of the menu has the same item id"
    return None


def can_use_name(
    menu: grammar.Menu, state: MenuState, names: dict[tuple[str, int], bytes]
) -> tuple[bytes | None, str | None]:
    """(label, None) when `select=name:` is safe, else (None, why not)."""
    if menu.items is None or menu.items.text_off is None:
        return None, "the menu has no items= with text="
    label = label_of(menu, state, names)
    if label is None:
        return None, "no label known (no row text in the source, no --names-from)"
    if state.label_unique is False:
        return None, "another row of the menu has the same label"
    try:
        grammar.decode_name(grammar.encode_name(label))
    except grammar.NavError as error:
        return None, f"label {label!r} not usable ({error})"
    return label, None


def choose_select(
    menu: grammar.Menu, state: MenuState, names: dict[tuple[str, int], bytes], options: Options
) -> tuple[tuple[str, int, bytes] | None, str, list[str]]:
    """((kind, number, label), why-not, notes). auto follows the menu's `selectby=` (what the host recorder writes for it):
    `id` (default) prefers the item id (stable when the list is reordered), then the name, then the cursor index; `name` (lists
    whose ids are positions) the name, then the index; `index` the index. `--select-by` overrides the table."""
    id_problem = can_use_id(menu, state, options)
    label, name_problem = can_use_name(menu, state, names)
    wanted = options.select_by
    notes: list[str] = []
    if wanted == "id" and id_problem is not None:
        return None, f"--select-by id: {id_problem}", notes
    if wanted == "name" and name_problem is not None:
        return None, f"--select-by name: {name_problem} for {menu.id} index {state.cursor}", notes
    auto = wanted == "auto"
    tried_id = wanted == "id" or (auto and menu.selectby == "id")
    tried_name = wanted == "name" or (auto and menu.selectby in ("id", "name"))
    if tried_id and id_problem is None and state.item_id is not None:
        return ("id", state.item_id, b""), "", notes
    if tried_name and name_problem is None and label is not None:
        if wanted == "auto" and menu.selectby == "id":
            notes.append(f"no item id ({id_problem}), the label is used")
        return ("name", 0, label), "", notes
    if wanted == "auto":
        if menu.selectby == "index":
            notes.append("index: the menu table says selectby=index")
        else:
            why_id = f"no id ({id_problem}), " if menu.selectby == "id" else ""
            notes.append(f"index used: {why_id}no name ({name_problem})")
    return ("index", state.cursor, b""), "", notes


def promote_line(
    host: HostNavLine, edge: int, marks: list[int], occupied: list[tuple[int, int]], count: int
) -> Outcome:
    """The host recorder's own `nav-line` as the conversion of the press at `edge` (checked, not recomputed)."""
    step = host.step
    where = f"the host's nav-line (logged at poll {host.poll}, at={step.at} to={step.to})"
    if not step.at <= edge <= step.to or not step.activate:
        return Outcome(edge, None, f"{where} does not hold the activation press at poll {edge}")
    if any(step.at < mark < step.to for mark in marks):
        return Outcome(edge, None, f"{where} has a mark inside its range")
    if any(start < step.to and end > step.at for start, end in occupied):
        return Outcome(edge, None, f"{where} overlaps another nav step")
    if count >= grammar.MAX_NAV_LINES:
        return Outcome(edge, None, f"the record already has {grammar.MAX_NAV_LINES} nav lines")
    return Outcome(edge, step, f"promoted from {where}")


def plan_conversion(
    route: route_trim.Route,
    text: str,
    menus: grammar.MenuTable,
    sources: list[StateSource],
    names: dict[tuple[str, int], bytes],
    options: Options,
    promoted: dict[int, HostNavLine] | None = None,
) -> list[Outcome]:
    pad = build_pad(route, options.button)
    marks = route.marks()
    for source in sources:
        prepare = getattr(source, "prepare", None)
        if prepare is not None:
            prepare([edge for edge, _ in pad.a_edges], marks)
    present = existing_steps(text)
    occupied = [(step.at, step.to) for _, step in present]
    count = len(present)
    outcomes: list[Outcome] = []
    for edge, release in pad.a_edges:
        covered = [
            number for number, step in present if step.at <= edge < max(step.to, step.at + 1)
        ]
        if covered:
            outcomes.append(
                Outcome(edge, None, f"already covered by the nav step of line {covered[0]}")
            )
            continue
        if any(edge < mark < release for mark in marks):
            outcomes.append(
                Outcome(
                    edge, None, "a mark lies inside the A press (a nav range cannot contain a mark)"
                )
            )
            continue
        if any(edge < start < release for start, _ in occupied):
            outcomes.append(Outcome(edge, None, "an existing nav step starts inside the A press"))
            continue
        host = (promoted or {}).get(edge)
        if host is not None:
            outcomes.append(promote_line(host, edge, marks, occupied, count))
            if outcomes[-1].step is not None:
                occupied.append((host.step.at, host.step.to))
                count += 1
            continue
        found = first_lookup(sources, edge, menus)
        if found.state is None:
            outcomes.append(Outcome(edge, None, found.reason))
            continue
        state = found.state
        menu = menus[state.menu]
        if menu.select != options.button:
            outcomes.append(
                Outcome(
                    edge,
                    None,
                    f"menu {menu.id} is selected with {menu.select}, this conversion looks for {options.button} presses (--button)",
                )
            )
            continue
        cursor = state.cursor
        notes: list[str] = []
        if state.ready is False:
            notes.append(
                "the menu's ready= conditions did not hold at the press (the page was still opening, the recorded press worked)"
            )
        # the run of d-pad presses that led to the activation
        bound = max(
            [0]
            + [mark for mark in marks if mark <= edge]
            + [end for _, end in occupied if end <= edge]
        )
        axis = AXIS_DIRECTIONS[menu.axis or "v"]
        cursor_position = edge
        run: list[Press] = []
        for press in reversed([item for item in pad.dpad if item.edge < edge]):
            if press.release > edge or press.edge < bound or not press.dirs <= axis:
                break
            if cursor_position - press.release > options.max_gap:
                break
            if any(not pad.quiet[poll] for poll in range(press.release, cursor_position)):
                break
            run.append(press)
            cursor_position = press.edge
        start = run[-1].edge if run else edge
        chosen, why_not, choice_notes = choose_select(menu, state, names, options)
        notes += choice_notes
        if chosen is None:
            outcomes.append(Outcome(edge, None, why_not))
            continue
        kind, number, label = chosen
        step = grammar.NavStep(
            start, release, menu.id, kind, number if kind != "name" else 0, label, activate=True
        )
        if count >= grammar.MAX_NAV_LINES:
            outcomes.append(
                Outcome(edge, None, f"the record already has {grammar.MAX_NAV_LINES} nav lines")
            )
            continue
        travel = f"cursor {cursor}" + (f" of {state.count}" if state.count is not None else "")
        label_text = f", row '{state.label.decode('latin-1')}'" if state.label else ""
        outcomes.append(
            Outcome(
                edge,
                step,
                f"menu {menu.id}, {travel}{label_text}, {len(run)} d-pad press(es), from {found.source}",
                notes,
            )
        )
        occupied.append((step.at, step.to))
        count += 1
    return outcomes


def insert_lines(text: str, steps: list[grammar.NavStep]) -> tuple[str, list[int]]:
    """Insert the nav lines before the run line holding their `at` (or before the trailer). Returns (text, line numbers added, 1 based)."""
    lines = text.split("\n")
    eol = "\r" if lines and lines[0].endswith("\r") else ""
    pending = sorted(steps, key=lambda step: (step.at, step.to))
    out: list[str] = []
    added: list[int] = []
    position = 0

    def flush(limit: int | None) -> None:
        while pending and (limit is None or pending[0].at < limit):
            out.append(pending.pop(0).format() + eol)
            added.append(len(out))

    for raw in lines:
        stripped = raw.strip()
        if stripped.startswith("# polls:"):
            flush(None)
        elif stripped and not stripped.startswith("#"):
            head = stripped.split(None, 1)[0]
            if head.isdigit():
                frames = int(head)
                flush(position + frames)
                position += frames
        out.append(raw)
    flush(None)
    return "\n".join(out), added


def normalised(problems: list[grammar.Problem]) -> Counter[str]:
    return Counter(re.sub(r"line \d+", "line N", problem.message) for problem in problems)


def check_conversion(
    original: str, converted: str, added: list[int], menus: dict[str, grammar.Menu] | None
) -> list[str]:
    """Problems of a converted/edited record against its original. Empty = only valid comment lines were added."""
    problems: list[str] = []
    drop = set(added)
    kept = [line for number, line in enumerate(converted.split("\n"), 1) if number not in drop]
    if "\n".join(kept) != original:
        problems.append("removing the added lines does not give the original text back")
    if any(
        not converted.split("\n")[number - 1].startswith(grammar.NAV_PREFIX) for number in added
    ):
        problems.append("a line other than a '# nav:' comment was added")
    try:
        before = route_trim.parse_route(original)
        after = route_trim.parse_route(converted)
    except route_trim.RouteError as error:
        problems.append(f"the record no longer parses: {error}")
        return problems
    if route_trim.expand(before) != route_trim.expand(after):
        problems.append("the pad states changed")
    if before.polls != after.polls or before.marks() != after.marks():
        problems.append("the poll total or the marks changed")
    if before.trailer_line != after.trailer_line:
        problems.append("the '# polls:' trailer changed")
    fresh = normalised(grammar.validate_navs(converted, menus)) - normalised(
        grammar.validate_navs(original, menus)
    )
    if fresh:
        problems.append("new nav problems: " + "; ".join(sorted(fresh)))
    return problems


@dataclass
class Conversion:
    outcomes: list[Outcome]
    text: str
    added: list[int]

    @property
    def converted(self) -> list[Outcome]:
        return [outcome for outcome in self.outcomes if outcome.step is not None]


def convert_text(
    text: str,
    menus: grammar.MenuTable,
    source: MemTimeline | StateSource | list[StateSource],
    names: dict[tuple[str, int], bytes] | None = None,
    options: Options | None = None,
    promoted: dict[int, HostNavLine] | None = None,
) -> Conversion:
    """Plan and insert. `source` is a MemTimeline (mem samples), one state source or a priority ordered list of them."""
    if isinstance(source, MemTimeline):
        sources: list[StateSource] = [SampleSource(source)]
    elif isinstance(source, list):
        sources = source
    else:
        sources = [source]
    route = route_trim.parse_route(text)
    outcomes = plan_conversion(
        route, text, menus, sources, names or {}, options or Options(), promoted
    )
    steps = [outcome.step for outcome in outcomes if outcome.step is not None]
    new_text, added = insert_lines(text, steps)
    return Conversion(outcomes, new_text, added)


def report(route_name: str, conversion: Conversion) -> str:
    lines = [
        f"route_nav convert: {route_name}: {len(conversion.outcomes)} activation(s) of A found"
    ]
    for outcome in conversion.outcomes:
        if outcome.step is None:
            lines.append(f"  poll {outcome.poll:6d}  SKIPPED  {outcome.reason}")
        else:
            lines.append(f"  poll {outcome.poll:6d}  CONVERTED  {outcome.step.format()}")
            lines.append(f"                          {outcome.reason}")
        lines += [f"                          note: {note}" for note in outcome.notes]
    skipped = len(conversion.outcomes) - len(conversion.converted)
    lines.append(f"  {len(conversion.converted)} converted, {skipped} skipped")
    return "\n".join(lines)


# ---- subcommands ----


def read_text(path: Path) -> str:
    return path.read_bytes().decode("utf-8", errors="surrogateescape")


def write_text(path: Path, text: str) -> None:
    path.write_bytes(text.encode("utf-8", errors="surrogateescape"))


def load_menus(path: Path | None, required: bool) -> dict[str, grammar.Menu] | None:
    target = path or DEFAULT_MENUS
    if not target.is_file():
        if required:
            raise ToolError(
                f"menu table {target} does not exist (tools/data/menu_nav.txt is written by the menu state reverse engineering, T1640; give --menus FILE)"
            )
        return None
    try:
        return grammar.load_menu_table(target)
    except grammar.NavError as error:
        raise ToolError(f"{target}: {error}") from None


def default_out(route: Path) -> Path:
    return route.with_name(route.stem + ".nav" + route.suffix)


def parse_stalls(text: str | None) -> dict[int, int] | None:
    """`mark1=120,mark3=704` to {1: 120, 3: 704}: the polls the replay stalled at each mark."""
    if text is None:
        return None
    stalls: dict[int, int] = {}
    for item in text.split(","):
        match = re.fullmatch(r"mark([1-9][0-9]?)=([0-9]{1,9})", item.strip())
        if not match or int(match.group(1)) in stalls:
            raise ToolError(
                f"--stall-polls {text!r}: want markK=N,markJ=M (K 1..16, N polls, each mark once)"
            )
        stalls[int(match.group(1))] = int(match.group(2))
    return stalls


def build_sources(
    route_path: Path,
    events: Path | None,
    dumps: Path | None,
    host_offset: int | None = None,
    stalls: dict[int, int] | None = None,
) -> tuple[list[StateSource], list[str], dict[int, HostNavLine]]:
    """The state sources of a conversion in priority order (button dumps, nav lines, mem samples), a line about each, and the
    host's own `nav-line` additions keyed by their select edge (promoted as they are)."""
    sources: list[StateSource] = []
    promoted: dict[int, HostNavLine] = {}
    described: list[str] = []
    if dumps is not None:
        try:
            dump_events = read_dump_manifest(dumps)
        except OSError as error:
            raise ToolError(f"cannot read the button dump manifest of {dumps}: {error}") from None
        sources.append(DumpSource(dump_events, host_offset, stalls))
        described.append(
            f"button dumps {dumps}: {len(dump_events)} A press(es) with a before snapshot"
        )
    if events is None:
        found = [
            path
            for path in route_events.sibling_logs(route_path)
            if path.suffix in (".events", ".event-log")
        ]
        events = found[0] if found else None
    if events is None and not sources:
        raise ToolError(
            f"no state source: no event log next to {route_path} and no --dumps DIR. Record with the host's --route-event-log "
            "(nav lines, or --route-log-mem for plain memory menus: python -m tools.route_nav suggest-memlog) or replay the "
            "route with --dump-on-button (python -m tools.route_nav suggest-dumps)"
        )
    if events is not None:
        try:
            nav_events = read_nav_events(events)
            nav_samples = nav_events.samples
            timeline = MemTimeline.from_items(route_events.parse_event_log(events))
        except OSError as error:
            raise ToolError(f"cannot read {events}: {error}") from None
        if nav_samples:
            sources.append(NavLogSource(nav_samples))
        promoted = {line.edge: line for line in nav_events.lines}
        sources.append(SampleSource(timeline))
        described.append(
            f"event log {events}: {len(nav_samples)} nav state line(s), {len(nav_events.lines)} nav-line, "
            f"{len(nav_events.skips)} nav-skip, "
            f"{sum(len(series) for series in timeline.series.values())} memory sample(s) of {len(timeline.series)} address(es)"
        )
    return sources, described, promoted


def command_convert(args: argparse.Namespace) -> int:
    route_path: Path = args.route
    try:
        text = read_text(route_path)
        route_trim.parse_route(text)
    except OSError as error:
        raise ToolError(f"cannot read {route_path}: {error}") from None
    except route_trim.RouteError as error:
        raise ToolError(f"{route_path}: {error}") from None
    out = args.out or default_out(route_path)
    if out.resolve() == route_path.resolve():
        raise ToolError("--out is the input file, the original is never overwritten")
    if out.exists() and not args.force and not args.dry_run:
        raise ToolError(
            f"{out} exists (use --force to replace a previous conversion, never the original)"
        )
    menus = load_menus(args.menus, required=True)
    assert menus is not None
    sources, described, promoted = build_sources(
        route_path,
        args.events,
        args.dumps,
        args.host_poll_offset,
        parse_stalls(args.stall_polls),
    )
    if args.no_promote:
        promoted = {}
    names = read_names(args.names_from) if args.names_from else {}
    conversion = convert_text(
        text,
        menus,
        sources,
        names,
        Options(
            args.max_gap,
            args.select_by,
            args.button,
            REPEATED_ID_MENUS | frozenset(args.avoid_id),
        ),
        promoted,
    )
    for line in described:
        print(line)
    print(report(route_path.name, conversion))
    problems = check_conversion(text, conversion.text, conversion.added, menus)
    if problems:
        print("route_nav: INTERNAL CHECK FAILED, nothing written:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 3
    if not conversion.converted:
        print("nothing to add: no file written")
        return 0
    if args.dry_run:
        print("dry run: nothing written")
        return 0
    write_text(out, conversion.text)
    print(
        f"wrote {out} ({len(conversion.added)} nav line(s) added, the pad states, marks, header and trailer are unchanged)"
    )
    return 0


def command_show(args: argparse.Namespace) -> int:
    try:
        text = read_text(args.route)
    except OSError as error:
        raise ToolError(f"cannot read {args.route}: {error}") from None
    table_problem = None
    try:
        menus = load_menus(args.menus, required=False)
    except ToolError as error:
        menus, table_problem = None, str(error)
        print(f"route_nav: {error} (menu names are not checked)", file=sys.stderr)
    found = grammar.scan_record(text)
    print(
        f"{args.route}: {found.polls} polls, {len(found.marks)} mark(s), {len(found.steps)} nav step(s)"
        + ("" if menus is not None else " (no menu table: menus not checked)")
    )
    for number, step in found.steps:
        before = sum(1 for mark in found.marks if mark <= step.at)
        print(
            f"  line {number}: polls {step.at}..{step.to} ({step.to - step.at} recorded poll(s) replaced), after mark {before}, "
            f"menu {step.menu}, select {step.select_text}"
            + (", activate" if step.activate else "")
            + (f", expect {step.expect}" if step.expect else "")
            + f", timeout {step.effective_timeout_ms} ms, retry {step.effective_retry}"
        )
    for number, reason in found.bad:
        print(f"  line {number}: MALFORMED: {reason}")
    problems = grammar.validate_route(text, menus)
    for problem in problems:
        print(f"route_nav: {problem}", file=sys.stderr)
    print(f"{len(problems)} problem(s)")
    if args.events is not None:
        try:
            events = read_nav_events(args.events)
        except OSError as error:
            raise ToolError(f"cannot read {args.events}: {error}") from None
        print(f"{args.events}: {len(events.lines)} nav-line, {len(events.skips)} nav-skip")
        for line in skip_report(events):
            print(line)
    return 2 if problems or table_problem else 0


def skip_report(events: NavEvents) -> list[str]:
    """One line per nav-skip reason: how many and the first polls, then the menus they were about."""
    reasons: dict[str, list[HostNavSkip]] = {}
    for skip in events.skips:
        reasons.setdefault(skip.reason, []).append(skip)
    lines = []
    for reason, skips in sorted(reasons.items()):
        polls = ", ".join(str(skip.poll) for skip in skips[:6]) + (" ..." if len(skips) > 6 else "")
        menus = sorted({skip.menu for skip in skips if skip.menu})
        lines.append(
            f"  nav-skip {reason}: {len(skips)}x, polls {polls}"
            + (f", menus {', '.join(menus)}" if menus else "")
        )
    return lines


@dataclass
class Comparison:
    matched: list[grammar.NavStep] = field(default_factory=list)
    only_record: list[tuple[int, grammar.NavStep]] = field(
        default_factory=list
    )  # (record line, step)
    only_log: list[HostNavLine] = field(default_factory=list)
    explained: list[tuple[HostNavLine, str]] = field(
        default_factory=list
    )  # only in the log, with the nav-skip that says why

    @property
    def equal(self) -> bool:
        return not self.only_record and not self.only_log


def step_key(step: grammar.NavStep) -> tuple[int, int, str, str, bool]:
    return (step.at, step.to, step.menu, step.select_text, step.activate)


def compare_record(text: str, events: NavEvents) -> Comparison:
    """The `# nav:` lines of a record against the host's `nav-line` event log lines. A log line the record does not have is
    'explained' when a later `nav-skip ... reason=suppressed` names the same range (T1632 dropped it)."""
    record = grammar.scan_record(text).steps
    result = Comparison()
    pending = [(number, step) for number, step in record]
    for line in events.lines:
        key = step_key(line.step)
        found = next((item for item in pending if step_key(item[1]) == key), None)
        if found is not None:
            pending.remove(found)
            result.matched.append(line.step)
            continue
        dropped = next(
            (
                skip
                for skip in events.skips
                if skip.reason == "suppressed"
                and skip.fields.get("at") == str(line.step.at)
                and skip.fields.get("to") == str(line.step.to)
            ),
            None,
        )
        if dropped is not None:
            result.explained.append(
                (line, f"dropped by the hotkey suppression (nav-skip at poll {dropped.poll})")
            )
        else:
            result.only_log.append(line)
    result.only_record = pending
    return result


def command_compare(args: argparse.Namespace) -> int:
    try:
        text = read_text(args.route)
        events = read_nav_events(args.events)
    except OSError as error:
        raise ToolError(f"cannot read {error.filename}: {error.strerror}") from None
    result = compare_record(text, events)
    record_count = len(grammar.scan_record(text).steps)
    print(
        f"{args.route}: {record_count} nav line(s) in the record, {len(events.lines)} nav-line(s) in {args.events}"
    )
    print(f"  {len(result.matched)} identical")
    for number, step in result.only_record:
        print(f"  ONLY IN THE RECORD (line {number}): {step.format()}")
    for line in result.only_log:
        print(f"  ONLY IN THE LOG (logged at poll {line.poll}): {line.step.format()}")
    for line, why in result.explained:
        print(f"  only in the log, explained: {line.step.format()} -- {why}")
    for bad in events.bad_lines:
        print(f"  MALFORMED nav-line in the log: {bad}")
    for line in skip_report(events):
        print(line)
    verdict = (
        "the record equals the host's nav-lines"
        if result.equal and not events.bad_lines
        else "MISMATCH"
    )
    print(verdict)
    return 0 if result.equal and not events.bad_lines else 2


def sampleable_menus(
    menus: dict[str, grammar.Menu],
) -> tuple[list[grammar.Menu], list[grammar.Menu]]:
    """(menus a `--route-log-mem` sample can tell, menus that need page offsets and so nav lines or button dumps)."""
    plain = [menu for menu in menus.values() if menu.has_cursor and not menu.uses_page()]
    paged = [menu for menu in menus.values() if menu.uses_page()]
    return plain, paged


def memlog_groups(
    menus: dict[str, grammar.Menu], per_run: int = MAX_MEMLOG_PER_RUN, everything: bool = False
) -> list[list[str]]:
    """The `--route-log-mem` specs the plain memory menus need, deduplicated, in groups of at most `per_run`."""
    specs: list[str] = []
    seen: set[tuple[bool, int, int, int]] = set()
    for menu in sampleable_menus(menus)[0]:
        for ref in menu.refs(everything):
            if ref.key not in seen:
                seen.add(ref.key)
                specs.append(ref.spec())
    return [specs[index : index + per_run] for index in range(0, len(specs), per_run)]


def command_suggest(args: argparse.Namespace) -> int:
    menus = load_menus(args.menus, required=True)
    assert menus is not None
    plain, paged = sampleable_menus(menus)
    groups = memlog_groups(menus, args.max_per_run, args.everything)
    total = sum(len(group) for group in groups)
    print(
        f"# {len(plain)} of {len(menus)} menu(s) have plain memory conditions and cursor: {total} distinct address(es) to sample; "
        f"the host samples at most {MAX_MEMLOG_PER_RUN} --route-log-mem per run"
    )
    if paged:
        print(
            f"# HONEST LIMIT: {len(paged)} menu(s) ({', '.join(menu.id for menu in paged[:6])}{', ...' if len(paged) > 6 else ''}) read their cursor "
            "inside a page widget whose slot moves (@+OFF page specs): --route-log-mem cannot sample that. They convert only from "
            "'nav' lines of the host recorder log (phase 2) or from --dump-on-button snapshots (python -m tools.route_nav suggest-dumps)."
        )
    if not groups:
        print("# nothing to sample with --route-log-mem for this table")
        return 0
    print(
        "# Add these to the RECORDING run (with --route-event-log FILE); the 'names=' and 'items=' memory is not sampled (strings are not 4 byte values)"
    )
    for number, group in enumerate(groups, 1):
        print(f"# run {number} of {len(groups)}" if len(groups) > 1 else "# one run is enough")
        print(" ".join(f"--route-log-mem {spec}" for spec in group))
    return 0


MAX_DUMP_RANGES = 16  # the host takes at most 16 --dump-guest-range entries


def dump_ranges(menus: grammar.MenuTable, extra: list[str]) -> list[str]:
    """The `--dump-guest-range` entries the DumpSource needs: the widget table and every plain memory reference of the table
    (an indirect one also needs its pointer dword), plus the caller's extras (item node memory)."""
    ranges: list[str] = []

    def add(entry: str) -> None:
        if entry not in ranges:
            ranges.append(entry)

    if menus.widgets is not None:
        add(f"0x{menus.widgets.table:X}:0x{menus.widgets.stride * menus.widgets.count:X}")
    for menu in menus.values():
        for ref in menu.refs(everything=True):
            if ref.page:
                continue
            if ref.indirect:
                add(f"0x{ref.address:X}:4")
                add(f"*0x{ref.address:X}+0x{ref.offset:X}:{ref.width}")
            else:
                add(f"0x{ref.address:X}:{ref.width}")
    for entry in extra:
        add(entry)
    return ranges


def command_dumps(args: argparse.Namespace) -> int:
    menus = load_menus(args.menus, required=True)
    assert isinstance(menus, grammar.MenuTable)
    ranges = dump_ranges(menus, args.extra)
    if len(ranges) > MAX_DUMP_RANGES:
        raise ToolError(
            f"{len(ranges)} dump ranges, the host takes at most {MAX_DUMP_RANGES}: use --menus with fewer menus or drop --extra"
        )
    print(
        "# Replay the route (its '# flags:' line must match the replay command, T1618) with a button dump at every press, from poll 0:"
    )
    print(
        "#   --synthetic-pad --replay-input ROUTE --stop-at-poll LAST_POLL_PLUS_SLACK --dump-on-button --dump-after-frames 2"
    )
    print("#   --dump-guest-dir DIR")
    print(f'#   --dump-guest-range "{",".join(ranges)}"')
    print(
        f"# {len(ranges)} range(s) of {MAX_DUMP_RANGES}. Do NOT pass --dump-button-after-replay (that arms the dump only after the route). "
        "Dump polls are HOST polls: a mark stall (min=/event waits) shifts every later press, convert infers the shift per mark segment "
        "(or give --stall-polls markK=N,.. / --host-poll-offset N). Then: python -m tools.route_nav convert ROUTE --dumps DIR"
    )
    print(
        "# Row labels / ids by walking need the memory of the item nodes: add them with --extra (the T1640 measurements used "
        "'*0x7BB1DC+0:0x6000' and '0x79EF00:0x400'); without them the cursor and the item id (itemid=) are still read."
    )
    print(",".join(ranges))
    return 0


def command_insert(args: argparse.Namespace) -> int:
    route_path: Path = args.route
    try:
        original = read_text(route_path)
        route_trim.parse_route(original)
    except OSError as error:
        raise ToolError(f"cannot read {route_path}: {error}") from None
    except route_trim.RouteError as error:
        raise ToolError(f"{route_path}: {error}") from None
    parts = [f"at={args.at}", f"to={args.to}", f"menu={args.menu}", f"select={args.select}"]
    if args.activate:
        parts.append("activate")
    if args.expect:
        parts.append(f"expect={args.expect}")
    if args.timeout is not None:
        parts.append(f"timeout={args.timeout}")
    if args.retry is not None:
        parts.append(f"retry={args.retry}")
    try:
        step = grammar.parse_nav_line(grammar.NAV_PREFIX + " " + " ".join(parts))
    except grammar.NavError as error:
        raise ToolError(f"nav step refused: {error}") from None
    menus = load_menus(args.menus, required=False)
    new_text, added = insert_lines(original, [step])
    problems = check_conversion(original, new_text, added, menus)
    if problems:
        raise ToolError("not inserted: " + "; ".join(problems))
    target = args.out or route_path
    if target.resolve() != route_path.resolve() and target.exists() and not args.force:
        raise ToolError(f"{target} exists (use --force)")
    if target.resolve() == route_path.resolve():
        backup = route_path.with_name(route_path.name + ".t1640.bak")
        if not backup.exists():
            shutil.copyfile(route_path, backup)
    write_text(target, new_text)
    print(f"route_nav: inserted at line {added[0]} of {target}: {step.format()}")
    return 0


def command_encode(args: argparse.Namespace) -> int:
    encoded = grammar.encode_name(args.text)
    try:
        grammar.decode_name(encoded)
    except grammar.NavError as error:
        raise ToolError(f"{args.text!r} cannot be a name: {error}") from None
    print("name:" + encoded)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.route_nav",
        description="Nav steps for a recorded route: convert an existing recording, list, insert, memory sampling advice (T1640).",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    convert = sub.add_parser(
        "convert",
        help="add '# nav:' lines where the event log lets the menu and cursor be recovered",
    )
    convert.add_argument("route", type=Path, help="the recorded route (.txt); never modified")
    convert.add_argument(
        "--events",
        type=Path,
        help="host --route-event-log file with --route-log-mem samples (default ROUTE.events / .event-log)",
    )
    convert.add_argument(
        "--menus", type=Path, help=f"menu table (default {grammar.DEFAULT_MENU_FILE})"
    )
    convert.add_argument("--out", type=Path, help="output file (default ROUTE.nav.txt)")
    convert.add_argument(
        "--force", action="store_true", help="replace an existing output (never the original)"
    )
    convert.add_argument("--dry-run", action="store_true", help="print the report, write nothing")
    convert.add_argument(
        "--names-from",
        type=Path,
        metavar="FILE",
        help="'MENU INDEX TEXT' lines with the item labels (emits select=name:TEXT)",
    )
    convert.add_argument(
        "--select-by",
        choices=["auto", "index", "id", "name"],
        default="auto",
        help="auto: the item id when the source has one that is unique (id:N), else the label (name:TEXT) when known, else the cursor index",
    )
    convert.add_argument(
        "--dumps",
        type=Path,
        metavar="DIR",
        help="a --dump-on-button directory (buttons.jsonl and buttons/) of a replay of this route: the menu state is read from the "
        "snapshots of the A presses (python -m tools.route_nav suggest-dumps prints the ranges)",
    )
    convert.add_argument(
        "--stall-polls",
        metavar="markK=N,..",
        help="with --dumps: the polls the replay stalled at each mark (dump polls are HOST polls, record poll + the stalls of the marks before it); default: inferred per mark segment from the dumped A presses",
    )
    convert.add_argument(
        "--host-poll-offset",
        type=int,
        metavar="N",
        help="with --dumps: a constant added to every record poll to get its dump poll (exact lookup, no inference)",
    )
    convert.add_argument(
        "--no-promote",
        action="store_true",
        help="ignore the host recorder's nav-line event log lines and compute every step from the menu state",
    )
    convert.add_argument(
        "--avoid-id",
        action="append",
        default=[],
        metavar="MENU",
        help="never select this menu by item id (its ids repeat); editor-map-settings and editor-tools are always avoided",
    )
    convert.add_argument(
        "--max-gap",
        type=int,
        default=DEFAULT_MAX_GAP,
        help="polls between presses that still belong to one run of d-pad presses (default 90)",
    )
    convert.add_argument(
        "--button",
        default="A",
        choices=list(grammar.SELECT_BUTTONS),
        help="the activation button (default A)",
    )
    convert.set_defaults(func=command_convert)

    show = sub.add_parser("show", help="list and validate the nav steps of a route")
    show.add_argument("route", type=Path)
    show.add_argument(
        "--menus", type=Path, help="menu table (default tools/data/menu_nav.txt when it exists)"
    )
    show.add_argument(
        "--events", type=Path, help="a host --route-event-log: also report its nav-skip reasons"
    )
    show.set_defaults(func=command_show)

    compare = sub.add_parser(
        "compare",
        help="check that the record's '# nav:' lines equal the nav-line lines of a host event log, list the nav-skip reasons",
    )
    compare.add_argument("route", type=Path)
    compare.add_argument("--events", type=Path, required=True, metavar="LOG")
    compare.set_defaults(func=command_compare)

    suggest = sub.add_parser(
        "suggest-memlog", help="print the --route-log-mem arguments the recording host needs"
    )
    suggest.add_argument(
        "--menus", type=Path, help=f"menu table (default {grammar.DEFAULT_MENU_FILE})"
    )
    suggest.add_argument(
        "--max-per-run",
        type=int,
        default=MAX_MEMLOG_PER_RUN,
        help="specs per group (the host takes 8)",
    )
    suggest.add_argument(
        "--everything",
        action="store_true",
        help="also the count and ready addresses (default: the active conditions and the cursor, what a conversion reads)",
    )
    suggest.set_defaults(func=command_suggest)

    dumps = sub.add_parser(
        "suggest-dumps",
        help="print the --dump-on-button replay arguments the DumpSource of convert needs",
    )
    dumps.add_argument(
        "--menus", type=Path, help=f"menu table (default {grammar.DEFAULT_MENU_FILE})"
    )
    dumps.add_argument(
        "--extra",
        action="append",
        default=[],
        metavar="RANGE",
        help="one more --dump-guest-range entry (item node memory)",
    )
    dumps.set_defaults(func=command_dumps)

    insert = sub.add_parser(
        "insert", help="write one nav step into a route (backup ROUTE.t1640.bak)"
    )
    insert.add_argument("route", type=Path)
    insert.add_argument("--at", type=int, required=True, metavar="P")
    insert.add_argument("--to", type=int, required=True, metavar="Q")
    insert.add_argument("--menu", required=True, metavar="ID")
    insert.add_argument(
        "--select",
        required=True,
        metavar="SEL",
        help="index:N or name:TEXT (see the encode-name subcommand)",
    )
    insert.add_argument("--activate", action="store_true")
    insert.add_argument("--expect", metavar="FIELDS", help="T1633 wait fields without spaces")
    insert.add_argument("--timeout", type=int, metavar="MS")
    insert.add_argument("--retry", type=int, metavar="N")
    insert.add_argument("--menus", type=Path)
    insert.add_argument("--out", type=Path, help="write here instead of changing ROUTE")
    insert.add_argument("--force", action="store_true")
    insert.set_defaults(func=command_insert)

    encode = sub.add_parser("encode-name", help="print the select=name:... form of an item label")
    encode.add_argument("text")
    encode.set_defaults(func=command_encode)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if getattr(args, "max_gap", 0) < 0 or getattr(args, "max_per_run", 1) < 1:
        print("route_nav: --max-gap must be >= 0 and --max-per-run >= 1", file=sys.stderr)
        return 2
    try:
        return args.func(args)
    except ToolError as error:
        print(f"route_nav: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
