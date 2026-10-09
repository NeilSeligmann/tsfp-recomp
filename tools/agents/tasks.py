# SPDX-License-Identifier: GPL-3.0-or-later
"""Task-ledger CLI: add, set, tree, next and milestone over docs/tasks.md metadata.

python3 -m tools.agents.tasks tree --open
python3 -m tools.agents.tasks next
python3 -m tools.agents.tasks set T123 --priority high
python3 -m tools.agents.tasks set T123 --blocked-by T120,T121 --mark-blocked
python3 -m tools.agents.tasks set T123 --external owner-key --reason "needs the owner's key"
python3 -m tools.agents.tasks set T123 --clear-blocked
python3 -m tools.agents.tasks reopen T123 [--force]
python3 -m tools.agents.tasks milestone M5 --priority low
python3 -m tools.agents.tasks add --title "..." --parent T123 --owner claude-main
python3 -m tools.agents.tasks add --title "..." --root --milestone M5 --priority mid \
    --owner claude-main
"""

import argparse
import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from tools.agents import coord, taskmeta
from tools.agents import roadmap as roadmap_doc

DEFAULT_TASKS = Path("docs/tasks.md")
DEFAULT_ROADMAP = Path("docs/roadmap.md")
SECTION = "## Next, not yet started"
FORCE_NOTE = "Reopened by --force with open blockers"


def milestone_keys(roadmap_path: Path) -> set[str]:
    parsed = roadmap_doc.parse_roadmap(roadmap_path.read_text())
    return {milestone.key for milestone in parsed.milestones}


def write_atomic(path: Path, text: str) -> None:
    mode = path.stat().st_mode & 0o777
    handle, name = tempfile.mkstemp(dir=path.parent, prefix=path.name, suffix=".tmp")
    try:
        with os.fdopen(handle, "w") as stream:
            stream.write(text)
        os.chmod(name, mode)
        os.replace(name, path)
    except BaseException:
        Path(name).unlink(missing_ok=True)
        raise


def grandfathered_ids(tasks_path: Path) -> set[str]:
    """Ids exempt from the blocked-by rule, from the file beside the ledger."""
    ids, _ = roadmap_doc.read_grandfathered(tasks_path.with_name(roadmap_doc.GRANDFATHER_NAME))
    return set(ids)


def new_problems(
    before: str, after: str, milestones: set[str], grandfathered: set[str] | None = None
) -> list[str]:
    exempt = grandfathered or set()
    old = set(taskmeta.parse(before).problems(milestones, exempt))
    return [p for p in taskmeta.parse(after).problems(milestones, exempt) if p not in old]


def blocker_list(value: str) -> str:
    """Normalise `T1, T2` or `T1,T2` to `T1,T2`, rejecting anything that is not task ids."""
    ids = [part for part in re.split(r"[,\s]+", value.strip()) if part]
    if not ids or not all(taskmeta.TASK_ID.fullmatch(i) for i in ids):
        raise ValueError(f"--blocked-by {value!r} is not a list of task ids T<n>,T<n>")
    return ",".join(ids)


def external_slug(value: str) -> str:
    if not taskmeta.EXTERNAL_SLUG.fullmatch(value):
        raise ValueError(
            f"--external {value!r} is not a slug without spaces (letters, digits, ._-)"
        )
    return value


def insert_entry(text: str, entry_lines: list[str]) -> str:
    lines = text.split("\n")
    if SECTION not in lines:
        raise ValueError(f"{SECTION!r} heading not found in the ledger")
    start = lines.index(SECTION)
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    while end > start + 1 and not lines[end - 1].strip():
        end -= 1
    lines[end:end] = ["", *entry_lines]
    return "\n".join(lines)


def reserve_id(owner: str, slot: int, note: str, known: list[int] | None = None) -> str:
    args = argparse.Namespace(
        owner=owner, slot=slot, note=note, stale=coord.DEFAULT_STALE_SECONDS, known=known or []
    )
    captured = io.StringIO()
    with coord.claim_lock(), contextlib.redirect_stdout(captured):
        code = coord.cmd_next_id(args)
    if code != 0:
        raise RuntimeError("coord next-id failed")
    return captured.getvalue().strip()


