# SPDX-License-Identifier: GPL-3.0-or-later
"""One-screen view of the roadmap, active Claude and Codex sessions and overall progress.

The roadmap comes from `docs/roadmap.md`, with per-milestone progress derived from
`docs/tasks.md` (see `tools/agents/roadmap.py`).

Sessions come from `python3 -m tools.agents.coord list --json` (claims grouped by owner,
with percent, status text and the unique agent id) and the registered `sessions/*.json`
records. Blocked-by comes from the ledger (`taskmeta`), the audit rollups from the coord
events log through the pure helpers of `tools/agents/coord.py`. Progress comes from the
committed badge data in `docs/badges/metrics.json` and the checkbox counts in
`docs/tasks.md`. Read-only: nothing is claimed, beaten, reaped or logged.

    python3 -m tools.agents.status              # snapshot
    python3 -m tools.agents.status --watch 5    # interactive TUI (tabs 1-8, ? for help)
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import TypeVar

from tools.agents import coord, taskmeta
from tools.agents import roadmap as roadmap_doc

T = TypeVar("T")
DEFAULT_METRICS = Path("docs/badges/metrics.json")
DEFAULT_SUBSYSTEMS = Path("docs/data/subsystem-coverage.json")
DEFAULT_TASKS = Path("docs/tasks.md")
DEFAULT_ROADMAP = roadmap_doc.DEFAULT_ROADMAP
BAR_WIDTH = 24
COORD_TIMEOUT_SECONDS = 30
DEFAULT_WIDTH = 120
NEXT_UP_LIMIT = 8
# A claim older than this (seconds since it was claimed) with no `coord progress` report
# is flagged "no report": the duty is a report every five minutes, so ten is two missed.
NO_REPORT_SECONDS = 600
# A registered session that holds no claim and was not seen for this long counts as ended
# (T1122, `coord sessions`): hidden unless `--all-sessions`. Same default as `coord`.
SESSION_STALE_SECONDS = coord.SESSION_STALE_SECONDS
IDLE_SESSION_SECONDS = SESSION_STALE_SECONDS  # name kept for older callers
NO_REPORT = "no report"
STATUS_LIMIT = 30  # widest STATUS cell in a table, the TITLE or NOTE column keeps the rest
BLOCKED_LIMIT = 60  # widest BLOCKED BY cell in a table
OWNERS_LIMIT = 28  # widest OWNERS EVER cell in a table


@dataclass
class Claim:
    task: str
    owner: str
    slot: str
    age_seconds: int
    alive: bool
    note: str
    agent_id: str = ""
    percent: int | None = None
    percent_kind: str = ""
    status_text: str = ""
    progress_at: int | None = None
    held_seconds: int | None = None  # seconds since the claim was made, None when unknown

    @property
    def planning(self) -> bool:
        return self.task.startswith("NEXT-")

    @property
    def ident(self) -> str:
        """Unique id: `<owner>/s<slot>:T<n>` for a subagent claim, the owner for slot 0."""
        if self.agent_id:
            return self.agent_id
        slot = int(self.slot) if self.slot.isdigit() else 0
        return coord.subagent_id(self.owner, slot, coord.event_task(self.task))

    @property
    def reported(self) -> bool:
        return self.percent is not None or bool(self.status_text)

    @property
    def no_report(self) -> bool:
        return (
            not self.planning
            and not self.reported
            and self.held_seconds is not None
            and self.held_seconds > NO_REPORT_SECONDS
        )

    @property
    def state(self) -> str:
        """Display state of the claim: STALE, no report, or active."""
        if not self.alive:
            return "STALE"
        return NO_REPORT if self.no_report else "active"

    @property
    def progress(self) -> str:
        """`40%` for a measured percent, `~40%` for an assumed one, blank when unreported."""
        if self.percent is None:
            return ""
        return f"{'~' if self.percent_kind == 'assumed' else ''}{self.percent}%"


@dataclass
class Session:
    owner: str
    claims: list[Claim] = field(default_factory=list)
    role: str = ""
    parent: str = ""
    started_at: int | None = None
    lifecycle: str = ""  # coord's live|idle|ended for a registered session, "" otherwise
    last_seen: float | None = None  # coord `session_last_seen`, None when unregistered

    @property
    def family(self) -> str:
        return self.owner.split("-", 1)[0]

    @property
    def alive(self) -> bool:
        return any(claim.alive for claim in self.claims)

    @property
    def freshest_age(self) -> int:
        return min((claim.age_seconds for claim in self.claims), default=0)

    @property
    def state(self) -> str:
        if self.alive:
            return "active"
        if self.claims:
            return "silent"
        return "ended" if self.lifecycle == "ended" else "idle"


def parse_coord_list(text: str) -> list[Claim]:
    """Parse `coord list` rows: task, owner, slot=N, <age>s, alive|STALE, note (tab separated)."""
    claims: list[Claim] = []
    for line in text.splitlines():
        parts = line.split("\t", 5)
        if len(parts) < 6 or not re.fullmatch(r"\d+s", parts[3]):
            continue
        task, owner, slot, age, state, note = parts
        claims.append(
            Claim(
                task=task,
                owner=owner,
                slot=slot.removeprefix("slot="),
                age_seconds=int(age[:-1]),
                alive=state == "alive",
                note=note,
            )
        )
    return claims


def int_or_none(value: object) -> int | None:
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def parse_coord_json(text: str, now: float | None = None) -> list[Claim]:
    """Parse `coord list --json`. A record that is not an object with a task and owner is skipped.

    `held_seconds` (time since the claim was made) comes from `claimed_at` and `now`.
    """
    records = json.loads(text)
    if not isinstance(records, list):
        raise ValueError("coord list --json did not print an array")
    now = time.time() if now is None else now
    claims: list[Claim] = []
    for record in records:
        if (
            not isinstance(record, dict)
            or not isinstance(record.get("task"), str)
            or not isinstance(record.get("owner"), str)
        ):
            continue
        claimed_at = int_or_none(record.get("claimed_at"))
        stale = record.get("stale")
        claims.append(
            Claim(
                task=record["task"],
                owner=record["owner"],
                slot=str(record.get("slot", 0)),
                age_seconds=int_or_none(record.get("age")) or 0,
                alive=not stale if isinstance(stale, bool) else record.get("state") == "alive",
                note=str(record.get("note", "")),
                agent_id=str(record.get("agent_id") or ""),
                percent=int_or_none(record.get("percent")),
                percent_kind=str(record.get("percent_kind") or ""),
                status_text=str(record.get("status_text") or ""),
                progress_at=int_or_none(record.get("progress_at")),
                held_seconds=int(now - claimed_at) if claimed_at else None,
            )
        )
    return claims


def group_sessions(
    claims: list[Claim],
    registered: dict[str, dict] | None = None,
    now: float | None = None,
    show_ended: bool = False,
    session_stale: int = SESSION_STALE_SECONDS,
) -> list[Session]:
    """Claims grouped by owner. `registered` (see `read_sessions`) adds the role and parent,
    and lists a registered session that holds no claim as idle.

    A registered session gets coord's lifecycle (`coord.session_state`: live, idle or ended,
    ended when explicitly ended or not seen for `session_stale` seconds). A session with no
    claim that is ended is left out unless `show_ended`. One that still holds a claim (stale
    ones, never live, by coord's rule) stays listed so the claim can be reaped.
    """
    registered = registered or {}
    now = time.time() if now is None else now
    sessions: dict[str, Session] = {}
    for claim in claims:
        sessions.setdefault(claim.owner, Session(claim.owner)).claims.append(claim)
    for owner, record in registered.items():
        live = sum(
            1
            for claim in sessions.get(owner, Session(owner)).claims
            if claim.alive and not claim.planning
        )
        lifecycle = coord.session_state(record, live, now, session_stale)
        if owner not in sessions:
            if lifecycle == "ended" and not show_ended:
                continue
            sessions[owner] = Session(owner)
        session = sessions[owner]
        session.role = str(record.get("role") or "")
        session.parent = str(record.get("parent") or "")
        session.started_at = int_or_none(record.get("started_at"))
        session.lifecycle = lifecycle
        session.last_seen = coord.session_last_seen(record)
    return sorted(sessions.values(), key=lambda session: (session.family, session.owner))


def session_lifecycle(
    owner: str,
    claims: list[Claim],
    registered: dict[str, dict] | None,
    now: float | None = None,
    session_stale: int = SESSION_STALE_SECONDS,
) -> str:
    """coord's live|idle|ended for a registered owner, blank when it has no record."""
    record = (registered or {}).get(owner)
    if record is None:
        return ""
    live = sum(1 for claim in claims if claim.owner == owner and claim.alive and not claim.planning)
    return coord.session_state(record, live, time.time() if now is None else now, session_stale)


def hidden_ended_count(
    claims: list[Claim],
    registered: dict[str, dict] | None,
    now: float | None = None,
    session_stale: int = SESSION_STALE_SECONDS,
) -> int:
    """How many ended sessions `group_sessions` leaves out (for the `--all-sessions` hint)."""
    return len(group_sessions(claims, registered, now, True, session_stale)) - len(
        group_sessions(claims, registered, now, False, session_stale)
    )


def read_sessions() -> dict[str, dict]:
    """Registered sessions by owner, from the records `coord register` writes.

    Uses the pure read helper behind `coord sessions` (`coord.session_records`), which skips
    unreadable records. A missing folder gives an empty dict, never an error.
    """
    return {
        record["owner"]: record
        for record in coord.session_records()
        if isinstance(record.get("owner"), str)
    }


def fetch_claims(stale_seconds: int) -> list[Claim]:
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "tools.agents.coord",
            "--stale",
            str(stale_seconds),
            "list",
            "--json",
        ],
        capture_output=True,
        text=True,
        timeout=COORD_TIMEOUT_SECONDS,
    )
    if result.returncode != 0:
        raise RuntimeError(f"coord list failed ({result.returncode}): {result.stderr.strip()}")
    return parse_coord_json(result.stdout)


