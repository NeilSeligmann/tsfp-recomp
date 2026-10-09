#!/usr/bin/env python3
"""Run many tsfp_host boots and histogram the total HLE call count and stop reasons.

T434 harness. Each boot gets a fresh temporary --hdd directory and a hard timeout.
A boot that does not reproduce the expected figure is kept as a log under --keep-dir.

  python tools/boot_flake.py --serial 100 --parallel 0
  python tools/boot_flake.py --serial 1000 --parallel 300 --jobs 12 --keep-dir tmp/flake
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import TypedDict

SIX_FLAGS = [
    "--ac97-ready",
    "--headless-effects",
    "--headless-streams",
    "--headless-buffers",
    "--headless-listener",
    "--headless-first-vblank",
]
CALLS = re.compile(r"HLE calls reached.*\((\d+) recorded, (\d+) total")
STOP = re.compile(r"^guest thread (0x[0-9a-fA-F]+) \(entered 0x[0-9a-fA-F]+\) stopped: (.*)$")
GUEST_ADDR = re.compile(r"^\s+guest address\s+(0x[0-9A-Fa-f]+)")


class BootResult(TypedDict):
    """What one boot produced: its label, total HLE calls (-1 if unreported), exit code, the
    sorted stop lines (reason at guest address) and the whole combined output."""

    index: str
    total: int
    code: int
    stops: tuple[str, ...]
    text: str


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--host", default="build/tsfp_host", help="host binary")
    parser.add_argument("--xbe", default="build/default.xbe", help="title image")
    parser.add_argument("--serial", type=int, default=100, help="boots run one at a time")
    parser.add_argument("--parallel", type=int, default=0, help="boots run concurrently")
    parser.add_argument("--jobs", type=int, default=12, help="concurrency of the parallel batch")
    parser.add_argument("--timeout", type=int, default=60, help="seconds per boot (hard kill)")
    parser.add_argument("--flags", choices=["six", "none"], default="six", help="headless flag set")
    parser.add_argument(
        "--extra",
        default="",
        help='extra host arguments as one string, for example "--native-shader-assembler"',
    )
    parser.add_argument(
        "--keep-dir", default="", help="save logs of boots that differ from the modal result"
    )
    parser.add_argument(
        "--expect",
        type=int,
        default=0,
        help="expected total calls, default: the modal result",
    )
    parser.add_argument(
        "--wrap",
        default="",
        help="command prefix as one string, for example a debugger wrapper",
    )
    return parser.parse_args(argv)


def one_boot(args: argparse.Namespace, index: str) -> BootResult:
    hdd = tempfile.mkdtemp(prefix="t434-hdd-")
    command = [*shlex.split(args.wrap), args.host, args.xbe, "--hdd", hdd, "--trace", "1"]
    if args.flags == "six":
        command += SIX_FLAGS
    command += shlex.split(args.extra)
    try:
        proc = subprocess.run(
            ["timeout", "--kill-after=5s", str(args.timeout), *command],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        text = proc.stdout.decode("utf-8", "replace")
        code = proc.returncode
    finally:
        shutil.rmtree(hdd, ignore_errors=True)
    total = -1
    for line in text.splitlines():
        match = CALLS.search(line)
        if match:
            total = int(match.group(2))
    stops = []
    lines = text.splitlines()
    for position, line in enumerate(lines):
        match = STOP.match(line)
        if match:
            address = ""
            for follow in lines[position + 1 : position + 4]:
                found = GUEST_ADDR.match(follow)
                if found:
                    address = found.group(1)
            stops.append(f"{match.group(2)}@{address}")
    return {
        "index": index,
        "total": total,
        "code": code,
        "stops": tuple(sorted(stops)),
        "text": text,
    }


def run_batch(args: argparse.Namespace, label: str, count: int, jobs: int) -> list[BootResult]:
    results: list[BootResult] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(one_boot, args, f"{label}{i}") for i in range(count)]
        for future in concurrent.futures.as_completed(futures):
            results.append(future.result())
    return results


def summarise(label: str, results: list[BootResult], args: argparse.Namespace, expect: int) -> int:
    print(f"== {label}: {len(results)} boots ==")
    if not results:
        return 0
    histogram = collections.Counter((r["total"], r["code"]) for r in results)
    if expect == 0:
        expect = histogram.most_common(1)[0][0][0]
    for (total, code), count in sorted(histogram.items()):
        print(f"  total={total:<6d} rc={code:<4d} boots={count}")
    reasons = collections.Counter(r["stops"] for r in results)
    for stops, count in reasons.most_common():
        print(f"  {count:5d} x {'; '.join(stops) if stops else '(no stop line)'}")
    bad = [r for r in results if r["total"] != expect or r["code"] != 0]
    print(f"  expected total {expect}, deviating boots {len(bad)} of {len(results)}")
    if args.keep_dir and bad:
        os.makedirs(args.keep_dir, exist_ok=True)
        for r in bad:
            path = os.path.join(args.keep_dir, f"boot-{r['index']}-total{r['total']}.log")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(r["text"])
    return len(bad)


def main_with(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if not os.path.exists(args.host) or not os.path.exists(args.xbe):
        print("host or xbe missing", file=sys.stderr)
        return 2
    failures = 0
    expect = args.expect
    if args.serial:
        results = run_batch(args, "s", args.serial, 1)
        failures += summarise("serial", results, args, expect)
        if expect == 0:
            expect = collections.Counter(r["total"] for r in results).most_common(1)[0][0]
    if args.parallel:
        results = run_batch(args, "p", args.parallel, args.jobs)
        failures += summarise("parallel", results, args, expect)
    print(f"TOTAL deviating boots: {failures}")
    return 1 if failures else 0


def main() -> int:
    return main_with()


if __name__ == "__main__":
    sys.exit(main())
