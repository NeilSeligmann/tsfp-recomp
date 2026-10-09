# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T603 part (c): saturate a thread start's closure by resolving its indirect sites with evidence.

`ReachGraph.closure` stops at every indirect site it cannot read (`Closure.unresolved`). Here each
site is resolved by the stored pointers that can reach it, one RULE at a time, and every resolved
target is added to the closure until nothing new resolves (the SATURATED closure). A site no rule
resolves stays in the residue, so a start CLOSES only when its saturated closure holds no
unresolved site, no callback import it cannot explain and none of the writers.

RULES (a site is resolved by the first rule whose conditions hold, the rule is reported).
  jump-table   `jmp [reg*4 + base]` with a bounded table (`addresstaking.jump_table`): the targets are
               the slots the bound allows, all decoded instructions of the jump's section.
  literal      the operand's value set from `Flow` is bounded (a `mov reg, imm`, a frame slot, a
               literal argument substituted by nobody else): the literals are the targets.
  import       every definition of the register is a load of a kernel import slot (`graph.imports`)
               that no guest instruction writes: the call enters the kernel, no guest code runs from it
               (the callback services are reported apart, see `Saturated.callback_imports`).
  global slot  every definition is a load of one global dword `[S]` whose address is only named by
               plain operands (no immediate, stored pointer or table dword holds it, no indexed
               write names it), whose stores are all `mov [S], imm` or a register `Flow` bounds, plus
               its initial value if that is code. The pool of the slot is the target set. A slot
               nothing writes holds only its initial value.
  registry     the T424 registrations the caller passes in (`resolutions`), kept as claims.

WHAT IT NEVER DOES. A `call [reg + disp]` through a pointer another function loaded (a vtable or a
record field), a register loaded from such a field, and an argument no one bounded stay UNRESOLVED:
the fields every struct shares (`[eax]`, `[eax + 4]`) are written with arbitrary values all over the
image, so no pool for them is sound without types. That is the residue the report prints.

ASSUMPTIONS (named in the report): a global dword is read and written only through operands that
name its address (no array whose base is below it and no computed pointer), a kernel import slot is
written by the loader only, and a literal that is not a decoded instruction is no call target.
"""

from __future__ import annotations

import re
from collections import Counter
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field, replace

import capstone
from capstone import x86 as cs_x86

from tools.tracegaps import addresstaking
from tools.tracegaps.flow import Flow, ValueSet
from tools.tracegaps.reach import CALLBACK_IMPORTS, Closure, ReachGraph, Site

SLOT_REASON = re.compile(r"load \[(0x[0-9a-f]+)\] at (0x[0-9a-f]+)")

RULE_JUMP_TABLE = "jump-table"
RULE_LITERAL = "literal"
RULE_IMPORT = "import"
RULE_SLOT = "global-slot"
RULE_REGISTRY = "registry"
#: Kernel services whose third argument is the APC routine (`NtReadFile`, `NtWriteFile`).
APC_ARGUMENT = {"NtReadFile": 3, "NtWriteFile": 3}


@dataclass(frozen=True)
class Resolution:
    """One resolved indirect site: its targets and the rule and evidence behind them."""

    site: int
    targets: tuple[int, ...]
    rule: str
    #: kernel services entered instead of guest code (the `import` rule), empty otherwise
    kernel: tuple[str, ...] = ()
    #: the global slots read (the `global-slot` and `import` rules)
    slots: tuple[int, ...] = ()


@dataclass
class Saturated:
    """The fixed point of a start's closure under the resolver."""

    start: int
    closure: Closure
    resolutions: dict[int, Resolution]
    #: callback service call sites whose routine argument is not proven null
    callback_imports: list[Site]
    #: callback service call sites proven to pass a null routine
    callback_nulls: list[Site] = field(default_factory=list)

    @property
    def unresolved(self) -> list[Site]:
        return self.closure.unresolved

    def reaches(self, functions: Sequence[int]) -> list[int]:
        return [function for function in functions if function in self.closure.parent]

    def closed(self, writers: Sequence[int]) -> bool:
        return not self.unresolved and not self.callback_imports and not self.reaches(writers)

    def by_rule(self) -> Counter[str]:
        return Counter(resolution.rule for resolution in self.resolutions.values())


