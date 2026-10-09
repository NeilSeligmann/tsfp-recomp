# SPDX-License-Identifier: GPL-3.0-or-later
"""Structured metadata for the entries of `docs/tasks.md`.

An entry may carry one indented line `meta: milestone=M5 priority=high parent=T120`.
The same line carries `blocked-by=T12,T34` (comma list, no spaces) and the external form
`blocked-external=<slug>`; an optional second indented line `blocked-reason: free text`
carries the human reason (see docs/t1045-blocked-by-findings.md). The entry block runs
from its `- [ ] **T<n>` heading to the next checkbox bullet or markdown heading.
Milestone and priority are inherited through `parent` links.
This module is pure: it reads and returns text, never touches files or git.
"""

import re
from collections.abc import Collection, Mapping
from dataclasses import dataclass, field

TASK_LINE = re.compile(r"^- \[(.)\] \*\*(T\d+)\b", re.MULTILINE)
HEADING = re.compile(TASK_LINE.pattern)
BLOCK_END = re.compile(r"^(- \[.\] |#{1,6} )")
META_LINE = re.compile(r"^\s+meta:\s*(.*\S)\s*$")
REASON_LINE = re.compile(r"^\s+blocked-reason:\s*(.*\S)\s*$")
BLOCKER_LIST = re.compile(r"T\d+(,T\d+)*")
EXTERNAL_SLUG = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
MILESTONE_ID = re.compile(r"M\d+")
TASK_ID = re.compile(r"T\d+")
PRIORITIES = ("max", "high", "mid", "low", "for-later")
PRIORITY_RANK = {name: rank for rank, name in enumerate(PRIORITIES)}
LEGACY_MILESTONE = "M0"
META_KEYS = ("milestone", "priority", "parent", "blocked-by", "blocked-external")
REASON_KEY = "blocked-reason"
BLOCKED_STATE = "!"


def numeric(task_id: str) -> int:
    return int(task_id[1:])


def attr_of(key: str) -> str:
    return key.replace("-", "_")


def parse_grandfathered(text: str) -> tuple[list[str], list[str]]:
    """Task ids of a grandfather file (one per line, `#` comments) and its format errors."""
    ids: list[str] = []
    errors: list[str] = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.partition("#")[0].strip()
        if not line:
            continue
        if not TASK_ID.fullmatch(line):
            errors.append(f"grandfather file line {number}: {line!r} is not a task id T<n>")
        elif line in ids:
            errors.append(f"grandfather file line {number}: {line} is listed twice")
        else:
            ids.append(line)
    return ids, errors


@dataclass
class Task:
    id: str
    state: str
    title: str
    start: int
    end: int
    meta_index: int | None = None
    milestone: str | None = None
    priority: str | None = None
    parent: str | None = None
    blocked_by: list[str] = field(default_factory=list)
    blocked_external: str | None = None
    blocked_reason: str | None = None
    reason_index: int | None = None
    children: list[str] = field(default_factory=list)

    @property
    def is_open(self) -> bool:
        return self.state != "x"

    @property
    def has_block_reason(self) -> bool:
        """A `[!]` entry is explained by blockers, or by an external slug plus a reason line."""
        return bool(self.blocked_by or (self.blocked_external and self.blocked_reason))

    def meta_value(self, key: str) -> str | None:
        value = getattr(self, attr_of(key))
        return ",".join(value) or None if isinstance(value, list) else value


