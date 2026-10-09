# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line for `tools.d3dscan`: what methods does each D3D8 function write?

    python -m tools.d3dscan.cli IMAGE.xbe --functions functions.csv
    python -m tools.d3dscan.cli IMAGE.xbe --section D3D --method-names nv_regs.h
    python -m tools.d3dscan.cli IMAGE.xbe --null .data

Read-only in every mode: it opens the image and the function CSV and writes nothing.
"""

from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path

from tools.codediff.boundaries import load_functions
from tools.d3dscan.methods import NV2A_METHODS, name_method, parse_method_names
from tools.d3dscan.pushscan import attribute, header_null, load_image, scan_section
from tools.gen_d3d8_surface import Image
from tools.xbe import XbeSection


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.d3dscan.cli",
        description="Recover the NV2A methods each function in a linked XDK section writes.",
    )
    parser.add_argument("xbe", type=Path, help="path to the XBE image")
    parser.add_argument(
        "--section",
        default="D3D",
        help="section to scan (default: D3D)",
    )
    parser.add_argument(
        "--functions",
        type=Path,
        help="function-bounds CSV from tools/ghidra/ExportFunctionBounds.java; "
        "without it, writes are listed flat instead of grouped by function",
    )
    parser.add_argument(
        "--method-names",
        type=Path,
        help="reference register header to widen the built-in method name table",
    )
    parser.add_argument(
        "--only",
        type=lambda value: int(value, 16),
        action="append",
        help="report only this function entry VA (hex); repeatable",
    )
    parser.add_argument(
        "--null",
        metavar="SECTION",
        help="measure the header-plausibility false-positive rate over this section's "
        "bytes instead of scanning for writes",
    )
    parser.add_argument(
        "--methods-only",
        action="store_true",
        help="print one line per function: entry VA and its distinct methods",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    image = load_image(args.xbe)

    names = dict(NV2A_METHODS)
    if args.method_names is not None:
        names.update(parse_method_names(args.method_names))

    if args.null is not None:
        section = _section(image, args.null)
        hits, dwords = header_null(image.section_body(section))
        rate = hits / dwords if dwords else 0.0
        print(f"{args.null}: {hits} of {dwords} 4-aligned dwords plausible = {rate:.2%}")
        return 0

    section = _section(image, args.section)
    body = image.section_body(section)
    writes = scan_section(body, section.virtual_addr)
    print(
        f"{args.section} {section.virtual_addr:#010x}..{section.virtual_addr + len(body):#010x} "
        f"({len(body)} bytes): {len(writes)} method headers recovered"
    )
    shapes = Counter(write.shape for write in writes)
    print("  by shape: " + ", ".join(f"{shape}={count}" for shape, count in sorted(shapes.items())))

    if args.functions is None:
        for write in writes:
            print("  " + write.describe(names))
        return 0

    functions = attribute(
        writes,
        load_functions(args.functions),
        section.virtual_addr,
        section.virtual_addr + section.virtual_size,
    )
    attributed = sum(len(fn.writes) for fn in functions)
    print(
        f"  {len(functions)} functions in bounds, "
        f"{sum(1 for fn in functions if fn.recovered)} with recovered writes, "
        f"{attributed} of {len(writes)} writes attributed"
    )

    wanted = set(args.only or ())
    for fn in functions:
        if wanted and fn.entry_va not in wanted:
            continue
        if not fn.recovered and not wanted:
            continue
        labels = [name_method(method, names) for method in fn.methods()]
        print(f"{fn.entry_va:#010x} {len(fn.writes):3d} writes  {' '.join(labels)}")
        if not args.methods_only:
            for write in fn.writes:
                print("    " + write.describe(names))
    return 0


def _section(image: Image, name: str) -> XbeSection:
    for section in image.xbe.sections:
        if section.name == name:
            return section
    raise SystemExit(f"no section named {name!r} in the image")


if __name__ == "__main__":
    raise SystemExit(main())
