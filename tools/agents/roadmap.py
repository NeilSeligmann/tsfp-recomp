# SPDX-License-Identifier: GPL-3.0-or-later
"""Parse `docs/roadmap.md` and join it with task state from `docs/tasks.md`.

Milestone progress is never stored: it is the share of a milestone's tasks whose
checkbox in the ledger is `[x]`, so marking a task DONE moves the roadmap. A task's
milestone is its `meta: milestone=M<n>` line in the ledger (inherited through `parent`),
not a `Tasks:` line on the roadmap. Each milestone carries a `Priority:` line (one of
`taskmeta.PRIORITIES`) that orders the work queue ahead of task priority.

    python3 -m tools.agents.roadmap --check     # exit 1 on any roadmap or task-meta problem

`--check` also enforces blocked-by: every `[!]` entry names its blockers (or an external
slug plus a reason line) unless it is listed in `docs/blocked-grandfathered.txt`.
"""

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from tools.agents import taskmeta
from tools.agents.taskmeta import TASK_LINE  # noqa: F401  (re-exported: findings.py imports it)

DEFAULT_ROADMAP = Path("docs/roadmap.md")
DEFAULT_TASKS = Path("docs/tasks.md")
GRANDFATHER_NAME = "blocked-grandfathered.txt"
MILESTONE = re.compile(r"^## (M\d+)\. (.+)$")
FIELDS = ("Goal", "Now", "Exit", "Priority", "Tasks")


@dataclass
class Milestone:
    key: str
    title: str
    goal: str = ""
    now: str = ""
    exit: str = ""
    priority: str = ""
    tasks: list[str] = field(default_factory=list)
    legacy_tasks: list[str] = field(default_factory=list)


@dataclass
class Roadmap:
    where: list[str] = field(default_factory=list)
    next_steps: list[str] = field(default_factory=list)
    milestones: list[Milestone] = field(default_factory=list)
    ledger: taskmeta.Ledger | None = None
    grandfathered: list[str] = field(default_factory=list)
    grandfather_errors: list[str] = field(default_factory=list)


def parse_roadmap(text: str) -> Roadmap:
    roadmap = Roadmap()
    section = ""
    current: Milestone | None = None
    for line in text.splitlines():
        match = MILESTONE.match(line)
        if match:
            current = Milestone(match.group(1), match.group(2).strip())
            roadmap.milestones.append(current)
            section = "milestone"
            continue
        if line.startswith("## "):
            current = None
            section = line[3:].strip()
            continue
        if section == "Where we are" and line.strip():
            roadmap.where.append(line.strip())
        elif section == "Next steps" and line.startswith("- "):
            roadmap.next_steps.append(line[2:].strip())
        elif section == "milestone" and current is not None:
            name, _, value = line.partition(":")
            if name in FIELDS:
                if name == "Tasks":
                    current.tasks = re.findall(r"T\d+", value)
                    current.legacy_tasks = list(current.tasks)
                else:
                    setattr(current, name.lower(), value.strip())
    return roadmap


def read_grandfathered(path: Path) -> tuple[list[str], list[str]]:
    """Ids and format errors of the grandfather file. A missing file grandfathers nothing."""
    if not path.exists():
        return [], []
    return taskmeta.parse_grandfathered(path.read_text())


def load(
    roadmap_path: Path, tasks_path: Path, grandfathered_path: Path | None = None
) -> tuple[Roadmap, dict[str, str]]:
    """Parse the roadmap and ledger, deriving each milestone's tasks from task metadata.

    The grandfather file defaults to `blocked-grandfathered.txt` beside the ledger.
    """
    roadmap = parse_roadmap(roadmap_path.read_text())
    ledger = taskmeta.parse(tasks_path.read_text())
    roadmap.ledger = ledger
    roadmap.grandfathered, roadmap.grandfather_errors = read_grandfathered(
        grandfathered_path or tasks_path.with_name(GRANDFATHER_NAME)
    )
    for milestone in roadmap.milestones:
        milestone.tasks = sorted(
            (t for t in ledger.tasks if ledger.resolve(t, "milestone") == milestone.key),
            key=taskmeta.numeric,
        )
    return roadmap, ledger.states()


