# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1633: choose the event conditions of a recorded route.

  python -m tools.route_events tmp/owner-profiles/routes                 newest route in the directory
  python -m tools.route_events tmp/owner-profiles/routes --route route-20261008-171909
  python -m tools.route_events RECORD.txt --events RUN/route-events.log --apply
  python -m tools.route_events RECORD.txt --insert 'mark2:file-open=anicemap.mkr,file-idle=800,timeout=180000'
  python -m tools.route_events RECORD.txt --check

Prints, per recorded mark, the input timeline of its segment (the polls since the previous mark), the file I/O, memory changes
and watched calls the host logged in that segment (next to the inputs, with wall times when a `--route-event-log` file exists),
the `# mark-info:` facts the recording host wrote, and the conditions that look like "this screen has finished". `--apply` writes
the proposals into the record as `# wait: markK:...` lines (a `.bak` copy is kept, marks that already have a wait are left alone).

Sources, by reliability. (1) the host's `--route-event-log FILE` (every successful open, throttled reads, sampled memory, watched
calls, with wall ms and the pad poll), (2) the `# mark-info:` lines of the record, (3) the plain host log (`kernel:` lines, no
times, only some calls are logged: a successful open or a full read is silent, so an older log shows a file only through a refused
create, a short read, a directory listing or a handle query). Nothing here is a measurement of the game by itself: a proposal is a
guess to confirm by replaying (the host prints what each condition saw, and fails with the reason if it never holds).
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path

from tools.play import route_nav, route_wait

MIN_IDLE_MS = 250
MAX_IDLE_MS = 2000
# T1770 --tune: the file-idle quiet period only has to show that the load burst ended (the next screen is gated by the nav step
# or the mark's sentinels anyway), so it is bounded by TUNE_MAX_IDLE_MS, floored by TUNE_MIN_IDLE_MS and rounded to TUNE_STEP_MS.
TUNE_MIN_IDLE_MS = 300
TUNE_MAX_IDLE_MS = 500
TUNE_STEP_MS = 50
MIN_TIMEOUT_MS = 30_000
MAX_TIMEOUT_MS = 600_000


@dataclass
class IoEvent:
    kind: str  # open read write query dir refused
    path: str
    bytes: int | None = None
    t_ms: int | None = None
    poll: int | None = None
    source: str = ""


@dataclass
class MemEvent:
    spec: str
    old: str | None
    new: str
    t_ms: int | None = None
    poll: int | None = None


@dataclass
class CallEvent:
    va: int
    count: int
    t_ms: int | None = None
    poll: int | None = None


@dataclass
class NavEvent:
    """A `nav menu=ID cursor=C count=K id=I ready=R` line of a recording's event log (T1640): the screen is on and input-ready."""

    menu: str
    cursor: str
    ready: bool
    t_ms: int | None = None
    poll: int | None = None


@dataclass
class Segment:
    number: int  # the mark that ends it
    first_poll: int
    last_poll: int  # the mark position
    io: list[IoEvent] = field(default_factory=list)
    mem: list[MemEvent] = field(default_factory=list)
    calls: list[CallEvent] = field(default_factory=list)
    nav: list[NavEvent] = field(default_factory=list)
    start_ms: int | None = None
    end_ms: int | None = None
    info: str | None = None
    logged_poll: int | None = None  # the poll the host log says the mark was recorded at


@dataclass
class Proposal:
    mark: int
    spec: str | None
    notes: list[str]


# ---- the host logs ----

_KERNEL = re.compile(
    r'kernel: (Nt(?:Create|Open|Read|Write|QueryInformation|QueryDirectory)File)\("((?:[^"\\]|\\.)*)"'
)
_QUERY_DIR_NAME = re.compile(r'\) -> "([^"]+)" (file|directory)')
_TRANSFERRED = re.compile(r"transferred (\d+)")
_MARK_RECORDING = re.compile(r"input recording: mark (\d+) at poll (\d+)")
_MARK_REPLAY = re.compile(r"route: mark (\d+) reached at host poll (\d+), record position (\d+)")
_EVENT_LINE = re.compile(r"t=(\d+) poll=(\d+) (\S+)(?: (.*))?")
_NAV_LINE = re.compile(r"menu=(\S+) cursor=(\S+) count=\S+ id=\S+ ready=([01])")


