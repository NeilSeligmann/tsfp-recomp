#!/usr/bin/env python3
# ruff: noqa: E501
"""T1222 turn based driver: a private host run whose pad is live and whose clock can be frozen between commands.

`start` runs a daemon that owns Xvfb and the private host (`--pad-script-live`, FABRICATED input).  The host is frozen with
SIGSTOP between commands, so every screenshot and guest memory snapshot is coherent and the observer (a person or an agent)
can look, decide and press, like a turn based player.  `cmd` sends a command line to the daemon.

    python -m tools.t982_drive start --out-dir tmp/t982/drive1 &        # prints the control socket path
    python -m tools.t982_drive cmd --out-dir tmp/t982/drive1 "pad LY=32767 RT=255" "run 1.5" "shot a.png" "mem m1.bin"
    python -m tools.t982_drive cmd --out-dir tmp/t982/drive1 quit

Commands: `pad TOKENS` (script token syntax, empty = rest), `run SECONDS` (unfreeze that many wall seconds, then freeze),
`shot FILE` (game viewport PNG), `mem FILE` (read-only snapshot, see tools.guest_mem_probe), `tap TOKENS SECONDS` (press, run, release),
`status`, `quit` (WM_DELETE_WINDOW, the normal close).  Nothing writes guest memory.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

from tools.guest_mem_probe import read_snapshot, save_snapshot
from tools.t982_acceptance import ROOT, host_command, main_root, pick_display, send_close

PAD_BYTES = 256
VIEWPORT = "640x480+320+120"


def padded_line(tokens: str) -> bytes:
    """One fixed length line so the host never sees a half written state (a single pwrite)."""
    text = tokens.strip().encode()
    if len(text) >= PAD_BYTES - 1:
        raise ValueError("pad line too long")
    return text.ljust(PAD_BYTES - 1, b" ") + b"\n"


class Driver:
    def __init__(self, out: Path, disc: Path, tap_seconds: float, shot_first: bool) -> None:
        self.out = out
        out.mkdir(parents=True, exist_ok=True)
        for sub in ("hdd", "overlay"):
            (out / sub).mkdir(exist_ok=True)
        self.pad_path = out / "pad.live"
        self.pad_fd = os.open(self.pad_path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
        self.set_pad("")
        current = json.loads((ROOT / "tmp/private-host/current.json").read_text())
        main = main_root()
        self.display = f":{pick_display()}"
        self.xvfb = subprocess.Popen(
            ["Xvfb", self.display, "-screen", "0", "1280x720x24"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(2)
        env = dict(
            os.environ,
            DISPLAY=self.display,
            SDL_AUDIODRIVER="disk",
            SDL_AUDIO_DISK_OUTPUT_FILE=str(out / "audio.raw"),
            SDL_AUDIO_DISK_TIMESCALE="1.0",
        )
        command = host_command(
            ROOT / current["host"],
            ROOT / "build/default.xbe",
            out,
            disc,
            main / "tmp/play/modules",
            ["--pad-source", "script", "--pad-script-live", str(self.pad_path)],
        )
        self.log = (out / "run.log").open("wb")
        self.env = env
        self.host = subprocess.Popen(
            command, cwd=ROOT, env=env, stdout=self.log, stderr=subprocess.STDOUT
        )
        self.frozen = False
        self.started = time.monotonic()
        self.tap_seconds = tap_seconds
        self.commands = 0

    def set_pad(self, tokens: str) -> None:
        os.pwrite(self.pad_fd, padded_line(tokens), 0)

    def freeze(self) -> None:
        if not self.frozen and self.host.poll() is None:
            os.kill(self.host.pid, signal.SIGSTOP)
            self.frozen = True

    def thaw(self) -> None:
        if self.frozen and self.host.poll() is None:
            os.kill(self.host.pid, signal.SIGCONT)
            self.frozen = False

    def run_for(self, seconds: float) -> None:
        self.thaw()
        time.sleep(seconds)
        self.freeze()

    def shot(self, name: str) -> str:
        path = self.out / name
        subprocess.run(
            ["import", "-window", "root", "-crop", VIEWPORT, "+repage", str(path)],
            env=self.env,
            timeout=60,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return str(path)

    def mem(self, name: str) -> str:
        path = self.out / name
        save_snapshot(read_snapshot(self.host.pid), path)
        return str(path)

    def handle(self, line: str) -> str:
        words = shlex.split(line)
        if not words:
            return "empty"
        name, rest = words[0], words[1:]
        self.commands += 1
        if self.host.poll() is not None and name != "status":
            return f"host exited with status {self.host.poll()}"
        if name == "pad":
            self.set_pad(" ".join(rest))
            return "pad " + " ".join(rest)
        if name == "run":
            self.run_for(float(rest[0]))
            return f"ran {rest[0]} s"
        if name == "tap":
            self.set_pad(rest[0].replace(",", " "))
            self.run_for(float(rest[1]))
            self.set_pad("")
            return f"tapped {rest[0]} {rest[1]} s"
        if name == "shot":
            return self.shot(rest[0])
        if name == "mem":
            return self.mem(rest[0])
        if name == "status":
            return json.dumps(
                {
                    "host_exit": self.host.poll(),
                    "frozen": self.frozen,
                    "wall": round(time.monotonic() - self.started, 1),
                    "commands": self.commands,
                }
            )
        if name == "quit":
            self.thaw()
            note = send_close(self.display)
            try:
                status = self.host.wait(timeout=60)
            except subprocess.TimeoutExpired:
                self.host.kill()
                status = self.host.wait()
                note += ", killed after 60 s"
            return f"{note}, host exit {status}"
        return f"unknown command {name}"

    def close(self) -> None:
        if self.host.poll() is None:
            self.thaw()
            self.host.kill()
            self.host.wait()
        self.xvfb.send_signal(signal.SIGTERM)
        self.log.close()


def serve(args: argparse.Namespace) -> int:
    out = Path(args.out_dir)
    main = main_root()
    disc = (
        Path(args.disc)
        if args.disc
        else main / "tmp/TimeSplitters - Future Perfect (USA) (XBOX).iso"
    )
    driver = Driver(out, disc, args.tap_seconds, False)
    sock_path = out / "control.sock"
    sock_path.unlink(missing_ok=True)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(sock_path))
    server.listen(1)
    print(str(sock_path), flush=True)
    try:
        # start phase: tap A like the recorded menu input until the title is in game (the observer decides when via `pad`)
        if args.tap_seconds > 0:
            end = time.monotonic() + args.tap_seconds
            while time.monotonic() < end and driver.host.poll() is None:
                driver.set_pad("A")
                time.sleep(0.15)
                driver.set_pad("")
                time.sleep(0.3)
        driver.freeze()
        while True:
            connection, _ = server.accept()
            with connection:
                data = connection.makefile("rb").readline().decode()
                reply = driver.handle(data)
                connection.sendall((reply + "\n").encode())
            if (
                data.strip().startswith("quit")
                or driver.host.poll() is not None
                and data.strip().startswith("quit")
            ):
                break
    finally:
        driver.close()
        sock_path.unlink(missing_ok=True)
    return 0


def recipe_lines(path: str) -> list[str]:
    """Command lines of a recipe file, one per line, blank lines and lines starting with # skipped."""
    lines = []
    for raw in Path(path).read_text().splitlines():
        text = raw.strip()
        if text and not text.startswith("#"):
            lines.append(text)
    return lines


def send(args: argparse.Namespace) -> int:
    sock_path = Path(args.out_dir) / "control.sock"
    for line in [*(recipe_lines(args.file) if args.file else []), *args.commands]:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(args.timeout)
            client.connect(str(sock_path))
            client.sendall((line + "\n").encode())
            print(f"> {line}\n{client.makefile('rb').readline().decode().strip()}", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="mode", required=True)
    start = sub.add_parser("start", help="start Xvfb and the host and serve commands")
    start.add_argument("--out-dir", required=True)
    start.add_argument("--disc", help="XBOX ISO (default: main checkout tmp)")
    start.add_argument(
        "--tap-seconds",
        type=float,
        default=75.0,
        help="wall seconds of A taps (menu and cutscene skipping) before the first freeze",
    )
    cmd = sub.add_parser("cmd", help="send commands to a running daemon, one connection each")
    cmd.add_argument("--out-dir", required=True)
    cmd.add_argument("--timeout", type=float, default=300.0)
    cmd.add_argument(
        "--file", help="recipe file with one command per line, run before the command arguments"
    )
    cmd.add_argument("commands", nargs="*")
    args = parser.parse_args()
    return serve(args) if args.mode == "start" else send(args)


if __name__ == "__main__":
    sys.exit(main())
