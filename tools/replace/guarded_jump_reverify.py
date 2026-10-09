# SPDX-License-Identifier: GPL-3.0-or-later
"""T1576 defect-functions0: re-derive the per-root guarded-jump checks from stored receipts.

Selects the receipt row by the root VA (never positionally) and re-checks, per root, the four
predeclared tuples: the row is the root, every tuple PASSes the unloosened gate, there are
verdicts and no disagreement or subject fault, the arm witness passed with no failure and the
closure edges passed. No proof is re-run.

    python -m tools.replace.guarded_jump_reverify --receipts DIR [DIR...] --root 0x0003DFA0 ...
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from tools.replace.guarded_jump_contract import current_fingerprint


def check_root(receipt_dirs: list[Path], root: int) -> dict[str, object]:
    prefix = f"{root:08X}-o"
    paths = sorted(
        path
        for directory in receipt_dirs
        for path in directory.glob(f"{prefix}*.json")
        if not path.name.startswith(f"{root:08X}-rebased")
    )
    problems: list[str] = []
    fingerprints: set[object] = set()
    for path in paths:
        receipt = json.loads(path.read_text(encoding="utf-8"))
        function = receipt.get("function") or {}
        witness = receipt.get("arm_witness") or {}
        fingerprints.add(receipt.get("guard_ack_fingerprint"))
        tests = {
            "function-is-root": int(str(function.get("va")), 0) == root,
            "tuple-pass": (receipt.get("tuple") or {}).get("status") == "PASS",
            "gate-clean": not receipt.get("gate_failing"),
            "verdicts": (function.get("verdicts") or 0) > 0,
            "no-disagree": function.get("disagree") == 0 and not function.get("subject_faulted"),
            "arm-witness": bool(witness.get("passed")) and not witness.get("failure_count"),
            "closure-edges": bool((receipt.get("closure_edges") or {}).get("passed")),
        }
        problems += [f"{path.name}:{name}" for name, good in tests.items() if not good]
    if len(paths) != 4:
        problems.append(f"expected 4 tuple receipts, found {len(paths)}")
    return {
        "root": f"0x{root:08X}",
        "receipts": len(paths),
        "status": "PASS" if not problems else "FAIL",
        "problems": problems,
        "stored_ack_fingerprints": sorted(map(str, fingerprints)),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipts", type=Path, nargs="+", required=True)
    parser.add_argument("--root", type=lambda v: int(v, 0), nargs="+", required=True)
    args = parser.parse_args(argv)
    rows = [check_root(args.receipts, root) for root in args.root]
    print(json.dumps({"current_ack_fingerprint": current_fingerprint(), "roots": rows}, indent=2))
    return 0 if all(row["status"] == "PASS" for row in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
