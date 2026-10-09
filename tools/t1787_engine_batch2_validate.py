# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate batch2 frozen unique receipts, admission and spent-root safety."""

import hashlib
import json
import re
from pathlib import Path

from tools.harness.synth_domain import canonical
from tools.replace.batch_prove import evaluate_gate, synth_block_failure
from tools.replace.scan import scan_text


def main() -> None:
    root = Path("docs/data/t1787-engine/batch2")
    declaration = json.loads((root / "predeclared.json").read_text())
    frozen = root / "frozen-draft.c.txt"
    assert hashlib.sha256(frozen.read_bytes()).hexdigest() == declaration["draft_sha256"]
    roots = {int(r["va"], 16): r for r in declaration["roots"]}
    assert len(roots) == 12
    previous = json.loads(Path("docs/data/t1787-engine/predeclared.json").read_text())
    assert not set(roots) & {int(r["va"], 16) for r in previous["roots"]}
    assert all(r["executed"] == "0" for r in roots.values())
    keys = set()
    outcomes = {va: [] for va in roots}
    for p in (root / "tuples").glob("*/*/result.json"):
        result = json.loads(p.read_text())
        va = int(result["va"], 16)
        key = va, result["seed"], result["opt"]
        assert key not in keys
        keys.add(key)
        assert result["cases"] == 600 and not result["resumed"]
        assert result["exit"] == 0
        proof_path = p.with_name("proof.json")
        assert hashlib.sha256(proof_path.read_bytes()).hexdigest() == result["proof_sha256"]
        proof = json.loads(proof_path.read_text())
        entry = next(r for r in proof["functions"] if int(r["va"], 16) == va)
        gate = evaluate_gate(entry)
        assert gate.failing == result["failing"]
        assert gate.passed == (result["status"] == "pass")
        assert not entry["disagree"] and not entry["subject_faulted"]
        if roots[va]["flow"] == "synth-v2":
            artifact = p.with_name("domain.json")
            assert artifact.exists()
            domain = json.loads(artifact.read_text())
            assert domain["generator_version"] == "t1773-synth-v2"
            digest = domain.pop("sha256")
            assert hashlib.sha256(canonical(domain)).hexdigest() == digest
            assert digest == result["synth_domain_sha256"]
            assert synth_block_failure(entry) is None
            assert entry["synth_domain"]["domain_sha256"] == digest
            assert entry["synth_domain"]["case_stream_sha256"] == result["synth_stream_sha256"]
            assert result["synth_domain"]
        outcomes[va].append(result["status"])
    assert keys == {(va, s, o) for va in roots for s in (20261001, 20261006) for o in (0, 3)}
    admitted = {va for va, statuses in outcomes.items() if statuses == ["pass"] * 4}
    live = Path("src/game/game_engine_batch2.c").read_text()
    assert {r.va for r in scan_text(live, "batch2")} == admitted
    for va in admitted:
        pattern = rf"GAME_REPLACE_EXACT\({va:08X},[^\n]*\n\{{[^\n]*\}}"
        assert re.search(pattern, live).group() == re.search(pattern, frozen.read_text()).group()
    mutants = json.loads((root / "native-mutants.json").read_text())
    assert len(mutants) == 12 and sum(len(m["runs"]) for m in mutants) == 48
    assert all(r["exit"] != 0 for m in mutants for r in m["runs"])
    print(
        f"T1787 batch2: {len(keys)} unique spent tuples; {len(admitted)} admissions; "
        "48 native mutants killed"
    )


if __name__ == "__main__":
    main()
