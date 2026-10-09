# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared claim ledger and message log for concurrent Claude and Codex sessions.

State lives in `<git-common-dir>/agent-coord/`, which every worktree of the repo
shares and which is never committed, so claims cannot cause merge conflicts.

    claims/<TASK>.json   complete records published by an exclusive hard link
    messages.jsonl       append-only log agents use to talk to each other
    events.jsonl         append-only audit log of claim, release, progress and so on
    sessions/<OWNER>.json  record written by `register`

A claim is alive while its file mtime is younger than --stale seconds. Owners
refresh it with `beat`. `reap` deletes claims whose owner went silent, which
liberates the task for anyone else.

On Linux, supported claim CLI commands serialize through a permanent common-dir
flock inode. Publication is complete before the claim becomes visible. Already
loaded older tools and direct internal function calls do not participate; this
is coordination between cooperating CLI processes, not protection from external
filesystem mutation. Message-log operations use their existing separate behavior.

Audit trail (T1044). Every claim state change appends one JSON object to events.jsonl
while the claim lock is held: {ts, task, milestone, owner, slot, kind, detail}. A plain
`beat` is deliberately not logged. Readers skip a torn or corrupt line and writers start
a fresh line after one, so a killed writer never poisons later events. `progress` stores
percent, percent_kind, status_text and progress_at in the claim file (optional keys, old
claims keep working) and logs only when percent or status text changed. `audit` reports
held time per owner as claim to release or liberation, with the silence recorded at
liberation counted as stale time and the rest as active time, because beat evidence alone
cannot tell a quiet worker from a dead one before somebody liberates the claim.

Session expiry (T1122). Every command run with --owner refreshes `last_seen` in that
owner's existing session record (never creates one). `sessions` derives a state per
record: live (a live claim), idle (seen within --session-stale, default 6 h) or ended
(`session-end`, or silent past the threshold with no live claim). Record writes go through
`sessions.lock`, a short flock that `say` and `read` (which hold no claim lock) share.
"""

import argparse
import fcntl
import json
import os
import re
import subprocess
import sys
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from pathlib import Path
from uuid import uuid4

from tools.agents import taskmeta

DEFAULT_STALE_SECONDS = 1200
SLOT_COUNT = 3
EVENT_KINDS = (
    "claim",
    "handoff",
    "release",
    "liberate",
    "progress",
    "block",
    "unblock",
    "reserve",
    "register",
    "stall-probe",
    "recovery",
    "merge",
    "publish",
)
PERCENT_KINDS = ("measured", "assumed")
REGISTER_WINDOW_SECONDS = 24 * 3600
SESSION_STALE_SECONDS = 6 * 3600
SESSION_STATES = ("live", "idle", "ended")
# Commands that carry --owner and therefore show that session is alive.
TOUCH_COMMANDS = frozenset(
    {
        "claim",
        "beat",
        "release",
        "next-id",
        "slots",
        "say",
        "read",
        "progress",
        "register",
        "block",
        "unblock",
        "event",
    }
)
SESSION_OWNER_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]*")


def coord_dir() -> Path:
    common = subprocess.run(
        ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    path = Path(common) / "agent-coord"
    (path / "claims").mkdir(parents=True, exist_ok=True)
    return path


def claim_path(task: str) -> Path:
    return coord_dir() / "claims" / f"{task}.json"


def read_claim(path: Path) -> dict:
    data = json.loads(path.read_text())
    data["age"] = int(time.time() - path.stat().st_mtime)
    return data


def events_path() -> Path:
    return coord_dir() / "events.jsonl"


def event_task(task: str) -> str:
    """A reservation `NEXT-T5` is logged under the task it reserves."""
    match = re.fullmatch(r"NEXT-(T[0-9]+)", task)
    return match[1] if match else task


def main_ledger() -> taskmeta.Ledger | None:
    """The committed main ledger, None when unknowable (no repo ledger, no main branch)."""
    shown = subprocess.run(["git", "show", "main:docs/tasks.md"], capture_output=True, text=True)
    return taskmeta.parse(shown.stdout) if shown.returncode == 0 else None


def milestone_of(task: str | None) -> str | None:
    """Best-effort milestone of a task from the main ledger, None when unknown."""
    if task is None or not re.fullmatch(r"T[0-9]+", task):
        return None
    ledger = main_ledger()
    if ledger is None or task not in ledger.tasks:
        return None
    return ledger.resolve(task, "milestone")


def subagent_id(owner: str, slot: int | None, task: str) -> str:
    """Unique id of a subagent: `<owner>/s<slot>:T<n>`. Slot 0 is the session itself."""
    if not slot:
        return owner
    return f"{owner}/s{slot}:{task}"


def append_event(record: dict) -> None:
    """Append one line. A torn last line (no newline) is ended first so ours stays intact."""
    data = (json.dumps(record, separators=(",", ":")) + "\n").encode()
    fd = os.open(events_path(), os.O_RDWR | os.O_APPEND | os.O_CREAT, 0o666)
    try:
        size = os.fstat(fd).st_size
        if size and os.pread(fd, 1, size - 1) != b"\n":
            data = b"\n" + data
        os.write(fd, data)
    finally:
        os.close(fd)


def log_event(
    task: str | None, kind: str, owner: str, slot: int | None = None, detail: object = None
) -> None:
    """Record an audit event. Callers hold claim_lock. A write failure warns, never aborts."""
    if kind not in EVENT_KINDS:
        raise ValueError(f"unknown event kind {kind!r}")
    task = event_task(task) if task else None
    record = {
        "ts": int(time.time()),
        "task": task,
        "milestone": milestone_of(task),
        "owner": owner,
        "slot": slot,
        "kind": kind,
        "detail": detail,
    }
    try:
        append_event(record)
    except OSError as error:
        print(f"warning: audit event {kind} for {task} not logged: {error}", file=sys.stderr)


def read_events() -> list[dict]:
    """All parseable events in file order. Torn, corrupt or non-event lines are skipped."""
    path = events_path()
    if not path.exists():
        return []
    events = []
    for line in path.read_bytes().decode(errors="replace").splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if (
            isinstance(event, dict)
            and isinstance(event.get("ts"), int | float)
            and isinstance(event.get("kind"), str)
        ):
            events.append(event)
    return events


def previous_holder(events: list[dict], task: str) -> tuple[str | None, str]:
    """Owner of the last claim or handoff of `task` and how that holding ended."""
    owner, ended = None, "none"
    for event in events:
        if event.get("task") != task:
            continue
        if event["kind"] in ("claim", "handoff"):
            owner, ended = event.get("owner"), "unlogged"
        elif event["kind"] in ("release", "liberate") and owner is not None:
            ended = event["kind"]
    return owner, ended


def log_claim(task: str, owner: str, slot: int, detail: dict) -> None:
    """Log `claim`, or `handoff` when a different owner than the last holder takes over."""
    previous, ended = previous_holder(read_events(), task)
    if previous is not None and previous != owner:
        log_event(task, "handoff", owner, slot, {**detail, "from": previous, "after": ended})
    else:
        log_event(task, "claim", owner, slot, detail)


@contextmanager
def claim_lock() -> Iterator[None]:
    """Keep the lock inode stable; closing releases it even on process death."""
    # Linux flock does not require a writable descriptor. A group-readable
    # permanent lock therefore preserves access for readers sharing claim files.
    fd = os.open(coord_dir() / "claims.lock", os.O_RDONLY | os.O_CREAT, 0o666)
    with os.fdopen(fd, "r") as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        yield


def publish_claim(path: Path, record: dict, *, replace: bool = False) -> None:
    """Publish complete JSON exclusively, or atomically replace an owned reservation."""
    temporary = path.parent / f".claim-{uuid4().hex}.tmp"
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666)
    try:
        with os.fdopen(fd, "w") as handle:
            json.dump(record, handle)
            handle.flush()
            os.fsync(handle.fileno())
        if replace:
            # Used only for an owned reservation while holding the ledger lock.
            os.chmod(temporary, path.stat().st_mode & 0o777)
            os.replace(temporary, path)
        else:
            # No replacement: an existing owner remains authoritative.
            os.link(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


@contextmanager
def sessions_lock() -> Iterator[None]:
    """Short flock around one session record read-modify-write. Taken after claim_lock."""
    fd = os.open(coord_dir() / "sessions.lock", os.O_RDONLY | os.O_CREAT, 0o666)
    with os.fdopen(fd, "r") as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        yield


def number_or_none(value: object) -> float | None:
    return value if isinstance(value, int | float) and not isinstance(value, bool) else None


def session_last_seen(record: dict) -> float | None:
    """Last activity of a session. Records from before T1122 fall back to started_at."""
    seen = number_or_none(record.get("last_seen"))
    return seen if seen is not None else number_or_none(record.get("started_at"))


def session_state(record: dict, live_claims: int, now: float, session_stale: int) -> str:
    """live: holds a live claim. ended: explicitly ended, or silent past the threshold."""
    if live_claims:
        return "live"
    if record.get("ended_at") is not None:
        return "ended"
    seen = session_last_seen(record)
    # A record without any usable time cannot be dated, so it is never expired.
    return "ended" if seen is not None and now - seen > session_stale else "idle"


def update_session(owner: str, change: Callable[[dict], dict | None]) -> bool:
    """Atomically rewrite an existing session record. `change` returns the new record or None.

    Returns False when there is no readable record or nothing changed. Never creates a record.
    """
    if not SESSION_OWNER_PATTERN.fullmatch(owner):
        return False
    path = coord_dir() / "sessions" / f"{owner}.json"
    if not path.exists():
        return False
    with sessions_lock():
        try:
            record = json.loads(path.read_text())
        except FileNotFoundError:
            return False
        except ValueError:
            return False
        if not isinstance(record, dict):
            return False
        updated = change(dict(record))
        if updated is None or updated == record:
            return False
        publish_claim(path, updated, replace=True)
        return True


def touch_session(owner: str) -> None:
    """Stamp `last_seen` on the owner's session record, if it has one."""
    now = int(time.time())
    update_session(owner, lambda record: {**record, "last_seen": now})


