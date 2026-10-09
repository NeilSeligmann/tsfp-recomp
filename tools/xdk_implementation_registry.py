# SPDX-License-Identifier: GPL-3.0-or-later
"""Produce the `xdk-implementation-registry` artifact read by tools/coverage.py.

Output: generated/<build>/xdk-implementation-registry.json (kind
``xdk-implementation-registry``). It joins three sources, per surface address:

  1. the native ``xdk_report`` registration JSON (registered handler or not),
  2. the static refusal extraction of ``tools.xdk_coverage`` (refusal bucket),
  3. the tracked verification ledger ``tools/data/xdk_verified_implementations.json``.

``verified_full_implementation_count`` is the coverage numerator. It counts ONLY
ledger entries that name an address on the measured surface and carry evidence.
Registration and "no known refusals" never count: neither proves every original
branch is modelled (T15, T120). Nothing here is inferred or fabricated.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from tools.xdk_coverage import classify, load_report

KIND = "xdk-implementation-registry"
LEDGER = Path("tools/data/xdk_verified_implementations.json")


def read_ledger(path: Path, surface: set[int]) -> dict[int, str]:
    """address -> evidence for every valid ledger entry; invalid entries are errors."""
    data = json.loads(path.read_text(encoding="utf-8"))
    verified: dict[int, str] = {}
    for entry in data.get("verified", []):
        address = int(entry["address"], 16)
        evidence = str(entry.get("evidence", "")).strip()
        if address not in surface:
            raise SystemExit(f"ledger address {entry['address']} is not on the measured surface")
        if not evidence:
            raise SystemExit(f"ledger address {entry['address']} has no evidence")
        verified[address] = evidence
    return verified


def build_registry(root: Path, report: dict, ledger: Path) -> dict:
    coverage = classify(root, report)
    classes = {int(h["address"], 16): h["classification"] for h in coverage["handlers"]}
    surface = {int(e["address"], 16) for e in report["entries"]}
    verified = read_ledger(ledger, surface)
    entries = []
    for entry in sorted(report["entries"], key=lambda e: int(e["address"], 16)):
        address = int(entry["address"], 16)
        entries.append(
            {
                "address": entry["address"],
                "section": entry["section"],
                "module": entry.get("module"),
                "registered_handler": bool(entry["registered_handler"]),
                "refusal_class": classes.get(address, "unregistered"),
                "verified_full_implementation": address in verified,
                "evidence": verified.get(address),
            }
        )
    return {
        "schema": 1,
        "kind": KIND,
        "surface_count": len(entries),
        "registered_handler_count": coverage["registered_handler_count"],
        "buckets": coverage["buckets"],
        "verified_full_implementation_count": len(verified),
        "entries": entries,
        "caveats": coverage["caveats"],
    }


def main(argv: list[str] | None = None) -> int:
    root_default = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=root_default)
    parser.add_argument("--build", default="retail", help="generated/<build> subtree")
    parser.add_argument(
        "--xdk-report-binary",
        type=Path,
        default=None,
        help="native xdk_report (default: ROOT/build/xdk_report)",
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=None,
        help="existing xdk-handler-registration JSON instead of running the binary",
    )
    parser.add_argument("--ledger", type=Path, default=None)
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args(argv)
    out = args.out or args.root / "generated" / args.build / f"{KIND}.json"
    ledger = args.ledger or args.root / LEDGER
    if args.report is not None:
        report = load_report(args.report)
    else:
        binary = args.xdk_report_binary or args.root / "build" / "xdk_report"
        if not binary.exists():
            print(
                f"xdk_implementation_registry: {binary} missing; build xdk_report", file=sys.stderr
            )
            return 2
        proc = subprocess.run(
            [str(binary)], capture_output=True, text=True, check=True, timeout=120
        )
        report = json.loads(proc.stdout)
        if report.get("kind") != "xdk-handler-registration":
            raise SystemExit("xdk_report produced an unexpected document")
    registry = build_registry(args.root, report, ledger)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(registry, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        f"{out}: {registry['verified_full_implementation_count']} verified of "
        f"{registry['surface_count']} surface, {registry['registered_handler_count']} registered"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
