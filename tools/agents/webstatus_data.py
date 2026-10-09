# SPDX-License-Identifier: GPL-3.0-or-later
"""JSON snapshot of the agent status for the web viewer (`tools/agents/webstatus.py`).

Pure of HTTP and read-only. `build_snapshot` maps what `status_tui.gather` reads (claims,
roadmap, ledger, badge metrics, messages, fleet, audit events) onto the seven tab views of the
TUI as sortable tables (the TUI's Audit tab has no web view of its own: its per-task summary is
a set of hidden Tasks columns, and the full audit and history are in `task_detail`). The shape
is the contract in
`docs/superpowers/specs/2026-10-05-milestone-priority-and-web-viewer-design.md`, extended
additively by T1046 (docs/t1046-status-audit-findings.md): progress, status text, agent id,
blocked-by and audit columns. A failing source never raises: it appears in `errors` and its
tables are empty.
"""

import argparse
import copy
import hashlib
import json
import re
import sys
import time
from typing import Any

from tools.agents import coord, fleet, status, status_tui, taskmeta
from tools.agents import roadmap as roadmap_doc

STATE_NAMES = {"x": "done", " ": "open", "~": "in-flight", "!": "blocked"}
COLUMN_TYPES = {
    "text",
    "number",
    "id",
    "state",
    "priority",
    "progress",
    "percent",
    "age",
    "bool",
    "session",
}
# The TUI-only Audit tab is not a web view (see the module docstring).
VIEW_IDS = tuple(tab.lower() for tab in status_tui.TABS if tab != "Audit")
DEFAULT_INTERVAL = 5
TASK_ID = re.compile(r"T\d+")
MESSAGE_TABLE_LIMIT = 50

Row = dict[str, Any]


def column(
    key: str, label: str, kind: str = "text", filter_: str = "text", hidden: bool = False
) -> dict[str, Any]:
    return {"key": key, "label": label, "type": kind, "filter": filter_, "hidden": hidden}


def table(
    table_id: str, title: str, columns: list[dict[str, Any]], rows: list[Row], tree: bool = False
) -> dict[str, Any]:
    return {"id": table_id, "title": title, "tree": tree, "columns": columns, "rows": rows}


def view(view_id: str, notes: list[dict[str, Any]], tables: list[dict[str, Any]]) -> dict[str, Any]:
    title = status_tui.TABS[VIEW_IDS.index(view_id)]
    return {"id": view_id, "title": title, "notes": notes, "tables": tables}


def version_of(snapshot: dict) -> str:
    """Content hash: `generated_at`, `version` and every `age` cell do not count."""
    clean = copy.deepcopy(
        {key: value for key, value in snapshot.items() if key not in ("generated_at", "version")}
    )
    for each in clean["views"]:
        for tbl in each["tables"]:
            ages = [col["key"] for col in tbl["columns"] if col["type"] == "age"]
            for row in tbl["rows"]:
                for key in ages:
                    if key in row:
                        row[key] = 0
    blob = json.dumps(clean, sort_keys=True, separators=(",", ":"))
    return hashlib.sha1(blob.encode()).hexdigest()[:12]


def detail_version(payload: dict[str, Any]) -> str:
    """Content hash of a detail payload: `generated_at`, `version` and age values do not count."""
    clean = copy.deepcopy(
        {key: value for key, value in payload.items() if key not in ("generated_at", "version")}
    )
    if isinstance(clean.get("claim"), dict):
        clean["claim"]["age"] = 0
    audit = clean.get("audit")
    if isinstance(audit, dict):  # the held, active, stale and blocked seconds grow while live
        for key in ("held_seconds", "active_seconds", "stale_seconds"):
            audit[key] = 0
        if isinstance(audit.get("blocks"), dict):
            audit["blocks"]["seconds"] = 0
    if isinstance(clean.get("freshest_age"), int | float):
        clean["freshest_age"] = 0
    if isinstance(clean.get("claim"), dict) and "progress_age" in clean["claim"]:
        clean["claim"]["progress_age"] = 0
    for name in ("table", "children_table", "claims", "messages", "progress_table"):
        tbl = clean.get(name)
        if isinstance(tbl, dict):
            ages = [col["key"] for col in tbl["columns"] if col["type"] == "age"]
            for row in tbl["rows"]:
                for key in ages:
                    if key in row:
                        row[key] = 0
    blob = json.dumps(clean, sort_keys=True, separators=(",", ":"))
    return hashlib.sha1(blob.encode()).hexdigest()[:12]


# Audit time columns (elapsed, held) are whole minutes of type "number", not "age": the page
# clock advances an "age" cell, which is wrong for a finished milestone or a released task.


def blocked_object(info: status.BlockedBy) -> dict[str, Any]:
    """Structured blocked-by of one task: each blocker with its checkbox state and live owner."""
    return {
        "blockers": [
            {
                "id": blocker.id,
                "state": STATE_NAMES.get(blocker.state, "unknown") if blocker.state else "missing",
                "in_ledger": blocker.state is not None,
                "owner": blocker.owner or None,
            }
            for blocker in info.blockers
        ],
        "external": info.external or None,
        "reason": info.reason or None,
        "landed": info.landed,
        "text": info.text,
    }


