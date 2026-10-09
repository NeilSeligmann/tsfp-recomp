#!/usr/bin/env python3
# ruff: noqa: E501
"""T1632: the terminal side of recording a route (replaces the blocking `read` loop of tmp/record_mapmaker_route.fish).

    python -m tools.record_console --pid-file ROUTE.pid --stages 'profile finished,map loaded,editor up,preview running'
            [--hotkey-dir DIR] [--result-file F] [--poll 0.2]

The host writes `<record>.pid`. For stage K of N the console tells the owner to press Enter HERE or hold the pad MARK chord when
the stage is reached, and `s` + Enter or the pad STOP chord to stop the recording.
  * Enter in the terminal: the console sends SIGUSR2 to the host (the host places `# mark: at=N`).
  * a NEW hotkey file labelled `mark` (src/host/hotkey_actions.c): the HOST already placed the mark, the console only counts it
    and moves on. It never sends a second SIGUSR2.
  * `s` + Enter: SIGTERM to the host (the SDL quit handler ends it cleanly, the recording gets its `# polls:` trailer).
  * a NEW hotkey file labelled `stop`: the host ends itself, the console notes it.
The console exits 0 when the host pid is gone with 'marks sent N of M, host exited'. End of input on stdin does not stop the host,
the console keeps watching the hotkey directory. Hotkey files present at the start are old news. Without --hotkey-dir only the
terminal works (an old host without --hotkey).

--result-file F gets key=value lines for the calling script: marks, total, stop (none|hotkey|typed|window), terminal, pad.
"""

from __future__ import annotations

import argparse
import os
import signal
import sys
import time
from collections.abc import Callable
from pathlib import Path

from tools import hotkey_defaults
from tools.action_advance import POLL_SECONDS, stdin_reader
from tools.hotkey_watch import HotkeyWatcher, Stopper, pid_alive, read_pid

PID_WAIT_SECONDS = 10.0
STOP_WORDS = ("s", "stop")


class RecordConsole:
    """The state machine. `readline(timeout)` returns a typed line, None on a timeout, raises EOFError at end of input.
    `alive`, `send` (pid, signal) and `sleep` are test seams."""

    def __init__(
        self,
        pid: int,
        stages: list[str],
        watcher: HotkeyWatcher | None,
        say: Callable[[str], None] = print,
        readline: Callable[[float], str | None] = stdin_reader,
        alive: Callable[[int], bool] = pid_alive,
        send: Callable[[int, int], None] = os.kill,
        sleep: Callable[[float], None] = time.sleep,
        poll: float = POLL_SECONDS,
        mark_text: str = "",
        stop_text: str = "",
        stopper: Stopper | None = None,
    ) -> None:
        self.pid = pid
        self.stages = stages
        self.watcher = watcher
        self.say = say
        self.readline = readline
        self.alive = alive
        self.send = send
        self.sleep = sleep
        self.poll = poll
        self.mark_text = mark_text
        self.stop_text = stop_text
        self.stopper = stopper or Stopper()
        self.terminal_marks = 0
        self.pad_marks = 0
        self.stop_by = "none"
        self.stdin_open = True

    @property
    def marks(self) -> int:
        return self.terminal_marks + self.pad_marks

    def _how(self, text: str, typed: str) -> str:
        return f"{typed}, or {text}" if text else typed

    def prompt(self) -> None:
        total = len(self.stages)
        if self.marks < total:
            self.say(f"\n== mark {self.marks + 1} of {total}: {self.stages[self.marks]}")
            self.say("   When it is reached: " + self._how(self.mark_text, "press Enter here"))
        else:
            self.say(
                f"\n== all {total} marks are placed. Now stop the recording (the game must end normally)."
            )
        self.say(
            "   Stop the recording: "
            + self._how(self.stop_text, "type s and Enter")
            + ", or close the game window."
        )

    def on_hotkey(self, label: str, number: int) -> bool:
        """True when the prompt must be shown again."""
        if label == "mark":
            self.pad_marks += 1
            if self.marks > len(self.stages):
                self.say(
                    f"  WARNING: pad mark #{number} is beyond the {len(self.stages)} stages, the recording has an extra mark"
                )
                return False
            self.say(
                f"  mark {self.marks} placed by the host (pad hotkey #{number}), nothing sent from here"
            )
            return True
        if label == "stop":
            if self.stop_by == "none":
                self.stop_by = "hotkey"
            self.say(
                f"  stop requested by the pad (hotkey #{number}): the host ends itself, waiting for it to finish"
            )
        return False

    def on_line(self, line: str) -> bool:
        """True when the prompt must be shown again."""
        word = line.strip().lower()
        if word in STOP_WORDS:
            if self.stop_by == "none":
                self.stop_by = "typed"
            self.say("  stopping the host (SIGTERM, a clean exit writes the recording trailer) ...")
            self._signal(signal.SIGTERM)
            return False
        if word:
            self.say(f"  {line.strip()!r} is not understood: Enter = mark, s = stop the recording")
            return False
        if self.marks >= len(self.stages):
            self.say(
                f"  all {len(self.stages)} marks are placed already, nothing sent. Type s to stop, or close the game window."
            )
            return False
        self._signal(signal.SIGUSR2)
        self.terminal_marks += 1
        self.say(f"  mark {self.marks} sent")
        return True

    def _signal(self, sig: int) -> None:
        try:
            self.send(self.pid, sig)
        except OSError as error:
            self.say(f"  could not signal the host: {error}")

    def drain_hotkeys(self) -> bool:
        again = False
        if self.watcher is None:
            return again
        for hotkey in self.watcher.poll():
            again = self.on_hotkey(hotkey.label, hotkey.number) or again
        return again

    def wait_tick(self) -> str | None:
        """One poll interval: a typed line, or None. End of input stops the reading, not the console."""
        if not self.stdin_open:
            self.sleep(self.poll)
            return None
        try:
            return self.readline(self.poll)
        except EOFError:
            self.stdin_open = False
            self.say(
                "  (end of input on the terminal: still watching the hotkey directory and the host)"
            )
            return None

    def run(self) -> int:
        self.prompt()
        while not self.stopper.stop and self.alive(self.pid):
            again = self.drain_hotkeys()
            line = self.wait_tick()
            if line is not None:
                again = self.on_line(line) or again
            if again:
                self.prompt()
        if self.stopper.stop:
            self.say("\nconsole interrupted, the host keeps running")
            return 0
        self.drain_hotkeys()  # what the host wrote just before it ended
        if self.stop_by == "none":
            self.stop_by = "window"
        self.say(
            f"\nmarks sent {self.marks} of {len(self.stages)}, host exited (stop: {self.stop_by})"
        )
        return 0

    def result_lines(self) -> list[str]:
        return [
            f"marks={self.marks}",
            f"total={len(self.stages)}",
            f"stop={self.stop_by}",
            f"terminal={self.terminal_marks}",
            f"pad={self.pad_marks}",
        ]


