# SPDX-License-Identifier: GPL-3.0-or-later
"""T1576: trim the schema3 proof documents of a tuple run into tracked evidence receipts.

A proof.json carries the whole closure and per-function rows. The receipt keeps what a reviewer
needs to re-check the verdict: the tuple identity, the gate row, the arm witness (campaign level),
the closure guards/edges/events, the closure node hashes and the guard identity. Receipts are
written under docs/evidence (never docs/data: tests refuse schema3 documents there).

    python -m tools.replace.guarded_jump_receipts --tuples TUPLES.json --out-dir DIR
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from tools.replace.guarded_jump_contract import select_root

KEEP_FUNCTION = (
    "va",
    "name",
    "cases",
    "verdicts",
    "agree",
    "disagree",
    "subject_faulted",
    "oracle_faulted",
    "coverage",
    "near_vacuous",
    "null_agree_rate",
    "replaced_confirmed",
)


def trim(proof: dict, row: dict, va: str) -> dict:
    function = select_root(proof, va)
    closure = proof.get("live_call_closure") or {}
    receipt = {
        "schema": proof.get("schema"),
        "base_schema": proof.get("base_schema"),
        "guarded_jump_contract": proof.get("guarded_jump_contract"),
        "tuple": {key: row.get(key) for key in ("opt", "seed", "cases_requested", "status")},
        "gate_failing": row.get("gate_failing"),
        "function": {key: function.get(key) for key in KEEP_FUNCTION},
        "arm_witness": proof.get("arm_witness"),
        "tail_target": proof.get("tail_target"),
        "closure_edges": proof.get("closure_edges"),
        "table_sections": proof.get("table_sections"),
        "closure_events": proof.get("closure_events"),
        "closure_guards": proof.get("closure_guards"),
        "closure_nodes": [
            {key: node.get(key) for key in ("va", "size", "sha256", "calls", "tails", "tables")}
            for node in closure.get("nodes", [])
        ],
        "guard_identity": proof.get("guard_identity"),
        "guard_ack_fingerprint": proof.get("guard_ack_fingerprint"),
        "subject": proof.get("subject"),
        "seed": proof.get("seed"),
        "cases_per_function": proof.get("cases_per_function"),
    }
    return {key: value for key, value in receipt.items() if value is not None}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tuples", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--prefix", required=True, help="file name prefix, e.g. 002777E0")
    args = parser.parse_args(argv)
    summary = json.loads(args.tuples.read_text(encoding="utf-8"))
    args.out_dir.mkdir(parents=True, exist_ok=True)
    for row in summary["tuples"]:
        proof_path = Path(row["proof"]) if row.get("proof") else None
        if proof_path is None or not proof_path.exists():
            continue
        proof = json.loads(proof_path.read_text(encoding="utf-8"))
        name = f"{args.prefix}-o{row['opt']}-s{row['seed']}.json"
        (args.out_dir / name).write_text(
            json.dumps(trim(proof, row, summary["va"]), indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(name)
    (args.out_dir / f"{args.prefix}-tuples.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
