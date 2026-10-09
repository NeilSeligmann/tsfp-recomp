# SPDX-License-Identifier: GPL-3.0-or-later
"""Spawn, watch and restart supervised Claude and Codex worker sessions.

State lives in `<git-common-dir>/agent-coord/fleet.json`, next to the claim ledger that
`tools/agents/coord.py` keeps, shared by every worktree and never committed. Claims and
messages go through the coord CLI. Everything that touches the `claude` binary sits in
`ClaudeBackend`. See docs/superpowers/specs/2026-10-03-supervisor-fleet-design.md.
"""

import argparse
import fcntl
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from uuid import uuid4

from tools.agents import coord
from tools.agents.codex_output import write_json

SUPERVISOR = "claude-sup"
GRACE_SECONDS = 120
TICK_SECONDS = 300
TICK_STALE_SECONDS = 3 * TICK_SECONDS
MAX_ATTEMPTS = 2
TOKEN_ENV = "CLAUDE_CODE_OAUTH_TOKEN"
DEAD_STATES = {"stopped"}
REPORT_STATUSES = {"done", "blocked", "failed"}
REPO_ROOT = Path(__file__).resolve().parents[2]
SESSION_LINE = re.compile(r"backgrounded\s+·\s+([0-9a-f]{8})\s+·")


class FleetError(Exception):
    """A failure the CLI reports on stderr with exit code 1."""


def state_path() -> Path:
    return coord.coord_dir() / "fleet.json"


@contextmanager
def state_lock() -> Iterator[None]:
    fd = os.open(coord.coord_dir() / "fleet.lock", os.O_RDONLY | os.O_CREAT, 0o666)
    with os.fdopen(fd, "r") as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        yield


def load_state() -> dict:
    path = state_path()
    if not path.exists():
        return {"supervisor": SUPERVISOR, "token_file": None, "last_tick": 0, "workers": {}}
    return json.loads(path.read_text())


def save_state(state: dict) -> None:
    path = state_path()
    temporary = path.parent / f".fleet-{uuid4().hex}.tmp"
    temporary.write_text(json.dumps(state, indent=2) + "\n")
    os.replace(temporary, path)


def coord_cli(
    *args: str, stale: int = coord.DEFAULT_STALE_SECONDS
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-m", "tools.agents.coord", "--stale", str(stale), *args],
        capture_output=True,
        text=True,
        timeout=60,
        env={**os.environ, "PYTHONPATH": str(REPO_ROOT)},
    )


