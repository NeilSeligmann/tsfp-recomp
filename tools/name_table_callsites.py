# SPDX-License-Identifier: GPL-3.0-or-later
"""Name tables, their getters and constant-index call sites (T1503).

    python -m tools.name_table_callsites census [--json tmp/t1503/census.json]
    python -m tools.name_table_callsites functions [--min-names 1 --max-names 2]
    python -m tools.name_table_callsites spec --out tmp/t1503/spec.txt

Generalizes the T1467 texture table recipe (24 byte records at 0x4d2d64, dword +16 points at
the texture name, getter 0x80f20 takes an index). Read-only on the XBE, repo-relative paths.

1. Table finder: arrays of fixed-stride records in .rdata/.data/.data1 where one dword field of
   consecutive records points at a printable C string of the image. Candidates are chains
   `slot + k*stride` over every stride 4..0xfc; a chain tolerates up to two null pointers in a
   row. Chains are ranked by valid record count, overlapping chains are dropped.
2. Code pass: forward abstract interpretation of every non-library function. Each register holds
   `const + mult*symbol` (a symbol is an unknown leaf such as a stack argument, the entry value
   of ecx or a loaded value). The state is thrown away at every jump target and after every
   jmp/ret, so a value that merges two paths is reported unresolved, never guessed.
3. Getters: small functions whose memory operand resolves to `table + mult*arg` with
   `mult == stride`. Functions that only forward their own argument to a getter are wrappers
   and become getters of the same table (a few rounds).
4. Call sites: a call to a getter whose index argument is a constant resolves to
   (table, index, string). Inline accesses with a constant address inside a table resolve the
   same way. Everything else is listed unresolved with the reason.
"""

from __future__ import annotations

import argparse
import bisect
import json
import re
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from capstone import Cs

DATA_NAMES = (".rdata", ".data", ".data1")
MAX_STRIDE = 0xFC
MAX_NULL_RUN = 2
GETTER_MAX_INSNS = 24
UNRESOLVED_JOIN = "join"
PARENT = {}
for _p, _names in {
    "eax": ("eax", "ax", "al", "ah"),
    "ecx": ("ecx", "cx", "cl", "ch"),
    "edx": ("edx", "dx", "dl", "dh"),
    "ebx": ("ebx", "bx", "bl", "bh"),
    "esi": ("esi", "si"),
    "edi": ("edi", "di"),
    "ebp": ("ebp", "bp"),
    "esp": ("esp", "sp"),
}.items():
    for _n in _names:
        PARENT[_n] = _p
ARG_REGS = ("eax", "ecx", "edx")
# Record origin hints from docs/t1474-census-weapon-session.md: entry 0 of the weapon entry table
# 0x4FA128 has no pak, so the first named slot (0x4fa17c) is entry 1 and the pak field sits
# at +0x18.
DEFAULT_ORIGIN_HINTS = {0x004FA17C: 0x004FA128}


# ---------------------------------------------------------------- image


@dataclass
class Image:
    """Sections as (name, va, bytes) plus function extents (entry, end) and metadata."""

    sections: list[tuple[str, int, bytes]]
    functions: list[tuple[int, int]]
    skip: set[int] = field(default_factory=set)  # library entries, not analysed
    names: dict[int, str] = field(default_factory=dict)

    def read(self, va: int, size: int) -> bytes:
        for _, base, blob in self.sections:
            if base <= va < base + len(blob):
                return blob[va - base : va - base + size]
        return b""

    def data_sections(self) -> list[tuple[str, int, bytes]]:
        return [s for s in self.sections if s[0] in DATA_NAMES]

    def owner(self, va: int) -> int | None:
        entries = [e for e, _ in self.functions]
        i = bisect.bisect_right(entries, va) - 1
        if i >= 0 and va < self.functions[i][1]:
            return entries[i]
        return None

    @classmethod
    def from_repo(cls, root: Path) -> Image:
        from tools.name_additions import World

        world = World(root)
        sections = []
        for sec in world.xbe.sections:
            sections.append(
                (sec.name, sec.virtual_addr, world.read(sec.virtual_addr, sec.raw_size))
            )
        by_entry = sorted(world.table, key=lambda f: f.entry_va)
        funcs = []
        for i, fn in enumerate(by_entry):
            nxt = by_entry[i + 1].entry_va if i + 1 < len(by_entry) else world.text_hi
            end = min(nxt, max(fn.body_max_va + 1, fn.entry_va + fn.size_bytes))
            funcs.append((fn.entry_va, end))
        return cls(sections, funcs, set(world.library), dict(world.names))


