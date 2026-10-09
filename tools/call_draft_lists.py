# SPDX-License-Identifier: GPL-3.0-or-later
"""T1534 offline conservative live-closure / exact-input draft worklists. No API calls."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from collections import Counter, deque
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from tools import llm_replacement_draft as draft
from tools import pilot_lists as pilot
from tools.harness.callclosure import LiveClosureError, discover_live_call_closure
from tools.harness.callstub import CalleeConventions
from tools.harness.image import build_guest_image
from tools.replace.audit import audit_all
from tools.replace.manifest import ManifestEntry
from tools.replace.straddle_corpus import data_ranges

REGS = ("eax", "ecx", "edx", "ebx", "ebp", "esi", "edi")
SUPPORTED = ((), ("ecx",), ("esi",), ("ecx", "esi"))
PLUMBING = {"push", "pop", "call", "ret", "leave", "nop", "int3"}


# T1575: only these exact encodings of `lea esp, [esp]` (32-bit, no prefix) are no-ops.
LEA_ESP_IDENTITY = frozenset({bytes.fromhex("8d2424"), bytes.fromhex("8da42400000000")})


class Refusal(ValueError):
    """A bounded recognizer cannot authenticate this contract; never a runtime verdict."""


@dataclass(frozen=True)
class ABI:
    inputs: tuple[str, ...]
    stack_args: int
    pop: int
    outputs: tuple[frozenset[str], ...]
    ebp_preserved: bool
    flags_defined: bool = False
    stack_tainted_outputs: frozenset[str] = frozenset()

    def document(self) -> dict[str, Any]:
        return {
            "convention": "stdcall" if self.pop else "cdecl",
            "stack_args": self.stack_args,
            "cleanup_bytes": self.pop,
            "register_inputs": list(self.inputs),
            "returns": "eax",
            "scratch": [],
            "input_abi": "legacy" if not self.inputs else "_".join(self.inputs),
        }


@dataclass(frozen=True)
class State:
    sp: int = 0
    bp: int | None = None
    origins: tuple[frozenset[str], ...] = tuple(frozenset({r}) for r in REGS)
    # Original pushes/local stores carry register origins for authentic restores.
    slots: tuple[tuple[int, frozenset[str], int | None, str, bool], ...] = ()
    flags_defined: bool = False
    bp_role: str = "incoming"
    stack_tainted: frozenset[str] = frozenset()


class ABIRecognizer:
    """Bounded original CFG dataflow, recursive direct-call summaries, affine frames.

    Joins require identical stack/frame shape; dependency sets union monotonically.
    Unknown incoming nonvolatile register use is retained, not assumed to be ABI scratch.
    """

    def __init__(self, sizes: dict[int, int], read: Any, *, max_states: int = 20000) -> None:
        self.sizes, self.read = sizes, read
        self.md = draft._capstone()
        self.conventions = CalleeConventions(sizes, read, self.md)
        self.cache: dict[int, ABI] = {}
        self.active: set[int] = set()
        self.max_states = max_states

    def infer(self, va: int) -> ABI:
        if va in self.cache:
            return self.cache[va]
        if va in self.active or len(self.active) >= 32:
            raise Refusal("abi-recursion-or-depth")
        self.active.add(va)
        try:
            target = self.conventions.thunk_target(va)
            if target is None:
                answer = self._infer(va)
            else:
                # T1578: only the shared complete-body thunk/cleanup proof may delegate.
                # Its depth bound remains independent of this recognizer's CFG depth.
                approved_pop = self.conventions.pop_bytes(va)
                if approved_pop is None:
                    raise Refusal("abi-unknown-cleanup")
                answer = self.infer(target)
                if answer.pop != approved_pop:
                    raise Refusal("abi-thunk-cleanup-mismatch")
            self.cache[va] = answer
            return answer
        finally:
            self.active.remove(va)

    def _infer(self, va: int) -> ABI:
        if va not in self.sizes:
            raise Refusal("abi-missing-extent")
        size = self.sizes[va]
        code = self.read(va, size)
        insns = list(self.md.disasm(code, va))
        if not insns or len(code) != size or sum(i.size for i in insns) != size:
            raise Refusal("abi-decode")
        by_va = {i.address: i for i in insns}
        pop = self.conventions.pop_bytes(va)
        if pop is None or pop % 4 or pop > 32:
            raise Refusal("abi-unknown-cleanup")
        required: set[str] = set()
        args = pop // 4
        pending = deque([(va, State())])
        seen: dict[int, State] = {}
        returns: list[State] = []
        steps = 0
        while pending:
            pc, state = pending.popleft()
            steps += 1
            if steps > self.max_states:
                raise Refusal("abi-state-bound")
            old = seen.get(pc)
            if old is not None:
                if (old.sp, old.bp, old.bp_role, tuple((x[0], x[2], x[3]) for x in old.slots)) != (
                    state.sp,
                    state.bp,
                    state.bp_role,
                    tuple((x[0], x[2], x[3]) for x in state.slots),
                ):
                    raise Refusal("abi-inconsistent-frame-join")
                merged = State(
                    state.sp,
                    state.bp,
                    tuple(a | b for a, b in zip(old.origins, state.origins, strict=True)),
                    tuple(
                        (a[0], a[1] | b[1], a[2], a[3], a[4] or b[4])
                        for a, b in zip(old.slots, state.slots, strict=True)
                    ),
                    old.flags_defined and state.flags_defined,
                    state.bp_role,
                    old.stack_tainted | state.stack_tainted,
                )
                if merged == old:
                    continue
                state = merged
            seen[pc] = state
            insn = by_va.get(pc)
            if insn is None:
                raise Refusal("abi-interior-or-outside-control")
            m = insn.mnemonic
            ops = insn.operands
            origins = dict(zip(REGS, state.origins, strict=True))
            slots = {o: (d, a, role, taint) for o, d, a, role, taint in state.slots}
            sp, bp = state.sp, state.bp
            bp_role = state.bp_role
            stack_tainted = set(state.stack_tainted)
            flags_defined = state.flags_defined
            from capstone import x86_const as xc

            flag_names = ("CF", "PF", "ZF", "SF", "OF")
            tested = any(insn.eflags & getattr(xc, "X86_EFLAGS_TEST_" + f) for f in flag_names)
            if tested and not flags_defined:
                raise Refusal("abi-incoming-arithmetic-flags")
            if all(
                any(
                    insn.eflags & getattr(xc, "X86_EFLAGS_" + action + "_" + f, 0)
                    for action in ("MODIFY", "SET", "RESET", "UNDEFINED")
                )
                for f in flag_names
            ):
                flags_defined = True
            reads, writes = insn.regs_access()
            read_regs = {draft._full(insn.reg_name(r)) for r in reads} & set(REGS)
            write_regs = {draft._full(insn.reg_name(r)) for r in writes} & set(REGS)
            zero = (
                m in {"xor", "sub"}
                and len(ops) == 2
                and ops[0].type == 1
                and (ops[1].type == 1 and ops[0].reg == ops[1].reg)
            )
            if zero:
                read_regs.clear()
            derives_stack = not zero and (
                any(insn.reg_name(r) in {"esp", "sp"} for r in reads)
                or bool(read_regs & stack_tainted)
                or ("ebp" in read_regs and bp_role == "frame")
            )
            deps = frozenset().union(*(origins[r] for r in read_regs))
            # A callee-saved PUSH is a save, not use of an incoming argument.
            if m != "push" or len(ops) != 1 or insn.op_str not in {"ebx", "ebp", "esi", "edi"}:
                required.update(deps)
            stack_refs: list[tuple[int, int]] = []
            for index, op in enumerate(ops):
                if op.type != 3:
                    continue
                base = insn.reg_name(op.mem.base)
                idx = insn.reg_name(op.mem.index)
                if idx == "esp" or (idx == "ebp" and bp_role == "frame"):
                    raise Refusal("abi-indexed-stack")
                if (base == "ebp" or idx == "ebp") and "ebp" in stack_tainted:
                    raise Refusal("abi-stack-derived-ebp")
                if {base, idx} & stack_tainted:
                    raise Refusal("abi-stack-derived-pointer")
                if base != "esp" and not (base == "ebp" and bp_role == "frame"):
                    continue
                if idx:
                    raise Refusal("abi-indexed-stack")
                delta = sp if base == "esp" else bp
                if delta is None:
                    raise Refusal("abi-unknown-frame")
                offset = delta + op.mem.disp
                if offset >= 4:
                    args = max(args, (offset + op.size - 1) // 4)
                stack_refs.append((index, offset))
            if args > 8:
                raise Refusal("abi-over-eight-stack-words")
            if m == "call":
                if len(ops) != 1 or ops[0].type != 2:
                    raise Refusal("abi-indirect-call")
                child = self.infer(ops[0].imm)
                if set(child.inputs) & stack_tainted or (
                    "ebp" in child.inputs and bp_role == "frame"
                ):
                    raise Refusal("abi-stack-derived-child-input")
                required.update(*(origins[r] for r in child.inputs))
                for index in range(child.stack_args):
                    offset = sp + index * 4
                    if offset >= 4:
                        args = max(args, offset // 4)
                    if offset in slots:
                        required.update(slots[offset][0])
                prior = origins.copy()
                for reg, out in zip(REGS, child.outputs, strict=True):
                    origins[reg] = frozenset().union(*(prior[r] for r in out))
                if not child.ebp_preserved:
                    raise Refusal("abi-child-ebp-not-preserved")
                prior_taint = stack_tainted.copy()
                stack_tainted = set(child.stack_tainted_outputs) | {
                    reg for reg, out in zip(REGS, child.outputs, strict=True) if out & prior_taint
                }
                flags_defined |= child.flags_defined
                sp += child.pop
                # Discard callee-popped argument slots; preserve caller saves/locals.
                # Child direct incoming-stack stores are refused by this recognizer.
                slots = {o: v for o, v in slots.items() if o >= sp}
            elif m == "push":
                sp -= 4
                affine = bp if insn.op_str == "ebp" else None
                slots[sp] = (
                    deps,
                    affine,
                    bp_role if insn.op_str == "ebp" else "data",
                    insn.op_str in stack_tainted
                    or insn.op_str in {"esp", "sp"}
                    or (insn.op_str == "ebp" and bp_role == "frame")
                    or (ops[0].type == 3 and derives_stack),
                )
            elif m == "pop":
                if len(ops) != 1 or ops[0].type != 1:
                    raise Refusal("abi-pop-shape")
                reg = draft._full(insn.reg_name(ops[0].reg))
                if sp not in slots or reg not in REGS:
                    raise Refusal("abi-untracked-pop")
                origins[reg], affine, saved_role, saved_taint = slots.pop(sp)
                stack_tainted.discard(reg)
                if saved_taint:
                    stack_tainted.add(reg)
                if reg == "ebp":
                    bp, bp_role = affine, saved_role
                sp += 4
            elif m == "leave":
                if bp is None or bp not in slots:
                    raise Refusal("abi-untracked-leave")
                sp = bp
                origins["ebp"], bp, bp_role, saved_taint = slots.pop(sp)
                stack_tainted.discard("ebp")
                if saved_taint:
                    stack_tainted.add("ebp")
                sp += 4
            elif (
                m in {"add", "sub"}
                and ops
                and ops[0].type == 1
                and insn.reg_name(ops[0].reg) == "esp"
            ):
                if len(ops) != 2 or ops[1].type != 2:
                    raise Refusal("abi-nonconstant-stack")
                sp += ops[1].imm * (1 if m == "add" else -1)
                slots = {o: v for o, v in slots.items() if o >= sp}
            elif m == "mov" and insn.op_str.replace(" ", "") == "ebp,esp":
                bp, bp_role = sp, "frame"
                stack_tainted.discard("ebp")
                origins["ebp"] = frozenset()
            elif m == "mov" and insn.op_str.replace(" ", "") == "esp,ebp":
                if bp is None:
                    raise Refusal("abi-unknown-frame-restore")
                sp = bp
            elif m.startswith("ret"):
                if sp != 0:
                    raise Refusal("abi-unbalanced-return")
                returns.append(
                    State(
                        sp,
                        bp,
                        tuple(origins[r] for r in REGS),
                        flags_defined=flags_defined,
                        bp_role=bp_role,
                        stack_tainted=frozenset(stack_tainted),
                    )
                )
                continue
            elif bytes(insn.bytes) in LEA_ESP_IDENTITY:
                # T1575: exact `lea esp, [esp+0]` leaves esp unchanged and has no other effect.
                pass
            else:
                if {"esp", "sp"} & {insn.reg_name(r) for r in writes}:
                    raise Refusal("abi-unsupported-stack-write")
                prior_origins = origins.copy()
                partial = {
                    draft._full(insn.reg_name(r))
                    for r in writes
                    if insn.reg_name(r) != draft._full(insn.reg_name(r))
                }
                for reg in write_regs:
                    origins[reg] = deps | (prior_origins[reg] if reg in partial else frozenset())
                for reg in write_regs:
                    stack_tainted.discard(reg)
                    if derives_stack or (reg in partial and reg in state.stack_tainted):
                        stack_tainted.add(reg)
                if "ebp" in write_regs:
                    bp, bp_role = None, "data"
                    if "ebp" in stack_tainted:
                        raise Refusal("abi-stack-derived-ebp")
                # MOV load from tracked saved/local slot carries original dependencies.
                if m == "mov" and len(ops) == 2 and ops[0].type == 1:
                    reg = draft._full(insn.reg_name(ops[0].reg))
                    if reg in origins:
                        if ops[1].type == 1:
                            origins[reg] = origins.get(
                                draft._full(insn.reg_name(ops[1].reg)), frozenset()
                            )
                        elif ops[1].type in {2, 3}:
                            origins[reg] = frozenset()
                        if reg in partial:
                            origins[reg] |= prior_origins[reg]
                        for index, offset in stack_refs:
                            if index == 1 and offset < 0 and offset in slots:
                                origins[reg] = slots[offset][0]
                for index, offset in stack_refs:
                    if index == 0 and ops[0].access & 2:
                        if offset >= 0 or ops[0].size != 4:
                            raise Refusal("abi-return-slot-or-partial-stack-store")
                        slots[offset] = (deps, None, "data", derives_stack)
            if abs(sp) > 65536 or args > 8:
                raise Refusal("abi-frame-or-argument-bound")
            nxt = State(
                sp,
                bp,
                tuple(origins[r] for r in REGS),
                tuple((o, *v) for o, v in sorted(slots.items())),
                flags_defined,
                bp_role,
                frozenset(stack_tainted),
            )
            fallthrough = insn.address + insn.size
            if m.startswith("j") or m.startswith("loop"):
                if len(ops) != 1 or ops[0].type != 2 or ops[0].imm not in by_va:
                    raise Refusal("abi-branch-target")
                pending.append((ops[0].imm, nxt))
                if m == "jmp":
                    continue
            pending.append((fallthrough, nxt))
        if not returns:
            raise Refusal("abi-no-reachable-return")
        if pop and args > pop // 4:
            raise Refusal("abi-stack-use-exceeds-cleanup")
        return ABI(
            tuple(r for r in REGS if r in required),
            args,
            pop,
            tuple(frozenset().union(*(s.origins[n] for s in returns)) for n in range(len(REGS))),
            all(s.origins[REGS.index("ebp")] == frozenset({"ebp"}) for s in returns),
            all(s.flags_defined for s in returns),
            frozenset().union(*(s.stack_tainted for s in returns)),
        )


_STRING_OPS = ("movs", "stos", "lods", "scas", "cmps")


def _implicit_es_string_operand_only(item: Any) -> bool:
    """True for a string op with no segment-override prefix byte.

    Capstone prints the architectural destination as `es:[edi]`, so ':' alone is
    not an override. Without a prefix byte (item.prefix[1]) the only segment text
    a movs/stos/lods/scas/cmps form can carry is that implicit es:[edi].
    """
    if item.prefix[1] != 0 or item.prefix[3] != 0:
        return False
    return item.mnemonic.split()[-1] in {base + width for base in _STRING_OPS for width in "bwd"}


def closure_capability(items: list[Any], text: tuple[int, int]) -> None:
    """Capability guards do not inherit root ranking size/instruction thresholds."""
    from tools.harness.selection import instruction_reason

    for item in items:
        reason = instruction_reason(item.mnemonic, item.op_str)
        if reason in {"x87", "sse-mmx"}:
            raise Refusal("closure-integer-wave-" + reason)
        if ":" in item.op_str and not _implicit_es_string_operand_only(item):
            raise Refusal("closure-segment")
        for op in item.operands:
            if op.type != 3 or not op.access & 2:
                continue
            if not op.mem.base and not op.mem.index:
                address = op.mem.disp & 0xFFFFFFFF
                if address < text[1] and address + op.size > text[0]:
                    raise Refusal("closure-known-code-write")


def manifest_entry(row: dict[str, Any]) -> ManifestEntry:
    """Legacy absence is None; an explicit empty register input set is invalid."""
    inputs = tuple(row["abi"]["register_inputs"])
    return ManifestEntry(
        int(row["va"], 16),
        row["name"],
        row["abi"]["convention"],
        row["abi"]["stack_args"],
        "eax",
        (),
        row["source"],
        register_inputs=inputs or None,
    )


def caller_eligible(audit: Any) -> bool:
    return bool(
        audit.eligible
        and audit.direct_sites >= 1
        and audit.tail_jumps == 0
        and audit.data_references == 0
        and audit.unsafe == 0
        and audit.unresolved == 0
    )


def rank_key(row: dict[str, Any]) -> tuple[Any, ...]:
    return tuple(
        row[k]
        for k in (
            "meaningful_insns",
            "closure_nodes",
            "closure_insns",
            "call_sites",
            "simplicity",
            "size",
            "va",
        )
    )


def worklists(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    ranked = sorted(rows, key=rank_key)
    if len({r["va"] for r in ranked}) != len(ranked):
        raise ValueError("duplicate eligible VA")
    return [
        {
            "number": n // 25 + 1,
            "partial": len(ranked[n : n + 25]) < 25,
            "count": len(ranked[n : n + 25]),
            "functions": ranked[n : n + 25],
        }
        for n in range(0, len(ranked), 25)
    ]


def validate_worklists(lists: list[dict[str, Any]], *, expected: int) -> None:
    """Recheck exported identities, root byte integrity, ABI and disjoint full coverage."""
    rows = [row for item in lists for row in item["functions"]]
    if len(rows) != expected or len({r["va"] for r in rows}) != expected:
        raise ValueError("worklist count or disjointness mismatch")
    if len({r["source"] for r in rows}) != expected:
        raise ValueError("target filename collision")
    for number, item in enumerate(lists, 1):
        n = len(item["functions"])
        if item["number"] != number or item["count"] != n or not 1 <= n <= 25:
            raise ValueError("invalid worklist count/order")
        if item["partial"] != (n < 25) or (n < 25 and number != len(lists)):
            raise ValueError("partial worklist must be last and explicit")
    for row in rows:
        code = bytes.fromhex(row["bytes"])
        if len(code) != row["size"] or hashlib.sha256(code).hexdigest() != row["sha256"]:
            raise ValueError("root byte integrity mismatch")
        abi = row["abi"]
        if tuple(abi["register_inputs"]) not in SUPPORTED or abi["scratch"]:
            raise ValueError("unsupported exact input ABI")
        if not 0 <= abi["stack_args"] <= 8 or abi["convention"] not in {"cdecl", "stdcall"}:
            raise ValueError("unsupported stack ABI")
        if abi["cleanup_bytes"] != (abi["stack_args"] * 4 if abi["convention"] == "stdcall" else 0):
            raise ValueError("cleanup mismatch")
        if bool(row["call_sites"]) != row["recipe"]["live_call_closure"]:
            raise ValueError("leaf/call closure recipe mismatch")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--private-root", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    os.setpriority(os.PRIO_PROCESS, 0, 19)
    assert os.getpriority(os.PRIO_PROCESS, 0) == 19
    assert os.sched_getaffinity(0) == set(range(24, 32))
    try:
        return run(args.private_root.resolve(), args.out_dir)
    except Exception as error:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "infrastructure-failure.json").write_text(
            json.dumps(
                {
                    "error": type(error).__name__,
                    "detail": str(error),
                    "status": "INCOMPLETE; no eligible worklists authorized",
                },
                indent=2,
            )
            + "\n"
        )
        raise


def run(private: Path, out: Path) -> int:
    if out.exists():
        raise SystemExit("refuse overwrite: use a fresh output directory")
    out.mkdir(parents=True)
    paths = (
        set(Path("tools").rglob("*.py"))
        | set(Path("src/game").glob("*.c"))
        | set(Path("src/game").glob("*.h"))
    )
    paths |= {Path("docs/data/replace-proof-snapshot.json")}
    paths.update((private / "tools/data").glob("*.csv"))
    paths.update((private / "generated/retail").glob("*.csv"))
    paths.update((private / "generated/retail").glob("*manifest*.json"))
    for pattern in pilot.REJECT_GLOBS:
        paths.update(p for p in Path(".").glob(pattern) if p.is_file())
    paths |= {private / "build/default.xbe", private / "generated/retail/functions.csv"}
    paths.update((private / "generated/lifted/gen").glob("*.c"))
    paths.update((private / "generated/lifted/gen").glob("*manifest*.json"))
    pins = {str(p): sha(p) for p in sorted(paths) if p.is_file()}
    (out / "pins-before.json").write_text(json.dumps(pins, indent=2, sort_keys=True) + "\n")
    from tools.name_additions import World

    world = World(private)
    md = draft._capstone()
    functions = draft.load_functions(private / "generated/retail/functions.csv", world.read)
    proven, failed = pilot.snapshot_sets()
    registered = draft.registered_vas(Path("src/game"))
    rejected = pilot.rejected_vas() | failed
    waterfall: Counter[str] = Counter()
    pool: dict[int, str] = {}
    for va in sorted(v for v in world.size if v not in world.library):
        if va in proven:
            reason = "proven"
        elif va in registered:
            reason = "registered"
        elif va in rejected:
            reason = "rejected-before"
        elif va not in functions:
            reason = "decode-mismatch"
        else:
            f = functions[va]
            reason = pilot.classify(f, md, (world.text_lo, world.text_hi))
            if reason is None:
                inp = pilot.register_input(f, md)
                reason = (
                    "implicit-ecx-input"
                    if "ecx" in inp
                    else "register-input-abi"
                    if inp
                    else "later-pilot-stage"
                )
            if reason in {"call", "implicit-ecx-input", "register-input-abi"}:
                pool[va] = reason
        waterfall[reason] += 1
    sizes = {va: f.size for va, f in functions.items()}
    # Mirror CLI's fallback manifest extents for non-game direct callees.
    fallback = private / "generated/retail/manifest.json"
    if fallback.exists():
        for r in json.loads(fallback.read_text()).get("functions", []):
            sizes.setdefault(int(r["address"], 16), int(r["size"]))
    names = {va: f"sub_{va:08X}" for va in sizes}
    recognizer = ABIRecognizer(sizes, world.read)
    dispositions: list[dict[str, Any]] = []
    candidates: list[dict[str, Any]] = []
    summaries: Counter[str] = Counter()
    for va, stratum in pool.items():
        f = functions[va]
        row: dict[str, Any] = {"va": f"0x{va:08x}", "stratum": stratum}
        try:
            meaningful = sum(i.mnemonic not in {"nop", "int3"} for i in f.insns)
            if meaningful < 5:
                raise Refusal("fewer-than-five-meaningful-insns")
            read_nodes: set[int] = set()

            def bounded_read(address: int, count: int, nodes: set[int] = read_nodes) -> bytes:
                nodes.add(address)
                if len(nodes) > 128:
                    raise LiveClosureError("closure-node-bound-128")
                return world.read(address, count)

            closure = discover_live_call_closure(
                va, sizes=sizes, names=names, read_code=bounded_read
            )
            closure_insns = 0
            for node in closure.nodes:
                code = world.read(node.va, node.size)
                items = list(md.disasm(code, node.va))
                closure_capability(items, (world.text_lo, world.text_hi))
                closure_insns += len(items)
            abi = recognizer.infer(va)
            row["abi"] = abi.document()
            if abi.inputs not in SUPPORTED:
                raise Refusal("unsupported-register-input-set:" + ",".join(abi.inputs))
            stem = f"t1534_{va:08x}"
            row.update(
                {
                    "name": names[va],
                    "known_name": world.names.get(va),
                    "provenance": {"pins": "pins-before.json", "status": "static only"},
                    "limitations": [
                        "Not original/native proof or admission",
                        "Indirect pointer writes require future authenticated domains",
                        "Existing fault channel does not certify full faulty continuation",
                    ],
                    "source": stem + ".c",
                    "size": f.size,
                    "bytes": world.read(va, f.size).hex(),
                    "sha256": hashlib.sha256(world.read(va, f.size)).hexdigest(),
                    "meaningful_insns": meaningful,
                    "closure_nodes": len(closure.nodes),
                    "closure_insns": closure_insns,
                    "call_sites": sum(i.mnemonic == "call" for i in f.insns),
                    "simplicity": pilot.simplicity(f),
                    "disassembly": draft.disassembly_text(f),
                    "closure": closure.document(
                        xbe_sha256=pins[str(private / "build/default.xbe")],
                        functions_sha256=pins[str(private / "generated/retail/functions.csv")],
                    ),
                    "recipe": {
                        "live_call_closure": stratum == "call",
                        "live_call_boundaries": [],
                        "fixture_providers": [],
                        "count": 600,
                        "seeds": [20261001, 20261006],
                        "optimizations": [0, 3],
                        "scratch": [],
                        "input_contract": abi.document(),
                    },
                }
            )
            row["status"] = "pending-caller-audit"
            candidates.append(row)
        except (LiveClosureError, Refusal) as error:
            row["status"] = "refused"
            row["reason"] = str(error)
            summaries[row["reason"].split(":")[0]] += 1
        dispositions.append(row)
    (out / "pre-audit-dispositions.json").write_text(
        json.dumps(dispositions, indent=2, sort_keys=True) + "\n"
    )
    print(f"static union {len(pool)}, prospective audits {len(candidates)}", flush=True)
    entries = [manifest_entry(row) for row in candidates]
    image = build_guest_image(private / "build/default.xbe")
    audits = audit_all(
        entries,
        private / "generated/lifted/gen",
        image,
        data_ranges=[(lo, hi) for _, lo, hi in data_ranges(private / "build/default.xbe")],
    )
    eligible = []
    for row, audit in zip(candidates, audits, strict=True):
        row["audit"] = asdict(audit)
        row["audit_contract"] = manifest_entry(row).as_json()
        if caller_eligible(audit):
            row["status"] = "static-draft-eligible-NOT-proof"
            eligible.append(row)
        else:
            row["status"] = "refused"
            row["reason"] = "caller-audit-gates"
            summaries["caller-audit-gates"] += 1
    after = {p: sha(Path(p)) for p in pins}
    if after != pins:
        raise RuntimeError("frozen inputs changed; no eligible worklists published")
    lists = worklists(eligible)
    validate_worklists(lists, expected=len(eligible))
    report = {
        "version": 1,
        "historical_counts": {"call": 2757, "implicit-ecx-input": 129, "register-input-abi": 99},
        "actual_pilot_waterfall": dict(sorted(waterfall.items())),
        "unique_union": len(pool),
        "draft_exclusions": dict(sorted(summaries.items())),
        "unique_eligible": len(eligible),
        "list_count": len(lists),
        "static_only": True,
        "dispositions": dispositions,
    }
    assert len(pool) == sum(summaries.values()) + len(eligible)
    for item in lists:
        item["provenance"] = {
            "pins": "pins-before.json",
            "report": "report.json",
            "status": "static draft eligibility only; no proof/admission",
        }
        (out / f"list-{item['number']:03d}.json").write_text(
            json.dumps(item, indent=2, sort_keys=True) + "\n"
        )
    (out / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    (out / "pins-after.json").write_text(json.dumps(after, indent=2, sort_keys=True) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "dispositions"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
