# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Round-trip validation of a snapshot (T1153): the resumed run reaches the same stop as the straight run.

    python -m tools.snapshot roundtrip --host H --disc ISO --replay-input RECORD --runs 3

1. A straight replay (the private-host form, xvfb-run, no snapshot) gives the stop, the census, the last
   guest calls and the polls the run consumed.
2. `tools.play --snapshot-at-poll N --snapshot-continue` takes a snapshot N polls from the stop (N = polls
   consumed minus --margin unless --at-poll) and lets that run continue to its own stop.
3. `tools.play --resume-snapshot` runs the snapshot to its stop --runs times.

Equal means: the same STOP line and detail and the same last guest calls in every run, and a census whose
spread over all runs stays inside --tolerance (the straight runs of one build differ by about 0.2 percent
between themselves because host thread timing is not deterministic, so exact census equality is NOT claimed).
Exit 0 only when every run is equal and every run produced a stop. A title that no longer stops by itself
(the frontier moved past the old stop) is cut deterministically with --stop-at-poll M, which passes the host
`--stop-at-poll M` to every run: the stop is then "poll limit M of port 0 reached".
"""

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

from tools.private_host import parse_stop, private_env, record_flags
from tools.snapshot import options

ROOT = Path(__file__).resolve().parents[2]
CENSUS_TOTAL = re.compile(r"(\d+) total")
RUN_TIMEOUT = 900


def census_total(text: str) -> int | None:
    found = CENSUS_TOTAL.search(text)
    return int(found.group(1)) if found else None


def play_prefix(args: argparse.Namespace) -> list[str]:
    """The tools.play arguments of a replay, as `private_host replay` builds them."""
    cut = ["--stop-at-poll", str(args.stop_at_poll)] if args.stop_at_poll else []
    return [
        sys.executable,
        "-m",
        "tools.play",
        "--window",
        "--gpu-live",
        "--gpu-live-inferred",
        "--present",
        "window",
        "--mute",
        "--disc",
        str(args.disc),
        *record_flags(args.replay_input),
        "--replay-input",
        str(args.replay_input),
        "--replay-interactive",
        "--host",
        str(args.host),
        *cut,
    ]


def run_logged(argv: list[str], log: Path, timeout: int) -> tuple[int, float]:
    started = time.monotonic()
    with log.open("wb") as handle:
        try:
            done = subprocess.run(
                argv,
                cwd=ROOT,
                env=private_env(True),
                stdin=subprocess.DEVNULL,
                stdout=handle,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            )
            code = done.returncode
        except subprocess.TimeoutExpired:
            code = -9
    return code, time.monotonic() - started


def observation(label: str, run_dir: Path, seconds: float, code: int) -> dict[str, object]:
    found = parse_stop(run_dir)
    return {
        "label": label,
        "exit": code,
        "stop": found["stop"],
        "detail": found["detail"],
        "census": census_total(found["census"]),
        "calls": found["calls"],
        "seconds": round(seconds, 1),
        "run_dir": str(run_dir),
    }


def judge(runs: list[dict[str, object]], tolerance: float) -> dict[str, object]:
    """Same stop and calls everywhere, census spread inside the tolerance."""
    problems: list[str] = []
    for run in runs:
        if run["stop"] == "none" or run["census"] is None:
            problems.append(f"{run['label']}: no stop report")
    present = [run for run in runs if run["stop"] != "none" and run["census"] is not None]
    for key in ("stop", "detail", "calls"):
        values = {str(run[key]) for run in present}
        if len(values) > 1:
            problems.append(f"{key} differs between runs: {sorted(values)}")
    censuses = [int(str(run["census"])) for run in present]
    spread = (max(censuses) - min(censuses)) / max(censuses) if censuses else 1.0
    if spread > tolerance:
        problems.append(f"census spread {spread:.4%} exceeds the tolerance {tolerance:.2%}")
    return {
        "equal": not problems and bool(present),
        "problems": problems,
        "census_spread": round(spread, 6),
        "census_identical": len(set(censuses)) == 1 and bool(censuses),
    }


def render(runs: list[dict[str, object]], verdict: dict[str, object]) -> str:
    lines = ["label | stop | census | seconds", "--- | --- | --- | ---"]
    for run in runs:
        stop = str(run["stop"])[:70]
        lines.append(f"{run['label']} | {stop} | {run['census']} | {run['seconds']}")
    lines.append(f"last guest calls: {runs[0]['calls']}")
    lines.append(
        f"equal: {verdict['equal']}, census spread {verdict['census_spread']:.4%}, "
        f"census identical: {verdict['census_identical']}"
    )
    lines += [f"PROBLEM: {problem}" for problem in verdict["problems"]]  # type: ignore[union-attr]
    return "\n".join(lines)


def run_roundtrip(args: argparse.Namespace) -> int:
    out = args.out or Path("tmp/snapshots") / f"roundtrip-{time.strftime('%Y%m%d-%H%M%S')}"
    out = (ROOT / out).resolve() if not out.is_absolute() else out
    out.mkdir(parents=True, exist_ok=True)
    base = play_prefix(args)
    straight_dir = out / "straight"
    code, seconds = run_logged(
        ["xvfb-run", "-a", *base, "--run-dir", str(straight_dir), "--timeout", str(RUN_TIMEOUT)],
        out / "straight.log",
        RUN_TIMEOUT + 120,
    )
    runs = [observation("straight", straight_dir, seconds, code)]
    consumed = options.polls_consumed(straight_dir)
    poll = args.at_poll or (max(1, consumed - args.margin) if consumed else None)
    if poll is None:
        print(
            "roundtrip: the straight run reported no consumed polls, give --at-poll",
            file=sys.stderr,
        )
        return 2
    snapshot_dir, run_dir = out / "snapshot", out / "snapshot-run"
    code, seconds = run_logged(
        [
            *base,
            "--run-dir",
            str(run_dir),
            "--timeout",
            str(RUN_TIMEOUT),
            "--snapshot-at-poll",
            str(poll),
            "--snapshot-continue",
            "--snapshot-dir",
            str(snapshot_dir),
        ],
        out / "take.log",
        RUN_TIMEOUT + 300,
    )
    if code != 0 or not (snapshot_dir / "manifest.json").is_file():
        print(
            f"roundtrip: the snapshot was not taken (exit {code}), see {out / 'take.log'}",
            file=sys.stderr,
        )
        return 1
    runs.append(observation("take-run (continued)", run_dir, seconds, code))
    for index in range(1, args.runs + 1):
        code, seconds = run_logged(
            [
                sys.executable,
                "-m",
                "tools.play",
                "--resume-snapshot",
                str(snapshot_dir),
                "--resume-timeout",
                str(RUN_TIMEOUT),
            ],
            out / f"resume-{index}.log",
            RUN_TIMEOUT + 300,
        )
        runs.append(observation(f"resume {index}", run_dir, seconds, code))
    verdict = judge(runs, args.tolerance)
    report = {"poll": poll, "consumed_by_straight": consumed, "runs": runs, "verdict": verdict}
    (out / "roundtrip.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"roundtrip: snapshot at poll {poll} of {consumed}, output {out}")
    print(render(runs, verdict))
    return 0 if verdict["equal"] else 1


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--host", type=Path, required=True, help="private host binary")
    parser.add_argument("--disc", type=Path, required=True)
    parser.add_argument("--replay-input", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=3, help="resumed runs (default 3)")
    parser.add_argument(
        "--stop-at-poll",
        type=int,
        help="host --stop-at-poll M: a deterministic cut for a title that no longer stops by itself "
        "(the snapshot is then taken at M minus --margin)",
    )
    parser.add_argument(
        "--at-poll", type=int, help="snapshot poll (default: stop polls minus margin)"
    )
    parser.add_argument("--margin", type=int, default=options.DEFAULT_MARGIN)
    parser.add_argument(
        "--tolerance", type=float, default=0.01, help="census spread (default 1 percent)"
    )
    parser.add_argument(
        "--out", type=Path, help="output directory (default tmp/snapshots/roundtrip-*)"
    )
