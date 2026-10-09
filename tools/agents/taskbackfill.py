# SPDX-License-Identifier: GPL-3.0-or-later
"""One-time migration: write `meta:` lines onto every ledger task. Idempotent.

python3 -m tools.agents.taskbackfill --dry-run
python3 -m tools.agents.taskbackfill --strip-roadmap-tasks
"""

import argparse
import collections
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from tools.agents import roadmap as roadmap_doc
from tools.agents import taskmeta
from tools.agents.tasks import write_atomic

LETTERED = re.compile(r"^- \[.\] \*\*(T\d+[a-z]+)\b", re.MULTILINE)
SECTION_HEADING = re.compile(r"^## (T\d+)(?:\s|$)", re.MULTILINE)
HEADING_IDS = re.compile(r"^- \[.\] \*\*(T\d+)\b", re.MULTILINE)
DEFAULT_PRIORITY = "mid"


@dataclass
class Report:
    milestone_set: int = 0
    legacy_set: int = 0
    priority_set: int = 0
    unplaced_open: list[str] = field(default_factory=list)
    duplicate_listed: list[str] = field(default_factory=list)
    lettered_ids: list[str] = field(default_factory=list)
    section_headings: int = 0
    duplicate_headings: list[str] = field(default_factory=list)


def parse_legacy(roadmap_text: str) -> roadmap_doc.Roadmap:
    """`parse_roadmap` with every `Tasks:` line of a milestone kept (it keeps the last only)."""
    legacy = roadmap_doc.parse_roadmap(roadmap_text)
    by_key = {milestone.key: milestone for milestone in legacy.milestones}
    for milestone in legacy.milestones:
        milestone.legacy_tasks = []
    current: roadmap_doc.Milestone | None = None
    for line in roadmap_text.splitlines():
        match = roadmap_doc.MILESTONE.match(line)
        if match:
            current = by_key[match.group(1)]
        elif line.startswith("## "):
            current = None
        elif current is not None and line.partition(":")[0] == "Tasks":
            current.legacy_tasks += re.findall(r"T\d+", line)
    return legacy


def listed_milestones(legacy: roadmap_doc.Roadmap) -> dict[str, list[str]]:
    """Task id to the milestones whose `Tasks:` line lists it, in roadmap order."""
    listed: dict[str, list[str]] = {}
    for milestone in legacy.milestones:
        for task_id in dict.fromkeys(milestone.legacy_tasks):
            listed.setdefault(task_id, []).append(milestone.key)
    return listed


def plan(ledger_text: str, legacy: roadmap_doc.Roadmap) -> tuple[str, Report]:
    ledger = taskmeta.parse(ledger_text)
    listed = listed_milestones(legacy)
    report = Report()
    updates: dict[str, dict[str, str | None]] = {}
    ordered = sorted(ledger.tasks.values(), key=lambda task: taskmeta.numeric(task.id))
    needs_priority = {
        task.id for task in ordered if task.is_open and ledger.resolve(task.id, "priority") is None
    }
    for task in ordered:
        change: dict[str, str | None] = {}
        if not task.milestone and not task.parent:
            if task.id in listed:
                change["milestone"] = listed[task.id][0]
                report.milestone_set += 1
            elif not task.is_open:
                change["milestone"] = taskmeta.LEGACY_MILESTONE
                report.legacy_set += 1
            else:
                report.unplaced_open.append(task.id)
        # An open ancestor that needs a priority gets it and the task inherits it.
        if task.id in needs_priority and task.parent not in needs_priority:
            change["priority"] = DEFAULT_PRIORITY
            report.priority_set += 1
        if change:
            updates[task.id] = change
    report.duplicate_listed = sorted(
        (task_id for task_id, keys in listed.items() if len(keys) > 1), key=taskmeta.numeric
    )
    report.lettered_ids = LETTERED.findall(ledger_text)
    report.section_headings = len(SECTION_HEADING.findall(ledger_text))
    heading_counts = collections.Counter(HEADING_IDS.findall(ledger_text))
    report.duplicate_headings = sorted(
        (task_id for task_id, count in heading_counts.items() if count > 1), key=taskmeta.numeric
    )
    text = taskmeta.apply_meta(ledger_text, updates) if updates else ledger_text
    return text, report


def strip_tasks_lines(roadmap_text: str) -> str:
    """Drop the `Tasks:` lines inside `## M<n>.` sections, leaving all else untouched."""
    kept: list[str] = []
    in_milestone = False
    for line in roadmap_text.split("\n"):
        if roadmap_doc.MILESTONE.match(line):
            in_milestone = True
        elif line.startswith("## "):
            in_milestone = False
        if in_milestone and line.startswith("Tasks:"):
            continue
        kept.append(line)
    return "\n".join(kept)


def print_report(report: Report) -> None:
    print(f"milestone_set: {report.milestone_set}")
    print(f"legacy_set: {report.legacy_set}")
    print(f"priority_set: {report.priority_set}")
    for name in ("unplaced_open", "duplicate_listed", "lettered_ids", "duplicate_headings"):
        ids = getattr(report, name)
        print(f"{name}: {len(ids)} {' '.join(ids)}".rstrip())
    print(f"section_headings: {report.section_headings}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--tasks", type=Path, default=Path("docs/tasks.md"))
    parser.add_argument("--roadmap", type=Path, default=Path("docs/roadmap.md"))
    parser.add_argument(
        "--legacy-roadmap",
        type=Path,
        help="file whose `Tasks:` lines are read (default --roadmap)",
    )
    parser.add_argument("--dry-run", action="store_true", help="print the report, write nothing")
    parser.add_argument(
        "--strip-roadmap-tasks", action="store_true", help="remove `Tasks:` lines from --roadmap"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    legacy_path = args.legacy_roadmap or args.roadmap
    legacy = parse_legacy(legacy_path.read_text())
    before = args.tasks.read_text()
    after, report = plan(before, legacy)
    print_report(report)
    if args.dry_run:
        return 0
    if after != before:
        write_atomic(args.tasks, after)
    if args.strip_roadmap_tasks:
        roadmap_text = args.roadmap.read_text()
        stripped = strip_tasks_lines(roadmap_text)
        if stripped != roadmap_text:
            write_atomic(args.roadmap, stripped)
    return 0


if __name__ == "__main__":
    sys.exit(main())
