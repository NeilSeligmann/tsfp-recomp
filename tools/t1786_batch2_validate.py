# SPDX-License-Identifier: GPL-3.0-or-later
"""Receipt/native validator for the frozen T1786 batch2; never reruns proof tuples."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import resource
import subprocess
from pathlib import Path

from tools.replace.batch_prove import evaluate_gate
from tools.replace.scan import scan_text

EVIDENCE = Path("docs/data/t1786-batch2")
MUTATIONS = {
    "00085830": ("g_ebp+=g_ebx", "g_ebp-=g_ebx"),
    "0009A260": ("if (!g_ecx)", "if (g_ecx)"),
    "00058240": ("g_ecx*8+0x7DE664,g_eax", "g_ecx*8+0x7DE668,g_eax"),
    "000698D0": ("g_edi=b2_pop()", "g_edi=0x87654321u; g_esp+=4"),
    "000177D0": ("if(!g_ecx)", "if(g_ecx)"),
}


def native(negatives: bool = False) -> list[dict]:
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    work = Path("tmp/batch2-controls").resolve()
    work.mkdir(parents=True, exist_ok=True)
    original = (EVIDENCE / "all-drafts.c.txt").read_text()
    test = Path("tests/c/test_game_data_batch2.c").read_text()
    variants = [("original", original)]
    if negatives:
        for va, (before, after) in MUTATIONS.items():
            start = original.index(f"GAME_REPLACE_EXACT({va},")
            end = original.find("GAME_REPLACE_EXACT(", start + 1)
            end = len(original) if end < 0 else end
            body = original[start:end]
            assert body.count(before) == 1
            variants.append((va, original[:start] + body.replace(before, after) + original[end:]))
    records = []
    for va, source in variants:
        draft = work / f"{va}.c"
        draft.write_text(source)
        runner = work / f"{va}-test.c"
        runner.write_text(test.replace("../../docs/data/t1786-batch2/all-drafts.c.txt", str(draft)))
        for cc in ("gcc", "clang"):
            for opt in (0, 2, 3):
                binary = work / f"{va}-{cc}-{opt}"
                cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-O{opt}"]
                if cc == "gcc":
                    cmd.append("-malign-data=abi")
                subprocess.run(cmd + ["-Isrc/game", str(runner), "-o", str(binary)], check=True)
                run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                assert (run.returncode == 0) == (va == "original"), (va, cc, opt, run.stderr)
                records.append(
                    {
                        "variant": va,
                        "compiler": cc,
                        "opt": opt,
                        "exit": run.returncode,
                        "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
                        "stdout": run.stdout,
                        "stderr": run.stderr,
                    }
                )
    if negatives:
        (EVIDENCE / "native-mutations.json").write_text(json.dumps(records, indent=2) + "\n")
    print(f"Native controls: {len(records)} builds pass expected disposition")
    return records


def validate() -> None:
    declaration = json.loads((EVIDENCE / "predeclared.json").read_text())
    draft = (EVIDENCE / "all-drafts.c.txt").read_text()
    assert hashlib.sha256(draft.encode()).hexdigest() == declaration["source_sha256"]
    targets = {int(r["va"], 16) for r in declaration["roots"]}
    assert len(targets) == 12
    assert declaration["seeds"] == [20261001, 20261006]
    assert declaration["opts"] == [0, 3] and declaration["cases"] == 600
    assert declaration["retry"] is False and declaration["mode"] == "default-domain"
    frozen_regs = scan_text(draft, "all-drafts.c.txt")
    assert {r.va for r in frozen_regs} == targets
    for r in frozen_regs:
        row = next(x for x in declaration["roots"] if int(x["va"], 16) == r.va)
        assert r.exact_registers and r.stack_args == row["stack_args"]
        assert r.function == row["name"] and r.convention == row["convention"]
    spent = json.loads((EVIDENCE / "spent-root-exclusions.json").read_text())
    assert not targets.intersection(int(k, 16) for k in spent)
    assert all(
        r["eligible"] for r in json.loads((EVIDENCE / "caller-audit.json").read_text())["functions"]
    )
    seen = {}
    for p in (EVIDENCE / "tuples").glob("*/*/result.json"):
        r = json.loads(p.read_text())
        key = (int(r["va"], 16), r["seed"], r["opt"])
        assert key not in seen
        seen[key] = r
        assert r["cases"] == 600 and not r["resumed"] and r["exit"] == 0
        proof = p.with_name("proof.json")
        assert hashlib.sha256(proof.read_bytes()).hexdigest() == r["proof_sha256"]
        entry = next(
            f for f in json.loads(proof.read_text())["functions"] if int(f["va"], 16) == key[0]
        )
        gate = evaluate_gate(entry)
        assert gate.passed == (r["status"] == "pass")
        assert gate.first_failing == r["first_failing"]
    assert set(seen) == {(va, s, o) for va in targets for s in (20261001, 20261006) for o in (0, 3)}
    admitted = {
        va
        for va in targets
        if all(seen[va, s, o]["status"] == "pass" for s in (20261001, 20261006) for o in (0, 3))
    }
    live = Path("src/game/game_data_batch2.c").read_text()
    assert {r.va for r in scan_text(live, "game_data_batch2.c")} == admitted
    blocks = re.split(r"(?=GAME_REPLACE_EXACT\()", draft)[1:]
    live_blocks = re.split(r"(?=GAME_REPLACE_EXACT\()", live)[1:]
    assert set(live_blocks).issubset(set(blocks))
    negatives = json.loads((EVIDENCE / "native-mutations.json").read_text())
    assert {int(r["variant"], 16) for r in negatives if r["variant"] != "original"} == admitted
    assert len(negatives) == 6 * (1 + len(admitted))
    assert all((r["exit"] == 0) == (r["variant"] == "original") for r in negatives)
    for row in negatives:
        variant = row["variant"]
        if variant == "original":
            source = draft
        else:
            begin = draft.index(f"GAME_REPLACE_EXACT({variant},")
            end = draft.find("GAME_REPLACE_EXACT(", begin + 1)
            end = len(draft) if end < 0 else end
            a, b = MUTATIONS[variant]
            source = draft[:begin] + draft[begin:end].replace(a, b) + draft[end:]
        assert hashlib.sha256(source.encode()).hexdigest() == row["source_sha256"]
    aliases = json.loads((EVIDENCE / "pop-alias-original.json").read_text())
    assert len(aliases) == 12 and all(r["match"] for r in aliases)
    print(f"T1786 batch2 PASS: 12 roots, 48 unique pinned tuples, {len(admitted)} admissions")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--record-negatives", action="store_true")
    args = parser.parse_args()
    if args.record_negatives:
        native(True)
    else:
        validate()
        if args.native:
            native()
