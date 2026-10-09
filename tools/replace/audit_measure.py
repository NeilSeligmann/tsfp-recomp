"""Measure the caller audit for hypothetical replacement roots (T1252/T1672 helper).

Audits arbitrary guest VAs as if they were registered (cdecl, scratch ecx/edx) and prints the
per-site reasons, so drafted roots that are not in the registry can be measured. Only the audit
runs, nothing is proven.
"""

from __future__ import annotations

import argparse
import collections
import json
import re
from pathlib import Path

from tools.harness.image import build_guest_image

from . import audit as audit_module
from .manifest import ManifestEntry

DEFAULT_ROOTS = "150830:3,151920:4,151e80:2,151fc0:3,152090:2,1521d0:2,150920:2,151da0:2"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    parser.add_argument("--gen-dir", type=Path, default=Path("generated/lifted/gen"))
    parser.add_argument("--functions", type=Path, default=Path("generated/retail/functions.csv"))
    parser.add_argument("--roots", default=DEFAULT_ROOTS, help="hex VA:stack_args,...")
    parser.add_argument("--out", type=Path, help="write a JSON summary")
    args = parser.parse_args()
    entries = []
    for item in args.roots.split(","):
        va_text, count = item.split(":")
        entries.append(
            ManifestEntry(
                int(va_text, 16),
                f"root_{va_text}",
                "cdecl",
                int(count),
                "eax",
                ("ecx", "edx"),
                "measure",
            )
        )
    image = build_guest_image(args.xbe)
    from .bridge import BridgeExemption
    from .straddle_corpus import data_ranges

    ranges = [(low, high) for _, low, high in data_ranges(args.xbe)]
    bridge = BridgeExemption.open(args.xbe, args.gen_dir, Path("src/game"))
    results = audit_module.audit_all(
        entries, args.gen_dir, image, data_ranges=ranges, bridge=bridge, functions=args.functions
    )
    summary = {}
    for item in results:
        counts = collections.Counter(
            re.sub(r"(0x)?[0-9A-Fa-f]{5,8}\b", "ADDR", v.reason)
            for v in item.flags
            if v.verdict != "safe"
        )
        states = collections.Counter(v.verdict for v in item.flags)
        print(f"0x{item.va:08x} eligible={item.eligible} sites={item.direct_sites} {dict(states)}")
        for reason, count in counts.most_common(12):
            print(f"   {count:5d} {reason[:260]}")
        summary[f"0x{item.va:08x}"] = {"eligible": item.eligible, "reasons": dict(counts)}
    if args.out:
        args.out.write_text(json.dumps(summary, indent=1, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
