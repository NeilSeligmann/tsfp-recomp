# SPDX-License-Identifier: GPL-3.0-or-later
"""T1576 batch C: guarded sweep v2 producers (dispatch index source, tail condition source).

Opt-in only (`--guarded-sweep-version 2`). Everything here generates TEST INPUTS from the
ORIGINAL's decoded instructions. A derived input is never evidence: admission still needs the
original's own arm/default/tail events on AGREE cases. A derivation that cannot be proven returns a
`Refusal` with a reason and the caller falls back to the legacy sweep, so a mis-derivation can
only waste cases. Rules and soundness arguments: docs/evidence/t1576/batch-c-predeclared.md.

`checks` names the refusals that are active. The default is all of them. Tests switch ONE off to
prove that a negative control kills the mutant (see tests/test_t1576_dispatch_source.py).
"""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass

import capstone
from capstone import x86

from .model import Case
from .seeding import SCRATCH_WORDS

MASK = 0xFFFFFFFF
#: The data window the generator points pointer arguments into (seeding.make_case).
SCRATCH_BYTES = SCRATCH_WORDS * 4 + 0x100
CHECKS = frozenset(
    {
        "branch-target",
        "unreachable-fallthrough",
        "return-slot",
        "text",
        "non-affine",
        "call-clobber",
        "esp-writer",
        "pointer-load",
        "base-offset",
    }
)
DEFAULT_CHECKS = CHECKS | {"adjacent-flags", "both-sides"}
#: Sweep version 3 additionally derives a load through an argument pointer (R1b).
V3_CHECKS = DEFAULT_CHECKS | {"deref"}
#: How far (instructions) the tail condition may sit before the out-of-body jmp.
TAIL_WINDOW = 6
#: Highest argument byte offset above the entry ESP accepted as an argument slot.
MAX_ARG_OFFSET = 4 + 4 * 32
CANDIDATE_OFFSETS = (-1, 0, 1)
CANDIDATE_CONSTANTS = (0, MASK, 0x80000000, 0x7FFFFFFF)

_FULL = {
    "al": "eax", "ah": "eax", "ax": "eax", "eax": "eax",
    "bl": "ebx", "bh": "ebx", "bx": "ebx", "ebx": "ebx",
    "cl": "ecx", "ch": "ecx", "cx": "ecx", "ecx": "ecx",
    "dl": "edx", "dh": "edx", "dx": "edx", "edx": "edx",
    "si": "esi", "esi": "esi", "di": "edi", "edi": "edi",
    "bp": "ebp", "ebp": "ebp", "sp": "esp", "esp": "esp",
}  # fmt: skip
_CALLER_SAVED = frozenset({"eax", "ecx", "edx"})
_CONDITIONAL = {
    "je", "jne", "ja", "jae", "jb", "jbe", "jg", "jge", "jl", "jle", "js", "jns",
}  # fmt: skip


@dataclass(frozen=True)
class Refusal:
    reason: str
    detail: str = ""

    def document(self) -> dict[str, str]:
        return {"refused": self.reason, "detail": self.detail}


@dataclass(frozen=True)
class Source:
    """Where the compared value lives: `kind` abs (guest address) or arg (byte offset above the
    entry ESP, so the patch address is case.esp + offset). `offset`: compared = raw + offset."""

    kind: str
    location: int
    width: int
    offset: int
    disp: int = 0  # deref only: the load is [argument pointer + disp]

    def raw_for(self, compared: int) -> int | None:
        raw = (compared - self.offset) & MASK
        return raw if raw < (1 << (8 * self.width)) else None

    def address(self, esp: int) -> int:
        return self.location if self.kind == "abs" else esp + self.location

    def resolve(self, case: Case, *, window: bool = True) -> int | None:
        """The guest address to patch for `case`, or None (deref pointer outside the data window).

        A deref source reads the pointer from the seed's entry frame and accepts it only when the
        whole load lies inside the seed scratch window (`patches[1]` base + SCRATCH_BYTES), never
        text or a table."""
        if self.kind != "deref":
            return self.address(case.esp)
        _esp, frame = case.patches[0]
        if self.location + 4 > len(frame):
            return None
        pointer = int.from_bytes(frame[self.location : self.location + 4], "little")
        address = (pointer + self.disp) & MASK
        low = case.patches[1][0]
        if window and not (low <= address and address + self.width <= low + SCRATCH_BYTES):
            return None
        return address

    def document(self) -> dict[str, object]:
        document: dict[str, object] = {
            "kind": self.kind,
            "location": f"0x{self.location:x}",
            "width": self.width,
            "offset": self.offset,
        }
        if self.kind == "deref":
            document["disp"] = self.disp
        return document


def _decoder() -> capstone.Cs:
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    return decoder


