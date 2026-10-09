# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1770: where does a route replay spend its wall time? A timeline of a host `--route-event-log` (the fish replay writes
`route-events.log` into the run folder).

  python -m tools.route_timeline tmp/owner-profiles/forced-weapons-b1-20261008-232520/route-events.log
  python -m tools.route_timeline REPLAY_LOG --ready-from tmp/owner-profiles/routes/route-20261008-232157.events
  python -m tools.route_timeline REPLAY_LOG --json

The log has one anchor per route event: `nav-start N`, `nav-ok N` (since T1770 with `menu first ready after R polls`), `wait-ok mark K
stalled-polls=S stalled-ms=M` (the mark's wait, which also marks the moment the mark was reached), `route end`. Between two anchors the time
is one of

  lead-in             boot until the first nav step (the replay plays recorded polls, the guest boots)
  recorded polls before mark K   the recorded polls between the previous step and the mark (mostly the pad at rest, the replay waits for nothing)
  wait stall mark K              the mark's wait was not yet true when the record reached the mark (a real wait: file-idle, sentinels, min=)
  recorded polls after mark K    the recorded polls between the mark and the next step (the margin route_trim keeps, plus open-loop inputs if any)
  recorded polls, steps N-M      the recorded polls between two nav steps with no mark (idle plus open-loop presses)
  nav step N          the step itself: waiting for its menu to be ready plus the presses (`ready R` = polls until the menu was input-ready)
  tail                after the last mark until the record is exhausted

`recorded polls` rows are what route_trim can shorten (the idle part), `wait stall` and `nav step` rows are waits for the guest (a stall that is only the quiet
period of a file-idle can be shortened with route_events --tune, route_trim --tune-waits). Wall ms come from the log (`t=`), polls
from `poll=`. With `--ready-from RECORDING.events` the input-ready times of the menus in the recording (real window, `nav menu=ID
cursor=C ... ready=1`) are listed next to each activation (`nav-line`): the load latency the replay cannot go below.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

_LINE = re.compile(r"t=(\d+) poll=(\d+) (\S+)(?: (.*))?")
_NAV_START = re.compile(
    r"nav-start step (\d+) \(menu=(\S+) select=(\S+?)\) at record position (\d+)"
)
_NAV_OK = re.compile(
    r"nav-ok step (\d+) done after (\d+) press\(es\), (\d+) lost, (\d+) polls: the record continues at position (\d+)(?: \(menu first ready after (\d+) polls\))?"
)
_WAIT_OK = re.compile(r"wait-ok mark (\d+) stalled-polls=(\d+) stalled-ms=(\d+)")
_MARK = re.compile(r"mark (\d+) reached record-position=(\d+)")
_NAV_MENU = re.compile(r"menu=(\S+) cursor=(\S+) count=\S+ id=\S+ ready=([01])")
_NAV_LINE = re.compile(r"at=(\d+) to=(\d+) edge=(\d+) menu=(\S+) select=(\S+)")


@dataclass
class Anchor:
    kind: str  # nav-start nav-ok mark end
    t_ms: int
    poll: int
    number: int = 0  # step or mark ordinal
    text: str = ""
    stalled_ms: int = 0
    stalled_polls: int = 0
    ready_polls: int | None = None
    step_polls: int = 0
    presses: int = 0


@dataclass
class Interval:
    label: str
    kind: str  # lead-in idle-pre-mark wait-stall idle-post-mark idle-between nav-step tail
    t_start: int
    t_end: int
    poll_start: int
    poll_end: int
    note: str = ""

    @property
    def ms(self) -> int:
        return self.t_end - self.t_start

    @property
    def polls(self) -> int:
        return self.poll_end - self.poll_start


def parse_replay_log(text: str) -> list[Anchor]:
    anchors: list[Anchor] = []
    for raw in text.splitlines():
        match = _LINE.fullmatch(raw.rstrip())
        if not match or match.group(3) != "route":
            continue
        t_ms, poll, rest = int(match.group(1)), int(match.group(2)), match.group(4) or ""
        found = _NAV_START.match(rest)
        if found:
            anchors.append(
                Anchor(
                    "nav-start",
                    t_ms,
                    poll,
                    int(found.group(1)),
                    f"{found.group(2)} {found.group(3)}",
                )
            )
            continue
        found = _NAV_OK.match(rest)
        if found:
            anchors.append(
                Anchor(
                    "nav-ok",
                    t_ms,
                    poll,
                    int(found.group(1)),
                    "",
                    presses=int(found.group(2)),
                    step_polls=int(found.group(4)),
                    ready_polls=int(found.group(6)) if found.group(6) else None,
                )
            )
            continue
        found = _WAIT_OK.match(rest)
        if found:
            anchors.append(
                Anchor(
                    "mark",
                    t_ms,
                    poll,
                    int(found.group(1)),
                    "wait",
                    stalled_polls=int(found.group(2)),
                    stalled_ms=int(found.group(3)),
                )
            )
            continue
        found = _MARK.match(rest)
        repeated = bool(found) and bool(anchors)
        repeated = (
            repeated and anchors[-1].kind == "mark" and anchors[-1].number == int(found.group(1))
        )  # type: ignore[union-attr]
        if found and not repeated:
            anchors.append(Anchor("mark", t_ms, poll, int(found.group(1)), "no wait"))
            continue
        if rest.startswith("end "):
            anchors.append(Anchor("end", t_ms, poll, 0, rest))
    return anchors


def intervals(anchors: list[Anchor]) -> list[Interval]:
    """Classified intervals between the anchors (see the module doc)."""
    out: list[Interval] = []
    previous: Anchor | None = None
    for anchor in anchors:
        if previous is None:
            if anchor.kind == "nav-start":
                out.append(
                    Interval(
                        "lead-in",
                        "lead-in",
                        0,
                        anchor.t_ms,
                        0,
                        anchor.poll,
                        "boot and the recorded idle before step 1",
                    )
                )
            previous = anchor
            continue
        if anchor.kind == "nav-ok":
            ready = (
                f"menu input-ready after {anchor.ready_polls} of {anchor.step_polls} step polls"
                if anchor.ready_polls is not None
                else f"{anchor.presses} press(es), {anchor.step_polls} step polls (host without the ready counter)"
            )
            out.append(
                Interval(
                    f"nav step {anchor.number}",
                    "nav-step",
                    previous.t_ms,
                    anchor.t_ms,
                    previous.poll,
                    anchor.poll,
                    ready,
                )
            )
        elif anchor.kind == "mark":
            stall_start_t = anchor.t_ms - anchor.stalled_ms
            stall_start_poll = anchor.poll - anchor.stalled_polls
            if previous.kind == "mark":
                out.append(
                    Interval(
                        f"recorded polls, marks {previous.number}-{anchor.number}",
                        "idle-between",
                        previous.t_ms,
                        stall_start_t,
                        previous.poll,
                        stall_start_poll,
                    )
                )
            else:
                out.append(
                    Interval(
                        f"recorded polls before mark {anchor.number}",
                        "idle-pre-mark",
                        previous.t_ms,
                        stall_start_t,
                        previous.poll,
                        stall_start_poll,
                    )
                )
            if anchor.stalled_ms > 0 or anchor.stalled_polls > 0:
                out.append(
                    Interval(
                        f"wait stall mark {anchor.number}",
                        "wait-stall",
                        stall_start_t,
                        anchor.t_ms,
                        stall_start_poll,
                        anchor.poll,
                        "the mark's wait was not yet true",
                    )
                )
        elif anchor.kind == "nav-start":
            if previous.kind == "mark":
                out.append(
                    Interval(
                        f"recorded polls after mark {previous.number}",
                        "idle-post-mark",
                        previous.t_ms,
                        anchor.t_ms,
                        previous.poll,
                        anchor.poll,
                    )
                )
            else:
                out.append(
                    Interval(
                        f"recorded polls, steps {previous.number}-{anchor.number}",
                        "idle-between",
                        previous.t_ms,
                        anchor.t_ms,
                        previous.poll,
                        anchor.poll,
                    )
                )
        elif anchor.kind == "end":
            out.append(
                Interval(
                    "tail",
                    "tail",
                    previous.t_ms,
                    anchor.t_ms,
                    previous.poll,
                    anchor.poll,
                    "recorded polls after the last mark, then the live pad",
                )
            )
        previous = anchor
    return out


def ready_latencies(text: str) -> list[str]:
    """From a RECORDING's event log: for each activation (`nav-line`, written at most ~10 polls after the A edge) the first menu with rows that
    was input-ready afterwards and the time to it (the load latency the replay cannot go below)."""
    lines: list[tuple[int, int, str, str]] = []  # t, poll, kind, rest
    for raw in text.splitlines():
        match = _LINE.fullmatch(raw.rstrip())
        if match:
            lines.append(
                (int(match.group(1)), int(match.group(2)), match.group(3), match.group(4) or "")
            )
    out: list[str] = []
    for index, (t_ms, poll, kind, rest) in enumerate(lines):
        if kind != "nav-line":
            continue
        found = _NAV_LINE.match(rest)
        if not found:
            continue
        activated = found.group(4)
        after = ""
        for t_next, poll_next, kind_next, rest_next in lines[index + 1 :]:
            if kind_next != "nav":
                continue
            menu = _NAV_MENU.fullmatch(rest_next)
            if (
                menu
                and menu.group(3) == "1"
                and menu.group(2).isdigit()
                and menu.group(1) != activated
            ):
                after = f"{menu.group(1)} input-ready {t_next - t_ms} ms / {poll_next - poll} polls later (t={t_next})"
                break
        out.append(
            f"activation of {activated} {found.group(5)} (t={t_ms}, edge poll {found.group(3)}): {after or 'no later input-ready menu in the log'}"
        )
    return out


def render(anchors: list[Anchor], rows: list[Interval]) -> str:
    lines = [f"{'what':<34} {'t start':>8} {'t end':>8} {'ms':>6} {'polls':>6}  note"]
    for row in rows:
        lines.append(
            f"{row.label:<34} {row.t_start:>8} {row.t_end:>8} {row.ms:>6} {row.polls:>6}  {row.note}"
        )
    total = sum(row.ms for row in rows)
    by_kind: dict[str, int] = {}
    for row in rows:
        by_kind[row.kind] = by_kind.get(row.kind, 0) + row.ms
    lines.append("")
    lines.append(f"total {total} ms over {len(rows)} intervals")
    for kind in (
        "lead-in",
        "idle-pre-mark",
        "wait-stall",
        "idle-post-mark",
        "idle-between",
        "nav-step",
        "tail",
    ):
        if kind in by_kind:
            lines.append(
                f"  {kind:<15} {by_kind[kind]:>7} ms  {100.0 * by_kind[kind] / max(1, total):5.1f} %"
            )
    idle = sum(ms for kind, ms in by_kind.items() if kind.startswith("idle"))
    lines.append(
        f"recorded polls (idle plus open-loop inputs, the idle part is what route_trim shortens): {idle} ms"
    )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.route_timeline",
        description="Timeline of a route replay from its --route-event-log (T1770).",
    )
    parser.add_argument("log", type=Path, help="route-events.log of a replay")
    parser.add_argument(
        "--ready-from",
        type=Path,
        help="event log of the RECORDING: list the load latency after each activation (real window timing)",
    )
    parser.add_argument("--json", action="store_true", help="machine readable intervals")
    args = parser.parse_args(argv)
    try:
        anchors = parse_replay_log(args.log.read_text(encoding="utf-8", errors="replace"))
    except OSError as error:
        print(f"route_timeline: cannot read {args.log}: {error}", file=sys.stderr)
        return 2
    if not anchors:
        print(
            f"route_timeline: {args.log}: no route events (was the replay run with --route-event-log?)",
            file=sys.stderr,
        )
        return 2
    rows = intervals(anchors)
    if args.json:
        print(
            json.dumps([asdict(row) | {"ms": row.ms, "polls": row.polls} for row in rows], indent=1)
        )
    else:
        print(render(anchors, rows))
    if args.ready_from is not None:
        try:
            latencies = ready_latencies(
                args.ready_from.read_text(encoding="utf-8", errors="replace")
            )
        except OSError as error:
            print(f"route_timeline: cannot read {args.ready_from}: {error}", file=sys.stderr)
            return 2
        print("")
        print(f"load latencies in the recording {args.ready_from.name}:")
        for line in latencies:
            print(f"  {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
