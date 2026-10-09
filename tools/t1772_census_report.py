# SPDX-License-Identifier: GPL-3.0-or-later
"""T1772: aggregation of the replacement census (tables, blocker classes, lists, markdown).

Reads the per-function rows written by `tools.t1772_replacement_census` and writes
`tables.json`, `tables.md`, the per-band work lists and the recall check. Addresses, sizes and
counts only.
"""

from __future__ import annotations

import collections
import json
import re
from pathlib import Path
from typing import Any

BANDS = (
    ("T1475", 0x00000000, 0x00080000),
    ("T1476", 0x00080000, 0x00100000),
    ("T1477", 0x00100000, 0x00180000),
    ("T1478", 0x00180000, 0x00280000),
    ("T1479", 0x00280000, 0x00380000),
    ("T1480", 0x00380000, 0x00470000),
    ("beyond", 0x00470000, 1 << 32),
)
BUCKETS = (
    ("1-4", 1, 4),
    ("5-20", 5, 20),
    ("21-60", 21, 60),
    ("61-150", 61, 150),
    ("151+", 151, 1 << 30),
)
STATIC_BLOCKERS = (
    "size-over-300",
    "too-short",
    "x87",
    "vector-unadmitted",
    "privileged-or-segment",
    "self-modifying-code",
    "indirect-jump",
    "jump-out-of-body",
    "no-near-ret",
    "unsupported-input-set",
)
TIER_RANK = {
    "A-leaf": 0,
    "A-leaf-inputs": 1,
    "B-call": 2,
    "C-leaf-hard": 3,
    "D-leaf-large": 4,
    "F-fp-leaf": 5,
    "V-vector-call": 6,
}
LIST_SIZE = 25
LIST_BANDS = ("T1477", "T1478", "T1479", "T1480")
RECALL_LISTS = (
    "docs/data/t1534-call-draft-lists/list-00*.json",
    "docs/data/t1578-newly-reachable-lists/list-00*.json",
    "docs/data/t1587-af-kill-lists/list-00*.json",
    "docs/data/t1595-vector-lists/list-00*.json",
)


def bucket(count: int) -> str:
    return next((name for name, lo, hi in BUCKETS if lo <= count <= hi), "n/a")


def cs_class(cause: str) -> str:
    """Stable class of a call-screen refusal text (VAs already replaced by VA)."""
    c = cause
    if c.startswith("vector-closure: "):
        return "vector-closure(" + cs_class(c.removeprefix("vector-closure: ")) + ")"
    if c.startswith("unsupported-register-input-set"):
        return "unsupported-input-set"
    if c.startswith("abi-"):
        return "abi-recognizer:" + c.split(":")[0]
    if c.startswith("fewer-than-five"):
        return "too-short"
    if c.startswith("closure-"):
        return "closure:" + c.split(":")[0].removeprefix("closure-")
    m = re.search(r"unsupported call/branch plan: ([a-z-]+)", c)
    if m:
        return "call-plan:" + m.group(1)
    m = re.search(r"contains unsupported ([a-z-]+)", c)
    if m:
        return "closure-node:" + m.group(1)
    if "does not end in a near ret" in c:
        return "closure-node:no-near-ret"
    if "has no known function bound" in c:
        return "call-plan:callee-without-bound"
    if "not fully decodable" in c:
        return "closure-node:not-fully-decodable"
    if "unreadable" in c:
        return "closure-node:unreadable"
    if c.startswith("caller-audit-gates"):
        return "caller-audit"
    if "node-bound" in c:
        return "closure:node-bound-128"
    return "other:" + c[:60]


