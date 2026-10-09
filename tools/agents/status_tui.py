# SPDX-License-Identifier: GPL-3.0-or-later
"""Interactive curses TUI behind `python3 -m tools.agents.status --watch N`.

Strictly read-only: it reads the claims (via `coord list --json`), the registered sessions,
the coord events log (audit and history, through the pure helpers of `coord.py`), the message
log, the fleet state file, the roadmap, the task ledger and the badge metrics, and writes
nothing.
Rendering is pure (`compose_frame` returns styled text lines), so tests drive it with
fake data and tiny sizes. Data is gathered on a worker thread, so a slow `coord list`
never delays key handling.
"""

import argparse
import curses
import json
import os
import re
import textwrap
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

from tools.agents import coord, fleet, status, taskmeta
from tools.agents import roadmap as roadmap_doc

TABS = ["Dashboard", "Milestones", "Tasks", "Sessions", "Workers", "Messages", "Progress", "Audit"]
TASKS_TAB = 2
SESSIONS_TAB = 3
AUDIT_TAB = 7
SELECT_TABS = (TASKS_TAB, AUDIT_TAB)  # tabs with a selectable row list and a detail view
DASHBOARD_BLOCKED_LIMIT = 6
DASHBOARD_READY_LIMIT = 5
MESSAGE_LIMIT = 200
TASK_ENTRY = re.compile(r"^- \[(.)\] \*\*(T\d+[a-z]?)\.?\s*(.*)$")
STATE_STYLE = {
    "active": "ok",
    "done": "accent",
    "silent": "bad",
    "STALE": "bad",
    "blocked": "bad",
    "pending": "dim",
    "idle": "dim",
    "ended": "dim",
    "live": "ok",
    status.NO_REPORT: "bad",
    "claimed": "ok",
    "in-flight": "ok",
    "open": "",
}
HELP = [
    "Keys",
    "  1-8            jump to a tab",
    "  Tab / Right    next tab,  Left / Shift-Tab  previous tab",
    "  Up Down        scroll (Tasks, Audit: move selection)",
    "  PgUp PgDn      scroll a page,  Home / End  top / bottom",
    "  /              filter tasks (Enter keeps it, Esc clears it)",
    "  Enter          Tasks, Audit: show the selected row with its history, Esc closes it",
    "  p              in a detail view: show or hide progress events in the history",
    "  a              Sessions: show or hide ended sessions (hidden by default)",
    "  r              refresh now",
    "  ?              this help,  q  quit",
]


@dataclass
class TaskEntry:
    ident: str
    char: str
    title: str
    body: list[str] = field(default_factory=list)


@dataclass
class Message:
    stamp: str
    sender: str
    to: str
    text: str


@dataclass
class Data:
    claims: list[status.Claim] | None = None
    registered: dict[str, dict] | None = None
    ledger: taskmeta.Ledger | None = None
    events: list[dict] | None = None
    audits: dict[str, dict] | None = None
    rollups: dict[str, status.Rollup] | None = None
    roadmap: roadmap_doc.Roadmap | None = None
    states: dict[str, str] | None = None
    metrics: dict | None = None
    counts: tuple[int, int] | None = None
    tasks: list[TaskEntry] | None = None
    messages: list[Message] | None = None
    fleet: dict | None = None
    errors: dict[str, str] = field(default_factory=dict)
    fetched_at: float = 0.0
    session_stale: int = status.SESSION_STALE_SECONDS


@dataclass
class View:
    tab: int = 0
    scroll: list[int] = field(default_factory=lambda: [0] * len(TABS))
    selected: int = 0
    audit_selected: int = 0
    show_progress: bool = False
    show_ended: bool = False
    filter: str = ""
    editing: bool = False
    detail: bool = False
    help: bool = False
    interval: int = 5
    refreshing: bool = False


Line = tuple[str, str]


def parse_task_entries(text: str) -> list[TaskEntry]:
    """Ledger entries with their indented body lines, in file order."""
    entries: list[TaskEntry] = []
    current: TaskEntry | None = None
    for line in text.splitlines():
        match = TASK_ENTRY.match(line)
        if match:
            title = match.group(3).split("**", 1)[0].strip()
            current = TaskEntry(match.group(2), match.group(1), title, [match.group(3)])
            entries.append(current)
        elif line.startswith(("- ", "#")):
            current = None
        elif current is not None and line.strip():
            current.body.append(line.strip())
    return entries


def parse_messages(text: str, limit: int = MESSAGE_LIMIT) -> list[Message]:
    messages = []
    for line in text.splitlines():
        try:
            raw = json.loads(line)
            stamp = time.strftime("%H:%M:%S", time.gmtime(raw["ts"]))
            messages.append(Message(stamp, str(raw["from"]), str(raw["to"]), str(raw["text"])))
        except (ValueError, KeyError, TypeError):
            continue
    return messages[-limit:]


