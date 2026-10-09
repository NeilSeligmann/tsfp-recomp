# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line for the T59 probe. Paths default to the repository-relative ones."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from tools.initmap.image import Image
from tools.initmap.liftparse import index_directory
from tools.mmio_reach.census import (
    classify_reach,
    read_functions,
    read_manual,
    scan,
)
from tools.mmio_reach.instrument import instrument
from tools.mmio_reach.report import Assessment, EmptyCapture, assess

DEFAULT_LIFTED = Path("generated/lifted/gen")
DEFAULT_FUNCTIONS = Path("generated/lifted/disasm/functions.json")
DEFAULT_XBE = Path("tmp/oxm-extract/retail/default.xbe")
DEFAULT_SCRATCH = Path("tmp/mmio_reach")
#: Sections that hold statically linked XDK library code.
LIBRARY_SECTIONS = ("D3D", "XGRPH", "DSOUND", "XONLINE", "XNET", "XMV", "XPP", "DOLBY", "XON_RD")


def _hex(value: str) -> int:
    return int(value, 0)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.mmio_reach", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="command", required=True)

    census = sub.add_parser("census", help="static: NV2A constants and how each is reached")
    census.add_argument("--lifted", type=Path, default=DEFAULT_LIFTED)
    census.add_argument("--functions", type=Path, default=DEFAULT_FUNCTIONS)
    census.add_argument("--all", action="store_true", help="also list masks and other non-accesses")

    inst = sub.add_parser(
        "instrument", help="write a lifted tree with an entry marker per function"
    )
    inst.add_argument("--lifted", type=Path, default=DEFAULT_LIFTED)
    inst.add_argument("--out", type=Path, required=True)

    build = sub.add_parser("build", help="configure and build tsfp_host against a lifted tree")
    build.add_argument("--lifted", type=Path, required=True, help="tree from `instrument`")
    build.add_argument("--build-dir", type=Path, required=True)
    build.add_argument("--jobs", type=int, default=8)
    build.add_argument("--timeout", type=int, default=3000)

    boot = sub.add_parser("boot", help="run the host and report whether original code touched NV2A")
    boot.add_argument("--host", type=Path, required=True, help="tsfp_host binary")
    boot.add_argument("--xbe", type=Path, default=DEFAULT_XBE)
    boot.add_argument("--lifted", type=Path, default=DEFAULT_LIFTED)
    boot.add_argument("--functions", type=Path, default=DEFAULT_FUNCTIONS)
    boot.add_argument("--scratch", type=Path, default=DEFAULT_SCRATCH, help="fresh HDD directory")
    boot.add_argument("--timeout", type=int, default=120)
    boot.add_argument("--capture", type=Path, help="also save the host's combined output here")
    boot.add_argument("--from-capture", type=Path, help="parse a saved capture, run nothing")
    boot.add_argument(
        "--instrumented",
        action="store_true",
        help="the host was built from `instrument` output, so T59-HIT lines are expected",
    )
    boot.add_argument("host_args", nargs="*", help="extra tsfp_host arguments, after `--`")
    return parser


def _library_name(image: Image, entry: int) -> str | None:
    name = image.section_name(entry)
    return name if name in LIBRARY_SECTIONS else None


def run_census(args: argparse.Namespace) -> int:
    index = index_directory(args.lifted)
    findings = scan(index)
    manual = read_manual(args.lifted / "recomp_xdk_manual.c")
    records = read_functions(args.functions)
    shown = [f for f in findings if args.all or f.direct]
    print(f"lifted functions: {len(index.spans)}; with an NV2A-window literal: {len(findings)};")
    print(
        f"with a direct access: {sum(1 for f in findings if f.direct)}; intercepted: {len(manual)}"
    )
    for finding in shown:
        reach = classify_reach(records, finding.entry, manual)
        constants = " ".join(f"{v:#010x}" for v in finding.constants)
        kind = "direct" if finding.direct else "literal-only"
        print(f"\n{finding.entry:#010x}  {kind}  {constants}")
        if finding.direct:
            blockers = " ".join(f"{b:#010x}" for b in reach.blockers) or "none"
            ends = " ".join(f"{k}:{a:#010x}" for k, a in reach.open_ends) or "none"
            print(f"    blocked by intercepted callers: {blockers}")
            print(f"    open path ends: {ends}")
    return 0


