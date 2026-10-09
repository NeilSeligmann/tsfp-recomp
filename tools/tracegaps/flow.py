# SPDX-License-Identifier: GPL-3.0-or-later
"""A per-function control-flow graph, a stack model, and a backward value-set resolver.

THE QUESTION. "What can this register or stack slot hold at this instruction?" answered
as a FINITE SET of integers, or as a set plus the exact reasons it is not finite. An
unbounded part is never dropped and never guessed: each reason names the instruction or
the incoming argument that makes it unbounded.

HOW. The function body is the forward-reachable set from its entry (fall-through,
conditional and unconditional jumps, switch tables, the instruction after a call). A
`jmp` to another function's entry is a tail call and is not followed. For one query the
resolver walks BACKWARD over every predecessor path to the instructions that define the
location (reaching definitions, one visit per node and constraint set), then evaluates
each definition:

    `mov r, imm` and `xor r, r`      the literal
    `mov r, r2` and `mov r, [esp+k]` the value of r2 (or of that frame slot) just before
    `add/sub/and/or/shl/shr r, imm`  the inner set mapped through the operation
    anything else                    unbounded, with the mnemonic and address as the reason

PATH CONDITIONS. When a definition is reached over the edge of a `je`/`jne` whose flags
come from `test r, r` or `cmp r, imm` on the same register, the value on that edge is
restricted to the literal (equal edge) or loses it (not-equal edge). That is what turns
`mov eax, [ebp+0x14]; test eax, eax; jne X; push eax` into a pushed zero.

THE STACK MODEL. Frame slots are named by their offset from the ENTRY stack pointer
(arguments at +4, +8, ..., locals negative). The delta of every instruction from the entry
`esp` is propagated forward: push and pop, `add/sub esp, imm`, and a call's callee-pop
(`ret N` of a direct callee, 0 for cdecl). A call through a register or a memory slot, a
`leave`, or any other write to `esp` LOSES the delta, and everything downstream is then
refused rather than guessed. Two different deltas reaching one instruction are reported as
a conflict and the analysis is not trusted until there are none.

WHAT IT CANNOT SEE (reported, never ignored).
  * A store through a pointer that aliases a slot. Taking a slot's address (`lea r, [esp+k]`
    resolving to the slot) is detected and added to that slot's result as a reason.
  * A switch whose table or bound it cannot read: listed in `unresolved_indirect`, and the
    code only reachable through it is not in the graph.
  * Loop-carried arithmetic: a definition that depends on itself is unbounded.
"""

from __future__ import annotations

import re
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, field

import capstone
from capstone import x86 as cs_x86

from tools.tracegaps.code import Code

#: A set larger than this is reported unbounded (the operations here never need more).
MAX_VALUES = 4096
MASK32 = 0xFFFFFFFF
FAMILIES = {
    "eax": ("eax", "ax", "al", "ah"),
    "ebx": ("ebx", "bx", "bl", "bh"),
    "ecx": ("ecx", "cx", "cl", "ch"),
    "edx": ("edx", "dx", "dl", "dh"),
    "esi": ("esi", "si"),
    "edi": ("edi", "di"),
    "ebp": ("ebp", "bp"),
    "esp": ("esp", "sp"),
}
FAMILY_OF = {alias: full for full, names in FAMILIES.items() for alias in names}
VOLATILE = frozenset({"eax", "ecx", "edx"})
#: Instructions that end a path with no successor.
_DEAD_END = frozenset({"ret", "retn", "retf", "int3", "hlt", "ud2"})


@dataclass(frozen=True)
class ValueSet:
    """A finite set of values plus the reasons the true set is larger (empty when exact)."""

    values: frozenset[int] = frozenset()
    unbounded: tuple[str, ...] = ()

    @property
    def bounded(self) -> bool:
        return not self.unbounded

    def union(self, other: ValueSet) -> ValueSet:
        reasons = tuple(dict.fromkeys(self.unbounded + other.unbounded))
        merged = self.values | other.values
        if len(merged) > MAX_VALUES:
            return ValueSet(frozenset(), (*reasons, f"more than {MAX_VALUES} values"))
        return ValueSet(merged, reasons)

    def map(self, function: Callable[[int], int]) -> ValueSet:
        return ValueSet(frozenset(function(v) & MASK32 for v in self.values), self.unbounded)

    def only(self, value: int) -> ValueSet:
        """Restrict to `value` (the equal edge). Empty when the set provably excludes it."""
        if value in self.values or self.unbounded:
            return ValueSet(frozenset({value}))
        return ValueSet()

    def without(self, value: int) -> ValueSet:
        return ValueSet(self.values - {value}, self.unbounded)

    def describe(self) -> str:
        parts = [", ".join(f"{v:#x}" for v in sorted(self.values))] if self.values else []
        for reason in self.unbounded:
            parts.append(f"UNBOUNDED({reason})")
        return "{" + "; ".join(parts) + "}" if parts else "{}"


