# SPDX-License-Identifier: GPL-3.0-or-later
"""T1620: measure how many scalar-float roots the fp-scalar-v1 whitelist makes static-eligible.

Runs the T1594 pilot pool screen (`tools.pilot_lists`: the recognizer rules, the caller and
data-reference audit and `llm_replacement_draft.screen_function`) four times with a different
vector predicate and diffs the resulting pools:

* legacy        the shipped T1594 whitelist (10 move/bitwise mnemonics), the baseline pool
* step1         legacy + addss subss mulss comiss ucomiss cvtsi2ss (what T1620 admits)
* step2b        step1 + divss cvttss2si movaps reg-reg (matrix-green, NOT admitted, hypothetical)
* ceiling       every SSE instruction with xmm0-7 operands and no segment (the old "60 roots")

The screens themselves are not changed: the predicates are patched in memory for this
measurement only. A candidate count is a screen+audit count, not a proof count.

    python -m tools.t1620_unlock --xbe build/default.xbe --out docs/evidence/t1620/unlock.json
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path
from typing import Any

STEP2B = frozenset({"divss", "cvttss2si", "movaps"})


def permissive_instruction(insn: Any) -> bool:
    """Ceiling predicate: any SSE/SSE2 instruction on xmm0-7 without a segment override."""
    import re

    from tools.vector_whitelist import has_segment, touches_vector

    if has_segment(insn) or not touches_vector(insn):
        return False
    names = {insn.group_name(group) for group in insn.groups}
    if names & {"mmx", "avx", "x87_fpu", "fpu"} or insn.mnemonic.lower() in {
        "emms",
        "femms",
        "ldmxcsr",
        "stmxcsr",
        "fxsave",
        "fxrstor",
    }:
        return False
    return not re.search(r"\b(?:mm\d|ymm\d+|zmm\d+|xmm(?:[89]|1\d))\b", insn.op_str)


def predicates(name: str) -> tuple[Any, Any]:
    """(body predicate for pilot_lists rule 8, per-instruction predicate for screen_function)."""
    from tools import vector_whitelist as W

    if name == "legacy":
        return W.vector_body_admissible, W.admissible_vector_instruction
    if name == "step1":  # the T1620 step 1 set: production admission minus the step 2b mnemonics

        def step1(insn: Any) -> bool:
            return insn.mnemonic.lower() not in STEP2B and W.admissible_scalar_fp_instruction(insn)

        return (
            lambda insns: (
                all(step1(i) for i in insns if W.touches_vector(i))
                and not any(W.is_x87(i) for i in insns)
            ),
            step1,
        )
    extra = {"step2b": STEP2B}.get(name)
    if extra is not None:
        return (
            lambda insns: W.vector_scalar_fp_body_admissible(insns, extra),
            lambda insn: W.admissible_scalar_fp_instruction(insn, extra),
        )

    def body(insns: list[Any]) -> bool:
        if any(W.is_x87(i) for i in insns):
            return False
        return all(permissive_instruction(i) for i in insns if W.touches_vector(i))

    return body, permissive_instruction


def pool_for(name: str, args: argparse.Namespace) -> set[str]:
    from tools import llm_replacement_draft as draft
    from tools import pilot_lists as pilot

    body, per_instruction = predicates(name)
    saved = (pilot.vector_body_admissible, draft.admissible_vector_instruction)
    pilot.vector_body_admissible, draft.admissible_vector_instruction = body, per_instruction
    try:
        with tempfile.TemporaryDirectory(prefix="t1620-unlock-") as scratch:
            report = Path(scratch) / "report.json"
            pilot.run(
                argparse.Namespace(
                    functions=args.functions,
                    xbe=args.xbe,
                    gen_dir=args.gen_dir,
                    out_dir=str(Path(scratch) / "lists"),
                    report=str(report),
                    lists=4,
                    per_list=25,
                )
            )
            return set(json.loads(report.read_text(encoding="utf-8"))["pool_vas"])
    finally:
        pilot.vector_body_admissible, draft.admissible_vector_instruction = saved


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--functions", default="generated/retail/functions.csv")
    parser.add_argument("--xbe", default="build/default.xbe")
    parser.add_argument("--gen-dir", default="generated/lifted/gen")
    parser.add_argument("--out", type=Path, default=None, help="write the JSON result here")
    args = parser.parse_args(argv)
    pools = {name: pool_for(name, args) for name in ("legacy", "step1", "step2b", "ceiling")}
    base = pools["legacy"]
    result: dict[str, Any] = {
        "xbe": args.xbe,
        "pools": {name: len(pool) for name, pool in pools.items()},
        "lost_vs_legacy": {name: sorted(base - pool) for name, pool in pools.items()},
        "gained": {name: sorted(pool - base) for name, pool in pools.items() if name != "legacy"},
    }
    result["gained_counts"] = {name: len(vas) for name, vas in result["gained"].items()}
    text = json.dumps(result, indent=1, sort_keys=True)
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text + "\n", encoding="utf-8")
    print(json.dumps({k: result[k] for k in ("pools", "gained_counts")}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
