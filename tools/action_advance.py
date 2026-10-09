#!/usr/bin/env python3
# ruff: noqa: E501
"""T1627: the owner-facing parts of the guided census session that are not about profiles.

* `Advance`: ONE place that waits for the owner. Enter in the terminal and the host hotkey (the host writes
  `DIR/hotkey.<n>` when the owner holds the chord, src/input/xinput_hotkey.c) both advance. A phase can show a live
  elapsed time or countdown and stop by itself (idle phases).
* `parse_plan`: the ordered plan file of a session (scenarios, steps, questions), used by the multi-batch forced weapons
  run (tmp/run_forced_weapons.fish) and the in-preview experiment.
* `apply_poke`: serve a guarded poke request (`DIR/guestpoke.<label>`) or a plain dump on the RUNNING host with SIGUSR2
  (the same path as tmp/poke_now.fish, which is the manual twin of this). FABRICATED-STATE, the host refuses it without
  --forced-state.

Nothing here touches the game; a hotkey file is only read.
"""

from __future__ import annotations

import glob
import os
import re
import select
import signal
import sys
import time
from collections.abc import Callable
from pathlib import Path

HOTKEY_FILE_RE = re.compile(r"hotkey\.(\d+)")
POLL_SECONDS = 0.2
POKE_ACK_SECONDS = 30.0
STEP_KINDS = ("step", "observe")
LABEL_RE = re.compile(r"[A-Za-z0-9_-]{1,63}")


# --------------------------------------------------------------------------------------------- plan file
def parse_plan(text: str, known_scenarios: set[str]) -> list[dict]:
    """The items of a plan file, in order. One item per line, `#` comments and blank lines ignored:

    scenario ID[:label]                    a census phase (id from tools/data/action_scenarios.json)
    step NAME|ACTION|TEXT[|AFTER]          an instruction that is not recorded. ACTION is none, poke=LABEL (serve
                                           DIR/guestpoke.LABEL then dump) or dump=LABEL (dump only, no request).
                                           With an ACTION the tool waits for the owner (TEXT shown), runs it, prints the
                                           result, then waits again if AFTER (shown) is not empty.
    observe NAME|QUESTION                  ask the owner a question and record the typed answer in session.json
    """
    items: list[dict] = []
    seen: set[str] = set()
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        word, _, rest = line.partition(" ")
        rest = rest.strip()
        if word == "scenario":
            name, _, label = rest.partition(":")
            if name not in known_scenarios:
                raise ValueError(f"plan line {number}: unknown scenario id {name!r}")
            if ":" in rest and not re.fullmatch(r"[a-z0-9_]+", label):
                raise ValueError(f"plan line {number}: bad label {label!r} (use [a-z0-9_]+)")
            key = f"{name}@{label}" if label else name
            if key in seen:
                raise ValueError(f"plan line {number}: {key} is listed twice")
            seen.add(key)
            items.append({"item": "scenario", "id": name, "label": label or None})
        elif word == "step":
            fields = rest.split("|")
            if len(fields) not in (3, 4) or not re.fullmatch(
                r"[A-Za-z0-9_-]{1,40}", fields[0].strip()
            ):
                raise ValueError(f"plan line {number}: want `step NAME|ACTION|TEXT[|AFTER]`")
            action = fields[1].strip()
            kind, _, label = action.partition("=")
            if action != "none" and (
                kind not in ("poke", "dump", "pickup") or not LABEL_RE.fullmatch(label)
            ):
                raise ValueError(
                    f"plan line {number}: ACTION must be none, poke=LABEL, dump=LABEL or pickup=LABEL, got {action!r}"
                )
            if not fields[2].strip():
                raise ValueError(f"plan line {number}: a step needs TEXT")
            items.append(
                {
                    "item": "step",
                    "name": fields[0].strip(),
                    "action": None if action == "none" else kind,
                    "label": label or None,
                    "text": fields[2].strip(),
                    "after": fields[3].strip() if len(fields) == 4 else "",
                }
            )
        elif word == "observe":
            name, sep, question = rest.partition("|")
            if (
                not sep
                or not re.fullmatch(r"[A-Za-z0-9_-]{1,40}", name.strip())
                or not question.strip()
            ):
                raise ValueError(f"plan line {number}: want `observe NAME|QUESTION`")
            items.append({"item": "observe", "name": name.strip(), "text": question.strip()})
        else:
            raise ValueError(f"plan line {number}: unknown item {word!r} (scenario, step, observe)")
    if not items:
        raise ValueError("the plan is empty")
    return items


