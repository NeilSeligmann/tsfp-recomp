# SPDX-License-Identifier: GPL-3.0-or-later
"""Authenticate the T1776 extent candidates against the original XBE and merged entries."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from tools.codediff.boundaries import load_function_table, load_functions
from tools.harness.image import build_guest_image
from tools.llm_replacement_draft import _capstone


def check_span(va: int, size: int, code: bytes, entries: set[int]) -> str | None:
    """Refuse incomplete bytes, overlapping entries, partial decode or non-return endings."""
    if size <= 0 or len(code) != size:
        return "incomplete-bytes"
    if any(va < entry < va + size for entry in entries):
        return "contains-entry"
    insns = list(_capstone().disasm(code, va))
    cursor = va
    for insn in insns:
        if insn.address != cursor:
            return "decode-gap"
        cursor += insn.size
    if cursor != va + size:
        return "partial-decode"
    if not insns or insns[-1].mnemonic not in ("ret", "retn"):
        return "not-ret"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    parser.add_argument("--functions", type=Path, default=Path("generated/retail/functions.csv"))
    parser.add_argument(
        "--candidates",
        type=Path,
        default=Path("docs/data/t1772-replacement-census/extent-repair-candidates.json"),
    )
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    candidates = json.loads(args.candidates.read_text())
    raw = {f.entry_va: f for f in load_functions(args.functions)}
    merged = {f.entry_va: f for f in load_function_table(args.functions)}
    entries = set(merged)
    image = build_guest_image(args.xbe)
    rows = []
    for row in candidates["candidates"]:
        va, size = int(row["va"], 16), row["contiguous_span_bytes"]
        original = raw.get(va)
        failure = None
        if (
            original is None
            or original.size_bytes != row["table_size_bytes"]
            or original.body_max_va != va + size - 1
        ):
            failure = "raw-table-identity"
        elif va not in merged or merged[va].size_bytes not in (original.size_bytes, size):
            failure = "conflicting-override"
        code = image.code_at(va, size)
        failure = failure or check_span(va, size, code, entries)
        rows.append(
            {
                **row,
                "body_sha256": hashlib.sha256(code).hexdigest(),
                "end_va": f"0x{va + size - 1:08X}",
                "failure": failure,
            }
        )
    result = {
        "confidence": "MEASURED original-instruction decode (not xemu)",
        "xbe_sha256": hashlib.sha256(args.xbe.read_bytes()).hexdigest(),
        "candidates_sha256": hashlib.sha256(args.candidates.read_bytes()).hexdigest(),
        "count": len(rows),
        "passed": all(r["failure"] is None for r in rows),
        "rows": rows,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(f"checked={len(rows)} failures={sum(r['failure'] is not None for r in rows)}")
    return 0 if result["passed"] and len(rows) == candidates["count"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
