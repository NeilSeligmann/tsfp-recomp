# SPDX-License-Identifier: GPL-3.0-or-later
"""T1520: screen the game functions for the cheap-model replacement pilot work lists.

    python -m tools.pilot_lists --out-dir docs/data/t-pilot-lists --report tmp/pilot/report.json

Waterfall screen over every game function (placeholder-named, in .text, not library), each
exclusion reason counted once in the order the rules are applied. Reads the pinned XBE and
the lifted tree only for the hypothetical caller audit. Nothing owner-private is printed or
written: only addresses, sizes and counts leave this tool. Rules: docs/t-cheap-model-pilot.md.
"""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path
from typing import Any

from tools import llm_replacement_draft as draft
from tools.vector_whitelist import vector_body_admissible

SNAPSHOT = Path("docs/data/replace-proof-snapshot.json")
GAME_DIR = Path("src/game")
REJECT_GLOBS = (
    "docs/t77-*.md",
    "docs/replace-audit-t1252*.md",
    "docs/replace-proof-whole-registry.md",
    "docs/replace-targets.md",
    "docs/t1482-call-bearing-replacements.md",
    "docs/t1486-llm-replacement-pilot.md",
    "docs/evidence/t1252-round*/**/*",
    "docs/evidence/t147[5-9]/**/*",
    "docs/evidence/t1480/**/*",
    "docs/evidence/claude-continuation/**/*",
)
VA_PATTERN = re.compile(r"\b0x([0-9A-Fa-f]{5,8})\b")
SIZE_PREFERRED = 120
SIZE_ALLOWED = 300
MIN_INSNS = 2
BITOPS = {"shl", "shr", "sar", "rol", "ror", "and", "or", "bswap", "bt", "bts", "btr", "btc"}
VECTOR_REG = re.compile(r"\b(?:x|y|z)?mm\d+\b")
MEM_ABS = re.compile(r"\[(0x[0-9a-f]+)\]")
ORDER = (
    "game-function",
    "proven",
    "registered",
    "rejected-before",
    "decode-mismatch",
    "size-over-300",
    "too-short",
    "x87",
    "vector-register",
    "privileged-or-segment",
    "self-modifying-code",
    "call",
    "indirect-jump",
    "jump-out-of-body",
    "implicit-ecx-input",
    "register-input-abi",
    "pointer-writing-bit-packer",
    "loop-with-memory-effects",
    "screen-function-reject",
    "caller-audit-ineligible",
)


def game_functions() -> tuple[Any, list[int]]:
    from tools.name_additions import World

    world = World(Path("."))
    return world, sorted(va for va in world.size if va not in world.library)


def snapshot_sets() -> tuple[set[int], set[int]]:
    data = json.loads(SNAPSHOT.read_text(encoding="utf-8"))
    proven = {int(item["va"], 16) for item in data["functions"] if item["gate"] is None}
    failed = {
        int(item["va"], 16)
        for item in data["functions"]
        if item["gate"] not in (None, "not-a-game-function")
    }
    return proven, failed


def rejected_vas() -> set[int]:
    found: set[int] = set()
    for pattern in REJECT_GLOBS:
        for path in Path(".").glob(pattern):
            if path.is_file():
                text = path.read_text(encoding="utf-8", errors="ignore")
                found |= {int(m.group(1), 16) for m in VA_PATTERN.finditer(text)}
    return found