def blocker_set(r: dict[str, Any]) -> set[str]:
    """Blocker classes of one open function (static flags plus the first screen refusal)."""
    s: set[str] = set()
    if r["first"] == "decode-mismatch":
        s.add("decode-mismatch")
    for name in STATIC_BLOCKERS:
        if name in r["labels"]:
            s.add(name)
    if r["first"] == "call-screen":
        s.add("call-screen:" + cs_class(r["cause"]))
    if r["first"] == "leaf-caller-audit":
        s.add("leaf-caller-audit")
    if r["first"] == "pilot-heuristic":
        s.add("screen-function-reject")
    if "fp-scalar-step2b" == r["vector"]:
        s.add("fp-scalar-step2b")
    return s


def is_leaf_stratum(r: dict[str, Any]) -> bool:
    return not r["calls"] and not any(x.startswith("input:") for x in r["labels"])


def load(rows_path: Path) -> dict[int, dict[str, Any]]:
    data = json.loads(rows_path.read_text())
    return {int(k, 16): v for k, v in data.items()}


def counter(items: Any) -> dict[str, int]:
    return dict(collections.Counter(items).most_common())


def tables(rows: dict[int, dict[str, Any]], call_report: dict[str, Any]) -> dict[str, Any]:
    T: dict[str, Any] = {}
    allr = list(rows.values())
    openr = [r for r in allr if r["status"] == "open"]
    T["population"] = {
        "game": len(allr),
        "proven": sum(r["status"] == "proven" for r in allr),
        "registered-unproven": sum(r["status"] == "registered-unproven" for r in allr),
        "registered-unproven:snapshot-failed": sum(
            r.get("status_detail") == "snapshot-failed" for r in allr
        ),
        "registered-unproven:no-proof-in-snapshot": sum(
            r.get("status_detail") == "no-proof-in-snapshot" for r in allr
        ),
        "open": len(openr),
        "open:from_function_additions": sum(r["addition"] for r in openr),
        "open:executed_in_owner_sessions": sum(r["executed"] for r in openr),
    }
    T["open_by_mention"] = counter(r["mention"] or "fresh" for r in openr)
    T["waterfall_open"] = counter(r["first"] for r in openr)
    T["waterfall_open_by_band"] = {
        band: counter(r["first"] for r in openr if r["band"] == band) for band, _, _ in BANDS
    }
    T["status_by_band"] = {
        band: counter(r["status"] for r in allr if r["band"] == band) for band, _, _ in BANDS
    }
    T["decode_mismatch_causes"] = counter(
        r["cause"] for r in openr if r["first"] == "decode-mismatch"
    )
    T["call_screen_classes"] = counter(
        cs_class(r["cause"]) for r in openr if r["first"] == "call-screen"
    )
    T["leaf_caller_audit_causes"] = counter(
        r["cause"] for r in openr if r["first"] == "leaf-caller-audit"
    )
    leaf_fail = [r for r in openr if r["first"] == "leaf-caller-audit"]
    T["leaf_caller_audit_failures"] = len(leaf_fail)
    T["leaf_caller_audit_failures_pass_with_scratch_ecx_edx"] = sum(
        1 for r in leaf_fail if r.get("scratch_audit_ok")
    )
    T["unknown_callee_kinds"] = counter(r["unknown_callee"] for r in openr if "unknown_callee" in r)
    T["indirect_call_kinds"] = counter(r["indirect_call"] for r in openr if "indirect_call" in r)
    T["vector_closure_stage"] = counter(
        re.sub(r"0x[0-9a-fA-F]+", "VA", r["vec_stage"]) for r in openr if "vec_stage" in r
    )
    elig = [r for r in openr if r["first"] == "eligible"]
    fresh = [r for r in elig if not r["mention"]]
    T["eligible_total"] = len(elig)
    T["eligible_fresh"] = len(fresh)
    T["eligible_executed"] = sum(r["executed"] for r in elig)
    T["eligible_by_tier"] = counter(r["tier"] for r in elig)
    T["eligible_fresh_by_tier"] = counter(r["tier"] for r in fresh)
    T["eligible_by_mention"] = counter(r["mention"] or "fresh" for r in elig)
    T["eligible_by_bucket_tier"] = {
        b: counter(r["tier"] for r in elig if bucket(r["meaningful"]) == b) for b, _, _ in BUCKETS
    }
    T["eligible_fresh_by_bucket"] = counter(bucket(r["meaningful"]) for r in fresh)
    T["eligible_by_band"] = {
        band: counter(r["mention"] or "fresh" for r in elig if r["band"] == band)
        for band, _, _ in BANDS
    }
    T["eligible_by_prereq"] = counter(r["prereq"] for r in elig)
    T["eligible_fresh_by_prereq"] = counter(r["prereq"] for r in fresh)
    T["eligible_by_subsystem_all"] = counter(r["subsystem"] or "unknown" for r in elig)
    T["eligible_by_subsystem_fresh"] = counter(r["subsystem"] or "unknown" for r in fresh)
    jud = [r for r in openr if r["baseline"] == "verdict"]
    T["judgeable_open"] = len(jud)
    T["judgeable_open_by_bucket"] = counter(bucket(r["meaningful"]) for r in jud)
    T["judgeable_open_by_band"] = counter(r["band"] for r in jud)
    T["judgeable_open_first_blocker"] = counter(r["first"] for r in jud)
    T["judgeable_open_by_mention"] = counter(r["mention"] or "fresh" for r in jud)
    T["judgeable_open_by_subsystem"] = counter(r["subsystem"] or "unknown" for r in jud)
    T["baseline_all_open"] = counter(r["baseline"] for r in openr)
    T["eligible_vs_baseline"] = counter(r["baseline"].split(":")[0] for r in elig)
    cross: dict[str, collections.Counter[str]] = collections.defaultdict(collections.Counter)
    for r in openr:
        cross[r["first"]][r["baseline"]] += 1
    T["cross_first_vs_baseline"] = {k: dict(v.most_common(8)) for k, v in cross.items()}
    vec: collections.Counter[str] = collections.Counter()
    for r in openr:
        if r["vector"] and r["vector"] != "unadmitted":
            others = {x for x in r["labels"] if x in STATIC_BLOCKERS}
            vec[
                f"{r['vector']}|{'calls' if r['calls'] else 'leaf'}|"
                f"{'static-clean' if not others else 'also-blocked'}"
            ] += 1
    T["vector_prereq_pools"] = dict(sorted(vec.items()))
    # stage pass rates (INFERRED yield model)
    leaf_reached = [
        r
        for r in openr
        if is_leaf_stratum(r) and r["first"] in ("eligible", "leaf-caller-audit", "pilot-heuristic")
    ]
    call_reached = [
        r for r in openr if not is_leaf_stratum(r) and r["first"] in ("eligible", "call-screen")
    ]
    p_leaf = sum(r["first"] == "eligible" for r in leaf_reached) / max(1, len(leaf_reached))
    p_call = sum(r["first"] == "eligible" for r in call_reached) / max(1, len(call_reached))
    T["stage_pass_rates"] = {
        "leaf_reached": len(leaf_reached),
        "leaf_eligible": sum(r["first"] == "eligible" for r in leaf_reached),
        "leaf_rate": round(p_leaf, 4),
        "call_reached": len(call_reached),
        "call_eligible": sum(r["first"] == "eligible" for r in call_reached),
        "call_rate": round(p_call, 4),
    }
    classes: dict[str, dict[str, Any]] = {}
    for va, r in rows.items():
        if r["status"] != "open" or r["first"] == "eligible":
            continue
        s = blocker_set(r)
        for k in s:
            e = classes.setdefault(
                k,
                {
                    "any": 0,
                    "solo": 0,
                    "solo_leaf": 0,
                    "solo_call": 0,
                    "first": 0,
                    "solo_examples": [],
                },
            )
            e["any"] += 1
            if len(s) == 1:
                e["solo"] += 1
                e["solo_leaf" if is_leaf_stratum(r) else "solo_call"] += 1
                if len(e["solo_examples"]) < 3:
                    e["solo_examples"].append(f"0x{va:08X}")
        classes.setdefault(
            r["first"],
            {"any": 0, "solo": 0, "solo_leaf": 0, "solo_call": 0, "first": 0, "solo_examples": []},
        )["first"] += 1
    for k, e in classes.items():
        # downstream classes are the last screen: their solo count is the unlock upper bound
        if k.startswith("call-screen:") or k in ("leaf-caller-audit", "decode-mismatch"):
            e["expected"] = e["solo"] * (1.0 if k == "leaf-caller-audit" else p_call)
        else:
            e["expected"] = round(e["solo_leaf"] * p_leaf + e["solo_call"] * p_call, 1)
        # examples of the first-screen refusal classes need not be solo
        if not e["solo_examples"]:
            ex = [
                f"0x{va:08X}"
                for va, r in rows.items()
                if r["status"] == "open" and k in blocker_set(r)
            ][:3]
            e["examples_any"] = ex
    T["blocker_classes"] = classes
    nodes: collections.Counter[tuple[str, str]] = collections.Counter()
    root_vs_callee: collections.Counter[str] = collections.Counter()
    for d in call_report.get("dispositions", []):
        if d["status"] != "refused":
            continue
        m = re.search(r"(?:function|callee) (0x[0-9a-fA-F]{8})", d.get("reason", ""))
        if not m:
            continue
        node = "0x" + m.group(1)[2:].upper()
        nodes[(node, cs_class(re.sub(r"0x[0-9a-fA-F]+", "VA", d["reason"])))] += 1
        root_vs_callee["root" if node.lower() == d["va"].lower() else "callee"] += 1
    T["call_screen_failing_node"] = dict(root_vs_callee)
    T["call_screen_top_nodes"] = [
        {"node": n, "class": c, "roots": v} for (n, c), v in nodes.most_common(25)
    ]
    # leverage: distinct failing callee nodes needed to clear 50% / 80% of callee-caused refusals
    by_node: collections.Counter[str] = collections.Counter()
    for (n, _c), v in nodes.items():
        by_node[n] += v
    total = sum(by_node.values())
    run_sum = 0
    marks = {}
    for index, (_n, v) in enumerate(by_node.most_common(), start=1):
        run_sum += v
        for pct in (50, 80):
            if pct not in marks and run_sum * 100 >= pct * total:
                marks[pct] = index
    T["call_screen_node_leverage"] = {
        "distinct_failing_nodes": len(by_node),
        "refusals": total,
        "nodes_for_50pct": marks.get(50),
        "nodes_for_80pct": marks.get(80),
    }
    return T