# --------------------------------------------------------------------------------------------- the waiting
def stdin_reader(timeout: float) -> str | None:
    """One line of stdin if it arrives within `timeout` seconds, None on a timeout, EOFError at end of input."""
    if not hasattr(select, "select"):  # pragma: no cover (no select on stdin on this platform)
        return input()
    ready, _, _ = select.select([sys.stdin], [], [], timeout)
    if not ready:
        return None
    line = sys.stdin.readline()
    if line == "":
        raise EOFError
    return line.rstrip("\r\n")


def format_seconds(seconds: float) -> str:
    return f"{seconds:5.1f} s"


class Advance:
    """Wait for the owner: typed Enter/answer in the terminal, or a host hotkey file. See the module doc.

    `ask` is the plain blocking prompt (input), used verbatim when neither hotkeys nor timers are on, so a session without
    the new options behaves exactly as before. `readline(timeout)` and `clock` are test seams."""

    def __init__(
        self,
        ask: Callable[[str], str] = input,
        say: Callable[[str], None] = print,
        pattern: str | None = None,
        label: str | None = None,
        hint: str = "",
        timers: bool = False,
        cwd: Path | None = None,
        clock: Callable[[], float] = time.monotonic,
        readline: Callable[[float], str | None] = stdin_reader,
        write: Callable[[str], None] | None = None,
    ) -> None:
        self.ask = ask
        self.say = say
        self.pattern = pattern
        self.label = label
        self.hint = hint
        self.timers = timers
        self.cwd = cwd or Path.cwd()
        self.clock = clock
        self.readline = readline
        self.write = write or self._stdout
        self.seen: set[str] = set()
        self.presses = 0
        self.typed = 0
        self.auto = 0
        self._seen_all = False

    @staticmethod
    def _stdout(text: str) -> None:
        sys.stdout.write(text)
        sys.stdout.flush()

    @property
    def polling(self) -> bool:
        return bool(self.pattern) or self.timers

    def hotkey_names(self) -> list[str]:
        """The `hotkey.<n>` files now present (the host's temporary `.tmp` files do not match), sorted by number."""
        if not self.pattern:
            return []
        names = []
        for path in glob.glob(str(self.cwd / self.pattern)):
            if HOTKEY_FILE_RE.fullmatch(Path(path).name):
                names.append(path)
        return sorted(names, key=lambda p: int(HOTKEY_FILE_RE.fullmatch(Path(p).name).group(1)))  # type: ignore[union-attr]

    def _label_of(self, path: str) -> str:
        try:
            words = Path(path).read_text().split()
        except OSError:
            return ""
        return words[1] if len(words) > 1 else ""

    def absorb(self) -> None:
        """Presses made before a prompt appeared are old news: the new prompt only reacts to later ones."""
        self.seen = set(self.hotkey_names())

    def new_press(self) -> str | None:
        for path in self.hotkey_names():
            if path in self.seen:
                continue
            self.seen.add(path)
            if self.label and self._label_of(path) != self.label:
                continue
            return path
        return None

    def wait(
        self,
        prompt: str,
        auto_seconds: float | None = None,
        suggested: float | None = None,
    ) -> tuple[str, str]:
        """Show `prompt`, wait. Returns (typed text, source) with source `typed`, `hotkey` or `auto` (the countdown ended).

        `auto_seconds` stops by itself (idle phases), `suggested` is shown next to the elapsed time (action phases)."""
        if not self.polling:
            self.typed += 1
            return self.ask(prompt), "typed"
        self.absorb()
        self.write(prompt if prompt.endswith("\n") or not prompt else prompt + "\n")
        start = self.clock()
        announced = False
        while True:
            if self.pattern and self.new_press() is not None:
                self.presses += 1
                self.write("\n")
                return "", "hotkey"
            line = self.readline(POLL_SECONDS)
            if line is not None:
                self.typed += 1
                return line, "typed"
            elapsed = self.clock() - start
            if auto_seconds is not None and elapsed >= auto_seconds:
                self.auto += 1
                self.write("\n")
                return "", "auto"
            if self.timers and (auto_seconds is not None or suggested is not None):
                if (
                    suggested is not None
                    and auto_seconds is None
                    and elapsed >= suggested
                    and not announced
                ):
                    announced = True
                    self.write("\a")
                self.write("\r" + self.status(elapsed, auto_seconds, suggested) + "\x1b[K")

    def status(self, elapsed: float, auto_seconds: float | None, suggested: float | None) -> str:
        how = (
            f"press Enter here or {self.hint}" if self.pattern and self.hint else "press Enter here"
        )
        if auto_seconds is not None:
            remaining = max(0.0, auto_seconds - elapsed)
            return f"  STAND STILL  remaining {format_seconds(remaining)} of {auto_seconds:.0f} s   (stops by itself, or {how} to stop early)"
        if suggested is not None:
            mark = "  (suggested time reached)" if elapsed >= suggested else ""
            return f"  elapsed {format_seconds(elapsed)}   suggested {suggested:.0f} s{mark}   ({how} to stop)"
        return ""

    def hint_text(self) -> str:
        """The words for a prompt: how the owner continues."""
        if self.pattern and self.hint:
            return f"Enter here, or {self.hint}"
        return "Enter"