def exact(*values: int) -> ValueSet:
    return ValueSet(frozenset(v & MASK32 for v in values))


def open_set(reason: str) -> ValueSet:
    return ValueSet(frozenset(), (reason,))


_ARGUMENT_REASON = re.compile(r"incoming argument (\d+) of (0x[0-9a-f]+)")


def substitute_arguments(
    value: ValueSet, entry: int, arguments: Mapping[int, ValueSet]
) -> ValueSet:
    """Replace "incoming argument N of `entry`" in a result by what a caller passes for N.

    The resolver stops at a function boundary and names the argument; this is the step
    across it. Reasons about other functions or other causes are kept as they were.
    """
    result = ValueSet(value.values)
    for reason in value.unbounded:
        match = _ARGUMENT_REASON.fullmatch(reason)
        if match and int(match.group(2), 16) == entry and int(match.group(1)) in arguments:
            result = result.union(arguments[int(match.group(1))])
        else:
            result = result.union(ValueSet(frozenset(), (reason,)))
    return result


@dataclass(frozen=True)
class Edge:
    source: int
    #: "fall", "taken", "jump" or "table"
    kind: str


@dataclass(frozen=True)
class Definition:
    """One reaching definition of a location, with the constraints on the path to it."""

    address: int
    #: "insn", "call" or "entry"
    kind: str
    constraints: tuple[tuple[str, int], ...]
    value: ValueSet


