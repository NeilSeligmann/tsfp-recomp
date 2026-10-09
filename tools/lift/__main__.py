# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line for the lift driver.

::

    uv run python -m tools.lift vendor --source tmp/xboxrecomp
    uv run python -m tools.lift status
    uv run python -m tools.lift run path/to/default.xbe

``run`` writes to ``generated/lifted/`` by default, which ``.gitignore`` already
covers via ``/generated/``. Nothing derived from the user's executable is ever
written inside the repository's tracked tree.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from tools.lift.pipeline import DEFAULT_CHUNK_SIZE, LiftError, run_pipeline
from tools.lift.vendor import VENDORED_PATHS, VendorStatus, vendor_import, vendor_status

#: Repository root, three levels up from this file.
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_VENDOR_ROOT = REPO_ROOT / "third_party" / "xboxrecomp"
DEFAULT_OUT_DIR = REPO_ROOT / "generated" / "lifted"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.lift",
        description="Lift a user-supplied XBE to C with the vendored xboxrecomp lifter.",
    )
    parser.add_argument(
        "--vendor-root",
        type=Path,
        default=DEFAULT_VENDOR_ROOT,
        help="Vendored lifter tree (default: third_party/xboxrecomp)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    vendor = sub.add_parser("vendor", help="Import the lifter from an upstream checkout.")
    vendor.add_argument(
        "--source",
        type=Path,
        required=True,
        help="Path to a git checkout of sp00nznet/xboxrecomp",
    )

    sub.add_parser("status", help="Report whether the vendored tree is present and intact.")

    run = sub.add_parser("run", help="Lift an XBE to C.")
    run.add_argument("xbe", type=Path, help="Path to the user's default.xbe")
    run.add_argument(
        "--out-dir",
        type=Path,
        default=DEFAULT_OUT_DIR,
        help="Where to write lifted C (default: generated/lifted)",
    )
    run.add_argument(
        "--chunk-size",
        type=int,
        default=DEFAULT_CHUNK_SIZE,
        help=f"Functions per generated chunk (default: {DEFAULT_CHUNK_SIZE})",
    )
    run.add_argument(
        "--python",
        default=sys.executable,
        help="Interpreter used for the vendored stages (default: this one)",
    )
    run.add_argument(
        "--skip-existing",
        action="store_true",
        help="Reuse stage outputs that already exist instead of recomputing them",
    )
    run.add_argument(
        "--timeout",
        type=int,
        default=3600,
        help="Per-stage timeout in seconds (default: 3600)",
    )
    run.add_argument(
        "--manual-functions",
        type=Path,
        default=None,
        help="JSON list or {address: name} map of guest addresses to emit NO body "
        "for. Their direct calls become RECOMP_ICALL_SAFE and their tail jumps "
        "RECOMP_ITAIL, which is the only way lifted code reaches a host-side "
        "address-keyed dispatcher. Every listed symbol must still be DEFINED by "
        "hand or the link fails: see tools/gen_xdk_manual_list.py and "
        "docs/xdk-dispatch.md",
    )
    run.add_argument(
        "--only-function",
        action="append",
        type=lambda value: int(value, 0),
        default=[],
        help="Emit only selected function bodies with complete analysis and manual-call context",
    )
    run.add_argument(
        "--seed-functions",
        action="append",
        type=Path,
        help="Additional observed entry points in vendored seed-functions JSON format. "
        "Unlike --manual-functions, these entries get real generated bodies. "
        "Bundled measured title roots apply automatically only to their bound XBE.",
    )
    run.add_argument(
        "--publish-eflags",
        action="store_true",
        help="Emit the opt-in EFLAGS publication at every ret "
        "(g_harness_eflags/g_harness_eflags_mask), for the differential "
        "harness's --compare-eflags mode. The resulting tree is a HARNESS "
        "SUBJECT, not a shippable lift: its chunks reference globals only "
        "tools/harness/runtime_min.c defines. A lift without this flag is "
        "byte-identical to one made before the option existed",
    )
    run.add_argument(
        "--local-float-relations",
        action="store_true",
        help="Enable reviewed local scalar COMISS/UCOMISS relation provenance; "
        "default off, with calls and unsupported readers still refused",
    )
    run.add_argument(
        "--flag-bridge",
        type=Path,
        default=None,
        help="JSON {providers, consumers} of functions whose flags cross a call "
        "(tools/config/flag_bridge.json, lifter patch 21, docs/lifter-patches/"
        "21-flag-bridge.md). A provider publishes its modelled flags at every ret, a "
        "consumer starts from them. Prove the shape on the image first with "
        "`python -m tools.lift.flag_bridge prove`. A lift without this flag is "
        "byte-identical to one made before the option existed",
    )
    run.add_argument(
        "--install-shader-original",
        action="store_true",
        help="after the lift, generate and install the retained original shader-compiler "
        "chunk for it (tools.install_shader_original, about 45 s, opt-in at runtime by "
        "--native-shader-assembler). Without this a chunk left by an earlier lift is "
        "checked and reported STALE if it no longer matches",
    )
    return parser


def _report_shader_original(args: argparse.Namespace) -> bool:
    """Install or check the retained shader chunk. False only when a requested install failed."""
    from tools import install_shader_original

    try:
        lines = install_shader_original.after_lift(
            xbe=args.xbe,
            lifted=args.out_dir,
            install=args.install_shader_original,
            work=REPO_ROOT / "tmp" / "shader-original-work",
        )
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f"shader original: {error}", file=sys.stderr)
        return not args.install_shader_original
    for line in lines:
        print(line)
    return True


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    vendor_root: Path = args.vendor_root

    if args.command == "vendor":
        manifest = vendor_import(args.source, vendor_root)
        print(f"vendored {len(manifest.files)} files from {manifest.upstream_commit[:12]}")
        print(f"  into {vendor_root}")
        print("  paths: " + ", ".join(VENDORED_PATHS))
        return 0

    if args.command == "status":
        status = vendor_status(vendor_root)
        print(f"{vendor_root}: {status.value}")
        if status is VendorStatus.PATCHED:
            # Expected under the ADAPT verdict: the files we patch are recorded in
            # the manifest's `local` map, so this says "as intended", not "dirty".
            print("  carrying recorded local patches (see docs/lifter-patches/)")
        elif status is VendorStatus.MODIFIED:
            print("  WARNING: a file matches neither its upstream hash nor a recorded")
            print("  local patch. That is what an upstream change landing on top of")
            print("  one of our patches looks like. Reconcile before lifting.")
        return 0 if status is not VendorStatus.ABSENT else 1

    try:
        result = run_pipeline(
            args.xbe,
            args.out_dir,
            vendor_root,
            chunk_size=args.chunk_size,
            python=args.python,
            skip_existing=args.skip_existing,
            timeout=args.timeout,
            manual_functions=args.manual_functions,
            seed_functions=tuple(args.seed_functions or ()),
            only_functions=tuple(args.only_function),
            publish_eflags=args.publish_eflags,
            flag_bridge=args.flag_bridge,
            local_float_relations=args.local_float_relations,
        )
    except (LiftError, FileNotFoundError) as error:
        print(f"lift failed: {error}", file=sys.stderr)
        return 1

    for line in result.summary_lines():
        print(line)
    for stage in result.stages:
        print(f"stage {stage.name:<13} {stage.seconds:7.1f}s")
    if not _report_shader_original(args):
        return 1
    # A nonzero failure count means some guest functions have no body at all, so
    # the link will succeed and the program will be wrong. Say so in the exit code.
    return 1 if result.failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
