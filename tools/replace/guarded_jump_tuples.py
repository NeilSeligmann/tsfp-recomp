# SPDX-License-Identifier: GPL-3.0-or-later
"""T1576: the fixed proof tuples of one guarded-jump root, one attempt each (T1481).

Seeds 20261001 and 20261006 at -O0 and -O3, 600 cases, the unloosened pilot gate of
`batch_prove.evaluate_gate` PLUS the guarded-jump session (`arm_witness.passed`, every declared
arm dispatched, witness faithful). Serialized, no best-of-N, no retry: a failing tuple stays
failing and is recorded with its first failing gate.

    python -m tools.replace.guarded_jump_tuples --va 0x002777E0 --scratch DIR --out OUT.json \
        -- <prove arguments that are not seed/opt/cases/out-dir/work-dir>
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from tools.replace.batch_prove import DEFAULT_CASES, OPT_LEVELS, SEEDS, evaluate_gate
from tools.replace.guarded_jump_contract import select_root


def run_tuple(
    va: str, seed: int, opt: int, scratch: Path, prove_args: list[str], timeout: int
) -> dict[str, object]:
    out_dir = scratch / f"o{opt}-s{seed}"
    command = [
        sys.executable,
        "-m",
        "tools.replace",
        "prove",
        "--only-va",
        va,
        "--opt-level",
        str(opt),
        "--seed",
        str(seed),
        "--cases-per-function",
        str(DEFAULT_CASES),
        "--work-dir",
        str(scratch / "work"),
        "--out-dir",
        str(out_dir),
        *prove_args,
    ]
    row: dict[str, object] = {"opt": opt, "seed": seed, "cases_requested": DEFAULT_CASES}
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {**row, "status": "ERROR", "why": "timeout"}
    row["returncode"] = done.returncode
    proof_path = next(out_dir.rglob("proof.json"), None)
    if proof_path is None:
        return {**row, "status": "ERROR", "why": "no proof.json", "log": done.stderr[-600:]}
    proof = json.loads(proof_path.read_text(encoding="utf-8"))
    try:
        entry = select_root(proof, va)
    except ValueError as error:
        return {**row, "status": "ERROR", "why": str(error), "proof": str(proof_path)}
    gate = evaluate_gate(entry)
    witness = proof.get("arm_witness") or {}
    edges = proof.get("closure_edges") or {}
    guard_ok = (
        bool(witness.get("passed", True))
        and bool(edges.get("passed", True))
        and proof.get("schema") in (2, 3)
    )
    row.update(
        {
            "status": "PASS" if gate.passed and guard_ok and done.returncode == 0 else "FAIL",
            "gate_failing": gate.failing,
            "gate_reasons": gate.reasons,
            "guard_passed": guard_ok,
            "guard_failures": witness.get("failures", [])[:5],
            "closure_edges_missing": edges.get("missing", []),
            "proof_schema": proof.get("schema"),
            "verdicts": entry["verdicts"],
            "agree": entry["agree"],
            "disagree": entry["disagree"],
            "coverage": entry["coverage"],
            "null_agree_rate": entry["null_agree_rate"],
            "dispatched_arms": witness.get("dispatched_arms"),
            "proof": str(proof_path),
        }
    )
    if proof.get("guarded_sweep"):
        row["guarded_sweep"] = proof["guarded_sweep"]
    return row


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--va", required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=1700)
    parser.add_argument("prove_args", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    prove_args = [a for a in args.prove_args if a != "--"]
    rows = [
        run_tuple(args.va, seed, opt, args.scratch, prove_args, args.timeout)
        for opt in OPT_LEVELS
        for seed in SEEDS
    ]
    document = {
        "schema": 1,
        "va": args.va,
        "policy": "one attempt per tuple, gates unloosened, no best-of-N (T1481)",
        "tuples": rows,
        "all_pass": all(row["status"] == "PASS" for row in rows),
    }
    if "--guarded-sweep-version" in prove_args:
        version = prove_args[prove_args.index("--guarded-sweep-version") + 1]
        document["contract_version"] = f"schema3-sweep-v{version}"
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    for row in rows:
        print(row["opt"], row["seed"], row["status"], row.get("gate_failing"), row.get("why", ""))
    return 0 if document["all_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