def count_tasks(tasks_path: Path) -> tuple[int, int]:
    text = tasks_path.read_text()
    done = len(re.findall(r"^- \[x\] \*\*T\d+", text, flags=re.MULTILINE))
    open_ = len(re.findall(r"^- \[ \] \*\*T\d+", text, flags=re.MULTILINE))
    return done, open_


def load_metrics(metrics_path: Path) -> dict:
    return json.loads(metrics_path.read_text())


def format_age(seconds: int) -> str:
    if seconds < 90:
        return f"{seconds}s"
    if seconds < 5400:
        return f"{seconds // 60}m"
    return f"{seconds // 3600}h{seconds % 3600 // 60:02d}m"


def clip(text: str, limit: int) -> str:
    """Cut to `limit` characters, marking the cut with `~`."""
    return text if len(text) <= limit else text[: max(0, limit - 1)] + "~"


def format_span(seconds: float) -> str:
    """Like `format_age`, with days from 48 hours: `3d04h`."""
    seconds = int(seconds)
    if seconds < 48 * 3600:
        return format_age(seconds)
    return f"{seconds // 86400}d{seconds % 86400 // 3600:02d}h"


def bar(percent: float | None, width: int = BAR_WIDTH) -> str:
    if percent is None:
        return " " * (width + 2)
    filled = round(max(0.0, min(100.0, percent)) / 100 * width)
    return "[" + "#" * filled + "." * (width - filled) + "]"