def gather(args: argparse.Namespace) -> Data:
    """Read every source; a failing source is recorded in `errors`, never raised."""
    data = Data(
        fetched_at=time.time(),
        session_stale=getattr(args, "session_stale", status.SESSION_STALE_SECONDS),
    )

    def attempt(name: str, action: Callable[[], object]) -> None:
        try:
            action()
        except Exception as error:  # shown in the pane, the TUI must keep running
            data.errors[name] = f"{type(error).__name__}: {error}"

    def claims() -> None:
        data.claims = status.fetch_claims(args.stale)

    def roadmap() -> None:
        data.roadmap, data.states = roadmap_doc.load(args.roadmap, args.tasks)

    def tasks() -> None:
        text = args.tasks.read_text()
        data.tasks = parse_task_entries(text)
        data.counts = status.count_tasks(args.tasks)
        data.ledger = taskmeta.parse(text)

    def sessions() -> None:
        data.registered = status.read_sessions()

    def audit() -> None:
        data.events = status.fetch_events()
        data.audits = status.task_audits(data.events, data.claims or [], data.fetched_at)
        if data.roadmap is not None and data.states is not None:
            data.rollups = status.milestone_rollups(
                data.roadmap, data.audits, data.events, data.states, data.fetched_at
            )

    def metrics() -> None:
        data.metrics = status.load_metrics(args.metrics)

    def messages() -> None:
        log = coord.coord_dir() / "messages.jsonl"
        data.messages = parse_messages(log.read_text()) if log.exists() else []

    def fleet_state() -> None:
        data.fleet = fleet.load_state()

    attempt("claims", claims)
    attempt("sessions", sessions)
    if not args.no_roadmap:
        attempt("roadmap", roadmap)
    attempt("tasks", tasks)
    attempt("events", audit)
    if not args.no_progress:
        attempt("metrics", metrics)
    attempt("messages", messages)
    attempt("fleet", fleet_state)
    return data


def fit(text: str, width: int) -> str:
    """Truncate to `width` cells, marking the cut with `~`."""
    if width <= 0:
        return ""
    return text if len(text) <= width else text[: max(0, width - 1)] + "~"


def table_lines(
    headers: list[str], rows: list[list[str]], width: int, state_column: int | None = None
) -> list[Line]:
    """Space separated columns; the last column is cut so a row fits in `width`."""
    widths = [max(len(row[index]) for row in [headers, *rows]) for index in range(len(headers))]
    gaps = 2 * (len(headers) - 1)
    widths[-1] = max(4, min(widths[-1], width - sum(widths[:-1]) - gaps))

    def join(cells: list[str]) -> str:
        padded = (cell[: widths[i]].ljust(widths[i]) for i, cell in enumerate(cells))
        return fit("  ".join(padded), width)

    out: list[Line] = [(join(headers), "head")]
    for row in rows:
        style = STATE_STYLE.get(row[state_column], "") if state_column is not None else ""
        out.append((join(row), style))
    return out


def error_lines(data: Data, *names: str) -> list[Line]:
    return [(f"ERROR {name}: {data.errors[name]}", "bad") for name in names if name in data.errors]


def ledger_of(data: Data) -> taskmeta.Ledger | None:
    if data.ledger is not None:
        return data.ledger
    return data.roadmap.ledger if data.roadmap is not None else None


def task_extras(entry: TaskEntry, data: Data) -> tuple[str, str, str, str]:
    """(state shown, progress, status text, blocked-by text) of a ledger entry."""
    state, _ = task_status(entry, data)
    claim = status.live_claims_by_task(data.claims or []).get(entry.ident)
    owners = status.live_owners(data.claims or [])
    blocked = status.blocked_text(ledger_of(data), entry.ident, owners)
    if claim is None:
        return state, "", "", blocked
    return status.task_state(entry.char, claim), claim.progress, claim.status_text, blocked


def task_status(entry: TaskEntry, data: Data) -> tuple[str, str]:
    """(state, owner) of a ledger entry: claimed by a live session, in flight, blocked or open."""
    owners = status.live_owners(data.claims or [])
    if entry.ident in owners:
        return "claimed", ",".join(sorted(owners[entry.ident]))
    return {"~": "in-flight", "!": "blocked"}.get(entry.char, "open"), ""


def render_dashboard(data: Data, width: int) -> list[Line]:
    out: list[Line] = [("Dashboard", "title")]
    out += error_lines(data, "roadmap", "claims", "tasks", "metrics", "messages", "fleet")
    if data.roadmap is not None:
        out.append(("ROADMAP", "head"))
        out += [(fit(f"  {text}", width), "") for text in data.roadmap.where[:2]]
        if data.states is not None:
            for item in status.milestone_progress(data.roadmap, data.states, data.claims or []):
                label = f"  {item.milestone.key:<4}{fit(item.milestone.title, 28):<28} "
                text = (
                    f"{label}{status.bar(item.percent, 16)} {item.done}/{item.known} {item.state}"
                )
                out.append((fit(text, width), STATE_STYLE.get(item.state, "")))
    if data.claims is not None:
        sessions = status.group_sessions(data.claims)
        live = sum(1 for claim in data.claims if claim.alive)
        active = sum(1 for session in sessions if session.alive)
        out += [
            ("SESSIONS", "head"),
            (
                f"  {len(sessions)} session(s), {active} active, {live} live claim(s), "
                f"{len(data.claims) - live} stale",
                "",
            ),
        ]
    if data.fleet is not None:
        workers = data.fleet.get("workers", {})
        out += [
            ("FLEET", "head"),
            (
                f"  {len(workers)} worker(s), supervisor {data.fleet.get('supervisor', '?')}, "
                f"last tick {tick_age(data.fleet)}",
                "",
            ),
        ]
    if data.counts is not None:
        done = data.counts[0]
        states = [task_status(entry, data)[0] for entry in visible_tasks(data, "")]
        tally = ", ".join(
            f"{states.count(name)} {name}" for name in ("claimed", "in-flight", "blocked", "open")
        )
        out += [
            ("TASKS", "head"),
            (f"  {done} done, {len(states)} open/claimed ({tally})", ""),
        ]
        out += ready_lines(data, width)
        out += blocked_lines(data, width)
    if data.metrics is not None:
        out.append(("BADGES", "head"))
        out += badge_lines(data.metrics, width)[:6]
    if data.messages is not None:
        out.append(("LATEST MESSAGES", "head"))
        out += [
            (fit(f"  {m.stamp} {m.sender}->{m.to}: {m.text}", width), "dim")
            for m in data.messages[-5:]
        ] or [("  no messages", "dim")]
    return [(fit(text, width), style) for text, style in out]