@dataclass
class Ledger:
    lines: list[str]
    tasks: dict[str, Task]
    syntax: list[str]

    def resolve(self, task_id: str, attr: str) -> str | None:
        """Own value of `attr`, else the nearest ancestor's. Cycle-safe."""
        seen: set[str] = set()
        current = self.tasks.get(task_id)
        while current is not None and current.id not in seen:
            value = getattr(current, attr)
            if value:
                return value
            seen.add(current.id)
            current = self.tasks.get(current.parent) if current.parent else None
        return None

    def states(self) -> dict[str, str]:
        return {task.id: task.state for task in self.tasks.values()}

    def open_children(self, task_id: str) -> list[str]:
        children = self.tasks[task_id].children
        return sorted((c for c in children if self.tasks[c].is_open), key=numeric)

    def blocker_states(self, task_id: str) -> dict[str, str | None]:
        """Each blocker of `task_id` to its checkbox char, None when not in the ledger."""
        return {
            blocker: (self.tasks[blocker].state if blocker in self.tasks else None)
            for blocker in self.tasks[task_id].blocked_by
        }

    def unlanded_blockers(self, task_id: str) -> dict[str, str | None]:
        """The blockers of `task_id` that are not `[x]` (an unknown id counts as not landed)."""
        return {b: s for b, s in self.blocker_states(task_id).items() if s != "x"}

    def blockers_landed(self, task_id: str) -> bool:
        states = self.blocker_states(task_id)
        return bool(states) and all(state == "x" for state in states.values())

    def landed_blockers(self) -> list[str]:
        """`[!]` tasks whose listed blockers are all `[x]`: ready to reopen."""
        return sorted(
            (
                t.id
                for t in self.tasks.values()
                if t.state == BLOCKED_STATE and self.blockers_landed(t.id)
            ),
            key=numeric,
        )

    def _in_blocker_cycle(self, task_id: str) -> bool:
        seen: set[str] = set()
        pending = list(self.tasks[task_id].blocked_by)
        while pending:
            current = pending.pop()
            if current == task_id:
                return True
            if current in seen or current not in self.tasks:
                continue
            seen.add(current)
            pending.extend(self.tasks[current].blocked_by)
        return False

    def _in_cycle(self, task_id: str) -> bool:
        seen: set[str] = set()
        current = self.tasks[task_id].parent
        while current in self.tasks:
            if current == task_id:
                return True
            if current in seen:
                return False
            seen.add(current)
            current = self.tasks[current].parent
        return False

    def notes(self, grandfathered: Collection[str] = ()) -> list[str]:
        """Non-failing observations: landed blockers and stale grandfather entries."""
        found = [f"note: {task_id} blocker landed, reopen" for task_id in self.landed_blockers()]
        for task_id in sorted(grandfathered, key=numeric):
            task = self.tasks.get(task_id)
            if task is not None and task.state != BLOCKED_STATE:
                found.append(
                    f"note: {task_id} is grandfathered but no longer [{BLOCKED_STATE}], "
                    "remove it from the grandfather file"
                )
        return found

    def _blocked_problems(self, task: Task, grandfathered: Collection[str]) -> list[str]:
        found: list[str] = []
        for blocker in task.blocked_by:
            if blocker not in self.tasks:
                found.append(f"{task.id} blocked-by {blocker} is not in the ledger")
        if task.blocked_by and self._in_blocker_cycle(task.id):
            found.append(f"{task.id} is in a blocked-by cycle")
        if task.state == "x" and (task.blocked_by or task.blocked_external or task.blocked_reason):
            found.append(f"{task.id} is [x] but still carries blocked metadata, clear it (stale)")
        if task.blocked_reason and not (task.blocked_by or task.blocked_external):
            found.append(f"{task.id} has blocked-reason without blocked-by or blocked-external")
        if (
            task.state == BLOCKED_STATE
            and not task.has_block_reason
            and task.id not in grandfathered
        ):
            found.append(
                f"{task.id} is [{BLOCKED_STATE}] with no blocked-by, and no "
                "blocked-external plus blocked-reason line"
            )
        return found

    def problems(self, milestones: set[str], grandfathered: Collection[str] = ()) -> list[str]:
        found = list(self.syntax)
        found += [
            f"grandfathered {task_id} is not in the ledger"
            for task_id in sorted(set(grandfathered) - set(self.tasks), key=numeric)
        ]
        for task in sorted(self.tasks.values(), key=lambda t: numeric(t.id)):
            if task.parent and task.parent not in self.tasks:
                found.append(f"{task.id} parent {task.parent} is not in the ledger")
            if self._in_cycle(task.id):
                found.append(f"{task.id} is in a parent cycle")
            milestone = self.resolve(task.id, "milestone")
            if milestone is None:
                found.append(f"{task.id} has no milestone")
            elif milestone not in milestones:
                found.append(f"{task.id} milestone {milestone} is not in the roadmap")
            elif milestone == LEGACY_MILESTONE and task.is_open:
                found.append(
                    f"{task.id} is open in {LEGACY_MILESTONE}, "
                    "the closed-history milestone; place it"
                )
            if task.is_open and self.resolve(task.id, "priority") is None:
                found.append(f"{task.id} is open and has no priority")
            if not task.is_open:
                for child in self.open_children(task.id):
                    found.append(f"{task.id} is done but sub-task {child} is open")
            found += self._blocked_problems(task, grandfathered)
        return found


