# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate the frozen T1788 campaign; never runs or repeats a proof tuple."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

from tools.replace.batch_prove import evaluate_gate
from tools.replace.scan import scan_text


def main() -> None:
    root = Path("docs/data/t1788-gameplay")
    declaration = json.loads((root / "predeclared.json").read_text())
    draft = root / "frozen-drafts.c.txt"
    assert hashlib.sha256(draft.read_bytes()).hexdigest() == declaration["draft_sha256"]
    regs = scan_text(draft.read_text(), str(draft))
    targets = {row["va"]: row for row in declaration["roots"]}
    assert len(targets) == len(regs) == 12
    for reg in regs:
        assert reg.va in targets and reg.exact_registers
        assert reg.stack_args == targets[reg.va]["abi"]["stack_args"]
        assert reg.convention == "cdecl"
    supplement = json.loads((root / "supplemental-predeclared.json").read_text())
    extra = root / "supplemental-draft.c.txt"
    assert hashlib.sha256(extra.read_bytes()).hexdigest() == supplement["draft_sha256"]
    extra_regs = scan_text(extra.read_text(), str(extra))
    assert len(extra_regs) == 1 and extra_regs[0].va == 0xBDBA0
    assert extra_regs[0].exact_registers and extra_regs[0].stack_args == 1
    targets.update({row["va"]: row for row in supplement["roots"]})
    assert len(targets) == 13
    audit = json.loads((root / "caller-audit.json").read_text())
    assert len(audit["functions"]) == 12
    assert all(row["eligible"] for row in audit["functions"])
    extra_audit = json.loads((root / "supplemental-caller-audit.json").read_text())
    assert len(extra_audit["functions"]) == 1 and extra_audit["functions"][0]["eligible"]
    table_vas = []
    for table in sorted(root.glob("classification-*.md")):
        table_vas += re.findall(r"^\| (0x[0-9A-F]{8}) \|", table.read_text(), re.MULTILINE)
    assert len(table_vas) == len(set(table_vas)) == 585
    keys = set()
    statuses: dict[int, list[str]] = {va: [] for va in targets}
    for receipt in (root / "tuples").glob("*/result.json"):
        result = json.loads(receipt.read_text())
        va = int(result["va"], 16)
        key = va, result["seed"], result["opt"]
        assert key not in keys and va in targets
        keys.add(key)
        assert result["cases"] == declaration["cases"] == 600
        assert result["resumed"] is False
        assert result["exit"] == 0 and result["status"] in ("pass", "fail")
        proof_path = receipt.with_name("proof.json")
        assert hashlib.sha256(proof_path.read_bytes()).hexdigest() == result["proof_sha256"]
        proof = json.loads(proof_path.read_text())
        assert proof["edge_cases"] is True
        entry = next(row for row in proof["functions"] if int(row["va"], 16) == va)
        gate = evaluate_gate(entry)
        assert gate.failing == result["failing"]
        assert (result["status"] == "pass") == gate.passed
        assert entry["disagree"] == entry["subject_faulted"] == 0
        statuses[va].append(result["status"])
    assert keys == {
        (va, seed, opt)
        for va in targets
        for seed in declaration["seeds"]
        for opt in declaration["opts"]
    }
    assert len(keys) == 52
    admitted = {va for va, values in statuses.items() if values == ["pass"] * 4}
    live = {
        reg.va
        for reg in scan_text(Path("src/game/game_gameplay_campaign.c").read_text(), "campaign")
    }
    prior = {0xDCA40, 0x168160, 0x10E9A0}
    assert prior <= set(targets)
    origin = json.loads((root / "origin-registry.json").read_text())
    origin_vas = {int(row["va"], 16) for row in origin["registry_rows"]}
    assert prior <= origin_vas
    assert not ((set(targets) - prior) & origin_vas)
    assert live == admitted - prior == {0xC9BB0, 0x1B58C0}
    final_source = Path("src/game/game_gameplay_campaign.c").read_text()
    for va in live:
        start = draft.read_text().index(f"GAME_REPLACE_EXACT({va:08X}")
        end = draft.read_text().find("\nGAME_REPLACE_EXACT(", start + 1)
        segment = draft.read_text()[start : end if end != -1 else None].strip()
        assert segment in final_source, f"production body drifted: {va:#x}"

    batch = json.loads((root / "results.json").read_text())
    assert len(batch["functions"]) == 12
    for row in batch["functions"]:
        va = int(row["va"], 16)
        assert row["outcome"] == ("ADMITTED" if va in admitted else "REJECTED")
    supplement_batch = json.loads((root / "supplemental-results.json").read_text())
    assert supplement_batch["functions"][0]["outcome"] == "REJECTED"
    mutation = json.loads((root / "mutation-predeclared.json").read_text())
    mutation_keys = set()
    for row in mutation["mutants"]:
        text = draft.read_text()
        assert text.count(row["before"]) == 1
        mutated = text.replace(row["before"], row["after"])
        assert hashlib.sha256(mutated.encode()).hexdigest() == row["sha256"]
        for receipt in (root / "mutations" / row["name"]).glob("*/result.json"):
            result = json.loads(receipt.read_text())
            key = row["name"], result["seed"], result["opt"]
            assert key not in mutation_keys
            mutation_keys.add(key)
            assert result["exit"] == 1 and result["status"] == "fail"
            assert result["resumed"] is False and result["cases"] == 600
            assert result["metrics"]["disagree"] > 0
            assert result["metrics"]["subject_faulted"] == 0
            proof = receipt.with_name("proof.json")
            assert hashlib.sha256(proof.read_bytes()).hexdigest() == result["proof_sha256"]
    assert mutation_keys == {
        (row["name"], seed, opt)
        for row in mutation["mutants"]
        for seed in declaration["seeds"]
        for opt in declaration["opts"]
    }
    assert len(mutation_keys) == 8
    print(
        "T1788 PASS: 585 classified; 13 audited; 52 unique tuples; "
        "2 new admissions; 2 mutants killed across 8 tuples"
    )


if __name__ == "__main__":
    main()
