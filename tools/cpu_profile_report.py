#!/usr/bin/env python3
# ruff: noqa: E501
"""T1235: summarise a `--cpu-profile FILE` of the host (src/host/cpu_sampler.c).

The file holds `# base <load address> <exe>` and one `tid rip` line per SIGPROF sample (process CPU time, about
997 Hz). The instruction pointers are resolved with addr2line (-f -C, the PIE load address subtracted) into the
self time per thread and per function. A sample outside the executable (a shared library such as the Vulkan driver,
libc) is attributed to its mapping name when --maps is given, else to `[other]`.

Usage: python -m tools.cpu_profile_report FILE [--top 25] [--window A:B] [--tid N]
(T1250 gaps: the host also writes FILE.ms, one CLOCK_MONOTONIC ms per sample, so --window selects a time range, CPU mode only.)
"""

from __future__ import annotations

import argparse
import collections
import subprocess
import sys
from pathlib import Path


def load(path: Path) -> tuple[int, str, list[tuple[int, int]]]:
    """Samples as (tid, innermost rip); `chains()` returns the full wall mode chains."""
    base = 0
    exe = ""
    samples: list[tuple[int, int]] = []
    for line in path.read_text().splitlines():
        if line.startswith("# base "):
            parts = line.split(" ", 3)
            base = int(parts[2], 16)
            exe = parts[3]
        elif line and not line.startswith("#"):
            fields = line.split()
            if len(fields) >= 2:
                samples.append((int(fields[0]), int(fields[1], 16)))
    return base, exe, samples


def maps_of(path: Path) -> list[tuple[int, int, int, str]]:
    """`# map START END OFFSET PATH` lines (T1289): the executable library mappings of the profiled process."""
    result = []
    for line in path.read_text().splitlines():
        if line.startswith("# map "):
            parts = line.split(" ", 5)
            result.append((int(parts[2], 16), int(parts[3], 16), int(parts[4], 16), parts[5]))
    return result


def library_name(maps: list[tuple[int, int, int, str]], rip: int) -> str:
    """Library file and the nearest exported symbol below the address (internal symbols of a stripped library are not exported)."""
    for start, end, offset, name in maps:
        if start <= rip < end:
            return f"{Path(name).name}+{rip - start + offset:#x}"
    return "[other]"


def chains(path: Path) -> list[tuple[int, list[int]]]:
    result = []
    for line in path.read_text().splitlines():
        if line and not line.startswith("#"):
            fields = line.split()
            result.append((int(fields[0]), [int(value, 16) for value in fields[1:]]))
    return result


def wall_report(path: Path, base: int, exe: str, top: int) -> int:
    """Wall mode: per thread the share of samples by the first three frames inside the executable."""
    size = Path(exe).stat().st_size if exe and Path(exe).exists() else 0
    rows = chains(path)
    limit = max(size * 4, 1 << 28)
    wanted = [rip - base - 1 for _, chain in rows for rip in chain if 0 <= rip - base < limit]
    names = resolve(exe, wanted)
    per_thread: dict[int, collections.Counter[str]] = collections.defaultdict(collections.Counter)
    totals: collections.Counter[int] = collections.Counter(tid for tid, _ in rows)
    for tid, chain in rows:
        frames = [
            names[rip - base - 1]
            for rip in chain
            if 0 <= rip - base < limit and rip - base - 1 in names
        ]
        key = " <- ".join(frames[:3]) if frames else "(no frame in the executable)"
        per_thread[tid][key] += 1
    print(f"wall samples {len(rows)} over {len(totals)} threads")
    for tid, count in totals.most_common(8):
        print(f"\nthread {tid}: {count} samples")
        for key, hits in per_thread[tid].most_common(top):
            print(f"  {hits:6d} {100.0 * hits / count:5.1f}%  {key}")
    return 0


