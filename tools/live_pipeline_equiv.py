#!/usr/bin/env python3
# ruff: noqa: E501
"""T1246: run the private host on the Story replay with the live renderer serial or pipelined, and compare runs.

`run` starts the host under Xvfb on the replay record (default tmp/recorded-input-story-mode, sha256 bc589fe5...), stops it
after N swaps (--stop-after-calls 0x3D8E50:N) and keeps run.log plus frames.txt (--live-frame-hash: one sha256 per frame
handed to the window). `compare` lists, for two or more runs, the first differing frame index and the equal-frame counts, and
the guest-visible lines of the stop report that must agree. Evidence stays in gitignored tmp/. Relative paths only.
"""

from __future__ import annotations

import argparse
import itertools
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools import timing_probe  # noqa: E402

SWAP_ADDRESS = "0x3D8E50"
GUEST_LINES = (
    "dispatches counted",
    "live renderer  visibility",
    "live renderer  draws offered",
    "live renderer  OPT-IN",
    "live renderer  frame pipeline",
    "live renderer  guest thread blocked",
    "present vblank",
    "guest clock",
)


def run(args: argparse.Namespace) -> int:
    out = ROOT / args.out_dir
    out.mkdir(parents=True, exist_ok=True)
    main = timing_probe.main_root()
    current = json.loads((ROOT / "tmp/private-host/current.json").read_text())
    host = Path(args.host).resolve() if getattr(args, "host", None) else ROOT / current["host"]
    namespace = argparse.Namespace(
        scenario=args.scenario,
        disc=args.disc,
        no_gpu_live=False,
        modules=args.modules,
        extra=[],
    )
    command = timing_probe.host_command(namespace, out, main, host)
    if args.deterministic:
        # Wall clock coupled parts out: no SDL audio sink and no sampler (--interactive keeps the vblank pace).
        for flag, count in (("--audio-sink", 1), ("--present-timeline", 1)):
            while flag in command:
                at = command.index(flag)
                del command[at : at + 1 + count]
    command += [
        "--profile-calls",
        "--stop-after-calls",
        f"{SWAP_ADDRESS}:{args.swaps}",
        "--live-pipeline",
        str(args.pipeline),
        *(["--live-frame-hash", str(out / "frames.txt")] if not args.no_frame_hash else []),
        *(["--live-present-sync"] if args.present_sync else []),
        *args.extra,
    ]
    number = timing_probe.pick_display()
    display = f":{number}"
    xvfb = subprocess.Popen(
        ["Xvfb", display, "-screen", "0", "1280x720x24"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(2)
    environment = dict(os.environ, DISPLAY=display, SDL_AUDIODRIVER="disk")
    environment["SDL_AUDIO_DISK_OUTPUT_FILE"] = str(out / "audio.raw")
    (out / "hdd").mkdir(exist_ok=True)
    runtime = out / "xdg"
    runtime.mkdir(mode=0o700, exist_ok=True)
    environment["XDG_RUNTIME_DIR"] = str(runtime)
    started = time.monotonic()
    with (out / "run.log").open("wb") as log:
        process = subprocess.Popen(
            command, cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT
        )
        try:
            status = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            status = -9
        finally:
            xvfb.send_signal(signal.SIGTERM)
    wall = time.monotonic() - started
    (out / "wall.json").write_text(json.dumps({"wall_seconds": round(wall, 1), "exit": status}))
    print(f"{args.out_dir}: exit {status}, wall {wall:.1f} s")
    return 0


def load_stream(path: Path) -> list[tuple[str, str]]:
    """The presenter thread's ordered lines: F n WxH sha256 (a frame handed to the window), J n digest (the digest of what a frame
    job reads, taken at its start), Q ... (a visibility report written for the guest)."""
    stream = []
    for line in path.read_text().splitlines():
        if line.strip():
            kind, _, rest = line.partition(" ")
            stream.append((kind, rest))
    return stream


def load_frames(path: Path) -> list[str]:
    return [rest.split()[2] for kind, rest in load_stream(path) if kind == "F"]


def first_mismatch(left: list, right: list) -> int | None:
    return next((i for i in range(min(len(left), len(right))) if left[i] != right[i]), None)


def explain(left: list[tuple[str, str]], right: list[tuple[str, str]]) -> str:
    """Differential verdict. Jobs with equal input digests must yield equal outputs, so the stream (frames, reports, in presenter
    order) before the first job whose digest differs must be equal in both runs: a mismatch there is the RENDERER's. A first
    differing job is the GUEST's divergence (the guest thread schedule differs run to run, also between two serial runs)."""
    jobs_left = [(i, rest) for i, (kind, rest) in enumerate(left) if kind == "J"]
    jobs_right = [(i, rest) for i, (kind, rest) in enumerate(right) if kind == "J"]
    k = first_mismatch(
        [r.split()[1:] for _, r in jobs_left], [r.split()[1:] for _, r in jobs_right]
    )
    limit = min(len(left), len(right))
    if k is not None:
        limit = min(jobs_left[k][0], jobs_right[k][0])
    bad = first_mismatch(left[:limit], right[:limit])
    frames_before = sum(1 for kind, _ in left[:limit] if kind == "F")
    reports_before = sum(1 for kind, _ in left[:limit] if kind == "Q")
    jobs = k if k is not None else min(len(jobs_left), len(jobs_right))
    parts = ""
    if k is not None:
        names = [a.split("=")[0] for a in jobs_left[k][1].split()[1:] if "=" in a]
        differing = [
            n
            for n, a, b in zip(
                names, jobs_left[k][1].split()[1:], jobs_right[k][1].split()[1:], strict=False
            )
            if a != b
        ]
        parts = f" (parts {','.join(differing)})"
    guest = (
        "no input digest differs" if k is None else f"first input digest differs at job {k}{parts}"
    )
    renderer = (
        f"RENDERER MISMATCH at stream line {bad}: {left[bad]} vs {right[bad]}"
        if bad is not None
        else f"outputs equal before it: {frames_before} frames, {reports_before} reports, {jobs} jobs"
    )
    return f"{guest}; {renderer}"


def stop_lines(path: Path) -> dict[str, str]:
    found: dict[str, str] = {}
    for line in path.read_text(errors="replace").splitlines():
        for key in GUEST_LINES:
            if line.startswith(key):
                found[key] = line
    return found


def compare(args: argparse.Namespace) -> int:
    streams = {name: load_stream(ROOT / name / "frames.txt") for name in args.runs}
    frames = {
        name: [r.split()[2] for k, r in stream if k == "F"] for name, stream in streams.items()
    }
    for left, right in itertools.combinations(streams, 2):
        first, second = frames[left], frames[right]
        count = min(len(first), len(second))
        diff = first_mismatch(first, second)
        equal = sum(first[i] == second[i] for i in range(count))
        print(
            f"{left} vs {right}: frames {len(first)}/{len(second)}, first differing frame "
            f"{'none' if diff is None else diff}, equal {equal} of {count}; "
            + explain(streams[left], streams[right])
        )
    for name in args.runs:
        wall = (
            json.loads((ROOT / name / "wall.json").read_text())
            if (ROOT / name / "wall.json").exists()
            else {}
        )
        print(f"== {name} {wall}")
        if args.stop_lines:
            for _key, line in stop_lines(ROOT / name / "run.log").items():
                print("  " + re.sub(r"\s+", " ", line)[:300])
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)
    runner = sub.add_parser("run")
    runner.add_argument("--out-dir", required=True)
    runner.add_argument(
        "--swaps", type=int, default=3000, help="stop after N dispatches of the Swap"
    )
    runner.add_argument("--pipeline", type=int, default=0, help="--live-pipeline N (0 serial)")
    runner.add_argument(
        "--scenario", default="story-replay", choices=sorted(timing_probe.SCENARIOS)
    )
    runner.add_argument("--disc")
    runner.add_argument("--modules")
    runner.add_argument("--deterministic", action="store_true", help="no audio sink, no sampler")
    runner.add_argument(
        "--host",
        help="host binary (default: tmp/private-host/current.json), e.g. a baseline copy for a before/after comparison",
    )
    runner.add_argument(
        "--present-sync",
        action="store_true",
        help="--live-present-sync (T1262: the old wait after every present)",
    )
    runner.add_argument("--timeout", type=float, default=1500.0)
    runner.add_argument(
        "--no-frame-hash",
        action="store_true",
        help="T1267: no --live-frame-hash (it forces the readback route), for the blit route runs and --live-blit-verify",
    )
    runner.add_argument("extra", nargs="*")
    runner.set_defaults(func=run)
    comparer = sub.add_parser("compare")
    comparer.add_argument("runs", nargs="+", help="run directories relative to the repository")
    comparer.add_argument(
        "--stop-lines", action="store_true", help="also print the guest visible stop report lines"
    )
    comparer.set_defaults(func=compare)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