def parse_host_log(path: Path) -> tuple[list[tuple[str, object]], dict[int, int]]:
    """(ordered items ('io', IoEvent) / ('mark', K), logged mark polls) of a plain host stdout/stderr log."""
    items: list[tuple[str, object]] = []
    marks: dict[int, int] = {}
    with path.open(encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            match = _MARK_RECORDING.match(line)
            if match:
                number = int(match.group(1))
                marks[number] = int(match.group(2))
                items.append(("mark", number))
                continue
            match = _MARK_REPLAY.match(line)
            if match:
                number = int(match.group(1))
                marks.setdefault(number, int(match.group(3)))
                items.append(("mark", number))
                continue
            match = _KERNEL.match(line)
            if not match:
                continue
            call, target = match.group(1), match.group(2)
            rest = line[match.end() :]
            if call == "NtCreateFile" or call == "NtOpenFile":
                kind = "refused" if "REFUSED" in rest else "open"
                if "a REAL directory" in rest or "directory that already existed" in rest:
                    continue
                items.append(("io", IoEvent(kind, target, source=path.name)))
            elif call == "NtReadFile":
                transferred = _TRANSFERRED.search(rest)
                items.append(
                    (
                        "io",
                        IoEvent(
                            "read",
                            target,
                            int(transferred.group(1)) if transferred else None,
                            source=path.name,
                        ),
                    )
                )
            elif call == "NtWriteFile":
                items.append(("io", IoEvent("write", target, source=path.name)))
            elif call == "NtQueryInformationFile":
                size = re.search(r"-> (\d+) bytes", rest)
                items.append(
                    (
                        "io",
                        IoEvent(
                            "query", target, int(size.group(1)) if size else None, source=path.name
                        ),
                    )
                )
            elif call == "NtQueryDirectoryFile":
                name = _QUERY_DIR_NAME.search(rest)
                if name and name.group(2) == "directory":
                    items.append(
                        (
                            "io",
                            IoEvent(
                                "dir", target.rstrip("\\") + "\\" + name.group(1), source=path.name
                            ),
                        )
                    )
    return items, marks


def parse_event_log(path: Path) -> list[tuple[str, object]]:
    """Ordered items of a host `--route-event-log` file: ('io'|'mem'|'call'|'mark', object)."""
    items: list[tuple[str, object]] = []
    with path.open(encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if line.startswith("#"):
                continue
            match = _EVENT_LINE.fullmatch(line)
            if not match:
                continue
            t_ms, poll, kind, rest = (
                int(match.group(1)),
                int(match.group(2)),
                match.group(3),
                match.group(4) or "",
            )
            if kind == "open":
                items.append(("io", IoEvent("open", rest, None, t_ms, poll, path.name)))
            elif kind in ("read", "write"):
                fields = re.fullmatch(r"(.*) (?:reads|writes)=(\d+) bytes=(\d+)", rest)
                if fields:
                    items.append(
                        (
                            "io",
                            IoEvent(
                                kind, fields.group(1), int(fields.group(3)), t_ms, poll, path.name
                            ),
                        )
                    )
            elif kind == "call":
                fields = re.fullmatch(r"0x([0-9A-Fa-f]+) n=(\d+)", rest)
                if fields:
                    items.append(
                        (
                            "call",
                            CallEvent(int(fields.group(1), 16), int(fields.group(2)), t_ms, poll),
                        )
                    )
            elif kind == "mem":
                changed = re.fullmatch(
                    r"(\S+) (0x[0-9A-Fa-f]+|value) -> (0x[0-9A-Fa-f]+|unreadable)", rest
                )
                first = re.fullmatch(r"(\S+) = (0x[0-9A-Fa-f]+)", rest)
                if changed:
                    items.append(
                        (
                            "mem",
                            MemEvent(
                                changed.group(1),
                                None if changed.group(2) == "value" else changed.group(2),
                                changed.group(3),
                                t_ms,
                                poll,
                            ),
                        )
                    )
                elif first:
                    items.append(
                        ("mem", MemEvent(first.group(1), None, first.group(2), t_ms, poll))
                    )
            elif kind == "mark":
                fields = re.match(r"(\d+)", rest)
                if fields:
                    items.append(("mark", (int(fields.group(1)), t_ms, poll)))
            elif kind == "nav":
                fields = _NAV_LINE.fullmatch(rest)
                if fields:
                    items.append(
                        (
                            "nav",
                            NavEvent(
                                fields.group(1), fields.group(2), fields.group(3) == "1", t_ms, poll
                            ),
                        )
                    )
            elif kind == "route":
                fields = re.match(r"mark (\d+) reached", rest)
                if fields:
                    items.append(("mark", (int(fields.group(1)), t_ms, poll)))
    return items


# ---- segments ----


def build_segments(
    marks: list[int], items: list[tuple[str, object]], infos: dict[int, str], logged: dict[int, int]
) -> list[Segment]:
    """One Segment per recorded mark: the events logged between the previous mark line and this one."""
    segments: list[Segment] = []
    previous = 0
    for index, position in enumerate(marks, 1):
        segments.append(
            Segment(index, previous, position, info=infos.get(index), logged_poll=logged.get(index))
        )
        previous = position
    current = 0  # index of the segment collecting events
    for kind, payload in items:
        if kind == "mark":
            number, t_ms = (payload, None) if isinstance(payload, int) else (payload[0], payload[1])
            if 1 <= number <= len(segments):
                segments[number - 1].end_ms = t_ms
                if number < len(segments):
                    segments[number].start_ms = t_ms
            current = max(current, number)
            continue
        if current >= len(segments):
            continue  # after the last mark: not part of any wait
        segment = segments[current]
        if kind == "io":
            segment.io.append(payload)  # type: ignore[arg-type]
        elif kind == "mem":
            segment.mem.append(payload)  # type: ignore[arg-type]
        elif kind == "call":
            segment.calls.append(payload)  # type: ignore[arg-type]
        elif kind == "nav":
            segment.nav.append(payload)  # type: ignore[arg-type]
    return segments


def basename(path: str) -> str:
    return re.split(r"[\\/]", path.rstrip("\\/"))[-1]


def is_disc(path: str) -> bool:
    lowered = path.lower()
    return (
        lowered.startswith("\\??\\d:")
        or lowered.startswith("d:")
        or "\\pak\\" in lowered
        or lowered.endswith(".pak")
    )


def is_directory_path(event: IoEvent) -> bool:
    return event.kind == "dir"


def volume_filter(path: str) -> str:
    """The path filter for a file-idle condition: the save volume of the chosen file (`@u:\\` for `\\??\\U:\\...`), else
    everything that is not the disc (`@!d:\\`). The disc streams paks and music through every menu (MEASURED, the first headless
    route run: file I/O every ~50 ms), so an unfiltered idle never holds."""
    match = re.match(r"\\\?\?\\([A-Za-z]):", path)
    if match and match.group(1).lower() != "d":
        return f"@{match.group(1).lower()}:\\"
    return "@!d:\\"


def parse_info(text: str | None) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in (text or "").split():
        key, _, value = token.partition("=")
        result[key] = value
    return result


def propose(segment: Segment, earlier_paths: set[str], tune: bool = False) -> Proposal:
    """The conditions that look like the end of this segment's screen, with the reasons."""
    notes: list[str] = []
    fields: list[str] = []
    info = parse_info(segment.info)
    # 1. a save file (not disc streaming) touched for the first time in this segment
    fresh: list[IoEvent] = []
    seen_names: set[str] = set()
    for event in segment.io:
        if event.kind in ("refused", "dir") or is_disc(event.path):
            continue
        name = basename(event.path).lower()
        if name in seen_names or name in earlier_paths:
            continue
        seen_names.add(name)
        fresh.append(event)
    informative = [
        event
        for event in fresh
        if basename(event.path).lower()
        not in ("savemeta.xbx", "titlemeta.xbx", "titleimage.xbx", "saveimage.xbx")
    ]
    chosen = (informative or fresh)[-1:] if (informative or fresh) else []
    if chosen:
        name = basename(chosen[0].path).lower()
        if "," not in name and 0 < len(name) < route_wait.ROUTE_COND_TEXT:
            fields.append(f"file-open={name}")
            notes.append(
                f"file-open={name}: first touched in this segment ({chosen[0].kind}, {chosen[0].source})"
            )
    elif segment.io:
        notes.append(
            "file I/O was logged but only disc streaming, directory listings or files seen earlier: no file-open candidate"
        )
    else:
        notes.append(
            "no file I/O logged in this segment: a file condition is not possible from this log"
        )
    # 2. quiet period of the save volume (the disc streams all the time), from the recording host's facts or from times in the event log
    idle_ms: int | None = None
    hdd_quiet = info.get("hdd-last-io-ago-ms")
    if hdd_quiet is not None and info.get("hdd-io", "0") != "0":
        idle_ms = int(hdd_quiet)
        notes.append(
            f"the save volume had been quiet for {idle_ms} ms when the mark was recorded (mark-info)"
        )
    elif "last-io-ago-ms" in info and info.get("io", "0") != "0" and "hdd-io" not in info:
        idle_ms = int(info["last-io-ago-ms"])
        notes.append(
            f"I/O (all files) had been quiet for {idle_ms} ms when the mark was recorded (mark-info, an older host)"
        )
    else:
        timed = [
            event for event in segment.io if event.t_ms is not None and not is_disc(event.path)
        ]
        if timed and segment.end_ms is not None:
            idle_ms = segment.end_ms - max(event.t_ms or 0 for event in timed)
            notes.append(f"the last save volume I/O was {idle_ms} ms before the mark (event log)")
    if idle_ms is not None and idle_ms >= MIN_IDLE_MS * 2:
        scope = volume_filter(chosen[0].path) if chosen else "@!d:\\"
        proposed_idle = min(MAX_IDLE_MS, max(MIN_IDLE_MS, idle_ms // 2))
        if tune:
            lag = settle_lag(segment, scope)
            proposed_idle = tuned_idle_ms(lag.lag_ms if lag else None, proposed_idle)
            notes.append(
                f"--tune: file-idle {proposed_idle} ms"
                + (
                    f" (the last {scope} I/O was {lag.lag_ms} ms before {lag.menu} was input-ready in the recording)"
                    if lag
                    else f" (no settle lag in the event log, cap {TUNE_MAX_IDLE_MS} ms)"
                )
            )
        fields.append(f"file-idle={proposed_idle}{scope}")
    elif idle_ms is not None:
        notes.append(
            "the mark followed the last I/O closely, so no file-idle is proposed (it would hold only after the owner's reaction time)"
        )
    # 3. memory that changed in the segment (last change per sampled item)
    last_change: dict[str, MemEvent] = {}
    for event in segment.mem:
        if event.new != "unreadable":
            last_change[event.spec] = event
    for spec, event in last_change.items():
        if (
            event.old is not None
            and event.t_ms is not None
            and segment.end_ms is not None
            and segment.end_ms - event.t_ms > 60_000
        ):
            continue
        if spec.startswith("*") and event.new in ("0x0", "0"):
            # T1639: a pointer-indirect value that fell back to 0 is a recycled object (the editor list at the preview flip): a
            # wait on it would read through a stale pointer, so it is not proposed (the mode sentinel carries the screen)
            notes.append(
                f"mem={spec}=={event.new} not proposed: pointer-indirect value back to 0 (recycled object)"
            )
            continue
        if event.old is not None:
            fields.append(f"mem={spec}=={event.new}")
            notes.append(
                f"mem={spec}=={event.new}: changed {event.old} -> {event.new} in this segment (sampled, confirm it is a screen sentinel and does not change back)"
            )
    # 4. watched calls
    for call in segment.calls:
        notes.append(f"call=0x{call.va:X} was reached in this segment (count {call.count} logged)")
    seen_calls: set[int] = set()
    for call in segment.calls:
        if call.va in seen_calls:
            continue
        seen_calls.add(call.va)
        fields.append(f"call=0x{call.va:X}")
    if not fields:
        notes.append(
            "nothing to wait on: keep the time based wait (min=N) or record with --route-event-log and --route-log-mem"
        )
        return Proposal(segment.number, None, notes)
    seconds = None
    if info.get("seg-ms", "").isdigit():
        seconds = int(info["seg-ms"])
    elif segment.start_ms is not None and segment.end_ms is not None:
        seconds = segment.end_ms - segment.start_ms
    timeout = (
        MAX_TIMEOUT_MS
        if seconds is None
        else min(MAX_TIMEOUT_MS, max(MIN_TIMEOUT_MS, seconds * 3 + 15_000))
    )
    fields.append(f"timeout={timeout}")
    return Proposal(segment.number, f"mark{segment.number}:" + ",".join(fields), notes)


# ---- T1770: tuned file-idle ----


def scope_matches(path: str, scope: str) -> bool:
    """The host's file-idle filter on a guest path: `@SUBSTR` case blind substring, `@!SUBSTR` the paths without it, empty = all."""
    wanted = scope.lstrip("@").lower()
    if not wanted:
        return True
    lowered = path.lower()
    if wanted.startswith("!"):
        return wanted[1:] not in lowered
    return wanted in lowered


@dataclass
class SettleLag:
    """How long after the segment's last matching file I/O the first input-ready menu with rows appeared (recording facts)."""

    last_io_ms: int
    last_io_path: str
    ready_ms: int
    menu: str

    @property
    def lag_ms(self) -> int:
        return self.ready_ms - self.last_io_ms


def settle_lag(segment: Segment, scope: str) -> SettleLag | None:
    """The settle lag of a segment for a file-idle scope, from the event log of the RECORDING (real timing). None when the log has no
    timed I/O matching the scope or no input-ready menu with a cursor after it inside the segment."""
    timed = [
        event
        for event in segment.io
        if event.t_ms is not None
        and event.kind in ("open", "read", "write")
        and scope_matches(event.path, scope)
    ]
    if not timed:
        return None
    last = max(timed, key=lambda event: event.t_ms or 0)
    last_ms = last.t_ms or 0
    for nav in segment.nav:
        if nav.t_ms is None or nav.t_ms <= last_ms or not nav.ready:
            continue
        # '-' = a page without rows (a transient page); 4294967295 (no row under the cursor) is still a menu with rows
        if not nav.cursor.isdigit():
            continue
        return SettleLag(last_ms, last.path, nav.t_ms, nav.menu)
    return None


def tuned_idle_ms(
    lag_ms: int | None,
    old_ms: int | None = None,
    cap_ms: int = TUNE_MAX_IDLE_MS,
    floor_ms: int = TUNE_MIN_IDLE_MS,
) -> int:
    """The tuned file-idle: half the measured settle lag rounded up to TUNE_STEP_MS, clamped to [floor, cap]; with no lag evidence the
    cap. Never above the existing value (tuning only shortens a wait)."""
    base = cap_ms
    if lag_ms is not None:
        half = max(0, lag_ms) // 2
        base = -(-half // TUNE_STEP_MS) * TUNE_STEP_MS
    value = max(floor_ms, min(cap_ms, base))
    if old_ms is not None:
        value = min(value, old_ms)
    return value


_IDLE_FIELD = re.compile(r"(?<![\w-])file-idle=(\d+)(@[^,\s]*)?")


@dataclass
class TunedWait:
    mark: int
    old: str
    new: str
    note: str

    @property
    def changed(self) -> bool:
        return self.old != self.new


def tune_wait_spec(
    spec: str,
    segment: Segment | None,
    cap_ms: int = TUNE_MAX_IDLE_MS,
    floor_ms: int = TUNE_MIN_IDLE_MS,
) -> TunedWait | None:
    """Shorten the `file-idle=MS[@FILTER]` of one `markK:fields` spec. None when the spec has no file-idle."""
    head = re.match(r"\s*mark(\d+):", spec)
    found = _IDLE_FIELD.search(spec)
    if head is None or found is None:
        return None
    mark = int(head.group(1))
    old_ms = int(found.group(1))
    scope = found.group(2) or ""
    lag = settle_lag(segment, scope) if segment is not None else None
    new_ms = tuned_idle_ms(lag.lag_ms if lag else None, old_ms, cap_ms, floor_ms)
    if lag is not None:
        note = (
            f"mark {mark}: the last {scope or 'any'} I/O ({basename(lag.last_io_path)}) was {lag.lag_ms} ms before the menu "
            f"{lag.menu} was input-ready in the recording (t={lag.last_io_ms} -> {lag.ready_ms}), file-idle {old_ms} -> {new_ms} ms"
        )
    else:
        note = f"mark {mark}: no settle lag in the recording's event log, file-idle {old_ms} -> {new_ms} ms (cap {cap_ms})"
    new_spec = spec[: found.start()] + f"file-idle={new_ms}{scope}" + spec[found.end() :]
    return TunedWait(mark, spec, new_spec, note)


def tune_record_text(
    text: str,
    segments: list[Segment],
    cap_ms: int = TUNE_MAX_IDLE_MS,
    floor_ms: int = TUNE_MIN_IDLE_MS,
) -> tuple[str, list[TunedWait]]:
    """The record text with every `# wait:` line's file-idle tuned (other lines byte identical), and what changed."""
    by_mark = {segment.number: segment for segment in segments}
    out: list[str] = []
    results: list[TunedWait] = []
    for line in text.splitlines(keepends=True):
        if line.startswith("# wait:"):
            body = line[7:].strip()
            head = re.match(r"mark(\d+):", body)
            tuned = tune_wait_spec(
                body, by_mark.get(int(head.group(1))) if head else None, cap_ms, floor_ms
            )
            if tuned is not None:
                results.append(tuned)
                if tuned.changed:
                    ending = line[len(line.rstrip("\r\n")) :]
                    line = f"# wait: {tuned.new}{ending}"
        out.append(line)
    return "".join(out), results


# ---- the record ----


def input_runs(path: Path) -> list[tuple[int, int, str]]:
    """(first poll, last poll, tokens) of each run of the record body."""
    runs: list[tuple[int, int, str]] = []
    poll = 0
    with path.open(encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if not parts[0].isdigit():
                continue
            count = int(parts[0])
            runs.append((poll, poll + count - 1, " ".join(parts[1:]) or "rest"))
            poll += count
    return runs


def find_record(location: Path, name: str | None) -> Path:
    if location.is_file():
        return location
    candidates = [
        entry
        for entry in location.glob("*.txt")
        if entry.read_text(errors="replace")[:20].startswith("# tsfp-input")
    ]
    if name:
        candidates = [entry for entry in candidates if entry.stem == name or entry.name == name]
    if not candidates:
        raise SystemExit(
            f"{location}: no input record (a '# tsfp-input' file){' named ' + name if name else ''}"
        )
    return max(candidates, key=lambda entry: (entry.stat().st_mtime, entry.name))


def sibling_logs(record: Path) -> list[Path]:
    names = [
        f"{record.stem}.events",
        f"{record.stem}.event-log",
        f"{record.stem}.log",
        f"{record.stem}.stdout",
    ]
    return [record.parent / name for name in names if (record.parent / name).is_file()]


def collect(
    record: Path, events: list[Path], logs: list[Path]
) -> tuple[route_wait.RecordInfo, list[Segment], list[Path]]:
    info = route_wait.read_record(record)
    items: list[tuple[str, object]] = []
    logged: dict[int, int] = {}
    used: list[Path] = []
    for path in events:
        items = parse_event_log(path)
        used.append(path)
        break
    if not items:
        for path in logs:
            log_items, marks = parse_host_log(path)
            if any(kind == "io" for kind, _ in log_items):
                items, logged = log_items, marks
                used.append(path)
                break
    segments = build_segments(info.marks, items, info.infos, logged)
    return info, segments, used


def nav_lines(
    steps: list[tuple[int, route_nav.NavStep]], first: int, last: int | None
) -> list[str]:
    """T1640: the `# nav:` steps whose range starts in the polls [first, last) (`last` None = to the end of the record)."""
    return [
        f"  nav  line {number}: polls {step.at}..{step.to} menu={step.menu} select={step.select_text}"
        + (" activate" if step.activate else "")
        + (f" expect={step.expect}" if step.expect else "")
        for number, step in steps
        if step.at >= first and (last is None or step.at < last)
    ]


def render(
    record: Path,
    info: route_wait.RecordInfo,
    segments: list[Segment],
    used: list[Path],
    full: bool,
    tune: bool = False,
) -> tuple[str, list[Proposal]]:
    lines = [
        f"route {record}  polls {info.polls}  marks {len(info.marks)}  existing waits {len(info.waits)}"
    ]
    lines.append(
        "sources: "
        + (
            ", ".join(path.name for path in used)
            if used
            else "none (no event log or host log found next to the record)"
        )
    )
    runs = input_runs(record)
    navs = route_nav.scan_record(record.read_text(encoding="utf-8", errors="replace")).steps
    proposals: list[Proposal] = []
    earlier: set[str] = set()
    for segment in segments:
        lines.append("")
        span = f"polls {segment.first_poll}..{segment.last_poll}"
        timing = ""
        if segment.start_ms is not None and segment.end_ms is not None:
            timing = f", {(segment.end_ms - segment.start_ms) / 1000:.1f} s of wall time"
        lines.append(
            f"== segment {segment.number}: {span} (mark {segment.number} at poll {segment.last_poll}{timing}) =="
        )
        shown = [
            run for run in runs if run[1] >= segment.first_poll and run[0] <= segment.last_poll
        ]
        taps = [run for run in shown if run[2] != "rest"]
        lines.append(
            f"  inputs: {len(taps)} button run(s) in {len(shown)} run(s)"
            + (
                ": "
                + ", ".join(f"{a}-{b} {tokens}" for a, b, tokens in taps[:12])
                + (" ..." if len(taps) > 12 else "")
                if taps
                else ""
            )
        )
        lines += nav_lines(navs, segment.first_poll, segment.last_poll)
        if full:
            for first, last, tokens in shown:
                lines.append(f"      poll {first:5d}-{last:5d}  {tokens}")
        if segment.info:
            lines.append(f"  mark-info (recording host): {segment.info}")
        shown_io = 0
        for event in segment.io:
            if shown_io >= (400 if full else 14):
                lines.append(f"  ... {len(segment.io) - shown_io} more file events (use --full)")
                break
            when = f"t={event.t_ms:>7} poll={event.poll} " if event.t_ms is not None else ""
            amount = f" {event.bytes} bytes" if event.bytes is not None else ""
            lines.append(
                f"  io   {when}{event.kind:<8}{basename(event.path) if not full else event.path}{amount}"
            )
            shown_io += 1
        for event in segment.mem:
            when = f"t={event.t_ms:>7} poll={event.poll} " if event.t_ms is not None else ""
            lines.append(f"  mem  {when}{event.spec} {event.old or '(first)'} -> {event.new}")
        for call in segment.calls:
            lines.append(f"  call t={call.t_ms} poll={call.poll} 0x{call.va:X} n={call.count}")
        proposal = propose(segment, earlier, tune)
        proposals.append(proposal)
        for event in segment.io:
            if event.kind not in (
                "refused",
                "dir",
            ):  # a refused create or a listing did not touch the file
                earlier.add(basename(event.path).lower())
        for note in proposal.notes:
            lines.append(f"  note: {note}")
        lines.append(f"  PROPOSED: {proposal.spec}" if proposal.spec else "  PROPOSED: (none)")
    tail = nav_lines(navs, segments[-1].last_poll if segments else 0, None)
    if tail:
        lines += ["", "== after the last mark =="] + tail
    return "\n".join(lines), proposals


def insert_waits(record: Path, specs: list[str], force: bool = False) -> list[str]:
    """Write `# wait:` lines into the record (after the mark or its mark-info line). Returns what was written."""
    parsed = []
    for spec in specs:
        wait = route_wait.parse_wait(spec, event=True)  # raises WaitError, nothing is written then
        parsed.append((wait.mark, spec))
    lines = record.read_text(encoding="utf-8", errors="replace").splitlines(keepends=True)
    existing = {
        route_wait.parse_wait(text, event=True).mark
        for _, text in route_wait.read_record(record).waits
        if text.startswith("mark")
    }
    marks_in_file = len(route_wait.read_record(record).marks)
    written: list[str] = []
    for mark, spec in parsed:
        if mark > marks_in_file:
            raise ValueError(f"mark{mark}: the record has {marks_in_file} mark(s)")
        if mark in existing and not force:
            continue
        if mark in existing:
            lines = [
                line
                for line in lines
                if not (line.startswith("# wait:") and line[7:].strip().startswith(f"mark{mark}:"))
            ]
        counter = 0
        position = None
        for index, line in enumerate(lines):
            if line.startswith("# mark:"):
                counter += 1
                if counter == mark:
                    position = index + 1
                    if position < len(lines) and lines[position].startswith(
                        f"# mark-info: mark{mark} "
                    ):
                        position += 1
                    break
        if position is None:
            raise ValueError(f"mark{mark}: not found in the record")
        lines.insert(position, f"# wait: {spec}\n")
        written.append(spec)
    if written:
        shutil.copyfile(record, record.with_name(record.name + ".t1633.bak"))
        record.write_text("".join(lines), encoding="utf-8")
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.route_events",
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "path",
        type=Path,
        help="a route record file, or a directory holding route-*.txt (newest is used)",
    )
    parser.add_argument("--route", help="record name (stem) to pick inside a directory")
    parser.add_argument(
        "--events",
        type=Path,
        action="append",
        default=[],
        help="host --route-event-log file(s); default: siblings of the record",
    )
    parser.add_argument(
        "--log",
        type=Path,
        action="append",
        default=[],
        help="plain host stdout/stderr log(s); default: siblings of the record",
    )
    parser.add_argument(
        "--full", action="store_true", help="every input run and file event, full paths"
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="write the proposals as '# wait:' lines (marks that have one are left alone; --force replaces)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="with --apply or --insert: replace an existing wait for the mark",
    )
    parser.add_argument(
        "--insert",
        action="append",
        default=[],
        metavar="SPEC",
        help="write this wait into the record (markK:fields), validated first",
    )
    parser.add_argument(
        "--tune",
        action="store_true",
        help="T1770: shorten file-idle (proposals and the record's existing '# wait:' lines) to the cap, evidence from the nav ready lines of the recording's event log",
    )
    parser.add_argument(
        "--idle-cap-ms",
        type=int,
        default=TUNE_MAX_IDLE_MS,
        help=f"--tune: largest file-idle (default {TUNE_MAX_IDLE_MS})",
    )
    parser.add_argument(
        "--idle-floor-ms",
        type=int,
        default=TUNE_MIN_IDLE_MS,
        help=f"--tune: smallest file-idle (default {TUNE_MIN_IDLE_MS})",
    )
    parser.add_argument(
        "--out",
        type=Path,
        help="--tune --apply: write the tuned record here (the original stays untouched); default is in place with a .t1770.bak backup",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="validate the record's waits (same checks as tools.play --check-route), exit 2 on a problem",
    )
    args = parser.parse_args(argv)
    record = find_record(args.path, args.route)
    if args.insert:
        try:
            written = insert_waits(record, args.insert, args.force)
        except (route_wait.WaitError, ValueError) as error:
            print(f"route_events: {error}", file=sys.stderr)
            return 2
        print(
            f"route_events: wrote {len(written)} wait(s) into {record}"
            + (
                f" (backup {record.name}.t1633.bak)"
                if written
                else " (every named mark already has a wait, use --force)"
            )
        )
        return 0
    if args.check:
        problems = route_wait.validate(
            route_wait.read_record(record), [], ["--headless-second-vblank"]
        )
        for problem in problems:
            print(f"route_events: {problem}", file=sys.stderr)
        print(f"route_events: {record}: {len(problems)} problem(s) in the waits")
        return 2 if problems else 0
    events = args.events or [
        path for path in sibling_logs(record) if path.suffix in (".events", ".event-log")
    ]
    logs = args.log or [path for path in sibling_logs(record) if path.suffix in (".log", ".stdout")]
    info, segments, used = collect(record, events, logs)
    text, proposals = render(record, info, segments, used, args.full, args.tune)
    print(text)
    tuned_results: list[TunedWait] = []
    tuned_text: str | None = None
    if args.tune:
        if args.idle_cap_ms < args.idle_floor_ms or args.idle_floor_ms < 0:
            print("route_events: --idle-cap-ms must be >= --idle-floor-ms >= 0", file=sys.stderr)
            return 2
        tuned_text, tuned_results = tune_record_text(
            record.read_text(encoding="utf-8", errors="replace"),
            segments,
            args.idle_cap_ms,
            args.idle_floor_ms,
        )
        print("")
        print("--tune: existing waits of the record")
        if not tuned_results:
            print("  (no '# wait:' line has a file-idle)")
        for result in tuned_results:
            print(f"  {result.note}")
            if result.changed:
                print(f"    was: {result.old}")
                print(f"    now: {result.new}")
    chosen = [proposal.spec for proposal in proposals if proposal.spec]
    print("")
    if chosen:
        print("proposed waits (paste into the record as '# wait: ...' lines, or pass --apply):")
        for spec in chosen:
            print(f"  {spec}")
    else:
        print(
            "no event condition could be proposed from these logs; record again with --route-event-log (tmp/record_mapmaker_route.fish does)"
        )
    target = record
    if args.apply and args.tune and tuned_text is not None:
        if args.out is not None:
            if args.out.resolve() == record.resolve():
                print("route_events: --out is the input record", file=sys.stderr)
                return 2
            if args.out.exists() and not args.force:
                print(f"route_events: {args.out} exists (use --force)", file=sys.stderr)
                return 2
            args.out.write_text(tuned_text, encoding="utf-8")
            target = args.out
            print(f"route_events: wrote the tuned record {args.out} (original untouched)")
        elif tuned_text != record.read_text(encoding="utf-8", errors="replace"):
            shutil.copyfile(record, record.with_name(record.name + ".t1770.bak"))
            record.write_text(tuned_text, encoding="utf-8")
            print(f"route_events: tuned {record} in place (backup {record.name}.t1770.bak)")
    if args.apply and chosen:
        try:
            written = insert_waits(target, [spec for spec in chosen if spec], args.force)
        except (route_wait.WaitError, ValueError) as error:
            print(f"route_events: {error}", file=sys.stderr)
            return 2
        print(
            f"route_events: wrote {len(written)} wait(s) into {target}"
            + (
                f" (backup {target.name}.t1633.bak)"
                if written
                else " (all marks already had a wait)"
            )
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