def decode(code: bytes, va: int) -> list[capstone.CsInsn]:
    return list(_decoder().disasm(code, va))


def _written(decoder: capstone.Cs, insn: capstone.CsInsn) -> set[str]:
    _read, written = insn.regs_access()
    return {_FULL[name] for name in (decoder.reg_name(r) for r in written) if name in _FULL}


def branch_targets(insns: Sequence[capstone.CsInsn]) -> set[int]:
    found: set[int] = set()
    for insn in insns:
        if (
            insn.mnemonic == "jmp" or insn.mnemonic in _CONDITIONAL or insn.mnemonic.startswith("j")
        ) and insn.operands[0].type == x86.X86_OP_IMM:
            found.add(insn.operands[0].imm)
    return found


def _esp_delta(
    decoder: capstone.Cs,
    insns: Sequence[capstone.CsInsn],
    upto: int,
    targets: set[int],
    checks: frozenset[str],
) -> int | Refusal:
    """Net push/pop/sub/add bytes of insns[:upto]. Refuses any other ESP writer, a CALL while
    the delta is not zero (callee-pops is not known), and any control flow once ESP moved."""
    delta = 0
    moved = False
    branched = False
    for insn in insns[:upto]:
        if insn.address in targets or insn.mnemonic.startswith("j"):
            branched = True
        if insn.mnemonic == "push":
            delta += 4
            moved = True
        elif insn.mnemonic == "pop":
            delta -= 4
            moved = True
        elif (
            insn.mnemonic in ("sub", "add")
            and insn.operands[0].type == x86.X86_OP_REG
            and decoder.reg_name(insn.operands[0].reg) == "esp"
            and insn.operands[1].type == x86.X86_OP_IMM
        ):
            delta += insn.operands[1].imm if insn.mnemonic == "sub" else -insn.operands[1].imm
            moved = True
        elif insn.mnemonic == "call":
            if delta != 0:
                return Refusal("esp-writer", f"call with ESP delta {delta} at {insn.address:#x}")
        elif "esp" in _written(decoder, insn) and "esp-writer" in checks:
            return Refusal("esp-writer", f"{insn.mnemonic} writes ESP at {insn.address:#x}")
    if moved and branched and "esp-writer" in checks:
        return Refusal("esp-writer", "ESP moved in a prefix with control flow")
    return delta


def _memory_source(
    decoder: capstone.Cs,
    insns: Sequence[capstone.CsInsn],
    ordinal: int,
    operand: x86.X86Op,
    width: int,
    offset: int,
    ranges: Sequence[tuple[int, int]],
    targets: set[int],
    checks: frozenset[str],
) -> Source | Refusal:
    memory = operand.mem
    if memory.segment != 0 or memory.index != 0:
        return Refusal("pointer-load", "segment or index register in the source operand")
    base = decoder.reg_name(memory.base) if memory.base else ""
    if base == "":
        address = memory.disp & MASK
        if "text" in checks and any(low <= address < high for low, high in ranges):
            return Refusal("text", f"source {address:#x} is inside text or a guarded region")
        return Source("abs", address, width, offset)
    if base == "esp":
        delta = _esp_delta(decoder, insns, ordinal, targets, checks)
        if isinstance(delta, Refusal):
            return delta
        slot = memory.disp - delta
        if "return-slot" in checks and not 4 <= slot < MAX_ARG_OFFSET:
            return Refusal("return-slot", f"esp-relative slot +{slot} is not an argument")
        return Source("arg", slot, width, offset)
    if "deref" in checks:
        pointer = derive_source(insns, ordinal, base, ranges=ranges, checks=checks)
        if (
            isinstance(pointer, Source)
            and pointer.kind == "arg"
            and pointer.width == 4
            and (pointer.offset == 0 or "base-offset" not in checks)
        ):
            return Source("deref", pointer.location, width, offset, memory.disp)
        why = pointer.reason if isinstance(pointer, Refusal) else "not an argument slot"
        return Refusal("pointer-load", f"load through {base}: {why}")
    if "pointer-load" in checks:
        return Refusal("pointer-load", f"load through {base}")
    # Mutant path only (the check is switched off): treat the displacement as an absolute address.
    return Source("abs", memory.disp & MASK, width, offset)