class SiteResolver:
    """Resolves the indirect sites of a graph by the rules above, with memoised slot pools."""

    def __init__(self, graph: ReachGraph) -> None:
        self.graph = graph
        self.code = graph.code
        self._pools: dict[int, ValueSet | None] = {}
        self._operands: dict[int, list[tuple[int, str, bool]]] | None = None
        self._tables = addresstaking.TableRules(graph)

    # ------------------------------------------------------------------ operands

    def _memory_operands(self) -> dict[int, list[tuple[int, str, bool]]]:
        """displacement -> [(instruction, shape, writes)] for every memory operand, one pass.

        `shape` is "absolute" (no base, no index) or "indexed".
        """
        if self._operands is None:
            found: dict[int, list[tuple[int, str, bool]]] = {}
            for insn in self.code.insns:
                for operand in insn.operands:
                    if operand.type != cs_x86.X86_OP_MEM:
                        continue
                    mem = operand.mem
                    shape = "absolute" if mem.base == 0 and mem.index == 0 else "indexed"
                    found.setdefault(mem.disp & 0xFFFFFFFF, []).append(
                        (insn.address, shape, bool(operand.access & capstone.CS_AC_WRITE))
                    )
            self._operands = found
        return self._operands

    def flow_at(self, address: int) -> Flow | None:
        for owner in sorted(self.graph.enclosing_entries(address), reverse=True):
            try:
                flow = self.graph.flows.at_entry(owner)
            except KeyError:
                continue
            if address in flow.insns:
                return flow
        return None

    # ------------------------------------------------------------------ slots

    def slot_pool(self, slot: int) -> ValueSet | None:
        """The code addresses the global dword at `slot` can hold, or None when it is not closed.

        Closed means: no immediate, stored dword or table dword holds the slot's address, no
        indexed operand writes it, every store is a `mov [slot], imm` or a register `Flow` bounds.
        """
        if slot not in self._pools:
            self._pools[slot] = self._slot_pool(slot)
        return self._pools[slot]

    def _slot_pool(self, slot: int) -> ValueSet | None:
        image = self.code.image
        for hits in self.graph.address_hits([slot]).values():
            for hit in hits:
                if hit.kind in ("immediate", "data", "table"):
                    return None
        values: set[int] = set()
        initial = image.u32(slot)
        if initial is not None:
            values.add(initial)
        for address, shape, writes in self._memory_operands().get(slot, ()):
            if not writes:
                continue
            if shape != "absolute":
                return None
            store = self._stored_value(address, slot)
            if store is None or not store.bounded:
                return None
            values |= store.values
        return ValueSet(frozenset(v for v in values if self.graph.is_code(v)))

    def _stored_value(self, address: int, slot: int) -> ValueSet | None:
        insn = self.code.insn_at(address)
        if insn is None or insn.mnemonic != "mov" or len(insn.operands) != 2:
            return None
        destination, source = insn.operands
        if destination.type != cs_x86.X86_OP_MEM or (destination.mem.disp & 0xFFFFFFFF) != slot:
            return None
        if destination.size != 4:
            return None
        if source.type == cs_x86.X86_OP_IMM:
            return ValueSet(frozenset({source.imm & 0xFFFFFFFF}))
        flow = self.flow_at(address)
        if flow is None:
            return None
        return flow.operand_value(insn, source)

    # ------------------------------------------------------------------ values

    def _value_set(self, site: Site) -> ValueSet | None:
        """The value the site transfers to, as `Flow` bounds it, or None when it cannot ask."""
        insn = self.code.insn_at(site.address)
        if insn is None or len(insn.operands) != 1:
            return None
        operand = insn.operands[0]
        if operand.type == cs_x86.X86_OP_MEM and operand.mem.base == 0 and operand.mem.index == 0:
            pool = self.slot_pool(operand.mem.disp & 0xFFFFFFFF)
            return None if pool is None else pool
        flow = self.flow_at(site.address)
        if flow is None:
            return None
        if operand.type == cs_x86.X86_OP_MEM:
            frame = flow.frame_offset(site.address, operand)
            return None if frame is None else flow.slot_before(site.address, frame)
        return flow.operand_value(insn, operand)

    def resolve(self, site: Site) -> Resolution | None:
        """The resolution of `site`, or None (the site stays in the residue)."""
        if site.kind == "jump":
            table = self._tables.jump(site.address)
            if isinstance(table, addresstaking.JumpTable):
                return Resolution(site.address, table.targets, RULE_JUMP_TABLE)
        value = self._value_set(site)
        if value is None:
            return None
        targets = set(value.values)
        kernels: list[str] = []
        slots: list[int] = []
        for reason in value.unbounded:
            match = SLOT_REASON.fullmatch(reason)
            if match is None:
                return None
            slot = int(match.group(1), 16)
            slots.append(slot)
            name = self.graph.imports.get(slot)
            if name is not None:
                if self._writers_of(slot):
                    return None
                kernels.append(name)
                continue
            pool = self.slot_pool(slot)
            if pool is None:
                return None
            targets |= pool.values
        rule = RULE_LITERAL
        if self._names_a_slot(site):
            rule = RULE_SLOT
        elif slots:
            rule = RULE_IMPORT if kernels and len(kernels) == len(slots) else RULE_SLOT
        return Resolution(
            site.address,
            tuple(sorted(target for target in targets if self.graph.is_code(target))),
            rule,
            tuple(dict.fromkeys(kernels)),
            tuple(dict.fromkeys(slots)),
        )

    def _names_a_slot(self, site: Site) -> bool:
        """Whether the site is a `call [S]` / `jmp [S]` through a plain global dword."""
        insn = self.code.insn_at(site.address)
        if insn is None or len(insn.operands) != 1:
            return False
        operand = insn.operands[0]
        return (
            operand.type == cs_x86.X86_OP_MEM and operand.mem.base == 0 and operand.mem.index == 0
        )

    def _writers_of(self, slot: int) -> list[int]:
        return [address for address, _, writes in self._memory_operands().get(slot, ()) if writes]

    # ------------------------------------------------------------------ callback services

    def null_routine(self, site: Site) -> bool:
        """Whether a call of a callback service passes a literal null routine (the APC argument)."""
        position = APC_ARGUMENT.get(site.kernel or "")
        if position is None:
            return False
        try:
            values = self.graph.call_arguments(site.address, position)
        except LookupError:
            return False
        value = values[position - 1]
        return value.bounded and value.values == frozenset({0})