def audit_summary(audit: dict, stable: bool = False) -> dict[str, Any]:
    """The per-task audit rollup without the progress curve.

    `stable` drops the figures that grow while a claim is live (held, active and stale
    seconds, blocked seconds), so a row that carries it keeps its content hash between polls.
    """
    progress = audit["progress"]
    summary: dict[str, Any] = {
        "owners": audit["owners"],
        "claims": audit["claims"],
        "handoffs": audit["handoffs"],
        "replacements": audit["replacements"],
        "stale_liberations": audit["stale_liberations"],
        "blocks": {"count": audit["blocks"]["count"], "open": audit["blocks"]["open"]},
        "first_event_ts": audit["first_event_ts"],
        "last_event_ts": audit["last_event_ts"],
        "progress": {key: value for key, value in progress.items() if key != "curve"},
    }
    if not stable:
        summary["held_seconds"] = audit["held_seconds"]
        summary["active_seconds"] = audit["active_seconds"]
        summary["stale_seconds"] = audit["stale_seconds"]
        summary["blocks"] = audit["blocks"]
    return summary


def number_or_none(value: object) -> int | float | None:
    return value if isinstance(value, int | float) and not isinstance(value, bool) else None


def claimed_tasks(claims: list[status.Claim]) -> dict[str, status.Claim]:
    """The claim shown for each task: any non-planning claim, a live one before a stale one."""
    best: dict[str, status.Claim] = {}
    for claim in claims:
        if claim.planning:
            continue
        held = best.get(claim.task)
        if held is None or (not claim.alive, claim.age_seconds) < (
            not held.alive,
            held.age_seconds,
        ):
            best[claim.task] = claim
    return best


def claim_state(claim: status.Claim | None) -> str:
    return "" if claim is None else "live" if claim.alive else "stale"


def percent_cell(claim: status.Claim | None) -> dict[str, Any] | None:
    """The `percent` column cell of one claim, None when it is unreported and not overdue."""
    if claim is None or not (claim.reported or claim.no_report):
        return None
    return {
        "percent": claim.percent,
        "kind": claim.percent_kind if claim.percent is not None else "",
        "status": claim.status_text,
        "no_report": claim.no_report,
    }


def progress_age(claim: status.Claim | None) -> int | None:
    """Seconds since the last progress report, None when there is none."""
    if claim is None or claim.progress_at is None:
        return None
    return max(0, int(time.time()) - claim.progress_at)


def progress_values(claim: status.Claim | None) -> Row:
    """The three progress cells shared by every claim-bearing table."""
    return {
        "progress": percent_cell(claim),
        "progress_status": claim.status_text if claim else "",
        "progress_age": progress_age(claim),
    }


def progress_columns() -> list[dict[str, Any]]:
    return [
        column("progress", "Progress", "percent", "range"),
        column("progress_status", "Progress status"),
        column("progress_age", "Last report", "age", "range", hidden=True),
    ]


def progress_event_rows(events: list[dict], task_id: str) -> list[Row]:
    """One row per `progress` event of the task, chronological."""
    rows: list[Row] = []
    mine = [e for e in events if e.get("task") == task_id and e["kind"] == "progress"]
    for index, event in enumerate(sorted(mine, key=lambda each: each["ts"])):
        detail = coord.detail_of(event)
        percent = detail.get("percent")
        kind = detail.get("percent_kind")
        slot = event.get("slot")
        rows.append(
            {
                "_key": f"{int(event['ts'])}-{index}",
                "time": time.strftime("%H:%M:%S", time.gmtime(event["ts"])),
                "owner": event.get("owner") or "",
                "slot": slot if isinstance(slot, int) else None,
                "percent": {
                    "percent": percent if isinstance(percent, int) else None,
                    "kind": kind if isinstance(kind, str) and percent is not None else "",
                    "status": str(detail.get("status") or ""),
                    "no_report": False,
                },
                "status": str(detail.get("status") or ""),
            }
        )
    return rows


def progress_table(rows: list[Row]) -> dict[str, Any]:
    columns = [
        column("time", "Time (UTC)"),
        column("owner", "Session", "session", "enum"),
        column("slot", "Slot", "number", "range"),
        column("percent", "Progress", "percent", "range"),
        column("status", "Status"),
    ]
    return table("progress", "Progress reports", columns, rows)


def claim_progress_cells(claim: status.Claim) -> dict[str, Any]:
    """PROGRESS, STATUS and `no report` of one claim, the same rules as the Tasks view."""
    return {
        "agent_id": claim.ident,
        "percent": claim.percent,
        "percent_kind": claim.percent_kind or None,
        "status_text": claim.status_text or None,
        "no_report": claim.state == status.NO_REPORT,
    }


def claim_audit_cells(audit: dict | None) -> dict[str, Any]:
    """Time held and handoffs of a claim's task from the one-read audit data, blank without it."""
    return {
        "held_min": audit["held_seconds"] // 60 if audit else None,
        "handoffs": audit["handoffs"] if audit else None,
    }


def in_cycle(ledger: taskmeta.Ledger, task_id: str) -> bool:
    seen: set[str] = set()
    current = ledger.tasks[task_id].parent
    while current in ledger.tasks and current not in seen:
        if current == task_id:
            return True
        seen.add(current)
        current = ledger.tasks[current].parent
    return False


def ancestors_of(ledger: taskmeta.Ledger, task_id: str) -> list[str]:
    """Parent chain that exists in the ledger, outermost first. Cycle-safe."""
    chain: list[str] = []
    current = ledger.tasks[task_id].parent
    while current in ledger.tasks and current != task_id and current not in chain:
        chain.append(current)
        current = ledger.tasks[current].parent
    return chain[::-1]