def run_instrument(args: argparse.Namespace) -> int:
    count = instrument(args.lifted, args.out)
    print(f"marked {count} functions into {args.out}")
    print(f"build: python -m tools.mmio_reach build --lifted {args.out} --build-dir <dir>")
    return 0


def run_build(args: argparse.Namespace) -> int:
    configure = [
        "cmake",
        "-S",
        ".",
        "-B",
        str(args.build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DTSFP_LIFTED_DIR={args.lifted.resolve()}",
    ]
    compile_ = ["cmake", "--build", str(args.build_dir), f"-j{args.jobs}", "--target", "tsfp_host"]
    for command in (configure, compile_):
        completed = subprocess.run(command, timeout=args.timeout, check=False)
        if completed.returncode != 0:
            return completed.returncode
    return 0


def _run_host(args: argparse.Namespace) -> str:
    if args.scratch.exists():
        shutil.rmtree(args.scratch)
    args.scratch.mkdir(parents=True)
    command = [
        str(args.host),
        str(args.xbe),
        "--hdd",
        str(args.scratch),
        "--trace",
        "2048",
        *args.host_args,
    ]
    try:
        completed = subprocess.run(
            ["timeout", "--kill-after=5", str(args.timeout), *command],
            capture_output=True,
            text=True,
            errors="replace",
            check=False,
            timeout=args.timeout + 30,
        )
    finally:
        shutil.rmtree(args.scratch, ignore_errors=True)
    if completed.returncode in (124, 137):
        raise SystemExit(
            f"host timed out after {args.timeout}s: a timeout is a failure, not a result"
        )
    return completed.stdout + "\n" + completed.stderr


def _print_assessment(result: Assessment, image: Image | None) -> None:
    for stop in result.stops:
        where = (
            f" fault address {stop.fault_address:#010x}" if stop.fault_address is not None else ""
        )
        at = f" guest address {stop.guest_address:#010x}" if stop.guest_address is not None else ""
        print(f"thread {stop.thread} stopped: {stop.reason};{where}{at}")
    d3d = [c for c in result.xdk_calls if image and image.section_name(c.address) == "D3D"]
    print(f"XDK dispatches: {len(result.xdk_calls)}; into D3D: {len(d3d)}")
    counts: dict[int, int] = {}
    for call in d3d:
        counts[call.address] = counts.get(call.address, 0) + 1
    names = {call.address: call.name for call in d3d}
    print("D3D addresses in first-reached order (count):")
    for address, count in counts.items():
        print(f"    {address:#010x} {names[address]} x{count}")
    if result.hits is None:
        print("function entry markers: none (host not instrumented): execution NOT measured")
    else:
        library = {}
        if image:
            for entry in result.hits:
                name = _library_name(image, entry)
                if name:
                    library[name] = library.get(name, 0) + 1
        print(f"functions entered: {len(result.hits)}; library bodies per section: {library}")
        d3d_bodies = [e for e in result.hits if image and image.section_name(e) == "D3D"]
        print(
            "original D3D bodies entered, in order: "
            + (", ".join(f"{e:#010x}" for e in d3d_bodies) or "none")
        )
        print(
            "direct NV2A accessors entered: "
            + (", ".join(f"{e:#010x}" for e in result.executed_accessors) or "none")
        )
        print(
            "direct NV2A accessors never entered: "
            + (", ".join(f"{e:#010x}" for e in result.unexecuted_accessors) or "none")
        )
    print(
        f"VERDICT original code touched NV2A: {'YES' if result.touched else 'NO'}"
        f" ({'faults and entry markers' if result.execution_measured else 'faults only'})"
    )


def run_boot(args: argparse.Namespace) -> int:
    text = (
        args.from_capture.read_text(encoding="utf-8", errors="replace")
        if args.from_capture
        else _run_host(args)
    )
    if args.capture and not args.from_capture:
        args.capture.write_text(text, encoding="utf-8")
    findings = scan(index_directory(args.lifted)) if args.lifted.is_dir() else []
    try:
        result = assess(text, [f for f in findings if f.direct], args.instrumented)
    except EmptyCapture as error:
        print(f"no verdict: {error}", file=sys.stderr)
        return 3
    image = Image.load(args.xbe) if args.xbe.is_file() else None
    _print_assessment(result, image)
    return 1 if result.touched else 0


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    handlers = {
        "census": run_census,
        "instrument": run_instrument,
        "build": run_build,
        "boot": run_boot,
    }
    return handlers[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