def classify(func: draft.Function, md: Any, text_range: tuple[int, int]) -> str | None:
    """First exclusion reason for a function body (None when it survives the static rules)."""
    insns = func.insns
    if not insns:
        return "decode-mismatch"
    if func.size > SIZE_ALLOWED:
        return "size-over-300"
    if len(insns) < MIN_INSNS:
        return "too-short"
    decoded = []
    for insn in insns:
        items = list(md.disasm(insn.raw, insn.address))
        if len(items) != 1:
            return "decode-mismatch"
        decoded.append((insn, items[0]))
    groups = [{i.group_name(g) for g in i.groups} for _, i in decoded]
    if any(
        i.mnemonic.startswith("f") or "fpu" in g for (_, i), g in zip(decoded, groups, strict=True)
    ):
        return "x87"
    vec = {"mmx", "sse1", "sse2", "sse3", "ssse3", "sse41", "sse42", "avx"}
    if any(
        g & vec or VECTOR_REG.search(n.operands) for (n, _), g in zip(decoded, groups, strict=True)
    ) and not vector_body_admissible([i for _, i in decoded]):
        return "vector-register"
    priv = {"privilege", "int", "iret", "interrupt"}
    if any(
        g & priv
        or "fs:" in n.operands
        or "gs:" in n.operands
        or "cs:" in n.operands
        or i.mnemonic in {"hlt", "cli", "sti", "in", "out", "int3", "ud2"}
        for (n, i), g in zip(decoded, groups, strict=True)
    ):
        return "privileged-or-segment"
    lo, hi = text_range
    for n, i in decoded:
        dest = n.operands.split(",")[0]
        if i.mnemonic in {"call", "jmp"} or i.mnemonic.startswith("j") or i.mnemonic == "ret":
            continue
        hit = MEM_ABS.search(dest)
        if "[" in dest and hit and lo <= int(hit.group(1), 16) < hi and i.mnemonic != "cmp":
            if i.mnemonic not in {"test", "push"}:
                return "self-modifying-code"
    if any(i.mnemonic == "call" for _, i in decoded):
        return "call"
    for n, i in decoded:
        if i.mnemonic.startswith("j") and not n.operands.startswith("0x"):
            return "indirect-jump"
    for n, i in decoded:
        if i.mnemonic.startswith("j") or i.mnemonic == "loop":
            target = int(n.operands.split()[0], 16)
            if not func.va <= target < func.end:
                return "jump-out-of-body"
    return None


def register_input(func: draft.Function, md: Any) -> set[str]:
    """Registers read before written (same rule as tools.llm_replacement_draft)."""
    written: set[str] = set()
    inputs: set[str] = set()
    for n in func.insns:
        item = next(iter(md.disasm(n.raw, n.address)))
        read, write = item.regs_access()
        full = {draft._full(item.reg_name(r)) for r in read} - draft._SKIP_READS
        ops = n.operands.replace(" ", "").split(",")
        if item.mnemonic in {"xor", "sub", "sbb"} and len(set(ops)) == 1:
            full = set()
        if item.mnemonic == "push":
            full -= {"ebx", "esi", "edi", "ebp"}
        inputs |= full - written
        written |= {draft._full(item.reg_name(r)) for r in write}
    return inputs & {"eax", "ecx", "edx", "ebx", "esi", "edi", "ebp"}


def stores_through_pointer(func: draft.Function) -> bool:
    for n in func.insns:
        parts = n.operands.split(",", 1)
        if n.mnemonic in {"cmp", "test", "push", "lea", "jmp"} or n.mnemonic.startswith("j"):
            continue
        dest = parts[0]
        if "[" in dest and not re.fullmatch(r".*\[(?:esp|ebp)[^\]]*\]", dest):
            if not MEM_ABS.search(dest) or "+" in dest:
                return True
        if n.mnemonic.startswith(("stos", "movs", "rep")):
            return True
    return False


def loop_with_memory(func: draft.Function) -> bool:
    backward = [
        n
        for n in func.insns
        if (n.mnemonic.startswith("j") or n.mnemonic == "loop")
        and n.operands.startswith("0x")
        and int(n.operands.split()[0], 16) <= n.address
    ]
    if not backward:
        return False
    if any(n.mnemonic.startswith(("rep", "stos", "movs", "scas", "cmps")) for n in func.insns):
        return True
    for jump in backward:
        start = int(jump.operands.split()[0], 16)
        for n in func.insns:
            if start <= n.address <= jump.address and "[" in n.operands and n.mnemonic != "lea":
                return True
    return False


def simplicity(func: draft.Function) -> float:
    """Lower is simpler: size, plus weights for branches, memory writes, loops, bit ops."""
    branches = sum(1 for n in func.insns if n.mnemonic.startswith("j") and n.mnemonic != "jmp")
    writes = 0
    for n in func.insns:
        dest = n.operands.split(",")[0]
        if "[" in dest and n.mnemonic not in {"cmp", "test", "push", "lea"}:
            writes += 1
    bits = sum(1 for n in func.insns if n.mnemonic in BITOPS)
    loops = int(
        any(
            n.mnemonic.startswith("j")
            and n.operands.startswith("0x")
            and int(n.operands.split()[0], 16) <= n.address
            for n in func.insns
        )
    )
    return len(func.insns) + 2 * branches + 3 * writes + bits + 8 * loops


