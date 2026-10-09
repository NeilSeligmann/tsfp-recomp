#!/usr/bin/env python3
# ruff: noqa: E501
"""T1235/T1236/T1237: measure the private host's timing model (game speed, stutter, audio latency).

Runs the private host (`tmp/private-host/current.json`) under Xvfb with the interactive flag set of
`python -m tools.play ... --window --gpu-live --gpu-live-inferred --present window`, the SDL audio sink on the
disk driver (the PCM the device would play) and the host's own 25 ms sampler (`--present-timeline`). It then
reduces the CSV to windows of wall time:

  speed       modelled (guest clock) ms advanced per wall ms: 1.0 is real time, under 1 slow motion, over 1 too fast
  ring        audio ring fill in ms (the queue between the guest's mixer and the device: the audio latency the
              host adds, the device buffer comes on top)
  lead        written minus played ms (audio the guest produced that the device has not played)
  playing     fraction of samples in which the sink was playing (0 = rebuffering silence)
  cpu         the busiest threads by CPU ticks (1/100 s) in the window

Nothing here forces state: it only reads the sampler file. Relative paths only, evidence in gitignored tmp/.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SAMPLE_RATE = 48000

SCENARIOS = {
    "story": {"replay": None, "pad": "tmp/t1204-evidence/story-controllable.pad"},
    "story-replay": {"replay": "tmp/recorded-input-story-mode", "pad": None},
    "mapmaker": {"replay": "tmp/recorded-input-mapmaker", "pad": None},
    "mapmaker-edit": {"replay": "tmp/recorded-input-mapmaker-edit", "pad": None},
}


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def reduce_timeline(path: Path, window_s: float) -> dict:
    """Window summaries of the sampler CSV (columns wall_ms, modelled_ms, ring_fill, written_ms, played_ms,
    pictures_queued, playing, threads)."""
    rows = []
    with path.open() as handle:
        reader = csv.reader(handle)
        next(reader, None)
        for row in reader:
            if len(row) < 7:
                continue
            try:
                rows.append(
                    {
                        "wall": int(row[0]),
                        "model": int(row[1]),
                        "fill": int(row[2]),
                        "written": int(row[3]),
                        "played": int(row[4]),
                        "queued": int(row[5]),
                        "playing": int(row[6]),
                        "threads": row[7:] if len(row) > 7 else [],
                    }
                )
            except ValueError:
                continue
    windows = []
    step = int(window_s * 1000)
    start = 0
    while rows and start <= rows[-1]["wall"]:
        chunk = [r for r in rows if start <= r["wall"] < start + step]
        start += step
        if len(chunk) < 2:
            continue
        wall = chunk[-1]["wall"] - chunk[0]["wall"]
        model = chunk[-1]["model"] - chunk[0]["model"]
        cpu: dict[str, int] = {}
        for r in chunk:
            for item in " ".join(r["threads"]).split():
                if ":" in item:
                    tid, ticks = item.split(":", 1)
                    if ticks.isdigit():
                        cpu[tid] = cpu.get(tid, 0) + int(ticks)
        top = sorted(cpu.items(), key=lambda pair: -pair[1])[:3]
        windows.append(
            {
                "wall_s": round(chunk[0]["wall"] / 1000.0, 1),
                "speed": round(model / wall, 3) if wall else 0.0,
                "ring_ms_mean": round(
                    sum(r["fill"] for r in chunk) / len(chunk) * 1000 / SAMPLE_RATE, 0
                ),
                "ring_ms_max": round(max(r["fill"] for r in chunk) * 1000 / SAMPLE_RATE, 0),
                "lead_ms_mean": round(
                    sum(r["written"] - r["played"] for r in chunk) / len(chunk), 0
                ),
                "playing": round(sum(r["playing"] for r in chunk) / len(chunk), 2),
                "cpu_top": top,
            }
        )
    speeds = [w["speed"] for w in windows if w["wall_s"] >= 5]
    fills = [r["fill"] * 1000.0 / SAMPLE_RATE for r in rows if r["wall"] >= 5000 and r["playing"]]
    guest_gaps = []
    previous = None
    for r in rows:
        if previous is not None and r["model"] == previous["model"]:
            guest_gaps.append(r["wall"] - previous["wall"])
        previous = r
    return {
        "samples": len(rows),
        "windows": windows,
        "speed_mean": round(sum(speeds) / len(speeds), 3) if speeds else 0.0,
        "speed_min": min(speeds) if speeds else 0.0,
        "speed_max": max(speeds) if speeds else 0.0,
        "ring_ms_p50": round(percentile(fills, 0.5), 0),
        "ring_ms_p95": round(percentile(fills, 0.95), 0),
        "ring_ms_max": round(max(fills), 0) if fills else 0.0,
        "playing_fraction": round(sum(r["playing"] for r in rows) / len(rows), 3) if rows else 0.0,
        "model_flat_samples": len(guest_gaps),
    }


def sample_states(pid: int, seconds: float, interval: float) -> dict:
    """Thread state sampling from /proc (no ptrace): per thread the share of samples in state R (running) and the
    wait channel of the others, so a thread that is alternately blocked on another shows up."""
    counts: dict[str, dict[str, int]] = {}
    names: dict[str, str] = {}
    end = time.monotonic() + seconds
    total = 0
    while time.monotonic() < end:
        total += 1
        try:
            tids = os.listdir(f"/proc/{pid}/task")
        except OSError:
            break
        for tid in tids:
            try:
                stat = Path(f"/proc/{pid}/task/{tid}/stat").read_text()
                state = stat[stat.rindex(")") + 2]
                names[tid] = stat[stat.index("(") + 1 : stat.rindex(")")]
                wchan = (
                    Path(f"/proc/{pid}/task/{tid}/wchan").read_text().strip()
                    if state != "R"
                    else ""
                )
            except OSError:
                continue
            key = state if state == "R" else f"{state}:{wchan or '-'}"
            counts.setdefault(tid, {})
            counts[tid][key] = counts[tid].get(key, 0) + 1
        time.sleep(interval)
    out = {}
    for tid, states in counts.items():
        if sum(states.values()) < total // 4:
            continue
        out[f"{tid}/{names[tid]}"] = {
            k: round(v / total, 3) for k, v in sorted(states.items(), key=lambda p: -p[1])[:4]
        }
    return {"samples": total, "threads": out}


def main_root() -> Path:
    common = subprocess.run(
        ["git", "rev-parse", "--git-common-dir"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=30,
    )
    return (ROOT / common.stdout.strip()).resolve().parent


def pick_display() -> int:
    for number in range(200, 260):
        if (
            not Path(f"/tmp/.X{number}-lock").exists()
            and not Path(f"/tmp/.X11-unix/X{number}").exists()
        ):
            return number
    raise SystemExit("no free X display")


def host_command(args: argparse.Namespace, out: Path, main: Path, host: Path) -> list[str]:
    scenario = SCENARIOS[args.scenario]
    disc = (
        Path(args.disc)
        if args.disc
        else main / "tmp/TimeSplitters - Future Perfect (USA) (XBOX).iso"
    )
    live = [] if args.no_gpu_live else ["--gpu-live", "--gpu-live-inferred"]
    translate = [] if args.no_gpu_live else ["--gpu-live-translate"]
    command = [
        str(host),
        "build/default.xbe",
        "--hdd",
        str(out / "hdd"),
        "--xonline-offline",
        "--skip-intro",
        "--ac97-ready",
        "--headless-streams",
        "--headless-buffers",
        "--headless-listener",
        "--headless-second-vblank",
        "--native-shader-assembler",
        "--native-xmv",
        "--headless-movie-audio",
        "--couple-vblank-effects",
        "--check-vblank-quiescence",
        "--overlay-consume",
        *live,
        "--interactive",
        "--thread-timeout",
        "2147483647",
        "--vblank-owner-waits",
        "1000",
        "--vblank-worker-blanks",
        "100",
        "--present",
        "window",
        "--present-hold-ms",
        "0",
        "--audio-sink",
        "sdl",
        "--synthetic-pad",
        "--present-timeline",
        str(out / "timeline.csv"),
        "--gpu-replay",
        args.modules or str(main / "tmp/play/modules"),
        "--gpu-replay-lenient",
        *translate,
        "--disc",
        str(disc),
    ]
    if scenario["replay"]:
        command += ["--replay-input", str(main / scenario["replay"])]
    else:
        command += ["--pad-source", "script", "--pad-script", str(main / scenario["pad"])]
    command += args.extra
    return command


def close_window(display: str) -> str:
    sys.path.insert(0, str(ROOT))
    from tools.t982_acceptance import send_close

    return send_close(display)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--scenario", choices=sorted(SCENARIOS), default="story")
    parser.add_argument(
        "--seconds", type=float, default=60.0, help="wall seconds before the window close request"
    )
    parser.add_argument("--out-dir", required=True, help="evidence directory (gitignored tmp/)")
    parser.add_argument("--window", type=float, default=5.0, help="reduction window in seconds")
    parser.add_argument(
        "--sample-states",
        type=float,
        default=0.0,
        help="seconds of /proc thread state sampling from 20 s on",
    )
    parser.add_argument(
        "--modules", help="shader module directory (an EMPTY directory measures a cold cache)"
    )
    parser.add_argument(
        "--no-gpu-live", action="store_true", help="control: no live renderer (host CPU only)"
    )
    parser.add_argument("--disc", help="XBOX ISO (default: main checkout tmp)")
    parser.add_argument(
        "--host", help="host binary (default: tmp/private-host/current.json), e.g. a baseline copy"
    )
    parser.add_argument("--close-grace", type=float, default=60.0)
    parser.add_argument("extra", nargs="*", help="more host arguments")
    args = parser.parse_args()

    out = ROOT / args.out_dir
    out.mkdir(parents=True, exist_ok=True)
    main = main_root()
    current = json.loads((ROOT / "tmp/private-host/current.json").read_text())
    host = Path(args.host).resolve() if args.host else ROOT / current["host"]
    number = pick_display()
    display = f":{number}"
    xvfb = subprocess.Popen(
        ["Xvfb", display, "-screen", "0", "1280x720x24"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(2)
    env = dict(
        os.environ,
        DISPLAY=display,
        SDL_AUDIODRIVER="disk",
        SDL_AUDIO_DISK_OUTPUT_FILE=str(out / "audio.raw"),
        SDL_AUDIO_DISK_TIMESCALE="1.0",
    )
    (out / "hdd").mkdir(exist_ok=True)
    runtime = out / "xdg"
    runtime.mkdir(mode=0o700, exist_ok=True)
    env["XDG_RUNTIME_DIR"] = str(runtime)
    log = (out / "run.log").open("wb")
    started = time.monotonic()
    proc = subprocess.Popen(
        host_command(args, out, main, host), cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT
    )
    close_sent = None
    states = {}
    try:
        while proc.poll() is None:
            elapsed = time.monotonic() - started
            if args.sample_states and not states and elapsed >= 20.0:
                states = sample_states(proc.pid, args.sample_states, 0.02)
            if elapsed >= args.seconds and close_sent is None:
                close_window(display)
                close_sent = time.monotonic()
            if close_sent is not None and time.monotonic() - close_sent > args.close_grace:
                proc.kill()
                break
            time.sleep(0.5)
        status = proc.wait(timeout=60)
    finally:
        if proc.poll() is None:
            proc.kill()
        xvfb.send_signal(signal.SIGTERM)
        log.close()
    summary = (
        reduce_timeline(out / "timeline.csv", args.window)
        if (out / "timeline.csv").exists()
        else {}
    )
    summary.update(
        {
            "scenario": args.scenario,
            "gpu_live": not args.no_gpu_live,
            "host": str(host.name),
            "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(),
            "exit_status": status,
            "wall_seconds": round(time.monotonic() - started, 1),
        }
    )
    summary["thread_states"] = states
    (out / "summary.json").write_text(json.dumps(summary, indent=1))
    printable = {key: value for key, value in summary.items() if key != "windows"}
    print(json.dumps(printable, indent=1))
    for window in summary.get("windows", []):
        print(
            f"t={window['wall_s']:6.1f}s speed {window['speed']:.2f} ring {window['ring_ms_mean']:5.0f}/"
            f"{window['ring_ms_max']:5.0f} ms lead {window['lead_ms_mean']:6.0f} ms playing {window['playing']:.2f} "
            f"cpu {window['cpu_top']}"
        )
    return 0 if status == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