def ancestor_token(start: int, proc: Path = Path("/proc")) -> str | None:
    """Token from the nearest ancestor process whose launch environment holds one.

    The launcher (claude-docker's account-supervisor) puts CLAUDE_CODE_OAUTH_TOKEN in the
    `claude` process environment, and Claude Code keeps it out of the shells its tools
    run. A tool process such as this one therefore reads it from the claude process above.
    """
    prefix = f"{TOKEN_ENV}=".encode()
    pid = start
    while pid > 1:
        try:
            for entry in (proc / str(pid) / "environ").read_bytes().split(b"\0"):
                if entry.startswith(prefix):
                    return entry[len(prefix) :].decode()
            fields = (proc / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()
            pid = int(fields[1])
        except (OSError, ValueError, IndexError):
            return None
    return None


def codex_runtime_identity() -> str:
    """Bind local /proc observations to a boot and PID namespace, not a mount path."""
    try:
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        namespace = Path("/proc/self/ns/pid").stat()
    except OSError as exc:
        raise FleetError("cannot establish Codex PID namespace visibility") from exc
    return f"{boot}:{namespace.st_dev}:{namespace.st_ino}"


def codex_runtime_visible(state: dict) -> bool:
    recorded = state.get("codex_runtime")
    return bool(recorded) and recorded == codex_runtime_identity()


def background_sessions(entries: list[dict]) -> dict[str, dict]:
    return {entry["id"]: entry for entry in entries if entry.get("kind") == "background"}


class ClaudeBackend:
    """Everything that touches the `claude` binary, kept behind one seam."""

    def __init__(self, binary: str, token_file: Path | None) -> None:
        self.binary = binary
        self.token_file = token_file

    def token(self) -> str | None:
        """The token to hand a spawned session: the token file, else the one this session has."""
        if self.token_file is not None:
            if not self.token_file.is_file():
                raise FleetError(f"token file not found: {self.token_file}")
            return self.token_file.read_text().strip()
        return os.environ.get(TOKEN_ENV) or ancestor_token(os.getppid())

    def auth(self) -> tuple[list[str], dict[str, str]]:
        """Extra argv and environment that log a spawned session in."""
        token = self.token()
        if not token:
            print(
                f"fleet: no {TOKEN_ENV} in this environment or any parent process and no "
                "--token-file, spawned sessions may start logged out (state blocked)",
                file=sys.stderr,
            )
            return [], {}
        return [], {TOKEN_ENV: token}

    def spawn(self, name: str, brief: str) -> str:
        extra_args, extra_env = self.auth()
        result = subprocess.run(
            [
                self.binary,
                "--bg",
                "--name",
                name,
                "--permission-mode",
                "bypassPermissions",
                *extra_args,
                brief,
            ],
            capture_output=True,
            text=True,
            timeout=120,
            env={**os.environ, **extra_env},
        )
        match = SESSION_LINE.search(result.stdout)
        if result.returncode != 0 or match is None:
            raise FleetError(
                f"could not spawn {name}: exit {result.returncode}: {result.stderr.strip()}"
            )
        return match.group(1)

    def entries(self) -> list[dict]:
        """Every session `claude agents` shows, interactive and background."""
        result = subprocess.run(
            [self.binary, "agents", "--json", "--all"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if result.returncode != 0:
            raise FleetError(f"claude agents failed: {result.stderr.strip()}")
        return json.loads(result.stdout)

    def sessions(self) -> dict[str, dict]:
        return background_sessions(self.entries())

    def stop(self, session: str, pid: int | None = None) -> None:
        subprocess.run([self.binary, "stop", session], capture_output=True, text=True, timeout=60)


class CodexBackend:
    """Run one non-interactive Codex exec per assignment and resume by thread UUID."""

    def __init__(self, binary: str) -> None:
        self.binary = binary
        self.pids: dict[str, int] = {}
        self.pid_starts: dict[str, int] = {}
        self.diagnostics: dict[str, str] = {}

    @staticmethod
    def _finish_failed_launch(
        process: subprocess.Popen | None, collector: subprocess.Popen | None
    ) -> None:
        """Close only this launch's descriptors and reap its owned children."""
        if process is not None:
            if process.stdout is not None:
                process.stdout.close()
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            else:
                process.wait()
        if collector is not None:
            try:
                collector.wait(timeout=5)
            except subprocess.TimeoutExpired:
                collector.terminate()
                collector.wait(timeout=5)

    def spawn(
        self, name: str, brief: str, resume: str | None = None, generation: int | None = None
    ) -> str:
        if resume:
            command = [
                self.binary,
                "exec",
                "resume",
                "--json",
                "-c",
                'approval_policy="never"',
                resume,
                brief,
            ]
        else:
            command = [
                self.binary,
                "exec",
                "--json",
                "-c",
                'approval_policy="never"',
                "-s",
                "workspace-write",
                "-C",
                str(REPO_ROOT),
                brief,
            ]
        directory = Path(tempfile.mkdtemp(prefix="codex-output-", dir=coord.coord_dir()))
        os.chmod(directory, 0o700)
        receipt = {
            "owner": name,
            "task": re.search(r"\bASSIGN (T\d+)\b", brief).group(1)
            if re.search(r"\bASSIGN (T\d+)\b", brief)
            else None,
            "backend": "codex",
            "source": subprocess.run(
                ["git", "rev-parse", "HEAD"], capture_output=True, text=True
            ).stdout.strip(),
            "resume_thread": resume,
            "state": "launching",
            "generation": generation,
            "assignment_id": directory.name,
            "exit_status": "unavailable",
            "output_limit_bytes": 256 * 1024,
        }
        write_json(directory / "assignment.json", receipt)
        process = None
        collector = None
        try:
            process = subprocess.Popen(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                start_new_session=True,
                env=os.environ.copy(),
            )
            receipt.update(pid=process.pid, pid_start=proc_start_time(process.pid) or 0)
            write_json(directory / "assignment.json", receipt)
            collector = subprocess.Popen(
                [sys.executable, str(Path(__file__).with_name("codex_output.py")), str(directory)],
                stdin=process.stdout,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            receipt["collector_pid"] = collector.pid
            write_json(directory / "assignment.json", receipt)
            # The collector is the sole pipe reader after handoff.
            process.stdout.close()
            deadline = time.monotonic() + 60
            thread_id = None
            while time.monotonic() < deadline:
                try:
                    thread_id = json.loads((directory / "thread.json").read_text())["thread_id"]
                except (OSError, ValueError, KeyError):
                    pass
                if thread_id or collector.poll() is not None:
                    break
                time.sleep(0.1)
        except Exception as error:
            receipt.update(state="launch_failed", error_type=type(error).__name__)
            write_json(directory / "assignment.json", receipt)
            self._finish_failed_launch(process, collector)
            raise
        if not thread_id:
            self._finish_failed_launch(process, collector)
            receipt.update(state="handshake_failed")
            write_json(directory / "assignment.json", receipt)
            if resume:
                return self.spawn(
                    name,
                    f"The previous Codex thread {resume} could not be resumed. Continue the "
                    f"assignment from its existing worktree and files. {brief}",
                    generation=generation,
                )
            raise FleetError(
                f"could not start Codex worker {name}; private diagnostics: {directory}"
            )
        self.pids[thread_id] = process.pid
        self.pid_starts[thread_id] = receipt["pid_start"]
        self.diagnostics[thread_id] = str(directory)
        receipt.update(state="thread_started", thread_id=thread_id)
        write_json(directory / "assignment.json", receipt)
        return thread_id

    def entries(self) -> list[dict]:
        """Visible managed Codex processes in the common backend session shape.

        Unlike a remote Claude daemon, the local process set may legitimately be
        empty when all assigned execs exited or all workers are idle.
        """
        return [
            {"id": session, "kind": "background", **details}
            for session, details in self.sessions().items()
        ]

    def sessions(self) -> dict[str, dict]:
        state = load_state()
        return {
            worker["session"]: {"pid": worker["pid"], "state": "working"}
            for worker in state["workers"].values()
            if worker.get("session")
            and _pid_matches_codex(worker.get("pid"), worker["session"], worker.get("pid_start"))
        }

    def stop(self, session: str, pid: int | None = None, pid_start: int | None = None) -> None:
        pid = pid or self.pids.get(session)
        pid_start = pid_start or self.pid_starts.get(session)
        if pid and _pid_matches_codex(pid, session, pid_start):
            os.killpg(pid, signal.SIGTERM)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and _pid_matches_codex(pid, session, pid_start):
                time.sleep(0.05)
            if _pid_matches_codex(pid, session, pid_start):
                os.killpg(pid, signal.SIGKILL)


def proc_start_time(pid: int) -> int | None:
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return int(fields[19])
    except (OSError, ValueError, IndexError):
        return None


def _pid_matches_codex(pid: int | None, session: str, started: int | None = None) -> bool:
    """Guard PID health checks and signaling against process-id reuse."""
    if not pid:
        return False
    try:
        args = Path(f"/proc/{pid}/cmdline").read_bytes().replace(b"\0", b" ").decode()
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    except (OSError, UnicodeDecodeError):
        return False
    return (
        fields[0] != "Z"
        and "codex" in args
        and (started is None or proc_start_time(pid) == started)
    )


def build_brief(name: str, supervisor: str) -> str:
    return (
        f"You are {name}, a supervised worker. {supervisor} is your supervisor. "
        "Invoke the orchestrate skill and follow its Worker mode section. "
        f"Take work only from ASSIGN messages sent to you (run: python3 -m tools.agents.coord "
        f"read --owner {name}), never pick tasks yourself. "
        "Start every worktree slug with the lowercase task id, for example t443-copy-composition. "
        "Run unattended: never ask the user anything and never wait for a reply. "
        f"Report each finished task to {supervisor} with a REPORT message."
    )


def build_codex_assignment(
    name: str, supervisor: str, task: str, slot: int, note: str, resume: str | None
) -> str:
    brief = (
        f"You are {name}, a supervised Codex worker. {supervisor} is your supervisor. "
        "Invoke the orchestrate skill and follow its Worker mode section. Handle this one "
        "assignment, then report and exit. Do not wait for future assignments or choose tasks. "
        f"Work unattended: do not ask the user or wait for a reply. ASSIGN {task} slot={slot} "
        f"note={json.dumps(note)}"
    )
    return f"{brief} resume={resume}" if resume else brief


def is_dead(
    worker: dict,
    session: dict | None,
    claims: list[dict],
    now: int,
    stale: int,
    grace: int,
) -> str | None:
    """Return why a worker counts as dead, or None if it is alive."""
    if session is None:
        if worker.get("backend") == "codex" and not worker["assigned"]:
            return None
        return "session missing"
    if session.get("state") in DEAD_STATES:
        return f"session {session['state']}"
    # A killed process keeps its last `state` (seen: "working") but loses `pid` and `status`.
    if session.get("pid") is None:
        return "session not running"
    if now - worker["spawned"] <= grace:
        return None
    if session.get("state") == "blocked":
        return "session blocked"
    held = [c for c in claims if c["task"] in worker["assigned"]]
    if worker["assigned"] and all(c["age"] > stale for c in held):
        return "claims stale or lost"
    return None


def parse_report(text: str, check_main: bool) -> dict:
    body = text.strip().removeprefix("REPORT").strip()
    try:
        data = json.loads(body)
    except json.JSONDecodeError as error:
        raise FleetError(f"report is not valid JSON: {error}") from error
    if not isinstance(data, dict):
        raise FleetError("report must be a JSON object")
    task = data.get("task")
    if not isinstance(task, str) or not re.fullmatch(r"T[0-9]+", task):
        raise FleetError("report needs a task id like T123")
    status = data.get("status")
    if status not in REPORT_STATUSES:
        raise FleetError(f"status must be one of {sorted(REPORT_STATUSES)}")
    merge = data.get("merge", "")
    if status == "done":
        if not isinstance(merge, str) or not re.fullmatch(r"[0-9a-f]{7,40}", merge):
            raise FleetError("a done report needs a merge sha of 7 to 40 hex characters")
        if check_main:
            reachable = subprocess.run(
                ["git", "merge-base", "--is-ancestor", merge, "main"], capture_output=True
            )
            if reachable.returncode != 0:
                raise FleetError(f"merge {merge} is not in local main")
    new_tasks = data.get("new_tasks", [])
    if not isinstance(new_tasks, list) or not all(
        isinstance(entry, dict) and isinstance(entry.get("title"), str) for entry in new_tasks
    ):
        raise FleetError("new_tasks must be a list of objects that each have a title")
    blockers = data.get("blockers", [])
    if not isinstance(blockers, list) or not all(isinstance(entry, str) for entry in blockers):
        raise FleetError("blockers must be a list of strings")
    return {
        "task": task,
        "status": status,
        "merge": merge,
        "validation": str(data.get("validation", "")),
        "new_tasks": new_tasks,
        "blockers": blockers,
    }


def now() -> int:
    return int(time.time())


def worker_names(count: int, backend: str = "claude") -> list[str]:
    family = "codex" if backend == "codex" else "claude"
    return [f"{family}-w{number}" for number in range(1, count + 1)]


def make_backend(args: argparse.Namespace, state: dict) -> ClaudeBackend | CodexBackend:
    backend = getattr(args, "backend", None) or state.get("backend", "claude")
    if backend == "codex":
        return CodexBackend(args.codex_bin)
    token = Path(state["token_file"]) if state.get("token_file") else None
    return ClaudeBackend(args.claude_bin, token)


def owner_claims(owner: str) -> list[dict]:
    paths = (coord.coord_dir() / "claims").glob("T[0-9]*.json")
    return [held for held in (coord.read_claim(path) for path in paths) if held["owner"] == owner]


def find_worktree(task: str) -> str | None:
    """Path of the worktree on task/<id> or task/<id>-..., relative to the main checkout."""
    listing = subprocess.run(
        ["git", "worktree", "list", "--porcelain"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    pattern = re.compile(rf"branch refs/heads/task/{re.escape(task.lower())}(-.*)?")
    main_checkout = ""
    current = ""
    for line in listing:
        if line.startswith("worktree "):
            current = line.removeprefix("worktree ")
            main_checkout = main_checkout or current
        elif pattern.fullmatch(line):
            return os.path.relpath(current, main_checkout)
    return None


def assign_text(task: str, slot: int, note: str, resume: str | None) -> str:
    text = f"ASSIGN {task} slot={slot} note={json.dumps(note)}"
    return f"{text} resume={resume}" if resume else text


def send_assignment(state: dict, worker: str, task: str, slot: int, note: str) -> None:
    text = assign_text(task, slot, note, find_worktree(task))
    coord_cli("say", "--owner", state["supervisor"], "--to", worker, text)


def restart_worker(
    backend: ClaudeBackend | CodexBackend, state: dict, name: str, reason: str, stale: int
) -> dict:
    worker = state["workers"][name]
    if worker.get("session"):
        if isinstance(backend, CodexBackend):
            backend.stop(worker["session"], worker.get("pid"), worker.get("pid_start"))
        else:
            backend.stop(worker["session"])
    coord_cli("reap", stale=stale)
    resumed: list[str] = []
    parked: list[str] = []
    for task, info in list(worker["assigned"].items()):
        if info["attempts"] >= MAX_ATTEMPTS:
            coord_cli("release", task, "--owner", name)
            del worker["assigned"][task]
            parked.append(task)
        else:
            info["attempts"] += 1
            resumed.append(task)
    worker["generation"] += 1
    worker["spawned"] = now()
    lost: list[str] = []
    for task in resumed:
        info = worker["assigned"][task]
        coord_cli("release", task, "--owner", name)
        note = f"resume after {reason}"
        slot = str(info["slot"])
        claimed = coord_cli(
            "claim", task, "--owner", name, "--slot", slot, "--note", note, stale=stale
        )
        if claimed.returncode != 0:
            del worker["assigned"][task]
            lost.append(task)
            continue
        if isinstance(backend, CodexBackend):
            resume_path = find_worktree(task)
            brief = build_codex_assignment(
                name, state["supervisor"], task, info["slot"], note, resume_path
            )
            worker["session"] = backend.spawn(
                name, brief, worker.get("session"), generation=worker["generation"]
            )
            worker["pid"] = backend.pids[worker["session"]]
            worker["pid_start"] = backend.pid_starts[worker["session"]]
            worker["diagnostics"] = backend.diagnostics[worker["session"]]
        else:
            send_assignment(state, name, task, info["slot"], note)
    if isinstance(backend, ClaudeBackend):
        worker["session"] = backend.spawn(name, build_brief(name, state["supervisor"]))
    resumed = [task for task in resumed if task not in lost]
    if isinstance(backend, CodexBackend) and not resumed:
        worker["session"] = None
        worker["pid"] = None
    return {
        "worker": name,
        "reason": reason,
        "generation": worker["generation"],
        "resumed": resumed,
        "parked": parked,
        "lost": lost,
    }


def cmd_start(args: argparse.Namespace) -> int:
    state = load_state()
    selected = args.backend or state.get("backend", "claude")
    if selected == "codex" and args.token_file:
        raise FleetError("--token-file applies only to the Claude backend")
    if state["workers"] and state.get("backend", "claude") != selected:
        raise FleetError("cannot change backend while workers are in fleet state")
    if selected == "codex" and not state["workers"]:
        state["codex_runtime"] = codex_runtime_identity()
    state["backend"] = selected
    if args.token_file:
        token = Path(args.token_file).expanduser()
        if not token.is_file():
            raise FleetError(f"token file not found: {token}")
        state["token_file"] = str(token.resolve())
    backend = make_backend(args, state)
    spawned: list[str] = []
    for name in worker_names(args.workers, selected):
        if name in state["workers"]:
            continue
        session = (
            backend.spawn(name, build_brief(name, state["supervisor"]))
            if isinstance(backend, ClaudeBackend)
            else None
        )
        state["workers"][name] = {
            "session": session,
            "pid": backend.pids.get(session)
            if isinstance(backend, CodexBackend) and session
            else None,
            "backend": selected,
            "generation": 1,
            "spawned": now(),
            "assigned": {},
        }
        spawned.append(name)
        save_state(state)
    save_state(state)
    print(json.dumps({"spawned": spawned}))
    return 0


def cmd_assign(args: argparse.Namespace) -> int:
    state = load_state()
    worker = state["workers"].get(args.worker)
    if worker is None:
        raise FleetError(f"unknown worker {args.worker}")
    if not 1 <= args.slot <= coord.SLOT_COUNT:
        raise FleetError(f"slot must be 1 to {coord.SLOT_COUNT}")
    if args.slot in {info["slot"] for info in worker["assigned"].values()} or (
        worker.get("backend") == "codex" and worker["assigned"]
    ):
        raise FleetError(f"{args.worker} already uses slot {args.slot}")
    claimed = coord_cli(
        "claim", args.task, "--owner", args.worker, "--slot", str(args.slot), "--note", args.note
    )
    if claimed.returncode != 0:
        raise FleetError(claimed.stderr.strip() or f"could not claim {args.task}")
    worker["assigned"][args.task] = {"slot": args.slot, "attempts": 1}
    backend = make_backend(args, state)
    try:
        if isinstance(backend, CodexBackend):
            brief = build_codex_assignment(
                args.worker,
                state["supervisor"],
                args.task,
                args.slot,
                args.note,
                find_worktree(args.task),
            )
            worker["session"] = backend.spawn(args.worker, brief, generation=worker["generation"])
            worker["pid"] = backend.pids[worker["session"]]
            worker["pid_start"] = backend.pid_starts[worker["session"]]
            worker["diagnostics"] = backend.diagnostics[worker["session"]]
        else:
            send_assignment(state, args.worker, args.task, args.slot, args.note)
    except Exception:
        coord_cli("release", args.task, "--owner", args.worker)
        del worker["assigned"][args.task]
        raise
    save_state(state)
    return 0


def cmd_complete(args: argparse.Namespace) -> int:
    state = load_state()
    for name, worker in state["workers"].items():
        if args.task in worker["assigned"]:
            coord_cli("release", args.task, "--owner", name)
            del worker["assigned"][args.task]
            save_state(state)
            return 0
    raise FleetError(f"{args.task} is not assigned to any worker")


def cmd_heal(args: argparse.Namespace) -> int:
    state = load_state()
    backend = make_backend(args, state)
    if isinstance(backend, CodexBackend) and not codex_runtime_visible(state):
        raise FleetError(
            "Codex PID namespace is unknown or differs from the fleet owner; refusing heal"
        )
    entries = backend.entries()
    if isinstance(backend, ClaudeBackend) and state["workers"] and not entries:
        # The supervisor's own session is always listed. An empty list means this runtime
        # cannot see the claude daemon (seen from the Codex environment), so every worker
        # would look missing and a heal would stop and respawn live ones.
        raise FleetError(
            "claude agents lists no sessions at all from here, so worker health is "
            "unknown. Refusing to heal from a runtime that cannot see the claude daemon."
        )
    sessions = background_sessions(entries)
    report = []
    for name, worker in state["workers"].items():
        reason = is_dead(
            worker,
            sessions.get(worker["session"]),
            owner_claims(name),
            now(),
            args.stale,
            args.grace,
        )
        if reason:
            report.append(restart_worker(backend, state, name, reason, args.stale))
            save_state(state)
    print(json.dumps(report, indent=2))
    return 0


def cmd_restart(args: argparse.Namespace) -> int:
    state = load_state()
    if args.worker not in state["workers"]:
        raise FleetError(f"unknown worker {args.worker}")
    backend = make_backend(args, state)
    result = restart_worker(backend, state, args.worker, "forced restart", args.stale)
    save_state(state)
    print(json.dumps(result, indent=2))
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    state = load_state()
    backend = make_backend(args, state)
    entries = backend.entries() if state["workers"] else []
    sessions = background_sessions(entries)
    rows = []
    for name, worker in sorted(state["workers"].items()):
        ages = {
            held["task"]: held["age"]
            for held in owner_claims(name)
            if held["task"] in worker["assigned"]
        }
        rows.append(
            {
                "worker": name,
                "session": worker["session"],
                "state": sessions.get(worker["session"], {}).get(
                    "state",
                    "idle"
                    if worker.get("backend") == "codex" and not worker["assigned"]
                    else "missing",
                ),
                "generation": worker["generation"],
                "assigned": worker["assigned"],
                "claim_ages": ages,
            }
        )
    last = state["last_tick"]
    age = now() - last if last else None
    out = {
        "supervisor": state["supervisor"],
        "last_tick_age": age,
        "supervisor_stale": age is not None and age > TICK_STALE_SECONDS,
        "blind": bool(state["workers"])
        and (
            not entries if isinstance(backend, ClaudeBackend) else not codex_runtime_visible(state)
        ),
        "workers": rows,
    }
    if args.json:
        print(json.dumps(out, indent=2))
        return 0
    when = "never" if age is None else f"{age}s ago"
    stale = f" STALE (expected every {TICK_SECONDS}s)" if out["supervisor_stale"] else ""
    print(f"supervisor {out['supervisor']}, last tick {when}{stale}")
    if out["blind"]:
        if isinstance(backend, ClaudeBackend):
            print("WARNING: claude lists no sessions from here, worker states below are unknown")
        else:
            print("WARNING: Codex PID namespace is unknown or differs; worker health is unknown")
    for row in rows:
        tasks = [f"{task}(slot {info['slot']})" for task, info in sorted(row["assigned"].items())]
        print(
            f"{row['worker']}  {row['session']}  {row['state']}  gen {row['generation']}  "
            + " ".join(tasks)
        )
    return 0


def cmd_tick(args: argparse.Namespace) -> int:
    state = load_state()
    state["last_tick"] = now()
    save_state(state)
    return 0


def cmd_stop(args: argparse.Namespace) -> int:
    state = load_state()
    backend = make_backend(args, state)
    for worker in state["workers"].values():
        if worker.get("session"):
            if isinstance(backend, CodexBackend):
                backend.stop(worker["session"], worker.get("pid"), worker.get("pid_start"))
            else:
                backend.stop(worker["session"])
    state["workers"] = {}
    save_state(state)
    return 0


def cmd_parse_report(args: argparse.Namespace) -> int:
    print(json.dumps(parse_report(args.text, args.check_main)))
    return 0


READ_ONLY = {"parse-report", "status"}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--claude-bin",
        default=os.environ.get("FLEET_CLAUDE_BIN", "claude"),
        help="claude executable (default $FLEET_CLAUDE_BIN or claude)",
    )
    parser.add_argument(
        "--codex-bin",
        default=os.environ.get("FLEET_CODEX_BIN", "codex"),
        help="codex executable (default $FLEET_CODEX_BIN or codex)",
    )
    parser.add_argument("--backend", choices=("claude", "codex"))
    parser.add_argument("--stale", type=int, default=coord.DEFAULT_STALE_SECONDS)
    parser.add_argument("--grace", type=int, default=GRACE_SECONDS)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("start", help="spawn workers that are not yet in fleet state")
    p.add_argument("--workers", type=int, default=2)
    p.add_argument(
        "--token-file",
        help="override: file holding a token. Default is the CLAUDE_CODE_OAUTH_TOKEN of this "
        "environment or of the claude session above this process",
    )
    p.set_defaults(func=cmd_start)
    p = sub.add_parser("status")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_status)
    p = sub.add_parser("assign", help="claim a task as a worker and send ASSIGN")
    p.add_argument("worker")
    p.add_argument("task")
    p.add_argument("--slot", type=int, required=True)
    p.add_argument("--note", default="")
    p.set_defaults(func=cmd_assign)
    p = sub.add_parser("complete", help="release a pushed task and forget it")
    p.add_argument("task")
    p.set_defaults(func=cmd_complete)
    sub.add_parser("heal", help="restart every dead worker").set_defaults(func=cmd_heal)
    p = sub.add_parser("restart", help="force-restart one worker")
    p.add_argument("worker")
    p.set_defaults(func=cmd_restart)
    p = sub.add_parser("parse-report", help="validate a REPORT message")
    p.add_argument("text")
    p.add_argument("--check-main", action="store_true", help="require the merge sha in local main")
    p.set_defaults(func=cmd_parse_report)
    sub.add_parser("tick", help="stamp the supervisor heartbeat").set_defaults(func=cmd_tick)
    sub.add_parser("stop", help="stop every worker session").set_defaults(func=cmd_stop)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        if args.command in READ_ONLY:
            return args.func(args)
        with state_lock():
            return args.func(args)
    except FleetError as error:
        print(f"fleet: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
