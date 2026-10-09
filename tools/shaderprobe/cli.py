# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Command line for `tools.shaderprobe`: log the shader keys a boot of the title really forms.

    python -m tools.shaderprobe.cli instrument --lift generated/lifted/gen --out tmp/shaderprobe/gen \
        --extra generated/shader-original/recomp_xdk_original.c
    cmake -S . -B tmp/shaderprobe/build -DTSFP_LIFTED_DIR=tmp/shaderprobe/gen && ninja -C tmp/shaderprobe/build tsfp_host
    python -m tools.shaderprobe.cli run --host tmp/shaderprobe/build/tsfp_host \
        --xbe tmp/oxm-extract/retail/default.xbe --disc discs/tsfp-xbox.iso --log tmp/shaderprobe/boot.log
    python -m tools.shaderprobe.cli analyze --xbe tmp/oxm-extract/retail/default.xbe --log tmp/shaderprobe/boot.log

Counts, addresses and dwords only. No mode prints a shader, a program or a source string.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

from tools.shaderprobe import instrument as instrument_module
from tools.shaderprobe import log as log_module
from tools.shaderprobe.profile import RETAIL

#: Host options of the deepest boot measured so far (docs/shader-inputs.md section 11).
DEFAULT_HOST_FLAGS = (
    "--ac97-ready",
    "--headless-effects",
    "--headless-streams",
    "--headless-buffers",
    "--headless-listener",
    "--headless-second-vblank",
    "--native-shader-assembler",
    "--trace",
    "64",
    "--thread-timeout",
    "20000",
)


def _hex(text: str) -> int:
    return int(text, 0)


def cmd_instrument(args: argparse.Namespace) -> int:
    entries = set(args.entry) if args.entry else RETAIL.entry_functions()
    try:
        report = instrument_module.instrument_directory(
            args.lift, args.out, entries, args.key_global, extra_files=args.extra
        )
    except (FileExistsError, FileNotFoundError, ValueError) as error:
        print(f"instrument: {error}", file=sys.stderr)
        return 2
    print(f"instrumented copy at {args.out}")
    print(
        f"  entry hooks {len(report.entries)}: {', '.join(f'{a:#x}' for a in sorted(report.entries))}"
    )
    print(f"  logged stores to {args.key_global:#x}: {len(report.stores)}")
    for function, label in report.stores:
        print(f"    in {function:#x} near {label:#x}")
    if report.unhandled:
        print(
            f"  UNHANDLED mentions of {args.key_global:#x}: {len(report.unhandled)}",
            file=sys.stderr,
        )
        for function, text in report.unhandled:
            print(f"    in {function:#x}: {text}", file=sys.stderr)
        return 3
    return 0


def _kill_group(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def cmd_run(args: argparse.Namespace) -> int:
    if not args.host.is_file():
        print(f"run: no host binary at {args.host}", file=sys.stderr)
        return 2
    if args.hdd_dir.exists():
        shutil.rmtree(args.hdd_dir)
    args.hdd_dir.mkdir(parents=True)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    command = [str(args.host), str(args.xbe), "--hdd", str(args.hdd_dir)]
    if args.disc:
        command += ["--disc", str(args.disc)]
    command += list(DEFAULT_HOST_FLAGS) + list(args.host_arg or [])
    environment = dict(os.environ, TSFP_XDK_PROBE=RETAIL.probe_spec())
    with args.log.open("wb") as sink:
        process = subprocess.Popen(
            command, stdout=sink, stderr=subprocess.STDOUT, env=environment, start_new_session=True
        )
        try:
            code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            _kill_group(process)
            process.wait()
            print(
                f"run: killed after {args.timeout} s (a timeout is a failure, not a result)",
                file=sys.stderr,
            )
            return 4
    with args.log.open(errors="replace") as handle:
        capture = log_module.parse(handle)
    print(
        f"host exit {code}; {len(capture.probes)} probe, {len(capture.enters)} enter, {len(capture.stores)} store records"
    )
    if not (capture.probes and capture.enters and capture.stores):
        print(
            "run: a record kind is EMPTY, so nothing here is evidence (was the host built from the instrumented lift, and was TSFP_XDK_PROBE honoured?)",
            file=sys.stderr,
        )
        return 3
    return 0


def cmd_analyze(args: argparse.Namespace) -> int:
    from tools.shaderprobe import analyze

    with args.log.open(errors="replace") as handle:
        capture = log_module.parse(handle)
    try:
        report = analyze.analyze(args.xbe, capture, RETAIL)
    except analyze.EmptyCapture as error:
        print(f"analyze: {error}", file=sys.stderr)
        return 3
    print(analyze.render(report))
    if args.json:
        args.json.write_text(json.dumps(analyze.to_json(report), indent=2, sort_keys=True) + "\n")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="mode", required=True)

    instrument = sub.add_parser("instrument", help="copy the lift and add the key logging")
    instrument.add_argument(
        "--lift", type=Path, required=True, help="lifted gen directory (read only)"
    )
    instrument.add_argument(
        "--out", type=Path, required=True, help="destination copy (must not exist)"
    )
    instrument.add_argument("--key-global", type=_hex, default=RETAIL.key_global)
    instrument.add_argument(
        "--entry",
        type=_hex,
        action="append",
        help="extra or replacement entry hook (default: the profile's)",
    )
    instrument.add_argument(
        "--extra",
        type=Path,
        action="append",
        help="file to copy into the copy, for example the retained original shader chunk",
    )
    instrument.set_defaults(func=cmd_instrument)

    run = sub.add_parser("run", help="boot the instrumented host and capture the log")
    run.add_argument("--host", type=Path, required=True)
    run.add_argument("--xbe", type=Path, required=True)
    run.add_argument("--disc", type=Path)
    run.add_argument(
        "--hdd-dir",
        type=Path,
        default=Path("tmp/shaderprobe/hdd"),
        help="fresh private directory, recreated each run",
    )
    run.add_argument("--log", type=Path, default=Path("tmp/shaderprobe/boot.log"))
    run.add_argument(
        "--timeout",
        type=float,
        default=120.0,
        help="seconds before the host process group is killed",
    )
    run.add_argument("--host-arg", action="append", help="extra host option, repeatable")
    run.set_defaults(func=cmd_run)

    analyze = sub.add_parser("analyze", help="reconcile a capture with the static census")
    analyze.add_argument("--xbe", type=Path, required=True)
    analyze.add_argument("--log", type=Path, required=True)
    analyze.add_argument("--json", type=Path)
    analyze.set_defaults(func=cmd_analyze)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
