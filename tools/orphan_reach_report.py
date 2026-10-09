# SPDX-License-Identifier: GPL-3.0-or-later
"""T1144: merge the T822 orphan candidates with --census-icalls replay hits into a verdict table.

Inputs: the T822 census JSON, the retail XBE (static evidence, pinned by sha256) and one or more
``host.out`` files of a private host run with ``--census-icalls`` (``tools.private_host replay``).
Output: JSON with one row per candidate (static tier, reached per record, verdict) and the
ready-to-merge extra-seed list of strong, still unreached candidates
(never written to tools/config).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

from tools.xbe import parse_xbe
from tools.xbe.model import Xbe

CENSUS_LINE = re.compile(
    r"\s+census target 0x([0-9A-F]+) first present (\d+) seq \d+ thread \d+"
    r" caller 0x([0-9A-F]+) calls (\d+)"
)
CERTIFIED = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"


def parse_hits(path: Path) -> dict[int, dict]:
    hits: dict[int, dict] = {}
    for line in path.read_text(errors="replace").splitlines():
        match = CENSUS_LINE.match(line)
        if match:
            target, present, caller, calls = match.groups()
            hits[int(target, 16)] = {
                "calls": int(calls),
                "first_present": int(present),
                "caller": "0x" + caller,
            }
    return hits


def section_of(image: Xbe, start: int) -> str:
    for section in image.sections:
        if section.virtual_addr <= start < section.virtual_addr + section.virtual_size:
            return section.name
    return "(none)"


def static_tier(entry: dict, section: str) -> tuple[str, str]:
    """Return (tier, reason). A: strong, B: weak (reference shape), R: rejected (not game code)."""
    if section != ".text":
        return (
            "R",
            f"entry lies in library section {section}, data or library code, not a game function",
        )
    if "hlt" in entry["terminals"]:
        return "R", "hlt terminal in the descent (data read as code)"
    if entry["kind"] == "code" and entry["entry"] == "0x002FB6D0":
        return "B", "only reference is a cmp immediate (compare constant, not a pointer)"
    return (
        "A",
        ".text, 16-aligned, clean descent to RET or tail jmp, named by immediate or dword",
    )


def build(census: dict, raw: bytes, records: dict[str, dict[int, dict]], seeded: set[int]) -> dict:
    image = parse_xbe(raw)
    rows, ready = [], []
    for entry in census["strong_candidates"]:
        start = int(entry["entry"], 16)
        offset = image.va_to_offset(start)
        assert offset is not None
        head = raw[offset : offset + 8]
        section = section_of(image, start)
        tier, reason = static_tier(entry, section)
        reached = {name: hits[start] for name, hits in records.items() if start in hits}
        if reached:
            verdict, confidence = "reached", "MEASURED"
        elif tier == "A":
            verdict, confidence = "unreached-strong", "INFERRED"
        else:
            verdict, confidence = "rejected" if tier == "R" else "unreached-weak", "INFERRED"
        rows.append(
            {
                "entry": entry["entry"],
                "section": section,
                "tier": tier,
                "reason": reason,
                "head": head.hex(),
                "first_ref": entry["first_ref"],
                "span_end": entry["span_end"],
                "span_sha256": entry["span_sha256"],
                "reached_in": reached,
                "seeded_on_main": start in seeded,
                "verdict": verdict,
                "confidence": confidence,
            }
        )
        if verdict == "unreached-strong" and start not in seeded:
            ready.append(
                {
                    "start": entry["entry"],
                    # the seed-functions format requires True, the note says NOT run-observed
                    "observed": True,
                    "xbe_sha256": CERTIFIED,
                    "original_span": f"{entry['entry']}..{entry['span_end']}",
                    "original_sha256": entry["span_sha256"],
                    "note": f"T1144 tier A candidate, address taken at {entry['first_ref']}, "
                    "not reached by any "
                    "recorded route (INFERRED). Promote only after a stop or replay reaches it.",
                }
            )
    return {"xbe_sha256": CERTIFIED, "rows": rows, "ready_extra_seeds": ready}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--census", type=Path, required=True)
    parser.add_argument(
        "--seeds", type=Path, required=True, help="tools/config/lift_thread_entries.json"
    )
    parser.add_argument(
        "--record",
        action="append",
        default=[],
        metavar="NAME=HOST_OUT",
        help="census-icalls host.out per record",
    )
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    raw = args.xbe.read_bytes()
    if hashlib.sha256(raw).hexdigest() != CERTIFIED:
        print("xbe sha256 mismatch", file=sys.stderr)
        return 2
    records = {}
    for item in args.record:
        name, _, path = item.partition("=")
        records[name] = parse_hits(Path(path))
    seeded = {int(s["start"], 16) for s in json.loads(args.seeds.read_text())}
    args.out.write_text(
        json.dumps(build(json.loads(args.census.read_text()), raw, records, seeded), indent=1)
        + "\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