def where_paragraphs(text: str) -> list[str]:
    """One line per paragraph of `## Where we are`: wrapped source lines joined with a space.

    `parse_roadmap` drops the blank lines, so paragraph breaks are only in the raw text.
    A `- ` bullet starts its own paragraph.
    """
    paragraphs: list[list[str]] = []
    inside = False
    fresh = True
    for line in text.splitlines():
        if line.startswith("## "):
            inside = line[3:].strip() == "Where we are"
            fresh = True
        elif inside and not line.strip():
            fresh = True
        elif inside:
            if fresh or line.startswith("- "):
                paragraphs.append([])
            paragraphs[-1].append(line.strip())
            fresh = False
    return [" ".join(parts) for parts in paragraphs]


def where_lines(args: argparse.Namespace, roadmap: roadmap_doc.Roadmap | None) -> list[str]:
    if roadmap is None:
        return []
    try:
        return where_paragraphs(args.roadmap.read_text())
    except OSError:  # the roadmap parsed a moment ago: fall back to its unwrapped lines
        return list(roadmap.where)


def dashboard_view(
    data: status_tui.Data,
    ledger: taskmeta.Ledger | None,
    owners: dict[str, set[str]],
    ready: list[str] | None,
    later: list[str] | None,
    where: list[str],
    waiting: list[tuple[str, dict[str, str | None]]] | None = None,
) -> dict[str, Any]:
    roadmap = data.roadmap
    notes = [
        {"title": "Where we are", "lines": where},
        {"title": "Next steps", "lines": list(roadmap.next_steps) if roadmap else []},
    ]
    totals: list[tuple[str, int]] = []
    if data.claims is not None:
        sessions = status.group_sessions(data.claims)
        live = sum(1 for claim in data.claims if claim.alive)
        totals += [
            ("sessions", len(sessions)),
            ("active sessions", sum(1 for session in sessions if session.alive)),
            ("live claims", live),
            ("stale claims", len(data.claims) - live),
        ]
    if data.fleet is not None:
        totals.append(("fleet workers", len(data.fleet.get("workers", {}))))
    if ledger is not None:
        states = [task.state for task in ledger.tasks.values()]
        totals += [
            ("tasks done", states.count("x")),
            ("tasks open", len(states) - states.count("x")),
            ("tasks in flight", states.count("~")),
            ("tasks blocked", states.count("!")),
            ("tasks claimed", len(owners)),
        ]
    if ready is not None and later is not None:
        totals += [("tasks ready", len(ready)), ("tasks for later", len(later))]
    if waiting is not None:
        totals.append(("tasks waiting", len(waiting)))
    ready_rows: list[Row] = []
    if ledger is not None and ready is not None and data.roadmap is not None:
        priorities = roadmap_doc.milestone_priorities(data.roadmap)
        for task_id in ready:
            milestone = ledger.resolve(task_id, "milestone") or ""
            ready_rows.append(
                {
                    "_key": task_id,
                    "_task": task_id,
                    "id": task_id,
                    "milestone_priority": priorities.get(milestone, ""),
                    "priority": ledger.resolve(task_id, "priority") or "mid",
                    "milestone": milestone,
                    "title": ledger.tasks[task_id].title,
                }
            )
    waiting_rows: list[Row] = []
    if ledger is not None and waiting is not None and data.roadmap is not None:
        priorities = roadmap_doc.milestone_priorities(data.roadmap)
        for task_id, blockers in waiting:
            milestone = ledger.resolve(task_id, "milestone") or ""
            waiting_rows.append(
                {
                    "_key": task_id,
                    "_task": task_id,
                    "id": task_id,
                    "milestone_priority": priorities.get(milestone, ""),
                    "priority": ledger.resolve(task_id, "priority") or "mid",
                    "milestone": milestone,
                    "title": ledger.tasks[task_id].title,
                    "waiting_on": status.describe_blockers(blockers),
                }
            )
    return view(
        "dashboard",
        notes,
        [
            table(
                "totals",
                "Totals",
                [column("metric", "Metric"), column("value", "Value", "number", "range")],
                [{"_key": name, "metric": name, "value": value} for name, value in totals],
            ),
            table(
                "ready",
                "Ready, by priority",
                [
                    column("id", "Task", "id"),
                    column("milestone_priority", "Milestone priority", "priority", "enum"),
                    column("priority", "Priority", "priority", "enum"),
                    column("milestone", "Milestone", "id", "enum"),
                    column("title", "Title"),
                ],
                ready_rows,
            ),
            table(
                "waiting",
                "Waiting on blockers (not offered)",
                [
                    column("id", "Task", "id"),
                    column("milestone_priority", "Milestone priority", "priority", "enum"),
                    column("priority", "Priority", "priority", "enum"),
                    column("milestone", "Milestone", "id", "enum"),
                    column("title", "Title"),
                    column("waiting_on", "Waiting on"),
                ],
                waiting_rows,
            ),
        ],
    )