# ---------------------------------------------------------------- tables


@dataclass
class Table:
    slot0: int  # address of the first valid name pointer
    stride: int
    count: int  # valid (named) records
    span: int  # records covered including tolerated null gaps
    slots: list[int]
    names: dict[int, str]  # slot address -> string
    origin: int = 0  # address of record 0 as far as known
    origin_source: str = "first name slot"
    field_off: int = 0
    getters: list[int] = field(default_factory=list)

    @property
    def end(self) -> int:
        return self.slot0 + self.span * self.stride

    def covers(self, addr: int) -> bool:
        return self.slot0 - self.stride < addr < self.slot0 + self.span * self.stride + self.stride

    def record_name(self, index: int) -> str | None:
        return (
            self.names.get(self.origin + self.field_off + index * self.stride)
            if index >= 0
            else None
        )

    def locality(self) -> float:
        addrs = sorted(self.names.values())
        return float(len(addrs))


def printable_string(blob: bytes) -> str | None:
    end = blob.find(b"\0")
    if end < 2 or end > 80:
        return None
    text = blob[:end]
    if any(c < 0x20 or c > 0x7E for c in text):
        return None
    s = text.decode("ascii")
    if sum(ch.isalnum() for ch in s) < 2:
        return None
    if not (s[0].isalnum() or s[0] in "/_\\."):
        return None
    return s


def find_tables(image: Image, min_count: int = 6, max_stride: int = MAX_STRIDE) -> list[Table]:
    secs = image.data_sections()
    str_cache: dict[int, str | None] = {}

    def string_at(ptr: int) -> str | None:
        if ptr in str_cache:
            return str_cache[ptr]
        res = None
        for _, base, blob in secs:
            if base <= ptr < base + len(blob):
                res = printable_string(blob[ptr - base : ptr - base + 82])
                break
        str_cache[ptr] = res
        return res

    def string_like(ptr: int) -> bool:
        return any(base <= ptr < base + len(blob) for _, base, blob in secs)

    words: dict[int, int] = {}
    pslots: dict[int, str] = {}
    for _, base, blob in secs:
        n = len(blob) // 4
        vals = struct.unpack_from(f"<{n}I", blob)
        for i, v in enumerate(vals):
            words[base + 4 * i] = v
            if v and string_at(v) is not None:
                pslots[base + 4 * i] = string_at(v)  # type: ignore[assignment]
    cands: list[Table] = []
    for stride in range(4, max_stride + 1, 4):
        for p in pslots:
            if p - stride in pslots:
                continue
            slots = [p]
            span = 1
            nulls = 0
            k = 1
            while True:
                q = p + k * stride
                if q in pslots:
                    slots.append(q)
                    span = k + 1
                    nulls = 0
                elif (
                    words.get(q, 1) == 0 or string_like(words.get(q, 0))
                ) and nulls < MAX_NULL_RUN:
                    nulls += 1
                else:
                    break
                k += 1
            if len(slots) >= min_count:
                names = {s: pslots[s] for s in slots}
                if len(set(names.values())) * 2 < len(slots):
                    continue
                cands.append(Table(p, stride, len(slots), span, slots, names))
    cands.sort(key=lambda t: (-t.count, t.span - t.count, -t.stride, t.slot0))
    taken: set[int] = set()
    out: list[Table] = []
    for t in cands:
        overlap = sum(1 for s in t.slots if s in taken)
        if overlap * 2 > len(t.slots):
            continue
        out.append(t)
        for s in t.slots:
            taken.add(s)
            # a stride multiple or divisor shares slots, mark neighbours of other fields too
    for t in out:
        t.origin = t.slot0
    out.sort(key=lambda t: t.slot0)
    return out


# ---------------------------------------------------------------- abstract values


@dataclass(frozen=True)
class Aff:
    """const + mult*sym. sym is None for a constant.

    sym strings: argN, r:<reg>, join:<va>, unk:<va>.
    """

    const: int = 0
    mult: int = 0
    sym: str | None = None

    @property
    def is_const(self) -> bool:
        return self.sym is None


def fresh(tag: str, va: int) -> Aff:
    return Aff(0, 1, f"{tag}:{va:x}")


