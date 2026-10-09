# SPDX-License-Identifier: GPL-3.0-or-later
"""T1557 historical refusal accounting and explicitly conditional investigation lists."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from collections import Counter
from copy import deepcopy
from pathlib import Path

from tools import call_draft_lists as prior
from tools.llm_replacement_draft import registered_vas

# These are prerequisite proposals, never claims that implementing one proves a root.
REMEDIES = {
    "closure-call-plan:indirect-call": (
        "Core authenticated indirect target/dispatch closure authority; exact "
        "targets, ABI and all reachable callees; fixture selection alone cannot "
        "close it"
    ),
    "closure-call-plan:callee-convention-unknown": (
        "Authenticate callee cleanup/stack ABI and source extents; improve bounded "
        "convention recognizer only with controls, then full closure and caller "
        "re-audit"
    ),
    "closure-no-ret": (
        "Authenticate exact original extent and terminal control; genuine tail "
        "continuation needs core closure support, never truncate/prune to RET"
    ),
    "closure-instruction:jump": (
        "Distinguish authentic tail/out-of-body/indirect control from extent errors; "
        "core supported closure/control authority or authenticated existing boundary "
        "repair, no opaque waiver"
    ),
    "closure-instruction:sse-mmx": (
        "T1547 vector owner: full supported vector closure/state/comparison and "
        "independent ABI/caller eligibility; old integer scanner refusal does not "
        "establish current eligibility"
    ),
    "unsupported-register-input-set": (
        "Announce exact input set to header/core owner; current approved shapes only "
        "legacy/ecx/esi/ecx+esi; independent full-register adapter and audit "
        "validation required, no local header edit"
    ),
    "caller-audit-gates": (
        "Per-site authentic caller/flags/dispatch/tail evidence and unchanged audit "
        "rules; provider cannot repair caller flags or unresolved control"
    ),
    "closure-segment": (
        "Core authenticated segment/TLS state and comparison semantics plus "
        "closure/ABI; ordinary pointer fixture insufficient"
    ),
    "closure-call-plan:stackprobe-caller-unproven": (
        "Authenticate stackprobe caller/frame/return contract under current guarded "
        "stackprobe support; no arbitrary helper boundary"
    ),
    "fewer-than-five-meaningful-insns": (
        "Existing near-vacuous gate blocks this root; defer, no padding/NOP/gate "
        "change or claimed fixture remedy"
    ),
    "abi-unsupported-stack-write": (
        "Original-grounded stack-write/alias/frame semantics and independent bounded "
        "recognizer evidence; no assumed safe store"
    ),
    "abi-return-slot-or-partial-stack-store": (
        "Exact byte-width return/frame effects and alias authority needed; no "
        "saved-frame experiment or invented clean continuation"
    ),
    "abi-indexed-stack": (
        "Authenticate dynamic/indexed frame bounds and incoming argument semantics; "
        "independent recognizer and alias controls"
    ),
    "closure-instruction:privileged": (
        "Core faithful privileged architectural state/exception contract; no fixture-only fix"
    ),
    "abi-unknown-frame": (
        "Authenticate original ESP/EBP affine or dynamic frame semantics and exact "
        "cleanup; independent recognizer controls"
    ),
    "closure-instruction:atomic": (
        "Core atomic/order/state semantics and comparison support; scalar fixture cannot justify it"
    ),
    "abi-recursion-or-depth": (
        "Bounded authentic recursive/deep closure ABI and termination authority; "
        "cannot increase bound as proof"
    ),
    "abi-inconsistent-frame-join": (
        "Authenticate path-dependent stack/ABI join; independent CFG/frame "
        "recognizer controls, no guessed joined shape"
    ),
    "closure-call-plan:out-of-body-branch": (
        "Authenticate target extents and real tail/control closure; no clipping reachable code"
    ),
    "closure-integer-wave-x87": (
        "Deferred x87 owner: existing x87 path requires actual "
        "closure/state/ABI/caller revalidation; integer-wave policy removal alone is "
        "not eligibility"
    ),
    "closure-instruction:unlifted-stack-op": (
        "Core supported original stack instruction/cleanup semantics with independent controls"
    ),
    "abi-unknown-cleanup": (
        "Authenticate every original RET/cleanup path and exact supported convention"
    ),
    "abi-incoming-arithmetic-flags": (
        "Authenticate incoming arithmetic flags contract and transport/comparison "
        "authority; register inputs alone do not carry flags"
    ),
}


def category(reason: str) -> str:
    if reason.startswith(("abi-", "unsupported-register", "fewer-", "caller-", "closure-")):
        result = reason.split(":")[0]
    elif "does not end in a near ret" in reason:
        result = "closure-no-ret"
    elif "contains unsupported " in reason:
        result = "closure-instruction:" + reason.split("contains unsupported ")[1].split(" at ")[0]
    elif "unsupported call/branch plan:" in reason:
        result = (
            "closure-call-plan:"
            + reason.split("unsupported call/branch plan:")[1]
            .split(";")[0]
            .strip()
            .split(" at ")[0]
        )
    else:
        raise ValueError("unclassified historical refusal: " + reason)
    if result not in REMEDIES:
        raise ValueError("unknown remedy category: " + result)
    return result


def caller_blockers(row: dict) -> list[str]:
    audit = row.get("audit")
    if audit is None:
        return ["caller-audit-not-reached"]
    reasons = []
    if not audit["direct_sites"]:
        reasons.append("no-authenticated-direct-caller")
    if audit["data_references"]:
        reasons.append("data-reference-dispatch-authority")
    if audit["tail_jumps"]:
        reasons.append("tail-caller-authority")
    for name in ("flags", "verdicts"):
        for verdict in sorted({item["verdict"] for item in audit[name]} - {"safe"}):
            reasons.append(name + "-" + verdict)
    return reasons


def classify(report: dict, registered: set[int]) -> tuple[list[dict], list[str]]:
    old_eligible = {
        row["va"]
        for row in report["dispositions"]
        if row["status"] == "static-draft-eligible-NOT-proof"
    }
    refused = [row for row in report["dispositions"] if row["status"] == "refused"]
    if len(refused) != 2885 or len(old_eligible) != 95 or len(report["dispositions"]) != 2980:
        raise ValueError("unexpected historical population; no silent cohort substitution")
    if len({row["va"] for row in report["dispositions"]}) != 2980:
        raise ValueError("duplicate historical root")
    rows, admitted = [], []
    for original in refused:
        row = deepcopy(original)
        reason = category(row["reason"])
        row["historical_status"] = row["status"]
        row["status"] = "conditional-investigation-NOT-eligible-NOT-proof"
        row["blocker_class"] = reason
        row["required_change"] = REMEDIES[reason]
        row["caller_blockers"] = caller_blockers(row)
        row["remaining_requirements"] = [
            "fresh complete closure/ABI/selected caller revalidation",
            "independent candidate/native/domain declaration",
            "fixed four-tuple unchanged proof gates",
        ]
        if int(row["va"], 16) in registered:
            admitted.append(row["va"])
        else:
            rows.append(row)
    return rows, sorted(admitted)


def conditional_rank(row: dict) -> tuple:
    # Complete historical caller metadata first; vector and x87 are explicitly dependencies.
    group = {
        "caller-audit-gates": 0,
        "closure-call-plan:callee-convention-unknown": 1,
        "unsupported-register-input-set": 2,
        "closure-instruction:sse-mmx": 3,
        "closure-integer-wave-x87": 4,
    }.get(row["blocker_class"], 5)
    return (
        group,
        row["meaningful_insns"],
        row.get("closure_nodes", 1 << 30),
        row.get("closure_insns", 1 << 30),
        row["call_sites"],
        row["size"],
        row["va"],
    )


def validate(lists: list[dict], rows: list[dict], excluded: set[str]) -> None:
    flat = [row for item in lists for row in item["functions"]]
    if len(flat) != len(rows) or len({r["va"] for r in flat}) != len(rows):
        raise ValueError("conditional coverage/disjointness failure")
    if {r["va"] for r in flat} & excluded:
        raise ValueError("excluded eligible/admitted root leaked")
    if flat != sorted(rows, key=conditional_rank):
        raise ValueError("rank or row integrity changed")
    for number, item in enumerate(lists, 1):
        n = len(item["functions"])
        if item["number"] != number or item["count"] != n or not 1 <= n <= 25:
            raise ValueError("conditional envelope mismatch")
        if item["partial"] != (n < 25) or (n < 25 and number != len(lists)):
            raise ValueError("conditional partial list mismatch")
        for row in item["functions"]:
            raw = bytes.fromhex(row["bytes"])
            if len(raw) != row["size"] or hashlib.sha256(raw).hexdigest() != row["sha256"]:
                raise ValueError("original byte integrity failure")
            if row["status"] != "conditional-investigation-NOT-eligible-NOT-proof":
                raise ValueError("conditional eligibility promotion")
            if not row["required_change"] or not row["reason"]:
                raise ValueError("missing conditional blocker")


def run(private: Path, out: Path) -> None:
    if out.exists():
        raise ValueError("refuse output overwrite")
    from tools.name_additions import World

    report_path = Path("docs/data/t1534-call-draft-lists/report.json")
    paths = {
        report_path,
        Path(__file__),
        Path("tools/call_draft_lists.py"),
        private / "build/default.xbe",
        private / "generated/retail/functions.csv",
    }
    paths.update(Path("tools").rglob("*.py"))
    paths.update(Path("tools/replace").glob("*.py"))
    paths.update(Path("src/game").glob("*.h"))
    paths.update(Path("src/game").glob("*.c"))
    paths.update((private / "tools/data").glob("*.csv"))
    paths.update((private / "generated/retail").glob("*.csv"))
    paths.update((private / "generated/retail").glob("*manifest*.json"))
    pins = {str(p): prior.sha(p) for p in sorted(paths)}
    report = json.loads(report_path.read_text())
    registered = registered_vas()
    rows, admitted = classify(report, registered)
    world = World(private)
    md = __import__("tools.llm_replacement_draft", fromlist=["_capstone"])._capstone()
    for row in rows:
        va = int(row["va"], 16)
        size = world.size[va]
        code = world.read(va, size)
        insns = list(md.disasm(code, va))
        decoded = sum(i.size for i in insns) == size
        if "bytes" in row and (row["bytes"] != code.hex() or row["size"] != size):
            raise ValueError("historical root bytes/extent drift: " + row["va"])
        row.update(
            name=row.get("name", f"sub_{va:08X}"),
            size=size,
            bytes=code.hex(),
            sha256=hashlib.sha256(code).hexdigest(),
            source=f"t1557_{va:08x}.c",
            meaningful_insns=sum(i.mnemonic not in {"nop", "int3"} for i in insns),
            call_sites=sum(i.mnemonic == "call" for i in insns),
            decode_complete=decoded,
            provenance={
                "pins": "pins-before.json",
                "status": "conditional static investigation only",
            },
        )
        row.setdefault("abi", None)
        row.setdefault("closure", None)
        row.setdefault("recipe", None)
        if not decoded:
            row["remaining_requirements"].insert(0, "root complete decode/extent authority")
        row["limitations"] = [
            "NOT eligible/draft-ready/proven/admitted",
            "unknown ABI/closure/recipe remain null; never runnable defaults",
            "recorded first refusal is not exhaustive blocker analysis",
        ]
    ranked = sorted(rows, key=conditional_rank)
    lists = [
        {
            "number": n // 25 + 1,
            "partial": len(ranked[n : n + 25]) < 25,
            "count": len(ranked[n : n + 25]),
            "functions": ranked[n : n + 25],
        }
        for n in range(0, len(ranked), 25)
    ]
    excluded = set(admitted) | {r["va"] for r in report["dispositions"] if r["status"] != "refused"}
    validate(lists, rows, excluded)
    after = {p: prior.sha(Path(p)) for p in pins}
    if after != pins:
        raise ValueError("frozen source drift during census")
    counts = Counter(
        category(r["reason"]) for r in report["dispositions"] if r["status"] == "refused"
    )
    result = {
        "version": 1,
        "historical_refused": 2885,
        "historical_early": 2776,
        "historical_caller_refused": 109,
        "historical_eligible_excluded": 95,
        "current_admissions_excluded": admitted,
        "conditional_unique": len(rows),
        "currently_newly_eligible": 0,
        "conditional_list_count": len(lists),
        "reason_counts": dict(sorted(counts.items())),
        "caller_blocker_counts_overlapping": dict(
            sorted(
                Counter(
                    b
                    for r in rows
                    if r["blocker_class"] == "caller-audit-gates"
                    for b in r["caller_blockers"]
                ).items()
            )
        ),
        "unsupported_input_sets": dict(
            sorted(
                Counter(
                    ",".join(r["abi"]["register_inputs"])
                    for r in rows
                    if r["blocker_class"] == "unsupported-register-input-set"
                ).items()
            )
        ),
        "remedies": REMEDIES,
        "static_only": True,
        "dispositions": rows,
    }
    result.pop("dispositions")
    result["dispositions_lists"] = [f"list-{item['number']:03d}.json" for item in lists]
    out.mkdir(parents=True)
    for filename, value in [
        ("report.json", result),
        ("pins-before.json", pins),
        ("pins-after.json", after),
    ]:
        (out / filename).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    for item in lists:
        (out / f"list-{item['number']:03d}.json").write_text(
            json.dumps(item, indent=2, sort_keys=True) + "\n"
        )
    print(
        json.dumps(
            {k: v for k, v in result.items() if k not in {"dispositions", "remedies"}},
            sort_keys=True,
        )
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--private-root", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    os.setpriority(os.PRIO_PROCESS, 0, 19)
    assert os.getpriority(os.PRIO_PROCESS, 0) == 19
    assert os.sched_getaffinity(0) == set(range(24, 32))
    run(args.private_root.resolve(), args.out_dir)


if __name__ == "__main__":
    main()
