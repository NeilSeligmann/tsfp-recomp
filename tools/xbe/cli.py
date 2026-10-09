# SPDX-License-Identifier: GPL-3.0-or-later
"""Dump the contents of an Xbox executable."""

from __future__ import annotations

import argparse
from pathlib import Path

from tools.xbe import parse_xbe


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Inspect an Xbox executable (XBE).")
    parser.add_argument("xbe", type=Path, help="path to the XBE file")
    parser.add_argument("--sections", action="store_true", help="list sections")
    parser.add_argument("--cert", action="store_true", help="show the certificate")
    parser.add_argument("--imports", action="store_true", help="list kernel import ordinals")
    parser.add_argument("--xtlid", action="store_true", help="list .XTLID entries")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    xbe = parse_xbe(args.xbe.read_bytes())

    print(f"base address    {xbe.base_address:#010x}")
    print(f"image size      {xbe.size_of_image:#010x}")
    print(f"entry point     {xbe.entry_point:#010x}")
    print(f"kernel thunk    {xbe.kernel_thunk_addr:#010x}")
    print(f"build           {'retail' if xbe.is_retail else 'debug'}")
    print(f"sections        {len(xbe.sections)}")
    print(f"kernel imports  {len(xbe.kernel_import_ordinals)}")
    if xbe.xdk_build is not None:
        print(f"XDK build       {xbe.xdk_build}")
        print(f".XTLID entries  {len(xbe.xtlid)}")

    if args.cert:
        cert = xbe.certificate
        print(f"\ntitle           {cert.title_name!r}")
        print(f"title id        {cert.title_id:#010x}")
        print(f"region          {cert.game_region}")
        print(f"version         {cert.version}")
        print(f"disk number     {cert.disk_number}")

    if args.sections:
        print(f"\n{'name':16s} {'vaddr':>10s} {'vsize':>10s} {'raw':>10s} {'rsize':>10s} flags")
        for section in xbe.sections:
            marks = "".join(
                [
                    "W" if section.writable else "-",
                    "P" if section.preload else "-",
                    "X" if section.executable else "-",
                    "F" if section.inserted_file else "-",
                ]
            )
            print(
                f"{section.name:16s} {section.virtual_addr:#010x} {section.virtual_size:#10x} "
                f"{section.raw_addr:#010x} {section.raw_size:#10x} {marks}"
            )

    if args.imports:
        print("\nkernel import ordinals:")
        print("  " + " ".join(str(o) for o in sorted(xbe.kernel_import_ordinals)))

    if args.xtlid:
        print(f"\n{'func_id':>8s} {'lib_id':>7s} {'address':>10s}")
        for entry in xbe.xtlid:
            print(f"{entry.func_id:>8d} {entry.lib_id:>7d} {entry.address:#010x}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