def aff_add(a: Aff, b: Aff, va: int) -> Aff:
    if a.is_const and b.is_const:
        return Aff(a.const + b.const)
    if a.is_const:
        return Aff(a.const + b.const, b.mult, b.sym)
    if b.is_const:
        return Aff(a.const + b.const, a.mult, a.sym)
    if a.sym == b.sym:
        return Aff(a.const + b.const, a.mult + b.mult, a.sym)
    return fresh("unk", va)


def aff_scale(a: Aff, k: int) -> Aff:
    return Aff(a.const * k, a.mult * k, a.sym) if not a.is_const else Aff(a.const * k)


# ---------------------------------------------------------------- code pass


@dataclass
class MemHit:
    fn: int
    va: int
    const: int
    mult: int
    sym: str | None


@dataclass
class CallEvent:
    fn: int
    va: int
    target: int | None
    stack_args: list[Aff]  # last pushed first
    regs: dict[str, Aff]


def _md() -> Cs:
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    return md


def analyse_function(
    md: Cs, image: Image, entry: int, end: int, hit_lo: int, hit_hi: int
) -> tuple[list, list, int, bool]:
    """Return (mem_hits, call_events, insn_count, has_ret) for one function."""
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

    code = image.read(entry, end - entry)
    insns = list(md.disasm(code, entry))
    targets: set[int] = set()
    for ins in insns:
        if ins.group(1) or ins.group(7):  # jump groups
            if ins.operands and ins.operands[0].type == X86_OP_IMM and ins.mnemonic != "call":
                targets.add(ins.operands[0].imm)
    regs: dict[str, Aff] = {}
    stack: list[Aff] = []
    esp_delta = 0
    ebp_delta: int | None = None
    hits: list[MemHit] = []
    calls: list[CallEvent] = []
    has_ret = False

    def reset(va: int, tag: str) -> None:
        nonlocal regs, stack
        regs = {r: fresh(tag, va) for r in ("eax", "ecx", "edx", "ebx", "esi", "edi", "ebp")}
        stack = []

    def entry_state() -> None:
        nonlocal regs
        regs = {r: Aff(0, 1, f"r:{r}") for r in ("eax", "ecx", "edx")}
        for r in ("ebx", "esi", "edi", "ebp"):
            regs[r] = Aff(0, 1, f"r:{r}")

    entry_state()

    def reg_val(name: str) -> Aff:
        return regs.get(PARENT.get(name, name)) or Aff(0, 1, f"r:{name}")

    def mem_addr(op: Any, va: int) -> Aff:
        m = op.mem
        base = m.base
        bname = md.reg_name(base) if base else None
        iname = md.reg_name(m.index) if m.index else None
        if bname in ("esp", "ebp") and not iname:
            delta = esp_delta if bname == "esp" else ebp_delta
            if delta is not None:
                off = m.disp - delta
                if bname == "esp":
                    off = m.disp - esp_delta
                if off >= 4 and off % 4 == 0:
                    return Aff(0, 1, f"arg{(off - 4) // 4}")
                return fresh("stk", va)
            return fresh("stk", va)
        total = Aff(m.disp)
        if bname:
            total = aff_add(total, reg_val(bname), va)
        if iname:
            total = aff_add(total, aff_scale(reg_val(iname), m.scale), va)
        return total

    def note_hit(a: Aff, va: int) -> None:
        if hit_lo <= a.const <= hit_hi and (a.is_const or a.mult in HIT_MULTS):
            hits.append(MemHit(entry, va, a.const, a.mult, a.sym))

    prev_end = False
    for ins in insns:
        va = ins.address
        if va in targets and va != entry:
            reset(va, UNRESOLVED_JOIN)
        elif prev_end and va != entry:
            reset(va, UNRESOLVED_JOIN)
        prev_end = False
        mn = ins.mnemonic
        ops = ins.operands
        written: set[str] = set()
        try:
            _, w = ins.regs_access()
            written = {PARENT.get(md.reg_name(r), md.reg_name(r)) for r in w}
        except Exception:
            written = set()
        handled = False
        # memory operand hit check
        for op in ops:
            if op.type == X86_OP_MEM and mn != "lea":
                note_hit(mem_addr(op, va), va)
        if mn == "lea" and ops[1].type == X86_OP_MEM:
            a = mem_addr(ops[1], va)
            note_hit(a, va)
            dst = PARENT.get(md.reg_name(ops[0].reg))
            if dst:
                regs[dst] = a
            handled = True
        elif (
            mn == "mov"
            and len(ops) == 2
            and ops[0].type == X86_OP_REG
            and md.reg_name(ops[0].reg) in PARENT
            and len(md.reg_name(ops[0].reg)) == 3
        ):
            dst = md.reg_name(ops[0].reg)
            if ops[1].type == X86_OP_IMM:
                regs[dst] = Aff(ops[1].imm & 0xFFFFFFFF)
                note_hit(regs[dst], va) if False else None
                handled = True
            elif ops[1].type == X86_OP_REG:
                src = md.reg_name(ops[1].reg)
                if dst == "ebp" and src == "esp":
                    ebp_delta = esp_delta
                regs[dst] = reg_val(src) if len(src) == 3 else fresh("unk", va)
                handled = True
            elif ops[1].type == X86_OP_MEM:
                a = mem_addr(ops[1], va)
                regs[dst] = (
                    a
                    if a.sym and a.sym.startswith("arg") and a.mult == 1 and a.const == 0
                    else fresh("unk", va)
                )
                handled = True
        elif (
            mn == "xor"
            and len(ops) == 2
            and ops[0].type == X86_OP_REG
            and ops[1].type == X86_OP_REG
            and ops[0].reg == ops[1].reg
        ):
            regs[PARENT.get(md.reg_name(ops[0].reg), "eax")] = Aff(0)
            handled = True
        elif (
            mn in ("add", "sub")
            and len(ops) == 2
            and ops[0].type == X86_OP_REG
            and md.reg_name(ops[0].reg) == "esp"
            and ops[1].type == X86_OP_IMM
        ):
            esp_delta += ops[1].imm if mn == "sub" else -ops[1].imm
            handled = True
        elif (
            mn in ("add", "sub")
            and len(ops) == 2
            and ops[0].type == X86_OP_REG
            and len(md.reg_name(ops[0].reg)) == 3
        ):
            dst = md.reg_name(ops[0].reg)
            if ops[1].type == X86_OP_IMM:
                imm = ops[1].imm if mn == "add" else -ops[1].imm
                regs[dst] = aff_add(reg_val(dst), Aff(imm), va)
                handled = True
            elif ops[1].type == X86_OP_REG and mn == "add" and len(md.reg_name(ops[1].reg)) == 3:
                regs[dst] = aff_add(reg_val(dst), reg_val(md.reg_name(ops[1].reg)), va)
                handled = True
        elif mn == "imul" and ops and ops[0].type == X86_OP_REG:
            dst = md.reg_name(ops[0].reg)
            if len(ops) == 3 and ops[1].type == X86_OP_MEM and ops[2].type == X86_OP_IMM:
                src = mem_addr(ops[1], va)
                regs[dst] = (
                    aff_scale(src, ops[2].imm)
                    if src.sym and src.sym.startswith("arg") and src.mult == 1 and src.const == 0
                    else fresh("unk", va)
                )
                handled = True
            elif len(ops) == 3 and ops[1].type == X86_OP_REG and ops[2].type == X86_OP_IMM:
                regs[dst] = aff_scale(reg_val(md.reg_name(ops[1].reg)), ops[2].imm)
                handled = True
            elif len(ops) == 2 and ops[1].type == X86_OP_IMM:
                regs[dst] = aff_scale(reg_val(dst), ops[1].imm)
                handled = True
        elif (
            mn == "shl"
            and len(ops) == 2
            and ops[0].type == X86_OP_REG
            and ops[1].type == X86_OP_IMM
        ):
            dst = md.reg_name(ops[0].reg)
            regs[dst] = aff_scale(reg_val(dst), 1 << ops[1].imm)
            handled = True
        elif (
            mn in ("inc", "dec")
            and ops
            and ops[0].type == X86_OP_REG
            and len(md.reg_name(ops[0].reg)) == 3
        ):
            dst = md.reg_name(ops[0].reg)
            regs[dst] = aff_add(reg_val(dst), Aff(1 if mn == "inc" else -1), va)
            handled = True
        elif mn == "push":
            esp_delta += 4
            if ops and ops[0].type == X86_OP_IMM:
                v = Aff(ops[0].imm & 0xFFFFFFFF)
                note_hit(v, va)
                stack.append(v)
            elif ops and ops[0].type == X86_OP_REG:
                stack.append(reg_val(md.reg_name(ops[0].reg)))
            else:
                stack.append(fresh("unk", va))
            handled = True
        elif mn == "pop":
            esp_delta -= 4
            if stack:
                stack.pop()
            if ops and ops[0].type == X86_OP_REG:
                regs[PARENT.get(md.reg_name(ops[0].reg), "eax")] = fresh("unk", va)
            handled = True
        elif mn == "call":
            target = ops[0].imm if ops and ops[0].type == X86_OP_IMM else None
            calls.append(
                CallEvent(entry, va, target, list(reversed(stack)), {r: regs[r] for r in ARG_REGS})
            )
            pops = callee_pop_bytes(md, image, target) if target is not None else 0
            esp_delta -= pops
            stack = []
            for r in ARG_REGS:
                regs[r] = fresh("ret", va)
            handled = True
        if mn in ("ret", "retn"):
            has_ret = True
        if not handled:
            for r in written:
                if r == "esp":
                    continue
                regs[r] = fresh("unk", va)
        if mn in ("jmp", "ret", "retn", "int3") or mn.startswith("ud"):
            prev_end = True
    return hits, calls, len(insns), has_ret


