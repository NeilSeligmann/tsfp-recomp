# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate frozen T1785 AI campaign declarations, receipts and rejection decisions.

Does not execute a proof tuple or modify snapshots. Run from the repository root.
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

from tools.replace.batch_prove import evaluate_gate, fp_scalar_gate
from tools.replace.scan import scan_text


def main() -> None:
    root = Path("docs/data/t1785-ai")
    declared = json.loads((root / "predeclared.json").read_text())
    pin = json.loads((root / "draft-pin.json").read_text())
    draft = root / "proved-all-11-drafts.c.txt"
    assert hashlib.sha256(draft.read_bytes()).hexdigest() == pin["sha256"]
    regs = scan_text(draft.read_text(), draft.name)
    targets = {int(row["va"], 16): row for row in declared["roots"]}
    assert len(targets) == len(regs) == 11
    for reg in regs:
        abi = targets[reg.va]["abi"]
        assert reg.exact_registers
        assert reg.convention == abi["convention"]
        assert reg.stack_args == abi["stack_args"]
    audit = json.loads((root / "caller-audit.json").read_text())
    assert all(row["ok"] for row in audit.values()) and len(audit) == 11
    keys = set()
    admitted = []
    for mode in ("integer", "fp"):
        batch = json.loads((root / f"{mode}-results.json").read_text())
        for row in batch["functions"]:
            assert row["outcome"] == "REJECTED", row
        # Full original proof JSON is preserved and hash-checked against the result receipt.
        for receipt in (root / "tuples" / mode).glob("*/result.json"):
            result = json.loads(receipt.read_text())
            va = int(result["va"], 16)
            key = (va, result["seed"], result["opt"])
            assert key not in keys
            keys.add(key)
            assert result["cases"] == declared["cases"] == 600
            assert result["resumed"] is False
            assert result["exit"] == 0 and result["status"] in ("pass", "fail")
            proof_path = receipt.with_name("proof.json")
            assert hashlib.sha256(proof_path.read_bytes()).hexdigest() == result["proof_sha256"]
            proof = json.loads(proof_path.read_text())
            assert proof["edge_cases"] is True
            entry = next(r for r in proof["functions"] if int(r["va"], 16) == va)
            gate = evaluate_gate(entry)
            assert gate.failing == result["failing"]
            assert entry["disagree"] == entry["subject_faulted"] == 0
            if mode == "fp":
                reasons = fp_scalar_gate(proof_path)
                assert reasons == result["reasons"]
                assert reasons and result["first_failing"] == "fp_scalar_gate"
            if result["status"] == "pass":
                assert gate.passed
    expected = {
        (va, seed, opt) for va in targets for seed in declared["seeds"] for opt in declared["opts"]
    }
    assert keys == expected and len(keys) == 44
    for va in targets:
        statuses = [
            json.loads(p.read_text())["status"]
            for p in (root / "tuples").glob(f"*/*{va:08x}*/result.json")
        ]
        assert len(statuses) == 4
        if all(status == "pass" for status in statuses):
            admitted.append(va)
    assert not admitted
    table_vas = []
    for tree, count in (("characters", 150), ("behavior", 96), ("pathfinding", 23)):
        text = (root / f"classification-{tree}.md").read_text()
        vas = re.findall(r"^\| (0x[0-9A-F]{8}) \|", text, re.MULTILINE)
        assert len(vas) == count
        table_vas += vas
    assert len(set(table_vas)) == 269
    print(
        "T1785 PASS: 269 classified; 11 audited; 44 unique pinned tuples; 11 rejected; 0 admitted"
    )


if __name__ == "__main__":
    main()
