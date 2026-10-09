# SPDX-License-Identifier: GPL-3.0-or-later
"""Classify historical unresolved label sites conservatively, with operand witnesses.

Never infer a label from adjacent pushes across a branch or call. A classification
is a data dependency, not a proof of a finite label set. Input is the pinned T1507
callsites inventory; current function names are deliberately irrelevant.
"""

import argparse
import csv
import json
from collections import Counter
from pathlib import Path
from typing import Any

from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from tools.label_callsites import call_target, decode, jump_targets
from tools.name_additions import World


def classify(insns: list[Any], call_index: int, position: int) -> tuple[str, Any, str]:
    """Return disposition and witnesses, stopping at control/stack ambiguity."""
    targets = jump_targets(insns)
    pushes = 0
    for i in range(call_index - 1, -1, -1):
        ins = insns[i]
        if ins.mnemonic.startswith("j") or ins.mnemonic in ("call", "ret", "retn"):
            return (
                "control-dependent-stack",
                ins,
                "recover predecessor stack states before selecting argument",
            )
        if ins.mnemonic == "push":
            if pushes != position:
                pushes += 1
                continue
            op = ins.operands[0]
            if ins.address in targets:
                return (
                    "control-dependent-value",
                    ins,
                    "merge incoming argument values at push target",
                )
            if op.type == X86_OP_IMM:
                return (
                    "immediate-candidate",
                    ins,
                    "validate stack rank and label range before promoting",
                )
            if op.type == X86_OP_MEM:
                return "memory-dependent", ins, "identify record/table writer and index domain"
            if op.type != X86_OP_REG:
                return "unsupported-operand", ins, "decode argument operand"
            reg = op.reg
            for prior in reversed(insns[:i]):
                if prior.mnemonic.startswith("j") or prior.address in targets:
                    return (
                        "control-dependent-value",
                        prior,
                        "merge incoming register values with CFG proof",
                    )
                if prior.mnemonic in ("ret", "retn"):
                    return (
                        "entry-register-dependent",
                        ins,
                        "establish register ABI at function boundary",
                    )
                if prior.mnemonic == "call" and ins.reg_name(reg) in ("eax", "ecx", "edx"):
                    return (
                        "callee-result-dependent",
                        prior,
                        "prove callee return domain and calling convention",
                    )
                _, written = prior.regs_access()
                families = {
                    "eax": {"eax", "ax", "al", "ah"},
                    "ebx": {"ebx", "bx", "bl", "bh"},
                    "ecx": {"ecx", "cx", "cl", "ch"},
                    "edx": {"edx", "dx", "dl", "dh"},
                    "esi": {"esi", "si"},
                    "edi": {"edi", "di"},
                    "ebp": {"ebp", "bp"},
                }
                family = families.get(ins.reg_name(reg), {ins.reg_name(reg)})
                if not any(prior.reg_name(w) in family for w in written):
                    continue
                if reg not in written:
                    return (
                        "partial-register-dependent",
                        prior,
                        "compose partial register write with incoming high bits",
                    )
                ops = prior.operands
                if (
                    prior.mnemonic in ("mov", "movzx", "movsx")
                    and len(ops) > 1
                    and ops[1].type == X86_OP_MEM
                ):
                    return (
                        "memory-dependent",
                        prior,
                        "identify record/table writer and index domain",
                    )
                if prior.mnemonic == "mov" and len(ops) > 1 and ops[1].type == X86_OP_IMM:
                    return (
                        "immediate-candidate",
                        prior,
                        "validate path, stack rank and label range before promoting",
                    )
                return (
                    "expression-dependent",
                    prior,
                    "trace arithmetic/register inputs and prove label domain",
                )
            return "entry-register-dependent", ins, "establish register ABI at function boundary"
        if ins.address in targets:
            return (
                "control-dependent-stack",
                ins,
                "recover predecessor stack states before selecting argument",
            )
        if ins.mnemonic == "pop":
            return "stack-write-dependent", ins, "recover stack depth after pop"
        if (
            ins.mnemonic in ("add", "sub", "mov", "lea")
            and ins.operands
            and ins.operands[0].type == X86_OP_REG
            and ins.reg_name(ins.operands[0].reg) == "esp"
        ):
            return (
                "stack-write-dependent",
                ins,
                "recover outgoing stack argument store and stack depth",
            )
    return (
        "entry-stack-dependent",
        insns[call_index],
        "establish entry stack contract or preceding function boundary",
    )


def audit(root: Path, inventory: Path, output: Path) -> list[dict[str, Any]]:
    world = World(root)
    if inventory.suffix == ".json":
        source = json.loads(inventory.read_text())
        getters = {int(row["getter"], 16): row["argument_position"] for row in source}
    else:
        with inventory.open() as handle:
            source = list(csv.DictReader(handle))
        getters = {
            int(k, 16): v
            for k, v in json.loads((inventory.parent / "getters.json").read_text()).items()
        }
    rows = []
    for row in source:
        if row.get("status", "unresolved") != "unresolved":
            continue
        entry, call, getter = (int(row[k], 16) for k in ("function", "call", "getter"))
        insns = decode(world.read(entry, world.size[entry]), entry)
        index = next(i for i, ins in enumerate(insns) if ins.address == call)
        if call_target(insns[index]) != getter:
            raise ValueError(f"original call mismatch at {call:#x}")
        kind, witness, remaining = classify(insns, index, getters[getter])
        rows.append(
            dict(
                site=f"site-{len(rows) + 1:03}",
                function=row["function"],
                call=row["call"],
                getter=row["getter"],
                argument_position=getters[getter],
                historical_reason=row.get("historical_reason", row.get("reason")),
                dependency=kind,
                witness_va=f"{witness.address:#010x}",
                witness=witness.mnemonic + " " + witness.op_str,
                remaining_proof=remaining,
                confidence="MEASURED operand; INFERRED dependency",
            )
        )
    output.write_text(json.dumps(rows, indent=2) + "\n")
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = audit(args.root, args.inventory, args.output)
    print(len(rows), dict(Counter(row["dependency"] for row in rows)))


if __name__ == "__main__":
    main()