def render_tree(
    ledger: taskmeta.Ledger, milestone: str | None = None, open_only: bool = False
) -> list[str]:
    def matches(task: taskmeta.Task) -> bool:
        if open_only and not task.is_open:
            return False
        return milestone is None or ledger.resolve(task.id, "milestone") == milestone

    def walk(task_id: str, depth: int) -> list[str]:
        task = ledger.tasks[task_id]
        rows = [
            row
            for child in sorted(task.children, key=taskmeta.numeric)
            for row in walk(child, depth + 1)
        ]
        if not rows and not matches(task):
            return []
        priority = ledger.resolve(task_id, "priority") or "-"
        owner = ledger.resolve(task_id, "milestone") or "-"
        line = f"{'  ' * depth}[{task.state}] {task.id} {priority:<9} {owner:<4} {task.title[:70]}"
        return [line, *rows]

    roots = sorted(
        (t.id for t in ledger.tasks.values() if t.parent not in ledger.tasks),
        key=taskmeta.numeric,
    )
    return [row for root in roots for row in walk(root, 0)]


def claimed_ids() -> set[str]:
    """Task ids with a live claim file (NEXT-T<n> reservations are not claims)."""
    live: set[str] = set()
    for path in (coord.coord_dir() / "claims").glob("T*.json"):
        if not re.fullmatch(r"T\d+", path.stem):
            continue
        with contextlib.suppress(FileNotFoundError):
            if coord.read_claim(path)["age"] <= coord.DEFAULT_STALE_SECONDS:
                live.add(path.stem)
    return live


def cmd_tree(args: argparse.Namespace) -> int:
    ledger = taskmeta.parse(args.tasks.read_text())
    for line in render_tree(ledger, args.milestone, args.open):
        print(line)
    return 0


def cmd_next(args: argparse.Namespace) -> int:
    ledger = taskmeta.parse(args.tasks.read_text())
    try:
        claimed = claimed_ids()
    except subprocess.CalledProcessError:
        print("not inside a git repository, cannot read claims", file=sys.stderr)
        return 1
    parsed = roadmap_doc.parse_roadmap(args.roadmap.read_text())
    milestone_priority = roadmap_doc.milestone_priorities(parsed)
    ready, later = taskmeta.claimable(
        ledger, claimed, args.milestone, milestone_priority, skip_waiting=True
    )

    def row(task_id: str) -> str:
        priority = ledger.resolve(task_id, "priority") or "mid"
        milestone = ledger.resolve(task_id, "milestone") or "-"
        shown = milestone_priority.get(milestone) or "-"
        title = ledger.tasks[task_id].title[:60]
        return f"  {task_id:<7}{shown:<10}{priority:<10}{milestone:<5}{title}"

    print("ready (by milestone, then task priority):")
    for task_id in ready:
        print(row(task_id))
    if later:
        print("for-later:")
        for task_id in later:
            print(row(task_id))
    blocked = taskmeta.waiting(ledger, claimed, args.milestone)
    if blocked:
        print("waiting on (blocked-by not landed, not offered):")
        for task_id, blockers in blocked:
            print(f"{row(task_id)}  <- {describe_blockers(blockers)}")
    return 0


def describe_blockers(blockers: dict[str, str | None]) -> str:
    """`T4 [ ], T9 [!], T99 (not in ledger)` for the blockers that have not landed."""
    return ", ".join(
        f"{blocker} (not in ledger)" if state is None else f"{blocker} [{state}]"
        for blocker, state in blockers.items()
    )


def refuse(problems: list[str]) -> int:
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1


def cmd_set(args: argparse.Namespace) -> int:
    text = args.tasks.read_text()
    if args.task not in taskmeta.parse(text).tasks:
        return refuse([f"{args.task} is not in the ledger"])
    updates: dict[str, str | None] = {}
    if args.priority:
        updates["priority"] = args.priority
    if args.milestone:
        updates["milestone"] = args.milestone
    if args.parent:
        updates["parent"] = None if args.parent == "none" else args.parent
    blocked = args.blocked_by or args.external or args.reason
    if args.clear_blocked and blocked:
        return refuse(["--clear-blocked cannot be combined with --blocked-by/--external/--reason"])
    try:
        if args.blocked_by:
            updates["blocked_by"] = blocker_list(args.blocked_by)
        if args.external:
            updates["blocked_external"] = external_slug(args.external)
    except ValueError as error:
        return refuse([str(error)])
    if args.reason:
        updates["blocked_reason"] = args.reason
    if args.clear_blocked:
        updates.update(blocked_by=None, blocked_external=None, blocked_reason=None)
    if not updates and not args.mark_blocked:
        return refuse(
            [
                "nothing to set: give --priority, --milestone, --parent, --blocked-by, "
                "--external, --reason, --clear-blocked or --mark-blocked"
            ]
        )
    after = taskmeta.with_meta(text, args.task, **updates) if updates else text
    if args.mark_blocked:
        after = taskmeta.with_state(after, args.task, taskmeta.BLOCKED_STATE)
    found = new_problems(text, after, milestone_keys(args.roadmap), grandfathered_ids(args.tasks))
    if found:
        if args.clear_blocked and taskmeta.parse(text).tasks[args.task].state == "!":
            found.append(
                f"{args.task} is [!]: `tasks reopen {args.task}` clears the blocked "
                "metadata and reopens it in one step"
            )
        return refuse(found)
    write_atomic(args.tasks, after)
    print(f"updated {args.task}")
    return 0