HIT_MULTS: set[int] = set()
_POP_CACHE: dict[int, int] = {}


def callee_pop_bytes(md: Cs, image: Image, target: int) -> int:
    if target in _POP_CACHE:
        return _POP_CACHE[target]
    res = 0
    code = image.read(target, 160)
    for ins in md.disasm(code, target):
        if ins.mnemonic in ("ret", "retn"):
            res = ins.operands[0].imm if ins.operands else 0
            break
        if ins.mnemonic in ("jmp", "call") or ins.mnemonic.startswith("j"):
            if ins.mnemonic == "jmp":
                break
    _POP_CACHE[target] = res
    return res


# ---------------------------------------------------------------- getters and sites


@dataclass
class Getter:
    va: int
    table: int  # index into tables
    stride: int
    arg: str  # "arg0".. or "r:ecx"
    const: int
    kind: str  # direct or wrapper
    phase: int = 0  # distance from the getter's record address to the name pointer slot


@dataclass
class Site:
    fn: int
    va: int
    table: int
    kind: str  # call, inline
    getter: int | None
    index: int | None
    reason: str = ""
    string: str | None = None


@dataclass
class Analysis:
    tables: list[Table]
    getters: dict[int, Getter]
    sites: list[Site]
    insn_counts: dict[int, int]