def recall(rows: dict[int, dict[str, Any]]) -> dict[str, Any]:
    """Where do the VAs of earlier eligible lists stand now (validation of this census)."""
    out: dict[str, Any] = {}
    for pattern in RECALL_LISTS:
        for path in sorted(Path(".").glob(pattern)):
            doc = json.loads(path.read_text())
            vas = [int(row["va"], 16) for row in doc["functions"]]
            states: collections.Counter[str] = collections.Counter()
            for va in vas:
                r = rows.get(va)
                if r is None:
                    states["not-in-game-set"] += 1
                elif r["status"] != "open":
                    states[r["status"]] += 1
                else:
                    states[r["first"]] += 1
            out[str(path)] = {"rows": len(vas), "now": dict(states.most_common())}
    return out


def vector_sort_key(r: dict[str, Any]) -> tuple[Any, ...]:
    return (not r["executed"],) + sort_key(r)


def sort_key(r: dict[str, Any]) -> tuple[Any, ...]:
    return (
        TIER_RANK[r["tier"]],
        r["meaningful"],
        r.get("closure_nodes", 1),
        r["calls"],
        r.get("simplicity", 0),
        r["size"],
        r["va"],
    )


def is_vector_prereq(r: dict[str, Any]) -> bool:
    return bool(r["vector"]) or r["tier"] in ("F-fp-leaf", "V-vector-call")  # closure may hold it