def milestones_view(data: status_tui.Data) -> dict[str, Any]:
    rows: list[Row] = []
    if data.roadmap is not None and data.states is not None:
        for item in status.milestone_progress(data.roadmap, data.states, data.claims or []):
            milestone = item.milestone
            rollup = (data.rollups or {}).get(milestone.key)
            rows.append(
                {
                    "_key": milestone.key,
                    "id": milestone.key,
                    "title": milestone.title,
                    "priority": milestone.priority,
                    "state": item.state,
                    "progress": {"done": item.done, "total": item.known},
                    "sessions": ",".join(item.who),
                    "elapsed_min": rollup.elapsed // 60 if rollup else None,
                    "owners_ever": ",".join(rollup.owners) if rollup else "",
                    "now": milestone.now,
                    "goal": milestone.goal,
                    "exit": milestone.exit,
                }
            )
    columns = [
        column("id", "Milestone", "id"),
        column("title", "Title"),
        column("priority", "Priority", "priority", "enum"),
        column("state", "State", "state", "enum"),
        column("progress", "Progress", "progress", "range"),
        column("sessions", "Sessions"),
        column("elapsed_min", "Elapsed (min)", "number", "range"),
        column("owners_ever", "Owners ever"),
        column("now", "Now"),
        column("goal", "Goal", hidden=True),
        column("exit", "Exit", hidden=True),
    ]
    return view("milestones", [], [table("milestones", "Milestones", columns, rows)])


def tasks_view(
    ledger: taskmeta.Ledger | None,
    claims: list[status.Claim],
    priorities: dict[str, str] | None,
    ready: list[str] | None,
    audits: dict[str, dict] | None = None,
) -> dict[str, Any]:
    rows: list[Row] = []
    ready_ids = set(ready or [])
    held = claimed_tasks(claims)
    owners = status.live_owners(claims)
    if ledger is not None:
        for task in ledger.tasks.values():
            milestone = ledger.resolve(task.id, "milestone")
            claim = held.get(task.id)
            blocked = status.blocked_info(ledger, task.id, owners)
            audit = (audits or {}).get(task.id)
            row: Row = {
                "_key": task.id,
                "_task": task.id,
                "id": task.id,
                "state": STATE_NAMES.get(task.state, "open"),
                "title": task.title,
                "milestone": milestone,
                "milestone_priority": (
                    None if priorities is None else priorities.get(milestone or "", "")
                ),
                "priority": ledger.resolve(task.id, "priority") or "",
                "parent": task.parent,
                "claimed_by": held[task.id].owner if task.id in held else None,
                "claim_state": claim_state(held.get(task.id)),
                **progress_values(claim),
                "open_children": len(ledger.open_children(task.id)),
                "ready": None if ready is None else task.id in ready_ids,
                "percent": claim.percent if claim else None,
                "percent_kind": (claim.percent_kind or None) if claim else None,
                "status_text": (claim.status_text or None) if claim else None,
                "agent_id": claim.ident if claim else None,
                "no_report": claim.state == status.NO_REPORT if claim else None,
                "blocked_by": blocked.text if blocked else None,
                "owners_ever": ",".join(audit["owners"]) if audit else None,
                "held_min": audit["held_seconds"] // 60 if audit else None,
                "handoffs": audit["handoffs"] if audit else None,
                "audit_blocks": audit["blocks"]["count"] if audit else None,
            }
            if blocked:
                row["_blocked_by"] = blocked_object(blocked)
            if audit:
                row["_audit"] = audit_summary(audit, stable=True)
            if task.parent in ledger.tasks and not in_cycle(ledger, task.id):
                row["_parent"] = task.parent
            rows.append(row)
    columns = [
        column("id", "Task", "id"),
        column("state", "State", "state", "enum"),
        column("title", "Title"),
        column("milestone", "Milestone", "id", "enum"),
        column("milestone_priority", "Milestone priority", "priority", "enum"),
        column("priority", "Priority", "priority", "enum"),
        column("parent", "Parent", "id"),
        column("claimed_by", "Claimed by", "session", "enum"),
        column("claim_state", "Claim", "text", "enum"),
        *progress_columns(),
        column("open_children", "Open sub-tasks", "number", "range"),
        column("ready", "Ready", "bool", "enum"),
        column("percent", "Percent", "number", "range", hidden=True),
        column("status_text", "Status", hidden=True),
        column("blocked_by", "Blocked by"),
        column("percent_kind", "Percent kind", "text", "enum", hidden=True),
        column("agent_id", "Agent id", hidden=True),
        column("no_report", "No report", "bool", "enum", hidden=True),
        column("owners_ever", "Owners ever", hidden=True),
        column("held_min", "Held (min)", "number", "range", hidden=True),
        column("handoffs", "Handoffs", "number", "range", hidden=True),
        column("audit_blocks", "Blocks logged", "number", "range", hidden=True),
    ]
    return view("tasks", [], [table("tasks", "Tasks", columns, rows, tree=True)])


