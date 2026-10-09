# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line for the init-sequence map."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

from tools.initmap.compare import compare, parse_runtime_trace
from tools.initmap.crosscheck import cross_check
from tools.initmap.image import Image, read_abi, read_data_ordinals, read_surface
from tools.initmap.liftparse import index_directory
from tools.initmap.render import render_crosscheck, render_text, to_json
from tools.initmap.report import build
from tools.initmap.walk import Walker

#: The title's startup as measured and inferred, see docs/init-sequence.md section 2.
DEFAULT_ROOTS = "0x0038024D,0x0037FE1D,0x003801D9"
DEFAULT_RESOLVE = "0x0037FE1D=0x003801D9"
_IMMEDIATE = re.compile(r"\b0x([0-9A-Fa-f]{5,8})u?\b")


def _hex(value: str) -> int:
    return int(value, 0)


def _hex_list(value: str) -> tuple[int, ...]:
    return tuple(int(item, 0) for item in value.split(",") if item.strip())


def _resolve_pair(value: str) -> tuple[int, int]:
    function, _, target = value.partition("=")
    if not target:
        raise argparse.ArgumentTypeError(f"expected FUNCTION=TARGET, got {value!r}")
    return int(function, 0), int(target, 0)


def parse_implemented(value: str) -> frozenset[int]:
    """A comma list of ordinals, or a path to a file holding one."""
    path = Path(value)
    text = path.read_text(encoding="utf-8") if path.is_file() else value
    return frozenset(int(item) for item in re.split(r"[,\s]+", text.strip()) if item)


def run_hle_report(binary: Path) -> frozenset[int]:
    """Ask the runner which ordinals it has, rather than counting declarations."""
    completed = subprocess.run(
        [str(binary), "--implemented-ordinals"],
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return parse_implemented(completed.stdout)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Map what the title's startup needs from the XDK.")
    parser.add_argument("--xbe", type=Path, required=True, help="the user's default.xbe")
    parser.add_argument(
        "--lifted", type=Path, default=Path("generated/lifted/gen"), help="lifted C chunks"
    )
    parser.add_argument("--surface", type=Path, default=Path("src/xbox/xdk_surface.c"))
    parser.add_argument("--abi", type=Path, default=Path("src/xbox/xdk_abi.inc"))
    parser.add_argument(
        "--oracle",
        type=Path,
        default=Path("src/xbox/kernel_arity_oracle.c"),
        help="the arity oracle, read for which kernel exports are data",
    )
    parser.add_argument("--roots", type=_hex_list, default=_hex_list(DEFAULT_ROOTS))
    parser.add_argument(
        "--resolve-indirect",
        type=_resolve_pair,
        action="append",
        default=None,
        metavar="FUNCTION=TARGET",
        help="treat FUNCTION's first unresolved register call as a call to TARGET (inferred)",
    )
    parser.add_argument("--main", type=_hex, default=0x0018E8D0)
    parser.add_argument("--init-list", type=_hex, default=0x0003C330)
    parser.add_argument("--frame-loop", type=_hex, default=0x0003D370)
    parser.add_argument(
        "--follow-address-taken",
        action="store_true",
        help="also walk routines whose address is stored, as a second tier",
    )
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--implemented", help="comma list of implemented ordinals, or a file")
    group.add_argument("--hle-report", type=Path, help="run PATH --implemented-ordinals")
    parser.add_argument("--trace-file", type=Path, help="the host's report, to validate against")
    parser.add_argument("--top", type=int, default=0, help="rows per module (0 means all)")
    parser.add_argument(
        "--crosscheck",
        action="store_true",
        help="re-decode every expanded function from the image and compare call lists",
    )
    parser.add_argument(
        "--chain-depth", type=int, default=0, help="show this many callers for each backlog row"
    )
    parser.add_argument("--json", type=Path, default=None, help="write the data here")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.implemented is not None:
        implemented = parse_implemented(args.implemented)
    elif args.hle_report is not None:
        implemented = run_hle_report(args.hle_report)
    else:
        implemented = frozenset()
        print(
            "note: no --implemented or --hle-report, every ordinal reads as MISSING",
            file=sys.stderr,
        )

    image = Image.load(args.xbe)
    surface = read_surface(args.surface)
    abi = read_abi(args.abi)
    index = index_directory(args.lifted)
    walker = Walker(index, image, surface, read_data_ordinals(args.oracle))
    resolve = dict(args.resolve_indirect or [_resolve_pair(DEFAULT_RESOLVE)])
    walk = walker.run(
        tuple(args.roots),
        markers=frozenset({args.main, args.frame_loop}),
        follow_address_taken=args.follow_address_taken,
        resolve_indirect=resolve,
    )
    report = build(
        walker,
        walk,
        image,
        surface,
        abi,
        implemented,
        main=args.main,
        frame_loop=args.frame_loop,
        init_list=args.init_list,
    )
    comparisons = []
    if args.trace_file is not None:
        runtime = parse_runtime_trace(args.trace_file.read_text(encoding="utf-8", errors="replace"))
        comparisons.append(("all threads", compare(runtime, walk)))
        for thread in sorted({call.thread for call in runtime}):
            comparisons.append((f"thread t{thread}", compare(runtime, walk, thread)))
    sys.stdout.write(
        render_text(report, index, image, implemented, args.top, comparisons, args.chain_depth)
    )
    if args.crosscheck:
        expanded = sorted({frame.function for frame in walk.frames})
        sys.stdout.write("\n".join(render_crosscheck(cross_check(index, image, expanded))) + "\n")
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(to_json(report), indent=1) + "\n", encoding="utf-8")
    return 0
