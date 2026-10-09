# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate T1786's fixed declaration, receipts, disposition and native contracts.

No proof tuple is run and no snapshot is modified. Native controls also exercise
frozen rejected drafts; passing native tests alone never confers admission.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

from tools.replace.batch_prove import evaluate_gate
from tools.replace.scan import scan_text


def native(root: Path) -> None:
    dest = root / "tmp/t1786-native-validation"
    dest.mkdir(parents=True, exist_ok=True)
    source = root / "tests/c/test_game_data_campaign.c"
    for compiler in ("gcc", "clang"):
        for opt in (0, 3):
            binary = dest / f"native-{compiler}-o{opt}"
            command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-O{opt}"]
            if compiler == "gcc":
                command.append("-malign-data=abi")
            command += ["-Isrc/game", str(source), "-o", str(binary)]
            subprocess.run(command, cwd=root, check=True)
            subprocess.run([str(binary)], cwd=root, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", action="store_true")
    args = parser.parse_args()
    root = Path.cwd()
    evidence = root / "docs/data/t1786-data"
    declared = json.loads((evidence / "predeclared.json").read_text())
    pin = json.loads((evidence / "draft-pin.json").read_text())
    frozen = evidence / "all-12-drafts.c.txt"
    assert hashlib.sha256(frozen.read_bytes()).hexdigest() == pin["sha256"]
    assert declared["seeds"] == [20261001, 20261006]
    assert declared["opts"] == [0, 3] and declared["cases"] == 600
    assert declared["retry"] is False and declared["ladder"] is False
    targets = {int(row["va"], 16): row for row in declared["roots"]}
    registrations = scan_text(frozen.read_text(), frozen.name)
    assert len(targets) == len(registrations) == 12
    for reg in registrations:
        abi = targets[reg.va]["abi"]
        assert reg.exact_registers and reg.convention == abi["convention"]
        assert reg.stack_args == abi["stack_args"]
        assert targets[reg.va]["audit"]["ok"] is True
    table_vas = []
    for path in evidence.glob("classification-*.md"):
        table_vas += re.findall(r"^\| (0x[0-9A-F]{8}) \|", path.read_text(), re.MULTILINE)
    assert len(table_vas) == len(set(table_vas)) == 864
    expected_counts = {"assets": 91, "cutscenes": 104, "missions": 559, "save": 74, "text": 36}
    for leaf, count in expected_counts.items():
        matches = []
        for path in evidence.glob(f"classification-{leaf}-*.md"):
            matches += re.findall(r"^\| (0x[0-9A-F]{8}) \|", path.read_text(), re.MULTILINE)
        assert len(matches) == count
    expected = {
        (va, seed, opt) for va in targets for seed in declared["seeds"] for opt in declared["opts"]
    }
    observed = {}
    for path in (evidence / "tuples").glob("*/*/result.json"):
        receipt = json.loads(path.read_text())
        va = int(receipt["va"], 16)
        key = (va, receipt["seed"], receipt["opt"])
        assert key not in observed and key in expected
        assert receipt["cases"] == 600 and receipt["resumed"] is False
        assert receipt["exit"] == 0 and receipt["status"] in ("pass", "fail")
        proof_path = path.with_name("proof.json")
        assert hashlib.sha256(proof_path.read_bytes()).hexdigest() == receipt["proof_sha256"]
        proof = json.loads(proof_path.read_text())
        assert proof["edge_cases"] is True
        entry = next(r for r in proof["functions"] if int(r["va"], 16) == va)
        gate = evaluate_gate(entry)
        assert gate.failing == receipt["failing"]
        assert gate.passed == (receipt["status"] == "pass")
        assert entry["disagree"] == entry["subject_faulted"] == 0
        observed[key] = receipt["status"]
    assert observed.keys() == expected and len(observed) == 48
    admitted = {
        va
        for va in targets
        if all(
            observed[(va, seed, opt)] == "pass"
            for seed in declared["seeds"]
            for opt in declared["opts"]
        )
    }
    results = json.loads((evidence / "integer-results.json").read_text())
    assert len(results["functions"]) == 12
    for row in results["functions"]:
        assert row["outcome"] == ("ADMITTED" if int(row["va"], 16) in admitted else "REJECTED")
    live_path = root / "src/game/game_data_campaign.c"
    live = scan_text(live_path.read_text(), str(live_path))
    assert {r.va for r in live} == admitted
    for reg in live:
        assert reg.function == targets[reg.va]["name"]

    # A live admission must retain the exact frozen body, not merely its registration.
    def blocks(text: str) -> dict[int, str]:
        starts = list(re.finditer(r"GAME_REPLACE_EXACT\(([0-9A-F]+),", text))
        return {
            int(match[1], 16): text[
                match.start() : starts[i + 1].start() if i + 1 < len(starts) else len(text)
            ].strip()
            for i, match in enumerate(starts)
        }

    frozen_blocks = blocks(frozen.read_text())
    assert all(body == frozen_blocks[va] for va, body in blocks(live_path.read_text()).items())
    mutants = json.loads((evidence / "native-mutations.json").read_text())
    from tools.t1786_data_native_mutations import MUTATIONS

    mutant_keys = set()
    assert len(mutants) == 48
    for mutant in mutants:
        va = int(mutant["va"], 16)
        key = (va, mutant["compiler"], mutant["opt"])
        assert key not in mutant_keys and mutant["exit"] != 0
        assert tuple(mutant["mutation"]) == MUTATIONS[f"{va:08X}"]
        before, after = mutant["mutation"]
        text = frozen.read_text()
        begin = text.index(f"GAME_REPLACE_EXACT({va:08X},")
        end = text.find("GAME_REPLACE_EXACT(", begin + 1)
        if end < 0:
            end = len(text)
        body = text[begin:end]
        assert body.count(before) == 1
        changed = text[:begin] + body.replace(before, after) + text[end:]
        assert hashlib.sha256(changed.encode()).hexdigest() == mutant["source_sha256"]
        mutant_keys.add(key)
    assert mutant_keys == {
        (va, cc, opt) for va in targets for cc in ("gcc", "clang") for opt in (0, 3)
    }
    if args.native:
        native(root)
    print(
        f"T1786 PASS: 864 classified; 12 audited; 48 unique pinned tuples; "
        f"{len(admitted)} admitted; {12 - len(admitted)} rejected"
    )


if __name__ == "__main__":
    main()