def cmd_reopen(args: argparse.Namespace) -> int:
    text = args.tasks.read_text()
    ledger = taskmeta.parse(text)
    task = ledger.tasks.get(args.task)
    if task is None:
        return refuse([f"{args.task} is not in the ledger"])
    if task.state != taskmeta.BLOCKED_STATE:
        return refuse([f"{args.task} is [{task.state}], only a [!] task can be reopened"])
    open_blockers = ledger.unlanded_blockers(args.task)
    if open_blockers and not args.force:
        return refuse(
            [
                f"{args.task} is still blocked by {describe_blockers(open_blockers)}: "
                "wait for them to land, or pass --force to reopen anyway (recorded in the entry)"
            ]
        )
    after = taskmeta.with_meta(
        text, args.task, blocked_by=None, blocked_external=None, blocked_reason=None
    )
    after = taskmeta.with_state(after, args.task, " ")
    if open_blockers:
        after = taskmeta.append_line(after, args.task, f"  {FORCE_NOTE} {','.join(open_blockers)}")
    found = new_problems(text, after, milestone_keys(args.roadmap), grandfathered_ids(args.tasks))
    if found:
        return refuse(found)
    write_atomic(args.tasks, after)
    print(f"reopened {args.task}")
    return 0


def cmd_milestone(args: argparse.Namespace) -> int:
    try:
        after = roadmap_doc.set_milestone_priority(
            args.roadmap.read_text(), args.key, args.priority
        )
    except ValueError as error:
        return refuse([str(error)])
    write_atomic(args.roadmap, after)
    print(f"updated {args.key}")
    return 0


def cmd_add(args: argparse.Namespace) -> int:
    text = args.tasks.read_text()
    ledger = taskmeta.parse(text)
    milestones = milestone_keys(args.roadmap)
    title = " ".join(args.title.split()).rstrip(".")
    body = " ".join((args.body or "").split())
    if not title or "**" in title:
        return refuse(["--title must be non-empty and must not contain '**'"])
    if not args.parent and not args.root:
        return refuse(
            [
                "every new task belongs under a parent umbrella: pass --parent T<n> "
                "(find one with `python3 -m tools.agents.tasks tree`, or add an umbrella "
                "first). Only a new top-level goal may pass --root instead"
            ]
        )
    if args.parent and args.root:
        return refuse(["--root and --parent are mutually exclusive"])
    if args.parent and args.parent not in ledger.tasks:
        return refuse([f"parent {args.parent} is not in the ledger"])
    for attr in ("milestone", "priority"):
        inherited = ledger.resolve(args.parent, attr) if args.parent else None
        if not getattr(args, attr) and not inherited:
            return refuse([f"--{attr} is required unless --parent supplies one"])
    try:
        blocked_by = blocker_list(args.blocked_by) if args.blocked_by else None
        external = external_slug(args.external) if args.external else None
    except ValueError as error:
        return refuse([str(error)])
    values: dict[str, str | None] = {
        "milestone": args.milestone,
        "priority": args.priority,
        "parent": args.parent,
        "blocked-by": blocked_by,
        "blocked-external": external,
    }

    def build(task_id: str) -> str:
        heading = f"- [ ] **{task_id}. {title}.**" + (f" {body}" if body else "")
        entry = [heading, taskmeta.format_meta(values), *taskmeta.format_reason(args.reason)]
        return insert_entry(text, entry)

    # Validate with the id the reservation would hand out, before reserving anything.
    try:
        probe = f"T{max((taskmeta.numeric(i) for i in ledger.tasks), default=0) + 1}"
        found = new_problems(text, build(probe), milestones, grandfathered_ids(args.tasks))
    except ValueError as error:
        return refuse([str(error)])
    if found:
        return refuse(found)
    try:
        task_id = reserve_id(
            args.owner, args.slot, title, [taskmeta.numeric(i) for i in ledger.tasks]
        )
    except (subprocess.CalledProcessError, ValueError, RuntimeError) as error:
        return refuse([f"could not reserve a task id: {error}"])
    if task_id in ledger.tasks:
        return refuse(
            [
                f"{task_id} is already in the ledger: the previous add by {args.owner} "
                f"slot {args.slot} was not claimed. Add with --claim or use another --slot"
            ]
        )
    write_atomic(args.tasks, build(task_id))
    print(task_id)
    if args.claim:
        claim = argparse.Namespace(
            task=task_id,
            owner=args.owner,
            slot=args.slot,
            note=title,
            stale=coord.DEFAULT_STALE_SECONDS,
            allow_umbrella=True,
        )
        with coord.claim_lock(), contextlib.redirect_stdout(io.StringIO()):
            code = coord.cmd_claim(claim)
        if code != 0:
            return refuse([f"{task_id} was added but could not be claimed"])
    return 0


