# SPDX-License-Identifier: GPL-3.0-or-later
"""T1601: static drafting screen for game functions of the menu/ui, hud and map editor parts that
were seen EXECUTING in the owner sessions. Static only: no proof, no admission, no API call.

    python -m tools.t1601_ui_hud_mapedit_lists --private-root /workspace \
        --profiles-root /workspace/tmp/owner-profiles --host-root /workspace/tmp/private-host \
        --scratch-dir tmp/t1601-raw --out-dir docs/data/t1598-ui-hud-mapedit-lists

Stages, each function counted once at its first exclusion:
  executed (census targets + sampler samples mapped to the enclosing `sub_VA`) and in a target part
  -> proven | registered | rejected-before | mentioned-elsewhere
  -> pilot leaf screen (tools.pilot_lists.classify, incl. the T1594 vector whitelist)
  -> call / implicit-ecx / register-input strata through the T1534 call screen with the CURRENT
     recognizers (tools.call_draft_lists.run), including its caller audit
  -> leaf strata: pointer-writing-bit-packer, loop-with-memory, screen_function, caller audit.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
from pathlib import Path
from typing import Any

from tools import call_draft_lists as cdl
from tools import cpu_profile_report as rep
from tools import llm_replacement_draft as draft
from tools import name_additions as na
from tools import pilot_lists as pilot
from tools.coverage_by_subsystem import part_of_name

PARTS = ("menu and ui", "hud", "map editor")
SKIP_SUFFIXES = (".ms", ".pid", ".ack", ".phase")
LIST_SIZE = 20
# Drafting and rejection lineage only. Census, rename, lift and refusal-inventory artefacts
# (t710, t1473 renames, t634/t643 lift, t1513 precision, t1557 inventory) name VAs without
# drafting or rejecting them and are tallied separately as "named-elsewhere-info".
DRAFT_GLOBS = (
    "docs/data/t1534-call-draft-lists/**/*",
    "docs/data/t-pilot-lists/**/*",
    "docs/data/t1541-list002/**/*",
    "docs/data/t1544*",
    "docs/data/t1560-batch-prove-sample/**/*",
    "docs/data/t1578-newly-reachable-lists/**/*",
    "docs/data/t1583-reach-list001/**/*",
    "docs/data/t1587-af-kill-lists/**/*",
    "docs/data/t1588-af-list001/**/*",
    "docs/data/t1595-vec-list001/**/*",
    "docs/data/t1595-vector-lists/**/*",
    "docs/data/t1596-vec-list001b/**/*",
    "docs/data/t-gemini-jev-pilot/**/*",
    "docs/t-*list*.md",
    "docs/t-*draft*.md",
    "docs/t-semantic-review-drafted-bodies.md",
    "docs/t1542*",
    "docs/t1544*",
    "docs/evidence/t154[3-9]/**/*",
    "docs/evidence/t155[0-6]/**/*",
    "docs/evidence/t15[6-9]*/**/*",
)
INFO_GLOBS = ("docs/data/**/*", "docs/evidence/**/*")
T1534_POPULATION = {
    "pre-audit-dispositions.json",
    "report.json",
    "excluded-elsewhere.json",
    "pins-before.json",
    "pins-after.json",
}
HEX = re.compile(r"(?:0x|sub_)([0-9a-fA-F]{5,8})\b")


def census_targets(root: Path) -> set[int]:
    found: set[int] = set()
    for path in sorted(root.glob("*/census.txt.*")):
        if path.name.endswith(SKIP_SUFFIXES):
            continue
        for line in path.read_text(errors="ignore").splitlines():
            match = re.match(r"target 0x([0-9A-Fa-f]+)", line)
            if match:
                found.add(int(match.group(1), 16))
    return found


def sampled_functions(root: Path, host_root: Path) -> tuple[set[int], int]:
    """Enclosing guest functions of each profile innermost sample (addr2line on the host)."""
    offsets: dict[str, set[int]] = collections.defaultdict(set)
    files = 0
    for path in sorted(root.glob("*/profile.txt*")):
        if path.name.endswith(SKIP_SUFFIXES):
            continue
        files += 1
        base, exe, samples = rep.load(path)
        for _tid, rip in samples:
            if 0 <= rip - base < (1 << 28):
                offsets[exe].add(rip - base)
    found: set[int] = set()
    for exe, offs in offsets.items():
        local = host_root / Path(exe).parent.name / "tsfp_host"
        for name in rep.resolve(str(local), sorted(offs)).values():
            match = re.fullmatch(r"sub_([0-9A-Fa-f]{8})", name)
            if match:
                found.add(int(match.group(1), 16))
    return found, files


def mentions(globs: tuple[str, ...]) -> dict[int, str]:
    """VA -> first artefact naming it (own outputs and T1534 population files excluded)."""
    files: set[Path] = set()
    for pattern in globs:
        files |= {p for p in Path(".").glob(pattern) if p.is_file()}
    found: dict[int, str] = {}
    for path in sorted(files):
        text_path = str(path)
        if text_path.endswith(("replace-proof-snapshot.json", "coverage-by-subsystem.csv")):
            continue
        if "t1598-ui-hud-mapedit" in text_path:
            continue
        if path.name in T1534_POPULATION and ("t1534" in text_path or "t1578" in text_path):
            continue
        for match in HEX.finditer(path.read_text(errors="ignore")):
            found.setdefault(int(match.group(1), 16), text_path)
    return found


def leaf_row(va: int, func: Any, facts: dict[str, Any], world: Any) -> dict[str, Any]:
    code = world.read(va, func.size)
    return {
        "va": f"0x{va:08x}",
        "stratum": "leaf",
        "name": f"sub_{va:08X}",
        "known_name": world.names.get(va),
        "status": "static-draft-eligible-NOT-proof",
        "size": func.size,
        "bytes": code.hex(),
        "sha256": hashlib.sha256(code).hexdigest(),
        "meaningful_insns": sum(i.mnemonic not in {"nop", "int3"} for i in func.insns),
        "closure_nodes": 1,
        "closure_insns": len(func.insns),
        "call_sites": 0,
        "simplicity": pilot.simplicity(func),
        "stack_args_hint": facts["stack_args_hint"],
        "ret_pop": facts["ret_pop"],
        "disassembly": draft.disassembly_text(func),
        "recipe": {
            "live_call_closure": False,
            "count": 600,
            "seeds": [20261001, 20261006],
            "optimizations": [0, 3],
        },
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--private-root", type=Path, required=True)
    ap.add_argument("--profiles-root", type=Path, required=True)
    ap.add_argument("--host-root", type=Path, required=True)
    ap.add_argument("--scratch-dir", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    args = ap.parse_args()
    private = args.private_root.resolve()
    census = census_targets(args.profiles_root)
    sampled, profile_files = sampled_functions(args.profiles_root, args.host_root)
    world = na.World(private)
    md = draft._capstone()
    functions = draft.load_functions(private / "generated/retail/functions.csv", world.read)

    def part(va: int) -> str:
        return part_of_name(world.names.get(va, f"sub_{va:08X}"))

    executed = {
        va
        for va in census | sampled
        if va in world.size and va not in world.library and part(va) in PARTS
    }
    stages: collections.Counter[str] = collections.Counter()
    by_part: dict[str, collections.Counter[str]] = {p: collections.Counter() for p in PARTS}
    proven, failed = pilot.snapshot_sets()
    registered = draft.registered_vas(Path("src/game"))
    rejected = pilot.rejected_vas() | failed
    elsewhere = mentions(DRAFT_GLOBS)
    info = mentions(INFO_GLOBS)
    remaining: list[int] = []
    for va in sorted(executed):
        by_part[part(va)]["executed"] += 1
        if va in proven:
            reason = "proven"
        elif va in registered:
            reason = "registered"
        elif va in rejected:
            reason = "rejected-before"
        elif va in elsewhere:
            reason = "mentioned-elsewhere"
        else:
            reason = ""
        if reason:
            stages[reason] += 1
            by_part[part(va)][reason] += 1
        else:
            remaining.append(va)
    # Pilot leaf screen over the remaining functions (mirrors tools.pilot_lists.run).
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

    # Informational: the pilot stage of EVERY unproven executed function, before the
    # registered/rejected/mentioned exclusions (the "drafting pool" of the measurement stage).
    pool_strata = {"call", "implicit-ecx-input", "register-input-abi"}
    pre_exclusion: dict[str, collections.Counter[str]] = {p: collections.Counter() for p in PARTS}
    for va in sorted(executed - proven):
        pre_exclusion[part(va)][pilot_reason(va) or "leaf-survivor"] += 1
    reasons: dict[int, str] = {}
    survivors: list[int] = []
    for va in remaining:
        reason = pilot_reason(va)
        if reason:
            reasons[va] = reason
        else:
            survivors.append(va)
    facts_by_va = {
        va: draft.screen_function(functions[va], md, pilot.MIN_INSNS, 10_000) for va in survivors
    }
    verdicts = draft.audit_eligibility(
        [f for f in facts_by_va.values() if f is not None],
        private / "build/default.xbe",
        private / "generated/lifted/gen",
    )
    leaf_rows = []
    for va in survivors:
        if verdicts.get(va):
            leaf_rows.append(leaf_row(va, functions[va], facts_by_va[va], world))
        else:
            reasons[va] = "leaf-caller-audit-ineligible"
    # Call-bearing and register-input strata: the T1534 screen with the current recognizers.
    call_pop = {
        va
        for va, why in reasons.items()
        if why in {"call", "implicit-ecx-input", "register-input-abi"}
    }
    pins_out = args.scratch_dir
    original_init = na.World.__init__
    original_rejected = pilot.rejected_vas

    def init(self: Any, *a: Any, **k: Any) -> None:
        original_init(self, *a, **k)
        self.size = {va: s for va, s in self.size.items() if va in call_pop}

    na.World.__init__ = init  # type: ignore[method-assign]
    pilot.rejected_vas = lambda: set()  # population already filtered above
    try:
        cdl.run(private, pins_out)
    finally:
        na.World.__init__ = original_init  # type: ignore[method-assign]
        pilot.rejected_vas = original_rejected
    report = json.loads((pins_out / "report.json").read_text())
    call_rows: list[dict[str, Any]] = []
    for row in report["dispositions"]:
        va = int(row["va"], 16)
        if row["status"] == "refused":
            reasons[va] = "call-screen:" + re.sub(r"0x[0-9a-fA-F]+", "VA", row["reason"])
    for path in sorted(pins_out.glob("list-*.json")):
        call_rows += json.loads(path.read_text())["functions"]
    eligible = sorted(
        call_rows + leaf_rows,
        key=lambda r: (r["stratum"] != "leaf",) + cdl.rank_key(r),
    )
    funnel = collections.Counter(reasons.values())
    summary = {
        "executed_named_in_any_docs_data_or_evidence_artefact_info_only": sum(
            1 for v in executed if v in info
        ),
        "pilot_stage_of_all_unproven_executed_by_part": {
            p: dict(sorted(c.items())) for p, c in pre_exclusion.items()
        },
        "drafting_pool_before_exclusions": sum(
            c[k] for c in pre_exclusion.values() for k in pool_strata
        ),
        "executed_in_target_parts": len(executed),
        "census_targets_total": len(census),
        "sampled_functions_total": len(sampled),
        "profile_files": profile_files,
        "by_part": {p: dict(c) for p, c in by_part.items()},
        "first_exclusion_before_screen": dict(stages),
        "screen_remaining": len(remaining),
        "screen_waterfall": dict(sorted(funnel.items())),
        "call_screen_pool": len(call_pop),
        "call_eligible": len(call_rows),
        "leaf_eligible": len(leaf_rows),
        "static_eligible": len(eligible),
        "executed_by_part": {p: sum(1 for v in executed if part(v) == p) for p in PARTS},
        "eligible_by_part": {
            p: sum(1 for r in eligible if part(int(r["va"], 16)) == p) for p in PARTS
        },
    }
    out = args.out_dir
    out.mkdir(parents=True, exist_ok=True)
    chunks = [eligible[n : n + LIST_SIZE] for n in range(0, len(eligible), LIST_SIZE)]
    for number, chunk in enumerate(chunks, start=1):
        (out / f"list-{number:03d}.json").write_text(
            json.dumps(
                {
                    "number": number,
                    "partial": len(chunk) < LIST_SIZE,
                    "count": len(chunk),
                    "functions": chunk,
                    "provenance": {"status": "static draft eligibility only; no proof/admission"},
                },
                indent=2,
                sort_keys=True,
            )
            + "\n"
        )
    per_va = {
        f"0x{va:08x}": {
            "part": part(va),
            "census": va in census,
            "sampled": va in sampled,
            "disposition": (
                "static-draft-eligible"
                if any(int(r["va"], 16) == va for r in eligible)
                else "proven"
                if va in proven
                else "registered"
                if va in registered
                else "rejected-before"
                if va in rejected
                else "mentioned-elsewhere:" + elsewhere[va]
                if va in elsewhere
                else reasons.get(va, "?")
            ),
        }
        for va in sorted(executed)
    }
    (out / "report.json").write_text(
        json.dumps({"summary": summary, "functions": per_va}, indent=2, sort_keys=True) + "\n"
    )
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
