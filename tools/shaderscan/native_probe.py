#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive `tsfp_shader_probe` and classify what the retained original compiler did (T200).

The probe (tests/c/shader_compiler_probe.c) is the real host boot with the opt-in retained
shader compiler, plus link-time wraps that record every compiler call, repeat it, run it from
two guest threads at once, and fail a chosen title heap allocation. This module runs it as a
subprocess and turns its JSON lines into verdicts. It never prints shader bytes, only counts,
sizes, hashes and classes.

    python -m tools.shaderscan.native_probe oom --call 1 --jobs 8
    python -m tools.shaderscan.native_probe repeat --count 5
    python -m tools.shaderscan.native_probe concurrent --rounds 20 --runs 10 --route

A boot has a known, unrelated flake (about 3 percent of boots of the plain host stop at 791
calls with a SIGSEGV before any compiler call, see docs/tasks.md). Runs that never reached a
compiler call are retried and counted separately, never classified as compiler behaviour.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from collections import Counter
from collections.abc import Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

#: The boot that reaches the compiler: six headless flags plus the opt-in.
BASE_FLAGS = (
    "--ac97-ready",
    "--headless-effects",
    "--headless-streams",
    "--headless-buffers",
    "--headless-listener",
    "--headless-first-vblank",
    "--native-shader-assembler",
)
E_OUTOFMEMORY = 0x8007000E
DEFAULT_TIMEOUT = 40
FLAKE_RETRIES = 4

#: Outcome classes of one injected allocation failure.
CLASS_E_OUTOFMEMORY = "returned E_OUTOFMEMORY"
CLASS_ERROR_OTHER = "returned another error"
CLASS_SUCCESS_SAME = "returned success, output identical to the baseline"
CLASS_SUCCESS_DIFFERENT = "returned success, output DIFFERS from the baseline"
CLASS_FAULT = "native fault (SIGSEGV)"
CLASS_THROW = "unhandled C++ throw (host stop at RtlRaiseException)"
CLASS_HANG = "hang (watchdog)"
CLASS_OTHER = "other stop"
CLASS_FLAKE = "boot flake before any compiler call"


def find_probe(explicit: Path | None = None) -> Path | None:
    """The probe binary: the given path, `TSFP_SHADER_PROBE`, or `build/tsfp_shader_probe`."""
    for candidate in (
        explicit,
        Path(os.environ["TSFP_SHADER_PROBE"]) if "TSFP_SHADER_PROBE" in os.environ else None,
        ROOT / "build" / "tsfp_shader_probe",
    ):
        if candidate is not None and candidate.is_file():
            return candidate
    return None


def find_xbe(explicit: Path | None = None) -> Path | None:
    """The retail XBE: the given path, `TSFP_XBE`, `build/default.xbe` or the oxm extract."""
    for candidate in (
        explicit,
        Path(os.environ["TSFP_XBE"]) if "TSFP_XBE" in os.environ else None,
        ROOT / "build" / "default.xbe",
        ROOT / "tmp" / "oxm-extract" / "retail" / "default.xbe",
    ):
        if candidate is not None and candidate.is_file():
            return candidate
    return None


@dataclass
class ProbeRun:
    records: list[dict] = field(default_factory=list)
    log: str = ""
    returncode: int | None = None
    timed_out: bool = False
    attempts: int = 1

    def calls(self) -> list[dict]:
        return [row for row in self.records if row.get("kind") == "call"]

    def begun(self) -> list[dict]:
        return [row for row in self.records if row.get("kind") == "begin"]

    def concurrent(self) -> dict | None:
        found = [row for row in self.records if row.get("kind") == "concurrent"]
        return found[0] if found else None

    @property
    def stop_text(self) -> str:
        return self.log.split("where each host thread stopped")[-1]

    @property
    def flake(self) -> bool:
        """No compiler call was even started: the boot died first."""
        return not self.begun()