def derive_source(
    insns: Sequence[capstone.CsInsn],
    ordinal: int,
    register: str,
    *,
    ranges: Sequence[tuple[int, int]],
    checks: frozenset[str] = CHECKS,
) -> Source | Refusal:
    """The source of `register` as read by insns[ordinal] (the CMP), walking backward."""
    decoder = _decoder()
    targets = branch_targets(insns)
    tracked = register
    offset = 0
    for position in range(ordinal - 1, -2, -1):
        later = insns[position + 1]
        if "branch-target" in checks and later.address in targets:
            return Refusal("branch-target", f"{later.address:#x} is a branch target")
        if position < 0:
            return Refusal("entry-register", f"{tracked} is not defined in the body")
        insn = insns[position]
        if "unreachable-fallthrough" in checks and (
            insn.mnemonic == "jmp" or insn.mnemonic.startswith("ret")
        ):
            return Refusal("unreachable-fallthrough", f"{insn.address:#x} ends the fall-through")
        if insn.mnemonic == "call":
            if tracked in _CALLER_SAVED and "call-clobber" in checks:
                kind = "callee-return-source" if tracked == "eax" else "call-clobber"
                return Refusal(kind, f"{tracked} defined by the call at {insn.address:#x}")
            continue
        if tracked not in _written(decoder, insn):
            continue
        operands = insn.operands
        dest_ok = (
            len(operands) >= 1
            and operands[0].type == x86.X86_OP_REG
            and decoder.reg_name(operands[0].reg) == tracked
        )
        if dest_ok and insn.mnemonic in ("add", "sub") and operands[1].type == x86.X86_OP_IMM:
            offset += operands[1].imm if insn.mnemonic == "add" else -operands[1].imm
            continue
        if dest_ok and insn.mnemonic in ("inc", "dec"):
            offset += 1 if insn.mnemonic == "inc" else -1
            continue
        if dest_ok and insn.mnemonic == "lea" and operands[1].type == x86.X86_OP_MEM:
            memory = operands[1].mem
            if memory.index == 0 and memory.segment == 0 and memory.base:
                base = decoder.reg_name(memory.base)
                if base in _FULL and _FULL[base] == base and base != "esp":
                    offset += memory.disp
                    tracked = base
                    continue
        if dest_ok and insn.mnemonic == "mov" and operands[1].type == x86.X86_OP_REG:
            name = decoder.reg_name(operands[1].reg)
            if operands[1].size == 4 and name in _FULL:
                tracked = name
                continue
        if (
            dest_ok
            and operands[0].size == 4
            and operands[1].type == x86.X86_OP_MEM
            and insn.mnemonic in ("mov", "movzx")
        ):
            width = operands[1].size if insn.mnemonic == "movzx" else 4
            if width in (1, 2, 4):
                return _memory_source(
                    decoder, insns, position, operands[1], width, offset, ranges, targets, checks
                )
        if "non-affine" in checks:
            return Refusal("non-affine", f"{insn.mnemonic} writes {tracked} at {insn.address:#x}")
        continue  # mutant path only (the check is switched off): the writer is ignored
    return Refusal("entry-register", "unreachable")


def dispatch_comparison(
    insns: Sequence[capstone.CsInsn], guard_site: int
) -> tuple[int, str] | Refusal:
    """(ordinal of the CMP, its register) for the JA at `guard_site` (movaps fillers skipped)."""
    decoder = _decoder()
    ordinals = {insn.address: n for n, insn in enumerate(insns)}
    cursor = ordinals.get(guard_site)
    if cursor is None:
        return Refusal("no-guard", f"{guard_site:#x} is not a decoded instruction")
    cursor -= 1
    while cursor >= 0 and insns[cursor].mnemonic == "movaps":
        cursor -= 1
    compare = insns[cursor] if cursor >= 0 else None
    if (
        compare is None
        or compare.mnemonic != "cmp"
        or compare.operands[0].type != x86.X86_OP_REG
        or compare.operands[1].type != x86.X86_OP_IMM
    ):
        return Refusal("no-cmp", "no CMP r32,imm in front of the JA")
    return cursor, decoder.reg_name(compare.operands[0].reg)


def index_values(source: Source, bound: int) -> list[tuple[int, int]]:
    """(table index, raw loaded value) for 0..bound+2 and the unsigned-huge index."""
    pairs = []
    for index in (*range(bound + 3), MASK):
        raw = source.raw_for(index)
        if raw is not None:
            pairs.append((index, raw))
    return pairs


def _taken(mnemonic: str, left: int, right: int, kind: str) -> bool:
    result = ((left - right) if kind == "cmp" else (left & right)) & MASK
    zero = result == 0
    sign = bool(result >> 31)
    carry = kind == "cmp" and left < right
    overflow = kind == "cmp" and bool(((left ^ right) & (left ^ result)) >> 31)
    return {
        "je": zero,
        "jne": not zero,
        "ja": not carry and not zero,
        "jae": not carry,
        "jb": carry,
        "jbe": carry or zero,
        "jg": not zero and sign == overflow,
        "jge": sign == overflow,
        "jl": sign != overflow,
        "jle": zero or sign != overflow,
        "js": sign,
        "jns": not sign,
    }[mnemonic]