def sessions_view(data: status_tui.Data, show_ended: bool = False) -> dict[str, Any]:
    session_rows: list[Row] = []
    claim_rows: list[Row] = []
    shown = status.group_sessions(
        data.claims or [], data.registered, data.fetched_at, show_ended, data.session_stale
    )
    hidden = (
        0
        if show_ended
        else status.hidden_ended_count(
            data.claims or [], data.registered, data.fetched_at, data.session_stale
        )
    )
    for session in shown:
        session_rows.append(
            {
                "_key": session.owner,
                "owner": session.owner,
                "family": session.family,
                "role": session.role or None,
                "alive": session.alive,
                "claims": len(session.claims),
                "freshest_age": session.freshest_age if session.claims else None,
                "tasks": " ".join(sorted(claim.task for claim in session.claims)),
                "state": session.state,
                "lifecycle": session.lifecycle or None,
                "last_seen_age": (
                    None
                    if session.last_seen is None
                    else max(0, int(data.fetched_at - session.last_seen))
                ),
            }
        )
        for claim in sorted(session.claims, key=lambda held: (held.slot, held.task)):
            row: Row = {
                "_key": claim.task,
                "task": claim.task,
                "owner": claim.owner,
                "slot": int(claim.slot) if claim.slot.isdigit() else None,
                "age": claim.age_seconds,
                "alive": claim.alive,
                "note": claim.note,
                "agent_id": claim.ident,
                "percent": claim.percent,
                "percent_kind": claim.percent_kind or None,
                "status_text": claim.status_text or None,
                "no_report": claim.state == status.NO_REPORT,
                **progress_values(claim),
                **claim_audit_cells((data.audits or {}).get(claim.task)),
            }
            if TASK_ID.fullmatch(claim.task):
                row["_task"] = claim.task
            claim_rows.append(row)
    notes = (
        [{"title": "Ended sessions", "lines": [f"{hidden} ended session(s) hidden"]}]
        if hidden
        else []
    )
    return view(
        "sessions",
        notes,
        [
            table(
                "sessions",
                "Sessions",
                [
                    column("owner", "Session", "session"),
                    column("family", "Family", "text", "enum"),
                    column("role", "Role", "text", "enum"),
                    column("state", "State", "state", "enum"),
                    column("alive", "Active", "bool", "enum"),
                    column("claims", "Claims", "number", "range"),
                    column("freshest_age", "Last beat", "age", "range"),
                    column("last_seen_age", "Last seen", "age", "range"),
                    column("tasks", "Tasks"),
                    column("lifecycle", "Lifecycle", "text", "enum", hidden=True),
                ],
                session_rows,
            ),
            table(
                "claims",
                "Claims",
                [
                    column("task", "Task", "id"),
                    column("owner", "Session", "session", "enum"),
                    column("slot", "Slot", "number", "range"),
                    column("age", "Age", "age", "range"),
                    column("alive", "Alive", "bool", "enum"),
                    column("note", "Note"),
                    column("agent_id", "Agent id"),
                    column("percent", "Percent", "number", "range", hidden=True),
                    column("percent_kind", "Percent kind", "text", "enum", hidden=True),
                    column("status_text", "Status", hidden=True),
                    column("no_report", "No report", "bool", "enum", hidden=True),
                    *progress_columns(),
                    *audit_columns(),
                ],
                claim_rows,
            ),
        ],
    )


def audit_columns() -> list[dict[str, Any]]:
    """Hidden audit figures of a claim row: held minutes and handoffs."""
    return [
        column("held_min", "Held (min)", "number", "range", hidden=True),
        column("handoffs", "Handoffs", "number", "range", hidden=True),
    ]


def last_tick_text(state: dict, now: float) -> str:
    last = state.get("last_tick", 0)
    if not last:
        return "never"
    stamp = time.strftime("%H:%M:%S UTC", time.gmtime(last))
    return f"{stamp} STALE" if now - last > fleet.TICK_STALE_SECONDS else stamp


def workers_view(data: status_tui.Data, now: float) -> dict[str, Any]:
    notes: list[dict[str, Any]] = []
    rows: list[Row] = []
    if data.fleet is not None:
        notes.append(
            {
                "title": "Supervisor",
                "lines": [
                    f"supervisor {data.fleet.get('supervisor', '?')}, "
                    f"last tick {last_tick_text(data.fleet, now)}",
                    "process state is not queried: read-only",
                ],
            }
        )
        by_claim = {(claim.owner, claim.task): claim for claim in data.claims or []}
        ages = {key: claim.age_seconds for key, claim in by_claim.items()}
        for name, worker in sorted(data.fleet.get("workers", {}).items()):
            assigned = worker.get("assigned", {})
            held = [ages[(name, task)] for task in assigned if (name, task) in ages]
            lifecycle = status.session_lifecycle(
                name, data.claims or [], data.registered, now, data.session_stale
            )
            rows.append(
                {
                    "_key": name,
                    "lifecycle": lifecycle or None,
                    "worker": name,
                    "backend": str(worker.get("backend", "claude")),
                    "session": str(worker.get("session", "?")),
                    "generation": number_or_none(worker.get("generation")),
                    "assigned": " ".join(
                        f"{task}(slot {info.get('slot', '?')})"
                        for task, info in sorted(assigned.items())
                    )
                    or "idle",
                    "oldest_claim_age": max(held) if held else None,
                    "agent_ids": " ".join(
                        by_claim[(name, task)].ident
                        for task in sorted(assigned)
                        if (name, task) in by_claim
                    )
                    or None,
                }
            )
    columns = [
        column("worker", "Worker", "session"),
        column("backend", "Backend", "text", "enum"),
        column("session", "Session"),
        column("generation", "Generation", "number", "range"),
        column("lifecycle", "Session state", "state", "enum"),
        column("assigned", "Assigned"),
        column("oldest_claim_age", "Oldest claim", "age", "range"),
        column("agent_ids", "Agent ids"),
    ]
    return view("workers", notes, [table("workers", "Workers", columns, rows)])