def table_for(tables: list[Table], addr: int, mult: int | None) -> int | None:
    for i, t in enumerate(tables):
        if t.covers(addr) and (mult is None or mult == 0 or mult == t.stride):
            return i
    return None


def analyse(
    image: Image, tables: list[Table], max_rounds: int = 4, hints: dict[int, int] | None = None
) -> Analysis:
    hints = DEFAULT_ORIGIN_HINTS if hints is None else hints
    global HIT_MULTS
    HIT_MULTS = {t.stride for t in tables}
    lo = min(t.slot0 for t in tables) - 0x100 if tables else 0
    hi = max(t.end for t in tables) + 0x100 if tables else 0
    md = _md()
    all_hits: list[MemHit] = []
    all_calls: list[CallEvent] = []
    insn_counts: dict[int, int] = {}
    rets: dict[int, bool] = {}
    for entry, end in image.functions:
        if entry in image.skip:
            continue
        hits, calls, n, has_ret = analyse_function(md, image, entry, end, lo, hi)
        all_hits.extend(hits)
        all_calls.extend(calls)
        insn_counts[entry] = n
        rets[entry] = has_ret
    # origins: smallest getter-like constant, else first name slot
    getters: dict[int, Getter] = {}
    for h in all_hits:
        if h.sym is None or h.mult == 0:
            continue
        if not (h.sym.startswith("arg") or h.sym.startswith("r:")):
            continue
        ti = table_for(tables, h.const, h.mult)
        if ti is None or tables[ti].stride != h.mult:
            continue
        if insn_counts[h.fn] > GETTER_MAX_INSNS or not rets[h.fn]:
            continue
        getters.setdefault(h.fn, Getter(h.fn, ti, h.mult, h.sym, h.const, "direct"))
    for t in tables:
        if t.slot0 in hints:
            t.origin = hints[t.slot0]
            t.field_off = (t.slot0 - t.origin) % t.stride
            t.origin_source = "hint"
    for gv in getters.values():
        t = tables[gv.table]
        t.getters.append(gv.va)
        gv.phase = (t.slot0 - gv.const) % t.stride
        # the getter that reaches the name from the earliest field is the record base view
        if t.origin_source == "hint":
            continue
        if t.origin_source != "getter" or gv.phase > t.field_off:
            t.origin, t.field_off, t.origin_source = gv.const, gv.phase, "getter"
    # wrapper rounds
    by_target: dict[int, list[CallEvent]] = defaultdict(list)
    for c in all_calls:
        if c.target is not None:
            by_target[c.target].append(c)
    for _ in range(max_rounds):
        new: dict[int, Getter] = {}
        for gva, g in list(getters.items()):
            for c in by_target.get(gva, []):
                if c.fn in getters or c.fn in new:
                    continue
                val = arg_value(c, g)
                if (
                    val is not None
                    and val.sym
                    and not val.is_const
                    and val.mult == 1
                    and val.const == 0
                    and (val.sym.startswith("arg") or val.sym.startswith("r:"))
                ):
                    small = insn_counts[c.fn] <= GETTER_MAX_INSNS and rets[c.fn]
                    new[c.fn] = Getter(
                        c.fn,
                        g.table,
                        g.stride,
                        val.sym,
                        g.const,
                        "wrapper" if small else "forwarder",
                        g.phase,
                    )
        if not new:
            break
        getters.update(new)
        for gv in new.values():
            tables[gv.table].getters.append(gv.va)
        for c in all_calls:
            if c.target in new:
                by_target[c.target].append(c)
    sites: list[Site] = []
    for c in all_calls:
        g = getters.get(c.target) if c.target is not None else None
        if g is None:
            continue
        val = arg_value(c, g)
        t = tables[g.table]
        if val is None:
            sites.append(
                Site(c.fn, c.va, g.table, "call", g.va, None, "argument not on the tracked stack")
            )
        elif val.is_const:
            addr = (
                g.const
                + (val.const & 0xFFFFFFFF if val.const < 0x80000000 else val.const - (1 << 32))
                * g.stride
                + g.phase
            )
            sites.append(Site(c.fn, c.va, g.table, "call", g.va, val.const, "", t.names.get(addr)))
        else:
            sites.append(
                Site(
                    c.fn,
                    c.va,
                    g.table,
                    "call",
                    g.va,
                    None,
                    "unresolved "
                    + val.sym.split(":")[0]
                    + (" (join of paths)" if val.sym.startswith(UNRESOLVED_JOIN) else ""),
                )
            )
    for h in all_hits:
        ti = table_for(tables, h.const, h.mult if h.sym else 0)
        if ti is None or h.fn in getters:
            continue
        t = tables[ti]
        if h.sym is None:
            if h.const == t.origin:
                continue  # base load, not a record access
            idx = (h.const - t.origin) // t.stride
            if (h.const - t.origin) % t.stride >= 0:
                sites.append(Site(h.fn, h.va, ti, "inline", None, idx, "", t.record_name(idx)))
        else:
            sites.append(
                Site(
                    h.fn,
                    h.va,
                    ti,
                    "inline",
                    None,
                    None,
                    "unresolved inline index " + h.sym.split(":")[0],
                )
            )
    return Analysis(tables, getters, sites, insn_counts)


