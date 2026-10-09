#!/usr/bin/env python3
# ruff: noqa: E501
"""T1495: guided action-profile sessions. One game run, one profile per action, no restart between actions.

The owner plays, this tool tells them what to do and marks where each action starts and ends. The host sampler
(src/host/cpu_sampler.c, `--cpu-profile FILE`) dumps and resets its profile when it receives SIGUSR1, so each
action ends up in its own file `FILE.<phase>`; `analyze` then names the functions that run only during an action.

  python -m tools.action_profile list [--group menu|ingame]
  python -m tools.action_profile session --group menu --disc PATH_TO_YOUR_DISC_IMAGE [--host PATH_TO_tsfp_host]
  python -m tools.action_profile run --scenario fire --seconds 25 --disc PATH [--no-signals]
  python -m tools.action_profile analyze tmp/owner-profiles/SESSION

Everything lands under tmp/owner-profiles/<session>/ (gitignored). See docs/t1495-action-profiles.md.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import time
from collections.abc import Callable
from datetime import UTC, datetime
from pathlib import Path

from tools import action_advance, owner_scenario, pickup_poke
from tools.play import route_check

ROOT = Path(__file__).resolve().parent.parent
SCENARIO_FILE = Path("tools/data/action_scenarios.json")
OUT_ROOT = Path("tmp/owner-profiles")
PROFILE_NAME = "profile.txt"
CENSUS_NAME = "census.txt"
SOURCES = ("census", "sampler", "both")
ACK_TIMEOUT_SECONDS = 90.0
PHASE_RE = re.compile(r"[^A-Za-z0-9_-]")
LABEL_RE = re.compile(r"[a-z0-9_]+")
LABEL_FILE_SEP = "--"  # the host strips '@' from a phase name: `ingame-fire@pistol` is saved as FILE.ingame-fire--pistol


def load_scenarios(path: Path | None = None) -> list[dict]:
    data = json.loads((path or ROOT / SCENARIO_FILE).read_text())["scenarios"]
    needed = {"id", "title", "starts_at", "instruction", "seconds", "kind", "group"}
    seen: set[str] = set()
    for scenario in data:
        missing = needed - scenario.keys()
        if missing or scenario["id"] in seen or PHASE_RE.search(scenario["id"]):
            raise ValueError(f"bad scenario {scenario.get('id')}: {sorted(missing)}")
        seen.add(scenario["id"])
    return data


def host_commit() -> str:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short=12", "HEAD"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        return result.stdout.strip() or "unknown"
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"


def play_command(
    args: argparse.Namespace, session_dir: Path, signals: bool, scenario_id: str | None = None
) -> list[str]:
    """The tools.play command line, all paths relative to the repository root."""
    if getattr(args, "play_command", None):
        base = shlex.split(args.play_command)
    else:
        # T1618: the game affecting flags come from the ONE list the route recording uses too
        base = [
            sys.executable,
            "-m",
            "tools.play",
            *route_check.route_game_flags(skip_intro=not args.no_skip_intro),
        ]
        if args.disc:
            base += ["--disc", str(args.disc)]
        if args.host:
            base += ["--host", str(args.host), "--no-build"]
        base += ["--run-dir", str(session_dir / "run")]
    source = getattr(args, "source", "census")
    flags: list[str] = []
    if source in ("sampler", "both"):
        flag = "--cpu-profile-wall" if args.wall else "--cpu-profile"
        target = session_dir / (
            PROFILE_NAME if signals or scenario_id is None else f"{scenario_id}.whole"
        )
        flags += [flag, str(target)]
    if source in ("census", "both"):
        # T1502: the exact indirect-call census, dumped and reset at every mark (signals) or printed at exit (whole run)
        flags += ["--census-icalls"]
        if signals or scenario_id is None:
            flags += ["--census-phases", str(session_dir / CENSUS_NAME)]
    replays = any(str(item).startswith("--replay-input") for item in args.play_arg)
    if getattr(args, "no_route_nav", False) and replays:
        flags += ["--no-route-nav"]  # tools.play adds the nav flags itself otherwise
    return [*base, *flags, *args.play_arg]


def source_files(source: str, session_dir: Path) -> list[Path]:
    """The files whose `.pid`, `.ack` and `.phase` siblings carry the phase protocol, one per active source."""
    files = []
    if source in ("census", "both"):
        files.append(session_dir / CENSUS_NAME)
    if source in ("sampler", "both"):
        files.append(session_dir / PROFILE_NAME)
    return files


class HostSession:
    """The running host, reached through the files the sampler writes next to the profile."""

    def __init__(
        self,
        command: list[str],
        profile: Path,
        cwd: Path = ROOT,
        channels: list[Path] | None = None,
    ) -> None:
        self.command = command
        self.profile = profile
        # every source (census, sampler) has its own FILE.pid/.ack/.phase, all answer the one SIGUSR1
        self.channels = channels or [profile]
        self.cwd = cwd
        self.process: subprocess.Popen | None = None
        self.served = 0

    def side(self, suffix: str, base: Path | None = None) -> Path:
        base = base or self.profile
        return base.with_name(base.name + suffix)

    def start(self, log: Path) -> None:
        self.profile.parent.mkdir(parents=True, exist_ok=True)
        for channel in self.channels:
            for suffix in (".pid", ".ack", ".phase"):
                self.side(suffix, channel).unlink(missing_ok=True)
        handle = log.open("wb")
        self.process = subprocess.Popen(  # noqa: S603
            self.command,
            cwd=self.cwd,
            stdin=subprocess.DEVNULL,
            stdout=handle,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

    def alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def host_pid(self) -> int | None:
        pids = []
        for channel in self.channels:
            try:
                pids.append(int(self.side(".pid", channel).read_text().split()[0]))
            except (OSError, ValueError, IndexError):
                return None  # a source has not started yet
        return pids[0]

    def wait_ready(self, seconds: float = 7200.0) -> bool:
        """The host writes FILE.pid once its sampler runs."""
        end = time.monotonic() + seconds
        while time.monotonic() < end and self.alive():
            if self.host_pid() is not None:
                return True
            time.sleep(0.2)
        return self.host_pid() is not None

    def acked(self, channel: Path | None = None) -> int:
        try:
            return int(self.side(".ack", channel).read_text().split()[0])
        except (OSError, ValueError, IndexError):
            return 0

    def mark(self, phase: str) -> bool:
        """Save the profile since the last mark as FILE.<phase> and reset the counters. False when the host did not confirm."""
        pid = self.host_pid()
        if pid is None or not self.alive():
            return False
        phase = PHASE_RE.sub("", phase) or "phase"
        for channel in self.channels:
            self.side(".phase", channel).write_text(phase + "\n")
        before = {channel: self.acked(channel) for channel in self.channels}
        try:
            os.kill(pid, signal.SIGUSR1)
        except OSError:
            return False
        end = time.monotonic() + ACK_TIMEOUT_SECONDS
        while time.monotonic() < end:
            if all(self.acked(channel) > before[channel] for channel in self.channels):
                self.served = self.acked()
                return True
            if not self.alive():
                break
            time.sleep(0.1)
        return False

    def stop(self) -> None:
        pid = self.host_pid()
        if pid is not None:
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
        if self.process is not None:
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(self.process.pid, signal.SIGKILL)
                except OSError:
                    pass
                self.process.wait()


def now_iso() -> str:
    return datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


def scenario_key(scenario: dict) -> str:
    """`ingame-fire@shotgun` for a labeled scenario (T1502 per-phase label), else the plain id."""
    return f"{scenario['id']}@{scenario['label']}" if scenario.get("label") else scenario["id"]


def phase_file(phase: str) -> str:
    """The name the host saves a phase under (`ingame-fire@shotgun` -> `ingame-fire--shotgun`)."""
    return phase.replace("@", LABEL_FILE_SEP)


def select_scenarios(args: argparse.Namespace) -> list[dict]:
    scenarios = load_scenarios()
    # T1502: a scenario that crashes the game at present (skip_by_default) is only run when asked for with --only
    chosen = [s for s in scenarios if s["group"] == args.group and not s.get("skip_by_default")]
    if args.only:
        items = [name.strip() for name in args.only.split(",") if name.strip()]
        wanted = [item.partition(":")[0] for item in items]
        unknown = [name for name in wanted if name not in {s["id"] for s in scenarios}]
        if unknown:
            raise SystemExit(
                f"unknown scenario id(s) {unknown}, see: python -m tools.action_profile list"
            )
        if any(":" in item for item in items):
            # T1502 labels: the order and the repeats of --only are kept, `id:label` is its own row of the session
            by_id = {s["id"]: s for s in scenarios}
            chosen = []
            seen: set[str] = set()
            for item in items:
                name, _, label = item.partition(":")
                if ":" in item and not LABEL_RE.fullmatch(label):
                    raise SystemExit(
                        f"bad label {label!r} in --only {item!r}: use lowercase letters, digits and _ (for example {name}:shotgun)"
                    )
                entry = dict(by_id[name], label=label) if label else dict(by_id[name])
                if scenario_key(entry) in seen:
                    raise SystemExit(f"{scenario_key(entry)} is listed twice in --only")
                seen.add(scenario_key(entry))
                chosen.append(entry)
        else:
            chosen = [s for s in scenarios if s["id"] in wanted]
    if not chosen:
        raise SystemExit("no scenario selected")
    return chosen


def write_metadata(session_dir: Path, meta: dict) -> None:
    (session_dir / "session.json").write_text(json.dumps(meta, indent=1) + "\n")


def instruction_of(scenario: dict) -> str:
    """The instruction, led by the label when the phase has one (T1502): the owner changes weapon/state first.

    T1767: a phase with script specific text (`specific`) carries its own switch text and instruction, nothing is prefixed.
    """
    if scenario.get("specific"):
        return scenario["instruction"]
    if not scenario.get("label"):
        return scenario["instruction"]
    return f"Switch to: {scenario['label']}. Do: {scenario['instruction']}"


GENERIC_LABEL_WARNING = (
    "the text below is the GENERIC example (the pistol, Story 2401) and does NOT describe '{label}'. "
    "Run it from its own script (tmp/*.fish) or add the phase to tools/data/owner_scenarios/<script>.json"
)
MISSING_PHASE_WARNING = (
    "the owner scenario '{script}' has no text for phase {key}: the GENERIC example below is shown and may be wrong. "
    "Add `{key}` to tools/data/owner_scenarios/{script}.json"
)


def parse_owner_opts(items: list[str] | None) -> dict[str, str]:
    try:
        return owner_scenario.parse_opts(items or [])
    except owner_scenario.ScenarioError as error:
        raise SystemExit(f"--owner-opt: {error}") from error


def apply_owner_text(scenarios: list[dict], script: str | None, opts: dict[str, str]) -> list[dict]:
    """T1767: replace the generic phase text by the script specific one; warn loudly where only the generic text exists.

    A script scenario sets title, starts_at, instruction (and seconds, switch, note) of each phase it knows and marks the
    phase `specific`. A phase it does not know, or a labelled phase without any script, keeps the old text plus a warning.
    """
    result: list[dict] = []
    for scenario in scenarios:
        entry = dict(scenario)
        if entry["kind"] in action_advance.STEP_KINDS:
            result.append(entry)
            continue
        label = entry.get("label")
        key = scenario_key(entry).replace("@", ":")
        text = None
        if script:
            try:
                text = owner_scenario.phase_text(script, entry["id"], label, opts)
            except owner_scenario.ScenarioError as error:
                entry["warning"] = (
                    f"owner scenario '{script}' cannot be used ({error}): the GENERIC example below is shown and may be wrong."
                )
        if text:
            entry.update(
                title=text["title"],
                starts_at=text["starts_at"],
                instruction=text["instruction"],
                specific=True,
                switch=text["switch"],
                note=text["note"],
            )
            if text["seconds"]:
                entry["seconds"] = text["seconds"]
        elif "warning" not in entry:
            if script:
                entry["warning"] = MISSING_PHASE_WARNING.format(script=script, key=key)
            elif label:
                entry["warning"] = GENERIC_LABEL_WARNING.format(label=label)
        result.append(entry)
    return result


def route_refusal_lines(log: Path) -> list[str]:
    """T1618: the route refusal tools.play printed before the host started (`play: route ... different flag set` and its detail)."""
    try:
        lines = log.read_text(errors="replace").splitlines()
    except OSError:
        return []
    for index, line in enumerate(lines):
        if line.startswith("play: route "):
            return [
                line,
                *(follow for follow in lines[index + 1 : index + 5] if follow.startswith("  ")),
            ]
    return []


def describe_step(item: dict) -> str:
    """One line for --dry-run: a plan step or question."""
    if item["kind"] == "observe":
        return f"question {item['name']}: {item['text']}"
    action = f" [{item['action']}={item['label']}]" if item["action"] else ""
    return f"step {item['name']}{action}: {item['text']}"


def action_advance_pid(session_dir: Path) -> int | None:
    """The host pid from guestdump.pid (the dump thread that serves SIGUSR2), else None."""
    try:
        return int((session_dir / "guestdump.pid").read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def run_session(
    args: argparse.Namespace,
    scenarios: list[dict],
    session_name: str,
    ask: Callable[[str], str] = input,
    say: Callable[[str], None] = print,
    cwd: Path = ROOT,
    advance: action_advance.Advance | None = None,
) -> int:
    session_dir = OUT_ROOT / session_name
    absolute = cwd / session_dir
    command = play_command(args, session_dir, True)
    if args.dry_run:
        say("session directory: " + str(session_dir))
        say("host command: " + shlex.join(command))
        for scenario in scenarios:
            if scenario["kind"] in action_advance.STEP_KINDS:
                say(f"  [{scenario['kind']}] {describe_step(scenario)}")
                continue
            say(
                f"  [{scenario['kind']}] {scenario_key(scenario)}: {instruction_of(scenario)} ({scenario['seconds']} s)"
            )
            if scenario.get("warning"):
                say(f"    !!! WARNING (T1767): {scenario['warning']}")
        return 0
    if advance is None:
        advance = action_advance.Advance(
            ask=ask,
            say=say,
            pattern=getattr(args, "advance_file", None),
            label=getattr(args, "advance_label", None),
            hint=getattr(args, "advance_hint", None) or "",
            timers=action_advance.timers_wanted(getattr(args, "timers", "off"), ask),
            cwd=cwd,
        )
    absolute.mkdir(parents=True, exist_ok=True)
    source = getattr(args, "source", "census")
    channels = [cwd / path for path in source_files(source, session_dir)]
    host = HostSession(command, channels[0], cwd, channels)
    meta: dict = {
        "session": session_name,
        "tool": "tools.action_profile",
        "host_commit": host_commit(),
        "disc": Path(args.disc).name if args.disc else None,
        "mode": "wall" if args.wall else "cpu",
        "sources": ["census", "sampler"] if source == "both" else [source],
        "census_file": CENSUS_NAME if source in ("census", "both") else None,
        "wall_start": now_iso(),
        "wall_stop": None,
        "owner_script": getattr(args, "owner_script", None),
        "text_missing": [
            scenario_key(s)
            for s in scenarios
            if s.get("warning") and "kind" in s and s["kind"] not in action_advance.STEP_KINDS
        ],
        "scenario_order": [
            scenario_key(s) for s in scenarios if s["kind"] not in action_advance.STEP_KINDS
        ],
        "phases": [],
        "interrupted": False,
    }
    if any(s["kind"] in action_advance.STEP_KINDS for s in scenarios):
        # T1627: the plan with its steps (the FABRICATED-STATE pokes happen in steps) and what the owner answered
        meta["plan_order"] = [
            f"{s['kind']}:{s['name']}"
            if s["kind"] in action_advance.STEP_KINDS
            else scenario_key(s)
            for s in scenarios
        ]
        meta["steps"] = []
        meta["observations"] = []
    say(
        "Starting the game. The first run builds the host, which can take many minutes, then a window opens. Leave this terminal open."
    )
    host.start(absolute / "host.log")
    if not host.wait_ready():
        host.stop()
        say(
            f"The game did not start. Look at {session_dir / 'host.log'} and {session_dir / 'run' / 'host.err'}."
        )
        for line in route_refusal_lines(absolute / "host.log"):
            say(line)
        meta["wall_stop"] = now_iso()
        meta["error"] = "host did not start"
        write_metadata(absolute, meta)
        return 1
    nav_count = 0

    def save(
        phase: str,
        kind: str,
        scenario: str | None,
        started: str,
        started_clock: float,
        label: str | None = None,
    ) -> bool:
        confirmed = host.mark(phase_file(phase))
        meta["phases"].append(
            {
                "name": phase,
                "file": phase_file(phase),
                "label": label,
                "kind": kind,
                "scenario": scenario,
                "wall_start": started,
                "wall_stop": now_iso(),
                "seconds": round(time.monotonic() - started_clock, 1),
                "saved": confirmed,
            }
        )
        write_metadata(absolute, meta)
        if not confirmed:
            say("  (warning: the game did not confirm saving this part, it is lost)")
        return confirmed

    def run_step(item: dict, index: int, total: int) -> bool:
        """T1627: a plan step or question. False when the owner asked to finish the session."""
        say("")
        if item["kind"] == "observe":
            say(f"=== {index}/{total}: QUESTION {item['name']}  [not recorded] ===")
            say(item["text"])
            text, how = advance.wait("Your answer (type it, then Enter; q = finish the session): ")
            if text.strip().lower() == "q":
                return False
            meta["observations"].append(
                {
                    "name": item["name"],
                    "question": item["text"],
                    "answer": text.strip()
                    if how == "typed"
                    else "(continued with the hotkey, no text)",
                    "wall": now_iso(),
                }
            )
            write_metadata(absolute, meta)
            return True
        say(f"=== {index}/{total}: STEP {item['name']}  [not recorded] ===")
        say(item["text"])
        record: dict = {
            "name": item["name"],
            "action": item["action"],
            "label": item["label"],
            "wall_start": now_iso(),
        }
        meta["steps"].append(record)
        if item["action"]:
            what = {
                "poke": "poke the weapon slots (FABRICATED-STATE)",
                "pickup": "poke the LIVE pickup records of the spawners (FABRICATED-STATE)",
            }.get(item["action"], "take a memory dump")
            answer = (
                advance.wait(
                    f"Press {advance.hint_text()} when READY and I will {what} now (s = skip this, q = finish the session): "
                )[0]
                .strip()
                .lower()
            )
            if answer == "q":
                write_metadata(absolute, meta)
                return False
            if answer == "s":
                record["skipped"] = True
            else:
                pid = action_advance_pid(absolute) or host.host_pid()
                built = True
                if item["action"] == "pickup":
                    built, build_lines = pickup_poke.build_request(absolute, item["label"])
                    record["build_log"] = build_lines
                    for line in build_lines:
                        say("  " + line)
                    if not built:
                        say("  RESULT: no request could be built from the dump, nothing was poked.")
                        record["ok"] = False
                if not built:
                    pass
                elif pid is None:
                    say("  The host has no pid file yet, nothing was sent.")
                    record["error"] = "no host pid"
                else:
                    result = action_advance.apply_poke(
                        absolute,
                        pid,
                        item["label"],
                        item["action"] in ("poke", "pickup"),
                        alive=host.alive,
                    )
                    record.update(
                        acked=result["acked"],
                        ok=result["ok"],
                        request=result["request"],
                        log=result["lines"],
                    )
                    for line in result["lines"]:
                        say("  " + line)
                    if result["ok"]:
                        say(f"  RESULT: done, dump {session_dir}/guestdump.{item['label']}")
                    else:
                        say(
                            f"  RESULT: NOT CONFIRMED (acked: {result['acked']}). Look at guestpoke.log in {session_dir}. Nothing is claimed."
                        )
        if item["after"] or not item["action"]:
            after = item["after"] or "Do that now."
            answer = (
                advance.wait(
                    f"{after}  Press {advance.hint_text()} to continue (q = finish the session): "
                )[0]
                .strip()
                .lower()
            )
            if answer == "q":
                write_metadata(absolute, meta)
                return False
        record["wall_stop"] = now_iso()
        write_metadata(absolute, meta)
        return True

    clock_mark = time.monotonic()
    wall_mark = now_iso()
    try:
        say(
            "\nWait until the game shows the first thing asked below. You can take your time, nothing is measured until you say go."
        )
        for index, scenario in enumerate(scenarios, start=1):
            if not host.alive():
                say("The game window was closed. Saving what exists.")
                break
            if scenario["kind"] in action_advance.STEP_KINDS:
                if not run_step(scenario, index, len(scenarios)):
                    break
                continue
            say("")
            say(
                f"=== {index}/{len(scenarios)}: {scenario['title']}  [{'DO NOTHING' if scenario['kind'] == 'control' else 'ONE action'}] ==="
            )
            if scenario.get("warning"):
                say(f"!!! WARNING (T1767): {scenario['warning']}")
            if scenario.get("prelude"):
                say(f"First (not recorded): {scenario['prelude']}")
            if scenario.get("specific"):
                if scenario.get("switch"):
                    say(
                        f"Switch to: {scenario['switch']}  (do this FIRST, during the throwaway step below)"
                    )
            elif scenario.get("label"):
                say(
                    f"Switch to: {scenario['label']}  (change weapon or state to this FIRST, during the throwaway step below)"
                )
            say(f"Go to: {scenario['starts_at']}")
            say(f"Then:  {instruction_of(scenario)}")
            say(f"Time:  about {scenario['seconds']} seconds")
            if scenario.get("note"):
                say(f"Note:  {scenario['note']}")
            repeat = 0
            while True:
                answer = (
                    advance.wait(
                        f"Press {advance.hint_text()} when you are there and ready to START (s = skip this one, q = finish the session): "
                    )[0]
                    .strip()
                    .lower()
                )
                if answer in ("s", "q"):
                    break
                nav_count += 1
                save(f"nav-{nav_count:02d}", "navigation", None, wall_mark, clock_mark)
                clock_mark, wall_mark = time.monotonic(), now_iso()
                say(f">>> GO NOW: {instruction_of(scenario)}")
                # T1627: an idle phase (kind control) stops by itself at its scenario time when timers are on, an action
                # phase shows the elapsed time against the suggested one and the owner ends it
                idle = scenario["kind"] == "control"
                if advance.timers and idle:
                    say(
                        f">>> Stand still. The game stops this phase by itself after {scenario['seconds']} seconds."
                    )
                else:
                    say(
                        f">>> Do it for about {scenario['seconds']} seconds, then press {advance.hint_text()} at once."
                    )
                _, how = advance.wait(
                    f"Press {advance.hint_text()} when you STOP: ",
                    auto_seconds=float(scenario["seconds"]) if idle and advance.timers else None,
                    suggested=float(scenario["seconds"]),
                )
                if how == "auto":
                    say(f"Time is up ({scenario['seconds']} s), the phase was stopped by itself.")
                key = scenario_key(scenario)
                phase = key if repeat == 0 else f"{key}-r{repeat + 1}"
                elapsed = time.monotonic() - clock_mark
                save(
                    phase,
                    scenario["kind"],
                    scenario["id"],
                    wall_mark,
                    clock_mark,
                    scenario.get("label"),
                )
                clock_mark, wall_mark = time.monotonic(), now_iso()
                say(f"Saved '{phase}' ({elapsed:.0f} s).")
                if scenario.get("note"):
                    say(f"Remember to note: {scenario['note']}")
                if elapsed < getattr(args, "min_seconds", 5.0):
                    say("That was very short. Press Enter again to redo it, or type n to continue.")
                    if advance.wait("Redo? [Y/n]: ")[0].strip().lower() != "n":
                        repeat += 1
                        continue
                break
            if answer == "q":
                break
        say("\nAll done. Closing the game.")
    except (KeyboardInterrupt, EOFError):
        say("\nInterrupted. Saving the part in progress, then closing the game.")
        meta["interrupted"] = True
        save("interrupted", "navigation", None, wall_mark, clock_mark)
    finally:
        host.stop()
        meta["wall_stop"] = now_iso()
        write_metadata(absolute, meta)
    saved = [p["name"] for p in meta["phases"] if p["saved"] and p["kind"] != "navigation"]
    say(f"Saved {len(saved)} profile(s) in {session_dir}.")
    if source in ("census", "both"):
        say(f"Census:  python -m tools.action_census_diff {session_dir}")
    if source in ("sampler", "both"):
        say(f"Analyze them with:  python -m tools.action_profile analyze {session_dir}")
    say(
        f"Send back (or keep) the folder {session_dir} (profiles are small text files, no game data)."
    )
    return 0


def run_single_whole(
    args: argparse.Namespace, scenario: dict, session_name: str, say: Callable[[str], None] = print
) -> int:
    """No signals (for example Windows): one game run per scenario, the whole run is the profile."""
    session_dir = OUT_ROOT / session_name
    command = play_command(args, session_dir, False, scenario["id"])
    if args.dry_run:
        say("host command: " + shlex.join(command))
        return 0
    (ROOT / session_dir).mkdir(parents=True, exist_ok=True)
    say(f"{scenario['title']}: {scenario['instruction']}")
    say(
        "The profile covers the whole run including the boot. Do the action, then close the window."
    )
    code = subprocess.run(command, cwd=ROOT, stdin=subprocess.DEVNULL, check=False).returncode  # noqa: S603
    write_metadata(
        ROOT / session_dir,
        {
            "session": session_name,
            "mode": "whole-run",
            "sources": ["census", "sampler"] if args.source == "both" else [args.source],
            "host_commit": host_commit(),
            "disc": Path(args.disc).name if args.disc else None,
            "scenario_order": [scenario["id"]],
            "phases": [],
            "whole": [f"{scenario['id']}.whole"],
        },
    )
    return code


def cmd_list(args: argparse.Namespace) -> int:
    for scenario in load_scenarios():
        if args.group and scenario["group"] != args.group:
            continue
        print(
            f"{scenario['id']:22s} {scenario['group']:7s} {scenario['kind']:7s} {scenario['seconds']:3d}s  {scenario['title']}"
        )
        print(f"{'':22s} starts at: {scenario['starts_at']}")
        print(f"{'':22s} do: {scenario['instruction']}")
        if scenario.get("prelude"):
            print(f"{'':22s} first (not recorded): {scenario['prelude']}")
        if scenario.get("skip_by_default"):
            print(f"{'':22s} NOTE: {scenario.get('note', 'skipped by default')}")
    return 0


def new_session_name(group: str) -> str:
    return f"{datetime.now().strftime('%Y%m%d-%H%M%S')}-{group}"


def check_route_only(args: argparse.Namespace, session_name: str) -> int:
    """T1618: run `tools.play --check-route` with the exact flags this session would launch, nothing is started."""
    command = play_command(args, OUT_ROOT / session_name, True)
    if args.dry_run:
        print("check command: " + shlex.join([*command, "--check-route"]))
        return 0
    return subprocess.run(  # noqa: S603
        [*command, "--check-route"], cwd=ROOT, stdin=subprocess.DEVNULL, check=False
    ).returncode


def plan_scenarios(path: Path) -> list[dict]:
    """T1627: the items of a plan file as session scenarios (steps and questions carry kind step or observe)."""
    by_id = {scenario["id"]: scenario for scenario in load_scenarios()}
    try:
        items = action_advance.parse_plan(path.read_text(), set(by_id))
    except (OSError, ValueError) as error:
        raise SystemExit(f"plan {path}: {error}") from error
    chosen: list[dict] = []
    for item in items:
        if item["item"] == "scenario":
            entry = dict(by_id[item["id"]])
            if item["label"]:
                entry["label"] = item["label"]
            chosen.append(entry)
        else:
            chosen.append({**item, "kind": item["item"]})
    return chosen


def cmd_session(args: argparse.Namespace) -> int:
    if args.check_route:
        return check_route_only(args, args.name or new_session_name(args.group))
    scenarios = plan_scenarios(args.plan) if getattr(args, "plan", None) else select_scenarios(args)
    scenarios = apply_owner_text(
        scenarios,
        getattr(args, "owner_script", None),
        parse_owner_opts(getattr(args, "owner_opt", None)),
    )
    if not hasattr(signal, "SIGUSR1"):
        print(
            "This platform has no SIGUSR1. Use one run per scenario: python -m tools.action_profile run --scenario ID --no-signals ..."
        )
        return 2
    return run_session(args, scenarios, args.name or new_session_name(args.group))


def cmd_run(args: argparse.Namespace) -> int:
    scenarios = {s["id"]: s for s in load_scenarios()}
    if args.scenario not in scenarios:
        raise SystemExit(
            f"unknown scenario {args.scenario}, see: python -m tools.action_profile list"
        )
    if args.check_route:
        return check_route_only(args, args.name or new_session_name(args.scenario))
    scenario = dict(scenarios[args.scenario])
    if args.seconds:
        scenario["seconds"] = args.seconds
    name = args.name or new_session_name(scenario["id"])
    if args.no_signals or not hasattr(signal, "SIGUSR1"):
        return run_single_whole(args, scenario, name)
    args.group = scenario["group"]
    args.only = scenario["id"]
    return run_session(args, [scenario], name)


def cmd_analyze(args: argparse.Namespace) -> int:
    from tools import action_profile_diff

    return action_profile_diff.main(
        [
            str(args.session_dir),
            "--top",
            str(args.top),
            *(["--all-frames"] if args.all_frames else []),
            *(["--host", str(args.host)] if args.host else []),
        ]
    )


def cmd_census(args: argparse.Namespace) -> int:
    from tools import action_census_diff

    return action_census_diff.main(
        [
            str(args.session_dir),
            "--top",
            str(args.top),
            *(["--functions", str(args.functions)] if args.functions else []),
        ]
    )


def cmd_compare(args: argparse.Namespace) -> int:
    from tools import action_compare

    argv = [
        *map(str, args.sessions),
        "--scenarios",
        args.scenarios,
        "--source",
        args.source,
        "--top",
        str(args.top),
    ]
    for flag, value in (("--host", args.host), ("--out", args.out)):
        if value:
            argv += [flag, str(value)]
    return action_compare.main(argv)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.action_profile",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    def common(p: argparse.ArgumentParser) -> None:
        p.add_argument(
            "--disc", type=Path, help="your Xbox disc image of the game (the game dir or .iso)"
        )
        p.add_argument(
            "--host",
            type=Path,
            help="a built tsfp_host, skips the build (default: tools.play builds one)",
        )
        p.add_argument(
            "--name", help="session folder name under tmp/owner-profiles (default: date-time-group)"
        )
        p.add_argument(
            "--wall",
            action="store_true",
            help="wall-clock sampling with call chains (200 Hz) instead of CPU sampling (997 Hz)",
        )
        p.add_argument(
            "--no-skip-intro",
            action="store_true",
            help="play the intro videos too (about 4 more minutes)",
        )
        p.add_argument(
            "--no-route-nav",
            action="store_true",
            help="T1640: with --play-arg=--replay-input FILE, replay the recorded presses of the '# nav:' steps (host --route-nav off); by default tools.play passes --route-nav on and the menu table for a record with nav steps",
        )
        p.add_argument(
            "--play-arg", action="append", default=[], help="extra flag for tools.play, repeatable"
        )
        p.add_argument(
            "--source",
            choices=SOURCES,
            default="census",
            help="T1502: census = exact indirect-call census (default), sampler = CPU samples (T1495), both",
        )
        p.add_argument("--dry-run", action="store_true", help="print what would run and stop")
        p.add_argument(
            "--check-route",
            action="store_true",
            help="T1618: with --play-arg=--replay-input FILE, only check that the route was recorded with the flags of this session (tools.play --check-route), start nothing",
        )
        p.add_argument("--play-command", help=argparse.SUPPRESS)
        p.add_argument(
            "--advance-file",
            metavar="GLOB",
            help="T1627: also continue a prompt when a NEW file matching GLOB (relative to the repository root) appears, for example tmp/owner-profiles/NAME/hotkey.* written by the host option --hotkey. Enter in the terminal keeps working. A press made before a prompt appeared does not count",
        )
        p.add_argument(
            "--advance-label",
            metavar="LABEL",
            help="T1627: only a hotkey file whose label (second word) is LABEL advances",
        )
        p.add_argument(
            "--advance-hint",
            metavar="TEXT",
            help="T1627: how the owner presses the hotkey, shown in the prompts, for example 'hold Back+Start+LB+RB on the pad, or Ctrl+Shift+N in the game window'",
        )
        p.add_argument(
            "--timers",
            choices=("auto", "on", "off"),
            default="auto",
            help="T1627: live elapsed or countdown time in the terminal, an idle phase (kind control) stops by itself at its scenario time. auto = on in a terminal, off when piped",
        )

    listing = sub.add_parser("list", help="show the scenarios")
    listing.add_argument("--group", choices=["menu", "ingame"])
    listing.set_defaults(func=cmd_list)

    session = sub.add_parser("session", help="guided session, one game run, one profile per action")
    session.add_argument("--group", choices=["menu", "ingame"], required=True)
    session.add_argument(
        "--only",
        help="comma separated scenario ids instead of the whole group; `id:label` (label [a-z0-9_]+) makes a labeled phase `id@label`, for example ingame-idle,ingame-fire:pistol,ingame-fire:shotgun,ingame-reload:shotgun (the order is kept, one session for many weapons)",
    )
    session.add_argument(
        "--plan",
        type=Path,
        metavar="FILE",
        help="T1627: an ordered plan file instead of --only: `scenario ID[:label]`, `step NAME|poke=LABEL|TEXT|AFTER` (serve DIR/guestpoke.LABEL on the running host, dump, wait), `step NAME|dump=LABEL|TEXT|AFTER`, `observe NAME|QUESTION`. One session for several forced-weapon batches",
    )
    session.add_argument(
        "--owner-script",
        metavar="SCRIPT",
        help="T1767: take the per-phase prompt text (where to be, what to do, how long, what to note) from tools/data/owner_scenarios/SCRIPT.json instead of the generic example; a phase without text there warns loudly",
    )
    session.add_argument(
        "--owner-opt",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="T1767: an option of the owner scenario (repeatable), for example mode=manual or batch=1,2",
    )
    common(session)
    session.set_defaults(func=cmd_session)

    run = sub.add_parser("run", help="a single scenario")
    run.add_argument("--scenario", required=True)
    run.add_argument("--seconds", type=int, help="override the duration shown")
    run.add_argument(
        "--no-signals",
        action="store_true",
        help="no phase marking: the whole run is the profile (platform without SIGUSR1)",
    )
    common(run)
    run.set_defaults(func=cmd_run)

    analyze = sub.add_parser("analyze", help="rank the functions per scenario")
    analyze.add_argument("session_dir", type=Path)
    analyze.add_argument("--top", type=int, default=15)
    analyze.add_argument(
        "--all-frames",
        action="store_true",
        help="wall profiles: attribute a sample to every frame of its call chain",
    )
    analyze.add_argument(
        "--host",
        type=Path,
        help="host binary used for the session (default: the one named in the profile)",
    )
    analyze.set_defaults(func=cmd_analyze)

    census = sub.add_parser(
        "census", help="T1502: rank the indirect call targets per scenario (census sessions)"
    )
    census.add_argument("session_dir", type=Path)
    census.add_argument("--top", type=int, default=15)
    census.add_argument("--functions", type=Path, help="function table export")
    census.set_defaults(func=cmd_census)

    compare = sub.add_parser(
        "compare", help="T1502: generic versus weapon-specific targets across weapon sessions"
    )
    compare.add_argument("sessions", type=Path, nargs="+")
    compare.add_argument("--scenarios", default="ingame-fire,ingame-reload,ingame-melee")
    compare.add_argument("--source", choices=["auto", "census", "sampler"], default="auto")
    compare.add_argument("--host", type=Path)
    compare.add_argument("--out", type=Path)
    compare.add_argument("--top", type=int, default=20)
    compare.set_defaults(func=cmd_compare)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
