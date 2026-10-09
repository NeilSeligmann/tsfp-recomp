# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded v2 original-byte analysis; shared roots preserve caller/callee ABI mapping.

This is input steering, never a substitute for executing the original closure. No
allocator is stubbed and no return value is fabricated. Unknown calls remain unknown.
"""

from __future__ import annotations

import hashlib
from collections.abc import Callable
from typing import Any

from .synth_access import (
    ESP0,
    AccessSummary,
    Lin,
    _join,
    _same,
    _State,
    _Walker,
    const_lin,
    lin_add,
)

VERSION = "t1773-synth-v2"
POLICY = {
    "path_instruction_limit": 60,
    "path_block_visits": 2,
    "state_cap": 256,
    "call_depth": 8,
    "instruction_budget": 12000,
}


class _Context:
    def __init__(
        self, summary: AccessSummary, read: Callable[[int, int], bytes], sizes: dict[int, int]
    ) -> None:
        self.summary = summary
        self.read = read
        self.sizes = sizes
        self.codes: dict[int, dict[str, Any]] = {}
        self.budget = POLICY["instruction_budget"]


class _Interproc(_Walker):
    def __init__(self, va: int, code: bytes, context: _Context, ancestors: tuple[int, ...]) -> None:
        super().__init__(va, code)
        self.context = context
        self.ancestors = ancestors
        self.decode()
        context.summary.limits.extend(self.summary.limits)
        self.summary = context.summary
        context.codes[va] = {
            "va": f"{va:#010x}",
            "size": len(code),
            "sha256": hashlib.sha256(code).hexdigest(),
        }

    def execute(self, state: _State, insn: Any) -> list[_State]:
        if insn.mnemonic != "call":
            self.step(state, insn)
            return [state]
        self.summary.calls += 1
        target = self._jump_target(insn)
        reason = None
        if target is None:
            self.summary.indirect += 1
            reason = "indirect-call"
        elif target in self.ancestors or target == self.va:
            reason = "recursive-call"
        elif len(self.ancestors) >= POLICY["call_depth"]:
            reason = "call-depth-cap"
        elif target not in self.context.sizes:
            reason = "unknown-callee"
        if reason is None:
            size = self.context.sizes[target]
            code = self.context.read(target, size)
            if len(code) != size or not code:
                reason = "callee-bytes-unavailable"
        if reason is not None:
            self.summary.limits.append(reason)
            for reg in ("eax", "ecx", "edx"):
                state.regs[reg] = None
            state.written.clear()
            return [state]
        child = state.copy()
        # CALL puts a return address below the already pushed caller arguments.
        child.regs["esp"] = lin_add(self.reg_get(child, "esp"), const_lin(-4))
        slot = self._stack_slot(child.regs["esp"]) if child.regs["esp"] else None
        if slot is not None:
            child.stack[slot] = const_lin(insn.address + insn.size)
        walker = _Interproc(target, code, self.context, self.ancestors + (self.va,))
        returns = walker.paths(child)
        if returns:
            return returns
        self.summary.limits.append("callee-no-return")
        for reg in ("eax", "ecx", "edx", "esp"):
            state.regs[reg] = None
        state.written.clear()
        return [state]

    def paths(self, entry: _State) -> list[_State]:
        blocks, succ = self.build_blocks()
        if self.va not in blocks:
            self.summary.limits.append("empty-callee")
            return []
        enumerate_paths = len(self.order) <= POLICY["path_instruction_limit"]
        work = [(self.va, entry, {})]
        joined: dict[int, _State] = {}
        returns = []
        processed = 0
        while work:
            start, state, visits = work.pop(0)
            processed += 1
            if processed > POLICY["state_cap"]:
                self.summary.limits.append("path-state-cap")
                break
            visits = dict(visits)
            visits[start] = visits.get(start, 0) + 1
            if visits[start] > (POLICY["path_block_visits"] if enumerate_paths else 40):
                self.summary.limits.append("path-loop-cap" if enumerate_paths else "visit-cap")
                continue
            if not enumerate_paths:
                if start in joined:
                    merged = _join(joined[start], state)
                    if _same(merged, joined[start]):
                        continue
                    state = merged
                joined[start] = state.copy()
            frontier = [state.copy()]
            for address in blocks[start]:
                insn = self.insns[address]
                next_states = []
                for current in frontier:
                    self.context.budget -= 1
                    if self.context.budget < 0:
                        self.summary.limits.append("interproc-instruction-cap")
                        return returns
                    next_states.extend(self.execute(current, insn))
                frontier = next_states[: POLICY["state_cap"]]
                if len(next_states) > len(frontier):
                    self.summary.limits.append("call-return-state-cap")
            last = self.insns[blocks[start][-1]]
            if last.mnemonic in ("ret", "retn"):
                pop = int(last.operands[0].imm) if last.operands else 0
                for current in frontier:
                    current.regs["esp"] = lin_add(self.reg_get(current, "esp"), const_lin(4 + pop))
                    returns.append(current)
                continue
            if last.mnemonic == "jmp" and not succ[start]:
                target = self._jump_target(last)
                if (
                    target in self.context.sizes
                    and target not in self.ancestors
                    and target != self.va
                    and len(self.ancestors) < POLICY["call_depth"]
                ):
                    size = self.context.sizes[target]
                    code = self.context.read(target, size)
                    if len(code) == size and code:
                        tail = _Interproc(target, code, self.context, self.ancestors + (self.va,))
                        for current in frontier:
                            returns.extend(tail.paths(current))
                    else:
                        self.summary.limits.append("tail-bytes-unavailable")
                else:
                    self.summary.limits.append("unresolved-tail")
            for target in succ[start]:
                work.extend((target, current.copy(), visits) for current in frontier)
            if len(work) > POLICY["state_cap"]:
                self.summary.limits.append("path-queue-cap")
                work = work[: POLICY["state_cap"]]
        return returns[: POLICY["state_cap"]]


def summarize(
    va: int, code: bytes, *, read_code: Callable[[int, int], bytes], sizes: dict[int, int]
) -> tuple[AccessSummary, list[dict[str, Any]]]:
    summary = AccessSummary(va, len(code))
    context = _Context(summary, read_code, sizes)
    walker = _Interproc(va, code, context, ())
    summary.insns = len(walker.order)
    walker.paths(_State({"esp": Lin(((ESP0, 1),))}, {}, {}))
    return summary, [context.codes[key] for key in sorted(context.codes)]