def arg_value(c: CallEvent, g: Getter) -> Aff | None:
    if g.arg.startswith("arg"):
        k = int(g.arg[3:])
        return c.stack_args[k] if k < len(c.stack_args) else None
    return c.regs.get(g.arg.split(":")[1])


# ---------------------------------------------------------------- naming helpers


def role_tokens(string: str) -> str:
    base = re.split(r"[\\/]", string)[-1]
    base = re.sub(r"\.[A-Za-z0-9]{1,4}$", "", base)
    base = re.sub(r"%[a-z]", "", base)
    base = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", base)
    base = re.sub(r"[^A-Za-z0-9]+", "_", base).strip("_").lower()
    return base


def string_kind(string: str) -> str:
    low = string.lower()
    if low.endswith(".pak") or "/gun/" in low:
        return "weapon_pak"
    if "/" in low or "\\" in low:
        return "path"
    return "name"


def function_evidence(analysis: Analysis) -> dict[int, dict]:
    out: dict[int, dict] = {}
    for s in analysis.sites:
        d = out.setdefault(s.fn, {"names": {}, "unresolved": 0, "sites": 0})
        t = analysis.tables[s.table]
        if s.index is None:
            d["unresolved"] += 1
            continue
        d["sites"] += 1
        if s.string is not None:
            d["names"].setdefault((t.slot0, s.index), s.string)
    return out