def colour(text: str, code: str, enabled: bool) -> str:
    return f"\033[{code}m{text}\033[0m" if enabled else text


STATE_COLOURS = {
    "active": "32",
    "done": "36",
    "silent": "31",
    "STALE": "31",
    NO_REPORT: "31",
    "blocked": "31",
    "pending": "90",
    "idle": "90",
}


def render_table(
    headers: list[str],
    rows: list[list[str]],
    use_colour: bool,
    width: int,
    state_column: int | None = None,
) -> list[str]:
    """Plain-text table. The last column is truncated so a row fits in `width`."""
    widths = [max(len(row[index]) for row in [headers, *rows]) for index in range(len(headers))]
    fixed = sum(widths[:-1]) + 3 * (len(headers) - 1) + 4
    widths[-1] = max(10, min(widths[-1], width - fixed))

    def line(cells: list[str], paint: bool) -> str:
        parts = []
        for index, cell in enumerate(cells):
            text = cell if len(cell) <= widths[index] else cell[: widths[index] - 1] + "~"
            text = text.ljust(widths[index])
            if paint and index == state_column:
                text = colour(text, STATE_COLOURS.get(cell.strip(), "0"), use_colour)
            parts.append(text)
        return "| " + " | ".join(parts) + " |"

    rule = "+" + "+".join("-" * (size + 2) for size in widths) + "+"
    return [rule, line(headers, False), rule, *(line(row, True) for row in rows), rule]


@dataclass
class Blocker:
    id: str
    state: str | None  # checkbox char, None when the id is not in the ledger
    owner: str  # live claimant, "" when nobody holds it


