# SPDX-License-Identifier: GPL-3.0-or-later
"""Serialized two-mode synthetic validation, hard180-second total execution budget."""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

TOTAL_SECONDS = 180
CLEANUP_SECONDS = 5
MODES = ("raw", "effective")


def group_members(group: int) -> list[dict[str, object]]:
    members = []
    for path in Path("/proc").iterdir():
        if not path.name.isdecimal():
            continue
        try:
            pid = int(path.name)
            if os.getpgid(pid) == group:
                raw = (path / "stat").read_text()
                state = raw[raw.rindex(")") + 2 :].split()[0]
                members.append(dict(pid=pid, state=state))
        except (ProcessLookupError, FileNotFoundError, PermissionError):
            continue
    return members


def terminate_group(process: subprocess.Popen, row: dict[str, object]) -> None:
    row["cleanup_started"] = True
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=CLEANUP_SECONDS)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    # A child that exited first must not leave a live compiler/subject in its group.
    members = group_members(process.pid)
    if any(member["state"] not in ("Z", "X") for member in members):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    observation_started = time.monotonic()
    while True:
        members = group_members(process.pid)
        if not any(member["state"] not in ("Z", "X") for member in members):
            break
        if time.monotonic() - observation_started >= CLEANUP_SECONDS:
            break
        time.sleep(0.02)
    process.poll()
    row["postkill_observation_seconds"] = time.monotonic() - observation_started
    row["remaining_group_members"] = members
    row["all_children_stopped"] = not any(member["state"] not in ("Z", "X") for member in members)


def run_mode(output: Path, mode: str, deadline: float, row: dict, save: Callable[[], None]) -> bool:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        row.update(
            status="NOT-RUN", reason="total180-second deadline exhausted", all_children_stopped=True
        )
        save()
        return False
    command = [
        sys.executable,
        "-m",
        "tools.harness.t1525_controls",
        "--code-safety",
        mode,
        "--output",
        str(output / mode),
    ]
    row.update(command=command, status="RUNNING", timeout_seconds=remaining)
    save()
    with (
        (output / f"{mode}.stdout.txt").open("x") as stdout,
        (output / f"{mode}.stderr.txt").open("x") as stderr,
    ):
        # Do not lose ownership if SIGTERM arrives between child creation and
        # assigning its process handle. Pending delivery occurs inside the try.
        previous_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM})
        process = None
        try:
            process = subprocess.Popen(
                command,
                stdout=stdout,
                stderr=stderr,
                start_new_session=True,
                preexec_fn=lambda: signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask),
            )
            row["process_group"] = process.pid
            save()
            signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
            row["exit"] = process.wait(timeout=max(0.001, deadline - time.monotonic()))
            row["status"] = "COMPLETE" if row["exit"] == 0 else "FAILED"
        except subprocess.TimeoutExpired as error:
            row.update(status="TIMEOUT", error=repr(error))
            terminate_group(process, row)
            row["exit"] = process.returncode
        except BaseException as error:
            row.update(status="INTERRUPTED", error=repr(error))
            if process is not None:
                terminate_group(process, row)
                row["exit"] = process.returncode
            save()
            raise
        finally:
            if process is None:
                row["all_children_stopped"] = True
            elif "all_children_stopped" not in row:
                members = group_members(process.pid)
                if any(member["state"] not in ("Z", "X") for member in members):
                    terminate_group(process, row)
                else:
                    row.update(all_children_stopped=True, remaining_group_members=members)
            save()
            signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
    return row["status"] == "COMPLETE" and row["all_children_stopped"]


def install_signal_handlers() -> None:
    def interrupted(signum: int, frame: object) -> None:
        raise InterruptedError(f"T1525 supervisor interrupted by signal {signum}")

    signal.signal(signal.SIGTERM, interrupted)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    os.setpriority(os.PRIO_PROCESS, 0, 19)
    os.sched_setaffinity(0, set(range(24, 32)))
    assert os.getpriority(os.PRIO_PROCESS, 0) == 19
    assert os.sched_getaffinity(0) == set(range(24, 32))
    install_signal_handlers()
    started = time.monotonic()
    args.output.mkdir(parents=True, exist_ok=False)
    receipt = dict(
        kind="T1525-scoped-synthetic-batch",
        total_deadline_seconds=TOTAL_SECONDS,
        cleanup_grace_seconds=CLEANUP_SECONDS,
        nice=19,
        cpus=list(range(24, 32)),
        modes=[dict(mode=mode, status="NOT-RUN") for mode in MODES],
    )

    def save() -> None:
        receipt["elapsed_seconds"] = time.monotonic() - started
        pending = args.output / "batch.pending.json"
        pending.write_text(json.dumps(receipt, indent=2) + "\n")
        pending.replace(args.output / "batch.json")

    save()
    for row in receipt["modes"]:
        if not run_mode(args.output, row["mode"], started + TOTAL_SECONDS, row, save):
            receipt["status"] = "FAILED/UNPROVEN; no automatic retry"
            save()
            return 1
    receipt["status"] = "COMPLETE; synthetic engineering only, capability remains false"
    save()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
