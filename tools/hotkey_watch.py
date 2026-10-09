#!/usr/bin/env python3
# ruff: noqa: E501
"""T1632: small helpers for the owner scripts that watch the host hotkey directory (`--hotkey-dir`, T1627).

The host writes `DIR/hotkey.<n>` (one line `<n> LABEL poll=P ms=M`) through a temporary file and a rename, so a reader never sees
half a file. Files already in the directory when a tool starts are old news and are ignored.

    python -m tools.hotkey_watch follow --dir D [--host-pid N | --host-pid-file F] [--poll 0.2] [--prefix TEXT]
        one line per new hotkey ('hotkey #3 mark at poll 912'), exits 0 when the host pid is gone (or on SIGTERM).
    python -m tools.hotkey_watch dump-daemon --dir D --pid-file F --phase-file P [--label dump] [--signal USR1]
            [--phase-prefix pad] [--ack-file A] [--ack-timeout 10] [--poll 0.2]
        for every NEW hotkey labelled --label: write the phase name `<prefix><k>` (k from 1) into P, remove A, signal the pid in F,
        wait (bounded) for A to appear. EXACTLY what tmp/dump_now.fish and the typed-label loop of tmp/run_weaponslot_session.fish
        and tmp/run_inventory_session.fish do for a typed label, so a pad dump is indistinguishable from a typed one. Keeps going
        until the host pid dies or SIGTERM (exit 0).

    python -m tools.hotkey_watch read-line (--host-pid N | --host-pid-file F) [--prompt TEXT] [--poll 0.2]
        a `read -P` that gives up when the host is gone: prompt on stderr, the typed line on stdout (exit 0), end of input exit 1,
        host exited exit 3. Lets a fish typed-label loop end by itself when the pad STOP chord (or the window) ended the game.

Exit codes: 0 normal, 2 bad arguments (argparse, an unreadable pid file, a bad label or prefix).
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from tools.action_advance import HOTKEY_FILE_RE, LABEL_RE, stdin_reader

POLL_SECONDS = 0.2
ACK_TIMEOUT_SECONDS = 10.0  # the typed loop waits 20 x 0.5 s
PHASE_RE = re.compile(r"[a-z0-9_-]+")  # the typed-label charset of the fish loops
SIGNALS = {"USR1": signal.SIGUSR1, "USR2": signal.SIGUSR2}


@dataclass(frozen=True)
class Hotkey:
    number: int
    label: str
    poll: int | None
    ms: int | None
    path: Path

    def describe(self) -> str:
        at = f" at poll {self.poll}" if self.poll is not None else ""
        return f"hotkey #{self.number} {self.label or '?'}{at}"


def parse_hotkey(number: int, text: str, path: Path) -> Hotkey:
    """`<n> LABEL poll=P ms=M` -> Hotkey. A line that does not fit gives label '' and no numbers (still a hotkey)."""
    words = text.split()
    label = words[1] if len(words) > 1 else ""
    fields = {key: value for key, _, value in (word.partition("=") for word in words[2:])}

    def number_of(key: str) -> int | None:
        value = fields.get(key, "")
        return int(value) if value.isdecimal() else None

    return Hotkey(number, label, number_of("poll"), number_of("ms"), path)


def pid_alive(pid: int) -> bool:
    """True while `pid` runs. A zombie (ended, not yet reaped by its parent) counts as gone."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return True
    return stat.rpartition(")")[2].split()[:1] != ["Z"]


def read_pid(path: Path) -> int | None:
    try:
        text = path.read_text().strip()
    except OSError:
        return None
    return int(text) if text.isdecimal() and int(text) > 0 else None


class HotkeyWatcher:
    """New `hotkey.<n>` files of a directory. Everything present at construction is ignored."""

    def __init__(self, directory: Path) -> None:
        self.directory = directory
        self.seen: set[int] = set(self._numbers())

    def _numbers(self) -> list[int]:
        try:
            names = os.listdir(self.directory)
        except OSError:  # not created yet, or gone
            return []
        found = []
        for name in names:
            match = HOTKEY_FILE_RE.fullmatch(name)  # the host's `.tmp` files do not match
            if match:
                found.append(int(match.group(1)))
        return sorted(found)

    def poll(self) -> list[Hotkey]:
        """The hotkeys that appeared since the last call, in order. A file that vanishes or cannot be read yet is retried next time."""
        fresh = []
        for number in self._numbers():
            if number in self.seen:
                continue
            path = self.directory / f"hotkey.{number}"
            try:
                text = path.read_text()
            except OSError:
                continue
            self.seen.add(number)
            fresh.append(parse_hotkey(number, text, path))
        return fresh


class Stopper:
    """SIGTERM and SIGINT only set a flag, the loops end cleanly (exit 0)."""

    def __init__(self) -> None:
        self.stop = False

    def install(self) -> None:
        signal.signal(signal.SIGTERM, self._set)
        signal.signal(signal.SIGINT, self._set)

    def _set(self, _signum: int, _frame: object) -> None:
        self.stop = True