def run_once(
    probe: Path, xbe: Path, flags: Sequence[str], timeout: float = DEFAULT_TIMEOUT
) -> ProbeRun:
    """One boot of the probe with a fresh private HDD and its own record file."""
    with tempfile.TemporaryDirectory(prefix="shader_probe_") as raw:
        scratch = Path(raw)
        hdd = scratch / "hdd"
        hdd.mkdir()
        out = scratch / "probe.jsonl"
        command = [
            str(probe),
            str(xbe),
            "--hdd",
            str(hdd),
            "--trace",
            "1",
            *BASE_FLAGS,
            "--probe-out",
            str(out),
            *map(str, flags),
        ]
        result = ProbeRun()
        try:
            completed = subprocess.run(  # noqa: S603
                command,
                capture_output=True,
                text=True,
                errors="replace",
                timeout=timeout,
                check=False,
            )
            result.returncode = completed.returncode
            result.log = completed.stdout + completed.stderr
        except subprocess.TimeoutExpired as expired:
            result.timed_out = True
            partial = expired.stdout or b""
            result.log = (
                partial.decode("utf-8", "replace") if isinstance(partial, bytes) else partial
            )
        if out.exists():
            for line in out.read_text(encoding="utf-8").splitlines():
                if line.strip():
                    result.records.append(json.loads(line))
    return result


def run_probe(
    probe: Path,
    xbe: Path,
    flags: Sequence[str] = (),
    *,
    timeout: float = DEFAULT_TIMEOUT,
    retries: int = FLAKE_RETRIES,
) -> ProbeRun:
    """Run the probe, retrying boots that died before any compiler call."""
    last = ProbeRun()
    for attempt in range(1, retries + 1):
        last = run_once(probe, xbe, flags, timeout)
        last.attempts = attempt
        if not last.flake:
            return last
    return last


def digest(hex_text: str) -> str:
    return hashlib.sha256(bytes.fromhex(hex_text)).hexdigest()


def baseline(probe: Path, xbe: Path, repeat: int = 0) -> ProbeRun:
    """A boot with no injection. Its call records are the reference for every sweep."""
    flags = ["--probe-repeat", str(repeat)] if repeat else []
    return run_probe(probe, xbe, flags)


def classify(run: ProbeRun, call_seq: int, baseline_hex: str | None) -> str:
    """Verdict for the compiler call numbered `call_seq` in a run that injected a failure."""
    if run.flake:
        return CLASS_FLAKE
    matched = [row for row in run.calls() if row["seq"] == call_seq]
    if matched:
        call = matched[0]
        if call["hresult"] == E_OUTOFMEMORY:
            return CLASS_E_OUTOFMEMORY
        if call["hresult"] != 0:
            return CLASS_ERROR_OTHER
        output = call.get("output")
        if output is None:
            return CLASS_OTHER
        return CLASS_SUCCESS_SAME if output["hex"] == baseline_hex else CLASS_SUCCESS_DIFFERENT
    if run.timed_out or "STILL RUNNING" in run.log:
        return CLASS_HANG
    if "RtlRaiseException" in run.stop_text:
        return CLASS_THROW
    if "host fault" in run.stop_text:
        return CLASS_FAULT
    return CLASS_OTHER


def inject(probe: Path, xbe: Path, call_seq: int, allocation: int) -> ProbeRun:
    """Fail the `allocation`th title heap allocation of compiler call `call_seq`."""
    return run_probe(
        probe,
        xbe,
        [
            "--probe-fail-call",
            str(call_seq),
            "--probe-fail-alloc",
            str(allocation),
            "--probe-exit-after",
            str(call_seq),
        ],
    )


def oom_sweep(
    probe: Path, xbe: Path, call_seq: int, allocations: Sequence[int], jobs: int = 8
) -> dict[int, str]:
    """Class of every injected failure, keyed by allocation number."""
    reference = [row for row in baseline(probe, xbe).calls() if row["seq"] == call_seq]
    if not reference or reference[0]["output"] is None:
        raise RuntimeError(f"baseline has no successful compiler call {call_seq}")
    reference_hex = reference[0]["output"]["hex"]
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        runs = list(pool.map(lambda n: inject(probe, xbe, call_seq, n), allocations))
    return {
        n: classify(run, call_seq, reference_hex) for n, run in zip(allocations, runs, strict=True)
    }


