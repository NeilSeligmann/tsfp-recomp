# SPDX-License-Identifier: GPL-3.0-or-later
"""T728: pair the worker's global-selected object with its vtable slot conservatively.

The candidate site is ``0x2a34c`` (``call [ecx+4]``). Its object pointer comes from
``[0x665470]``, populated by ``0x2a2d0`` from ``0x476270 + 16 * index``. A pool may be
intersected with those record slots only when every selector caller, writer, and input is
accounted for. The real image currently fails that proof: the path reading ``[arg1+0x7c]``
accepts every nonnegative signed word and does not check the upper bound 54. The sibling
``[ecx+8]`` path is guarded to 0..54, but that does not close the first path.

This module does not infer a type from the table's contents or narrow from a missing writer.
"""

from __future__ import annotations

from dataclasses import dataclass

import capstone
from capstone import x86 as cs_x86

from tools.tracegaps.code import Code
from tools.tracegaps.flow import ValueSet
from tools.tracegaps.pools import Report
from tools.tracegaps.reach import ReachGraph

SELECTOR_SITE = 0x2A34C
SELECTOR_FUNCTION = 0x2A2D0
OBJECT_GLOBAL = 0x665470
OBJECT_TABLE = 0x476270
OBJECT_STRIDE = 0x10
OBJECT_METHOD_DISP = 4
MAX_OBJECT_INDEX = 0x36
CALLER_FUNCTION = 0x18EA90
CALLER_CALLSITE = 0x8F653
OPEN_FIELD_LOAD = 0x18F28E
BOUNDED_FIELD_LOAD = 0x18F2B4


@dataclass(frozen=True)
class SelectorInput:
    """One selector invocation and the abstract input values reaching its push."""

    callsite: int
    values: frozenset[int]
    open_reasons: tuple[str, ...] = ()


@dataclass(frozen=True)
class PoolDecision:
    before: tuple[int, ...]
    after: tuple[int, ...]
    indices: tuple[int, ...]
    reason: str

    @property
    def narrowed(self) -> bool:
        return self.after != self.before


@dataclass(frozen=True)
class SelectorReport:
    decision: PoolDecision
    callsites: tuple[int, ...]
    global_writes: tuple[tuple[int, str], ...]
    writer_inventory_complete: bool
    selector_refs: tuple[str, ...]
    open_fields: tuple[str, ...]
    caller_chain: tuple[str, ...]


def _signed32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def pair_pool(
    pool: tuple[int, ...],
    inputs: tuple[SelectorInput, ...],
    *,
    callers_complete: bool,
    writers_complete: bool,
    references_complete: bool,
    table_base: int = OBJECT_TABLE,
    stride: int = OBJECT_STRIDE,
    method_disp: int = OBJECT_METHOD_DISP,
    max_index: int = MAX_OBJECT_INDEX,
) -> PoolDecision:
    """Narrow only if the whole selector/write/reference inventory is explicitly closed."""
    if not callers_complete or not inputs:
        return PoolDecision(pool, pool, (), "refused: selector caller inventory is incomplete")
    indices: set[int] = set()
    for item in inputs:
        if item.open_reasons:
            reasons = "; ".join(item.open_reasons)
            return PoolDecision(
                pool,
                pool,
                (),
                f"refused: selector input at {item.callsite:#x} is unbounded: {reasons}",
            )
        for raw in item.values:
            value = _signed32(raw)
            if value < 0:
                # The verified setter clears the global pointer for signed-negative input.
                continue
            if value > max_index:
                return PoolDecision(
                    pool,
                    pool,
                    (),
                    f"refused: selector index {value} exceeds proved table bound {max_index}",
                )
            indices.add(value)

    if not references_complete:
        return PoolDecision(pool, pool, (), "refused: selector has an unaccounted reference")
    if not writers_complete:
        return PoolDecision(pool, pool, (), "refused: global object writer inventory is incomplete")

    slots = {table_base + stride * index + method_disp for index in indices}
    after = tuple(address for address in pool if address in slots)
    return PoolDecision(
        pool,
        after,
        tuple(sorted(indices)),
        f"paired against {len(indices)} proved selector indices",
    )