def ready_lines(data: Data, width: int) -> list[Line]:
    """Dashboard READY and WAITING ON lists, the same lists as `tasks next` (T1121)."""
    ledger = ledger_of(data)
    if ledger is None or data.claims is None:
        return []
    priorities = roadmap_doc.milestone_priorities(data.roadmap) if data.roadmap else {}
    ready, _, waiting = status.ready_and_waiting(ledger, data.claims, priorities)
    out: list[Line] = []
    if ready:
        out.append(("READY", "head"))
        out += [
            (fit(status.queue_row(ledger, priorities, task_id), width), "")
            for task_id in ready[:DASHBOARD_READY_LIMIT]
        ]
        if len(ready) > DASHBOARD_READY_LIMIT:
            out.append((f"  ... {len(ready) - DASHBOARD_READY_LIMIT} more on the Tasks tab", "dim"))
    if waiting:
        out.append(("WAITING ON (blocked-by not landed, not offered)", "head"))
        out += [
            (
                fit(
                    f"  {task_id:<7}<- {status.describe_blockers(blockers)}",
                    width,
                ),
                "dim",
            )
            for task_id, blockers in waiting[:DASHBOARD_READY_LIMIT]
        ]
        if len(waiting) > DASHBOARD_READY_LIMIT:
            out.append((f"  ... {len(waiting) - DASHBOARD_READY_LIMIT} more", "dim"))
    return out


def blocked_lines(data: Data, width: int) -> list[Line]:
    """Dashboard BLOCKED section: open tasks that name a blocker, then [!] ones that do not."""
    ledger = ledger_of(data)
    if ledger is None:
        return []
    owners = status.live_owners(data.claims or [])
    found = []
    for task in sorted(ledger.tasks.values(), key=lambda task: taskmeta.numeric(task.id)):
        text = status.blocked_text(ledger, task.id, owners)
        if text or (task.state == taskmeta.BLOCKED_STATE):
            found.append((task.id, text or "blocked, no blocker named"))
    if not found:
        return []
    out: list[Line] = [("BLOCKED", "head")]
    out += [
        (fit(f"  {task_id:<7}{text}", width), "bad")
        for task_id, text in found[:DASHBOARD_BLOCKED_LIMIT]
    ]
    if len(found) > DASHBOARD_BLOCKED_LIMIT:
        out.append((f"  ... {len(found) - DASHBOARD_BLOCKED_LIMIT} more on the Tasks tab", "dim"))
    return out


def render_milestones(data: Data, width: int) -> list[Line]:
    out: list[Line] = [("Milestones", "title")] + error_lines(data, "roadmap", "claims", "events")
    if data.roadmap is None or data.states is None:
        return out + [("  roadmap not loaded", "dim")]
    rows = []
    for item in status.milestone_progress(data.roadmap, data.states, data.claims or []):
        row = [
            item.milestone.key,
            item.milestone.priority or "-",
            item.milestone.title,
            item.state,
            f"{status.bar(item.percent, 12)} {item.done}/{item.known}",
            ",".join(item.who),
        ]
        if data.rollups is not None:
            rollup = data.rollups.get(item.milestone.key)
            row += [
                status.format_span(rollup.elapsed) if rollup else "",
                status.clip(",".join(rollup.owners), status.OWNERS_LIMIT) if rollup else "",
            ]
        rows.append([*row, item.milestone.now])
    if not rows:
        return out + [("  no milestones", "dim")]
    extra = ["ELAPSED", "OWNERS EVER"] if data.rollups is not None else []
    return out + table_lines(
        ["MS", "PRI", "MILESTONE", "STATE", "PROGRESS", "SESSIONS", *extra, "NOW"], rows, width, 3
    )


def visible_tasks(data: Data, text_filter: str) -> list[TaskEntry]:
    needle = text_filter.lower()
    result = []
    for entry in data.tasks or []:
        if entry.char == "x":
            continue
        state, owner = task_status(entry, data)
        shown, progress, text, blocked = task_extras(entry, data)
        haystack = (
            f"{entry.ident} {state} {shown} {owner} {entry.title} {progress} {text} {blocked}"
        )
        if needle and needle not in haystack.lower():
            continue
        result.append(entry)
    return result