def list_row(r: dict[str, Any]) -> dict[str, Any]:
    recipe: dict[str, Any] = {
        "live_call_closure": r["tier"] in ("B-call", "V-vector-call") or bool(r["vector"]),
        "live_call_boundaries": [],
        "fixture_providers": [],
        "count": 600,
        "seeds": [20261001, 20261006],
        "optimizations": [0, 3],
        "scratch": [],
        "input_contract": r["abi"],
    }
    mode = r["vector"] or r.get("vector_mode", "")
    if mode:
        recipe["live_vector_state"] = True
        recipe["vector_mode"] = "fp-scalar-v1" if mode == "fp-scalar-v1" else "legacy"
    return {
        "va": f"0x{r['va']:08x}",
        "size": r["size"],
        "insns": r["meaningful"],
        "bucket": bucket(r["meaningful"]),
        "subsystem": r["subsystem"] or "unknown",
        "band": r["band"],
        "tier": r["tier"],
        "prerequisite": r["prereq"],
        "executed_in_owner_sessions": r["executed"],
        "call_sites": r["calls"],
        "closure_nodes": r.get("closure_nodes", 1),
        "caller_audit_direct_sites": r.get("audit_sites"),
        "abi": r["abi"],
        "why_eligible": r["why"],
        "from_function_additions": r["addition"],
        "baseline_harness": r["baseline"],
        "recipe": recipe,
    }