def _store_inventory(code: Code, graph: ReachGraph) -> tuple[tuple[tuple[int, str], ...], bool]:
    writes: list[tuple[int, str]] = []
    references_closed = True
    for insn in code.insns:
        for operand in insn.operands:
            if operand.type != cs_x86.X86_OP_MEM:
                continue
            mem = operand.mem
            disp = mem.disp & 0xFFFFFFFF
            if disp != OBJECT_GLOBAL:
                continue
            if not operand.access & capstone.CS_AC_WRITE:
                continue
            direct = mem.base == 0 and mem.index == 0 and operand.size == 4
            if direct:
                writes.append((insn.address, insn.op_str))
            else:
                references_closed = False
                writes.append((insn.address, f"UNRESOLVED alias: {insn.op_str}"))

    # Encoded address-taking references (immediates/data/table entries) are not writers we
    # can pair with this global. Do not treat them as absent merely because the store scan
    # saw no direct instruction.
    hits = graph.address_hits([OBJECT_GLOBAL]).get(OBJECT_GLOBAL, [])
    for hit in hits:
        if hit.kind in {"data", "table", "immediate"}:
            references_closed = False
    return tuple(sorted(set(writes))), references_closed


def _input_values(
    code: Code, graph: ReachGraph
) -> tuple[tuple[SelectorInput, ...], bool, tuple[str, ...]]:
    transfers = code.transfers().get(SELECTOR_FUNCTION, [])
    inputs: list[SelectorInput] = []
    refs = graph.address_hits([SELECTOR_FUNCTION]).get(SELECTOR_FUNCTION, [])
    # Direct relative calls are present in transfers and are excluded by address_hits. Any
    # absolute/data/table reference could be an additional indirect caller and keeps the set open.
    unexpected = tuple(
        f"{hit.kind} reference at {hit.where:#x}"
        for hit in refs
        if hit.kind not in {"branch", "orphan"}
    )
    for transfer in transfers:
        if transfer.mnemonic != "call":
            inputs.append(
                SelectorInput(transfer.address, frozenset(), ("tail-jump arguments are unbounded",))
            )
            continue
        try:
            value: ValueSet = graph.call_arguments(transfer.address, 1)[0]
        except LookupError as error:
            inputs.append(SelectorInput(transfer.address, frozenset(), (str(error),)))
            continue
        inputs.append(SelectorInput(transfer.address, value.values, value.unbounded))
    return tuple(inputs), not unexpected, unexpected


def _field_provenance(code: Code, graph: ReachGraph) -> tuple[tuple[str, ...], tuple[str, ...]]:
    fields: list[str] = []
    chain: list[str] = []
    expected = {
        0x18F28E: ("mov", "ax, word ptr [edx + 0x7c]"),
        0x18F2B4: ("mov", "ax, word ptr [ecx + 8]"),
    }
    for address, (mnemonic, op_str) in expected.items():
        insn = code.insn_at(address)
        if insn is None or (insn.mnemonic, insn.op_str) != (mnemonic, op_str):
            fields.append(f"instruction drift at {address:#x}; selector field path unproved")
        else:
            fields.append(f"0x{address:08x}: {insn.mnemonic} {insn.op_str}")

    # Pin the signed-positive, unchecked path and its sibling's actual guard. If any of these
    # anchors drift, the analysis refuses instead of assuming the same control flow.
    anchors = {
        0x18F295: ("mov", "ebx, dword ptr [esp + 0x14]"),
        0x18F29D: ("mov", "dword ptr [esp + 0x10], 0"),
        0x18F2A5: ("jl", "0x18f2b0"),
        0x18F2A7: ("movsx", "eax, ax"),
        0x18F2AA: ("mov", "dword ptr [esp + 0x10], eax"),
        0x18F2B8: ("test", "ax, ax"),
        0x18F2BB: ("jl", "0x18f2ca"),
        0x18F2BD: ("cmp", "ax, 0x37"),
        0x18F2C1: ("jge", "0x18f2ca"),
        0x18F2C3: ("movsx", "edx, ax"),
        0x18F2C6: ("mov", "dword ptr [esp + 0x10], edx"),
    }
    for address, expected_insn in anchors.items():
        insn = code.insn_at(address)
        if insn is None or (insn.mnemonic, insn.op_str) != expected_insn:
            fields.append(f"instruction drift at {address:#x}; index bound unproved")

    flow = graph.flows.at_entry(CALLER_FUNCTION)
    load = code.insn_at(0x18F46C)
    frame = flow.frame_offset(0x18F46C, load.operands[-1]) if load else None
    if frame is None:
        fields.append("selector local stack slot could not be recovered")
    else:
        definitions = flow.definitions("slot", 0x18F6C1, frame)
        addresses = {definition.address for definition in definitions}
        if addresses != {0x18F29D, 0x18F2AA, 0x18F2C6}:
            fields.append(
                f"selector local definitions changed: {sorted(hex(a) for a in addresses)}"
            )
        else:
            fields.append(
                "slot -0x218 before selector push is written by 0x18f29d/0x18f2aa/0x18f2c6"
            )
            # The positive side of [arg1+0x7c] has no upper comparison before its store. The
            # second field is clamped by cmp ax,0x37; values >=55 branch to the zero default.
            chain.extend(
                (
                    "0x18ea90 arg1 -> 0x18f28e [arg1+0x7c] (signed word)",
                    "nonnegative branch -> movsx at 0x18f2a7 -> selector slot store at 0x18f2aa",
                    "no upper-bound compare on that branch; 55..32767 can reach 0x18f6c2",
                    "sibling [ecx+8] at 0x18f2b4 is limited to 0..54 by cmp ax,0x37 / jge",
                )
            )
    callers = code.transfers().get(CALLER_FUNCTION, [])
    if len(callers) != 1 or callers[0].address != CALLER_CALLSITE:
        fields.append("0x18ea90 direct caller inventory changed")
    else:
        args = graph.call_arguments(CALLER_CALLSITE, 4)
        chain.append(
            "0x18f6c2 is in 0x18ea90; its only direct caller 0x8f653 passes arg1 from "
            + args[0].describe()
            + " of 0x8f5e0"
        )
    return tuple(fields), tuple(chain)