# ---------------------------------------------------------------- cli


def cmd_census(args: argparse.Namespace, image: Image, analysis: Analysis) -> int:
    rows = []
    per_table_sites: dict[int, list[Site]] = defaultdict(list)
    for s in analysis.sites:
        per_table_sites[s.table].append(s)
    for i, t in enumerate(analysis.tables):
        sites = per_table_sites.get(i, [])
        resolved = [s for s in sites if s.index is not None]
        row = {
            "slot0": f"0x{t.slot0:08x}",
            "origin": f"0x{t.origin:08x}",
            "origin_source": t.origin_source,
            "stride": t.stride,
            "field_off": t.field_off,
            "named_records": t.count,
            "span_records": t.span,
            "samples": list(t.names.values())[:4],
            "getters": [f"0x{g:08x}" for g in t.getters],
            "sites_resolved": len(resolved),
            "sites_unresolved": len(sites) - len(resolved),
            "functions": len({s.fn for s in sites}),
        }
        rows.append(row)
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(rows, indent=1))
    shown = [r for r in rows if r["sites_resolved"] or r["sites_unresolved"] or args.all]
    for r in shown:
        print(
            f"{r['slot0']} stride 0x{r['stride']:x} field +{r['field_off']} "
            f"records {r['named_records']}/{r['span_records']} "
            f"getters {len(r['getters'])} sites {r['sites_resolved']} "
            f"(+{r['sites_unresolved']} unresolved) fns {r['functions']} {r['samples'][:2]}"
        )
    print(
        f"{len(analysis.tables)} tables, {len(shown)} with accesses, "
        f"{len(analysis.getters)} getters, {len(analysis.sites)} sites"
    )
    return 0


def cmd_functions(args: argparse.Namespace, image: Image, analysis: Analysis) -> int:
    ev = function_evidence(analysis)
    for fn, d in sorted(ev.items()):
        n = len(d["names"])
        if n < args.min_names or n > args.max_names:
            continue
        label = image.names.get(fn, "")
        print(
            f"0x{fn:08x} {label} unresolved={d['unresolved']} "
            + "; ".join(f"{a:#x}[{i}]={s}" for (a, i), s in d["names"].items())
        )
    return 0


def name_status(image: Image, fn: int) -> str:
    name = image.names.get(fn)
    if name is None or name.startswith(("FUN_", "SUB_")):
        return "unnamed"
    from tools.llm_name_pipeline import address_shaped

    return "address_shaped" if address_shaped(name) else "named"


def cmd_candidates(args: argparse.Namespace, image: Image, analysis: Analysis) -> int:
    ev = function_evidence(analysis)
    counts: dict[str, int] = defaultdict(int)
    rows = []
    for fn, d in sorted(ev.items()):
        if fn in image.skip:
            continue
        st = name_status(image, fn)
        n = len(d["names"])
        bucket = "none" if n == 0 else ("1-2" if n <= 2 else "many")
        counts[f"{st}/{bucket}"] += 1
        if st != "named" and n and (args.all or n <= args.max_names):
            rows.append((fn, st, d))
    for k in sorted(counts):
        print(k, counts[k])
    if args.list:
        for fn, st, d in rows:
            print(
                f"0x{fn:08x} {st} unres={d['unresolved']} "
                + "; ".join(f"[{i}]={x}" for (_, i), x in d["names"].items())
            )
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=Path("."))
    ap.add_argument("--min-count", type=int, default=6)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("census")
    c.add_argument("--json")
    c.add_argument("--all", action="store_true")
    f = sub.add_parser("functions")
    f.add_argument("--min-names", type=int, default=1)
    f.add_argument("--max-names", type=int, default=2)
    k = sub.add_parser("candidates")
    k.add_argument("--max-names", type=int, default=2)
    k.add_argument("--all", action="store_true")
    k.add_argument("--list", action="store_true")
    args = ap.parse_args(argv)
    image = Image.from_repo(args.root)
    tables = find_tables(image, args.min_count)
    analysis = analyse(image, tables)
    return {"census": cmd_census, "functions": cmd_functions, "candidates": cmd_candidates}[
        args.cmd
    ](args, image, analysis)


if __name__ == "__main__":
    sys.exit(main())