def render_tasks(
    data: Data, width: int, text_filter: str = "", editing: bool = False
) -> list[Line]:
    """Header lines then one line per open task; the caller scrolls the task rows."""
    shown = visible_tasks(data, text_filter)
    cursor = "_" if editing else ""
    out: list[Line] = [
        (
            fit(
                f"Tasks  filter: {text_filter}{cursor}  ({len(shown)} open/claimed, / to filter)",
                width,
            ),
            "title",
        )
    ] + error_lines(data, "tasks", "claims")
    if data.tasks is None:
        return out
    ledger = data.roadmap.ledger if data.roadmap is not None else None
    blocked_known = ledger_of(data) is not None
    extras = {entry.ident: task_extras(entry, data) for entry in shown}
    with_status = any(extras[entry.ident][2] for entry in shown)
    rows = []
    for entry in shown:
        _, owner = task_status(entry, data)
        state, progress, text, blocked = extras[entry.ident]
        row = [entry.ident, state, owner, progress]
        if with_status:
            row.append(status.clip(text, status.STATUS_LIMIT))
        if blocked_known:
            row.append(status.clip(blocked, status.BLOCKED_LIMIT))
        row.append(entry.title)
        if ledger is not None:
            row.insert(1, ledger.resolve(entry.ident, "priority") or "-")
        rows.append(row)
    if not rows:
        return out + [("  no matching tasks", "dim")]
    headers = ["TASK", "STATE", "OWNER", "PROGRESS"]
    headers += ["STATUS"] if with_status else []
    headers += ["BLOCKED BY"] if blocked_known else []
    headers += ["TITLE"]
    if ledger is not None:
        return out + table_lines([headers[0], "PRIORITY", *headers[1:]], rows, width, 2)
    return out + table_lines(headers, rows, width, 1)


EVENT_STYLE = {
    "liberate": "bad",
    "block": "bad",
    "handoff": "accent",
    "merge": "ok",
    "publish": "ok",
    "unblock": "ok",
    "progress": "dim",
}


def history_lines(data: Data, task_id: str, width: int, show_progress: bool = False) -> list[Line]:
    """Event timeline of one task from the coord events log. `progress` events are opt-in."""
    out: list[Line] = [
        ("", ""),
        (f"HISTORY ({'with' if show_progress else 'without'} progress)", "head"),
    ]
    events = sorted(
        (e for e in data.events or [] if e.get("task") == task_id), key=lambda e: e["ts"]
    )
    if not show_progress:
        events = [e for e in events if e["kind"] != "progress"]
    if not events:
        return out + [("  no events recorded", "dim")]
    for event in events:
        text = (
            f"  {coord.stamp(event['ts'])} UTC  {event.get('owner')}  {event['kind']}  "
            f"{coord.format_detail(event.get('detail'))}"
        ).rstrip()
        wrapped = textwrap.wrap(text, max(10, width), subsequent_indent="      ") or [""]
        out += [(line, EVENT_STYLE.get(event["kind"], "")) for line in wrapped]
    return out


def blocked_detail_lines(data: Data, task_id: str) -> list[Line]:
    info = status.blocked_info(ledger_of(data), task_id, status.live_owners(data.claims or []))
    if info is None:
        return []
    out: list[Line] = [("BLOCKED BY", "head")]
    for blocker in info.blockers:
        shown = "not in ledger" if blocker.state is None else f"[{blocker.state}]"
        out.append((f"  {blocker.id}  {shown} {blocker.owner}".rstrip(), ""))
    if info.external:
        out.append((f"  external: {info.external}", ""))
    if info.reason:
        out.append((f"  reason: {info.reason}", ""))
    if info.landed:
        out.append(("  blocker landed, reopen", "ok"))
    return out + [("", "")]


def render_task_detail(
    entry: TaskEntry, data: Data, width: int, show_progress: bool = False
) -> list[Line]:
    _, owner = task_status(entry, data)
    shown, progress, text, _ = task_extras(entry, data)
    out: list[Line] = [
        (fit(f"{entry.ident}  [{shown}] {owner}".rstrip(), width), "title"),
        (fit("Esc closes, p shows or hides progress events", width), "dim"),
    ]
    if progress or text:
        out.append((fit(f"progress: {progress or '-'}  {text}".rstrip(), width), ""))
    out.append(("", ""))
    out += blocked_detail_lines(data, entry.ident)
    for paragraph in entry.body:
        out += [(wrapped, "") for wrapped in textwrap.wrap(paragraph, max(10, width)) or [""]]
    return out + history_lines(data, entry.ident, width, show_progress)


