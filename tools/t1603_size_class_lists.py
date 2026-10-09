# SPDX-License-Identifier: GPL-3.0-or-later
"""T1603: static size-class screen. The T1520/T1534 'size over 300' cap is removed and every
other recognizer is the CURRENT one (pilot classify incl. T1594 vector whitelist, T1534 call
screen with live-closure and ABI recognizer, caller audit). Static only: no proof, no API call.

    python -m tools.t1603_size_class_lists --private-root /workspace \
        --profiles-root /workspace/tmp/owner-profiles --host-root /workspace/tmp/private-host \
        --scratch-dir tmp/t1603-raw --out-dir docs/data/t1603-size-class-lists
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
from pathlib import Path
from typing import Any

from tools import call_draft_lists as cdl
from tools import llm_replacement_draft as draft
from tools import name_additions as na
from tools import pilot_lists as pilot
from tools import t1601_ui_hud_mapedit_lists as t1601

TIERS = ((301, 500), (501, 800), (801, 1200), (1201, 1 << 30))
TIER_NAMES = ("301-500", "501-800", "801-1200", "over-1200")
LIST_SIZE = 20
MAX_LISTS = 4
MIN_TIER = 8
SMALL = 300


def tier_of(size: int) -> int:
    return next(i for i, (lo, hi) in enumerate(TIERS) if lo <= size <= hi)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--private-root", type=Path, required=True)
    ap.add_argument("--profiles-root", type=Path, required=True)
    ap.add_argument("--host-root", type=Path, required=True)
    ap.add_argument("--scratch-dir", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    args = ap.parse_args()
    os.setpriority(os.PRIO_PROCESS, 0, 19)
    private = args.private_root.resolve()
    pilot.SIZE_ALLOWED = 1 << 30  # the only change to the pilot rules: no size cap
    census = t1601.census_targets(args.profiles_root)
    sampled: set[int] = set()
    profile_files = 0
    if args.host_root.exists():
        sampled, profile_files = t1601.sampled_functions(args.profiles_root, args.host_root)
    world = na.World(private)
    md = draft._capstone()
    functions = draft.load_functions(private / "generated/retail/functions.csv", world.read)
    game = sorted(va for va in world.size if va not in world.library)
    big = [va for va in game if world.size[va] > SMALL]
    proven, failed = pilot.snapshot_sets()
    registered = draft.registered_vas(Path("src/game"))
    rejected = pilot.rejected_vas() | failed
    elsewhere = t1601.mentions(t1601.DRAFT_GLOBS)
    reasons: dict[int, str] = {}
    remaining: list[int] = []
    for va in big:
        if va in proven:
            reasons[va] = "proven"
        elif va in registered:
            reasons[va] = "registered"
        elif va in rejected:
            reasons[va] = "rejected-before"
        elif va in elsewhere:
            reasons[va] = "mentioned-in-draft-or-list-docs"
        else:
            remaining.append(va)
    text_range = (world.text_lo, world.text_hi)

    def pilot_reason(va: int) -> str | None:
        func = functions.get(va)
        if func is None:
            return "decode-mismatch"
        reason = pilot.classify(func, md, text_range)
        if reason is None:
            inputs = pilot.register_input(func, md)
            if "ecx" in inputs:
                reason = "implicit-ecx-input"
            elif inputs:
                reason = "register-input-abi"
            elif (
                pilot.stores_through_pointer(func)
                and sum(n.mnemonic in pilot.BITOPS for n in func.insns) >= 3
            ):
                reason = "pointer-writing-bit-packer"
            elif pilot.loop_with_memory(func):
                reason = "loop-with-memory-effects"
        if reason is None and draft.screen_function(func, md, pilot.MIN_INSNS, 10_000) is None:
            reason = "screen-function-reject"
        return reason

    survivors: list[int] = []
    for va in remaining:
        reason = pilot_reason(va)
        if reason:
            reasons[va] = reason
        else:
            survivors.append(va)
    facts = {
        va: draft.screen_function(functions[va], md, pilot.MIN_INSNS, 10_000) for va in survivors
    }
    verdicts = draft.audit_eligibility(
        [f for f in facts.values() if f is not None],
        private / "build/default.xbe",
        private / "generated/lifted/gen",
    )
    leaf_rows = []
    for va in survivors:
        if verdicts.get(va):
            leaf_rows.append(t1601.leaf_row(va, functions[va], facts[va], world))
        else:
            reasons[va] = "leaf-caller-audit-ineligible"
    call_pop = {
        va
        for va, why in reasons.items()
        if why in {"call", "implicit-ecx-input", "register-input-abi"}
    }
    original_init = na.World.__init__
    original_rejected = pilot.rejected_vas

    def init(self: Any, *a: Any, **k: Any) -> None:
        original_init(self, *a, **k)
        self.size = {va: s for va, s in self.size.items() if va in call_pop}

    na.World.__init__ = init  # type: ignore[method-assign]
    pilot.rejected_vas = lambda: set()  # population already filtered above
    try:
        cdl.run(private, args.scratch_dir)
    finally:
        na.World.__init__ = original_init  # type: ignore[method-assign]
        pilot.rejected_vas = original_rejected
    report = json.loads((args.scratch_dir / "report.json").read_text())
    for row in report["dispositions"]:
        if row["status"] == "refused":
            reasons[int(row["va"], 16)] = "call-screen:" + re.sub(
                r"0x[0-9a-fA-F]+", "VA", row["reason"]
            )
    call_rows = []
    for path in sorted(args.scratch_dir.glob("list-*.json")):
        call_rows += json.loads(path.read_text())["functions"]
    executed = census | sampled
    eligible = leaf_rows + call_rows
    for row in eligible:
        row["executed_in_owner_sessions"] = int(row["va"], 16) in executed
        row["tier"] = TIER_NAMES[tier_of(row["size"])]
    eligible.sort(
        key=lambda r: (
            (not r["executed_in_owner_sessions"], r["stratum"] != "leaf") + cdl.rank_key(r)
        )
    )
    out = args.out_dir
    out.mkdir(parents=True, exist_ok=True)
    tier_counts = {name: 0 for name in TIER_NAMES}
    by_tier_exec = {name: 0 for name in TIER_NAMES}
    for row in eligible:
        tier_counts[row["tier"]] += 1
        by_tier_exec[row["tier"]] += row["executed_in_owner_sessions"]
    written = []
    for name in TIER_NAMES:
        rows = [r for r in eligible if r["tier"] == name]
        if len(rows) < MIN_TIER:
            continue
        for number in range(MAX_LISTS):
            chunk = rows[number * LIST_SIZE : (number + 1) * LIST_SIZE]
            if not chunk:
                break
            path = out / f"tier-{name}-list-{number + 1:03d}.json"
            path.write_text(
                json.dumps(
                    {
                        "tier": name,
                        "number": number + 1,
                        "partial": len(chunk) < LIST_SIZE,
                        "count": len(chunk),
                        "functions": chunk,
                        "provenance": {
                            "status": "static draft eligibility only; no proof/admission"
                        },
                    },
                    indent=2,
                    sort_keys=True,
                )
                + "\n"
            )
            written.append({"path": str(path), "count": len(chunk)})
    eligible_vas = {int(r["va"], 16) for r in eligible}
    brief = [
        {k: r[k] for k in ("va", "size", "tier", "stratum", "executed_in_owner_sessions")}
        for r in eligible
    ]
    (out / "eligible-all-tiers.json").write_text(json.dumps(brief, indent=2) + "\n")
    funnel_by_tier: dict[str, collections.Counter[str]] = {
        n: collections.Counter() for n in TIER_NAMES
    }
    for va in big:
        name = TIER_NAMES[tier_of(world.size[va])]
        funnel_by_tier[name][
            "static-draft-eligible" if va in eligible_vas else reasons.get(va, "?")
        ] += 1
    summary = {
        "game_functions": len(game),
        "over_300": len(big),
        "executed_set": len(executed),
        "sampled_functions": len(sampled),
        "profile_files": profile_files,
        "eligible_by_tier": tier_counts,
        "eligible_executed_by_tier": by_tier_exec,
        "funnel_by_tier": {n: dict(sorted(c.items())) for n, c in funnel_by_tier.items()},
        "lists": written,
    }
    (out / "report.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