def wait_for_pid(
    path: Path, seconds: float, sleep: Callable[[float], None] = time.sleep
) -> int | None:
    end = time.monotonic() + seconds
    while True:
        pid = read_pid(path)
        if pid is not None or time.monotonic() >= end:
            return pid
        sleep(0.2)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.record_console",
        description="T1632: place the marks of a route recording from the terminal or the pad, and stop it.",
    )
    parser.add_argument(
        "--pid-file", required=True, type=Path, help="the host pid file (<record>.pid)"
    )
    parser.add_argument(
        "--stages", required=True, help="comma separated stage names, one mark each"
    )
    parser.add_argument(
        "--hotkey-dir", type=Path, help="the host --hotkey-dir (omit: terminal only)"
    )
    parser.add_argument(
        "--result-file", type=Path, help="write key=value lines here when the host has exited"
    )
    parser.add_argument("--poll", type=float, default=POLL_SECONDS, help="seconds between looks")
    parser.add_argument(
        "--pid-wait", type=float, default=PID_WAIT_SECONDS, help="seconds to wait for the pid file"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    stages = [stage.strip() for stage in args.stages.split(",") if stage.strip()]
    if not stages:
        parser.error("--stages names no stage")
    if args.poll <= 0:
        parser.error("--poll must be above 0")
    if args.pid_wait < 0:
        parser.error("--pid-wait must be 0 or more")
    pid = wait_for_pid(args.pid_file, args.pid_wait)
    if pid is None:
        print(f"record_console: no host pid in {args.pid_file}", file=sys.stderr)
        return 2
    watcher = HotkeyWatcher(args.hotkey_dir) if args.hotkey_dir else None
    mark_text = stop_text = ""
    if watcher is not None:
        mark_text = f"{hotkey_defaults.chord_text('mark', 'pad')}, or {hotkey_defaults.chord_text('mark', 'kb')}"
        stop_text = f"{hotkey_defaults.chord_text('stop', 'pad')}, or {hotkey_defaults.chord_text('stop', 'kb')}"
    stopper = Stopper()
    stopper.install()
    console = RecordConsole(
        pid,
        stages,
        watcher,
        say=lambda text: print(text, flush=True),
        poll=args.poll,
        mark_text=mark_text,
        stop_text=stop_text,
        stopper=stopper,
    )
    code = console.run()
    if args.result_file is not None and not stopper.stop:
        args.result_file.write_text("\n".join(console.result_lines()) + "\n")
    return code


if __name__ == "__main__":
    sys.exit(main())