def injected_call(run: ProbeRun, call_seq: int) -> dict | None:
    matched = [row for row in run.calls() if row["seq"] == call_seq]
    return matched[0] if matched else None


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument("--probe", type=Path, help="tsfp_shader_probe (default: build/)")
    parser.add_argument("--xbe", type=Path, help="the retail default.xbe (default: build/)")
    sub = parser.add_subparsers(dest="command", required=True)
    oom = sub.add_parser("oom", help="fail each title allocation of one compiler call in turn")
    oom.add_argument("--call", type=int, default=1, help="compiler call number (default 1)")
    oom.add_argument("--jobs", type=int, default=8)
    oom.add_argument("--first", type=int, default=1)
    oom.add_argument("--last", type=int, default=0, help="default: every allocation of the call")
    repeat = sub.add_parser("repeat", help="repeat each compiler call on the same frame")
    repeat.add_argument("--count", type=int, default=5)
    concurrent = sub.add_parser("concurrent", help="two guest threads compile at once")
    concurrent.add_argument("--rounds", type=int, default=20)
    concurrent.add_argument("--runs", type=int, default=10)
    concurrent.add_argument("--jobs", type=int, default=4)
    concurrent.add_argument(
        "--route", action="store_true", help="enter through the production route and its lock"
    )
    concurrent.add_argument(
        "--serial", action="store_true", help="control: a probe mutex serialises the threads"
    )
    args = parser.parse_args(argv)
    probe, xbe = find_probe(args.probe), find_xbe(args.xbe)
    if probe is None or xbe is None:
        print(f"missing {'probe' if probe is None else 'xbe'}; build tsfp_shader_probe first")
        return 2

    if args.command == "oom":
        reference = [row for row in baseline(probe, xbe).calls() if row["seq"] == args.call]
        if not reference:
            print("baseline made no such compiler call")
            return 1
        last = args.last or reference[0]["allocs"]
        classes = oom_sweep(probe, xbe, args.call, range(args.first, last + 1), args.jobs)
        for name, count in Counter(classes.values()).most_common():
            print(f"{count:4d}  {name}")
        for name in sorted(set(classes.values())):
            members = [n for n, c in classes.items() if c == name]
            print(f"  {name}: {members[:12]}{' ...' if len(members) > 12 else ''}")
        return 0
    if args.command == "repeat":
        run = baseline(probe, xbe, args.count)
        for call in run.calls():
            repeats = call["repeats"]
            stable = all(r["same_output"] and r["same_hresult"] for r in repeats)
            print(
                f"call {call['seq']}: size {call['size']} hresult {call['hresult']:#x} "
                f"repeats {len(repeats)} stable {stable} "
                f"growth {sorted({r['outstanding'] for r in repeats})}"
            )
        return 0
    flags = ["--probe-concurrent", str(args.rounds)]
    if args.route:
        flags += ["--probe-concurrent-route", "1"]
    if args.serial:
        flags += ["--probe-concurrent-serial", "1"]
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        runs = list(pool.map(lambda _: run_probe(probe, xbe, flags), range(args.runs)))
    tally: Counter[str] = Counter()
    for run in runs:
        record = run.concurrent()
        if record is not None:
            tally[
                f"record: aborted={record['aborted']} mismatches={record['mismatches']} "
                f"overlapping_rounds>0={record['overlapping_rounds'] > 0} "
                f"max_inside={record['max_inside']}"
            ] += 1
        elif run.timed_out or "STILL RUNNING" in run.log:
            tally[CLASS_HANG] += 1
        elif "RtlRaiseException" in run.stop_text:
            tally[CLASS_THROW] += 1
        elif "host fault" in run.stop_text:
            tally[CLASS_FAULT] += 1
        else:
            tally[CLASS_OTHER] += 1
    for name, count in tally.most_common():
        print(f"{count:4d}  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