# --------------------------------------------------------------------------------------------- follow
def follow(
    watcher: HotkeyWatcher,
    host_alive: Callable[[], bool],
    say: Callable[[str], None],
    prefix: str = "",
    poll: float = POLL_SECONDS,
    stopper: Stopper | None = None,
    sleep: Callable[[float], None] = time.sleep,
) -> int:
    """Print every new hotkey until the host is gone. The last poll after the host died still reports what it wrote."""
    stopper = stopper or Stopper()
    count = 0
    while not stopper.stop:
        alive = host_alive()
        for hotkey in watcher.poll():
            count += 1
            say(f"{prefix}{hotkey.describe()}")
        if not alive:
            break
        sleep(poll)
    return count


# --------------------------------------------------------------------------------------------- dump daemon
def ack_state(path: Path | None) -> tuple[bool, int]:
    if path is None:
        return False, 0
    try:
        return True, path.stat().st_mtime_ns
    except OSError:
        return False, 0


def take_dump(
    name: str,
    pid: int,
    phase_file: Path,
    sig: signal.Signals,
    ack_file: Path | None,
    ack_timeout: float,
    host_alive: Callable[[], bool],
    stopper: Stopper,
    poll: float = 0.1,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> str:
    """What a typed label does: phase file, drop the old ack, signal, wait for the ack. Returns the line to print."""
    phase_file.write_text(name + "\n")
    if ack_file is not None:
        try:
            ack_file.unlink(missing_ok=True)
        except OSError:
            pass  # a stale ack stays: only a changed mtime then counts as the new ack
    existed, mtime = ack_state(
        ack_file
    )  # still set when the unlink was refused: then a changed mtime is the ack
    try:
        os.kill(pid, sig)
    except OSError as error:
        return f"could not signal the host for dump {name}: {error}"
    if ack_file is None:
        return f"dump {name} requested (no ack file to wait for)"
    end = clock() + ack_timeout
    while True:
        now_exists, now_mtime = ack_state(ack_file)
        if now_exists and (not existed or now_mtime != mtime):
            return f"dump taken: {name}"
        if stopper.stop or not host_alive() or clock() >= end:
            break
        sleep(poll)
    return f"no ack yet (game busy?) for dump {name}: check the phase {name} in the dump folder"


def dump_daemon(
    watcher: HotkeyWatcher,
    label: str,
    pid_file: Path,
    phase_file: Path,
    phase_prefix: str,
    sig: signal.Signals,
    ack_file: Path | None,
    ack_timeout: float,
    say: Callable[[str], None],
    poll: float = POLL_SECONDS,
    stopper: Stopper | None = None,
    sleep: Callable[[float], None] = time.sleep,
) -> int:
    """One dump per NEW hotkey with `label`. Returns the number of dumps requested. Ends when the host died or on SIGTERM."""
    stopper = stopper or Stopper()
    pid = read_pid(pid_file)
    if pid is None:
        raise ValueError(f"no pid in {pid_file}")
    dumps = 0

    def host_alive() -> bool:
        return pid_alive(pid)

    while not stopper.stop:
        alive = host_alive()
        for hotkey in watcher.poll():
            if hotkey.label != label:
                continue
            if stopper.stop or not host_alive():
                break
            dumps += 1
            say(f"pad hotkey #{hotkey.number} ({label}): {phase_prefix}{dumps}")
            say(
                take_dump(
                    f"{phase_prefix}{dumps}",
                    pid,
                    phase_file,
                    sig,
                    ack_file,
                    ack_timeout,
                    host_alive,
                    stopper,
                )
            )
        if not alive:
            break
        sleep(poll)
    return dumps


# --------------------------------------------------------------------------------------------- read-line
EXIT_LINE, EXIT_EOF, EXIT_HOST_GONE = 0, 1, 3


def read_line(
    host_alive: Callable[[], bool],
    poll: float = POLL_SECONDS,
    stopper: Stopper | None = None,
    readline: Callable[[float], str | None] = stdin_reader,
) -> tuple[int, str]:
    """One typed line, or give up: (EXIT_LINE, text), (EXIT_EOF, '') at end of input or SIGTERM, (EXIT_HOST_GONE, '')."""
    stopper = stopper or Stopper()
    while not stopper.stop:
        if not host_alive():
            return EXIT_HOST_GONE, ""
        try:
            line = readline(poll)
        except EOFError:
            return EXIT_EOF, ""
        if line is not None:
            return EXIT_LINE, line
    return EXIT_EOF, ""


# --------------------------------------------------------------------------------------------- the command line
def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.hotkey_watch",
        description="T1632: watch the host hotkey directory (follow it, or turn a hotkey into a live dump).",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    follow_parser = sub.add_parser("follow", help="print a line per new hotkey file")
    follow_parser.add_argument(
        "--dir", required=True, type=Path, help="the --hotkey-dir of the host"
    )
    group = follow_parser.add_mutually_exclusive_group()
    group.add_argument("--host-pid", type=int, help="exit when this pid is gone")
    group.add_argument(
        "--host-pid-file", type=Path, help="file holding the host pid, exit when it is gone"
    )
    follow_parser.add_argument(
        "--poll", type=float, default=POLL_SECONDS, help="seconds between looks"
    )
    follow_parser.add_argument(
        "--prefix", default="", help="text put before every line, e.g. '[pad] '"
    )

    daemon = sub.add_parser(
        "dump-daemon", help="a new hotkey with --label takes a live dump (SIGUSR1)"
    )
    daemon.add_argument("--dir", required=True, type=Path, help="the --hotkey-dir of the host")
    daemon.add_argument("--label", default="dump", help="the hotkey label that asks for a dump")
    daemon.add_argument(
        "--pid-file",
        required=True,
        type=Path,
        help="host pid file (guestdump.pid), the host that dies ends the daemon",
    )
    daemon.add_argument("--signal", default="USR1", choices=sorted(SIGNALS), dest="signal_name")
    daemon.add_argument(
        "--phase-file",
        required=True,
        type=Path,
        help="the file the dump label is written to (guestdump.phase)",
    )
    daemon.add_argument(
        "--phase-prefix", default="pad", help="dumps are named PREFIX1, PREFIX2, ..."
    )
    daemon.add_argument(
        "--ack-file",
        type=Path,
        help="the file the host writes when the dump is done (guestdump.ack)",
    )
    daemon.add_argument(
        "--ack-timeout", type=float, default=ACK_TIMEOUT_SECONDS, help="seconds to wait for the ack"
    )
    daemon.add_argument("--poll", type=float, default=POLL_SECONDS, help="seconds between looks")

    reader = sub.add_parser("read-line", help="read one typed line, give up when the host is gone")
    group = reader.add_mutually_exclusive_group(required=True)
    group.add_argument("--host-pid", type=int, help="give up when this pid is gone")
    group.add_argument("--host-pid-file", type=Path, help="file holding the host pid")
    reader.add_argument("--prompt", default="", help="text shown on stderr before waiting")
    reader.add_argument("--poll", type=float, default=POLL_SECONDS, help="seconds between looks")
    return parser


def say_flushed(text: str) -> None:
    print(text, flush=True)


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.poll <= 0:
        parser.error("--poll must be above 0")
    stopper = Stopper()
    if args.command == "read-line":
        pid = args.host_pid
        if pid is not None and pid <= 0:
            parser.error("--host-pid must be above 0")
        if args.host_pid_file is not None:
            pid = read_pid(args.host_pid_file)
            if pid is None:
                print(f"hotkey_watch: no pid in {args.host_pid_file}", file=sys.stderr)
                return 2
        stopper.install()
        print(args.prompt, end="", file=sys.stderr, flush=True)
        code, line = read_line(lambda: pid_alive(pid), args.poll, stopper)
        if code == EXIT_HOST_GONE:
            print("\n(the game has exited)", file=sys.stderr, flush=True)
        if code == EXIT_LINE:
            print(line, flush=True)
        return code
    if args.command == "follow":
        if args.host_pid is not None and args.host_pid <= 0:
            parser.error("--host-pid must be above 0")
        pid = args.host_pid
        if args.host_pid_file is not None:
            pid = read_pid(args.host_pid_file)
            if pid is None:
                print(f"hotkey_watch: no pid in {args.host_pid_file}", file=sys.stderr)
                return 2
        stopper.install()
        watcher = HotkeyWatcher(args.dir)
        follow(
            watcher,
            (lambda: pid_alive(pid)) if pid else (lambda: True),
            say_flushed,
            args.prefix,
            args.poll,
            stopper,
        )
        return 0
    if not LABEL_RE.fullmatch(args.label):
        parser.error(f"--label {args.label!r}: want [A-Za-z0-9_-]")
    if not PHASE_RE.fullmatch(args.phase_prefix):
        parser.error("--phase-prefix must be [a-z0-9_-]+ (the typed label charset)")
    if args.ack_timeout < 0:
        parser.error("--ack-timeout must be 0 or more")
    if read_pid(args.pid_file) is None:
        print(f"hotkey_watch: no pid in {args.pid_file}", file=sys.stderr)
        return 2
    stopper.install()
    watcher = HotkeyWatcher(args.dir)
    dump_daemon(
        watcher,
        args.label,
        args.pid_file,
        args.phase_file,
        args.phase_prefix,
        SIGNALS[args.signal_name],
        args.ack_file,
        args.ack_timeout,
        say_flushed,
        args.poll,
        stopper,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