@dataclass
class BlockedBy:
    blockers: list[Blocker]
    external: str
    reason: str
    landed: bool  # a [!] task whose listed blockers are all [x]: reopen it

    @property
    def text(self) -> str:
        """`blocked by T456 [~ claude-2], T789 [ ]; external: slug (reason)`."""
        parts = []
        if self.blockers:
            shown = ", ".join(
                f"{b.id} [{'?' if b.state is None else b.state}{' ' + b.owner if b.owner else ''}]"
                for b in self.blockers
            )
            parts.append(f"blocked by {shown}")
        if self.external:
            parts.append(
                f"external: {self.external}" + (f" ({self.reason})" if self.reason else "")
            )
        text = "; ".join(parts)
        return f"{text} (blocker landed, reopen)" if self.landed else text


def blocked_info(
    ledger: taskmeta.Ledger | None, task_id: str, owners: dict[str, set[str]]
) -> BlockedBy | None:
    """What blocks a task, from the ledger. None when it names nothing or the task is done."""
    task = ledger.tasks.get(task_id) if ledger is not None else None
    if ledger is None or task is None or task.state == "x":
        return None
    if not task.blocked_by and not task.blocked_external:
        return None
    states = ledger.blocker_states(task_id)
    blockers = [
        Blocker(blocker, state, ",".join(sorted(owners.get(blocker, ()))))
        for blocker, state in states.items()
    ]
    return BlockedBy(
        blockers,
        task.blocked_external or "",
        task.blocked_reason or "",
        task.state == taskmeta.BLOCKED_STATE and ledger.blockers_landed(task_id),
    )


def blocked_text(ledger: taskmeta.Ledger | None, task_id: str, owners: dict[str, set[str]]) -> str:
    info = blocked_info(ledger, task_id, owners)
    return info.text if info else ""


def live_claims_by_task(claims: list[Claim]) -> dict[str, Claim]:
    return {claim.task: claim for claim in claims if claim.alive and not claim.planning}


def task_audits(events: list[dict], claims: list[Claim], now: float) -> dict[str, dict]:
    """`coord.audit_task` rollup of every task that has events, keyed by task id."""
    by_task: dict[str, list[dict]] = {}
    for event in events:
        if isinstance(event.get("task"), str):
            by_task.setdefault(event["task"], []).append(event)
    live = {
        claim.task: {"owner": claim.owner, "stale_age": 0 if claim.alive else claim.age_seconds}
        for claim in claims
        if not claim.planning
    }
    return {
        task: coord.audit_task(task_events, task, int(now), live)
        for task, task_events in by_task.items()
    }


@dataclass
class Rollup:
    owners: list[str]
    first_ts: float
    last_ts: float
    elapsed: int
    held_seconds: int
    handoffs: int
    replacements: int
    blocks: int
    open_blocks: int
    tasks: list[str]  # tasks of the milestone that have events
    merged: dict  # the `coord.merge_audits` result, for `coord.format_audit`


def milestone_rollups(
    roadmap: roadmap_doc.Roadmap,
    audits: dict[str, dict],
    events: list[dict],
    states: dict[str, str],
    now: float,
) -> dict[str, Rollup]:
    """Per milestone key with at least one event on its tasks: owners ever and elapsed time.

    Elapsed runs from the first event to the last event when every task is done, else to now.
    A milestone without events has no entry (the views show blanks).
    """
    rollups: dict[str, Rollup] = {}
    for milestone in roadmap.milestones:
        tasks = set(milestone.tasks) | {
            event["task"]
            for event in events
            if event.get("milestone") == milestone.key and isinstance(event.get("task"), str)
        }
        found = [audits[task] for task in sorted(tasks, key=taskmeta.numeric) if task in audits]
        merged = coord.merge_audits(milestone.key, found)
        if merged["first_event_ts"] is None:
            continue
        known = [states[task] for task in milestone.tasks if task in states]
        done = bool(known) and all(state == "x" for state in known)
        end = merged["last_event_ts"] if done else max(now, merged["last_event_ts"])
        rollups[milestone.key] = Rollup(
            merged["owners"],
            merged["first_event_ts"],
            merged["last_event_ts"],
            int(end - merged["first_event_ts"]),
            merged["held_seconds"],
            merged["handoffs"],
            merged["replacements"],
            merged["blocks"]["count"],
            merged["blocks"]["open"],
            [audit["task"] for audit in found],
            merged,
        )
    return rollups


def fetch_events() -> list[dict]:
    """The coord events log, read-only (torn lines skipped). Empty when there is none."""
    return coord.read_events()


