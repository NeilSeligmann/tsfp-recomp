# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1639: cut a poll interval out of a recorded route (for example the owner's visit of the weapon-settings page).

  python -m tools.route_splice ROUTE --list
  python -m tools.route_splice ROUTE --check-page
  python -m tools.route_splice ROUTE --cut 1478:1790 [--cut A:B ...] [--rest-polls 20] [--out FILE] [--dry-run]

A route (docs/input-replay.md) is a poll indexed pad script. `route_trim` (T1636) only cuts idle polls. This tool cuts a chosen
interval [A, B) of polls, INPUTS INCLUDED, and joins the two halves with a short pad-at-rest gap (`--rest-polls`, default 20) so no
button or stick state of the first half leaks into the second. What it keeps consistent:

  marks      a mark at or before A stays, a mark strictly inside (A, B) moves to A (the screen the mark names is the one reached at the
             start of the cut), a mark at or after B moves by -(B-A)+rest. Marks keep their ordinal, so the `# wait:` and `# mark-info:`
             lines (keyed by ordinal) stay valid. The number of marks never changes.
  nav lines  `# nav: at=P to=Q ...` (T1640, plain poll indices, an atomic span): remapped; a span partly overlapping a cut is REFUSED,
             a span fully inside a cut is dropped (reported).
  header     `# tsfp-input`, flag identity, XBE hash, budgets and every other comment are copied verbatim, `# polls:` is rewritten.

The original is never written. This tool does NOT know what the cut inputs do in the game: whether the spliced route still reaches
the preview is a measurement (headless replay, `docs/t-weaponslot-poke.md` T1639), not a property of the file.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from tools.route_trim import RUN_LIMIT, Route, RouteError, parse_route

NAV_PREFIX = "# nav:"


def parse_cut(spec: str, polls: int) -> tuple[int, int]:
    match = re.fullmatch(r"(\d+):(\d+)", spec)
    if not match:
        raise RouteError(f"--cut {spec!r}: want START:END (old poll indices, END exclusive)")
    start, end = int(match.group(1)), int(match.group(2))
    if not 0 <= start < end <= polls:
        raise RouteError(f"--cut {spec}: need 0 <= START < END <= {polls} polls")
    return start, end


def check_cuts(cuts: list[tuple[int, int]]) -> list[tuple[int, int]]:
    ordered = sorted(cuts)
    for (_, end), (start, _) in zip(ordered, ordered[1:], strict=False):
        if start < end:
            raise RouteError(f"cuts overlap: ... {end} and {start} ...")
    return ordered


def new_position(position: int, cuts: list[tuple[int, int]], rest: int) -> int:
    """Old poll index to the new one. Inside a cut -> its start; at/after a cut end -> minus the cut plus the rest gap."""
    shift = 0
    for start, end in cuts:
        if position >= end:
            shift += (end - start) - rest
        elif position > start:
            return position - shift - (position - start)
    return position - shift


def parse_nav(line: str) -> tuple[int, int]:
    at = re.search(r"\bat=(\d+)", line)
    to = re.search(r"\bto=(\d+)", line)
    if not at or not to:
        raise RouteError(f"malformed nav line {line!r} (want at=P to=Q)")
    return int(at.group(1)), int(to.group(1))


def remap_nav(line: str, cuts: list[tuple[int, int]], rest: int) -> str | None:
    """The nav line with at=/to= remapped, None when a cut swallows the whole span, RouteError when a cut splits it."""
    start_p, end_p = parse_nav(line)
    for start, end in cuts:
        if start_p < end and end_p > start:  # the span touches the cut interior
            if start_p >= start and end_p <= end:
                return None
            raise RouteError(
                f"nav span at={start_p} to={end_p} is split by the cut {start}:{end} (a nav span is atomic): "
                "move the cut or remove the nav line first"
            )
    new_at = new_position(start_p, cuts, rest)
    new_to = new_position(end_p, cuts, rest)
    out = re.sub(r"\bat=\d+", f"at={new_at}", line, count=1)
    return re.sub(r"\bto=\d+", f"to={new_to}", out, count=1)


