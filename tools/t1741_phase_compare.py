#!/usr/bin/env python3
# ruff: noqa: E501
"""T1741: compare the census phases of one live level run (idle control, hostile encounter, kill phase) target by target.

Reads `census.txt.<phase>` files (src/host/function_census.c format) from a session folder and prints, per phase, the targets
that are absent from the idle control or whose per-present rate is at least --ratio times idle's, and every T1740 class tick
entry (tools.ai_attribution.CLASS_TICKS) with its calls in each phase. Rates are calls per present spanned (header
first_present..last_present). MEASURED counts, roles INFERRED (indirect entry points only).

Usage: python -m tools.t1741_phase_compare SESSION_DIR [--idle ingame-idle] [--phases A,B] [--ratio 3] [--min-calls 5] [--top 40]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from tools.ai_attribution import CLASS_TICKS

HEADER = re.compile(
    r"# calls (\d+) targets (\d+) overflow (\d+) first_present (\d+) last_present (\d+)"
)
TARGET = re.compile(r"target 0x([0-9A-Fa-f]+) calls (\d+)")


def load(path: Path) -> tuple[dict[int, int], int]:
    """(calls per target, presents spanned) of one phase file."""
    counts: dict[int, int] = {}
    presents = 1
    for line in path.read_text().splitlines():
        header = HEADER.match(line)
        if header:
            presents = max(1, int(header[5]) - int(header[4]) + 1)
            continue
        target = TARGET.match(line)
        if target:
            counts[int(target[1], 16)] = int(target[2])
    return counts, presents


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("session_dir", type=Path)
    parser.add_argument("--idle", default="ingame-idle")
    parser.add_argument("--phases", default="ingame-enemy-encounter,ingame-enemy-kill")
    parser.add_argument("--ratio", type=float, default=3.0)
    parser.add_argument("--min-calls", type=int, default=5)
    parser.add_argument("--top", type=int, default=40)
    args = parser.parse_args(argv)
    idle, idle_presents = load(args.session_dir / f"census.txt.{args.idle}")
    phases = {
        name: load(args.session_dir / f"census.txt.{name}") for name in args.phases.split(",")
    }
    print(
        f"idle {args.idle}: {sum(idle.values())} calls, {len(idle)} targets, {idle_presents} presents"
    )
    for name, (counts, presents) in phases.items():
        print(
            f"\n== {name}: {sum(counts.values())} calls, {len(counts)} targets, {presents} presents"
        )
        rows = []
        for target, calls in counts.items():
            if calls < args.min_calls:
                continue
            rate = calls / presents
            idle_rate = idle.get(target, 0) / idle_presents
            if target not in idle or rate >= args.ratio * idle_rate:
                rows.append((target in idle, -calls, target, rate, idle_rate))
        print(f"  absent from idle or rate >= {args.ratio}x idle (calls >= {args.min_calls}):")
        for in_idle, negative_calls, target, rate, idle_rate in sorted(rows)[: args.top]:
            note = "" if in_idle else "  ABSENT in idle"
            print(
                f"  0x{target:08X} calls {-negative_calls:>7} rate {rate:8.3f}/present idle {idle_rate:8.3f}{note}"
            )
    print("\nclass tick entries (calls in idle / phases):")
    seen = False
    for tick, selector in sorted(CLASS_TICKS.items()):
        values = [idle.get(tick, 0), *(counts.get(tick, 0) for counts, _ in phases.values())]
        if any(values):
            seen = True
            print(f"  0x{tick:08X} selector {selector}: {values}")
    if not seen:
        print("  none of the class tick entries was called in any phase")
    return 0


if __name__ == "__main__":
    sys.exit(main())