def seen_text(session: Session, now: float) -> str:
    """Age of the session's last coord command (`last_seen`), `-` for an unregistered owner."""
    if session.last_seen is None:
        return "-"
    return format_age(max(0, int(now - session.last_seen)))


def hidden_note(
    claims: list[Claim], registered: dict[str, dict] | None, now: float, session_stale: int
) -> str:
    """`  2 ended session(s) hidden, pass --all-sessions to list them`, blank when none."""
    count = hidden_ended_count(claims, registered, now, session_stale)
    return f"  {count} ended session(s) hidden, pass --all-sessions to list them" if count else ""


def render_sessions(
    claims: list[Claim],
    use_colour: bool,
    width: int = DEFAULT_WIDTH,
    registered: dict[str, dict] | None = None,
    now: float | None = None,
    show_ended: bool = False,
    session_stale: int = SESSION_STALE_SECONDS,
) -> list[str]:
    now = time.time() if now is None else now
    sessions = group_sessions(claims, registered, now, show_ended, session_stale)
    hidden = [] if show_ended else [hidden_note(claims, registered, now, session_stale)]
    if not sessions:
        return ["SESSIONS", "  no claims held: no active sessions", *filter(None, hidden)]
    session_rows = [
        [
            session.owner,
            session.family,
            session.role,
            session.state,
            format_age(session.freshest_age) if session.claims else "-",
            seen_text(session, now),
            str(len(session.claims)),
            ",".join(sorted({claim.slot for claim in session.claims})),
        ]
        for session in sessions
    ]
    agent_rows = [
        [
            claim.ident,
            claim.slot,
            claim.task,
            claim.state,
            format_age(claim.age_seconds),
            claim.progress,
            clip(claim.status_text, STATUS_LIMIT),
            claim.note,
        ]
        for session in sessions
        for claim in sorted(session.claims, key=lambda held: (held.slot, held.task))
    ]
    live_claims = sum(1 for claim in claims if claim.alive)
    families = {
        family: [s for s in sessions if s.family == family]
        for family in {s.family for s in sessions}
    }
    summary = ", ".join(
        f"{family} {sum(1 for s in members if s.alive)}/{len(members)} active"
        for family, members in sorted(families.items())
    )
    lines = [
        f"SESSIONS ({summary})",
        *render_table(
            ["SESSION", "FAMILY", "ROLE", "STATE", "LAST BEAT", "LAST SEEN", "CLAIMS", "SLOTS"],
            session_rows,
            use_colour,
            width,
            state_column=3,
        ),
        *filter(None, hidden),
    ]
    if agent_rows:
        lines += [
            "",
            f"AGENTS ({live_claims} live claim(s), {len(claims) - live_claims} stale)",
            *render_table(
                ["AGENT ID", "SLOT", "TASK", "STATE", "AGE", "PROGRESS", "STATUS", "NOTE"],
                agent_rows,
                use_colour,
                width,
                state_column=3,
            ),
        ]
    return lines


def task_state(char: str, claim: Claim | None) -> str:
    """Display state of a ledger task: a live claim shows `claimed` (or `no report`)."""
    if claim is not None:
        return NO_REPORT if claim.no_report else "claimed"
    return {"~": "in-flight", "!": "blocked"}.get(char, "open")


def render_work(
    ledger: taskmeta.Ledger | None,
    claims: list[Claim],
    use_colour: bool,
    width: int = DEFAULT_WIDTH,
) -> list[str]:
    """Tasks in flight, claimed or blocked, with progress and what blocks them."""
    if ledger is None:
        return []
    owners = live_owners(claims)
    held = live_claims_by_task(claims)
    rows = []
    for task in sorted(ledger.tasks.values(), key=lambda task: taskmeta.numeric(task.id)):
        blocked = blocked_text(ledger, task.id, owners)
        if task.state == "x" or not (
            task.id in held or task.state in ("~", taskmeta.BLOCKED_STATE) or blocked
        ):
            continue
        claim = held.get(task.id)
        rows.append(
            [
                task.id,
                task_state(task.state, claim),
                ",".join(sorted(owners.get(task.id, ()))),
                claim.progress if claim else "",
                clip(claim.status_text, STATUS_LIMIT) if claim else "",
                clip(blocked, BLOCKED_LIMIT),
                task.title,
            ]
        )
    if not rows:
        return []
    return [
        f"TASKS IN FLIGHT AND BLOCKED ({len(rows)})",
        *render_table(
            ["TASK", "STATE", "OWNER", "PROGRESS", "STATUS", "BLOCKED BY", "TITLE"],
            rows,
            use_colour,
            width,
            state_column=1,
        ),
    ]