def deal(ranked: list[int], lists: int, size: int) -> list[list[int]]:
    """Snake deal so every list gets the same difficulty mix."""
    out: list[list[int]] = [[] for _ in range(lists)]
    for index, va in enumerate(ranked[: lists * size]):
        lap, pos = divmod(index, lists)
        out[pos if lap % 2 == 0 else lists - 1 - pos].append(va)
    return out


def run(args: argparse.Namespace) -> int:
    world, game = game_functions()
    md = draft._capstone()
    counts: Counter[str] = Counter()
    counts["game-function"] = len(game)
    proven, failed = snapshot_sets()
    registered = draft.registered_vas(GAME_DIR)
    rejected = rejected_vas() | failed
    functions = draft.load_functions(Path(args.functions), world.read)
    text_range = (world.text_lo, world.text_hi)
    survivors: list[int] = []
    for va in game:
        if va in proven:
            counts["proven"] += 1
        elif va in registered:
            counts["registered"] += 1
        elif va in rejected:
            counts["rejected-before"] += 1
        else:
            func = functions.get(va)
            if func is None:
                counts["decode-mismatch"] += 1
                continue
            reason = classify(func, md, text_range)
            if reason is None:
                inputs = register_input(func, md)
                if "ecx" in inputs:
                    reason = "implicit-ecx-input"
                elif inputs:
                    reason = "register-input-abi"
                elif (
                    stores_through_pointer(func)
                    and sum(n.mnemonic in BITOPS for n in func.insns) >= 3
                ):
                    reason = "pointer-writing-bit-packer"
                elif loop_with_memory(func):
                    reason = "loop-with-memory-effects"
            if reason is None:
                facts = draft.screen_function(func, md, MIN_INSNS, 10_000)
                reason = None if facts is not None else "screen-function-reject"
            if reason:
                counts[reason] += 1
            else:
                survivors.append(va)
    facts_by_va = {}
    for va in survivors:
        facts = draft.screen_function(functions[va], md, MIN_INSNS, 10_000)
        assert facts is not None
        facts_by_va[va] = facts
    verdicts = draft.audit_eligibility(
        list(facts_by_va.values()), Path(args.xbe), Path(args.gen_dir)
    )
    pool = [va for va in survivors if verdicts.get(va)]
    counts["caller-audit-ineligible"] = len(survivors) - len(pool)
    pool.sort(
        key=lambda va: (
            functions[va].size > SIZE_PREFERRED,
            simplicity(functions[va]),
            va,
        )
    )
    lists = deal(pool, args.lists, args.per_list)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for index, members in enumerate(lists, start=1):
        text = "".join(f"0x{va:08X}\n" for va in sorted(members))
        (out_dir / f"list-{index}.txt").write_text(text, encoding="utf-8")
    report = {
        "counts": {name: counts.get(name, 0) for name in ORDER},
        "pool": len(pool),
        "pool_vas": [f"0x{va:08X}" for va in sorted(pool)],
        "preferred_le_120": sum(1 for va in pool if functions[va].size <= SIZE_PREFERRED),
        "listed": sum(len(m) for m in lists),
        "ranked_pool_head": [
            {
                "va": f"0x{va:08X}",
                "size": functions[va].size,
                "insns": len(functions[va].insns),
                "score": simplicity(functions[va]),
                "stack_args": facts_by_va[va]["stack_args_hint"],
                "ret_pop": facts_by_va[va]["ret_pop"],
            }
            for va in pool[: args.lists * args.per_list]
        ],
    }
    report_path = Path(args.report)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(
        json.dumps(
            {k: v for k, v in report.items() if k not in {"ranked_pool_head", "pool_vas"}}, indent=1
        )
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--functions", default="generated/retail/functions.csv")
    parser.add_argument("--xbe", default="build/default.xbe")
    parser.add_argument("--gen-dir", default="generated/lifted/gen")
    parser.add_argument("--out-dir", default="docs/data/t-pilot-lists")
    parser.add_argument("--report", default="tmp/pilot/report.json")
    parser.add_argument("--lists", type=int, default=4)
    parser.add_argument("--per-list", type=int, default=25)
    return run(parser.parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
