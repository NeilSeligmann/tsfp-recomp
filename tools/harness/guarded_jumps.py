# SPDX-License-Identifier: GPL-3.0-or-later
"""T1576 source-only jump plans (static planning scaffold, still never an admission).

A decoded static plan is not actual arm/tail evidence, caller eligibility or runtime table
immutability. The opt-in runtime path (guarded_tables prover, oracle arm events, witness session,
typed tail adapter, closure scan) lives in tools/harness/guarded_tables.py,
tools/replace/guarded_jump_session.py and tools/replace/tail_adapter.py; see
docs/evidence/t1576/guard-contract.md and docs/evidence/t1576/wiring.md.
"""

import hashlib
from collections.abc import Callable
from dataclasses import asdict, dataclass

from .callstub import CalleeConventions, StubPlan, _decoder, plan_calls
from .image import SourceSection
from .jumps import direct_jump_target
from .static_tables import StaticJumpTable, prove_static_tables

STATIC_SCHEMA = 1


@dataclass(frozen=True)
class TablePlan:
    original: StaticJumpTable
    comparison: int
    rejection: int
    bound: int
    default: int
    table_sha256: str
    source_readonly: bool


@dataclass(frozen=True)
class TailPlan:
    site: int
    target: int
    target_size: int
    target_sha256: str
    cleanup_bytes: int


@dataclass(frozen=True)
class StaticPlan:
    root: int
    size: int
    body_sha256: str
    tables: tuple[TablePlan, ...]
    tails: tuple[TailPlan, ...]
    calls: StubPlan

    def document(self) -> dict:
        return {
            "schema": STATIC_SCHEMA,
            "protocol": "t1576-static-planning-only-v1",
            "admission": "DISABLED: authenticated original/replacement runtime API pending",
            "root": self.root,
            "size": self.size,
            "body_sha256": self.body_sha256,
            "tables": [asdict(table) for table in self.tables],
            "tails": [asdict(tail) for tail in self.tails],
            "calls": asdict(self.calls),
            "runtime_requirements": [
                "actual per-slot/default/tail equality and successful complete coverage",
                "all-table pre-case/dispatch byte rehash",
                "typed exact plan_calls tail target and full8GPR includingESP/EBP",
                "same guest fault kind/address; matched faults nonverdict",
                "strict producer/CLI/wire/schema3 reader/merge and named runtime mutant kills",
            ],
        }

    def require_admission(self) -> None:
        raise ValueError("T1576 admission disabled: actual paired runtime guard API unavailable")


def analyze_node(
    va: int,
    code: bytes,
    *,
    sizes: dict[int, int],
    read_code: Callable[[int, int], bytes],
    sections: tuple[SourceSection, ...],
) -> StaticPlan:
    """Analyze original instruction bytes without enabling closure/selection/proof."""
    if type(sections) is not tuple or any(type(s) is not SourceSection for s in sections):
        raise ValueError("immutable physical section metadata required")
    if any(
        a.address < b.address + b.size and b.address < a.address + a.size
        for n, a in enumerate(sections)
        for b in sections[:n]
    ):
        raise ValueError("ambiguous overlapping physical sections")
    size = len(code)
    if sizes.get(va) != size or not size or len(code) != len(read_code(va, size)):
        raise ValueError("exact original root bound/read required")
    if read_code(va, size) != code:
        raise ValueError("original root bytes differ")
    decoder = _decoder()
    instructions = list(decoder.disasm(code, va))
    if sum(i.size for i in instructions) != size:
        raise ValueError("complete original decode required")
    boundaries = {i.address for i in instructions}
    # Retain strict adjacent guard and all bypass/index bounds, permitting calls
    # only elsewhere; caller and closure eligibility remain independent.
    tables = prove_static_tables(va, size, code, read_code, allow_calls=True)
    # The legacy prover rejects out-of-body branches. For a tail-bearing node,
    # retain that refusal here; tail-only planning is handled separately below.
    tails = []
    indirect = any(i.mnemonic == "jmp" and direct_jump_target(i) is None for i in instructions)
    if indirect and tables is None:
        raise ValueError("unproved static table/index/default/bypass guard")
    conventions = CalleeConventions(sizes, read_code, decoder)
    calls = plan_calls(va, size, instructions, conventions)
    if not isinstance(calls, StubPlan):
        raise ValueError("original direct call plan refused: " + calls)
    planned_tables = []
    for table in tables or ():
        position = next(n for n, i in enumerate(instructions) if i.address == table.site)
        comparison, rejection = instructions[position - 2 : position]
        lo, hi = table.address, table.address + len(table.original_bytes)
        owners = [s for s in sections if s.address <= lo < hi <= s.address + s.size]
        if len(owners) != 1:
            raise ValueError("unique complete physical table section required")
        planned_tables.append(
            TablePlan(
                table,
                comparison.address,
                rejection.address,
                comparison.operands[1].imm,
                rejection.operands[0].imm,
                hashlib.sha256(table.original_bytes).hexdigest(),
                not owners[0].writable,
            )
        )
    for insn in instructions:
        target = direct_jump_target(insn)
        if target is None:
            continue
        if va <= target < va + size:
            if target not in boundaries:
                raise ValueError("in-body jump enters instruction interior")
            continue
        if target not in sizes or sizes[target] <= 0:
            raise ValueError("tail requires exact known original entry")
        target_code = read_code(target, sizes[target])
        cleanup = conventions.pop_bytes(target)
        if len(target_code) != sizes[target] or cleanup is None:
            raise ValueError("tail target bound/cleanup unproven")
        tails.append(
            TailPlan(
                insn.address,
                target,
                len(target_code),
                hashlib.sha256(target_code).hexdigest(),
                cleanup,
            )
        )
    if not planned_tables and not tails:
        raise ValueError("no guarded jump planning sites")
    return StaticPlan(
        va, size, hashlib.sha256(code).hexdigest(), tuple(planned_tables), tuple(tails), calls
    )