def refresh_session(args: argparse.Namespace, code: int) -> None:
    """Refresh last_seen for a command run with --owner. A failure only warns.

    A refused `register` is the one command that does not stamp: it is a name collision, so the
    caller may be a different session than the one holding the record.
    """
    owner = getattr(args, "owner", None)
    if args.command not in TOUCH_COMMANDS or not isinstance(owner, str):
        return
    if args.command == "register" and code != 0:
        return
    try:
        touch_session(owner)
    except Exception as error:
        print(f"warning: session {owner} last_seen not updated: {error}", file=sys.stderr)


def read_reservation(path: Path) -> dict:
    """Filename identity survives death between payload replacement and rename."""
    held = read_claim(path)
    task = path.stem.removeprefix("NEXT-")
    if (
        held.get("reserved_task") != task
        or held.get("task") not in {path.stem, task}
        or not isinstance(held.get("owner"), str)
        or not isinstance(held.get("slot"), int)
        or not isinstance(held.get("note"), str)
        or not isinstance(held.get("claimed_at"), int)
    ):
        raise ValueError(f"invalid reservation {path.name}")
    held["task"] = path.stem
    return held


# Task IDs longer than this are outside the supported allocator range. Reject them
# before scanning can silently omit a claim or produce a duplicate reservation.
MAX_TASK_ID_DIGITS = 5


TASK_HEADING_PATTERNS = (
    re.compile(r"^- \[([ xX~!])\] \*\*T([0-9]+)\."),
    re.compile(r"^## T([0-9]+)(?:\s|$)"),
)


def task_numbers_from_ledger(ledger: str) -> list[int]:
    """Extract IDs only from supported task headings, never from their prose."""
    numbers = []
    for line in ledger.splitlines():
        for pattern in TASK_HEADING_PATTERNS:
            match = pattern.match(line)
            if match:
                task_id = match.group(match.lastindex)
                if len(task_id) > MAX_TASK_ID_DIGITS:
                    raise ValueError(
                        f"task heading ID exceeds {MAX_TASK_ID_DIGITS} digits: T{task_id}"
                    )
                numbers.append(int(task_id))
                break
    if not numbers:
        raise ValueError("main:docs/tasks.md has no supported task headings")
    return numbers