def saturate(
    graph: ReachGraph,
    start: int,
    resolver: SiteResolver | None = None,
    claims: Mapping[int, Sequence[int]] | None = None,
) -> Saturated:
    """Resolve every site of the closure of `start` the rules can, until nothing new resolves."""
    resolver = resolver or SiteResolver(graph)
    resolutions: dict[int, Resolution] = {}
    sites: dict[int, Site] = {}
    for address, targets in (claims or {}).items():
        resolutions[address] = Resolution(address, tuple(targets), RULE_REGISTRY)
    while True:
        closure = graph.closure(
            [start], {address: resolution.targets for address, resolution in resolutions.items()}
        )
        added = False
        for site in closure.unresolved:
            if site.address in resolutions:
                continue
            resolution = resolver.resolve(site)
            if resolution is not None:
                resolutions[site.address] = resolution
                sites[site.address] = site
                added = True
        if not added:
            break
    # A callback service entered through a register (the `import` rule) is a callback import too.
    entered = [
        replace(sites[address], kernel=name)
        for address, resolution in resolutions.items()
        if address in sites
        for name in resolution.kernel
        if name in CALLBACK_IMPORTS
    ]
    callbacks = [*closure.callback_imports, *entered]
    nulls = [site for site in callbacks if resolver.null_routine(site)]
    rest = [site for site in callbacks if site not in nulls]
    return Saturated(start, closure, resolutions, rest, nulls)