def splice(
    route: Route, cuts: list[tuple[int, int]], rest: int, source: str = ""
) -> tuple[str, list[str]]:
    """The spliced record text and notes (dropped nav lines, relocated marks). `source` names the original in the provenance comment."""
    notes: list[str] = []
    out: list[str] = []
    provenance = (
        "# splice: T1639 cut="
        + ",".join(f"{start}:{end}" for start, end in cuts)
        + f" rest={rest}"
        + (f" from={source}" if source else "")
    )
    header_done = False
    pending_state: str | None = None
    pending_frames = 0
    position = 0
    done: set[int] = set()  # cut indices whose rest gap was written

    def flush() -> None:
        nonlocal pending_state, pending_frames
        frames = pending_frames
        while pending_state is not None and frames > 0:
            piece = min(frames, RUN_LIMIT)
            out.append(f"{piece} {pending_state}".rstrip())
            frames -= piece
        pending_state, pending_frames = None, 0

    def add(state: str, frames: int) -> None:
        nonlocal pending_state, pending_frames
        if frames <= 0:
            return
        if pending_state == state:
            pending_frames += frames
        else:
            flush()
            pending_state, pending_frames = state, frames

    def close_cuts(upto: int) -> None:
        """Write the rest gap of every cut that ended at or before `upto`."""
        for index, (_, end) in enumerate(cuts):
            if index not in done and end <= upto:
                add("", rest)
                done.add(index)

    for item in route.items:
        if item.kind != "text" and not header_done:
            out.append(
                provenance
            )  # after the leading header comments, before the first run or mark
            header_done = True
        if item.kind == "text":
            flush()
            if item.text.strip().startswith(NAV_PREFIX):
                mapped = remap_nav(item.text, cuts, rest)
                if mapped is None:
                    notes.append(f"dropped (inside a cut): {item.text.strip()}")
                    continue
                out.append(mapped)
            else:
                out.append(item.text)
        elif item.kind == "mark":
            close_cuts(item.at)
            flush()
            moved = new_position(item.at, cuts, rest)
            if any(start < item.at < end for start, end in cuts):
                notes.append(f"mark at {item.at} was inside a cut, moved to {moved}")
            out.append(f"# mark: at={moved}")
        else:
            index = position
            end_run = position + item.frames
            while index < end_run:
                close_cuts(index)
                cut = next(((s, e) for s, e in cuts if s <= index < e), None)
                if cut is not None:
                    index = min(end_run, cut[1])
                    continue
                boundaries = [s for s, _ in cuts if s > index]
                stop = min([end_run, *boundaries])
                add(item.state, stop - index)
                index = stop
            position = end_run
    close_cuts(position)
    flush()
    total = route.polls - sum(e - s for s, e in cuts) + rest * len(cuts)
    out.append(f"# polls: {total}")
    return "\n".join(out) + "\n", notes


def expand(route: Route) -> list[str]:
    states: list[str] = []
    for item in route.runs():
        states.extend([item.state] * item.frames)
    return states


def verify(original: Route, spliced: Route, cuts: list[tuple[int, int]], rest: int) -> list[str]:
    """Problems comparing the splice with the original and the cut list. Empty = sound."""
    problems: list[str] = []
    old = expand(original)
    expected: list[str] = []
    cursor = 0
    for start, end in cuts:
        expected.extend(old[cursor:start])
        expected.extend([""] * rest)
        cursor = end
    expected.extend(old[cursor:])
    if expand(spliced) != expected:
        problems.append(
            "the spliced poll states are not the original minus the cuts plus the rest gaps"
        )
    if spliced.polls != len(expected):
        problems.append(f"trailer {spliced.polls} != {len(expected)} polls")
    if len(spliced.marks()) != len(original.marks()):
        problems.append("the number of marks changed")
    if spliced.marks() != [new_position(at, cuts, rest) for at in original.marks()]:
        problems.append("marks were not remapped consistently")
    if spliced.marks() != sorted(spliced.marks()):
        problems.append("marks are no longer in ascending order")
    return problems


def describe_inputs(route: Route, start: int, end: int) -> list[str]:
    """Button presses (not stick values) of the polls [start, end), as `poll+frames:TOKENS`."""
    out: list[str] = []
    position = 0
    for item in route.runs():
        if start <= position < end or position < start < position + item.frames:
            tokens = [t for t in item.state.split() if not t.startswith(("LX", "LY", "RX", "RY"))]
            if tokens:
                out.append(f"{position}+{item.frames}:{'/'.join(tokens)}")
        position += item.frames
    return out


def listing(route: Route) -> str:
    marks = route.marks()
    bounds = [0, *marks, route.polls]
    lines = [f"{route.polls} polls, marks at {marks}"]
    for index in range(len(bounds) - 1):
        lines.append(
            f"segment {index + 1} [{bounds[index]},{bounds[index + 1]}): "
            + (" ".join(describe_inputs(route, bounds[index], bounds[index + 1])) or "(no buttons)")
        )
    return "\n".join(lines)


# After mark 2 (the Map Maker map finished loading) a route that goes straight to the preview presses A three times: open the
# editor from the map list, open the editor menu, choose the preview (docs/t-weaponslot-poke.md T1639). More means an excursion.
DIRECT_A_PRESSES = 3


def count_presses(route: Route, button: str, start: int, end: int) -> int:
    """Presses of `button` in the polls [start, end): a press is a maximal run of consecutive polls holding it."""
    count = 0
    held = False
    for index, state in enumerate(expand(route)):
        if not start <= index < end:
            held = False
            continue
        now = any(token.split("=")[0] == button for token in state.split())
        if now and not held:
            count += 1
        held = now
    return count