def checked_numeric_id(task_id: str, source: str) -> int:
    if len(task_id) > MAX_TASK_ID_DIGITS:
        raise ValueError(f"{source} ID exceeds {MAX_TASK_ID_DIGITS} digits: T{task_id}")
    return int(task_id)


def liberate_reservations(paths: list[Path]) -> None:
    """Delete stale reservations and log each as a liberation of the reserved task."""
    for path in paths:
        held = read_reservation(path)
        path.unlink()
        log_event(
            held["task"],
            "liberate",
            held["owner"],
            held["slot"],
            {"silent": held["age"], "reservation": True},
        )


def cmd_next_id(args: argparse.Namespace) -> int:
    # Read the committed main ledger, never a worker's uncommitted task draft.
    ledger = subprocess.run(
        ["git", "show", "main:docs/tasks.md"], check=True, capture_output=True, text=True
    ).stdout
    numbers = task_numbers_from_ledger(ledger)
    # Ids the caller already wrote into its own uncommitted ledger (an unclaimed add).
    known = {int(n) for n in getattr(args, "known", ())}
    numbers.extend(known)
    pending = []
    stale = []
    for path in sorted((coord_dir() / "claims").glob("*.json")):
        match = re.fullmatch(r"(NEXT-)?T([0-9]+)", path.stem)
        if not match:
            continue
        task_id = checked_numeric_id(match[2], "claim")
        if match[1]:
            held = read_reservation(path)
            if held["age"] > args.stale:
                stale.append(path)
                continue
            pending.append((path, held))
        numbers.append(task_id)
    owned = [
        held for _, held in pending if (held["owner"], held["slot"]) == (args.owner, args.slot)
    ]
    if len(owned) > 1:
        raise ValueError("multiple pending reservations for this owner and slot")
    # All reads and validation finish before any mutation.
    if owned and int(owned[0]["reserved_task"][1:]) in known:
        # The reservation was already spent by an earlier add that was not claimed.
        stale.append(claim_path("NEXT-" + owned[0]["reserved_task"]))
        owned = []
    if owned:
        liberate_reservations(stale)
        path = claim_path("NEXT-" + owned[0]["reserved_task"])
        path.touch()
        print(owned[0]["reserved_task"])
        return 0
    task_id = max(numbers, default=0) + 1
    if len(str(task_id)) > MAX_TASK_ID_DIGITS:
        raise ValueError(f"task ID range exhausted at {MAX_TASK_ID_DIGITS} digits")
    liberate_reservations(stale)
    task = f"T{task_id}"
    publish_claim(
        claim_path("NEXT-" + task),
        {
            "task": "NEXT-" + task,
            "reserved_task": task,
            "owner": args.owner,
            "slot": args.slot,
            "note": args.note,
            "claimed_at": int(time.time()),
        },
    )
    log_event(task, "reserve", args.owner, args.slot, {"note": args.note})
    print(task)
    return 0


def open_subtasks_on_main(task: str) -> list[str]:
    """Open direct sub-tasks of `task` in the committed main ledger, [] when unknowable."""
    shown = subprocess.run(["git", "show", "main:docs/tasks.md"], capture_output=True, text=True)
    if shown.returncode != 0:
        return []
    ledger = taskmeta.parse(shown.stdout)
    return ledger.open_children(task) if task in ledger.tasks else []


def cmd_claim(args: argparse.Namespace) -> int:
    if re.fullmatch(r"NEXT-T[0-9]+", args.task):
        print("NEXT-T<n> is reserved; use next-id to allocate it", file=sys.stderr)
        return 1
    numeric_task = re.fullmatch(r"T([0-9]+)", args.task)
    if numeric_task and len(numeric_task[1]) > MAX_TASK_ID_DIGITS:
        print(f"task ID exceeds {MAX_TASK_ID_DIGITS} digits: {args.task}", file=sys.stderr)
        return 1
    if not args.allow_umbrella:
        open_children = open_subtasks_on_main(args.task)
        if open_children:
            print(
                f"{args.task} is an umbrella task with open sub-tasks "
                f"{' '.join(open_children)}; claim a leaf, or pass --allow-umbrella",
                file=sys.stderr,
            )
            return 1
    path = claim_path(args.task)
    reservation = claim_path("NEXT-" + args.task)
    if re.fullmatch(r"T[0-9]+", args.task) and not path.exists() and reservation.exists():
        held = read_reservation(reservation)
        if held["owner"] != args.owner:
            if held["age"] <= args.stale:
                print(f"{args.task} reserved by {held['owner']}", file=sys.stderr)
                return 1
            liberate_reservations([reservation])
        else:
            if args.slot is not None and args.slot != held["slot"]:
                print(f"{args.task} reserved for slot {held['slot']}", file=sys.stderr)
                return 1
            record = {key: value for key, value in held.items() if key != "age"}
            record["task"] = args.task
            if args.note is not None:
                record["note"] = args.note
            publish_claim(reservation, record, replace=True)
            # One pathname moves: a killed or failed rename leaves a complete,
            # retryable reservation, never a second published claim.
            os.rename(reservation, path)
            log_claim(
                args.task, args.owner, held["slot"], {"note": record["note"], "reserved": True}
            )
            print(f"claimed {args.task} as {args.owner}")
            return 0
    record = {
        "task": args.task,
        "owner": args.owner,
        "slot": args.slot if args.slot is not None else 0,
        "note": args.note if args.note is not None else "",
        "claimed_at": int(time.time()),
    }
    try:
        publish_claim(path, record)
    except FileExistsError:
        held = read_claim(path)
        if held["age"] <= args.stale or held["owner"] == args.owner:
            print(f"{args.task} held by {held['owner']} (silent {held['age']}s)", file=sys.stderr)
            return 1
        path.unlink()  # silent past the limit: the owner is presumed dead
        log_event(
            args.task,
            "liberate",
            held["owner"],
            held["slot"],
            {"silent": held["age"], "by": args.owner},
        )
        print(f"liberated stale claim of {held['owner']} on {args.task}", file=sys.stderr)
        return cmd_claim(args)
    log_claim(args.task, args.owner, record["slot"], {"note": record["note"]})
    print(f"claimed {args.task} as {args.owner}")
    return 0