def render_sessions(data: Data, width: int, show_ended: bool = False) -> list[Line]:
    out: list[Line] = [("Sessions and Agents", "title")] + error_lines(data, "claims", "sessions")
    if data.claims is None:
        return out
    now = data.fetched_at or time.time()
    sessions = status.group_sessions(
        data.claims, data.registered, now, show_ended, data.session_stale
    )
    note = (
        ""
        if show_ended
        else status.hidden_note(data.claims, data.registered, now, data.session_stale)
    ).replace("--all-sessions", "key a")
    if not sessions:
        return (
            out + [("  no claims held: no active sessions", "dim")] + [(note, "dim")] * bool(note)
        )
    out.append(("SESSIONS", "head"))
    out += table_lines(
        ["SESSION", "FAMILY", "ROLE", "STATE", "LAST BEAT", "LAST SEEN", "CLAIMS", "SLOTS (free)"],
        [
            [
                session.owner,
                session.family,
                session.role,
                session.state,
                status.format_age(session.freshest_age) if session.claims else "-",
                status.seen_text(session, now),
                str(len(session.claims)),
                slot_text(session),
            ]
            for session in sessions
        ],
        width,
        3,
    )
    if note:
        out.append((note, "dim"))
    claim_rows = [
        [
            claim.ident,
            claim.slot,
            claim.task,
            claim.state,
            status.format_age(claim.age_seconds),
            claim.progress,
            status.clip(claim.status_text, status.STATUS_LIMIT),
            claim.note,
        ]
        for session in sessions
        for claim in sorted(session.claims, key=lambda held: (held.slot, held.task))
    ]
    if claim_rows:
        live = sum(1 for claim in data.claims if claim.alive)
        out += [("", ""), (f"CLAIMS ({live} live, {len(data.claims) - live} stale)", "head")]
        out += table_lines(
            ["AGENT ID", "SLOT", "TASK", "STATE", "AGE", "PROGRESS", "STATUS", "NOTE"],
            claim_rows,
            width,
            3,
        )
    return out


def slot_text(session: status.Session) -> str:
    used = sorted({claim.slot for claim in session.claims if claim.alive})
    free = [str(n) for n in range(1, coord.SLOT_COUNT + 1) if str(n) not in used]
    return f"{','.join(used) or '-'} ({','.join(free) or 'none'})"


def tick_age(state: dict, now: float | None = None) -> str:
    last = state.get("last_tick", 0)
    if not last:
        return "never"
    age = int((now if now is not None else time.time()) - last)
    stale = " STALE" if age > fleet.TICK_STALE_SECONDS else ""
    return f"{status.format_age(max(age, 0))} ago{stale}"


def render_workers(data: Data, width: int) -> list[Line]:
    out: list[Line] = [("Workers", "title")] + error_lines(data, "fleet")
    if data.fleet is None:
        return out
    out.append(
        (
            fit(
                f"supervisor {data.fleet.get('supervisor', '?')}, last tick {tick_age(data.fleet)} "
                f"(process state is not queried: read-only)",
                width,
            ),
            "",
        )
    )
    workers = data.fleet.get("workers", {})
    if not workers:
        return out + [("  no fleet workers recorded", "dim")]
    ages = {(c.owner, c.task): c.age_seconds for c in data.claims or []}
    rows = []
    for name, worker in sorted(workers.items()):
        lifecycle = status.session_lifecycle(
            name, data.claims or [], data.registered, data.fetched_at or None, data.session_stale
        )
        assigned = worker.get("assigned", {})
        tasks = " ".join(
            f"{task}(slot {info.get('slot', '?')},{status.format_age(ages[(name, task)])})"
            if (name, task) in ages
            else f"{task}(slot {info.get('slot', '?')})"
            for task, info in sorted(assigned.items())
        )
        rows.append(
            [
                name,
                str(worker.get("backend", "claude")),
                str(worker.get("session", "?")),
                f"gen {worker.get('generation', '?')}",
                lifecycle or "-",
                tasks or "idle",
            ]
        )
    out += table_lines(["WORKER", "BACKEND", "SESSION", "GEN", "STATE", "ASSIGNED"], rows, width, 4)
    claims = {(c.owner, c.task): c for c in data.claims or []}
    held = [
        claims[(name, task)]
        for name, worker in sorted(workers.items())
        for task in sorted(worker.get("assigned", {}))
        if (name, task) in claims
    ]
    if held:
        out += [("", ""), ("ASSIGNMENTS", "head")]
        out += table_lines(
            ["AGENT ID", "TASK", "STATE", "AGE", "PROGRESS", "STATUS"],
            [
                [
                    claim.ident,
                    claim.task,
                    claim.state,
                    status.format_age(claim.age_seconds),
                    claim.progress,
                    status.clip(claim.status_text, status.STATUS_LIMIT),
                ]
                for claim in held
            ],
            width,
            2,
        )
    return out


def render_messages(data: Data, width: int) -> list[Line]:
    out: list[Line] = [("Messages (newest first)", "title")] + error_lines(data, "messages")
    if data.messages is None:
        return out
    if not data.messages:
        return out + [("  no messages", "dim")]
    return out + [
        (fit(f"{m.stamp} {m.sender} -> {m.to}: {m.text}", width), "")
        for m in reversed(data.messages)
    ]


def badge_lines(metrics: dict, width: int) -> list[Line]:
    out: list[Line] = []
    entries = metrics.get("metrics", {})
    for key in sorted(entries):
        entry = entries[key]
        known = entry.get("known", False)
        text = str(entry.get("value", "unknown")) if known else "unknown"
        shown = status.bar(entry.get("percent") if known else None, 16)
        out.append(
            (
                fit(f"  {entry.get('label', key):<26} {shown} {text}", width),
                "ok" if known else "dim",
            )
        )
    return out