def message_rows(messages: list[status_tui.Message]) -> list[Row]:
    rows: list[Row] = []
    seen: dict[str, int] = {}
    for message in messages:
        digest = hashlib.sha1(
            f"{message.stamp}\0{message.sender}\0{message.to}\0{message.text}".encode()
        ).hexdigest()[:10]
        seen[digest] = seen.get(digest, 0) + 1
        rows.append(
            {
                "_key": f"{digest}-{seen[digest]}",
                "time": message.stamp,
                "from": message.sender,
                "to": message.to,
                "text": message.text,
            }
        )
    return rows


def message_columns() -> list[dict[str, Any]]:
    return [
        column("time", "Time"),
        column("from", "From", "session", "enum"),
        column("to", "To", "session", "enum"),
        column("text", "Message"),
    ]


def messages_view(data: status_tui.Data) -> dict[str, Any]:
    rows = message_rows(data.messages or [])
    return view(
        "messages",
        [],
        [table("messages", "Messages (newest last)", message_columns(), rows)],
    )


def progress_view(data: status_tui.Data) -> dict[str, Any]:
    notes: list[dict[str, Any]] = []
    badge_rows: list[Row] = []
    if data.metrics is not None:
        notes.append(
            {
                "title": "Badges",
                "lines": [
                    f"badges measured {data.metrics.get('measured_on', '?')} "
                    f"at {data.metrics.get('commit', '?')}"
                ],
            }
        )
        entries = data.metrics.get("metrics", {})
        for key in sorted(entries):
            entry = entries[key]
            known = bool(entry.get("known", False))
            badge_rows.append(
                {
                    "_key": key,
                    "label": str(entry.get("label", key)),
                    "value": str(entry.get("value", "unknown")) if known else "unknown",
                    "percent": number_or_none(entry.get("percent")) if known else None,
                    "known": known,
                }
            )
    done_rows: list[Row] = []
    if data.counts is not None:
        done, open_ = data.counts
        total = done + open_
        done_rows.append(
            {
                "_key": "tasks",
                "done": done,
                "open": open_,
                "percent": round(100 * done / total, 1) if total else 0.0,
            }
        )
    return view(
        "progress",
        notes,
        [
            table(
                "badges",
                "Badges",
                [
                    column("label", "Badge"),
                    column("value", "Value"),
                    column("percent", "Percent", "number", "range"),
                    column("known", "Measured", "bool", "enum"),
                ],
                badge_rows,
            ),
            table(
                "tasks_done",
                "Tasks done",
                [
                    column("done", "Done", "number", "range"),
                    column("open", "Open", "number", "range"),
                    column("percent", "Percent", "number", "range"),
                ],
                done_rows,
            ),
        ],
    )


def ledger_of(
    data: status_tui.Data, args: argparse.Namespace, errors: dict[str, str]
) -> taskmeta.Ledger | None:
    """The parsed ledger, reusing the roadmap's copy; a read failure goes into `errors`."""
    if data.roadmap is not None and data.roadmap.ledger is not None:
        return data.roadmap.ledger
    if "tasks" in errors:
        return None
    try:
        return taskmeta.parse(args.tasks.read_text())
    except Exception as error:  # reported in the snapshot, the viewer must keep serving
        errors["tasks"] = f"{type(error).__name__}: {error}"
        return None


def build_snapshot(args: argparse.Namespace, now: float | None = None) -> dict[str, Any]:
    data = status_tui.gather(args)
    generated_at = time.time() if now is None else now
    errors = dict(data.errors)
    ledger = ledger_of(data, args, errors)
    owners = status.live_owners(data.claims or [])
    priorities = (
        roadmap_doc.milestone_priorities(data.roadmap) if data.roadmap is not None else None
    )
    ready: list[str] | None = None
    later: list[str] | None = None
    waiting: list[tuple[str, dict[str, str | None]]] | None = None
    if ledger is not None and priorities is not None:
        ready, later, waiting = status.ready_and_waiting(ledger, data.claims or [], priorities)
    snapshot: dict[str, Any] = {
        "version": "",
        "generated_at": generated_at,
        "interval": getattr(args, "interval", DEFAULT_INTERVAL),
        "errors": errors,
        "views": [
            dashboard_view(
                data, ledger, owners, ready, later, where_lines(args, data.roadmap), waiting
            ),
            milestones_view(data),
            tasks_view(ledger, data.claims or [], priorities, ready, data.audits),
            sessions_view(data, getattr(args, "all_sessions", False)),
            workers_view(data, generated_at),
            messages_view(data),
            progress_view(data),
        ],
    }
    snapshot["version"] = version_of(snapshot)
    return snapshot