def cmd_beat(args: argparse.Namespace) -> int:
    path = claim_path(args.task)
    if not path.exists() or read_claim(path)["owner"] != args.owner:
        print(f"{args.owner} does not hold {args.task}", file=sys.stderr)
        return 1
    path.touch()
    return 0


def cmd_release(args: argparse.Namespace) -> int:
    path = claim_path(args.task)
    held = read_claim(path) if path.exists() else None
    if held is not None and held["owner"] == args.owner:
        path.unlink()
        log_event(args.task, "release", args.owner, held["slot"])
        return 0
    print(f"{args.owner} does not hold {args.task}", file=sys.stderr)
    return 1


def cmd_list(args: argparse.Namespace) -> int:
    claims = sorted(
        (
            read_reservation(p) if re.fullmatch(r"NEXT-T[0-9]+", p.stem) else read_claim(p)
            for p in (coord_dir() / "claims").glob("*.json")
        ),
        key=lambda c: c["task"],
    )
    if args.json:
        records = [
            {
                **held,
                "state": "STALE" if held["age"] > args.stale else "alive",
                "stale": held["age"] > args.stale,
                "reservation": held["task"].startswith("NEXT-"),
                "agent_id": subagent_id(held["owner"], held["slot"], event_task(held["task"])),
            }
            for held in claims
        ]
        print(json.dumps(records))
        return 0
    for held in claims:
        state = "STALE" if held["age"] > args.stale else "alive"
        print(
            f"{held['task']}\t{held['owner']}\tslot={held['slot']}\t{held['age']}s\t{state}\t{held['note']}"
        )
    live = sum(1 for held in claims if held["age"] <= args.stale)
    print(f"{live} live claim(s), {len(claims) - live} stale")
    return 0


def cmd_reap(args: argparse.Namespace) -> int:
    for path in (coord_dir() / "claims").glob("*.json"):
        held = read_claim(path)
        if held["age"] > args.stale:
            path.unlink()
            log_event(
                held["task"],
                "liberate",
                held["owner"],
                held["slot"],
                {"silent": held["age"], "by": "reap"},
            )
            print(f"liberated {held['task']} (owner {held['owner']} silent {held['age']}s)")
    return 0


def seconds_value(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"seconds must be an integer >= 0, got {text!r}") from None
    if value < 0:
        raise argparse.ArgumentTypeError(f"seconds must be >= 0, got {value}")
    return value


def percent_value(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"percent must be an integer 0-100, got {text!r}"
        ) from None
    if not 0 <= value <= 100:
        raise argparse.ArgumentTypeError(f"percent must be 0-100, got {value}")
    return value


def cmd_progress(args: argparse.Namespace) -> int:
    """Beat the claim and store percent, kind and status text. Log only a real change."""
    path = claim_path(args.task)
    held = read_claim(path) if path.exists() else None
    if held is None or held["owner"] != args.owner:
        print(f"{args.owner} does not hold {args.task}", file=sys.stderr)
        return 1
    status = args.status if args.status is not None else held.get("status_text", "")
    kind = "assumed" if args.assumed else "measured"
    changed = held.get("percent") != args.percent or held.get("status_text") != status
    record = {key: value for key, value in held.items() if key != "age"}
    record.update(
        percent=args.percent,
        percent_kind=kind,
        status_text=status,
        progress_at=int(time.time()),
    )
    publish_claim(path, record, replace=True)
    if changed:
        log_event(
            args.task,
            "progress",
            args.owner,
            held["slot"],
            {"percent": args.percent, "percent_kind": kind, "status": status},
        )
    return 0


TASK_ID_PATTERN = re.compile(r"T[0-9]+")


def live_claim_count(owner: str, stale: int) -> int:
    """Live (not stale) task claims held by `owner`, reservations excluded."""
    count = 0
    for path in (coord_dir() / "claims").glob("*.json"):
        if path.stem.startswith("NEXT-"):
            continue
        try:
            held = read_claim(path)
        except (OSError, ValueError):
            continue
        if held.get("owner") == owner and held["age"] <= stale:
            count += 1
    return count


def adopt_session(args: argparse.Namespace, sessions: Path) -> int:
    """Adopt an explicitly assigned owner name. The caller holds the claim lock."""
    owner = args.owner
    if not re.fullmatch(rf"{re.escape(args.family)}-[a-z0-9]+", owner):
        print(
            f"cannot adopt {owner!r}: the name must be {args.family}-<token> with a token of "
            "lowercase letters and digits (for example n, w1, sup, main)",
            file=sys.stderr,
        )
        return 1
    path = sessions / f"{owner}.json"
    existing = None
    if path.exists():
        try:
            existing = json.loads(path.read_text())
        except ValueError:
            existing = {}
    if existing is not None and not isinstance(existing, dict):
        existing = {}
    record = {
        "owner": owner,
        "family": args.family,
        "role": args.role,
        "parent": args.parent,
        "started_at": int(time.time()),
        "adopted": True,
    }
    revived = (
        existing is not None
        and session_state(
            existing, live_claim_count(owner, args.stale), time.time(), args.session_stale
        )
        == "ended"
    )
    if revived:
        # An ended name is free: a new session may take it over with any role or parent.
        existing = None
    if existing is not None:
        clash = (
            existing.get("role", args.role) != args.role
            or existing.get("parent", args.parent) != args.parent
        )
        if clash and not args.force:
            print(
                f"{owner} is already registered as role={existing.get('role')} "
                f"parent={existing.get('parent')}, not role={args.role} parent={args.parent}. "
                "Two sessions picking the same name is the collision the allocator exists to "
                "prevent. Pick another name, or pass --force if this is the same session "
                "re-registering with new details.",
                file=sys.stderr,
            )
            return 1
        if clash and live_claim_count(owner, args.stale):
            print(
                f"warning: forcing {owner} over a record with parent={existing.get('parent')} "
                f"while it holds live claims, a different session may be using this name",
                file=sys.stderr,
            )
        record["started_at"] = int(existing.get("started_at", record["started_at"]))
        if "adopted" not in existing and not clash:
            record.pop("adopted")  # an allocated record stays as allocated
        if "last_seen" in existing:
            record["last_seen"] = existing["last_seen"]
        if record == existing:
            print(owner)
            return 0
    detail = {"family": args.family, "role": args.role, "parent": args.parent, "adopted": True}
    if revived:
        detail["revived"] = True
    with sessions_lock():
        publish_claim(path, record, replace=path.exists())
    log_event(None, "register", owner, None, detail)
    print(owner)
    return 0