def render_progress(data: Data, width: int) -> list[Line]:
    out: list[Line] = [("Progress", "title")] + error_lines(data, "tasks", "metrics")
    if data.counts is not None:
        done, open_ = data.counts
        total = done + open_
        percent = 100 * done / total if total else 0.0
        out.append(
            (
                fit(
                    f"  {'tasks done':<26} {status.bar(percent, 16)} "
                    f"{done}/{total} ({percent:.1f}%)",
                    width,
                ),
                "ok",
            )
        )
    if data.metrics is not None:
        out += badge_lines(data.metrics, width)
        out.append(
            (
                fit(
                    f"  badges measured {data.metrics.get('measured_on', '?')} "
                    f"at {data.metrics.get('commit', '?')}",
                    width,
                ),
                "dim",
            )
        )
    elif "metrics" not in data.errors:
        out.append(("  badge metrics hidden or not loaded", "dim"))
    return out


@dataclass
class AuditRow:
    ident: str  # T<n> or M<n>
    owners: list[str]
    held_seconds: int
    handoffs: int
    replacements: int
    blocks: int
    open_blocks: int
    last_progress: str
    audit: dict  # `coord.audit_task` or `coord.merge_audits` result
    tasks: list[str]


def progress_text(progress: dict) -> str:
    percent = progress.get("last_percent")
    if percent is None:
        return ""
    mark = "~" if progress.get("last_percent_kind") == "assumed" else ""
    return f"{mark}{percent}% {progress.get('last_status') or ''}".rstrip()


def audit_rows(data: Data) -> list[AuditRow]:
    """Milestone rollups (by id), then every task with events, newest activity first."""
    rows: list[AuditRow] = []
    rollups = data.rollups or {}
    for key in sorted(rollups, key=taskmeta.numeric):
        item = rollups[key]
        rows.append(
            AuditRow(
                key,
                item.owners,
                item.held_seconds,
                item.handoffs,
                item.replacements,
                item.blocks,
                item.open_blocks,
                "",
                item.merged,
                item.tasks,
            )
        )
    tasks = sorted(
        (audit for audit in (data.audits or {}).values() if audit["events"]),
        key=lambda audit: (-audit["last_event_ts"], taskmeta.numeric(audit["task"])),
    )
    rows += [
        AuditRow(
            audit["task"],
            audit["owners"],
            audit["held_seconds"],
            audit["handoffs"],
            audit["replacements"],
            audit["blocks"]["count"],
            int(audit["blocks"]["open"]),
            progress_text(audit["progress"]),
            audit,
            [],
        )
        for audit in tasks
    ]
    return rows


def render_audit(data: Data, width: int) -> list[Line]:
    """Header lines then one row per milestone and task with events; the caller scrolls rows."""
    rows = audit_rows(data)
    out: list[Line] = [
        (fit(f"Audit  ({len(rows)} rollup(s), Enter shows the history)", width), "title")
    ] + error_lines(data, "events", "claims")
    if data.events is None:
        return out
    if not rows:
        return out + [("  no audit events recorded yet", "dim")]
    return out + table_lines(
        ["ID", "OWNERS EVER", "HELD", "HANDOFFS", "REPLACED", "BLOCKS", "LAST PROGRESS"],
        [
            [
                row.ident,
                status.clip(",".join(row.owners), status.OWNERS_LIMIT),
                status.format_span(row.held_seconds),
                str(row.handoffs),
                str(row.replacements),
                f"{row.blocks}{'*' if row.open_blocks else ''}",
                row.last_progress,
            ]
            for row in rows
        ],
        width,
    )


def audit_summary_lines(audit: dict) -> list[str]:
    """Rollup text of a `coord.audit_task` or `coord.merge_audits` result."""
    blocks = audit["blocks"]
    lines = [
        f"owners ever: {', '.join(audit['owners']) or '-'}",
        f"claims: {audit['claims']}  handoffs: {audit['handoffs']}  "
        f"replacements: {audit['replacements']}  stale liberations: {audit['stale_liberations']}",
        f"held: {status.format_span(audit['held_seconds'])}  "
        f"active: {status.format_span(audit['active_seconds'])}  "
        f"stale: {status.format_span(audit['stale_seconds'])}",
        f"blocks: {blocks['count']}  blocked: {status.format_span(blocks['seconds'])}"
        f"{'  (still blocked)' if blocks['open'] else ''}",
    ]
    if "progress" in audit:
        progress = audit["progress"]
        last = progress_text(progress)
        lines.append(
            f"progress: {progress['updates']} update(s)" + (f", last {last}" if last else "")
        )
    return lines


def render_audit_detail(
    row: AuditRow, data: Data, width: int, show_progress: bool = False
) -> list[Line]:
    out: list[Line] = [
        (fit(f"{row.ident} audit", width), "title"),
        (fit("Esc closes, p shows or hides progress events", width), "dim"),
        ("", ""),
    ]
    out += [(fit(line, width), "") for line in audit_summary_lines(row.audit)]
    if row.tasks:
        out += [("", ""), ("TASKS WITH EVENTS", "head")]
        audits = data.audits or {}
        out += [
            (
                fit(
                    f"  {task:<7}{','.join(audits[task]['owners'])}  "
                    f"held {status.format_span(audits[task]['held_seconds'])}",
                    width,
                ),
                "",
            )
            for task in row.tasks
            if task in audits
        ]
        return out
    return out + history_lines(data, row.ident, width, show_progress)


