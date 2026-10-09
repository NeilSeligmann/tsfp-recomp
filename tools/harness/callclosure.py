# SPDX-License-Identifier: GPL-3.0-or-later
"""Static provenance for explicitly selected live-callee replacement proofs."""

from __future__ import annotations

import hashlib
from collections import deque
from collections.abc import Callable
from dataclasses import asdict, dataclass

from .callstub import CalleeConventions, _decoder, _direct_target, plan_calls
from .guarded_tables import GuardedJumpTable, prove_guarded_tables
from .jumps import direct_jump_target
from .jumps import inbody_direct_jump as _is_inbody_direct_jump
from .selection import instruction_reason


class LiveClosureError(ValueError):
    """The requested live execution is not a closed, statically understood call graph."""


@dataclass(frozen=True)
class ClosureNode:
    va: int
    name: str
    size: int
    sha256: str
    calls: tuple[int, ...]
    boundary: str | None = None
    #: T1576 (guarded jumps only, empty otherwise and then absent from the document): exact
    #: out-of-body direct JMP targets (also in `calls`) and the node's proven jump tables.
    tails: tuple[int, ...] = ()
    tables: tuple[dict[str, object], ...] = ()


@dataclass(frozen=True)
class LiveCallClosure:
    root: int
    nodes: tuple[ClosureNode, ...]

    def document(self, *, xbe_sha256: str, functions_sha256: str) -> dict[str, object]:
        """Serializable proof provenance, including explicitly opaque boundary nodes."""
        return {
            "mode": "live-direct-callee-closure",
            "root": f"0x{self.root:08x}",
            "xbe_sha256": xbe_sha256,
            "functions_sha256": functions_sha256,
            "x87_comparison": {
                "stack": "depth and values; exact 80-bit when representable by subject",
                "control_word": "exact 16-bit equality",
                "status_word_mask": "0x3800 (TOP, the modeled status subset)",
                "status_word_unmodeled_mask": "0xc7ff",
                "ignored_stack_writes": "[entry ESP - 0x1000, entry ESP), returned frame locals",
            },
            "nodes": [_node_document(node) for node in self.nodes],
        }


def _node_document(node: ClosureNode) -> dict[str, object]:
    entry: dict[str, object] = {
        **asdict(node),
        "va": f"0x{node.va:08x}",
        "calls": [f"0x{va:08x}" for va in node.calls],
        "boundary": node.boundary,
    }
    # Existing receipts stay byte-identical: the guarded fields appear only when populated.
    del entry["tails"], entry["tables"]
    if node.tails:
        entry["tails"] = [f"0x{va:08x}" for va in node.tails]
    if node.tables:
        entry["tables"] = list(node.tables)
    return entry


def _guarded_tail(va: int, size: int, target: int, sizes: dict[int, int]) -> int:
    from .stackprobe import STACKPROBE_VA

    if target in (va, va + size, STACKPROBE_VA) or sizes.get(target, 0) <= 0:
        raise LiveClosureError(
            f"function 0x{va:08x} tail jmp to 0x{target:08x} is not an exact known callee entry"
        )
    return target


