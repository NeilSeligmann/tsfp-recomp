# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only audit of function table rows whose span (`body_max_va`) is implausibly wide.

    python -m tools.function_span_audit [--functions generated/retail/functions.csv]
        [--threshold 256] [--layered] [--csv OUT.csv] [--limit 20]

Ghidra exports `size_bytes` (an address count) and `body_max_va` (highest address of the
body). A fragmented body can legitimately have a gap, but a span far beyond entry+size
that swallows other table entries makes `apply_function_additions` refuse every addition
inside it and inflates the extent coverage counts. A row is flagged when its excess
(`body_max_va - (entry + size - 1)`) exceeds the threshold, or when its span holds other
table entries. Causes: `wide-and-contains-entries`, `wide-only`, `contains-entries-only`.
Nothing is written unless `--csv` is given. Paths are repo relative.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from tools.codediff.boundaries import Function, load_function_table, load_functions

DEFAULT_FUNCTIONS = Path("generated/retail/functions.csv")
CAUSES = ("wide-and-contains-entries", "wide-only", "contains-entries-only")


@dataclass(frozen=True)
class SpanFinding:
    entry_va: int
    size_bytes: int
    body_max_va: int
    excess: int
    contained: int
    cause: str


def audit(functions: list[Function], threshold: int) -> list[SpanFinding]:
    """Flagged rows, ascending by entry. `contained` counts other entries inside the span."""
    entries = sorted({f.entry_va for f in functions})
    findings: list[SpanFinding] = []
    for function in functions:
        last = function.entry_va + function.size_bytes - 1
        excess = max(0, function.body_max_va - last)
        low = bisect.bisect_right(entries, function.entry_va)
        high = bisect.bisect_right(entries, max(function.body_max_va, last))
        contained = high - low
        wide = excess > threshold
        if not wide and contained == 0:
            continue
        if wide and contained:
            cause = CAUSES[0]
        elif wide:
            cause = CAUSES[1]
        else:
            cause = CAUSES[2]
        findings.append(
            SpanFinding(
                function.entry_va,
                function.size_bytes,
                function.body_max_va,
                excess,
                contained,
                cause,
            )
        )
    return sorted(findings, key=lambda finding: finding.entry_va)


def count_by_cause(findings: list[SpanFinding]) -> dict[str, int]:
    counts = Counter(finding.cause for finding in findings)
    return {cause: counts.get(cause, 0) for cause in CAUSES}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--functions", type=Path, default=DEFAULT_FUNCTIONS)
    parser.add_argument("--threshold", type=int, default=256, help="excess bytes counted wide")
    parser.add_argument(
        "--layered",
        action="store_true",
        help="audit the shared table after overrides and additions (default: raw export)",
    )
    parser.add_argument("--csv", type=Path, help="write the findings here")
    parser.add_argument("--limit", type=int, default=20, help="widest rows to print")
    args = parser.parse_args(argv)
    functions = (
        load_function_table(args.functions) if args.layered else load_functions(args.functions)
    )
    findings = audit(functions, args.threshold)
    print(f"rows={len(functions)} flagged={len(findings)} threshold={args.threshold}")
    for cause, count in count_by_cause(findings).items():
        print(f"  {cause}: {count}")
    widest = sorted(findings, key=lambda finding: -finding.excess)[: args.limit]
    for finding in widest:
        print(
            f"  {finding.entry_va:#010x} size={finding.size_bytes} "
            f"body_max={finding.body_max_va:#010x} excess={finding.excess} "
            f"contained={finding.contained} {finding.cause}"
        )
    if args.csv:
        with args.csv.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(
                ["entry_va", "size_bytes", "body_max_va", "excess", "contained", "cause"]
            )
            for finding in findings:
                writer.writerow(
                    [
                        f"{finding.entry_va:#010x}",
                        finding.size_bytes,
                        f"{finding.body_max_va:#010x}",
                        finding.excess,
                        finding.contained,
                        finding.cause,
                    ]
                )
    return 0


if __name__ == "__main__":
    sys.exit(main())