def _apply_blocked_by(task: Task, value: str, syntax: list[str]) -> None:
    if not BLOCKER_LIST.fullmatch(value):
        syntax.append(f"{task.id} blocked-by {value!r} is not T<n>[,T<n>...]")
        return
    ids = value.split(",")
    repeated = sorted({i for i in ids if ids.count(i) > 1}, key=numeric)
    if repeated:
        syntax.append(f"{task.id} blocked-by lists {', '.join(repeated)} more than once")
    task.blocked_by = list(dict.fromkeys(ids))


def _apply_fields(task: Task, body: str, syntax: list[str]) -> None:
    for token in body.split():
        key, sep, value = token.partition("=")
        if not sep or key not in META_KEYS:
            syntax.append(f"{task.id} meta has unknown token {token!r}")
        elif key == "milestone" and not MILESTONE_ID.fullmatch(value):
            syntax.append(f"{task.id} milestone {value!r} is not M<n>")
        elif key == "priority" and value not in PRIORITY_RANK:
            syntax.append(f"{task.id} priority {value!r} is not one of {', '.join(PRIORITIES)}")
        elif key == "parent" and not TASK_ID.fullmatch(value):
            syntax.append(f"{task.id} parent {value!r} is not T<n>")
        elif key == "blocked-by":
            _apply_blocked_by(task, value, syntax)
        elif key == "blocked-external" and not EXTERNAL_SLUG.fullmatch(value):
            syntax.append(f"{task.id} blocked-external {value!r} is not a slug without spaces")
        else:
            setattr(task, attr_of(key), value)


def parse(text: str) -> Ledger:
    lines = text.split("\n")
    tasks: dict[str, Task] = {}
    syntax: list[str] = []
    current: Task | None = None
    last_content = 0
    for index, line in enumerate(lines):
        heading = HEADING.match(line)
        if heading or BLOCK_END.match(line):
            if current is not None:
                current.end = last_content + 1
            current = None
            if heading and heading.group(2) not in tasks:
                title = line[heading.end() :].split("**")[0].lstrip(". ").strip()
                current = Task(heading.group(2), heading.group(1), title, index, index + 1)
                tasks[current.id] = current
                last_content = index
            continue
        if current is None:
            continue
        if line.strip():
            last_content = index
        reason = REASON_LINE.match(line)
        if reason is not None:
            if current.reason_index is not None:
                syntax.append(f"{current.id} has more than one blocked-reason line")
            else:
                current.reason_index = index
                current.blocked_reason = reason.group(1)
            continue
        meta = META_LINE.match(line)
        if meta is None:
            continue
        if current.meta_index is not None:
            syntax.append(f"{current.id} has more than one meta line")
        else:
            current.meta_index = index
            _apply_fields(current, meta.group(1), syntax)
    if current is not None:
        current.end = last_content + 1
    for task in tasks.values():
        if task.parent in tasks:
            tasks[task.parent].children.append(task.id)
    return Ledger(lines, tasks, syntax)


def _unclaimed_leaves(ledger: Ledger, claimed: set[str], milestone: str | None) -> list[Task]:
    """Open, non-`[!]` leaf tasks nobody holds, optionally of one effective milestone."""
    found: list[Task] = []
    for task in ledger.tasks.values():
        if not task.is_open or task.state == BLOCKED_STATE or task.id in claimed:
            continue
        if ledger.open_children(task.id):
            continue
        if milestone and ledger.resolve(task.id, "milestone") != milestone:
            continue
        found.append(task)
    return found


