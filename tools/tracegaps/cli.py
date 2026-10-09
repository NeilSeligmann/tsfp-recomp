# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line for `tools.tracegaps`: close the T263 inferred-by-absence claims.

    python -m tools.tracegaps createevent             # part (a), the CreateEvent/Mutex callers
    python -m tools.tracegaps vertexkeys              # part (b), the five non-literal key sites
    python -m tools.tracegaps threadreach             # T424, can thread 0x3C0FE0 reach the getter
    python -m tools.tracegaps gateregion              # T592, the loading bar gate reads no counter
    python -m tools.tracegaps threadreach --start 0x30160   # T593, the loader thread
    python -m tools.tracegaps all --json              # (a) and (b), machine readable

Read-only. The image defaults to `tmp/oxm-extract/retail/default.xbe` relative to the
working directory, so run it from the repository root. It prints addresses and small
integers only. A missing image exits 2 with a message rather than printing an empty table.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Sequence
from pathlib import Path

from tools.tracegaps import createevent, gateregion, pools, threadreach, vertexkeys, vtable_pairing
from tools.tracegaps.code import Code
from tools.tracegaps.emulate import Runner
from tools.tracegaps.flow import FlowCache

DEFAULT_XBE = Path("tmp/oxm-extract/retail/default.xbe")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.tracegaps",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "part", choices=("createevent", "vertexkeys", "threadreach", "gateregion", "pools", "all")
    )
    parser.add_argument("--xbe", type=Path, default=DEFAULT_XBE, help="retail default.xbe")
    parser.add_argument(
        "--start",
        type=lambda text: int(text, 0),
        default=threadreach.START,
        help="thread start routine for threadreach (default 0x3C0FE0, T593 adds 0x30160)",
    )
    parser.add_argument("--json", action="store_true", help="print JSON instead of tables")
    parser.add_argument("--verbose", action="store_true", help="list every caller address")
    parser.add_argument(
        "--no-emissions",
        action="store_true",
        help="skip running the title's shader builders on the keys (vertexkeys only)",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.xbe.exists():
        print(f"error: no image at {args.xbe} (pass --xbe)", file=sys.stderr)
        return 2
    code = Code.load(args.xbe)
    runner = Runner(code.image)
    flows = FlowCache(code)
    output: dict[str, object] = {}
    texts: list[str] = []
    if args.part in ("createevent", "all"):
        for spec in (createevent.EVENT, createevent.MUTEX):
            report = createevent.trace_wrapper(code, runner, spec, flows)
            output[spec.name] = createevent.to_json(report)
            texts.append(createevent.render(report))
    if args.part in ("vertexkeys", "all"):
        keys = vertexkeys.analyze(code, flows, runner, with_emissions=not args.no_emissions)
        output["vertexkeys"] = vertexkeys.to_json(keys)
        texts.append(vertexkeys.render(keys, verbose=args.verbose))
    if args.part == "threadreach":
        report = threadreach.analyse(code, start=args.start)
        output["threadreach"] = threadreach.to_json(report)
        texts.append(threadreach.render(report))
    if args.part == "gateregion":
        gate = gateregion.analyse(code)
        output["gateregion"] = gateregion.to_json(gate)
        texts.append(gateregion.render(gate))
    if args.part == "pools":
        report = pools.analyse(code)
        texts.append(pools.render(report))
        texts.append(vtable_pairing.render(vtable_pairing.analyse(code, report)))
    if args.json:
        print(json.dumps(output, indent=2, sort_keys=True))
    else:
        print("\n\n".join(texts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