def analyse(code: Code, report: Report) -> SelectorReport:
    verdict = next(
        (
            item
            for values in report.starts.values()
            for item in values
            if item.site.address == SELECTOR_SITE
        ),
        None,
    )
    if verdict is None:
        decision = PoolDecision((), (), (), "refused: selector site is absent from the pool report")
        return SelectorReport(decision, (), (), False, (), ("site missing",), ())
    graph = report._graph
    if graph is None:
        decision = PoolDecision(
            verdict.pool, verdict.pool, (), "refused: reach graph was not retained"
        )
        return SelectorReport(decision, (), (), False, (), ("reach graph missing",), ())

    inputs, references_complete, selector_refs = _input_values(code, graph)
    writes, address_refs_complete = _store_inventory(code, graph)
    fields, chain = _field_provenance(code, graph)
    # The scan enumerates direct exact-address stores and overlapping displacements, but it
    # cannot rule out a pointer alias held in a register. Keep that closure explicitly open.
    writers_complete = False
    decision = pair_pool(
        verdict.pool,
        inputs,
        callers_complete=references_complete and bool(inputs),
        writers_complete=writers_complete,
        references_complete=references_complete,
    )
    if not decision.narrowed and decision.reason.startswith("paired"):
        decision = PoolDecision(
            decision.before,
            decision.after,
            decision.indices,
            "refused: no candidate pool slot is paired with the proven selector records",
        )
    callsites = tuple(item.callsite for item in inputs)
    if not address_refs_complete:
        fields += ("decoded 0x665470 write/reference alias requires review",)
    fields += ("writer closure open: exact-address scan cannot exclude arbitrary pointer aliases",)
    return SelectorReport(
        decision, callsites, writes, writers_complete, selector_refs, fields, chain
    )


def render(report: SelectorReport) -> str:
    decision = report.decision
    lines = [
        (
            f"  selector pairing 0x{SELECTOR_SITE:08x}: pool {len(decision.before)} -> "
            f"{len(decision.after)}; {decision.reason}"
        ),
        (
            f"    selector direct callers: {len(report.callsites)}; global "
            f"0x{OBJECT_GLOBAL:08x} decoded writes: {len(report.global_writes)}"
        ),
        f"    global writer inventory complete: {report.writer_inventory_complete}",
    ]
    lines.extend(f"    write 0x{address:08x}: {op_str}" for address, op_str in report.global_writes)
    lines.extend(f"    open/reference: {reference}" for reference in report.selector_refs)
    lines.extend(f"    field evidence: {field}" for field in report.open_fields)
    lines.extend(f"    caller path: {item}" for item in report.caller_chain)
    return "\n".join(lines)
