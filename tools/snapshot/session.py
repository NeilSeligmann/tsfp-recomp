# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Take and resume a whole-process snapshot of a `tools.play` run (T1153, docs/state-snapshot.md).

Take: one DMTCP computation holds Xvfb (via xvfb-run) and `python -m tools.play ...`, which runs the host.
The host asks for the checkpoint itself at the requested port 0 poll (src/host/host_snapshot.c), so the
point is a function of the guest, never of wall time. The plugin copies the run directory (the HDD files the
title created) while every process is quiesced. The launcher then writes the manifest that pins the exact
host binary and every mapped library, and ends the run.

Resume: verify the manifest (loud refusal on any mismatch), restore the run directory copy, start a fresh
coordinator and `dmtcp_restart` the images. The restored tools.play finishes the run and writes its stop
report into the run directory, which this module prints.
"""

import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

from tools.private_host import parse_stop
from tools.snapshot import dmtcp, manifest, options

ROOT = Path(__file__).resolve().parents[2]
SNAPSHOTS = Path("tmp/snapshots")
INNER_ENV = options.INNER_ENV
#: DMTCP's own checkpoint signal collides with the host's use of SIGUSR2, so a real-time signal is used.
CKPT_SIGNAL = "40"
INNER_LOG = "snapshot-inner.log"
TAKE_TIMEOUT = 900
SCREEN = "1280x1024x24"


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def free_display() -> int:
    """An X display number nobody holds, so the snapshot's Xvfb can be restored on the same number."""
    for number in range(200, 400):
        if not Path(f"/tmp/.X11-unix/X{number}").exists() and not manifest.x_display_in_use(number):
            return number
    raise SystemExit("snapshot: no free X display number in 200..399")


def coordinator_env(port: int) -> dict[str, str]:
    return {**os.environ, "DMTCP_COORD_PORT": str(port), "DMTCP_SIGCKPT": CKPT_SIGNAL}