def ended_session(path: Path, args: argparse.Namespace) -> bool:
    """True when the record at `path` is an ended session whose name may be reused."""
    try:
        record = json.loads(path.read_text())
    except (OSError, ValueError):
        return False
    if not isinstance(record, dict):
        return False
    state = session_state(record, 0, time.time(), args.session_stale)
    return state == "ended"


def cmd_register(args: argparse.Namespace) -> int:
    """Allocate `<family>-<n>` above every number seen in claims, sessions and recent logs.

    With --owner the named session is adopted instead (see `adopt_session`).
    """
    root = coord_dir()
    sessions = root / "sessions"
    sessions.mkdir(exist_ok=True)
    if args.owner is not None:
        return adopt_session(args, sessions)
    if args.force:
        print("--force only applies when adopting a name with --owner", file=sys.stderr)
        return 1
    pattern = re.compile(rf"{re.escape(args.family)}-([0-9]+)")
    owners = []
    for path in sessions.glob("*.json"):
        if not ended_session(path, args):
            owners.append(path.stem)
    for path in (root / "claims").glob("*.json"):
        try:
            owners.append(read_claim(path).get("owner", ""))
        except ValueError:
            continue
    cutoff = time.time() - REGISTER_WINDOW_SECONDS
    for event in read_events():
        if event["ts"] >= cutoff:
            owners.append(str(event.get("owner")))
    messages = root / "messages.jsonl"
    if messages.exists():
        for line in messages.read_bytes().decode(errors="replace").splitlines():
            try:
                message = json.loads(line)
            except ValueError:
                continue
            if isinstance(message, dict) and message.get("ts", 0) >= cutoff:
                owners += [str(message.get("from")), str(message.get("to"))]
    numbers = [int(m[1]) for m in map(pattern.fullmatch, owners) if m]
    owner = f"{args.family}-{max(numbers, default=0) + 1}"
    path = sessions / f"{owner}.json"
    record = {
        "owner": owner,
        "family": args.family,
        "role": args.role,
        "parent": args.parent,
        "started_at": int(time.time()),
    }
    detail = {"family": args.family, "role": args.role, "parent": args.parent}
    with sessions_lock():
        reused = path.exists()  # only an ended record is left over here, it is replaced
        publish_claim(path, record, replace=reused)
    if reused:
        detail["reused"] = True
    log_event(None, "register", owner, None, detail)
    print(owner)
    return 0


def cmd_whoami(args: argparse.Namespace) -> int:
    if args.owner is None:
        print(
            "whoami needs --owner <id>: there is no hidden session state. "
            "Allocate an id with `coord register --family claude|codex`.",
            file=sys.stderr,
        )
        return 1
    path = coord_dir() / "sessions" / f"{args.owner}.json"
    if not path.exists():
        print(f"{args.owner} is not registered", file=sys.stderr)
        return 1
    record = json.loads(path.read_text())
    if isinstance(record, dict):
        live = live_claim_count(args.owner, args.stale)
        state = session_state(record, live, time.time(), args.session_stale)
        record = {**record, "state": state, "last_seen": session_last_seen(record)}
    print(json.dumps(record))
    return 0


def session_records() -> list[dict]:
    """Registered sessions, oldest first. Unreadable or non-object records are skipped."""
    records = []
    for path in (coord_dir() / "sessions").glob("*.json"):
        try:
            record = json.loads(path.read_text())
        except (OSError, ValueError):
            continue
        if isinstance(record, dict):
            records.append(record)
    return sorted(records, key=lambda r: (r.get("started_at") or 0, str(r.get("owner"))))


def cmd_sessions(args: argparse.Namespace) -> int:
    """Read-only list of registered sessions with live claim count, state and last_seen.

    Ended sessions are hidden unless --all.
    """
    now = time.time()
    rows = []
    for record in session_records():
        live = live_claim_count(str(record.get("owner")), args.stale)
        rows.append(
            {
                "owner": record.get("owner"),
                "family": record.get("family"),
                "role": record.get("role"),
                "parent": record.get("parent"),
                "started_at": record.get("started_at"),
                "adopted": bool(record.get("adopted", False)),
                "live_claims": live,
                "state": session_state(record, live, now, args.session_stale),
                "last_seen": session_last_seen(record),
            }
        )
    hidden = 0
    if not args.all:
        hidden = sum(1 for row in rows if row["state"] == "ended")
        rows = [row for row in rows if row["state"] != "ended"]
    if args.json:
        print(json.dumps(rows))
        return 0
    for row in rows:
        print(
            f"{row['owner']}\t{row['family']}\t{row['role']}\t{row['parent'] or '-'}\t"
            f"{stamp(row['started_at'] or 0)}\tlive_claims={row['live_claims']}\t"
            f"state={row['state']}\tlast_seen={stamp(row['last_seen'] or 0)}"
        )
    print(f"{len(rows)} session(s)")
    if hidden:
        print(f"note: {hidden} ended session(s) hidden, pass --all to list them", file=sys.stderr)
    return 0


