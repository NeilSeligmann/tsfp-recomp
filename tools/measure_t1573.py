"""T1573 measurement: replay the pre-audit refusal stage before/after the es: string-op fix.

Static only. Replays closure discovery, closure_capability and ABI inference on the
T1534 pre-audit dispositions (2776 early refusals) with the old string-match
(`_implicit_es_string_operand_only` forced False) and the new decoded-prefix test.
No audit, original, native or proof run. Output is a JSON document of class changes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter
from pathlib import Path
from typing import Any

from tools import call_draft_lists as cdl
from tools import llm_replacement_draft as draft
from tools.harness.callclosure import LiveClosureError, discover_live_call_closure
from tools.name_additions import World


def classify(
    va: int, world: Any, md: Any, sizes: dict[int, int], names: dict[int, str], functions: Any
) -> str:
    if sum(i.mnemonic not in {"nop", "int3"} for i in functions[va].insns) < 5:
        return "fewer-than-five-meaningful-insns"
    recognizer = cdl.ABIRecognizer(sizes, world.read)
    try:
        closure = discover_live_call_closure(
            va, sizes=sizes, names=names, read_code=lambda a, n: world.read(a, n)
        )
        for node in closure.nodes:
            items = list(md.disasm(world.read(node.va, node.size), node.va))
            cdl.closure_capability(items, (world.text_lo, world.text_hi))
        abi = recognizer.infer(va)
        if abi.inputs not in cdl.SUPPORTED:
            return "unsupported-register-input-set:" + ",".join(abi.inputs)
    except (LiveClosureError, cdl.Refusal) as error:
        return str(error).split(":")[0]
    return "passes-pre-audit"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--private-root", type=Path, required=True)
    parser.add_argument("--dispositions", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    world = World(args.private_root)
    md = draft._capstone()
    functions = draft.load_functions(
        args.private_root / "generated/retail/functions.csv", world.read
    )
    sizes = {va: f.size for va, f in functions.items()}
    fallback = args.private_root / "generated/retail/manifest.json"
    if fallback.exists():
        for r in json.loads(fallback.read_text()).get("functions", []):
            sizes.setdefault(int(r["address"], 16), int(r["size"]))
    names = {va: f"sub_{va:08X}" for va in sizes}
    rows = [r for r in json.loads(args.dispositions.read_text()) if r["status"] == "refused"]
    new_check = cdl._implicit_es_string_operand_only
    changed: list[dict[str, str]] = []
    before_counts: Counter[str] = Counter()
    after_counts: Counter[str] = Counter()
    try:
        for row in rows:
            va = int(row["va"], 16)
            cdl._implicit_es_string_operand_only = lambda _item: False
            before = classify(va, world, md, sizes, names, functions)
            cdl._implicit_es_string_operand_only = new_check
            after = classify(va, world, md, sizes, names, functions)
            before_counts[before] += 1
            after_counts[after] += 1
            if before != after:
                changed.append({"va": row["va"], "before": before, "after": after})
    finally:
        cdl._implicit_es_string_operand_only = new_check
    report = {
        "roots": len(rows),
        "xbe_sha256": hashlib.sha256(
            (args.private_root / "build/default.xbe").read_bytes()
        ).hexdigest(),
        "before": dict(sorted(before_counts.items())),
        "after": dict(sorted(after_counts.items())),
        "changed_count": len(changed),
        "changed_to": dict(Counter(c["after"] for c in changed)),
        "changed": changed,
    }
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "changed"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