def claimable(
    ledger: Ledger,
    claimed: set[str],
    milestone: str | None = None,
    milestone_priority: Mapping[str, str] | None = None,
    skip_waiting: bool = False,
) -> tuple[list[str], list[str]]:
    """Open leaf tasks nobody holds: (ready, for-later), ready by milestone then task priority.

    A milestone missing from `milestone_priority`, or with a blank or unknown value, is mid.
    `skip_waiting` drops tasks whose `blocked-by` list has a blocker not `[x]` (see `waiting`).
    """
    ready: list[tuple[int, int, int, str]] = []
    later: list[tuple[int, str]] = []
    for task in _unclaimed_leaves(ledger, claimed, milestone):
        if skip_waiting and ledger.unlanded_blockers(task.id):
            continue
        resolved = ledger.resolve(task.id, "milestone")
        priority = ledger.resolve(task.id, "priority") or "mid"
        milestone_rank_name = (milestone_priority or {}).get(resolved or "", "mid")
        if milestone_rank_name not in PRIORITY_RANK:
            milestone_rank_name = "mid"
        if priority == "for-later" or milestone_rank_name == "for-later":
            later.append((numeric(task.id), task.id))
        else:
            ready.append(
                (
                    PRIORITY_RANK[milestone_rank_name],
                    PRIORITY_RANK[priority],
                    numeric(task.id),
                    task.id,
                )
            )
    return [t for *_, t in sorted(ready)], [t for _, t in sorted(later)]


def waiting(
    ledger: Ledger, claimed: set[str], milestone: str | None = None
) -> list[tuple[str, dict[str, str | None]]]:
    """Tasks `claimable` would offer but for an unlanded blocker, by id, with those blockers.

    Each entry is (task id, blocker id to checkbox char, None when not in the ledger),
    listing only the blockers that are not `[x]`.
    """
    rows = [
        (task.id, ledger.unlanded_blockers(task.id))
        for task in _unclaimed_leaves(ledger, claimed, milestone)
    ]
    return sorted(((i, b) for i, b in rows if b), key=lambda row: numeric(row[0]))


def format_meta(values: dict[str, str | None]) -> str:
    parts = [f"{key}={values[key]}" for key in META_KEYS if values.get(key)]
    return "  meta: " + " ".join(parts)


def format_reason(reason: str | None) -> list[str]:
    collapsed = " ".join((reason or "").split())
    return [f"  {REASON_KEY}: {collapsed}"] if collapsed else []


def apply_meta(text: str, updates: dict[str, dict[str, str | None]]) -> str:
    """Overlay `updates` (task id to key/value, None removes the key) on meta lines.

    `blocked-reason` is the free-text line after the meta line; it is rewritten only when
    the update names it.
    """
    ledger = parse(text)
    edits: list[tuple[int, list[int], list[str]]] = []
    for task_id, change in updates.items():
        task = ledger.tasks.get(task_id)
        if task is None:
            raise ValueError(f"{task_id} is not in the ledger")
        unknown = set(change) - set(META_KEYS) - {REASON_KEY}
        if unknown:
            raise ValueError(f"unknown meta keys: {', '.join(sorted(unknown))}")
        values: dict[str, str | None] = {key: task.meta_value(key) for key in META_KEYS}
        values.update({key: value for key, value in change.items() if key in META_KEYS})
        removed = [] if task.meta_index is None else [task.meta_index]
        written = [format_meta(values)] if any(values.get(key) for key in META_KEYS) else []
        if REASON_KEY in change:
            if task.reason_index is not None:
                removed.append(task.reason_index)
            written += format_reason(change[REASON_KEY])
        position = min(removed, default=task.end)
        edits.append((position, removed, written))
    lines = ledger.lines
    for position, removed, written in sorted(edits, key=lambda edit: edit[0], reverse=True):
        for index in sorted(removed, reverse=True):
            del lines[index]
        lines[position:position] = written
    return "\n".join(lines)


def with_meta(text: str, task_id: str, **updates: str | None) -> str:
    """`apply_meta` for one task, keyword names use `_` for `-` (`blocked_by="T1,T2"`)."""
    return apply_meta(text, {task_id: {key.replace("_", "-"): v for key, v in updates.items()}})


def with_state(text: str, task_id: str, state: str) -> str:
    """Set the checkbox char of `task_id`'s heading line."""
    ledger = parse(text)
    task = ledger.tasks.get(task_id)
    if task is None:
        raise ValueError(f"{task_id} is not in the ledger")
    if len(state) != 1:
        raise ValueError("state must be one checkbox char")
    ledger.lines[task.start] = f"- [{state}]" + ledger.lines[task.start][5:]
    return "\n".join(ledger.lines)


def append_line(text: str, task_id: str, line: str) -> str:
    """Add `line` as the last content line of `task_id`'s entry, before its trailing blank lines."""
    ledger = parse(text)
    task = ledger.tasks.get(task_id)
    if task is None:
        raise ValueError(f"{task_id} is not in the ledger")
    ledger.lines.insert(task.end, line)
    return "\n".join(ledger.lines)