def make_lists(rows: dict[int, dict[str, Any]], out_dir: Path) -> dict[str, Any]:
    summary: dict[str, Any] = {}
    for band in LIST_BANDS:
        elig = [
            r
            for r in rows.values()
            if r["status"] == "open" and r["first"] == "eligible" and r["band"] == band
        ]
        fresh = [r for r in elig if not r["mention"]]
        order = lambda r: (not r["executed"],) + sort_key(r)  # noqa: E731
        plain = sorted((r for r in fresh if not is_vector_prereq(r)), key=order)
        vec = sorted((r for r in fresh if is_vector_prereq(r)), key=order)
        appendix = sorted((r for r in elig if r["mention"]), key=lambda r: r["va"])
        bdir = out_dir / f"band-{band}"
        bdir.mkdir(parents=True, exist_ok=True)
        files = []
        for prefix, pool, note in (
            ("list", plain, "batch_prove LIST --out-dir tmp/bp/N --draft FILE.c --cores A-B"),
            (
                "vec",
                vec,
                "batch_prove LIST --live-vector-state [--vector-mode fp-scalar-v1 for fp rows]",
            ),
        ):
            for n in range(0, len(pool), LIST_SIZE):
                chunk = pool[n : n + LIST_SIZE]
                number = n // LIST_SIZE + 1
                doc = {
                    "task": band,
                    "band": band,
                    "kind": prefix,
                    "number": number,
                    "partial": len(chunk) < LIST_SIZE,
                    "count": len(chunk),
                    "usage": note,
                    "status": "static eligibility only (T1772 census); no proof, no admission",
                    "functions": [list_row(r) for r in sorted(chunk, key=lambda r: r["va"])],
                }
                name = f"{prefix}-{number:03d}.json"
                (bdir / name).write_text(json.dumps(doc, indent=1) + "\n")
                files.append({"file": f"band-{band}/{name}", "count": len(chunk)})
        app = [{**list_row(r), "mentioned_in": r["mention"]} for r in appendix]
        (bdir / "appendix-previously-mentioned.json").write_text(json.dumps(app, indent=1) + "\n")
        summary[band] = {
            "eligible_all": len(elig),
            "fresh": len(fresh),
            "fresh_plain": len(plain),
            "fresh_vector_prereq": len(vec),
            "appendix_previously_mentioned": len(appendix),
            "files": files,
        }
    return summary