def cmd_session_end(args: argparse.Namespace) -> int:
    """Mark a session ended. Idempotent. Refuses while it holds live claims unless --force."""
    live = live_claim_count(args.owner, args.stale)
    if live and not args.force:
        print(
            f"{args.owner} holds {live} live claim(s): release them first, or pass --force",
            file=sys.stderr,
        )
        return 1
    already = []

    def mark(record: dict) -> dict | None:
        if record.get("ended_at") is not None:
            already.append(True)
            return None
        return {**record, "ended_at": int(time.time()), "ended_reason": args.reason}

    if not update_session(args.owner, mark):
        if already:
            print(f"{args.owner} is already ended")
            return 0
        print(f"{args.owner} is not registered or its record is unreadable", file=sys.stderr)
        return 1
    log_event(
        None,
        "register",
        args.owner,
        None,
        {"ended": True, "reason": args.reason, "forced": bool(live), "live_claims": live},
    )
    print(f"ended {args.owner}")
    return 0


def cmd_block(args: argparse.Namespace) -> int:
    blockers = [part for part in (args.on or "").split(",") if part]
    bad = [part for part in blockers if not TASK_ID_PATTERN.fullmatch(part)]
    if bad or not (blockers or args.external):
        print("block needs --on T<n>[,T<m>] and/or --external TEXT", file=sys.stderr)
        return 1
    if not TASK_ID_PATTERN.fullmatch(args.task):
        print(f"{args.task} is not a task id", file=sys.stderr)
        return 1
    log_event(
        args.task,
        "block",
        args.owner,
        args.slot,
        {"on": blockers, "external": args.external, "reason": args.reason},
    )
    return 0


def cmd_unblock(args: argparse.Namespace) -> int:
    if not TASK_ID_PATTERN.fullmatch(args.task):
        print(f"{args.task} is not a task id", file=sys.stderr)
        return 1
    if not audit_task(read_events(), args.task, int(time.time()), {})["blocks"]["open"]:
        print(f"note: no open block recorded for {args.task}", file=sys.stderr)
    log_event(args.task, "unblock", args.owner, args.slot, {"reason": args.reason})
    return 0


def cmd_event(args: argparse.Namespace) -> int:
    """Generic logger so merge_task and the orchestrator can record merge, publish and so on."""
    if not TASK_ID_PATTERN.fullmatch(args.task):
        print(f"{args.task} is not a task id", file=sys.stderr)
        return 1
    log_event(args.task, args.kind, args.owner, args.slot, args.detail)
    return 0


def format_detail(detail: object) -> str:
    if detail is None:
        return ""
    if isinstance(detail, dict):
        return " ".join(f"{key}={json.dumps(value)}" for key, value in detail.items())
    return str(detail)


def stamp(ts: float) -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(ts))


def cmd_history(args: argparse.Namespace) -> int:
    events = sorted((e for e in read_events() if e.get("task") == args.task), key=lambda e: e["ts"])
    if args.important:
        events = [e for e in events if e["kind"] != "progress"]
    if args.json:
        print(json.dumps(events))
        return 0
    for event in events:
        print(
            f"{stamp(event['ts'])} UTC  {event.get('owner')}  {event['kind']}  "
            f"{format_detail(event.get('detail'))}".rstrip()
        )
    return 0


def detail_of(event: dict) -> dict:
    detail = event.get("detail")
    return detail if isinstance(detail, dict) else {}


def audit_task(events: list[dict], task: str, now: int, live: dict[str, dict]) -> dict:
    """Roll up the events of one task.

    Held time runs from a claim or handoff to the next release or liberation (to `now` while
    the claim file still exists). The silence recorded at a liberation, and the current silence
    of a live claim older than the stale limit (`live[task]["stale_age"]`), count as stale time.
    """
    mine = sorted((e for e in events if e.get("task") == task), key=lambda e: e["ts"])
    owners: list[str] = []
    held_by: dict[str, int] = {}
    stale_by: dict[str, int] = {}
    claims = handoffs = replacements = stale_liberations = block_count = 0
    blocked_seconds = 0
    block_opened: float | None = None
    open_claim: tuple[str, float] | None = None
    curve: list[list[float]] = []
    last_progress: dict = {}
    last_end = "none"

    def close(end_ts: float, silent: int = 0) -> None:
        nonlocal open_claim
        if open_claim is None:
            return
        holder, start = open_claim
        span = max(0, int(end_ts - start))
        held_by[holder] = held_by.get(holder, 0) + span
        stale_by[holder] = stale_by.get(holder, 0) + min(silent, span)
        open_claim = None

    for event in mine:
        kind = event["kind"]
        owner = event.get("owner")
        if owner and owner not in owners:
            owners.append(owner)
        if kind in ("claim", "handoff"):
            close(event["ts"])
            claims += 1
            if kind == "handoff":
                handoffs += 1
                replacements += last_end == "liberate"
            open_claim = (owner, event["ts"])
        elif kind in ("release", "liberate"):
            silent = detail_of(event).get("silent")
            if kind == "liberate" and not detail_of(event).get("reservation"):
                stale_liberations += 1
            close(event["ts"], silent if kind == "liberate" and isinstance(silent, int) else 0)
            last_end = kind
        elif kind == "block":
            block_count += 1
            block_opened = event["ts"] if block_opened is None else block_opened
        elif kind == "unblock" and block_opened is not None:
            blocked_seconds += max(0, int(event["ts"] - block_opened))
            block_opened = None
        elif kind == "progress":
            detail = detail_of(event)
            last_progress = {**detail, "ts": event["ts"]}
            curve.append([event["ts"], detail.get("percent")])
    if open_claim is not None:
        holder = open_claim[0]
        if task in live:
            close(now)
            stale_by[holder] = stale_by.get(holder, 0) + live[task]["stale_age"]
        else:
            close(mine[-1]["ts"])
    if block_opened is not None:
        blocked_seconds += max(0, int(now - block_opened))
    held_total = sum(held_by.values())
    stale_total = sum(stale_by.values())
    return {
        "task": task,
        "milestone": next((e["milestone"] for e in reversed(mine) if e.get("milestone")), None),
        "events": len(mine),
        "first_event_ts": mine[0]["ts"] if mine else None,
        "last_event_ts": mine[-1]["ts"] if mine else None,
        "owners": owners,
        "claims": claims,
        "handoffs": handoffs,
        "replacements": replacements,
        "stale_liberations": stale_liberations,
        "held_seconds": held_total,
        "stale_seconds": stale_total,
        "active_seconds": held_total - stale_total,
        "held_by_owner": held_by,
        "blocks": {
            "count": block_count,
            "seconds": blocked_seconds,
            "open": block_opened is not None,
        },
        "progress": {
            "updates": len(curve),
            "first_ts": curve[0][0] if curve else None,
            "last_ts": curve[-1][0] if curve else None,
            "last_percent": last_progress.get("percent"),
            "last_percent_kind": last_progress.get("percent_kind"),
            "last_status": last_progress.get("status"),
            "curve": curve,
        },
    }