def tab_bar(active: int, width: int) -> Line:
    text = " ".join(f"{index + 1}:{name}" for index, name in enumerate(TABS))
    if len(text) > width:
        text = " ".join(f"{index + 1}:{name[:3]}" for index, name in enumerate(TABS))
    return fit(text, width), "bar"


def footer(view: View, data: Data | None, width: int) -> Line:
    refreshed = (
        time.strftime("%H:%M:%S", time.localtime(data.fetched_at))
        if data and data.fetched_at
        else "never"
    )
    busy = " refreshing" if view.refreshing else ""
    text = f"q quit  ? help  r refresh | every {view.interval}s, last {refreshed}{busy}"
    return fit(text, width), "bar"


def tab_lines(view: View, data: Data, width: int) -> tuple[list[Line], int]:
    """Body lines of the active tab and how many leading lines are fixed header."""
    if view.tab == 0:
        return render_dashboard(data, width), 0
    if view.tab == 1:
        return render_milestones(data, width), 0
    if view.tab == TASKS_TAB:
        shown = visible_tasks(data, view.filter)
        if view.detail and shown:
            entry = shown[min(view.selected, len(shown) - 1)]
            return render_task_detail(entry, data, width, view.show_progress), 0
        lines = render_tasks(data, width, view.filter, view.editing)
        errors = len(data.errors.keys() & {"tasks", "claims"})
        return lines, 1 + errors + (1 if shown and data.tasks is not None else 0)
    if view.tab == AUDIT_TAB:
        rows = audit_rows(data)
        if view.detail and rows:
            row = rows[min(view.audit_selected, len(rows) - 1)]
            return render_audit_detail(row, data, width, view.show_progress), 0
        lines = render_audit(data, width)
        errors = len(data.errors.keys() & {"events", "claims"})
        return lines, 1 + errors + (1 if rows and data.events is not None else 0)
    if view.tab == 3:
        return render_sessions(data, width, view.show_ended), 0
    renderers = {4: render_workers, 5: render_messages, 6: render_progress}
    return renderers[view.tab](data, width), 0


def compose_frame(view: View, data: Data | None, height: int, width: int) -> list[Line]:
    """Exactly `height` lines of at most `width` cells. Never raises on small sizes."""
    if height < 3 or width < 12:
        small = [(fit("terminal too small", width), "bad")] + [("", "")] * max(0, height - 1)
        return small[: max(0, height)]
    data = data or Data()
    body_height = height - 2
    if view.help:
        lines, fixed = (
            [(fit(text, width), "title" if i == 0 else "") for i, text in enumerate(HELP)],
            0,
        )
    elif not data.fetched_at:
        lines, fixed = [("loading...", "dim")], 0
    else:
        lines, fixed = tab_lines(view, data, width)
    header, rows = lines[:fixed], lines[fixed:]
    room = max(1, body_height - len(header))
    maximum = max(0, len(rows) - room)
    if view.tab in SELECT_TABS and fixed and not view.help:
        selected = min(selected_of(view), max(0, len(rows) - 1))
        start = min(view.scroll[view.tab], maximum)
        if selected < start:
            start = selected
        elif selected >= start + room:
            start = selected - room + 1
        view.scroll[view.tab] = max(0, min(start, maximum))
        shown_rows = enumerate(
            rows[view.scroll[view.tab] : view.scroll[view.tab] + room], view.scroll[view.tab]
        )
        window = [
            (text, "sel" if index == selected else style) for index, (text, style) in shown_rows
        ]
    else:
        view.scroll[view.tab] = max(0, min(view.scroll[view.tab], maximum))
        window = rows[view.scroll[view.tab] : view.scroll[view.tab] + room]
    body = [(fit(text, width), style) for text, style in [*header, *window]][:body_height]
    body += [("", "")] * (body_height - len(body))
    return [tab_bar(view.tab, width), *body, footer(view, data, width)]


def handle_key(view: View, key: int, data: Data | None, height: int) -> str | None:
    """Apply one key to the view. Returns "quit", "refresh" or None."""
    page = max(1, height - 3)
    if view.editing:
        if key in (10, 13, curses.KEY_ENTER):
            view.editing = False
        elif key == 27:
            view.editing, view.filter = False, ""
        elif key in (8, 127, curses.KEY_BACKSPACE):
            view.filter = view.filter[:-1]
        elif 32 <= key < 127:
            view.filter += chr(key)
        view.selected = view.scroll[2] = 0
        return None
    if view.help:
        view.help = False
        return None
    if key in (ord("q"), ord("Q")):
        return "quit"
    if key in (ord("r"), ord("R")):
        return "refresh"
    if key == ord("?"):
        view.help = True
    elif ord("1") <= key <= ord(str(len(TABS))):
        switch_tab(view, key - ord("1"))
    elif key in (9, curses.KEY_RIGHT):
        switch_tab(view, (view.tab + 1) % len(TABS))
    elif key in (curses.KEY_LEFT, curses.KEY_BTAB):
        switch_tab(view, (view.tab - 1) % len(TABS))
    elif key == ord("/") and view.tab == TASKS_TAB and not view.detail:
        view.editing = True
    elif key == 27 and view.detail:
        view.detail = False
    elif key in (10, 13, curses.KEY_ENTER) and view.tab in SELECT_TABS and data is not None:
        view.detail = row_count(view, data) > 0
    elif key == ord("p") and view.detail:
        view.show_progress = not view.show_progress
    elif key in (ord("a"), ord("A")) and view.tab == SESSIONS_TAB:
        view.show_ended = not view.show_ended
    elif key in (curses.KEY_DOWN, ord("j")):
        move(view, 1, data)
    elif key in (curses.KEY_UP, ord("k")):
        move(view, -1, data)
    elif key == curses.KEY_NPAGE:
        move(view, page, data)
    elif key == curses.KEY_PPAGE:
        move(view, -page, data)
    elif key in (curses.KEY_HOME, ord("g")):
        move(view, -(10**9), data)
    elif key in (curses.KEY_END, ord("G")):
        move(view, 10**9, data)
    return None


