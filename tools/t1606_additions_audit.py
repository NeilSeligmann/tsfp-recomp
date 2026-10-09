# SPDX-License-Identifier: GPL-3.0-or-later
"""T1606: why does the caller audit refuse function_additions.csv functions?

Re-runs the static pilot stages on every addition with a loader that merges the additions
(load_function_table, as tools/coverage.py does), audits the survivors through
tools.replace.audit, and independently scans the XBE text for `call rel32`, `jmp rel32` and
`jcc rel32` to each survivor plus every 4 byte little-endian occurrence of its address.
Writes addresses and counts only.

    python -m tools.t1606_additions_audit --private-root /workspace \\
        --out docs/evidence/t1606-additions-audit/survivors.json
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import struct
import tempfile
from pathlib import Path

from tools import llm_replacement_draft as draft
from tools import name_additions as na
from tools import pilot_lists as pilot
from tools.harness.image import build_guest_image
from tools.replace import audit as audit_module
from tools.replace.manifest import ManifestEntry


def raw_branches(code: bytes, low: int, targets: set[int]) -> dict[str, dict[int, list[int]]]:
    """Branches to `targets` found by a byte scan (over-approximates: no instruction sync)."""
    found: dict[str, dict[int, list[int]]] = {"call": {}, "jmp": {}, "jcc": {}}
    patterns = (
        ("call", rb"\xe8", 1, 5),
        ("jmp", rb"\xe9", 1, 5),
        ("jcc", rb"\x0f[\x80-\x8f]", 2, 6),
    )
    for kind, pattern, rel_at, length in patterns:
        for match in re.finditer(pattern, code):
            at = match.start()
            if at + length > len(code):
                continue
            target = (
                low + at + length + struct.unpack_from("<i", code, at + rel_at)[0]
            ) & 0xFFFFFFFF
            if target in targets:
                found[kind].setdefault(target, []).append(low + at)
    return found


def survivors(
    world: na.World, funcs: dict[int, draft.Function]
) -> tuple[list[int], dict[int, str]]:
    md = draft._capstone()
    pilot.SIZE_ALLOWED = 1 << 30  # T1603 removed the size cap
    proven, failed = pilot.snapshot_sets()
    skip = draft.registered_vas(Path("src/game")) | proven | pilot.rejected_vas() | failed
    reasons: dict[int, str] = {}
    kept: list[int] = []
    for va in sorted(world.additions):
        if va in world.library:
            reasons[va] = "library"
        elif va in skip:
            reasons[va] = "proven-registered-or-rejected"
        else:
            reason = pilot.classify(funcs[va], md, (world.text_lo, world.text_hi))
            if reason is None:
                regs = pilot.register_input(funcs[va], md)
                if "ecx" in regs:
                    reason = "implicit-ecx-input"
                elif regs:
                    reason = "register-input-abi"
                elif pilot.stores_through_pointer(funcs[va]) and (
                    sum(n.mnemonic in pilot.BITOPS for n in funcs[va].insns) >= 3
                ):
                    reason = "pointer-writing-bit-packer"
                elif pilot.loop_with_memory(funcs[va]):
                    reason = "loop-with-memory-effects"
            if (
                reason is None
                and draft.screen_function(funcs[va], md, pilot.MIN_INSNS, 10_000) is None
            ):
                reason = "screen-function-reject"
            if reason:
                reasons[va] = reason
            else:
                kept.append(va)
    return kept, reasons


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--private-root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    root = args.private_root
    world = na.World(root)
    with tempfile.TemporaryDirectory() as scratch:
        merged = Path(scratch) / "functions.csv"
        with merged.open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(["entry_va", "size_bytes"])
            for va, size in sorted(world.size.items()):
                writer.writerow([f"0x{va:08x}", size])
        funcs = draft.load_functions(merged, world.read)
    kept, reasons = survivors(world, funcs)
    md = draft._capstone()
    facts = {va: draft.screen_function(funcs[va], md, pilot.MIN_INSNS, 10_000) for va in kept}
    entries = [
        ManifestEntry(
            va=va,
            name="hypothetical",
            convention="stdcall" if fact["ret_pop"] else "cdecl",
            stack_args=fact["stack_args_hint"],
            returns="eax",
            scratch=("ecx", "edx"),
            source="hypothetical.c",
        )
        for va, fact in facts.items()
    ]
    image = build_guest_image(root / "build/default.xbe")
    audits = audit_module.audit_all(entries, root / "generated/lifted/gen", image)
    index = audit_module.build_caller_index(root / "generated/lifted/gen")
    offset = world.xbe.va_to_offset(world.text_lo)
    code = world.data[offset : offset + world.text_hi - world.text_lo]
    every = set(world.additions)
    raw = raw_branches(code, world.text_lo, every)
    # caller-input completeness over ALL additions: byte scan sites vs lifted-tree sites
    sites_equal = all(
        {at + 5 for at in raw["call"].get(va, [])} == index.sites.get(va, set()) for va in every
    )
    rows = []
    for item in audits:
        needle = struct.pack("<I", item.va)
        refs, pos = [], image.data.find(needle)
        while pos != -1:
            refs.append(image.base + pos)
            pos = image.data.find(needle, pos + 1)
        starts = sorted((sec.virtual_addr, sec.name) for sec in world.xbe.sections)
        rows.append(
            {
                "va": f"0x{item.va:08x}",
                "size": world.size[item.va],
                "audit_direct_sites": item.direct_sites,
                "audit_tail_jumps": item.tail_jumps,
                "audit_data_references": item.data_references,
                "audit_unresolved": item.unresolved,
                "raw_call_sites": len(raw["call"].get(item.va, [])),
                "raw_jmp_or_jcc": len(raw["jmp"].get(item.va, []))
                + len(raw["jcc"].get(item.va, [])),
                "data_ref_sections": sorted(
                    {
                        max((a, n) for a, n in starts if a <= ref)[1]
                        for ref in refs
                        if ref >= starts[0][0]
                    }
                ),
                "eligible": bool(
                    item.eligible
                    and item.direct_sites >= 1
                    and item.tail_jumps == 0
                    and item.data_references == 0
                    and item.unresolved == 0
                ),
            }
        )
    summary = {
        "additions": len(every),
        "additions_defined_in_lifted_tree": len(every & set(index.entries)),
        "additions_with_direct_callers": sum(1 for va in every if index.sites.get(va)),
        "byte_scan_sites_equal_lifted_sites_for_all_additions": sites_equal,
        "stage_refusals": {
            k: sum(1 for v in reasons.values() if v == k) for k in sorted(set(reasons.values()))
        },
        "survivors": len(kept),
        "eligible": [r["va"] for r in rows if r["eligible"]],
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({"summary": summary, "survivors": rows}, indent=1) + "\n")
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