def merge_audits(target: str, tasks: list[dict]) -> dict:
    owners: list[str] = []
    for audit in tasks:
        owners += [o for o in audit["owners"] if o not in owners]
    firsts = [a["first_event_ts"] for a in tasks if a["first_event_ts"] is not None]
    lasts = [a["last_event_ts"] for a in tasks if a["last_event_ts"] is not None]
    return {
        "milestone": target,
        "tasks": tasks,
        "task_count": len(tasks),
        "tasks_with_events": sum(1 for a in tasks if a["events"]),
        "events": sum(a["events"] for a in tasks),
        "first_event_ts": min(firsts, default=None),
        "last_event_ts": max(lasts, default=None),
        "owners": owners,
        "claims": sum(a["claims"] for a in tasks),
        "handoffs": sum(a["handoffs"] for a in tasks),
        "replacements": sum(a["replacements"] for a in tasks),
        "stale_liberations": sum(a["stale_liberations"] for a in tasks),
        "held_seconds": sum(a["held_seconds"] for a in tasks),
        "stale_seconds": sum(a["stale_seconds"] for a in tasks),
        "active_seconds": sum(a["active_seconds"] for a in tasks),
        "blocks": {
            "count": sum(a["blocks"]["count"] for a in tasks),
            "seconds": sum(a["blocks"]["seconds"] for a in tasks),
            "open": sum(a["blocks"]["open"] for a in tasks),
        },
    }


def live_claim_info(stale: int) -> dict[str, dict]:
    info = {}
    for path in (coord_dir() / "claims").glob("*.json"):
        if path.stem.startswith("NEXT-"):
            continue
        try:
            held = read_claim(path)
        except ValueError:
            continue
        info[held["task"]] = {
            "owner": held["owner"],
            "stale_age": held["age"] if held["age"] > stale else 0,
        }
    return info


def cmd_audit(args: argparse.Namespace) -> int:
    events = read_events()
    now = int(time.time())
    live = live_claim_info(args.stale)
    if re.fullmatch(r"T[0-9]+", args.target):
        result = audit_task(events, args.target, now, live)
        if not result["events"] and args.target not in live:
            print(f"no events recorded for {args.target}", file=sys.stderr)
            return 1
    elif re.fullmatch(r"M[0-9]+", args.target):
        ledger = main_ledger()
        tasks = {
            task
            for task in (ledger.tasks if ledger else {})
            if ledger.resolve(task, "milestone") == args.target
        }
        tasks |= {e["task"] for e in events if e.get("milestone") == args.target and e.get("task")}
        if not tasks:
            print(f"no tasks known for {args.target}", file=sys.stderr)
            return 1
        ordered = sorted(tasks, key=taskmeta.numeric)
        result = merge_audits(args.target, [audit_task(events, t, now, live) for t in ordered])
    else:
        print("audit target must be T<n> or M<n>", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(result))
    else:
        print(format_audit(result))
    return 0


def format_audit(result: dict) -> str:
    def clock(seconds: int) -> str:
        return f"{seconds // 3600}h{seconds % 3600 // 60:02d}m{seconds % 60:02d}s"

    head = result.get("task") or result["milestone"]
    lines = [f"audit {head}"]
    if "tasks" in result:
        lines.append(f"tasks: {result['task_count']} ({result['tasks_with_events']} with events)")
    lines += [
        f"owners ever: {', '.join(result['owners']) or '-'}",
        f"claims: {result['claims']}  handoffs: {result['handoffs']}  "
        f"replacements: {result['replacements']}  stale liberations: {result['stale_liberations']}",
        f"held: {clock(result['held_seconds'])}  active: {clock(result['active_seconds'])}  "
        f"stale: {clock(result['stale_seconds'])}",
        f"blocks: {result['blocks']['count']}  blocked: {clock(result['blocks']['seconds'])}"
        f"{'  (still blocked)' if result['blocks']['open'] else ''}",
    ]
    if "progress" in result:
        progress = result["progress"]
        lines.append(
            f"progress: {progress['updates']} update(s), last {progress['last_percent']}% "
            f"({progress['last_percent_kind']}) {progress['last_status'] or ''}".rstrip()
        )
        if progress["curve"]:
            lines.append(
                "curve: "
                + " ".join(f"{stamp(ts)[11:]}={percent}%" for ts, percent in progress["curve"])
            )
    return "\n".join(lines)


def cmd_slots(args: argparse.Namespace) -> int:
    """Report which of the SLOT_COUNT subagent slots hold a live claim by this owner."""
    used = {
        held["slot"]
        for held in (read_claim(p) for p in (coord_dir() / "claims").glob("*.json"))
        if held["owner"] == args.owner and held["age"] <= args.stale
    }
    free = [slot for slot in range(1, SLOT_COUNT + 1) if slot not in used]
    print(f"{args.owner} free slots: {free if free else 'none'}")
    return 0


def cmd_say(args: argparse.Namespace) -> int:
    line = json.dumps(
        {"ts": int(time.time()), "from": args.owner, "to": args.to, "text": args.text}
    )
    with open(coord_dir() / "messages.jsonl", "a") as handle:
        handle.write(line + "\n")
    return 0