def check_page(route: Route) -> tuple[bool, str]:
    """(visited, text): ADVISORY guess whether the route visited a settings page between mark 2 and the last mark."""
    marks = route.marks()
    if len(marks) < 4:
        return (
            False,
            f"cannot tell: {len(marks)} marks, the 4 mark layout is needed (mark 2 map loaded .. mark 4 preview)",
        )
    presses = count_presses(route, "A", marks[1], marks[-1])
    visited = presses > DIRECT_A_PRESSES
    verdict = (
        f"LIKELY visits a settings page: {presses} A presses between mark 2 and mark {len(marks)} (a direct editor to preview route has {DIRECT_A_PRESSES})"
        if visited
        else f"no page visit expected: {presses} A presses between mark 2 and mark {len(marks)}"
    )
    return visited, verdict


def default_out(route_path: Path) -> Path:
    return route_path.with_name(route_path.stem + ".splice" + route_path.suffix)


def copy_waits(route_path: Path, out: Path, force: bool) -> Path | None:
    """The `.waits` file next to the spliced route (specs are keyed by mark ordinal, which a splice keeps)."""
    waits = route_path.with_suffix(".waits")
    if not waits.is_file():
        return None
    target = out.with_suffix(".waits")
    if target.exists() and not force:
        raise RouteError(f"{target} exists (use --force)")
    lines = [
        line.replace(route_path.name, out.name) if line.lstrip().startswith("#") else line
        for line in waits.read_text(encoding="utf-8", errors="replace").splitlines()
    ]
    target.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return target


def run(args: argparse.Namespace) -> int:
    path: Path = args.route
    try:
        route = parse_route(path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, RouteError) as error:
        print(f"route_splice: {path}: {error}", file=sys.stderr)
        return 2
    if args.list:
        print(listing(route))
        return 0
    if args.check_page:
        print(check_page(route)[1])
        return 0
    if not args.cut:
        print("route_splice: give --cut START:END (or --list to see the inputs)", file=sys.stderr)
        return 2
    try:
        cuts = check_cuts([parse_cut(spec, route.polls) for spec in args.cut])
        text, notes = splice(route, cuts, args.rest_polls, path.name)
        spliced = parse_route(text)
        problems = verify(route, spliced, cuts, args.rest_polls)
    except RouteError as error:
        print(f"route_splice: {error}", file=sys.stderr)
        return 2
    if problems:
        print("route_splice: INTERNAL CHECK FAILED, nothing written:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 3
    print(
        f"route_splice T1639: {path} {route.polls} -> {spliced.polls} polls, rest gap {args.rest_polls}"
    )
    for start, end in cuts:
        removed = describe_inputs(route, start, end)
        print(
            f"  cut [{start},{end}) = {end - start} polls, buttons removed: {' '.join(removed) or '(none)'}"
        )
    print(
        "  marks: "
        + ", ".join(f"{a} -> {b}" for a, b in zip(route.marks(), spliced.marks(), strict=True))
    )
    for note in notes:
        print(f"  note: {note}")
    out = args.out or default_out(path)
    if out.resolve() == path.resolve():
        print(
            "route_splice: --out is the input file, the original is never overwritten",
            file=sys.stderr,
        )
        return 2
    if args.dry_run:
        print("dry run: nothing written")
        return 0
    if out.exists() and not args.force:
        print(f"route_splice: {out} exists (use --force)", file=sys.stderr)
        return 2
    out.write_text(text, encoding="utf-8")
    print(f"wrote {out}")
    try:
        target = copy_waits(path, out, args.force)
    except RouteError as error:
        print(f"route_splice: {error}", file=sys.stderr)
        return 2
    if target is not None:
        print(f"wrote {target}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.route_splice",
        description="Cut a poll interval (inputs included) out of a recorded route, remapping marks and nav spans (T1639).",
    )
    parser.add_argument("route", type=Path, help="the recorded route (.txt); it is never modified")
    parser.add_argument(
        "--cut",
        action="append",
        default=[],
        metavar="START:END",
        help="old poll interval to remove, END exclusive (repeatable)",
    )
    parser.add_argument(
        "--rest-polls",
        type=int,
        default=20,
        help="pad-at-rest polls inserted at each join (default 20)",
    )
    parser.add_argument(
        "--out", type=Path, help="output file (default ROUTE.splice.txt next to it)"
    )
    parser.add_argument(
        "--check-page",
        action="store_true",
        help="ADVISORY: guess from the A presses whether the route visits a settings page, print one line, exit 0",
    )
    parser.add_argument(
        "--list", action="store_true", help="print the button presses per mark segment and exit"
    )
    parser.add_argument("--dry-run", action="store_true", help="print the plan, write nothing")
    parser.add_argument(
        "--force", action="store_true", help="replace an existing output (never the original)"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.rest_polls < 0:
        print("route_splice: --rest-polls must be >= 0", file=sys.stderr)
        return 2
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