def render_progress(metrics: dict, done: int, open_: int, use_colour: bool) -> list[str]:
    lines = ["PROGRESS"]
    total = done + open_
    percent = 100 * done / total if total else 0.0
    lines.append(f"  {'tasks done':<26} {bar(percent)} {done}/{total} ({percent:.1f}%)")
    entries = metrics.get("metrics", {})
    for key in sorted(entries):
        entry = entries[key]
        known = entry.get("known", False)
        text = entry.get("value", "unknown") if known else "unknown"
        shown = bar(entry.get("percent") if known else None)
        code = "32" if known else "90"
        lines.append(f"  {entry.get('label', key):<26} {shown} {colour(text, code, use_colour)}")
    lines.append(
        f"  badges measured {metrics.get('measured_on', '?')} at {metrics.get('commit', '?')}"
    )
    return lines


def load_subsystems(path: Path) -> dict | None:
    """The generated per-subsystem coverage (python -m tools.subsystems report --write), or None."""
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) and data.get("schema") == 1 else None


def render_subsystems(data: dict, use_colour: bool) -> list[str]:
    """Top-level decompilation subsystems: functions, named, registered, proven, unclassified.

    Read from docs/data/subsystem-coverage.json, never typed by hand. A counter the generator
    could not measure (a stale proof snapshot) is shown as n/a, not 0.
    """
    lines = ["DECOMPILATION BY SUBSYSTEM (docs/decomp-tree.md)"]
    header = f"  {'subsystem':<12} {'functions':>9} {'named':>7} {'registered':>10} {'proven':>7}"
    lines.append(colour(header, "90", use_colour))
    for entry in data.get("subsystems", []):
        if entry.get("depth") != 0:
            continue
        roll = entry["rollup"]
        proven = roll.get("proven")
        shown = (
            "n/a" if "proven" in data.get("unavailable", {}) or proven is None else f"{proven:,}"
        )
        lines.append(
            f"  {entry['path']:<12} {roll['functions']:>9,} {roll['named']:>7,} "
            f"{roll.get('registered', 0):>10,} {shown:>7}"
        )
    unknown = data.get("unknown", {})
    totals = data.get("totals", {})
    lines.append(
        f"  unclassified {unknown.get('functions', 0):,} of {totals.get('functions', 0):,} "
        f"(MEASURED {totals.get('measured', 0):,}, INFERRED {totals.get('inferred', 0):,})"
    )
    return lines


@dataclass
class MilestoneProgress:
    milestone: roadmap_doc.Milestone
    state: str
    done: int
    known: int
    percent: float
    who: list[str]


def live_owners(claims: list[Claim]) -> dict[str, set[str]]:
    owners: dict[str, set[str]] = {}
    for claim in claims:
        if claim.alive and not claim.planning:
            owners.setdefault(claim.task, set()).add(claim.owner)
    return owners


def milestone_progress(
    roadmap: roadmap_doc.Roadmap, states: dict[str, str], claims: list[Claim]
) -> list[MilestoneProgress]:
    """Per milestone: ledger-derived progress, state and the sessions working on it.

    Ordered by milestone priority, then roadmap file order.
    """
    owners = live_owners(claims)
    result = []
    for milestone in roadmap.milestones:
        known = [task for task in milestone.tasks if task in states]
        done = sum(1 for task in known if states[task] == "x")
        flying = [task for task in known if states[task] == "~" or task in owners]
        blocked = [task for task in known if states[task] == "!"]
        percent = 100 * done / len(known) if known else 0.0
        if known and done == len(known):
            state = "done"
        elif flying:
            state = "active"
        elif blocked and len(blocked) + done == len(known):
            state = "blocked"
        else:
            state = "pending"
        who = sorted({owner for task in milestone.tasks for owner in owners.get(task, ())})
        result.append(MilestoneProgress(milestone, state, done, len(known), percent, who))
    mid = taskmeta.PRIORITY_RANK["mid"]
    result.sort(key=lambda item: taskmeta.PRIORITY_RANK.get(item.milestone.priority, mid))
    return result


def describe_blockers(blockers: dict[str, str | None]) -> str:
    """`T4 [ ], T9 [!], T99 (not in ledger)`, the format of `tasks next`'s waiting list."""
    return ", ".join(
        f"{blocker} (not in ledger)" if state is None else f"{blocker} [{state}]"
        for blocker, state in blockers.items()
    )