def write_census_csv(rows: dict[int, dict[str, Any]], csv_dir: Path) -> None:
    """Per-function table, kept out of git (bulk per-address output, check-no-disc-data.sh)."""
    import csv

    csv_dir.mkdir(parents=True, exist_ok=True)

    cols = (
        "va size insns meaningful band subsystem status first_blocker cause labels prereq tier "
        "mention executed from_additions baseline"
    ).split()
    with (csv_dir / "census.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(cols)
        for va in sorted(rows):
            r = rows[va]
            writer.writerow(
                [
                    f"0x{va:08X}",
                    r["size"],
                    r["insns"],
                    r["meaningful"],
                    r["band"],
                    r["subsystem"],
                    r["status"],
                    r["first"],
                    r["cause"],
                    "|".join(r["labels"]),
                    r["prereq"],
                    r["tier"],
                    r["mention"],
                    int(r["executed"]),
                    int(r["addition"]),
                    r["baseline"],
                ]
            )


def write_all(
    rows: dict[int, dict[str, Any]],
    call_report: dict[str, Any],
    out_dir: Path,
    csv_dir: Path | None = None,
) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    write_census_csv(rows, csv_dir if csv_dir is not None else out_dir)
    T = tables(rows, call_report)
    T["validation"] = call_report.get("validation", {})
    T["whatif"] = call_report.get("whatif", {})
    T["jump_tables"] = call_report.get("jump_tables", {})
    extent = dict(call_report.get("extent_repair", {}))
    repair = extent.pop("_repair", {})
    T["extent_repair"] = extent
    if repair:
        candidates = [
            {"va": va, "table_size_bytes": rows[int(va, 16)]["size"], "contiguous_span_bytes": span}
            for va, span in repair.items()
        ]
        (out_dir / "extent-repair-candidates.json").write_text(
            json.dumps(
                {
                    "note": "T1776 input: decode of [entry, body_max_va] is clean, ends in ret, "
                    "holds no other function-table entry",
                    "count": len(candidates),
                    "candidates": candidates,
                },
                indent=1,
            )
            + "\n"
        )
    T["recall_of_earlier_lists"] = recall(rows)
    T["lists"] = make_lists(rows, out_dir)
    (out_dir / "tables.json").write_text(json.dumps(T, indent=1, sort_keys=True) + "\n")
    (out_dir / "tables.md").write_text(render_markdown(T))


def render_markdown(T: dict[str, Any]) -> str:
    lines = ["# T1772 census tables (generated, do not edit)", ""]

    def table(title: str, data: dict[str, Any], key: str = "class", val: str = "functions") -> None:
        lines.extend([f"## {title}", "", f"| {key} | {val} |", "| --- | --- |"])
        lines.extend(f"| {k} | {v} |" for k, v in data.items())
        lines.append("")

    table("Population", T["population"], "item")
    table("Open functions by earlier mention", T["open_by_mention"], "mention")
    table("First-blocker waterfall (open functions)", T["waterfall_open"], "first blocker")
    lines.extend(["## First blocker by band", ""])
    names = sorted({k for v in T["waterfall_open_by_band"].values() for k in v})
    lines.extend(["| band | " + " | ".join(names) + " |", "|" + " --- |" * (len(names) + 1)])
    for band, vals in T["waterfall_open_by_band"].items():
        lines.append(f"| {band} | " + " | ".join(str(vals.get(n, 0)) for n in names) + " |")
    lines.append("")
    table("Decode mismatch causes", T["decode_mismatch_causes"], "cause")
    table("Call-screen refusal classes", T["call_screen_classes"], "class")
    table("Leaf caller-audit failure causes", T["leaf_caller_audit_causes"], "cause")
    table("Unknown-callee kinds (callee-convention-unknown)", T["unknown_callee_kinds"], "kind")
    table("Indirect-call operand kinds", T["indirect_call_kinds"], "kind")
    table(
        "Vector closure probe (call-screen sse-mmx refusals)", T["vector_closure_stage"], "result"
    )
    table("Eligible by tier", T["eligible_by_tier"], "tier")
    table("Eligible (fresh) by tier", T["eligible_fresh_by_tier"], "tier")
    table("Eligible by earlier mention", T["eligible_by_mention"], "mention")
    table("Eligible by prerequisite", T["eligible_by_prereq"], "prerequisite")
    lines.extend(["## Eligible by instruction bucket and tier", ""])
    tiers = list(TIER_RANK)
    lines.extend(["| insns | " + " | ".join(tiers) + " |", "|" + " --- |" * (len(tiers) + 1)])
    for b, vals in T["eligible_by_bucket_tier"].items():
        lines.append(f"| {b} | " + " | ".join(str(vals.get(t, 0)) for t in tiers) + " |")
    lines.append("")
    lines.extend(
        [
            "## Eligible by band",
            "",
            "| band | fresh | listed | receipt |",
            "| --- | --- | --- | --- |",
        ]
    )
    for band, vals in T["eligible_by_band"].items():
        lines.append(
            f"| {band} | {vals.get('fresh', 0)} | {vals.get('listed', 0)} | "
            f"{vals.get('receipt', 0)} |"
        )
    lines.append("")
    table("Eligible by subsystem leaf (all)", T["eligible_by_subsystem_all"], "subsystem")
    table(
        "Judgeable (baseline verdict) open by instruction bucket",
        T["judgeable_open_by_bucket"],
        "bucket",
    )
    table("Judgeable open by band", T["judgeable_open_by_band"], "band")
    table(
        "Judgeable open by static first blocker", T["judgeable_open_first_blocker"], "first blocker"
    )
    table("Baseline harness outcome of all open functions", T["baseline_all_open"], "outcome")
    table("Vector prerequisite pools", T["vector_prereq_pools"], "pool")
    lines.extend(
        [
            "## Blocker classes",
            "",
            "| class | any | solo | solo leaf | solo call | expected unlock (INFERRED) | "
            "examples (solo) |",
            "| --- | --- | --- | --- | --- | --- | --- |",
        ]
    )
    for k, e in sorted(T["blocker_classes"].items(), key=lambda kv: -kv[1]["any"]):
        ex = ", ".join(e["solo_examples"] or e.get("examples_any", []))
        lines.append(
            f"| {k} | {e['any']} | {e['solo']} | {e['solo_leaf']} | {e['solo_call']} | "
            f"{e.get('expected', '')} | {ex} |"
        )
    lines.append("")
    lines.extend(["## Stage pass rates", "", json.dumps(T["stage_pass_rates"]), ""])
    lines.extend(["## Validation: census on already registered leaf roots", ""])
    lines.extend([json.dumps(T.get("validation", {}), indent=1), ""])
    lines.extend(["## Function extent repair what-if", ""])
    lines.extend([json.dumps(T.get("extent_repair", {}), indent=1), ""])
    lines.extend(["## Static jump tables (T1576 domain)", ""])
    lines.extend([json.dumps(T.get("jump_tables", {}), indent=1), ""])
    lines.extend(["## What-if simulations (UNSOUND upper bounds)", ""])
    lines.extend([json.dumps(T.get("whatif", {}), indent=1), ""])
    lines.extend(["## Recall of earlier eligible lists", ""])
    lines.extend([json.dumps(T.get("recall_of_earlier_lists", {}), indent=1), ""])
    lines.extend(
        [
            "## Top blocking nodes of the call screen",
            "",
            "| node | class | roots |",
            "| --- | --- | --- |",
        ]
    )
    for item in T["call_screen_top_nodes"]:
        lines.append(f"| {item['node']} | {item['class']} | {item['roots']} |")
    lines.extend(["", json.dumps(T["call_screen_node_leverage"]), ""])
    return "\n".join(lines) + "\n"
