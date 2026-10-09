#!/usr/bin/env python3
# ruff: noqa: E501
"""T1248: find audio dropouts in a captured sink PCM (SDL disk driver, `audio.raw`, 48 kHz stereo S16 LE).

A dropout is a run of digital silence (every sample within `--threshold` LSB) of at least `--min-ms`.
Per 100 ms window the tool reports the peak, so all-voices-silent gaps show up as runs of zero windows. A
`--control-tone` run synthesises a continuous tone, pushes it through the same analysis and must report 0
dropouts (and a tone with one injected gap must report exactly that gap): the analysis is validated, not the host.
`--log run.log` correlates with the stop report (`audio clock`, `audio stall`, `audio cutouts` lines).
Relative paths only, nothing is written unless `--json` is given.
"""

from __future__ import annotations

import argparse
import array
import json
import math
import re
import sys
from pathlib import Path

RATE = 48000
CHANNELS = 2


def load_samples(path: Path) -> array.array:
    data = array.array("h")
    raw = path.read_bytes()
    raw = raw[: len(raw) // (2 * CHANNELS) * 2 * CHANNELS]
    data.frombytes(raw)
    if sys.byteorder == "big":
        data.byteswap()
    return data


def frame_peaks(samples: array.array, frames_per_bin: int) -> list[int]:
    """Peak absolute sample per bin of `frames_per_bin` frames."""
    peaks = []
    step = frames_per_bin * CHANNELS
    for start in range(0, len(samples) - step + 1, step):
        chunk = samples[start : start + step]
        peaks.append(max(max(chunk), -min(chunk)))
    return peaks


def find_dropouts(
    samples: array.array, threshold: int, min_ms: float, bin_ms: float = 5.0
) -> list[dict]:
    """Runs of silent bins (peak <= threshold) of at least min_ms. Resolution is bin_ms."""
    bin_frames = max(1, int(RATE * bin_ms / 1000))
    peaks = frame_peaks(samples, bin_frames)
    runs = []
    start = None
    for index, peak in enumerate(peaks + [threshold + 1]):
        if peak <= threshold:
            if start is None:
                start = index
        elif start is not None:
            length_ms = (index - start) * bin_ms
            if length_ms >= min_ms:
                runs.append({"start_s": round(start * bin_ms / 1000, 3), "ms": round(length_ms, 1)})
            start = None
    return runs


def window_table(samples: array.array, window_ms: int) -> list[int]:
    return frame_peaks(samples, RATE * window_ms // 1000)


def summarise(
    samples: array.array, threshold: int, min_ms: float, window_ms: int, skip_s: float
) -> dict:
    skip = int(skip_s * RATE) * CHANNELS
    body = samples[skip:]
    runs = find_dropouts(body, threshold, min_ms)
    # T1731: report times on the capture clock, not relative to the skipped prefix.
    for run in runs:
        run["start_s"] = round(run["start_s"] + skip_s, 3)
    windows = window_table(body, window_ms)
    silent = sum(1 for peak in windows if peak <= threshold)
    total_ms = sum(run["ms"] for run in runs)
    buckets = {"<250": 0, "<500": 0, "<1000": 0, "<2000": 0, ">=2000": 0}
    for run in runs:
        key = (
            "<250"
            if run["ms"] < 250
            else "<500"
            if run["ms"] < 500
            else "<1000"
            if run["ms"] < 1000
            else "<2000"
            if run["ms"] < 2000
            else ">=2000"
        )
        buckets[key] += 1
    seconds = len(body) / CHANNELS / RATE
    first_sound = next((i for i, peak in enumerate(windows) if peak > threshold), None)
    return {
        "capture_seconds": round(len(samples) / CHANNELS / RATE, 1),
        "peak": max((max(body), -min(body)), default=0) if len(body) else 0,
        "leading_silence_s": None
        if first_sound is None
        else round(skip_s + first_sound * window_ms / 1000, 3),
        "seconds": round(seconds, 1),
        "skip_s": skip_s,
        "windows": len(windows),
        "silent_windows": silent,
        "silent_fraction": round(silent / len(windows), 3) if windows else 0.0,
        "dropouts": len(runs),
        "dropout_ms_total": round(total_ms, 1),
        "dropout_ms_longest": max((run["ms"] for run in runs), default=0.0),
        "dropouts_per_minute": round(len(runs) * 60 / seconds, 2) if seconds else 0.0,
        "histogram_ms": buckets,
        "runs": runs,
    }


def parse_log(path: Path) -> dict:
    text = path.read_text(errors="replace")
    out: dict = {"stalls": []}
    match = re.search(
        r"audio clock\s+rebuffers (\d+), silence while rebuffering (\d+) frames", text
    )
    if match:
        out["rebuffers"] = int(match.group(1))
        out["rebuffer_ms"] = round(int(match.group(2)) * 1000 / RATE)
    for match in re.finditer(r"audio stall\s+#\d+ (\w+) at wall (\d+) ms.*?lasted (\d+) ms", text):
        out["stalls"].append(
            {"kind": match.group(1), "wall_ms": int(match.group(2)), "ms": int(match.group(3))}
        )
    match = re.search(r"audio cutouts\s+(.*)", text)
    if match:
        out["cutouts_line"] = match.group(0)
        played = re.search(r"of the ([\d.]+) s the device played", match.group(0))
        if played:
            out["device_played_s"] = float(played.group(1))
    return out


def tone(seconds: float, gaps: list[tuple[float, float]]) -> array.array:
    data = array.array("h")
    for index in range(int(seconds * RATE)):
        time_s = index / RATE
        value = (
            0
            if any(a <= time_s < a + b for a, b in gaps)
            else int(8000 * math.sin(2 * math.pi * 440 * time_s))
        )
        data.extend((value, value))
    return data


def control(threshold: int, min_ms: float) -> dict:
    clean = summarise(tone(3.0, []), threshold, min_ms, 100, 0.0)
    gapped = summarise(tone(3.0, [(1.0, 0.4)]), threshold, min_ms, 100, 0.0)
    return {
        "clean_dropouts": clean["dropouts"],
        "gapped_dropouts": gapped["dropouts"],
        "gapped_ms": gapped["dropout_ms_total"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("pcm", nargs="?", help="captured audio.raw (48 kHz stereo S16 LE)")
    parser.add_argument("--log", help="the run's stop report text (run.log) to correlate")
    parser.add_argument(
        "--threshold", type=int, default=2, help="silence: peak at most this many LSB"
    )
    parser.add_argument(
        "--min-ms", type=float, default=100.0, help="shortest gap that is a dropout"
    )
    parser.add_argument("--window-ms", type=int, default=100)
    parser.add_argument(
        "--skip-s", type=float, default=0.0, help="ignore the first seconds (boot silence)"
    )
    parser.add_argument(
        "--control-tone", action="store_true", help="validate the analysis on a synthetic tone"
    )
    parser.add_argument("--json", help="write the summary here")
    args = parser.parse_args()
    if args.control_tone:
        result = control(args.threshold, args.min_ms)
        print(json.dumps(result))
        return 0 if result["clean_dropouts"] == 0 and result["gapped_dropouts"] == 1 else 1
    if not args.pcm:
        parser.error("pcm is required without --control-tone")
    summary = summarise(
        load_samples(Path(args.pcm)), args.threshold, args.min_ms, args.window_ms, args.skip_s
    )
    if args.log:
        summary["log"] = parse_log(Path(args.log))
        played = summary["log"].get("device_played_s")
        if played is not None:
            # the host's played time against the captured length: a large gap means a truncated capture
            summary["log"]["capture_minus_played_s"] = round(summary["capture_seconds"] - played, 1)
    printable = {key: value for key, value in summary.items() if key != "runs"}
    print(json.dumps(printable, indent=1))
    print(
        "longest runs (start s, ms):",
        [(r["start_s"], r["ms"]) for r in sorted(summary["runs"], key=lambda r: -r["ms"])[:10]],
    )
    if args.json:
        Path(args.json).write_text(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