def milestone_detail(args: argparse.Namespace, key: str) -> dict[str, Any] | None:
    """One milestone with its scoped tasks table, None when the roadmap has no such milestone."""
    data = status_tui.gather(args)
    errors = dict(data.errors)
    ledger = ledger_of(data, args, errors)
    if data.roadmap is None or data.states is None or ledger is None:
        return None
    item = next(
        (
            each
            for each in status.milestone_progress(data.roadmap, data.states, data.claims or [])
            if each.milestone.key == key
        ),
        None,
    )
    if item is None:
        return None
    priorities = roadmap_doc.milestone_priorities(data.roadmap)
    ready, _, _ = status.ready_and_waiting(ledger, data.claims or [], priorities)
    full = tasks_view(ledger, data.claims or [], priorities, ready, data.audits)["tables"][0]
    rows = [row for row in full["rows"] if row["milestone"] == key]
    members = {row["id"] for row in rows}
    for row in rows:
        if row.get("_parent") not in members:
            row.pop("_parent", None)
    milestone = item.milestone
    payload: dict[str, Any] = {
        "id": milestone.key,
        "title": milestone.title,
        "priority": milestone.priority,
        "state": item.state,
        "progress": {"done": item.done, "total": item.known},
        "goal": milestone.goal,
        "now": milestone.now,
        "exit": milestone.exit,
        "sessions": item.who,
        "ready": sum(1 for task_id in ready if ledger.resolve(task_id, "milestone") == key),
        "table": {**full, "rows": rows},
        "errors": errors,
        "version": "",
        "generated_at": time.time(),
    }
    payload["version"] = detail_version(payload)
    return payload


def children_table(
    ledger: taskmeta.Ledger,
    task: taskmeta.Task,
    claims: list[status.Claim],
    priorities: dict[str, str] | None,
    audits: dict[str, dict] | None = None,
) -> dict[str, Any]:
    """The tasks table scoped to the descendants of `task`: its direct children are the roots."""
    seen = {task.id}
    pending = list(task.children)
    while pending:
        current = pending.pop()
        if current in ledger.tasks and current not in seen:
            seen.add(current)
            pending.extend(ledger.tasks[current].children)
    seen.discard(task.id)
    ready, _, _ = status.ready_and_waiting(ledger, claims, priorities)
    full = tasks_view(ledger, claims, priorities, ready, audits)["tables"][0]
    rows = [row for row in full["rows"] if row["id"] in seen]
    for row in rows:
        if row.get("_parent") not in seen:
            row.pop("_parent", None)
    return {**full, "id": "children", "title": "Children", "rows": rows}


def task_detail(args: argparse.Namespace, task_id: str) -> dict[str, Any] | None:
    """Ledger entry, family and claim of one task, None when the ledger has no such id."""
    ledger = taskmeta.parse(args.tasks.read_text())
    task = ledger.tasks.get(task_id)
    if task is None:
        return None
    errors: dict[str, str] = {}
    claim = None
    held: dict[str, status.Claim] = {}
    claims: list[status.Claim] = []
    try:
        claims = status.fetch_claims(args.stale)
        held = claimed_tasks(claims)
        if task_id in held:
            claim = {
                "owner": held[task_id].owner,
                "slot": held[task_id].slot,
                "age": held[task_id].age_seconds,
                "alive": held[task_id].alive,
                "state": claim_state(held[task_id]),
                "note": held[task_id].note,
                "agent_id": held[task_id].ident,
                "percent": held[task_id].percent,
                "percent_kind": held[task_id].percent_kind or None,
                "status_text": held[task_id].status_text or None,
                "no_report": held[task_id].state == status.NO_REPORT,
                "progress_age": progress_age(held[task_id]),
            }
    except Exception as error:  # the entry is still worth showing without its claim
        errors["claims"] = f"{type(error).__name__}: {error}"
    blocked = status.blocked_info(ledger, task_id, status.live_owners(claims))
    history: list[dict[str, Any]] = []
    progress_rows: list[Row] = []
    audit = None
    audits: dict[str, dict] | None = None
    now = time.time()
    try:
        events = status.fetch_events()
        progress_rows = progress_event_rows(events, task_id)
        mine = sorted((e for e in events if e.get("task") == task_id), key=lambda e: e["ts"])
        history = [
            {"ts": e["ts"], "owner": e.get("owner"), "kind": e["kind"], "detail": e.get("detail")}
            for e in mine
        ]
        audits = status.task_audits(events, claims, now)
        found = audits.get(task_id)
        audit = audit_summary(found) if found else None
    except Exception as error:  # the entry is still worth showing without its audit
        errors["events"] = f"{type(error).__name__}: {error}"
    titles: dict[str, str] = {}
    priorities: dict[str, str] = {}
    known_priorities: dict[str, str] | None = None
    if not getattr(args, "no_roadmap", False):
        try:
            roadmap = roadmap_doc.parse_roadmap(args.roadmap.read_text())
            titles = {each.key: each.title for each in roadmap.milestones}
            priorities = roadmap_doc.milestone_priorities(roadmap)
            known_priorities = priorities
        except Exception as error:  # the entry is still worth showing without milestone names
            errors["roadmap"] = f"{type(error).__name__}: {error}"
    milestone = ledger.resolve(task.id, "milestone")
    ancestors = ancestors_of(ledger, task.id)
    children = sorted(task.children, key=taskmeta.numeric)
    detail: dict[str, Any] = {
        "id": task.id,
        "state": STATE_NAMES.get(task.state, "open"),
        "title": task.title,
        "milestone": milestone,
        "milestone_title": titles.get(milestone or "", ""),
        "milestone_priority": priorities.get(milestone or "", ""),
        "priority": ledger.resolve(task.id, "priority") or "",
        "parent": task.parent,
        "ancestors": ancestors,
        "ancestor_titles": {each: ledger.tasks[each].title for each in ancestors},
        "children": [
            {
                "id": child,
                "state": STATE_NAMES.get(ledger.tasks[child].state, "open"),
                "title": ledger.tasks[child].title,
                "priority": ledger.resolve(child, "priority") or "",
                "milestone": ledger.resolve(child, "milestone"),
                "claimed_by": held[child].owner if child in held else None,
                "claim_state": claim_state(held.get(child)),
            }
            for child in children
        ],
        "children_table": children_table(ledger, task, claims, known_priorities, audits),
        "claim": claim,
        "progress_table": progress_table(progress_rows),
        "blocked_by": blocked_object(blocked) if blocked else None,
        "audit": audit,
        "history": history,
        "body": [line.rstrip() for line in ledger.lines[task.start : task.end]],
        "errors": errors,
        "version": "",
        "generated_at": time.time(),
    }
    detail["version"] = detail_version(detail)
    return detail


