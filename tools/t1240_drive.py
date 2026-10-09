#!/usr/bin/env python3
# ruff: noqa: E501
"""T1240 turn based Story driver: the T1075 replay handover with live keyboard keys and a freezable clock.

`start` runs a daemon that owns Xvfb and the private host (Story replay `tmp/recorded-input-story-mode`, `--replay-handover`,
`--pad-source keyboard`).  After the record the pad is the keyboard, driven with xdotool (no guest state is written).  The host is
frozen with SIGSTOP between commands so a screenshot and a guest dump are coherent, like T982's `tools.t982_drive`.

    python -m tools.t1240_drive start --out-dir tmp/t1240/d1 --initial-run 50 &
    python -m tools.t1240_drive cmd --out-dir tmp/t1240/d1 "keys I" "run 3" "keys" "shot a.png" "dump p1"
    python -m tools.t1240_drive cmd --out-dir tmp/t1240/d1 quit

Commands: `keys K1,K2` (hold exactly this set, empty releases all), `run S` (thaw S wall seconds then freeze), `tap K1,K2 S`,
`shot FILE` (game viewport PNG), `dump LABEL` (SIGUSR1 read-only guest dump into DIR/dump/guestdump.LABEL, ranges are DUMP_RANGES),
`status`, `quit`, `quiet 1|0` (T1741: `pose`/`turn`/`goto` read /proc memory instead of a SIGUSR1 dump, so they add no census marks),
`census LABEL` (write the phase name and mark: the file census.txt.LABEL holds the indirect calls since the previous mark).  Keys: I K J L left stick, T G F H right stick (T look up, G down, F left, H right), Z A, X B, A X, S Y, 1 2 triggers.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shlex
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

from tools.guest_mem_probe import read_snapshot, save_snapshot
from tools.t982_acceptance import ROOT, main_root, pick_display, send_close
from tools.t982_drive import send
from tools.t1075_drive.recdiff import load as load_record

VIEWPORT = "640x480+320+120"
DUMP_RANGES = "*0x7B0C48+0:0x1584"
# the T1075 host argument set (tools/t1075_drive/run.sh), host and XBE paths are relative to the repo root
# flag ORDER is part of the replay identity (the record refuses "the same flags in a different order")
HEAD_FLAGS = (
    "--xonline-offline --skip-intro --ac97-ready --headless-streams --headless-buffers --headless-listener "
    "--headless-second-vblank --native-shader-assembler --native-xmv --headless-movie-audio --couple-vblank-effects "
    "--check-vblank-quiescence --overlay-consume --vblank-owner-waits 1000 --vblank-worker-blanks 100 "
    "--thread-timeout 2147483647 --gpu-live --gpu-live-inferred"
).split()
MID_FLAGS = (
    "--interactive --present window --present-hold-ms 0 --audio-sink sdl --audio-mute --synthetic-pad"
).split()


def pose_from_record(record: bytes) -> dict:
    """Player record words (T1240 MEASURED by key turns): position x y z at +0xA8, unit facing vector at +0xC0 (x, y, z)."""
    px, py, pz = struct.unpack_from("<3f", record, 0xA8)
    fx, fy, fz = struct.unpack_from("<3f", record, 0xC0)
    return {
        "pos": [round(px, 2), round(py, 2), round(pz, 2)],
        "yaw_deg": round(math.degrees(math.atan2(fz, fx)), 1),
        "pitch_deg": round(math.degrees(math.asin(max(-1.0, min(1.0, fy)))), 1),
    }


def wrap_degrees(angle: float) -> float:
    """Angle in (-180, 180]."""
    wrapped = (angle + 180.0) % 360.0 - 180.0
    return 180.0 if wrapped == -180.0 else wrapped


def turn_pulse(error_deg: float) -> tuple[str, float]:
    """Right stick key and hold seconds for a yaw error (+ = turn H, - = turn F).  Measured: 0.25 s about 30 to 50 deg, 0.55 s about 115 deg."""
    key = "h" if error_deg > 0 else "f"
    return key, max(0.06, min(0.4, abs(error_deg) / 260.0))


def key_set(text: str) -> list[str]:
    """`I,T` -> ['I', 'T'] (xdotool key names are lowercase letters), empty -> []."""
    return [word.lower() for word in text.replace(" ", "").split(",") if word]


def key_diff(held: set[str], wanted: set[str]) -> tuple[list[str], list[str]]:
    """Keys to release and keys to press to go from `held` to `wanted` (sorted, deterministic)."""
    return sorted(held - wanted), sorted(wanted - held)


def host_argv(
    host: str, out: Path, disc: Path, replay: str, modules: str, extra: list[str] | None = None
) -> list[str]:
    return [
        host,
        "build/default.xbe",
        "--hdd",
        str(out / "hdd"),
        *HEAD_FLAGS,
        "--dump-overlay",
        str(out / "overlay"),
        *MID_FLAGS,
        "--route-event-log",
        str(out / "route.log"),
        "--route-log-mem",
        "0x79094C",
        "--replay-input",
        replay,
        "--gpu-replay",
        modules,
        "--gpu-replay-lenient",
        "--gpu-live-translate",
        "--census-icalls",
        "--census-phases",
        str(out / "census.txt"),
        "--disc",
        str(disc),
        "--pad-source",
        "keyboard",
        "--replay-handover",
        "--dump-guest-range",
        DUMP_RANGES,
        "--dump-guest-dir",
        str(out / "dump"),
        *(extra or []),
    ]


class Driver:
    def __init__(
        self,
        out: Path,
        host: str,
        disc: Path,
        replay: str,
        modules: str,
        extra: list[str] | None = None,
    ) -> None:
        self.out = out
        for sub in ("hdd", "overlay", "dump"):
            (out / sub).mkdir(parents=True, exist_ok=True)
        self.display = f":{pick_display()}"
        self.xvfb = subprocess.Popen(
            ["Xvfb", self.display, "-screen", "0", "1280x720x24"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(2)
        runtime = ROOT / "tmp/xdg"
        runtime.mkdir(parents=True, exist_ok=True)
        runtime.chmod(0o700)
        self.env = dict(
            os.environ,
            DISPLAY=self.display,
            SDL_AUDIODRIVER="dummy",
            VK_ICD_FILENAMES="/usr/share/vulkan/icd.d/lvp_icd.json",
            XDG_RUNTIME_DIR=str(runtime),
        )
        self.log = (out / "run.log").open("wb")
        self.host = subprocess.Popen(
            host_argv(host, out, disc, replay, modules, extra),
            cwd=ROOT,
            env=self.env,
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )
        self.frozen = False
        self.held: set[str] = set()
        self.started = time.monotonic()
        self.commands = 0
        self.dumps = 0
        self.quiet = False

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

    def xdotool(self, *words: str) -> None:
        subprocess.run(
            ["xdotool", *words],
            env=self.env,
            timeout=30,
            check=False,
            stderr=subprocess.DEVNULL,
        )

    def set_keys(self, wanted: set[str]) -> None:
        release, press = key_diff(self.held, wanted)
        if press:
            found = subprocess.run(
                ["xdotool", "search", "--onlyvisible", "--name", "."],
                env=self.env,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            ).stdout.split()
            if found:
                self.xdotool("windowfocus", found[-1])
        for key in release:
            self.xdotool("keyup", key)
        for key in press:
            self.xdotool("keydown", key)
        self.held = set(wanted)

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

    def dump(self, label: str) -> str:
        (self.out / "dump" / "guestdump.phase").write_text(label + "\n")
        self.thaw()
        os.kill(self.host.pid, signal.SIGUSR1)
        time.sleep(1.0)
        self.freeze()
        return str(self.out / "dump" / f"guestdump.{label}")

    def census_mark(self, label: str) -> str:
        """T1741: close the census phase `label` (counts since the previous mark) with a SIGUSR1 mark and wait for the ack."""
        census = self.out / "census.txt"
        ack = Path(str(census) + ".ack")
        before = ack.stat().st_mtime_ns if ack.exists() else 0
        Path(str(census) + ".phase").write_text(label + "\n")
        self.thaw()
        os.kill(self.host.pid, signal.SIGUSR1)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if ack.exists() and ack.stat().st_mtime_ns != before:
                break
            time.sleep(0.1)
        self.freeze()
        return str(Path(str(census) + f".{label}"))

    def quiet_record(self) -> bytes:
        """The player record read with /proc memory reads: no SIGUSR1, so no census mark (T1741)."""
        snapshot = read_snapshot(self.host.pid)

        def read(address: int, size: int) -> bytes:
            for start, data in snapshot.items():
                if start <= address and address + size <= start + len(data):
                    return data[address - start : address - start + size]
            raise KeyError(hex(address))

        pointer = struct.unpack("<I", read(0x7B0C48, 4))[0]
        return read(pointer, 0x1584)

    def pose(self) -> dict:
        if self.quiet:
            return pose_from_record(self.quiet_record())
        self.dump("pose")
        return pose_from_record(load_record(str(self.out / "dump/guestdump.pose")))

    def turn_to(self, target_yaw: float, tolerance: float = 6.0, tries: int = 14) -> str:
        """Closed loop on the measured yaw (read only dumps) with short right stick pulses."""
        pose = self.pose()
        for _ in range(tries):
            error = wrap_degrees(target_yaw - pose["yaw_deg"])
            if abs(error) <= tolerance:
                break
            key, seconds = turn_pulse(error)
            self.set_keys({key})
            self.run_for(seconds)
            self.set_keys(set())
            self.run_for(0.15)
            pose = self.pose()
        return json.dumps(pose)

    def goto(self, x: float, z: float, seconds: float) -> str:
        """Turn to the bearing of (x, z) from the current position, then walk forward `seconds`."""
        pose = self.pose()
        bearing = math.degrees(math.atan2(z - pose["pos"][2], x - pose["pos"][0]))
        self.turn_to(bearing)
        self.set_keys({"i"})
        self.run_for(seconds)
        self.set_keys(set())
        return json.dumps(self.pose())

    def handle(self, line: str) -> str:
        words = shlex.split(line)
        if not words:
            return "empty"
        name, rest = words[0], words[1:]
        self.commands += 1
        if self.host.poll() is not None and name != "status":
            return f"host exited with status {self.host.poll()}"
        if name == "keys":
            self.set_keys(set(key_set(rest[0] if rest else "")))
            return "keys " + ",".join(sorted(self.held))
        if name == "run":
            self.run_for(float(rest[0]))
            return f"ran {rest[0]} s"
        if name == "tap":
            self.set_keys(set(key_set(rest[0])))
            self.run_for(float(rest[1]))
            self.set_keys(set())
            return f"tapped {rest[0]} {rest[1]} s"
        if name == "shot":
            return self.shot(rest[0])
        if name == "dump":
            return self.dump(rest[0])
        if name == "mem":
            save_snapshot(read_snapshot(self.host.pid), self.out / rest[0])
            return str(self.out / rest[0])
        if name == "turn":
            return self.turn_to(float(rest[0]))
        if name == "goto":
            return self.goto(float(rest[0]), float(rest[1]), float(rest[2]))
        if name == "quiet":
            self.quiet = rest[0] == "1"
            return f"quiet {self.quiet} (pose reads /proc memory, no census mark)"
        if name == "census":
            return self.census_mark(rest[0])
        if name == "pose":
            return json.dumps(self.pose())
        if name == "status":
            return json.dumps(
                {
                    "host_exit": self.host.poll(),
                    "frozen": self.frozen,
                    "held": sorted(self.held),
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
        else ROOT / "tmp/TimeSplitters - Future Perfect (USA) (XBOX).iso"
    )
    driver = Driver(
        out,
        args.host,
        disc,
        args.replay,
        args.modules or str(main / "tmp/play/modules"),
        args.host_arg,
    )
    sock_path = out / "control.sock"
    sock_path.unlink(missing_ok=True)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(sock_path))
    server.listen(1)
    print(str(sock_path), flush=True)
    try:
        time.sleep(args.initial_run)
        driver.freeze()
        while True:
            connection, _ = server.accept()
            with connection:
                data = connection.makefile("rb").readline().decode()
                try:
                    reply = driver.handle(data)
                except (ValueError, IndexError, OSError) as error:
                    reply = f"error {type(error).__name__}: {error}"
                connection.sendall((reply + "\n").encode())
            if data.strip().startswith("quit"):
                break
    finally:
        driver.close()
        sock_path.unlink(missing_ok=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="mode", required=True)
    start = sub.add_parser("start", help="start Xvfb and the host and serve commands")
    start.add_argument("--out-dir", required=True)
    start.add_argument("--host", default="tmp/private-host/build-b7de1f2885bb336e/tsfp_host")
    start.add_argument(
        "--host-arg",
        action="append",
        default=[],
        help="T1741: one extra host argument appended last (repeatable), e.g. --host-arg=--watch-write --host-arg='**0x7A3580+0+0x1CC:4'",
    )
    start.add_argument("--disc", help="XBOX ISO (default: tmp of the repo root)")
    start.add_argument("--replay", default="tmp/recorded-input-story-mode")
    start.add_argument(
        "--modules", help="gpu replay modules dir (default: main checkout tmp/play/modules)"
    )
    start.add_argument(
        "--initial-run",
        type=float,
        default=50.0,
        help="wall seconds before the first freeze (the replay ends and hands the pad over at about 40 s)",
    )
    cmd = sub.add_parser("cmd", help="send commands to a running daemon, one connection each")
    cmd.add_argument("--out-dir", required=True)
    cmd.add_argument("--timeout", type=float, default=300.0)
    cmd.add_argument("--file", help="recipe file with one command per line")
    cmd.add_argument("commands", nargs="*")
    args = parser.parse_args()
    return serve(args) if args.mode == "start" else send(args)


if __name__ == "__main__":
    sys.exit(main())