def cmd_read(args: argparse.Namespace) -> int:
    log = coord_dir() / "messages.jsonl"
    if not log.exists():
        return 0
    cutoff = time.time() - args.since
    for line in log.read_text().splitlines():
        message = json.loads(line)
        if message["ts"] >= cutoff and message["to"] in ("all", args.owner):
            stamp = time.strftime("%H:%M:%S", time.gmtime(message["ts"]))
            print(f"{stamp} {message['from']} -> {message['to']}: {message['text']}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--stale",
        type=int,
        default=DEFAULT_STALE_SECONDS,
        help="seconds of silence after which a claim counts as dead",
    )
    sub = parser.add_subparsers(dest="command", required=True)
    for name, func in (("claim", cmd_claim), ("beat", cmd_beat), ("release", cmd_release)):
        p = sub.add_parser(name)
        p.add_argument("task")
        p.add_argument("--owner", required=True, help="e.g. claude-main, codex-2")
        if name == "claim":
            p.add_argument(
                "--slot", type=int, default=None, help="subagent slot 1-3, 0 for the session itself"
            )
            p.add_argument("--note", default=None)
            p.add_argument(
                "--allow-umbrella",
                action="store_true",
                help="claim a parent that still has open sub-tasks",
            )
        p.set_defaults(func=func)
    p = sub.add_parser("next-id", help="reserve the next task number from main and shared claims")
    p.add_argument("--owner", required=True)
    p.add_argument("--slot", type=int, default=0)
    p.add_argument("--note", default="")
    p.set_defaults(func=cmd_next_id)
    p = sub.add_parser("list")
    p.add_argument("--json", action="store_true", help="full records with state and agent_id")
    p.set_defaults(func=cmd_list)
    sub.add_parser("reap").set_defaults(func=cmd_reap)
    p = sub.add_parser("slots")
    p.add_argument("--owner", required=True)
    p.set_defaults(func=cmd_slots)
    p = sub.add_parser("say")
    p.add_argument("--owner", required=True)
    p.add_argument("--to", default="all")
    p.add_argument("text")
    p.set_defaults(func=cmd_say)
    p = sub.add_parser("read")
    p.add_argument("--owner", required=True)
    p.add_argument("--since", type=int, default=3600, help="seconds of history")
    p.set_defaults(func=cmd_read)
    p = sub.add_parser("progress", help="beat the claim and record percent and status text")
    p.add_argument("task")
    p.add_argument("--owner", required=True)
    p.add_argument("--percent", type=percent_value, required=True, help="0-100")
    p.add_argument("--assumed", action="store_true", help="percent is an estimate, not measured")
    p.add_argument("--status", default=None, help="short status text")
    p.set_defaults(func=cmd_progress)
    p = sub.add_parser(
        "register", help="allocate a unique <family>-<n> session id, or adopt --owner NAME"
    )
    p.add_argument("--family", required=True, choices=("claude", "codex"))
    p.add_argument(
        "--owner",
        default=None,
        help="adopt this assigned name (<family>-<token>) instead of allocating one",
    )
    p.add_argument(
        "--force",
        action="store_true",
        help="with --owner: overwrite an existing record that has another role or parent",
    )
    p.add_argument("--role", default="worker", choices=("orchestrator", "worker", "subagent"))
    p.add_argument("--parent", default=None, help="owner id of the spawning session")
    p.add_argument(
        "--session-stale",
        type=seconds_value,
        default=SESSION_STALE_SECONDS,
        help="idle seconds after which a session record counts as ended (name reusable)",
    )
    p.set_defaults(func=cmd_register)
    p = sub.add_parser("whoami", help="print a registered session record")
    p.add_argument("--owner", default=None)
    p.add_argument("--session-stale", type=seconds_value, default=SESSION_STALE_SECONDS)
    p.set_defaults(func=cmd_whoami)
    p = sub.add_parser("sessions", help="list registered sessions with live claim counts")
    p.add_argument("--json", action="store_true")
    p.add_argument("--all", action="store_true", help="also list ended sessions")
    p.add_argument(
        "--session-stale",
        type=seconds_value,
        default=SESSION_STALE_SECONDS,
        help="seconds without any command after which an idle session counts as ended",
    )
    p.set_defaults(func=cmd_sessions)
    p = sub.add_parser("session-end", help="mark a session ended")
    p.add_argument("--owner", required=True)
    p.add_argument("--reason", default=None)
    p.add_argument("--force", action="store_true", help="end it although it holds live claims")
    p.set_defaults(func=cmd_session_end)
    p = sub.add_parser("block", help="log that a task is blocked")
    p.add_argument("task")
    p.add_argument("--owner", required=True)
    p.add_argument("--on", default=None, help="comma separated blocking tasks, e.g. T12,T13")
    p.add_argument("--external", default=None, help="blocker outside the ledger")
    p.add_argument("--reason", required=True)
    p.add_argument("--slot", type=int, default=None)
    p.set_defaults(func=cmd_block)
    p = sub.add_parser("unblock", help="log that a task is no longer blocked")
    p.add_argument("task")
    p.add_argument("--owner", required=True)
    p.add_argument("--reason", required=True)
    p.add_argument("--slot", type=int, default=None)
    p.set_defaults(func=cmd_unblock)
    p = sub.add_parser("event", help="log a generic event (merge, publish, stall-probe, recovery)")
    p.add_argument("task")
    p.add_argument("--owner", required=True)
    p.add_argument("--kind", required=True, choices=EVENT_KINDS)
    p.add_argument("--detail", default=None)
    p.add_argument("--slot", type=int, default=None)
    p.set_defaults(func=cmd_event)
    p = sub.add_parser("history", help="chronological events of a task")
    p.add_argument("task")
    p.add_argument("--important", action="store_true", help="hide progress events")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_history)
    p = sub.add_parser("audit", help="rollup of owners, time held, handoffs, blocks and progress")
    p.add_argument("target", help="T<n> or M<n>")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_audit)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    locked = {"claim", "beat", "release", "list", "reap", "slots", "next-id"}
    locked |= {"progress", "register", "block", "unblock", "event", "session-end"}
    if args.command in locked:
        with claim_lock():
            code = args.func(args)
            refresh_session(args, code)
            return code
    code = args.func(args)
    refresh_session(args, code)
    return code


if __name__ == "__main__":
    sys.exit(main())