def tail_condition_plans(
    insns: Sequence[capstone.CsInsn],
    body_va: int,
    body_size: int,
    *,
    ranges: Sequence[tuple[int, int]],
    checks: frozenset[str] = DEFAULT_CHECKS,
    invert: bool = False,
) -> list[dict[str, object]]:
    """One plan per out-of-body direct JMP: the condition source and a TAKEN and a NOT-TAKEN
    compared value, or a refusal. `invert` is a test hook for the flag-model mutant."""
    decoder = _decoder()
    plans: list[dict[str, object]] = []
    for ordinal, insn in enumerate(insns):
        if (
            insn.mnemonic != "jmp"
            or insn.operands[0].type != x86.X86_OP_IMM
            or body_va <= insn.operands[0].imm < body_va + body_size
        ):
            continue
        plan: dict[str, object] = {"tail_site": f"0x{insn.address:08x}"}
        plans.append(plan)
        jcc = next(
            (
                n
                for n in range(ordinal - 1, max(-1, ordinal - 1 - TAIL_WINDOW), -1)
                if insns[n].mnemonic in _CONDITIONAL
            ),
            None,
        )
        if jcc is None or jcc == 0:
            plan.update(Refusal("no-condition", "no jcc before the tail jmp").document())
            continue
        compare = insns[jcc - 1]
        if "adjacent-flags" in checks and compare.mnemonic not in ("cmp", "test"):
            plan.update(Refusal("adjacent-flags", compare.mnemonic).document())
            continue
        if (
            len(compare.operands) != 2
            or compare.operands[1].type not in (x86.X86_OP_IMM, x86.X86_OP_REG)
            or compare.operands[0].size != 4
        ):
            plan.update(Refusal("operand-form", "needs cmp/test x, imm or test r, r").document())
            continue
        immediate = (
            compare.operands[1].imm & MASK if compare.operands[1].type == x86.X86_OP_IMM else None
        )
        first = compare.operands[0]
        if first.type == x86.X86_OP_REG:
            name = decoder.reg_name(first.reg)
            if immediate is None and decoder.reg_name(compare.operands[1].reg) != name:
                plan.update(Refusal("operand-form", "two different registers").document())
                continue
            source = derive_source(insns, jcc - 1, name, ranges=ranges, checks=checks)
        elif first.type == x86.X86_OP_MEM and immediate is not None:
            targets = branch_targets(insns)
            source = _memory_source(decoder, insns, jcc - 1, first, 4, 0, ranges, targets, checks)
        else:
            plan.update(Refusal("operand-form", "unsupported operand").document())
            continue
        if isinstance(source, Refusal):
            plan.update(source.document())
            continue
        right = immediate if immediate is not None else None
        kind = compare.mnemonic
        candidates = {
            (right + delta) & MASK for delta in CANDIDATE_OFFSETS if right is not None
        } | set(CANDIDATE_CONSTANTS)
        if right is None:  # test r, r
            candidates = set(CANDIDATE_CONSTANTS)
        true_values: list[int] = []
        false_values: list[int] = []
        for value in sorted(candidates):
            raw = source.raw_for(value)
            if raw is None:
                continue
            outcome = _taken(
                insns[jcc].mnemonic, value, right if right is not None else value, kind
            )
            (true_values if outcome != invert else false_values).append(raw)
        plan["source"] = source.document()
        plan["jcc"] = f"0x{insns[jcc].address:08x} {insns[jcc].mnemonic}"
        plan["true_values"] = true_values
        plan["false_values"] = false_values
        if "both-sides" in checks and not (true_values and false_values):
            plan.update(Refusal("tail-cond-undrivable", "no taken+not-taken pair").document())
            continue
        plan["raw_values"] = sorted(set(true_values) | set(false_values))
        plan["_source"] = source
    return plans


def public_plan(plans: Iterable[dict[str, object]]) -> list[dict[str, object]]:
    return [{k: v for k, v in plan.items() if not k.startswith("_")} for plan in plans]


#: R5 (sweep v3): derived seeds from AGREE feedback variants, depth 2, at most this many.
MAX_DERIVED_SEEDS = 8
DERIVED_LIMIT_FACTOR = 2


def derived_seed_locations(
    parent_loads: Sequence[tuple[int, int, int]],
    variant_loads: Sequence[tuple[int, int, int]],
    esp: int,
    text: tuple[int, int],
    frame_bytes: int,
    *,
    faulted: bool,
) -> list[int]:
    """New 4-byte data locations a feedback variant loaded (empty when either side faulted).

    A location is new when the parent seed did not load it, it is not in the entry stack frame and
    not in text. Only an AGREE variant may seed further feedback (R5)."""
    if faulted:
        return []
    known = {address for address, _size, _value in parent_loads}
    found: list[int] = []
    for address, size, _value in variant_loads:
        if (
            size == 4
            and address not in known
            and address not in found
            and not esp <= address < esp + frame_bytes
            and not text[0] <= address < text[1]
        ):
            found.append(address)
    return found