def ready_and_waiting(
    ledger: taskmeta.Ledger,
    claims: list[Claim],
    milestone_priority: dict[str, str] | None,
) -> tuple[list[str], list[str], list[tuple[str, dict[str, str | None]]]]:
    """(ready, for-later, waiting) exactly as `tasks next` offers them (T1121).

    Waiting tasks are open leaves nobody holds whose `blocked-by` has not landed. They are
    not ready, the same call and the same claimed set give the same three lists.
    """
    claimed = set(live_owners(claims))
    ready, later = taskmeta.claimable(
        ledger, claimed, milestone_priority=milestone_priority, skip_waiting=True
    )
    return ready, later, taskmeta.waiting(ledger, claimed)


def queue_row(ledger: taskmeta.Ledger, milestone_priority: dict[str, str], task_id: str) -> str:
    priority = ledger.resolve(task_id, "priority") or "mid"
    milestone = ledger.resolve(task_id, "milestone") or "-"
    shown = milestone_priority.get(milestone) or "-"
    title = ledger.tasks[task_id].title[:60]
    return f"  {task_id:<7}{shown:<10}{priority:<10}{milestone:<5}{title}"


def render_next_up(
    roadmap: roadmap_doc.Roadmap, claims: list[Claim], limit: int = NEXT_UP_LIMIT
) -> list[str]:
    """Claimable leaf tasks, best priority first, then the tasks waiting on a blocker."""
    ledger = roadmap.ledger
    if ledger is None:
        return []
    milestone_priority = roadmap_doc.milestone_priorities(roadmap)
    ready, later, waiting = ready_and_waiting(ledger, claims, milestone_priority)
    lines: list[str] = []
    if ready:
        lines += ["", "READY, BY PRIORITY"]
        lines += [queue_row(ledger, milestone_priority, task_id) for task_id in ready[:limit]]
        if len(ready) > limit:
            lines.append(f"  ... {len(ready) - limit} more: python3 -m tools.agents.tasks next")
        if later:
            lines.append(f"  {len(later)} for-later task(s) not listed")
    if waiting:
        lines += ["", "WAITING ON (blocked-by not landed, not offered)"]
        lines += [
            f"{queue_row(ledger, milestone_priority, task_id)}  <- {describe_blockers(blockers)}"
            for task_id, blockers in waiting[:limit]
        ]
        if len(waiting) > limit:
            lines.append(f"  ... {len(waiting) - limit} more: python3 -m tools.agents.tasks next")
    return lines


def render_roadmap(
    roadmap: roadmap_doc.Roadmap,
    states: dict[str, str],
    claims: list[Claim],
    use_colour: bool,
    width: int = DEFAULT_WIDTH,
    rollups: dict[str, Rollup] | None = None,
) -> list[str]:
    """Milestones with ledger-derived progress, who is on each, then where/next text.

    `rollups` (see `milestone_rollups`) adds ELAPSED and OWNERS EVER, blank without events.
    """
    lines = ["ROADMAP", *(f"  {text}" for text in roadmap.where)]
    rows = []
    for item in milestone_progress(roadmap, states, claims):
        row = [
            item.milestone.key,
            item.milestone.priority or "-",
            item.milestone.title,
            item.state,
            f"{bar(item.percent, 12)} {item.done}/{item.known}",
            ",".join(item.who),
        ]
        if rollups is not None:
            rollup = rollups.get(item.milestone.key)
            row += [
                format_span(rollup.elapsed) if rollup else "",
                clip(",".join(rollup.owners), OWNERS_LIMIT) if rollup else "",
            ]
        rows.append([*row, item.milestone.now])
    if rows:
        extra = ["ELAPSED", "OWNERS EVER"] if rollups is not None else []
        lines += render_table(
            ["MS", "PRI", "MILESTONE", "STATE", "PROGRESS", "SESSIONS", *extra, "NOW"],
            rows,
            use_colour,
            width,
            state_column=3,
        )
    if roadmap.next_steps:
        lines += [
            "",
            "NEXT STEPS",
            *(f"  {number}. {step}" for number, step in enumerate(roadmap.next_steps, 1)),
        ]
    lines += render_next_up(roadmap, claims)
    missing = roadmap_doc.unplaced_open(roadmap, states)
    if missing:
        lines += ["", f"  in-flight tasks in no milestone: {' '.join(missing)}"]
    return lines