def switch_tab(view: View, tab: int) -> None:
    view.tab, view.detail = tab, False


def selected_of(view: View) -> int:
    return view.audit_selected if view.tab == AUDIT_TAB else view.selected


def row_count(view: View, data: Data) -> int:
    """Number of selectable rows of the active tab (Tasks or Audit)."""
    if view.tab == AUDIT_TAB:
        return len(audit_rows(data))
    return len(visible_tasks(data, view.filter))


def move(view: View, delta: int, data: Data | None) -> None:
    if view.tab in SELECT_TABS and not view.detail and data is not None:
        count = row_count(view, data)
        moved = max(0, min(selected_of(view) + delta, max(0, count - 1)))
        if view.tab == AUDIT_TAB:
            view.audit_selected = moved
        else:
            view.selected = moved
    else:
        view.scroll[view.tab] = max(0, view.scroll[view.tab] + delta)


STYLE_ATTRS = {"title": "bold", "head": "bold", "bar": "reverse", "sel": "reverse", "dim": "dim"}
STYLE_COLOURS = {"ok": "green", "bad": "red", "accent": "cyan", "title": "cyan", "dim": "white"}


def build_styles(use_colour: bool) -> dict[str, int]:
    attrs = {"bold": curses.A_BOLD, "reverse": curses.A_REVERSE, "dim": curses.A_DIM}
    styles = {name: attrs[attr] for name, attr in STYLE_ATTRS.items()}
    if use_colour and curses.has_colors():
        curses.start_color()
        curses.use_default_colors()
        palette = {
            "green": curses.COLOR_GREEN,
            "red": curses.COLOR_RED,
            "cyan": curses.COLOR_CYAN,
            "white": curses.COLOR_WHITE,
        }
        for index, (name, colour) in enumerate(STYLE_COLOURS.items(), 1):
            curses.init_pair(index, palette[colour], -1)
            if name != "dim":
                styles[name] = styles.get(name, 0) | curses.color_pair(index)
    return styles


def draw(screen: Any, view: View, data: Data | None, styles: dict[str, int]) -> None:
    height, width = screen.getmaxyx()
    screen.erase()
    for row, (text, style) in enumerate(compose_frame(view, data, height, width)):
        try:
            screen.addnstr(
                row,
                0,
                text,
                max(0, width - 1 if row == height - 1 else width),
                styles.get(style, 0),
            )
        except curses.error:
            pass  # writing the bottom-right cell raises, harmless
    screen.refresh()


class Refresher:
    """Gathers data on a worker thread so a slow source never blocks key handling."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.result: Data | None = None
        self.thread: threading.Thread | None = None

    @property
    def busy(self) -> bool:
        return self.thread is not None and self.thread.is_alive()

    def start(self) -> None:
        if not self.busy:
            self.thread = threading.Thread(target=self.work, daemon=True)
            self.thread.start()

    def work(self) -> None:
        self.result = gather(self.args)

    def take(self) -> Data | None:
        data, self.result = self.result, None
        return data


def new_view(args: argparse.Namespace) -> View:
    """The starting view: `--watch` interval, ended sessions shown only with `--all-sessions`."""
    return View(interval=max(1, args.watch or 5), show_ended=getattr(args, "all_sessions", False))


def loop(screen: Any, args: argparse.Namespace) -> int:
    curses.curs_set(0)
    styles = build_styles(not args.no_colour)
    view = new_view(args)
    refresher = Refresher(args)
    data: Data | None = None
    refresher.start()
    next_refresh = time.monotonic() + view.interval
    screen.timeout(200)
    while True:
        fresh = refresher.take()
        if fresh is not None:
            data = fresh
            next_refresh = time.monotonic() + view.interval
        view.refreshing = refresher.busy
        draw(screen, view, data, styles)
        key = screen.getch()
        if key == curses.KEY_RESIZE:
            curses.update_lines_cols()
        elif key != -1:
            action = handle_key(view, key, data, screen.getmaxyx()[0])
            if action == "quit":
                return 0
            if action == "refresh":
                refresher.start()
        if time.monotonic() >= next_refresh:
            refresher.start()
            next_refresh = time.monotonic() + view.interval


def run(args: argparse.Namespace) -> int:
    os.environ.setdefault("ESCDELAY", "25")
    try:
        return curses.wrapper(loop, args)
    except KeyboardInterrupt:
        return 0