def milestone_priorities(roadmap: Roadmap) -> dict[str, str]:
    return {milestone.key: milestone.priority for milestone in roadmap.milestones}


def set_milestone_priority(text: str, key: str, priority: str) -> str:
    """Return `text` with milestone `key`'s `Priority:` line set, inserting it if absent."""
    if priority not in taskmeta.PRIORITIES:
        raise ValueError(f"priority must be one of {', '.join(taskmeta.PRIORITIES)}")
    lines = text.split("\n")
    heading = f"## {key}. "
    start = next((i for i, line in enumerate(lines) if line.startswith(heading)), None)
    if start is None:
        raise ValueError(f"{key} is not a milestone in the roadmap")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    block = range(start + 1, end)
    existing = next((i for i in block if lines[i].startswith("Priority:")), None)
    if existing is not None:
        lines[existing] = f"Priority: {priority}"
        return "\n".join(lines)
    anchors = [i for i in block if lines[i].split(":", 1)[0] in ("Goal", "Now", "Exit")]
    exit_lines = [i for i in anchors if lines[i].startswith("Exit:")]
    after = exit_lines[0] if exit_lines else (anchors[-1] if anchors else start)
    lines.insert(after + 1, f"Priority: {priority}")
    return "\n".join(lines)


def task_states(tasks_text: str) -> dict[str, str]:
    """Task id to checkbox char (`x`, `~`, ` `, `!`). The first entry for an id wins."""
    states: dict[str, str] = {}
    for char, task in TASK_LINE.findall(tasks_text):
        states.setdefault(task, char)
    return states


def problems(roadmap: Roadmap) -> list[str]:
    found: list[str] = []
    if not roadmap.milestones:
        found.append("no milestones parsed")
    for milestone in roadmap.milestones:
        if milestone.legacy_tasks:
            found.append(
                f"{milestone.key} still has a Tasks line; membership is "
                "`meta: milestone=` on each task"
            )
        if milestone.priority not in taskmeta.PRIORITIES:
            found.append(
                f"{milestone.key} has no valid Priority line "
                f"(one of {', '.join(taskmeta.PRIORITIES)})"
            )
        if not milestone.tasks:
            found.append(f"{milestone.key} has no tasks")
    found += roadmap.grandfather_errors
    if roadmap.ledger is not None:
        found += roadmap.ledger.problems(
            {m.key for m in roadmap.milestones}, set(roadmap.grandfathered)
        )
    return found


def notes(roadmap: Roadmap) -> list[str]:
    """Non-failing `note:` lines: landed blockers and stale grandfather entries."""
    return roadmap.ledger.notes(roadmap.grandfathered) if roadmap.ledger is not None else []


def unplaced_open(roadmap: Roadmap, states: dict[str, str]) -> list[str]:
    placed = {task for milestone in roadmap.milestones for task in milestone.tasks}
    return sorted(
        (task for task, char in states.items() if char in "~!" and task not in placed),
        key=lambda task: int(task[1:]),
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Validate docs/roadmap.md against the ledger.")
    parser.add_argument("--roadmap", type=Path, default=DEFAULT_ROADMAP)
    parser.add_argument("--tasks", type=Path, default=DEFAULT_TASKS)
    parser.add_argument(
        "--grandfathered",
        type=Path,
        help=f"[!] ids exempt from blocked-by (default: {GRANDFATHER_NAME} beside --tasks)",
    )
    parser.add_argument("--check", action="store_true", help="exit 1 on any problem")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    roadmap, states = load(args.roadmap, args.tasks, args.grandfathered)
    found = problems(roadmap)
    for line in found:
        print(line)
    for line in notes(roadmap):
        print(line)
    for task in unplaced_open(roadmap, states):
        print(f"note: in-flight task {task} is in no milestone")
    return 1 if found and args.check else 0


if __name__ == "__main__":
    sys.exit(main())