def render(
    claims: list[Claim],
    metrics: dict | None,
    tasks: tuple[int, int] | None,
    use_colour: bool,
    width: int = DEFAULT_WIDTH,
    roadmap: roadmap_doc.Roadmap | None = None,
    states: dict[str, str] | None = None,
    registered: dict[str, dict] | None = None,
    rollups: dict[str, Rollup] | None = None,
    now: float | None = None,
    show_ended: bool = False,
    session_stale: int = SESSION_STALE_SECONDS,
    subsystems: dict | None = None,
) -> str:
    sections = []
    if roadmap is not None and states is not None:
        sections.append(render_roadmap(roadmap, states, claims, use_colour, width, rollups))
        work = render_work(roadmap.ledger, claims, use_colour, width)
        if work:
            sections.append(work)
    sections.append(
        render_sessions(claims, use_colour, width, registered, now, show_ended, session_stale)
    )
    if metrics is not None and tasks is not None:
        sections.append(render_progress(metrics, tasks[0], tasks[1], use_colour))
    if subsystems is not None:
        sections.append(render_subsystems(subsystems, use_colour))
    return "\n\n".join("\n".join(section) for section in sections)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Show active Claude and Codex sessions and overall progress."
    )
    parser.add_argument(
        "--stale", type=int, default=1200, help="seconds of silence after which a claim is dead"
    )
    parser.add_argument("--metrics", type=Path, default=DEFAULT_METRICS, help="badge metrics.json")
    parser.add_argument(
        "--subsystems",
        type=Path,
        default=DEFAULT_SUBSYSTEMS,
        help="generated per-subsystem coverage json (python -m tools.subsystems report --write)",
    )
    parser.add_argument("--tasks", type=Path, default=DEFAULT_TASKS, help="task ledger markdown")
    parser.add_argument("--roadmap", type=Path, default=DEFAULT_ROADMAP, help="roadmap markdown")
    parser.add_argument("--no-roadmap", action="store_true", help="hide the roadmap")
    parser.add_argument("--no-progress", action="store_true", help="sessions only")
    parser.add_argument("--no-colour", action="store_true", help="plain text")
    parser.add_argument(
        "--all-sessions",
        action="store_true",
        help="also list ended sessions (hidden by default, TUI key a toggles)",
    )
    parser.add_argument(
        "--session-stale",
        type=int,
        default=SESSION_STALE_SECONDS,
        help="seconds without any coord command after which a session without a claim is ended",
    )
    parser.add_argument(
        "--watch",
        type=int,
        metavar="SECONDS",
        help="interactive TUI refreshing every SECONDS (plain redraw loop when not a TTY)",
    )
    return parser


def optional(action: Callable[[], T], fallback: T) -> T:
    """Run a read that may fail (no coord events, no sessions): blanks, never a crash."""
    try:
        return action()
    except Exception:  # a missing audit source only blanks its columns
        return fallback


def snapshot(args: argparse.Namespace) -> str:
    claims = fetch_claims(args.stale)
    metrics = tasks = roadmap = states = rollups = subsystems = None
    registered = optional(read_sessions, {})
    if not args.no_roadmap:
        roadmap, states = roadmap_doc.load(args.roadmap, args.tasks)
        events = optional(fetch_events, [])
        now = time.time()
        rollups = optional(
            lambda: milestone_rollups(
                roadmap, task_audits(events, claims, now), events, states, now
            ),
            {},
        )
    if not args.no_progress:
        metrics = load_metrics(args.metrics)
        tasks = count_tasks(args.tasks)
        subsystems = load_subsystems(getattr(args, "subsystems", DEFAULT_SUBSYSTEMS))
    width = shutil.get_terminal_size((DEFAULT_WIDTH, 24)).columns
    use_colour = not args.no_colour and sys.stdout.isatty()
    return render(
        claims,
        metrics,
        tasks,
        use_colour,
        width,
        roadmap,
        states,
        registered,
        rollups,
        None,
        getattr(args, "all_sessions", False),
        getattr(args, "session_stale", SESSION_STALE_SECONDS),
        subsystems=subsystems,
    )


def main() -> int:
    args = build_parser().parse_args()
    if args.watch is None:
        print(snapshot(args))
        return 0
    if sys.stdout.isatty() and sys.stdin.isatty():
        from tools.agents import status_tui

        return status_tui.run(args)
    try:
        while True:
            frame = snapshot(args)
            print(
                "\033[2J\033[H" + frame + f"\n\nrefreshing every {args.watch}s, Ctrl-C to stop",
                flush=True,
            )
            time.sleep(args.watch)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