def session_claim_rows(
    claims: list[status.Claim],
    ledger: taskmeta.Ledger | None,
    audits: dict[str, dict] | None = None,
) -> list[Row]:
    rows: list[Row] = []
    for claim in sorted(claims, key=lambda held: (held.slot, held.task)):
        task = ledger.tasks.get(claim.task) if ledger is not None else None
        row: Row = {
            "_key": claim.task,
            "task": claim.task,
            "title": task.title if task else "",
            "state": STATE_NAMES.get(task.state, "open") if task else None,
            "priority": (ledger.resolve(task.id, "priority") or "") if ledger and task else None,
            "milestone": ledger.resolve(task.id, "milestone") if ledger and task else None,
            "slot": int(claim.slot) if claim.slot.isdigit() else None,
            "age": claim.age_seconds,
            "claim_state": "planning" if claim.planning else claim_state(claim),
            **progress_values(claim),
            "note": claim.note,
            **claim_progress_cells(claim),
            **claim_audit_cells((audits or {}).get(claim.task)),
        }
        if task:
            row["_task"] = claim.task
        rows.append(row)
    return rows


def session_detail(args: argparse.Namespace, owner: str) -> dict[str, Any] | None:
    """One session with its claims, messages and fleet worker, None when nothing names it.

    A registered session counts as named: the Sessions table lists it and links to this page.
    """
    errors: dict[str, str] = {}
    claims: list[status.Claim] = []
    messages: list[status_tui.Message] = []
    worker: dict[str, Any] | None = None
    try:
        claims = [claim for claim in status.fetch_claims(args.stale) if claim.owner == owner]
    except Exception as error:  # the page is still worth showing without its claims
        errors["claims"] = f"{type(error).__name__}: {error}"
    try:
        log = coord.coord_dir() / "messages.jsonl"
        parsed = (
            status_tui.parse_messages(log.read_text(), limit=sys.maxsize) if log.exists() else []
        )
        messages = [each for each in parsed if owner in (each.sender, each.to)]
    except Exception as error:
        errors["messages"] = f"{type(error).__name__}: {error}"
    try:
        entry = fleet.load_state().get("workers", {}).get(owner)
        if entry is not None:
            worker = {
                "backend": str(entry.get("backend", "claude")),
                "generation": number_or_none(entry.get("generation")),
                "assigned": sorted(entry.get("assigned", {})),
            }
    except Exception as error:
        errors["fleet"] = f"{type(error).__name__}: {error}"
    try:
        registered = owner in status.read_sessions()
    except Exception as error:
        registered = False
        errors["sessions"] = f"{type(error).__name__}: {error}"
    if not claims and not messages and worker is None and not registered:
        return None
    ledger: taskmeta.Ledger | None = None
    try:
        ledger = taskmeta.parse(args.tasks.read_text())
    except Exception as error:  # titles are optional
        errors["tasks"] = f"{type(error).__name__}: {error}"
    audits: dict[str, dict] | None = None
    now = time.time()
    try:
        audits = status.task_audits(status.fetch_events(), claims, now)
    except Exception as error:  # the progress cells stay, the audit cells go blank
        errors["events"] = f"{type(error).__name__}: {error}"
    live = sum(1 for claim in claims if claim.alive)
    payload: dict[str, Any] = {
        "owner": owner,
        "family": owner.split("-", 1)[0],
        "alive": live > 0,
        "live_claims": live,
        "stale_claims": len(claims) - live,
        "freshest_age": min((claim.age_seconds for claim in claims), default=None),
        "worker": worker,
        "claims": table(
            "claims",
            "Claims",
            [
                column("task", "Task", "id"),
                column("title", "Title"),
                column("state", "State", "state", "enum"),
                column("priority", "Priority", "priority", "enum"),
                column("milestone", "Milestone", "id", "enum"),
                column("slot", "Slot", "number", "range"),
                column("age", "Age", "age", "range"),
                column("claim_state", "Claim", "text", "enum"),
                *progress_columns(),
                column("note", "Note"),
                column("agent_id", "Agent id", hidden=True),
                column("percent", "Percent", "number", "range", hidden=True),
                column("percent_kind", "Percent kind", "text", "enum", hidden=True),
                column("status_text", "Status", hidden=True),
                column("no_report", "No report", "bool", "enum", hidden=True),
                *audit_columns(),
            ],
            session_claim_rows(claims, ledger, audits),
        ),
        "messages": table(
            "messages",
            f"Messages of {owner} (newest last)",
            message_columns(),
            message_rows(messages[-MESSAGE_TABLE_LIMIT:]),
        ),
        "errors": errors,
        "version": "",
        "generated_at": time.time(),
    }
    payload["version"] = detail_version(payload)
    return payload