def timers_wanted(mode: str, ask: Callable[[str], str]) -> bool:
    """--timers on|off|auto. auto: a terminal on both ends and the real `input` prompt (a test or a pipe gets none)."""
    if mode == "on":
        return True
    if mode == "off":
        return False
    return bool(ask is input and sys.stdin.isatty() and sys.stdout.isatty())


# --------------------------------------------------------------------------------------------- the poke
def read_lines_from(path: Path, offset: int) -> list[str]:
    try:
        with path.open("rb") as handle:
            handle.seek(offset)
            return handle.read().decode(errors="replace").splitlines()
    except OSError:
        return []


def apply_poke(
    session_dir: Path,
    pid: int,
    label: str,
    expect_request: bool,
    alive: Callable[[], bool] = lambda: True,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
    timeout: float = POKE_ACK_SECONDS,
) -> dict:
    """Serve `DIR/guestpoke.LABEL` (or only dump when there is none) on the running host with SIGUSR2 and wait for its ack.

    Returns {label, request, acked, ok, lines} where `lines` are the guestpoke.log lines the host appended. `ok` means
    acked and, for a request, every poke line says status=OK and none says REFUSED. The host refuses a request without
    --forced-state and says so in the log (ok False). Never raises on a dead host, it reports acked False."""
    request = session_dir / f"guestpoke.{label}"
    result: dict = {
        "label": label,
        "request": request.is_file(),
        "acked": False,
        "ok": False,
        "lines": [],
    }
    if expect_request and not request.is_file():
        result["lines"] = [
            f"no request file {request.name} in the session folder: nothing was poked"
        ]
        return result
    log = session_dir / "guestpoke.log"
    offset = log.stat().st_size if log.is_file() else 0
    ack = session_dir / "guestpoke.ack"
    ack.unlink(missing_ok=True)
    (session_dir / "guestpoke.phase").write_text(label + "\n")
    try:
        os.kill(pid, signal.SIGUSR2)
    except OSError as error:
        result["lines"] = [f"could not signal the host: {error}"]
        return result
    end = clock() + timeout
    while clock() < end:
        if ack.is_file():
            result["acked"] = True
            break
        if not alive():
            break
        sleep(0.1)
    result["lines"] = [
        line
        for line in read_lines_from(log, offset)
        if f"label={label}" in line or "REFUSED" in line
    ]
    status_lines = [line for line in result["lines"] if " status=" in line]
    refused = any("REFUSED" in line or "status=refused" in line for line in result["lines"])
    all_ok = bool(status_lines) and all(line.endswith("status=OK") for line in status_lines)
    result["ok"] = bool(result["acked"] and not refused and (all_ok if result["request"] else True))
    return result