def cmd_backfill(args: argparse.Namespace) -> int:
    from tools.agents import taskbackfill  # imports write_atomic from this module

    return taskbackfill.main(args.passthrough)


def build_parser() -> argparse.ArgumentParser:
    shared = argparse.ArgumentParser(add_help=False)
    shared.add_argument("--tasks", type=Path, default=DEFAULT_TASKS)
    shared.add_argument("--roadmap", type=Path, default=DEFAULT_ROADMAP)
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("tree", parents=[shared], help="print the task hierarchy")
    p.add_argument("--milestone")
    p.add_argument("--open", action="store_true", help="only open tasks and their ancestors")
    p.set_defaults(func=cmd_tree)

    p = sub.add_parser("next", parents=[shared], help="claimable work by priority")
    p.add_argument("--milestone")
    p.set_defaults(func=cmd_next)

    p = sub.add_parser("set", parents=[shared], help="edit a task's meta line")
    p.add_argument("task")
    p.add_argument("--priority", choices=taskmeta.PRIORITIES)
    p.add_argument("--milestone")
    p.add_argument("--parent", help="T<n>, or 'none' to clear")
    p.add_argument("--blocked-by", help="blocking task ids, T1,T2")
    p.add_argument("--external", help="slug (no spaces) naming an external blocker")
    p.add_argument("--reason", help="free-text blocked-reason line")
    p.add_argument("--clear-blocked", action="store_true", help="drop all blocked metadata")
    p.add_argument("--mark-blocked", action="store_true", help="also set the state to [!]")
    p.set_defaults(func=cmd_set)

    p = sub.add_parser("reopen", parents=[shared], help="reopen a [!] task, clearing its blockers")
    p.add_argument("task")
    p.add_argument(
        "--force", action="store_true", help="reopen although a listed blocker is not [x]"
    )
    p.set_defaults(func=cmd_reopen)

    p = sub.add_parser("milestone", parents=[shared], help="set a milestone's Priority line")
    p.add_argument("key", help="milestone, e.g. M5")
    p.add_argument("--priority", choices=taskmeta.PRIORITIES, required=True)
    p.set_defaults(func=cmd_milestone)

    p = sub.add_parser("add", parents=[shared], help="reserve an id and append a task")
    p.add_argument("--title", required=True)
    p.add_argument("--milestone")
    p.add_argument("--priority", choices=taskmeta.PRIORITIES)
    p.add_argument("--parent", help="parent umbrella T<n> (required unless --root)")
    p.add_argument("--root", action="store_true", help="allow a parentless top-level goal")
    p.add_argument("--blocked-by", help="blocking task ids, T1,T2")
    p.add_argument("--external", help="slug (no spaces) naming an external blocker")
    p.add_argument("--reason", help="free-text blocked-reason line")
    p.add_argument("--body")
    p.add_argument("--owner", required=True)
    p.add_argument("--slot", type=int, default=0)
    p.add_argument("--claim", action="store_true", help="claim the new task for --owner")
    p.set_defaults(func=cmd_add)

    p = sub.add_parser("backfill", add_help=False, help="one-time meta backfill (see taskbackfill)")
    p.set_defaults(func=cmd_backfill)
    return parser


def main() -> int:
    parser = build_parser()
    args, args.passthrough = parser.parse_known_args()
    if args.passthrough and args.command != "backfill":
        parser.error(f"unrecognized arguments: {' '.join(args.passthrough)}")
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
