# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the pinned T1787 classification, unique fixed proof tuples and admission."""

import hashlib
import json
import re
from pathlib import Path

from tools.replace.batch_prove import evaluate_gate
from tools.replace.scan import scan_text


def main() -> None:
    root = Path("docs/data/t1787-engine")
    declaration = json.loads((root / "predeclared.json").read_text())
    draft = root / "proved-all-12-drafts.c.txt"
    assert hashlib.sha256(draft.read_bytes()).hexdigest() == declaration["draft_sha256"]
    registrations = scan_text(draft.read_text(), draft.name)
    roots = {int(r["va"], 16): r for r in declaration["roots"]}
    assert len(roots) == len(registrations) == 12
    for reg in registrations:
        assert reg.exact_registers and reg.convention == "cdecl"
        assert reg.stack_args == roots[reg.va]["args"]
    addresses = []
    for table in root.glob("classification-*.md"):
        addresses.extend(re.findall(r"^\| (0x[0-9A-F]{8}) \|", table.read_text(), re.M))
    assert len(addresses) == len(set(addresses)) == 2402
    tuples = set()
    statuses = {va: [] for va in roots}
    for result_path in (root / "tuples").glob("*/*/result.json"):
        result = json.loads(result_path.read_text())
        key = (int(result["va"], 16), result["seed"], result["opt"])
        assert key not in tuples
        tuples.add(key)
        assert result["cases"] == 600 and not result["resumed"]
        assert result["exit"] == 0 and result["status"] in ("pass", "fail")
        proof_path = result_path.with_name("proof.json")
        assert hashlib.sha256(proof_path.read_bytes()).hexdigest() == result["proof_sha256"]
        proof = json.loads(proof_path.read_text())
        entry = next(x for x in proof["functions"] if int(x["va"], 16) == key[0])
        gate = evaluate_gate(entry)
        assert gate.failing == result["failing"]
        assert gate.passed == (result["status"] == "pass")
        statuses[key[0]].append(result["status"])
    assert tuples == {
        (va, seed, opt) for va in roots for seed in (20261001, 20261006) for opt in (0, 3)
    }
    admitted = {va for va, outcomes in statuses.items() if outcomes == ["pass"] * 4}
    live = scan_text(Path("src/game/game_engine_campaign.c").read_text(), "game_engine_campaign.c")
    assert {reg.va for reg in live} == admitted
    for reg in live:
        pattern = rf"GAME_REPLACE_EXACT\({reg.va:08X},[^\n]*\n\{{[^\n]*\}}"
        assert (
            re.search(pattern, draft.read_text()).group()
            == re.search(pattern, Path("src/game/game_engine_campaign.c").read_text()).group()
        )
    mutant_declaration = json.loads((root / "mutants/predeclared.json").read_text())
    for mutation in mutant_declaration["mutations"]:
        mutant = root / "mutants" / (mutation["name"] + ".c.txt")
        assert hashlib.sha256(mutant.read_bytes()).hexdigest() == mutation["draft_sha256"]
        assert mutant.read_text() == draft.read_text().replace(mutation["old"], mutation["new"])
    scope = json.loads((root / "control-scope.json").read_text())
    mutant_keys = set()
    for name in scope["tested_original_mutants"]:
        records = list((root / "mutant-tuples" / name).glob("*/*/result.json"))
        assert len(records) == 4
        for path in records:
            result = json.loads(path.read_text())
            key = (name, result["seed"], result["opt"])
            assert key not in mutant_keys
            mutant_keys.add(key)
            assert result["exit"] == 1 and result["status"] == "fail"
            assert result["metrics"]["disagree"] > 0 and not result["resumed"]
            assert (
                hashlib.sha256(path.with_name("proof.json").read_bytes()).hexdigest()
                == result["proof_sha256"]
            )
    assert len(mutant_keys) == 8
    for name in scope["remaining_predeclared_not_executed"]:
        assert not (root / "mutant-tuples" / name).exists()
    native = json.loads((root / "native-mutants.json").read_text())
    assert len(native) == 48 and all(x["outcome"] == "KILLED" for x in native)
    for p in (root / "preflight-v1").glob("*/*/result.json"):
        result = json.loads(p.read_text())
        assert result["exit"] == 2 and result["first_failing"] == "no_proof"
        assert not p.with_name("proof.json").exists()
    print(
        f"T1787 PASS: 2402 classifications, 48 unique measured tuples, "
        f"{len(admitted)} admitted, 48 native mutants killed"
    )


if __name__ == "__main__":
    main()