def start_coordinator(tools: dict[str, Path], port: int, ckpt_dir: Path, log: Path) -> None:
    ckpt_dir.mkdir(parents=True, exist_ok=True)
    done = subprocess.run(
        [
            str(tools["dmtcp_coordinator"]),
            "--daemon",
            "--port",
            str(port),
            "--ckptdir",
            str(ckpt_dir),
            "--coord-logfile",
            str(log),
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=60,
        check=False,
    )
    if done.returncode != 0:
        raise SystemExit(f"snapshot: dmtcp_coordinator failed to start on port {port}")


def coordinator_alive(tools: dict[str, Path], port: int) -> bool:
    done = subprocess.run(
        [str(tools["dmtcp_command"]), "--port", str(port), "--status"],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    return "NUM_PEERS" in done.stdout


def quit_computation(tools: dict[str, Path], port: int) -> None:
    """Ask the coordinator to kill every process of the computation and exit, until it is gone."""
    for _ in range(6):
        subprocess.run(
            [str(tools["dmtcp_command"]), "--port", str(port), "--quit"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=60,
            check=False,
        )
        time.sleep(0.5)
        if not coordinator_alive(tools, port):
            return
    print(f"snapshot: WARNING coordinator on port {port} did not exit", file=sys.stderr)


def private_runtime() -> Path:
    runtime = ROOT / "tmp/xdg-snapshot"
    runtime.mkdir(parents=True, exist_ok=True)
    runtime.chmod(0o700)
    return runtime


def repository_head() -> str:
    probe = subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    return probe.stdout.strip()


def inner_command(
    tools: dict[str, Path],
    port: int,
    ckpt_dir: Path,
    display: int,
    play_argv: list[str],
    gzip: bool,
) -> list[str]:
    return [
        str(tools["dmtcp_launch"]),
        "--join-coordinator",
        "--coord-port",
        str(port),
        "--ckptdir",
        str(ckpt_dir),
        "--gzip" if gzip else "--no-gzip",
        "--with-plugin",
        str(tools["plugin"]),
        "xvfb-run",
        "-n",
        str(display),
        "-s",
        f"-screen 0 {SCREEN} -nolisten tcp",
        sys.executable,
        "-m",
        "tools.play",
        *play_argv,
    ]


def wait_for_checkpoint(process: subprocess.Popen[bytes], ckpt_dir: Path, limit: float) -> bool:
    """True once the coordinator wrote the restart script (all images complete), False if the run ended."""
    script = ckpt_dir / "dmtcp_restart_script.sh"
    deadline = time.monotonic() + limit
    while time.monotonic() < deadline:
        if script.exists() and list(ckpt_dir.glob("ckpt_*.dmtcp")):
            time.sleep(0.5)
            return True
        if process.poll() is not None:
            return script.exists() and bool(list(ckpt_dir.glob("ckpt_*.dmtcp")))
        time.sleep(0.2)
    return False


def stop_process_group(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    for sig, wait in ((signal.SIGTERM, 10), (signal.SIGKILL, 10)):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            return
        try:
            process.wait(timeout=wait)
            return
        except subprocess.TimeoutExpired:
            continue


def take(
    play_argv: list[str],
    *,
    poll: int,
    host: Path,
    xbe: Path,
    record: Path | None,
    run_dir: Path,
    snapshot_dir: Path,
    keep_running: bool,
    gzip: bool,
) -> int:
    """Run `tools.play play_argv` under DMTCP and snapshot it at port 0 poll `poll`."""
    tools = dmtcp.locate()
    started = time.monotonic()
    snapshot_dir = snapshot_dir.resolve()
    run_dir = (ROOT / run_dir).resolve() if not run_dir.is_absolute() else run_dir
    shutil.rmtree(snapshot_dir, ignore_errors=True)
    snapshot_dir.mkdir(parents=True)
    ckpt_dir = snapshot_dir / "ckpt"
    run_dir.mkdir(parents=True, exist_ok=True)
    port, display = free_port(), free_display()
    start_coordinator(tools, port, ckpt_dir, snapshot_dir / "coordinator.log")
    env = {
        **coordinator_env(port),
        INNER_ENV: "1",
        "TSFP_SNAPSHOT_DIR": str(snapshot_dir),
        "TSFP_SNAPSHOT_RUN_DIR": str(run_dir),
        "XDG_RUNTIME_DIR": str(private_runtime()),
        "SDL_AUDIODRIVER": os.environ.get("SDL_AUDIODRIVER", "dummy"),
    }
    argv = inner_command(tools, port, ckpt_dir, display, play_argv, gzip)
    print(
        f"snapshot: taking at poll {poll} of port 0, host {host}, X display :{display}", flush=True
    )
    log = (run_dir / INNER_LOG).open("wb")
    process = subprocess.Popen(  # noqa: S603
        argv,
        cwd=ROOT,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        taken = wait_for_checkpoint(process, ckpt_dir, TAKE_TIMEOUT)
        run_seconds = time.monotonic() - started
        if taken and not keep_running:
            quit_computation(tools, port)
        if keep_running:
            process.wait(timeout=TAKE_TIMEOUT)
    except subprocess.TimeoutExpired:
        taken = False
    finally:
        quit_computation(tools, port)  # also ends the coordinator when the run ended by itself
        stop_process_group(process)
        log.close()
    if not taken:
        found = parse_stop(run_dir)
        print(
            f"snapshot: NOT taken: the run ended before port 0 reached poll {poll} "
            f"(stop: {found['stop']}). See {run_dir / INNER_LOG}",
            file=sys.stderr,
        )
        return 1
    seconds = {"take_total": run_seconds}
    record_manifest(
        play_argv=play_argv,
        poll=poll,
        host=host,
        xbe=xbe,
        record=record,
        run_dir=run_dir,
        display=display,
        snapshot_dir=snapshot_dir,
        gzip=gzip,
        seconds=seconds,
    )
    return 0


def record_manifest(
    *,
    play_argv: list[str],
    poll: int,
    host: Path,
    xbe: Path,
    record: Path | None,
    run_dir: Path,
    display: int,
    snapshot_dir: Path,
    gzip: bool,
    seconds: dict[str, float],
) -> None:
    import json

    data = manifest.build_manifest(
        poll=poll,
        play_argv=play_argv,
        host=host,
        xbe=xbe,
        record=record,
        run_dir=run_dir,
        display=display,
        snapshot_dir=snapshot_dir,
        gzip=gzip,
        seconds=seconds,
        repository_head=repository_head(),
    )
    (snapshot_dir / manifest.MANIFEST_NAME).write_text(json.dumps(data, indent=2) + "\n")
    images = data["images"]
    assert isinstance(images, dict)
    total = sum(int(entry["bytes"]) for entry in images.values())
    host_info = data["host"]
    assert isinstance(host_info, dict)
    print(
        f"snapshot: taken at poll {poll}: {len(images)} images, {total / 1e6:.0f} MB, "
        f"host sha256 {host_info['sha256']}, lift key {host_info['lift_key']}\n"
        f"snapshot: {snapshot_dir / manifest.MANIFEST_NAME}\n"
        f"snapshot: resume with: python -m tools.play --resume-snapshot {snapshot_dir}"
    )


def restore_run_dir(snapshot_dir: Path, run_dir: Path) -> None:
    """Put the HDD files of the snapshot instant back, replacing whatever the run directory holds."""
    shutil.rmtree(run_dir, ignore_errors=True)
    shutil.copytree(snapshot_dir / "run-dir", run_dir, symlinks=True)


def resume(
    snapshot_dir: Path,
    *,
    host_override: Path | None,
    timeout: int,
    fast_verify: bool,
) -> int:
    """Verify, restore the run directory and run the snapshot to its stop. 2 when refused."""
    snapshot_dir = snapshot_dir.resolve()
    data = manifest.load(snapshot_dir)
    problems = manifest.verify(data, snapshot_dir, host_override=host_override, fast=fast_verify)
    if problems:
        for problem in problems:
            print(f"snapshot: REFUSED: {problem}", file=sys.stderr)
        print(
            "snapshot: nothing was resumed. A snapshot is valid only for the exact host binary and "
            "libraries it was taken on, take a new one with --snapshot-at-poll.",
            file=sys.stderr,
        )
        return 2
    host = data["host"]
    assert isinstance(host, dict)
    print(
        f"snapshot: resuming poll {data['poll']}, host {host['path']}\n"
        f"snapshot: pinned host sha256 {host['sha256']}, lift key {host['lift_key']}",
        flush=True,
    )
    tools = dmtcp.locate()
    run_dir = Path(str(data["run_dir"]))
    restore_run_dir(snapshot_dir, run_dir)
    port = free_port()
    ckpt_dir = snapshot_dir / "ckpt"
    started = time.monotonic()
    start_coordinator(tools, port, ckpt_dir, snapshot_dir / f"coordinator-resume-{port}.log")
    images = [str(ckpt_dir / name) for name in sorted(data["images"])]  # type: ignore[arg-type]
    log = (snapshot_dir / f"resume-{port}.log").open("wb")
    process = subprocess.Popen(  # noqa: S603
        [
            str(tools["dmtcp_restart"]),
            "--join-coordinator",
            "--coord-port",
            str(port),
            "--ckptdir",
            str(ckpt_dir),
            *images,
        ],
        cwd=ROOT,
        env=coordinator_env(port),
        stdin=subprocess.DEVNULL,
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        print(f"snapshot: resumed run exceeded {timeout} s, killed", file=sys.stderr)
    finally:
        quit_computation(tools, port)
        stop_process_group(process)
        log.close()
    seconds = time.monotonic() - started
    found = parse_stop(run_dir)
    print(
        f"snapshot: resumed run STOP {found['stop']}\n    detail: {found['detail']}\n"
        f"    census: {found['census']}\n    last guest calls: {found['calls']}\n"
        f"    time: {seconds:.1f} s\n    run dir: {run_dir}"
    )
    return 0 if found["stop"] != "none" else 1