def resolve(exe: str, offsets: list[int]) -> dict[int, str]:
    if not offsets:
        return {}
    unique = sorted(set(offsets))
    result = subprocess.run(
        ["addr2line", "-f", "-C", "-e", exe, *[hex(offset) for offset in unique]],
        capture_output=True,
        text=True,
        timeout=600,
        check=False,
    )
    lines = result.stdout.splitlines()
    names: dict[int, str] = {}
    for index, offset in enumerate(unique):
        name = lines[2 * index] if 2 * index < len(lines) else "??"
        names[offset] = name
    return names


def resolve_lines(exe: str, offsets: list[int]) -> dict[int, str]:
    """T1289: `file:line` per offset (needs a host built with -g), the outermost inlined frame is the line reported."""
    if not offsets:
        return {}
    unique = sorted(set(offsets))
    result = subprocess.run(
        ["addr2line", "-e", exe, *[hex(offset) for offset in unique]],
        capture_output=True,
        text=True,
        timeout=900,
        check=False,
    )
    lines = result.stdout.splitlines()
    return {
        offset: Path(lines[i].split(" ")[0]).name if i < len(lines) else "??"
        for i, offset in enumerate(unique)
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("file", type=Path)
    parser.add_argument("--top", type=int, default=25)
    parser.add_argument(
        "--window",
        help="CLOCK_MONOTONIC ms range A:B (the clock of the audio stall and slow call lines): only samples inside it, needs FILE.ms",
    )
    parser.add_argument("--tid", type=int, help="only this thread id")
    parser.add_argument(
        "--lines",
        action="store_true",
        help="T1289: also the hottest source lines per thread (the host must be built with -g)",
    )
    args = parser.parse_args()
    base, exe, samples = load(args.file)
    if args.window or args.tid:
        times = (
            [int(line) for line in Path(f"{args.file}.ms").read_text().split()]
            if args.window
            else []
        )
        low, high = (
            (int(value) for value in args.window.split(":")) if args.window else (0, 1 << 62)
        )
        samples = [
            sample
            for index, sample in enumerate(samples)
            if (not args.window or low <= times[index] <= high)
            and (not args.tid or sample[0] == args.tid)
        ]
    if "# mode wall" in args.file.read_text()[:400]:
        return wall_report(args.file, base, exe, args.top)
    size = Path(exe).stat().st_size if exe and Path(exe).exists() else 0
    inside = [(tid, rip - base) for tid, rip in samples if 0 <= rip - base < max(size * 4, 1 << 28)]
    names = resolve(exe, [offset for _, offset in inside])
    per_thread: dict[int, collections.Counter[str]] = collections.defaultdict(collections.Counter)
    totals: collections.Counter[int] = collections.Counter()
    for tid, _rip in samples:
        totals[tid] += 1
    for tid, offset in inside:
        per_thread[tid][names.get(offset, "??")] += 1
    lines = resolve_lines(exe, [offset for _, offset in inside]) if args.lines else {}
    maps = maps_of(args.file)
    libraries: dict[int, collections.Counter[str]] = collections.defaultdict(collections.Counter)
    for tid, rip in samples:
        if not 0 <= rip - base < max(size * 4, 1 << 28):
            libraries[tid][
                Path(library_name(maps, rip).split("+")[0]).name if maps else "[other]"
            ] += 1
    print(
        f"samples {len(samples)} ({len(samples) / 997.0:.1f} CPU s at 997 Hz), in the executable {len(inside)}"
    )
    for tid, count in totals.most_common(6):
        in_exe = sum(per_thread[tid].values())
        print(
            f"\nthread {tid}: {count} samples ({count / 997.0:.1f} CPU s), {in_exe} in the executable, the rest in libraries"
        )
        for name, hits in per_thread[tid].most_common(args.top):
            print(f"  {hits:7d} {100.0 * hits / count:5.1f}%  {name}")
        if args.lines:
            per_line = collections.Counter(
                lines.get(offset, "??") for t, offset in inside if t == tid
            )
            for name, hits in per_line.most_common(args.top):
                print(f"  {hits:7d} {100.0 * hits / count:5.1f}%  [line] {name}")
        for name, hits in libraries[tid].most_common(6):
            print(f"  {hits:7d} {100.0 * hits / count:5.1f}%  [library] {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
