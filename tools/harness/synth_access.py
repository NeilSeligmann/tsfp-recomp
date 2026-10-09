# SPDX-License-Identifier: GPL-3.0-or-later
"""T1773 access summary: what the ORIGINAL function does with its inputs, from its bytes only.

A bounded forward abstract interpretation over the function's own control-flow graph. Every
value is either unknown (`None`) or a linear form over named input roots:

    value = sum(coeff_i * root_i) + const   (mod 2**32), optionally widened with a stride

An input root is a place the harness can seed before the call: a stack argument dword, a
register at entry, or a memory cell (a global, or a field reached through another root).
Every memory operand is recorded as an access (read/write, width, address form), every
comparison against a constant as a seed fact. Nothing here reads a replacement, a draft or an
oracle outcome: the summary is a pure function of `(code bytes, va)`.

The summary only steers which initial state the fixture generator draws. It is never evidence
that the function behaves a certain way, so an imprecise or wrong summary lowers coverage,
it cannot make a verdict unsound (both sides receive the same state whatever it is).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from math import gcd
from typing import Any

MASK32 = 0xFFFFFFFF
ESP0 = "esp0"

#: Registers the interpreter models, in capstone-name form.
FULL_REGS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
_SUBREGS: dict[str, tuple[str, int, int]] = {}
for _base, _w16, _lo, _hi in (
    ("eax", "ax", "al", "ah"),
    ("ecx", "cx", "cl", "ch"),
    ("edx", "dx", "dl", "dh"),
    ("ebx", "bx", "bl", "bh"),
):
    _SUBREGS[_w16] = (_base, 16, 0)
    _SUBREGS[_lo] = (_base, 8, 0)
    _SUBREGS[_hi] = (_base, 8, 8)
for _base, _w16 in (("esp", "sp"), ("ebp", "bp"), ("esi", "si"), ("edi", "di")):
    _SUBREGS[_w16] = (_base, 16, 0)

MAX_BLOCK_VISITS = 40
MAX_INSNS = 1200


@dataclass(frozen=True)
class Lin:
    """sum(coeff * root) + const, with an optional loop stride on the constant part."""

    terms: tuple[tuple[str, int], ...] = ()
    const: int = 0
    stride: int = 0

    @property
    def is_const(self) -> bool:
        return not self.terms and not self.stride

    def single(self) -> tuple[str, int] | None:
        return self.terms[0] if len(self.terms) == 1 else None


def const_lin(value: int) -> Lin:
    return Lin((), value & MASK32, 0)


def root_lin(key: str) -> Lin:
    return Lin(((key, 1),), 0, 0)


def _norm(terms: dict[str, int]) -> tuple[tuple[str, int], ...]:
    return tuple(sorted((key, coeff & MASK32) for key, coeff in terms.items() if coeff & MASK32))


def _signed(value: int) -> int:
    value &= MASK32
    return value - (1 << 32) if value & 0x80000000 else value


def lin_add(a: Lin | None, b: Lin | None, sign: int = 1) -> Lin | None:
    if a is None or b is None:
        return None
    merged: dict[str, int] = dict(a.terms)
    for key, coeff in b.terms:
        merged[key] = merged.get(key, 0) + sign * coeff
    return Lin(_norm(merged), (a.const + sign * b.const) & MASK32, gcd(a.stride, b.stride))


def lin_mul(a: Lin | None, factor: int) -> Lin | None:
    if a is None:
        return None
    factor &= MASK32
    return Lin(
        _norm({key: coeff * factor for key, coeff in a.terms}),
        (a.const * factor) & MASK32,
        (a.stride * factor) & MASK32,
    )


def _join_rank(value: Lin) -> tuple[int, int, str]:
    text = lin_text(value)
    return (0 if value.terms else 1, len(text), text)


def lin_join(a: Lin | None, b: Lin | None) -> Lin | None:
    """Merge two path values. A STEERING heuristic: the summary only chooses inputs.

    Equal values stay. An unknown value yields to the known one. Values over the same roots
    widen into a stride (a loop-advanced pointer). Anything else keeps the value over the
    shallower root form (deterministic total order), so a pointer chain walked by a loop or
    selected by a branch still contributes its accesses instead of collapsing to unknown.
    """
    if a == b:
        return a
    if a is None:
        return b
    if b is None:
        return a
    if a.terms == b.terms:
        stride = gcd(gcd(a.stride, b.stride), abs(_signed(a.const - b.const)))
        return Lin(a.terms, min(a.const, b.const), stride)
    return min((a, b), key=_join_rank)


def lin_text(lin: Lin) -> str:
    parts = [key if coeff == 1 else f"{_signed(coeff)}*{key}" for key, coeff in lin.terms]
    text = "+".join(parts) if parts else ""
    if lin.const or not text:
        text += f"{'+' if text else ''}{lin.const:#x}"
    if lin.stride:
        text += f"~{lin.stride:#x}"
    return text


@dataclass(frozen=True)
class Root:
    """An input place. `kind` is arg, reg, cell or ret."""

    key: str
    kind: str
    width: int
    index: int = 0
    reg: str = ""
    addr: Lin | None = None
    #: For an opaque root: the roots its value was derived from.
    sources: tuple[str, ...] = ()


@dataclass(frozen=True)
class Access:
    va: int
    rw: str
    width: int
    addr: Lin
    repeat: int = 1
    #: True for an access through a loop-advanced address (extent not bounded by the summary).
    strided: bool = False


@dataclass(frozen=True)
class Fact:
    """A constant the function compares an input against (a seed for the generator)."""

    va: int
    kind: str  # eq, mask, zero, rel
    root: str
    value: int = 0
    other: str = ""
    width: int = 4
    stride: int = 0


@dataclass
class AccessSummary:
    va: int
    size: int
    insns: int = 0
    roots: dict[str, Root] = field(default_factory=dict)
    accesses: list[Access] = field(default_factory=list)
    facts: list[Fact] = field(default_factory=list)
    unresolved: int = 0
    calls: int = 0
    string_ops: int = 0
    loops: int = 0
    indirect: int = 0
    limits: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        roots = []
        for key in sorted(self.roots):
            root = self.roots[key]
            row: dict[str, Any] = {"key": key, "kind": root.kind, "width": root.width}
            if root.kind == "arg":
                row["index"] = root.index
            if root.kind == "reg":
                row["reg"] = root.reg
            if root.sources:
                row["sources"] = list(root.sources)
            if root.addr is not None:
                row["addr"] = lin_text(root.addr)
            roots.append(row)
        accesses = sorted(
            {(a.va, a.rw, a.width, lin_text(a.addr), a.repeat, a.strided) for a in self.accesses}
        )
        facts = sorted(
            {(f.va, f.kind, f.root, f.value, f.other, f.width, f.stride) for f in self.facts}
        )
        return {
            "va": f"{self.va:#010x}",
            "size": self.size,
            "insns": self.insns,
            "roots": roots,
            "accesses": [
                {
                    "va": f"{va:#x}",
                    "rw": rw,
                    "width": width,
                    "addr": addr,
                    "repeat": repeat,
                    "strided": strided,
                }
                for va, rw, width, addr, repeat, strided in accesses
            ],
            "facts": [
                {
                    "va": f"{va:#x}",
                    "kind": kind,
                    "root": root,
                    "value": value,
                    "other": other,
                    "width": width,
                    "stride": stride,
                }
                for va, kind, root, value, other, width, stride in facts
            ],
            "unresolved_accesses": self.unresolved,
            "calls": self.calls,
            "string_ops": self.string_ops,
            "loops": self.loops,
            "indirect_jumps": self.indirect,
            "limits": sorted(set(self.limits)),
        }


@dataclass
class _State:
    regs: dict[str, Lin | None]
    stack: dict[int, Lin | None]
    written: dict[str, Lin | None]

    def copy(self) -> _State:
        return _State(dict(self.regs), dict(self.stack), dict(self.written))


def _join_dict(
    a: dict[Any, Lin | None], b: dict[Any, Lin | None], *, keep_one_sided: bool
) -> dict[Any, Lin | None]:
    out: dict[Any, Lin | None] = {}
    for key in set(a) | set(b):
        if key in a and key in b:
            out[key] = lin_join(a[key], b[key])
        elif keep_one_sided:
            out[key] = a[key] if key in a else b[key]
        else:
            out[key] = None
    return out


def _join(a: _State, b: _State) -> _State:
    return _State(
        _join_dict(a.regs, b.regs, keep_one_sided=True),
        _join_dict(a.stack, b.stack, keep_one_sided=True),
        _join_dict(a.written, b.written, keep_one_sided=False),
    )


def _same(a: _State, b: _State) -> bool:
    return a.regs == b.regs and a.stack == b.stack and a.written == b.written


def _capstone() -> Any:
    import capstone

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return md


class _Walker:
    def __init__(self, va: int, code: bytes) -> None:
        self.va = va
        self.code = code
        self.md = _capstone()
        self.summary = AccessSummary(va=va, size=len(code))
        self.insns: dict[int, Any] = {}
        self.order: list[int] = []

    # ---- decoding and CFG -------------------------------------------------------
    def decode(self) -> None:
        for insn in self.md.disasm(self.code, self.va):
            self.insns[insn.address] = insn
            self.order.append(insn.address)
            if len(self.order) > MAX_INSNS:
                self.summary.limits.append("instruction-cap")
                break
        self.summary.insns = len(self.order)
        decoded = sum(self.insns[a].size for a in self.order)
        if decoded != len(self.code):
            self.summary.limits.append("decode-short")

    def _jump_target(self, insn: Any) -> int | None:
        if insn.operands and insn.operands[0].type == 2:  # CS_OP_IMM
            return int(insn.operands[0].imm) & MASK32
        return None

    def build_blocks(self) -> tuple[dict[int, list[int]], dict[int, list[int]]]:
        leaders = {self.va}
        end = self.va + len(self.code)
        for address in self.order:
            insn = self.insns[address]
            name = insn.mnemonic
            if name == "jmp" or name.startswith("j") or name in ("loop", "loope", "loopne"):
                target = self._jump_target(insn)
                if target is not None and self.va <= target < end:
                    leaders.add(target)
                leaders.add(address + insn.size)
            elif name in ("ret", "retn"):
                leaders.add(address + insn.size)
        blocks: dict[int, list[int]] = {}
        current = None
        for address in self.order:
            if address in leaders or current is None:
                current = address
                blocks[current] = []
            blocks[current].append(address)
        succ: dict[int, list[int]] = {}
        for start, addresses in blocks.items():
            last = self.insns[addresses[-1]]
            name = last.mnemonic
            after = addresses[-1] + last.size
            targets: list[int] = []
            if name in ("ret", "retn"):
                pass
            elif name == "jmp":
                target = self._jump_target(last)
                if target is not None and target in blocks:
                    targets.append(target)
                elif target is None:
                    self.summary.indirect += 1
            elif name.startswith("j") or name in ("loop", "loope", "loopne"):
                target = self._jump_target(last)
                if target is not None and target in blocks:
                    targets.append(target)
                if after in blocks:
                    targets.append(after)
            elif after in blocks:
                targets.append(after)
            succ[start] = targets
        return blocks, succ

    # ---- register helpers -------------------------------------------------------
    def reg_get(self, state: _State, name: str) -> Lin | None:
        if name in state.regs:
            return state.regs[name]
        root = Root(f"R:{name}", "reg", 4, reg=name)
        self.summary.roots.setdefault(root.key, root)
        value = root_lin(root.key)
        state.regs[name] = value
        return value

    def read_reg(self, state: _State, name: str) -> Lin | None:
        if name in FULL_REGS:
            return self.reg_get(state, name)
        base, bits, shift = _SUBREGS.get(name, ("", 0, 0))
        if not base:
            return None
        full = self.reg_get(state, base)
        if full is None:
            return None
        if full.is_const:
            return const_lin((full.const >> shift) & ((1 << bits) - 1))
        single = full.single()
        if single is not None and single[1] == 1 and full.const == 0 and shift == 0:
            root = self.summary.roots.get(single[0])
            if root is not None and root.width <= bits // 8:
                return full
        return None

    def write_reg(self, state: _State, name: str, value: Lin | None, width: int = 4) -> None:
        if name in FULL_REGS:
            state.regs[name] = value
            return
        base, bits, shift = _SUBREGS.get(name, ("", 0, 0))
        if not base:
            return
        if value is not None and value.single() is not None and shift == 0:
            root = self.summary.roots.get(value.single()[0])  # type: ignore[index]
            if root is not None and root.kind in ("cell", "flag") and root.width <= bits // 8:
                state.regs[base] = value
                return
        state.regs[base] = None

    # ---- memory -----------------------------------------------------------------
    def mem_addr(self, state: _State, mem: Any, segment: str) -> Lin | None:
        if segment in ("fs", "gs"):
            return None
        total: Lin | None = const_lin(mem.disp)
        if mem.base:
            total = lin_add(total, self.read_reg(state, self.md.reg_name(mem.base)))
        if mem.index:
            idx = self.read_reg(state, self.md.reg_name(mem.index))
            total = lin_add(total, lin_mul(idx, mem.scale))
        return total

    def _cell(self, addr: Lin, width: int) -> Root:
        key = f"M{width}[{lin_text(addr)}]"
        root = self.summary.roots.get(key)
        if root is None:
            root = Root(key, "cell", width, addr=addr)
            self.summary.roots[key] = root
        return root

    def _stack_slot(self, addr: Lin) -> int | None:
        if addr.terms == ((ESP0, 1),) and not addr.stride:
            return _signed(addr.const)
        return None

    def record(self, rw: str, width: int, addr: Lin | None, va: int, repeat: int = 1) -> None:
        if addr is None:
            self.summary.unresolved += 1
            return
        if any(key == ESP0 for key, _ in addr.terms):
            return
        self.summary.accesses.append(Access(va, rw, width, addr, repeat, bool(addr.stride)))

    def load(self, state: _State, addr: Lin | None, width: int, va: int) -> Lin | None:
        if addr is None:
            self.summary.unresolved += 1
            return None
        slot = self._stack_slot(addr)
        if slot is not None:
            if slot in state.stack:
                return state.stack[slot] if width == 4 else None
            if slot >= 4 and slot % 4 == 0 and width == 4:
                index = (slot - 4) // 4
                key = f"A{index}"
                self.summary.roots.setdefault(key, Root(key, "arg", 4, index=index))
                return root_lin(key)
            return None
        if any(key == ESP0 for key, _ in addr.terms):
            return None
        self.record("r", width, addr, va)
        if addr.stride:
            # A loop-advanced element: its first iteration stands for the field layout.
            return root_lin(self._cell(Lin(addr.terms, addr.const, 0), width).key)
        key = f"M{width}[{lin_text(addr)}]"
        if key in state.written:
            return state.written[key]
        return root_lin(self._cell(addr, width).key)

    def store(
        self, state: _State, addr: Lin | None, width: int, value: Lin | None, va: int
    ) -> None:
        if addr is None:
            self.summary.unresolved += 1
            return
        slot = self._stack_slot(addr)
        if slot is not None:
            state.stack[slot] = value if width == 4 else None
            return
        if any(key == ESP0 for key, _ in addr.terms):
            return
        self.record("w", width, addr, va)
        if not addr.stride:
            state.written[f"M{width}[{lin_text(addr)}]"] = value if width == 4 else None

    # ---- operand evaluation -----------------------------------------------------
    def operand_value(self, state: _State, insn: Any, op: Any) -> Lin | None:
        if op.type == 1:
            return self.read_reg(state, insn.reg_name(op.reg))
        if op.type == 2:
            return const_lin(op.imm)
        if op.type == 3:
            segment = insn.reg_name(op.mem.segment) if op.mem.segment else ""
            addr = self.mem_addr(state, op.mem, segment)
            if segment in ("fs", "gs"):
                return None
            return self.load(state, addr, op.size, insn.address)
        return None

    def mem_operand_addr(self, state: _State, insn: Any, op: Any) -> Lin | None:
        segment = insn.reg_name(op.mem.segment) if op.mem.segment else ""
        return self.mem_addr(state, op.mem, segment)

    def assign(self, state: _State, insn: Any, op: Any, value: Lin | None) -> None:
        if op.type == 1:
            self.write_reg(state, insn.reg_name(op.reg), value, op.size)
        elif op.type == 3:
            segment = insn.reg_name(op.mem.segment) if op.mem.segment else ""
            if segment in ("fs", "gs"):
                return
            self.store(state, self.mem_operand_addr(state, insn, op), op.size, value, insn.address)

    def opaque(self, insn: Any, source: Lin | None) -> Lin | None:
        """A value derived from a known input by an operation the summary does not model.

        It becomes an opaque root: the generator never seeds it (it contributes 0), but the
        sums it takes part in keep their other terms, so `opaque + global_pointer + 0x2c`
        still names the global as the pointer instead of collapsing to unknown.
        """
        if source is None or source.is_const:
            return None
        key = f"O{insn.address:x}"
        sources = tuple(sorted({term for term, _ in source.terms}))
        self.summary.roots.setdefault(key, Root(key, "opaque", 4, sources=sources))
        return root_lin(key)

    # ---- facts ------------------------------------------------------------------
    def compare_facts(self, insn: Any, a: Lin | None, b: Lin | None, width: int, kind: str) -> None:
        if a is None or b is None:
            return
        if kind == "test":
            if a == b and a.single() and a.const == 0:
                self.summary.facts.append(Fact(insn.address, "zero", a.single()[0], 0, "", width))  # type: ignore[index]
                return
            if b.is_const and a.single() and a.single()[1] == 1:  # type: ignore[index]
                self.summary.facts.append(
                    Fact(insn.address, "mask", a.single()[0], b.const, "", width)  # type: ignore[index]
                )
            return
        for var, other in ((a, b), (b, a)):
            single = var.single()
            if single is None or not other.is_const:
                continue
            root, coeff = single
            signed = _signed(coeff)
            if signed in (0,):
                continue
            delta = _signed(other.const - var.const)
            if delta % signed:
                continue
            value = (delta // signed) & MASK32
            self.summary.facts.append(Fact(insn.address, "eq", root, value, "", width))
            return
        sa, sb = a.single(), b.single()
        if sa and sb and sa[1] == 1 and sb[1] == 1 and sa[0] != sb[0]:
            offset = (a.const - b.const) & MASK32
            stride = max(a.stride, b.stride)
            self.summary.facts.append(
                Fact(insn.address, "rel", sa[0], offset, sb[0], width, stride)
            )

    # ---- instruction semantics --------------------------------------------------
    def step(self, state: _State, insn: Any) -> None:
        name = insn.mnemonic
        ops = insn.operands
        if name.startswith("rep"):
            name = name.split()[-1]
        mnemonic = name
        if mnemonic in ("nop", "int3", "fnop", "wait", "cld", "std", "clc", "stc", "cmc"):
            return
        if mnemonic == "push":
            value = self.operand_value(state, insn, ops[0]) if ops else None
            esp = self.reg_get(state, "esp")
            esp = lin_add(esp, const_lin(-4))
            state.regs["esp"] = esp
            if esp is not None and self._stack_slot(esp) is not None:
                state.stack[self._stack_slot(esp)] = value  # type: ignore[index]
            return
        if mnemonic == "pop":
            esp = self.reg_get(state, "esp")
            value = None
            if esp is not None and self._stack_slot(esp) is not None:
                value = state.stack.get(self._stack_slot(esp))  # type: ignore[arg-type]
                if value is None and self._stack_slot(esp) >= 4 and False:  # pragma: no cover
                    value = None
            state.regs["esp"] = lin_add(esp, const_lin(4))
            if ops:
                self.assign(state, insn, ops[0], value)
            return
        if mnemonic == "leave":
            ebp = self.reg_get(state, "ebp")
            state.regs["esp"] = lin_add(ebp, const_lin(4))
            slot = self._stack_slot(ebp) if ebp is not None else None
            state.regs["ebp"] = state.stack.get(slot) if slot is not None else None
            return
        if mnemonic == "call":
            self.summary.calls += 1
            for reg in ("eax", "ecx", "edx"):
                state.regs[reg] = None
            if ops and ops[0].type != 2:
                self.summary.indirect += 1
            state.written.clear()
            return
        if mnemonic in ("mov", "movzx", "movsx", "movsxd", "movabs"):
            value = self.operand_value(state, insn, ops[1])
            if mnemonic == "movsx" and value is not None and not value.is_const:
                value = value  # sign extension of an input cell is treated as the cell itself
            self.assign(state, insn, ops[0], value)
            return
        if mnemonic == "lea":
            self.assign(state, insn, ops[0], self.mem_operand_addr(state, insn, ops[1]))
            return
        if mnemonic in ("add", "sub", "adc", "sbb"):
            left = self.operand_value(state, insn, ops[0])
            right = self.operand_value(state, insn, ops[1])
            if (
                mnemonic == "sub"
                and ops[0].type == 1
                and ops[1].type == 1
                and ops[0].reg == ops[1].reg
            ):
                result: Lin | None = const_lin(0)
            elif mnemonic in ("adc", "sbb"):
                result = None
            else:
                result = lin_add(left, right, 1 if mnemonic == "add" else -1)
            if mnemonic == "sub":
                self.compare_facts(insn, left, right, ops[0].size, "cmp")
            self.assign(state, insn, ops[0], result)
            return
        if mnemonic in ("inc", "dec"):
            value = self.operand_value(state, insn, ops[0])
            self.assign(
                state, insn, ops[0], lin_add(value, const_lin(1 if mnemonic == "inc" else -1))
            )
            return
        if mnemonic == "neg":
            value = self.operand_value(state, insn, ops[0])
            self.assign(state, insn, ops[0], lin_mul(value, -1))
            return
        if mnemonic == "cmp":
            left = self.operand_value(state, insn, ops[0])
            right = self.operand_value(state, insn, ops[1])
            self.compare_facts(insn, left, right, ops[0].size, "cmp")
            return
        if mnemonic == "test":
            left = self.operand_value(state, insn, ops[0])
            right = self.operand_value(state, insn, ops[1])
            self.compare_facts(insn, left, right, ops[0].size, "test")
            return
        if mnemonic in ("and", "or", "xor"):
            left = self.operand_value(state, insn, ops[0])
            right = self.operand_value(state, insn, ops[1])
            result = None
            if (
                mnemonic == "xor"
                and ops[0].type == 1
                and ops[1].type == 1
                and ops[0].reg == ops[1].reg
            ):
                result = const_lin(0)
            elif left is not None and right is not None and left.is_const and right.is_const:
                fn = {"and": int.__and__, "or": int.__or__, "xor": int.__xor__}[mnemonic]
                result = const_lin(fn(left.const, right.const))
            elif mnemonic == "and" and left is not None and right is not None and right.is_const:
                self.compare_facts(insn, left, right, ops[0].size, "test")
            if result is None:
                result = self.opaque(insn, left)
            self.assign(state, insn, ops[0], result)
            return
        if mnemonic in ("shl", "sal"):
            value = self.operand_value(state, insn, ops[0])
            count = self.operand_value(state, insn, ops[1]) if len(ops) > 1 else const_lin(1)
            result = None
            if count is not None and count.is_const and count.const < 32:
                result = lin_mul(value, 1 << count.const)
            self.assign(state, insn, ops[0], result)
            return
        if mnemonic in ("shr", "sar", "rol", "ror", "not", "bswap", "sbb", "bts", "btr", "btc"):
            if ops:
                source = self.operand_value(state, insn, ops[0])
                self.assign(state, insn, ops[0], self.opaque(insn, source))
            return
        if mnemonic == "imul":
            if len(ops) == 3:
                src = self.operand_value(state, insn, ops[1])
                factor = self.operand_value(state, insn, ops[2])
                result = (
                    lin_mul(src, factor.const) if factor is not None and factor.is_const else None
                )
                self.assign(state, insn, ops[0], result)
                return
            if len(ops) == 2:
                left = self.operand_value(state, insn, ops[0])
                right = self.operand_value(state, insn, ops[1])
                result = None
                if right is not None and right.is_const:
                    result = lin_mul(left, right.const)
                elif left is not None and left.is_const:
                    result = lin_mul(right, left.const)
                self.assign(state, insn, ops[0], result)
                return
            if ops:
                self.operand_value(state, insn, ops[0])
            state.regs["eax"] = None
            state.regs["edx"] = None
            return
        if mnemonic in ("mul", "div", "idiv"):
            if ops:
                self.operand_value(state, insn, ops[0])
            state.regs["eax"] = None
            state.regs["edx"] = None
            return
        if mnemonic in ("cdq", "cwd"):
            state.regs["edx"] = None
            return
        if mnemonic in ("cwde", "cbw"):
            state.regs["eax"] = None
            return
        if mnemonic == "xchg":
            left = self.operand_value(state, insn, ops[0])
            right = self.operand_value(state, insn, ops[1])
            self.assign(state, insn, ops[0], right)
            self.assign(state, insn, ops[1], left)
            return
        if mnemonic.startswith("set"):
            key = f"F{insn.address:x}"
            self.summary.roots.setdefault(key, Root(key, "flag", 1))
            self.assign(state, insn, ops[0], root_lin(key))
            return
        if mnemonic.startswith("cmov"):
            self.operand_value(state, insn, ops[1])
            self.assign(state, insn, ops[0], None)
            return
        if mnemonic in (
            "movs",
            "movsb",
            "movsw",
            "movsd",
            "stos",
            "stosb",
            "stosw",
            "stosd",
            "lods",
            "lodsb",
            "lodsw",
            "lodsd",
            "scas",
            "scasb",
            "scasw",
            "scasd",
            "cmps",
            "cmpsb",
            "cmpsw",
            "cmpsd",
        ) and not ops_have_xmm(insn):
            self.string_op(state, insn, mnemonic)
            return
        if (
            mnemonic in ("ret", "retn", "jmp")
            or mnemonic.startswith("j")
            or mnemonic.startswith("loop")
        ):
            return
        self.generic(state, insn)

    def string_op(self, state: _State, insn: Any, mnemonic: str) -> None:
        self.summary.string_ops += 1
        width = {"b": 1, "w": 2, "d": 4}.get(mnemonic[-1], 4)
        if mnemonic in ("movs", "stos", "lods", "scas", "cmps"):
            width = insn.operands[0].size if insn.operands else 4
        rep = "rep" in insn.mnemonic or any(p for p in insn.prefix if p in (0xF2, 0xF3))
        count: Lin | None = const_lin(1)
        if rep:
            count = self.reg_get(state, "ecx")
        repeat = 1
        if count is not None and count.is_const:
            repeat = max(1, count.const)
        elif rep:
            repeat = 0  # unbounded in the summary, domain gives the count root small values
            if count is not None and count.single():
                self.summary.facts.append(Fact(insn.address, "count", count.single()[0], 0, "", 4))  # type: ignore[index]
        base = mnemonic.rstrip("bwd")
        edi = self.reg_get(state, "edi")
        esi = self.reg_get(state, "esi")
        if base in ("movs", "cmps", "lods"):
            self.record("r", width, esi, insn.address, repeat)
        if base in ("movs", "stos"):
            self.record("w", width, edi, insn.address, repeat)
        if base in ("cmps", "scas"):
            self.record("r", width, edi, insn.address, repeat)
        step = width * (repeat if repeat else 1)
        if base in ("movs", "cmps", "lods"):
            state.regs["esi"] = lin_add(esi, const_lin(step)) if repeat else None
        if base in ("movs", "stos", "cmps", "scas"):
            state.regs["edi"] = lin_add(edi, const_lin(step)) if repeat else None
        if rep:
            state.regs["ecx"] = const_lin(0) if repeat else None
        if base in ("lods", "scas"):
            state.regs["eax"] = None

    def generic(self, state: _State, insn: Any) -> None:
        """Unmodelled instruction: record memory operands, forget every written register."""
        try:
            _read, written = insn.regs_access()
        except Exception:
            written = ()
        for op in insn.operands:
            if op.type == 3:
                segment = insn.reg_name(op.mem.segment) if op.mem.segment else ""
                if segment in ("fs", "gs"):
                    continue
                addr = self.mem_operand_addr(state, insn, op)
                access = getattr(op, "access", 1)
                if access & 1:
                    self.load(state, addr, max(op.size, 1), insn.address)
                if access & 2:
                    self.store(state, addr, max(op.size, 1), None, insn.address)
        for reg in written:
            name = insn.reg_name(reg)
            if name in FULL_REGS:
                state.regs[name] = None
            elif name in _SUBREGS:
                state.regs[_SUBREGS[name][0]] = None

    # ---- driver -----------------------------------------------------------------
    def run(self) -> AccessSummary:
        self.decode()
        if not self.order:
            self.summary.limits.append("empty")
            return self.summary
        blocks, succ = self.build_blocks()
        entry = _State(regs={"esp": Lin(((ESP0, 1),), 0, 0)}, stack={}, written={})
        in_state: dict[int, _State] = {self.va: entry}
        visits: dict[int, int] = {}
        work = [self.va]
        while work:
            start = work.pop(0)
            visits[start] = visits.get(start, 0) + 1
            if visits[start] > MAX_BLOCK_VISITS:
                self.summary.limits.append("visit-cap")
                continue
            state = in_state[start].copy()
            scratch = _Scratch(self.summary)
            with scratch:
                for address in blocks[start]:
                    self.step(state, self.insns[address])
            for target in succ[start]:
                if target in in_state:
                    merged = _join(in_state[target], state)
                    if _same(merged, in_state[target]):
                        continue
                    in_state[target] = merged
                else:
                    in_state[target] = state.copy()
                if target not in work:
                    work.append(target)
        # Final emission pass over the fixed point, so facts and accesses see stable states.
        self.summary.accesses.clear()
        self.summary.facts.clear()
        self.summary.unresolved = 0
        self.summary.calls = 0
        self.summary.string_ops = 0
        for start in sorted(blocks):
            if start not in in_state:
                continue
            state = in_state[start].copy()
            for address in blocks[start]:
                self.step(state, self.insns[address])
        self.summary.loops = sum(
            1 for start, targets in succ.items() if any(target <= start for target in targets)
        )
        return self.summary


class _Scratch:
    """Discard facts recorded during fixed-point iteration (roots are kept, they are stable)."""

    def __init__(self, summary: AccessSummary) -> None:
        self.summary = summary

    def __enter__(self) -> None:
        self.counts = (
            len(self.summary.accesses),
            len(self.summary.facts),
            self.summary.unresolved,
            self.summary.calls,
            self.summary.string_ops,
        )

    def __exit__(self, *_exc: object) -> None:
        a, f, u, c, s = self.counts
        del self.summary.accesses[a:]
        del self.summary.facts[f:]
        self.summary.unresolved = u
        self.summary.calls = c
        self.summary.string_ops = s


def ops_have_xmm(insn: Any) -> bool:
    return any(op.type == 1 and insn.reg_name(op.reg).startswith("xmm") for op in insn.operands)


def summarize(va: int, code: bytes) -> AccessSummary:
    """The access summary of the function whose exact bytes are `code`, entered at `va`."""
    return _Walker(va, code).run()