@dataclass
class Flow:
    """The graph, the stack model and the resolver for one function."""

    code: Code
    entry: int
    max_insns: int = 20000
    insns: dict[int, capstone.CsInsn] = field(default_factory=dict)
    preds: dict[int, list[Edge]] = field(default_factory=dict)
    unresolved_indirect: list[int] = field(default_factory=list)
    tail_calls: list[tuple[int, int]] = field(default_factory=list)
    delta: dict[int, int | None] = field(default_factory=dict)
    delta_conflicts: list[tuple[int, int, int]] = field(default_factory=list)
    delta_lost: list[tuple[int, str]] = field(default_factory=list)
    returns: list[int] = field(default_factory=list)

    def __post_init__(self) -> None:
        self._successors: dict[int, list[tuple[int, str]]] = {}
        self._cycle_hits = 0
        self._memo: dict[tuple[str, int, str | int], ValueSet] = {}
        self._active: set[tuple[str, int, str | int]] = set()
        self._build()
        self._propagate_delta()

    # ------------------------------------------------------------------ graph

    def _build(self) -> None:
        targets = self.code.call_targets()
        pending = [self.entry]
        seen: set[int] = set()
        while pending:
            address = pending.pop()
            if address in seen:
                continue
            insn = self.code.insn_at(address)
            if insn is None:
                continue
            seen.add(address)
            if len(seen) > self.max_insns:
                raise RuntimeError(
                    f"function at {self.entry:#x} exceeds {self.max_insns} instructions"
                )
            self.insns[address] = insn
            edges = self._edges(insn, targets)
            self._successors[address] = edges
            for target, _ in edges:
                pending.append(target)
        self.preds = {address: [] for address in self.insns}
        for address, edges in self._successors.items():
            for target, kind in edges:
                if target in self.preds:
                    self.preds[target].append(Edge(address, kind))

    def _edges(self, insn: capstone.CsInsn, call_targets: frozenset[int]) -> list[tuple[int, str]]:
        """Successor edges as `(address, kind)`."""
        mnemonic = insn.mnemonic
        following = insn.address + insn.size
        if mnemonic in _DEAD_END or capstone.CS_GRP_RET in insn.groups:
            self.returns.append(insn.address)
            return []
        relative = capstone.CS_GRP_BRANCH_RELATIVE in insn.groups
        ops = insn.operands
        if mnemonic == "jmp":
            if relative and len(ops) == 1 and ops[0].type == cs_x86.X86_OP_IMM:
                target = ops[0].imm & MASK32
                if target in call_targets and target != self.entry:
                    self.tail_calls.append((insn.address, target))
                    return []
                return [(target, "jump")]
            table = self._table_targets(insn)
            if table is None:
                self.unresolved_indirect.append(insn.address)
                return []
            return [(target, "table") for target in table]
        if capstone.CS_GRP_JUMP in insn.groups and relative:
            target = ops[0].imm & MASK32
            return [(following, "fall"), (target, "taken")]
        return [(following, "fall")]

    def successors(self, address: int) -> list[tuple[int, str]]:
        return list(self._successors.get(address, []))

    # ------------------------------------------------------------------ switch tables

    def _table_targets(self, insn: capstone.CsInsn) -> list[int] | None:
        """Targets of `jmp [R*4 + table]`, sized by the compiler's own range check."""
        ops = insn.operands
        if len(ops) != 1 or ops[0].type != cs_x86.X86_OP_MEM:
            return None
        mem = ops[0].mem
        if mem.base != 0 or mem.index == 0 or mem.scale != 4:
            return None
        index_reg = FAMILY_OF.get(insn.reg_name(mem.index), "")
        table = mem.disp & MASK32
        window = self.code.window(insn.address, 14, 0)
        count: int | None = None
        byte_table: tuple[int, int] | None = None
        for position in range(len(window) - 1, -1, -1):
            candidate = window[position]
            if self.writes_family(candidate, index_reg):
                if (
                    candidate.mnemonic == "movzx"
                    and candidate.operands[1].type == cs_x86.X86_OP_MEM
                    and candidate.operands[1].size == 1
                    and candidate.operands[1].mem.base != 0
                    and candidate.operands[1].mem.index == 0
                ):
                    byte_table = (
                        candidate.operands[1].mem.disp & MASK32,
                        FAMILY_OF.get(candidate.reg_name(candidate.operands[1].mem.base), ""),
                    )
                    index_reg = byte_table[1]
                    continue
                if candidate.mnemonic in ("lea", "dec", "sub", "mov"):
                    continue
                return None
            bound = self._cmp_bound(candidate, index_reg, window[position + 1 :])
            if bound is not None:
                count = bound
                break
        if count is None:
            return None
        image = self.code.image
        if byte_table is not None:
            raw = [image.read(byte_table[0] + n, 1) for n in range(count)]
            if any(item is None for item in raw):
                return None
            count = max(item[0] for item in raw if item is not None) + 1
        targets: list[int] = []
        for slot in range(count):
            value = image.u32(table + 4 * slot)
            if value is None or self.code.insn_at(value) is None:
                return None
            targets.append(value)
        return targets

    @staticmethod
    def _cmp_bound(
        candidate: capstone.CsInsn, register: str, after: list[capstone.CsInsn]
    ) -> int | None:
        if candidate.mnemonic != "cmp" or len(candidate.operands) != 2:
            return None
        left, right = candidate.operands
        if left.type != cs_x86.X86_OP_REG or right.type != cs_x86.X86_OP_IMM:
            return None
        if FAMILY_OF.get(candidate.reg_name(left.reg), "") != register:
            return None
        for follower in after:
            if follower.mnemonic == "ja":
                return (right.imm & MASK32) + 1
            if follower.mnemonic == "jae":
                return right.imm & MASK32
        return None

    # ------------------------------------------------------------------ stack model

    def _callee_pop(self, target: int) -> int:
        """Bytes a direct callee pops: its first `ret N`, else 0 (cdecl)."""
        for _ in range(3):
            first = self.code.insn_at(target)
            if first is not None and first.mnemonic == "jmp" and first.operands[0].type == 2:
                target = first.operands[0].imm & MASK32
            else:
                break
        for count, insn in enumerate(self.code.iter_range(target, target + 0x4000)):
            if count > 1500:
                break
            if capstone.CS_GRP_RET in insn.groups:
                return insn.operands[0].imm if insn.operands else 0
        return 0

    def _effect(self, insn: capstone.CsInsn) -> int | None:
        """Change to `esp` from executing `insn`, or None when it cannot be modelled."""
        mnemonic = insn.mnemonic
        ops = insn.operands
        if mnemonic == "push":
            return -4
        if mnemonic == "pop":
            return (
                4
                if not (
                    ops and ops[0].type == cs_x86.X86_OP_REG and ops[0].reg == cs_x86.X86_REG_ESP
                )
                else None
            )
        if mnemonic in ("pushfd", "pushf"):
            return -4
        if mnemonic in ("popfd", "popf"):
            return 4
        if mnemonic == "pushal" or mnemonic == "pusha":
            return -32
        if mnemonic == "popal" or mnemonic == "popa":
            return 32
        if mnemonic == "call":
            if capstone.CS_GRP_BRANCH_RELATIVE in insn.groups and ops[0].type == cs_x86.X86_OP_IMM:
                return self._callee_pop(ops[0].imm & MASK32)
            return None
        if (
            mnemonic in ("add", "sub")
            and ops[0].type == cs_x86.X86_OP_REG
            and ops[0].reg == cs_x86.X86_REG_ESP
        ):
            if ops[1].type != cs_x86.X86_OP_IMM:
                return None
            return ops[1].imm if mnemonic == "add" else -ops[1].imm
        if self.writes_family(insn, "esp"):
            return None
        return 0

    def _propagate_delta(self) -> None:
        self.delta = {self.entry: 0}
        pending = [self.entry]
        while pending:
            address = pending.pop()
            before = self.delta[address]
            insn = self.insns[address]
            if before is None:
                after: int | None = None
            elif capstone.CS_GRP_RET in insn.groups:
                after = None
            else:
                effect = self._effect(insn)
                after = None if effect is None else before + effect
                if after is None and not self._leads_to_return(address):
                    self.delta_lost.append((address, f"{insn.mnemonic} {insn.op_str}"))
            for target, _ in self._successors[address]:
                if target not in self.delta:
                    self.delta[target] = after
                    pending.append(target)
                elif (
                    self.delta[target] != after
                    and after is not None
                    and self.delta[target] is not None
                ):
                    existing = self.delta[target]
                    assert existing is not None
                    self.delta_conflicts.append((target, existing, after))

    def _leads_to_return(self, address: int) -> bool:
        """True when every successor is a return, so a lost delta cannot matter."""
        edges = self._successors[address]
        return bool(edges) and all(
            capstone.CS_GRP_RET in self.insns[target].groups for target, _ in edges
        )

    def frame_offset(self, address: int, operand: cs_x86.X86Op) -> int | None:
        """Frame offset (from entry `esp`) of an `[esp+k]` operand at `address`, else None."""
        if operand.type != cs_x86.X86_OP_MEM or operand.mem.index != 0:
            return None
        insn = self.insns[address]
        if insn.reg_name(operand.mem.base) != "esp":
            return None
        delta = self.delta.get(address)
        return None if delta is None else delta + operand.mem.disp

    # ------------------------------------------------------------------ helpers

    @staticmethod
    def writes_family(insn: capstone.CsInsn, family: str) -> bool:
        _, written = insn.regs_access()
        return any(FAMILY_OF.get(insn.reg_name(reg), "") == family for reg in written)

    def _flag_setter(self, jcc: capstone.CsInsn) -> capstone.CsInsn | None:
        """The nearest earlier instruction (in memory order) that writes EFLAGS."""
        previous = self.code.previous_insn(jcc)
        for _ in range(8):
            if previous is None:
                return None
            _, written = previous.regs_access()
            if cs_x86.X86_REG_EFLAGS in written:
                return previous
            if capstone.CS_GRP_JUMP in previous.groups or capstone.CS_GRP_CALL in previous.groups:
                return None
            previous = self.code.previous_insn(previous)
        return None

    def _edge_constraint(
        self, edge: Edge, kind: str, location: str | int
    ) -> tuple[str, int] | None:
        """`("eq"|"ne", imm)` that `edge` implies for `location`, from an adjacent test/cmp."""
        jcc = self.insns[edge.source]
        if jcc.mnemonic not in ("je", "jne") or capstone.CS_GRP_JUMP not in jcc.groups:
            return None
        setter = self._flag_setter(jcc)
        if setter is None or len(setter.operands) != 2:
            return None
        first, second = setter.operands
        constant: int
        if (
            setter.mnemonic == "test"
            and first.type == second.type == cs_x86.X86_OP_REG
            and first.reg == second.reg
        ):
            constant = 0
        elif setter.mnemonic == "cmp" and second.type == cs_x86.X86_OP_IMM:
            constant = second.imm & MASK32
        else:
            return None
        if kind == "reg":
            if (
                first.type != cs_x86.X86_OP_REG
                or FAMILY_OF.get(setter.reg_name(first.reg), "") != location
            ):
                return None
            if first.size != 4:
                return None
        else:
            if self.frame_offset(setter.address, first) != location:
                return None
        # Nothing between the flag setter and the jcc may have rewritten the location.
        between = self.code.next_insn(setter)
        while between is not None and between.address != jcc.address:
            if kind == "reg" and self.writes_family(between, str(location)):
                return None
            if kind != "reg" and self._memory_write_to(between, location):
                return None
            between = self.code.next_insn(between)
        equal_edge = (edge.kind == "taken") == (jcc.mnemonic == "je")
        return ("eq" if equal_edge else "ne", constant)

    def _memory_write_to(self, insn: capstone.CsInsn, frame: str | int) -> bool:
        if not insn.operands or insn.address not in self.insns:
            return False
        return self._store_target(insn) == frame

    def _store_target(self, insn: capstone.CsInsn) -> int | None:
        """Frame offset an instruction stores to, else None."""
        address = insn.address
        delta = self.delta.get(address)
        if insn.mnemonic == "push":
            return None if delta is None else delta - 4
        if not insn.operands or insn.operands[0].type != cs_x86.X86_OP_MEM:
            return None
        if not insn.operands[0].access & capstone.CS_AC_WRITE:
            return None
        return self.frame_offset(address, insn.operands[0])

    # ------------------------------------------------------------------ call arguments

    def call_arguments(self, call_address: int, count: int) -> list[ValueSet]:
        """Value sets of the last `count` stack pushes before a call, argument 1 first.

        The walk goes back through the straight line that ends at the call and stops at a
        call, a branch, a return, any other write to `esp`, or a merge point (an
        instruction with more than one way in), because pushes beyond a merge belong to
        different paths. A push it could not reach is reported as an unbounded argument
        saying so. Each pushed operand is then resolved over EVERY path with the resolver.
        """
        pushes: list[capstone.CsInsn] = []
        cursor = self.code.previous_insn(self.insns[call_address])
        for _ in range(60):
            if cursor is None or cursor.address not in self.insns or len(pushes) >= count:
                break
            if capstone.CS_GRP_JUMP in cursor.groups or capstone.CS_GRP_RET in cursor.groups:
                break
            if cursor.mnemonic == "call":
                break
            if cursor.mnemonic == "push":
                pushes.append(cursor)
            elif self.writes_family(cursor, "esp"):
                break
            if len(self.preds.get(cursor.address, [])) > 1:
                break
            cursor = self.code.previous_insn(cursor)
        values = [self.operand_value(push, push.operands[0]) for push in pushes]
        while len(values) < count:
            values.append(open_set(f"push not reached from call at {call_address:#x}"))
        return values

    def incoming_arguments_read(self, before: int | None = None) -> set[int]:
        """1-based incoming stack arguments read by instructions that can reach `before`.

        `before` itself is excluded (it has not executed yet). With `before` None, every
        instruction of the function counts.
        """
        scope = self.insns.keys()
        if before is not None:
            scope = {a for a in self.insns if a != before and self.reaches(a, before)}
        found: set[int] = set()
        for address in scope:
            insn = self.insns[address]
            for operand in insn.operands:
                if operand.type != cs_x86.X86_OP_MEM or not operand.access & capstone.CS_AC_READ:
                    continue
                frame = self.frame_offset(address, operand)
                if frame is not None and frame >= 4:
                    found.add(frame // 4)
        return found

    def reaches(self, source: int, target: int, *, avoiding: Iterable[int] = ()) -> bool:
        """Whether `target` is reachable from `source` without passing through `avoiding`."""
        blocked = frozenset(avoiding)
        if source in blocked:
            return False
        pending = [source]
        seen = {source}
        while pending:
            address = pending.pop()
            if address == target:
                return True
            for successor, _ in self._successors.get(address, []):
                if successor not in seen and successor not in blocked:
                    seen.add(successor)
                    pending.append(successor)
        return False

    # ------------------------------------------------------------------ queries

    def reg_before(self, address: int, register: str) -> ValueSet:
        """The values `register` (a 32-bit family name) can hold just before `address` runs."""
        return self._query("reg", address, register)

    def slot_before(self, address: int, frame: int) -> ValueSet:
        return self._query("slot", address, frame)

    def definitions(self, kind: str, address: int, location: str | int) -> list[Definition]:
        """Every reaching definition with its value, for reporting."""
        return self._reaching(kind, address, location)

    def _query(self, kind: str, address: int, location: str | int) -> ValueSet:
        key = (kind, address, location)
        if key in self._memo:
            return self._memo[key]
        if key in self._active:
            self._cycle_hits += 1
            return open_set(f"cyclic dependency on {location} at {address:#x}")
        self._active.add(key)
        hits = self._cycle_hits
        try:
            result = ValueSet()
            for definition in self._reaching(kind, address, location):
                result = result.union(definition.value)
        finally:
            self._active.discard(key)
        if self._cycle_hits == hits:
            self._memo[key] = result
        return result

    def _defines(self, insn: capstone.CsInsn, kind: str, location: str | int) -> bool:
        if kind == "reg":
            return self.writes_family(insn, str(location)) and insn.mnemonic != "call"
        return self._store_target(insn) == location

    def _reaching(self, kind: str, address: int, location: str | int) -> list[Definition]:
        found: list[Definition] = []
        seen: set[tuple[int, tuple[tuple[str, int], ...]]] = set()
        pending: list[tuple[int, tuple[tuple[str, int], ...]]] = [(address, ())]
        while pending:
            node, constraints = pending.pop()
            if (node, constraints) in seen:
                continue
            seen.add((node, constraints))
            incoming = self.preds.get(node, [])
            if node == self.entry or not incoming:
                found.append(self._entry_definition(kind, location, constraints))
            for edge in incoming:
                step = self._edge_constraint(edge, kind, location)
                walked = constraints if step is None else (*constraints, step)
                source = self.insns[edge.source]
                if source.mnemonic == "call" and kind == "reg" and str(location) in VOLATILE:
                    found.append(self._call_definition(source, str(location), walked))
                elif self._defines(source, kind, location):
                    found.append(
                        Definition(
                            source.address, "insn", walked, self._evaluate(source, kind, location)
                        )
                    )
                else:
                    pending.append((edge.source, walked))
        return [self._constrain(item) for item in found]

    @staticmethod
    def _constrain(definition: Definition) -> Definition:
        value = definition.value
        for relation, constant in definition.constraints:
            value = value.only(constant) if relation == "eq" else value.without(constant)
        return Definition(definition.address, definition.kind, definition.constraints, value)

    def _entry_definition(
        self, kind: str, location: str | int, constraints: tuple[tuple[str, int], ...]
    ) -> Definition:
        if kind == "reg":
            reason = f"{location} at entry of {self.entry:#x} (the caller's value)"
        elif isinstance(location, int) and location > 0:
            reason = f"incoming argument {location // 4} of {self.entry:#x}"
        else:
            reason = f"frame slot {location:+#x} at entry of {self.entry:#x} (uninitialised)"
        return Definition(self.entry, "entry", constraints, open_set(reason))

    def _call_definition(
        self, call: capstone.CsInsn, register: str, constraints: tuple[tuple[str, int], ...]
    ) -> Definition:
        target = call.op_str
        if register == "eax":
            reason = f"result of call {target} at {call.address:#x}"
        else:
            reason = f"{register} clobbered by call {target} at {call.address:#x}"
        return Definition(call.address, "call", constraints, open_set(reason))

    # ------------------------------------------------------------------ evaluation

    def _evaluate(self, insn: capstone.CsInsn, kind: str, location: str | int) -> ValueSet:
        key = ("def", insn.address, location)
        if key in self._memo:
            return self._memo[key]
        if key in self._active:
            self._cycle_hits += 1
            return open_set(f"loop-carried value of {location} at {insn.address:#x}")
        self._active.add(key)
        hits = self._cycle_hits
        try:
            result = self._evaluate_uncached(insn, kind, location)
        finally:
            self._active.discard(key)
        if self._cycle_hits == hits:
            self._memo[key] = result
        return result

    def operand_value(self, insn: capstone.CsInsn, operand: cs_x86.X86Op) -> ValueSet:
        """The value of an operand of `insn` just before it executes."""
        return self._source(insn, operand)

    def _source(self, insn: capstone.CsInsn, operand: cs_x86.X86Op) -> ValueSet:
        """The value of a source operand just before `insn` executes."""
        if operand.type == cs_x86.X86_OP_IMM:
            return exact(operand.imm)
        if operand.type == cs_x86.X86_OP_REG:
            if operand.size != 4:
                return open_set(
                    f"{insn.reg_name(operand.reg)} partial register at {insn.address:#x}"
                )
            return self.reg_before(insn.address, FAMILY_OF.get(insn.reg_name(operand.reg), ""))
        if operand.type == cs_x86.X86_OP_MEM:
            frame = self.frame_offset(insn.address, operand)
            if frame is not None and operand.size == 4:
                result = self.slot_before(insn.address, frame)
                return self._with_address_taken(result, frame)
            if operand.mem.base == 0 and operand.mem.index == 0:
                return open_set(f"load [{operand.mem.disp & MASK32:#x}] at {insn.address:#x}")
            return open_set(
                f"load [{insn.op_str.split('[', 1)[-1].rstrip(']')}] at {insn.address:#x}"
            )
        return open_set(f"operand at {insn.address:#x}")

    def _with_address_taken(self, value: ValueSet, frame: int) -> ValueSet:
        for address, insn in self.insns.items():
            if insn.mnemonic != "lea" or len(insn.operands) != 2:
                continue
            if self.frame_offset(address, insn.operands[1]) == frame:
                return value.union(
                    open_set(f"address of frame slot {frame:+#x} taken at {address:#x}")
                )
        return value

    def _evaluate_uncached(self, insn: capstone.CsInsn, kind: str, location: str | int) -> ValueSet:
        mnemonic = insn.mnemonic
        ops = insn.operands
        here = f"{mnemonic} {insn.op_str} at {insn.address:#x}"
        if kind == "reg":
            destination = ops[0] if ops else None
            if destination is None or destination.type != cs_x86.X86_OP_REG:
                return open_set(f"implicit write by {here}")
            if destination.size != 4:
                if (
                    mnemonic.startswith("set")
                    and destination.size == 1
                    and insn.reg_name(destination.reg) in _LOW_BYTE
                ):
                    inner = self.reg_before(insn.address, str(location))
                    return ValueSet(
                        frozenset((v & ~0xFF) | bit for v in inner.values for bit in (0, 1)),
                        inner.unbounded,
                    )
                return open_set(f"partial register write by {here}")
        if mnemonic == "mov" and len(ops) == 2:
            return self._source(insn, ops[1])
        if mnemonic == "push" and kind == "slot":
            return self._source(insn, ops[0])
        if (
            mnemonic in ("xor", "sub")
            and len(ops) == 2
            and ops[0].type == ops[1].type == cs_x86.X86_OP_REG
            and ops[0].reg == ops[1].reg
        ):
            return exact(0)
        if (
            mnemonic == "sbb"
            and len(ops) == 2
            and ops[0].type == ops[1].type == cs_x86.X86_OP_REG
            and ops[0].reg == ops[1].reg
        ):
            previous = self.code.previous_insn(insn)
            if (
                previous is not None
                and previous.mnemonic == "neg"
                and previous.operands[0].reg == ops[0].reg
            ):
                return exact(0, MASK32)  # `neg r; sbb r, r` is 0 or -1 whatever r held
            return ValueSet(
                frozenset({0, MASK32}), (f"sbb r, r depends on CF at {insn.address:#x}",)
            )
        if mnemonic in ("neg", "not") and len(ops) == 1:
            inner = self._self_value(insn, kind, location)
            return inner.map(lambda v: -v if mnemonic == "neg" else ~v)
        if mnemonic == "lea" and len(ops) == 2:
            return self._lea(insn)
        if mnemonic in ("inc", "dec") and len(ops) == 1:
            inner = self._self_value(insn, kind, location)
            return inner.map(lambda v: v + 1 if mnemonic == "inc" else v - 1)
        if (
            mnemonic == "or"
            and len(ops) == 2
            and ops[1].type == cs_x86.X86_OP_IMM
            and (ops[1].imm & MASK32) == MASK32
        ):
            return exact(MASK32)  # `or r, -1` is all ones whatever r held
        if (
            mnemonic == "and"
            and len(ops) == 2
            and ops[1].type == cs_x86.X86_OP_IMM
            and (ops[1].imm & MASK32) == 0
        ):
            return exact(0)
        if mnemonic in _BINARY and len(ops) == 2:
            inner = self._self_value(insn, kind, location)
            other = self._source(insn, ops[1])
            if not (inner.bounded and other.bounded):
                reasons = inner.unbounded + other.unbounded
                return ValueSet(frozenset(), tuple(dict.fromkeys(reasons)) or (here,))
            combined = {
                _BINARY[mnemonic](a, b) & MASK32 for a in inner.values for b in other.values
            }
            return ValueSet(frozenset(combined)) if len(combined) <= MAX_VALUES else open_set(here)
        return open_set(here)

    def _self_value(self, insn: capstone.CsInsn, kind: str, location: str | int) -> ValueSet:
        """The destination's value just before a read-modify-write instruction."""
        if kind == "reg":
            return self.reg_before(insn.address, str(location))
        return self.slot_before(insn.address, int(location))

    def _lea(self, insn: capstone.CsInsn) -> ValueSet:
        mem = insn.operands[1].mem
        total = exact(mem.disp)
        terms: list[tuple[int, int]] = []
        if mem.base:
            terms.append((mem.base, 1))
        if mem.index:
            terms.append((mem.index, mem.scale))
        for register, scale in terms:
            name = FAMILY_OF.get(insn.reg_name(register), insn.reg_name(register))
            if name == "esp":
                return open_set(f"address of the stack at {insn.address:#x}")
            part = self.reg_before(insn.address, name)
            if not part.bounded:
                return ValueSet(frozenset(), part.unbounded)
            total = ValueSet(
                frozenset((a + b * scale) & MASK32 for a in total.values for b in part.values)
            )
        return total


_LOW_BYTE = frozenset({"al", "bl", "cl", "dl"})
_BINARY: dict[str, Callable[[int, int], int]] = {
    "add": lambda a, b: a + b,
    "sub": lambda a, b: a - b,
    "and": lambda a, b: a & b,
    "or": lambda a, b: a | b,
    "xor": lambda a, b: a ^ b,
    "shl": lambda a, b: a << (b & 31),
    "shr": lambda a, b: (a & MASK32) >> (b & 31),
}


MAX_ENTRY_CANDIDATES = 12


def flow_for(code: Code, address: int, *, entry: int | None = None) -> Flow:
    """The `Flow` of the function containing `address` (see `FlowCache.flow_for`)."""
    return FlowCache(code).flow_for(address, entry=entry)


class FlowCache:
    """`flow_for` with one `Flow` per entry, so a census builds each function once."""

    def __init__(self, code: Code) -> None:
        self.code = code
        self._flows: dict[int, Flow] = {}

    def at_entry(self, entry: int) -> Flow:
        if entry not in self._flows:
            self._flows[entry] = Flow(self.code, entry)
        return self._flows[entry]

    def flow_for(self, address: int, *, entry: int | None = None) -> Flow:
        """The `Flow` of the function containing `address`.

        With `entry` given it is used as is (and must reach `address`). Otherwise the nearest
        candidate entry whose forward flood contains `address` wins, which is what separates
        the true entry from an earlier function that merely ends before the site.
        """
        if entry is not None:
            flow = self.at_entry(entry)
            if address not in flow.insns:
                raise LookupError(f"{address:#x} is not reachable from the given entry {entry:#x}")
            return flow
        for count, candidate in enumerate(self.code.entry_candidates(address)):
            if count >= MAX_ENTRY_CANDIDATES:
                break
            flow = self.at_entry(candidate)
            if address in flow.insns:
                return flow
        raise LookupError(f"no candidate entry reaches {address:#x}")


def describe_all(definitions: Iterable[Definition]) -> list[str]:
    lines = []
    for definition in definitions:
        guard = "".join(
            f" [{relation} {constant:#x}]" for relation, constant in definition.constraints
        )
        lines.append(
            f"{definition.kind} {definition.address:#x}{guard}: {definition.value.describe()}"
        )
    return lines
