#!/usr/bin/env python3
# ruff: noqa: E501
# SPDX-License-Identifier: GPL-3.0-or-later
"""T1736: all-buttons-and-axes checklist driver for the button dump session (tmp/run_button_dumps.fish --checklist).

The host button dump (T1629) records only digital edges. This driver walks a fixed list of steps (tools/data/button_checklist.txt,
grammar in its header) and writes DIR/checklist.jsonl, which tools.button_checklist_report turns into a per-step table.

  list FILE                  validate the checklist and print the steps
  run DIR --file FILE        interactive: print the next instruction, wait for Enter. Digital steps use the host button events
                             (--dump-on-button) found in DIR/buttons.jsonl. Analog steps take two SIGUSR1 dumps of the pad arrays
                             (labels cl<NNN>_<id>_rest and _held) through DIR/guestdump.pid, the same read-only mechanism as
                             tmp/dump_now.fish. The host needs --dump-guest-range PAD_RANGE (printed by `range`).
  headless DIR --file FILE   replay run in a private Xvfb: boots the T1240 Story replay, then holds the KEY of every step with
                             xdotool and takes a rest and a held SIGUSR1 dump per step (digital steps too: no host button events).
  range                      print the --dump-guest-range spec of the pad arrays

Evidence class of what it records: xemu-level / replay-level MEASURED reads of the guest pad arrays, the keyboard is FABRICATED input.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

# guest pad arrays (docs/t-guest-input-chain.md): remap dwords 0x6B93C0 (4), physical records 0x6B93D0 + port*0x3C, effective 0x6B94C8 + slot*0x3C
PAD_BASE = 0x6B93C0
PAD_LEN = 0x1F8
PAD_RANGE = f"0x{PAD_BASE:X}:0x{PAD_LEN:X}"
DEFAULT_FILE = Path("tools/data/button_checklist.txt")
SESSION_FILE = "checklist.jsonl"
ID_RE = re.compile(r"[a-z0-9_-]+")
AXES = ("LX", "LY", "RX", "RY", "LT", "RT")
DIGITAL_NAMES = (
    "A",
    "B",
    "X",
    "Y",
    "BLACK",
    "WHITE",
    "START",
    "BACK",
    "DPAD_UP",
    "DPAD_DOWN",
    "DPAD_LEFT",
    "DPAD_RIGHT",
    "LTHUMB",
    "RTHUMB",
    "LT",
    "RT",
)


class ChecklistError(ValueError):
    """A malformed checklist line (the message carries the line number)."""


@dataclass(frozen=True)
class Expect:
    kind: str  # button or axis
    name: str
    sign: str = ""
    percent: int = 0

    def text(self) -> str:
        if self.kind == "button":
            return f"button:{self.name}"
        return f"axis:{self.name}:{self.sign}:{self.percent}"


@dataclass(frozen=True)
class Step:
    kind: str  # digital or analog
    id: str
    expects: tuple[Expect, ...]
    keys: tuple[str, ...]
    text: str

    def expect_text(self) -> str:
        return ",".join(item.text() for item in self.expects)


def parse_expect(text: str, number: int) -> Expect:
    parts = text.strip().split(":")
    if parts[0] == "button" and len(parts) == 2:
        if not parts[1]:
            raise ChecklistError(f"line {number}: empty button name")
        return Expect("button", parts[1])
    if parts[0] == "axis" and len(parts) == 4:
        _, axis, sign, percent = parts
        if axis not in AXES or sign not in ("+", "-") or not percent.isdigit():
            raise ChecklistError(f"line {number}: bad axis expectation {text!r}")
        if not 1 <= int(percent) <= 100:
            raise ChecklistError(f"line {number}: axis percent {percent} is not 1..100")
        return Expect("axis", axis, sign, int(percent))
    raise ChecklistError(
        f"line {number}: bad expectation {text!r} (button:NAME or axis:AXIS:SIGN:PERCENT)"
    )


def parse_checklist(text: str) -> list[Step]:
    steps: list[Step] = []
    seen: set[str] = set()
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [part.strip() for part in line.split("|")]
        if len(fields) != 5:
            raise ChecklistError(
                f"line {number}: {len(fields)} fields, need 5 (KIND | ID | EXPECT | KEY | INSTRUCTION)"
            )
        kind, step_id, expect_field, key_field, instruction = fields
        if kind not in ("digital", "analog"):
            raise ChecklistError(f"line {number}: kind {kind!r} is not digital or analog")
        if not ID_RE.fullmatch(step_id):
            raise ChecklistError(f"line {number}: id {step_id!r} must match [a-z0-9_-]+")
        if step_id in seen:
            raise ChecklistError(f"line {number}: duplicate id {step_id!r}")
        seen.add(step_id)
        expects = tuple(
            parse_expect(item, number) for item in expect_field.split(",") if item.strip()
        )
        if not expects:
            raise ChecklistError(f"line {number}: no expectation")
        wanted = "button" if kind == "digital" else "axis"
        if any(item.kind != wanted for item in expects):
            raise ChecklistError(f"line {number}: a {kind} step needs {wanted} expectations")
        if kind == "digital" and (len(expects) != 1 or expects[0].name not in DIGITAL_NAMES):
            raise ChecklistError(f"line {number}: unknown digital button in {expect_field!r}")
        keys = () if key_field == "-" else tuple(part for part in key_field.split("+") if part)
        if not instruction:
            raise ChecklistError(f"line {number}: empty instruction")
        steps.append(Step(kind, step_id, expects, keys, instruction))
    if not steps:
        raise ChecklistError("the checklist has no steps")
    return steps


def load_checklist(path: Path) -> list[Step]:
    return parse_checklist(path.read_text())


def step_label(number: int, step: Step, tag: str) -> str:
    return f"cl{number:03d}_{step.id}_{tag}"


# --------------------------------------------------------------------------------------------------------------------
# Dump request (the dump_now.fish mechanism): label into guestdump.phase, SIGUSR1, wait for guestdump.ack
# --------------------------------------------------------------------------------------------------------------------


def request_dump(
    directory: Path,
    pid: int,
    label: str,
    *,
    timeout: float = 10.0,
    sender: Callable[[int, int], None] = os.kill,
    sleeper: Callable[[float], None] = time.sleep,
) -> Path | None:
    """Take one SIGUSR1 dump named `label`. Returns its path, or None when no ack arrived in `timeout` seconds."""
    (directory / "guestdump.phase").write_text(label + "\n")
    ack = directory / "guestdump.ack"
    ack.unlink(missing_ok=True)
    sender(pid, signal.SIGUSR1)
    waited = 0.0
    while waited <= timeout:
        if ack.exists():
            return directory / f"guestdump.{label}"
        sleeper(0.25)
        waited += 0.25
    return None


# --------------------------------------------------------------------------------------------------------------------
# Session record
# --------------------------------------------------------------------------------------------------------------------


@dataclass
class Session:
    directory: Path

    @property
    def path(self) -> Path:
        return self.directory / SESSION_FILE

    def start(self, steps: list[Step], mode: str) -> None:
        header = {
            "type": "start",
            "mode": mode,
            "range": PAD_RANGE,
            "base": PAD_BASE,
            "steps": len(steps),
        }
        self.path.write_text(json.dumps(header) + "\n")

    def write_step(
        self,
        number: int,
        step: Step,
        pairs: list[dict[str, str]],
        host_events: int | None,
        note: str,
    ) -> None:
        record = {
            "type": "step",
            "n": number,
            "id": step.id,
            "kind": step.kind,
            "expect": step.expect_text(),
            "pairs": pairs,
            "host_events": host_events,
            "note": note,
        }
        with self.path.open("a") as handle:
            handle.write(json.dumps(record) + "\n")

    def relative(self, path: Path) -> str:
        try:
            return str(path.relative_to(self.directory))
        except ValueError:
            return str(path)


def manifest_event_count(directory: Path) -> int:
    path = directory / "buttons.jsonl"
    if not path.exists():
        return 0
    return sum(
        1 for line in path.read_text(errors="replace").splitlines() if '"type":"event"' in line
    )


def manifest_pair(directory: Path, after_index: int) -> tuple[dict[str, str] | None, int]:
    """First non-idle event with index > after_index: its before dump and its earliest after dump. Returns (pair, new last index)."""
    from tools import button_dump_report as report

    manifest = report.load_manifest(directory / "buttons.jsonl")
    last = after_index
    chosen: dict[str, str] | None = None
    for event in manifest.events:
        if event.index <= after_index:
            continue
        last = max(last, event.index)
        if event.idle or chosen is not None:
            continue
        before = next(
            (item for item in event.dumps if str(item.get("kind", "")).endswith("before")), None
        )
        afters = sorted(
            (item for item in event.dumps if str(item.get("kind", "")).startswith("after")),
            key=report._tag_offset,
        )
        if before is None or not afters:
            continue
        before_path = report._dump_path(directory, before)
        after_path = report._dump_path(directory, afters[0])
        if before_path is not None and after_path is not None:
            chosen = {
                "before": str(before_path.relative_to(directory)),
                "after": str(after_path.relative_to(directory)),
            }
    return chosen, last


# --------------------------------------------------------------------------------------------------------------------
# Interactive run
# --------------------------------------------------------------------------------------------------------------------


def run_interactive(
    directory: Path,
    steps: list[Step],
    pid: int,
    *,
    ask: Callable[[str], str],
    say: Callable[[str], None] = print,
    dumper: Callable[[Path, int, str], Path | None] = request_dump,
    sleeper: Callable[[float], None] = time.sleep,
    settle: float = 1.0,
    start_at: str = "",
) -> int:
    """Walk the steps. `ask` shows a prompt and returns the typed line. Returns the number of steps recorded."""
    session = Session(directory)
    if not session.path.exists() or not start_at:
        session.start(steps, "interactive")
    first = 0
    if start_at:
        ids = [step.id for step in steps]
        if start_at not in ids:
            raise ChecklistError(f"--start-at {start_at!r} is not a step id")
        first = ids.index(start_at)
    last_event = manifest_event_count(directory)
    recorded = 0
    total = len(steps)
    for index in range(first, total):
        number = index + 1
        step = steps[index]
        say("")
        say(f"[{number}/{total}] {step.id}: {step.text}")
        if step.kind == "digital":
            answer = ask("   tap it ONCE, wait a second, then Enter  (s = skip, q = quit): ")
            if answer.strip().lower() == "q":
                break
            if answer.strip().lower() == "s":
                session.write_step(number, step, [], None, "skipped")
                continue
            sleeper(1.5)
            pair, new_last = manifest_pair(directory, last_event)
            events = max(0, manifest_event_count(directory) - last_event)
            last_event = max(new_last, manifest_event_count(directory))
            pairs = [pair] if pair else []
            note = (
                "" if pair else "no host button event with dumps appeared (is --dump-on-button on?)"
            )
            if not pair:
                say(f"   !! {note}")
            else:
                say(f"   host events: {events}")
            session.write_step(number, step, pairs, events, note)
            recorded += 1
            continue
        sleeper(settle)
        rest = dumper(directory, pid, step_label(number, step, "rest"))
        if rest is None:
            say("   !! the rest dump got no ack (is the game busy?), step recorded without it")
        answer = ask(
            "   HOLD it steady and press Enter while you keep holding  (s = skip, q = quit): "
        )
        if answer.strip().lower() == "q":
            break
        if answer.strip().lower() == "s":
            session.write_step(number, step, [], None, "skipped")
            continue
        before_events = manifest_event_count(directory)
        held = dumper(directory, pid, step_label(number, step, "held"))
        events = max(0, manifest_event_count(directory) - before_events)
        last_event = manifest_event_count(directory)
        if held is None or rest is None:
            note = "a dump got no ack"
            say(f"   !! {note}")
            session.write_step(number, step, [], events, note)
        else:
            say("   taken, release it now")
            pair = {"before": session.relative(rest), "after": session.relative(held)}
            session.write_step(number, step, [pair], events, "")
        recorded += 1
    return recorded


def read_pid(directory: Path) -> int:
    path = directory / "guestdump.pid"
    try:
        return int(path.read_text().strip())
    except (OSError, ValueError) as error:
        raise SystemExit(f"cannot read the host pid from {path}: {error}") from error


# --------------------------------------------------------------------------------------------------------------------
# Headless replay run
# --------------------------------------------------------------------------------------------------------------------


def headless_run(directory: Path, steps: list[Step], args: argparse.Namespace) -> int:
    """Private Xvfb + the T1240 Story replay host, keys through xdotool (FABRICATED input), SIGUSR1 dumps of the pad arrays."""
    from tools import t1240_drive as drive
    from tools.t982_acceptance import ROOT, main_root

    drive.DUMP_RANGES = f"*0x7B0C48+0:0x1584,{PAD_RANGE}"
    disc = (
        Path(args.disc)
        if args.disc
        else ROOT / "tmp/TimeSplitters - Future Perfect (USA) (XBOX).iso"
    )
    modules = args.modules or str(main_root() / "tmp/play/modules")
    machine = drive.Driver(directory, args.host, disc, args.replay, modules, [])
    session = Session(directory)
    session.start(steps, "headless")
    recorded = 0
    try:
        deadline = time.monotonic() + args.handover_timeout
        log = directory / "run.log"
        while time.monotonic() < deadline:
            time.sleep(2)
            if machine.host.poll() is not None:
                print("the host exited before the replay handover, see run.log")
                return 0
            text = log.read_text(errors="replace") if log.exists() else ""
            if "the recorded inputs are exhausted" in text:
                break
        else:
            print("replay handover not seen in time, stepping anyway")
        time.sleep(args.settle)
        dump_dir = directory / "dump"
        for index, step in enumerate(steps):
            number = index + 1
            if not step.keys:
                note = "skipped: no keyboard key (a key is full deflection, a partial one cannot be typed)"
                session.write_step(number, step, [], None, note)
                print(f"[{number}/{len(steps)}] {step.id} {note}", flush=True)
                continue
            labels = {tag: step_label(number, step, tag) for tag in ("rest", "held")}
            machine.set_keys(set())
            time.sleep(args.hold)
            machine.thaw()
            paths = {}
            for tag in ("rest", "held"):
                if tag == "held":
                    machine.set_keys({key for key in step.keys})
                    time.sleep(args.hold)
                (dump_dir / "guestdump.phase").write_text(labels[tag] + "\n")
                ack = dump_dir / "guestdump.ack"
                ack.unlink(missing_ok=True)
                os.kill(machine.host.pid, signal.SIGUSR1)
                for _ in range(40):
                    if ack.exists():
                        break
                    time.sleep(0.25)
                paths[tag] = dump_dir / f"guestdump.{labels[tag]}"
            machine.set_keys(set())
            ok = all(path.exists() for path in paths.values())
            pairs = (
                [
                    {
                        "before": session.relative(paths["rest"]),
                        "after": session.relative(paths["held"]),
                    }
                ]
                if ok
                else []
            )
            session.write_step(number, step, pairs, None, "" if ok else "a dump is missing")
            recorded += 1
            print(
                f"[{number}/{len(steps)}] {step.id} keys={'+'.join(step.keys) or '-'} {'ok' if ok else 'DUMP MISSING'}",
                flush=True,
            )
    finally:
        machine.set_keys(set())
        machine.close()
    return recorded


# --------------------------------------------------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="mode", required=True)
    listing = sub.add_parser("list", help="validate a checklist file and print its steps")
    listing.add_argument("file", nargs="?", default=str(DEFAULT_FILE))
    sub.add_parser("range", help="print the --dump-guest-range spec of the pad arrays")
    run = sub.add_parser("run", help="interactive checklist against a running host")
    run.add_argument("directory")
    run.add_argument("--file", default=str(DEFAULT_FILE))
    run.add_argument("--start-at", default="", help="step id to resume at (the record is appended)")
    run.add_argument(
        "--settle",
        type=float,
        default=1.0,
        help="seconds to wait before the rest dump of an analog step",
    )
    head = sub.add_parser("headless", help="replay run in a private Xvfb with xdotool keys")
    head.add_argument("directory")
    head.add_argument("--file", default=str(DEFAULT_FILE))
    head.add_argument("--host", default="tmp/private-host/build-b7de1f2885bb336e/tsfp_host")
    head.add_argument("--disc")
    head.add_argument("--modules")
    head.add_argument("--replay", default="tmp/recorded-input-story-mode")
    head.add_argument("--handover-timeout", type=float, default=240.0)
    head.add_argument("--settle", type=float, default=3.0)
    head.add_argument(
        "--hold", type=float, default=0.6, help="seconds the keys are held before the held dump"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.mode == "range":
        print(PAD_RANGE)
        return 0
    try:
        steps = load_checklist(Path(args.file if args.mode != "list" else args.file))
    except (OSError, ChecklistError) as error:
        print(f"checklist error: {error}", file=sys.stderr)
        return 2
    if args.mode == "list":
        for number, step in enumerate(steps, 1):
            print(
                f"{number:3d} {step.kind:7s} {step.id:18s} {step.expect_text():40s} keys={'+'.join(step.keys) or '-'}"
            )
        analog = sum(1 for step in steps if step.kind == "analog")
        print(
            f"{len(steps)} steps: {len(steps) - analog} digital, {analog} analog; pad range {PAD_RANGE}"
        )
        return 0
    directory = Path(args.directory)
    directory.mkdir(parents=True, exist_ok=True)
    if args.mode == "headless":
        count = headless_run(directory, steps, args)
    else:
        pid = read_pid(directory)

        def ask(prompt: str) -> str:
            try:
                return input(prompt)
            except EOFError:
                return "q"

        try:
            count = run_interactive(
                directory, steps, pid, ask=ask, settle=args.settle, start_at=args.start_at
            )
        except ChecklistError as error:
            print(f"checklist error: {error}", file=sys.stderr)
            return 2
    print(f"{count} steps recorded in {directory / SESSION_FILE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