def discover_live_call_closure(
    root: int,
    *,
    sizes: dict[int, int],
    names: dict[int, str],
    read_code: Callable[[int, int], bytes],
    boundaries: frozenset[int] = frozenset(),
    allow_vector: bool = False,
    vector_mode: str = "",
    guarded_jumps: bool = False,
) -> LiveCallClosure:
    """Prove direct-call closure, rejecting undecodable nodes and implicit boundaries.

    A boundary must be named on the command line. Its complete body is still fingerprinted,
    but its descendants are excluded and the reason is recorded. No call is stubbed at run
    time: this only limits the claim made by the static closure report.

    `guarded_jumps` (T1576, default off, then this function is byte-for-byte the old one)
    additionally admits, per node: an in-body direct JMP (already allowed), a direct
    out-of-body JMP to an EXACT known function entry whose pop equals the node's own pop (the
    target joins the closure and is hashed like a callee), and `jmp [reg*4+abs]` only when
    `prove_guarded_tables` proves every such site. A node still must end in a near `ret`;
    anything else stays refused. Vector state needs no relaxation: the same boundary-free
    rule applies and every added node is scanned for unsupported vector state like any other.
    """
    if allow_vector and boundaries:
        raise LiveClosureError("vector state requires a complete closure without opaque boundaries")
    decoder = _decoder()
    conventions = CalleeConventions(sizes, read_code, decoder)
    pending = deque([root])
    visited: dict[int, ClosureNode] = {}

    while pending:
        va = pending.popleft()
        if va in visited:
            continue
        size = sizes.get(va)
        if size is None:
            raise LiveClosureError(f"direct callee 0x{va:08x} has no known function bound")
        code = read_code(va, size)
        if len(code) != size:
            raise LiveClosureError(f"function 0x{va:08x} body is unreadable ({len(code)}/{size})")
        instructions = list(decoder.disasm(code, va))
        if not instructions or sum(insn.size for insn in instructions) != size:
            raise LiveClosureError(f"function 0x{va:08x} is not fully decodable")
        thunk = conventions.thunk_target(va)
        if thunk is not None and conventions.pop_bytes(va) is not None:
            # T1574: a pure single-jmp thunk is a tail edge to a resolved target; the
            # target is fingerprinted and checked as its own node.
            visited[va] = ClosureNode(
                va,
                names.get(va, f"sub_{va:08x}"),
                size,
                hashlib.sha256(code).hexdigest(),
                (thunk,),
            )
            pending.append(thunk)
            continue
        if instructions[-1].mnemonic.lower() not in {"ret", "retn"}:
            raise LiveClosureError(f"function 0x{va:08x} does not end in a near ret")

        tails: list[int] = []
        tables: tuple[GuardedJumpTable, ...] | None = None
        for insn in instructions:
            reason = instruction_reason(insn.mnemonic, insn.op_str)
            if allow_vector:
                from .vector import instruction_supported, vector_state_instruction

                if vector_state_instruction(insn):
                    if instruction_supported(insn, vector_mode):
                        continue
                    raise LiveClosureError(
                        f"function 0x{va:08x} contains unsupported vector state "
                        f"at 0x{insn.address:08x} ({insn.mnemonic})"
                    )
            if reason == "jump" and _is_inbody_direct_jump(insn, va, size):
                continue
            if reason == "jump" and guarded_jumps:
                target = direct_jump_target(insn)
                if target is not None:
                    tails.append(_guarded_tail(va, size, target, sizes))
                    continue
                if tables is None:
                    tables = prove_guarded_tables(va, size, code, read_code, allow_tails=True)
                if tables is None or insn.address not in {table.site for table in tables}:
                    raise LiveClosureError(
                        f"function 0x{va:08x} has an unproved indirect jump at "
                        f"0x{insn.address:08x} (guarded tables)"
                    )
                continue
            if reason == "sse-mmx" and allow_vector:
                from .vector import instruction_supported

                if instruction_supported(insn, vector_mode):
                    continue
            if reason not in (None, "call", "x87"):
                raise LiveClosureError(
                    f"function 0x{va:08x} contains unsupported {reason} at 0x{insn.address:08x}"
                )

        plan = plan_calls(va, size, instructions, conventions)
        if tails:
            node_pop = conventions.pop_bytes(va)
            if node_pop is None or any(
                conventions.pop_bytes(target) != node_pop for target in tails
            ):
                raise LiveClosureError(
                    f"function 0x{va:08x} tail target pop differs from its own ret (tail-pop)"
                )
        if isinstance(plan, str):
            if va not in boundaries:
                raise LiveClosureError(
                    f"function 0x{va:08x} has unsupported call/branch plan: {plan}; "
                    "name it explicitly with --live-call-boundary only if this is an "
                    "intentional unproved boundary"
                )
            sites = [
                f"0x{insn.address:08x}"
                for insn in instructions
                if insn.mnemonic.lower().startswith("call") and _direct_target(insn) is None
            ]
            detail = f"{plan}" + (f" at {', '.join(sites)}" if sites else "")
            visited[va] = ClosureNode(
                va,
                names.get(va, f"sub_{va:08x}"),
                size,
                hashlib.sha256(code).hexdigest(),
                (),
                boundary=detail,
            )
            continue

        if va in boundaries:
            raise LiveClosureError(
                f"explicit boundary 0x{va:08x} is unnecessary: all its direct calls resolve"
            )
        calls = tuple(sorted({site.target_va for site in plan.sites} | set(tails)))
        visited[va] = ClosureNode(
            va,
            names.get(va, f"sub_{va:08x}"),
            size,
            hashlib.sha256(code).hexdigest(),
            calls,
            tails=tuple(sorted(set(tails))),
            tables=tuple(table.document() for table in tables or ()),
        )
        pending.extend(calls)

    unused = boundaries - visited.keys()
    if unused:
        rendered = ", ".join(f"0x{va:08x}" for va in sorted(unused))
        raise LiveClosureError(f"explicit live-call boundary is not reachable: {rendered}")
    return LiveCallClosure(root, tuple(visited[va] for va in sorted(visited)))
