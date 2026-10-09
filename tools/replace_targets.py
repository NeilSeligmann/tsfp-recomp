# SPDX-License-Identifier: GPL-3.0-or-later
"""Rank libsig-named library functions as candidates for native replacement (T354).

    python -m tools.replace_targets --xbe tmp/oxm-extract/retail/default.xbe \
        --functions generated/retail/functions.csv --libsig tmp/replace-targets/libsig_names.csv \
        --surface src/xbox/xdk_surface.c --game-src src/game --out tmp/replace-targets/ranked.csv

PROVENANCE. The libsig CSV is XDK-derived and local-only. The ranked CSV carries SDK-derived
names, so `--out` must resolve under tmp/ or generated/ (tools.libsig.guard). The stdout
summary prints addresses, counts and structural classes only, never a name.

HOTNESS. No per-function dynamic profile exists in the repo (the host trace records HLE calls
only), so the rank is static: distinct game-code calling functions (callers that libsig does not
name) of direct `call`/tail `jmp` rel32 sites, then all callers. The minimum direct-call depth
from the XBE entry point is reported but rarely defined, because the entry reaches most code
through indirect calls.

CLASS is structural, from the decoded body, never from the name:
  boundary  port I/O, privileged or system instructions, or a call through a memory pointer
            (kernel thunk or vtable): the hardware or HLE edge, skipped
  leaf-pure no calls, no absolute-address memory access, no x87/SSE: differential-testable
  leaf-fpu  no calls but x87/SSE/MMX: pure, yet the harness skips these (SKIPPED-UNSUPPORTED)
  leaf-state no calls but reads or writes an absolute address (global state)
  composite calls other functions
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import Counter, deque
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_ARCH_X86, CS_GRP_CALL, CS_GRP_JUMP, CS_MODE_32, Cs
from capstone.x86 import X86_OP_MEM

from tools.libsig.guard import ProvenanceError, check_output_path
from tools.xbe import parse_xbe

CONFIDENT = ("high", "resolved")
PRIVILEGED = frozenset(
    "in ins insb insw insd out outs outsb outsw outsd cli sti hlt rdmsr wrmsr int int3 into "
    "iret iretd lgdt lidt ltr invd wbinvd cpuid rdtsc rdpmc sysenter sysexit lock".split()
)
GROUP_X87 = 8
GROUP_FPU_MMX_SSE = frozenset({"mmx", "sse1", "sse2", "sse3", "ssse3", "fpu", "3dnow"})
CLASS_ORDER = ("leaf-pure", "leaf-fpu", "leaf-state", "composite", "boundary")
REGISTRATION = re.compile(r"GAME_REPLACE\w*\(\s*([0-9A-Fa-f]{6,8})\s*,")
SURFACE_ROW = re.compile(r"\{0x([0-9a-fA-F]+),\s*\"")


@dataclass
class Body:
    entry: int
    size: int
    calls: set[int]  # direct call and tail-jump targets (any address)
    indirect_calls: int
    privileged: bool
    fpu: bool
    abs_memory: bool


@dataclass
class Row:
    address: int
    symbol: str
    confidence: str
    size: int
    callers: int
    game_callers: int
    sites: int
    depth: int | None
    klass: str


def decode_body(code: bytes, entry: int) -> Body:
    """Decode one function body into the features the class and the call graph need."""
    cs = Cs(CS_ARCH_X86, CS_MODE_32)
    cs.detail = True
    calls: set[int] = set()
    indirect = 0
    privileged = False
    fpu = False
    abs_memory = False
    for insn in cs.disasm(code, entry):
        if insn.mnemonic in PRIVILEGED:
            privileged = True
        groups = {cs.group_name(g) for g in insn.groups}
        if groups & GROUP_FPU_MMX_SSE or insn.mnemonic.startswith("f"):
            fpu = True
        is_branch = CS_GRP_CALL in insn.groups or (
            CS_GRP_JUMP in insn.groups and insn.mnemonic == "jmp"
        )
        for op in insn.operands:
            if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                if is_branch:
                    indirect += 1
                else:
                    abs_memory = True
            elif is_branch and op.type == X86_OP_MEM:
                indirect += 1
        if insn.mnemonic == "call" and insn.op_str.startswith("0x"):
            calls.add(int(insn.op_str, 16))
        elif insn.mnemonic == "jmp" and insn.op_str.startswith("0x") and insn.bytes[0] == 0xE9:
            calls.add(int(insn.op_str, 16))
    return Body(entry, len(code), calls, indirect, privileged, fpu, abs_memory)


def classify(body: Body) -> str:
    if body.privileged or body.indirect_calls:
        return "boundary"
    if body.calls:
        return "composite"
    if body.abs_memory:
        return "leaf-state"
    return "leaf-fpu" if body.fpu else "leaf-pure"


def call_graph(bodies: dict[int, Body], library: set[int]) -> tuple[Counter[int], Counter[int]]:
    """(all, game-code) distinct calling functions per target. Self calls do not count.

    A caller is game code when it is not itself a confident libsig-named function.
    """
    callers: Counter[int] = Counter()
    game: Counter[int] = Counter()
    for entry, body in bodies.items():
        for target in body.calls:
            if target != entry:
                callers[target] += 1
                if entry not in library:
                    game[target] += 1
    return callers, game


def depths(bodies: dict[int, Body], root: int) -> dict[int, int]:
    """Minimum direct-call depth from `root` over the static call graph."""
    seen = {root: 0}
    queue = deque([root])
    while queue:
        current = queue.popleft()
        body = bodies.get(current)
        if body is None:
            continue
        for target in body.calls:
            if target in bodies and target not in seen:
                seen[target] = seen[current] + 1
                queue.append(target)
    return seen


def count_sites(code_by_entry: dict[int, bytes], entries: set[int]) -> Counter[int]:
    """Total direct `call` and `jmp` rel32 sites per target entry, self sites excluded."""
    total: Counter[int] = Counter()
    for entry, code in code_by_entry.items():
        for index in range(len(code) - 4):
            if code[index] in (0xE8, 0xE9):
                target = (
                    entry
                    + index
                    + 5
                    + int.from_bytes(code[index + 1 : index + 5], "little", signed=True)
                ) & 0xFFFFFFFF
                if target in entries and target != entry:
                    total[target] += 1
    return total


def registered_addresses(game_source: Path) -> set[int]:
    found: set[int] = set()
    for path in sorted(game_source.glob("*.c")):
        found.update(int(m.group(1), 16) for m in REGISTRATION.finditer(path.read_text()))
    return found


def surface_addresses(surface: Path) -> set[int]:
    return {int(m.group(1), 16) for m in SURFACE_ROW.finditer(surface.read_text())}


def read_libsig(path: Path) -> dict[int, tuple[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return {
            int(row["address"], 16): (row["symbol"], row["confidence"])
            for row in csv.DictReader(handle)
            if row["confidence"] in CONFIDENT and row["symbol"]
        }


def rank(rows: list[Row], max_size: int) -> list[Row]:
    """Most game-code callers first, then easier class, then most callers, then smallest."""
    kept = [r for r in rows if r.klass != "boundary" and r.size <= max_size]
    return sorted(
        kept,
        key=lambda r: (-r.game_callers, CLASS_ORDER.index(r.klass), -r.callers, r.size, r.address),
    )


def build_rows(
    named: dict[int, tuple[str, str]],
    bodies: dict[int, Body],
    sites: Counter[int],
    callers: Counter[int],
    game: Counter[int],
    depth: dict[int, int],
    excluded: set[int],
) -> list[Row]:
    return [
        Row(
            address,
            symbol,
            confidence,
            bodies[address].size,
            callers[address],
            game[address],
            sites[address],
            depth.get(address),
            classify(bodies[address]),
        )
        for address, (symbol, confidence) in named.items()
        if address in bodies and address not in excluded
    ]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--functions", type=Path, required=True)
    parser.add_argument("--libsig", type=Path, required=True)
    parser.add_argument("--surface", type=Path, required=True)
    parser.add_argument("--game-src", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--max-size", type=int, default=600)
    parser.add_argument("--top", type=int, default=40)
    args = parser.parse_args(argv)
    try:
        check_output_path(args.out)
    except ProvenanceError as error:
        print(error, file=sys.stderr)
        return 2
    data = args.xbe.read_bytes()
    xbe = parse_xbe(data)
    text = {
        s.virtual_addr: (s, data[s.raw_addr : s.raw_addr + s.raw_size])
        for s in xbe.sections
        if s.executable
    }

    def read(va: int, size: int) -> bytes:
        for base, (_, blob) in text.items():
            if base <= va < base + len(blob):
                return blob[va - base : va - base + size]
        return b""

    with args.functions.open(newline="", encoding="utf-8") as handle:
        table = [(int(r["entry_va"], 16), int(r["size_bytes"])) for r in csv.DictReader(handle)]
    code = {entry: read(entry, size) for entry, size in table}
    bodies = {entry: decode_body(blob, entry) for entry, blob in code.items() if blob}
    named = read_libsig(args.libsig)
    callers, game = call_graph(bodies, set(named))
    sites = count_sites(code, set(bodies))
    depth = depths(bodies, xbe.entry_point)
    excluded = registered_addresses(args.game_src) | surface_addresses(args.surface)
    rows = build_rows(named, bodies, sites, callers, game, depth, excluded)
    ranked = rank(rows, args.max_size)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "rank",
                "address",
                "symbol",
                "confidence",
                "size",
                "callers",
                "game_callers",
                "sites",
                "depth",
                "class",
            ]
        )
        for position, row in enumerate(ranked, 1):
            writer.writerow(
                [
                    position,
                    f"{row.address:#010x}",
                    row.symbol,
                    row.confidence,
                    row.size,
                    row.callers,
                    row.game_callers,
                    row.sites,
                    "" if row.depth is None else row.depth,
                    row.klass,
                ]
            )
    by_class = Counter(r.klass for r in rows)
    print(f"named confident: {len(named)}  candidates (not surface/registered): {len(rows)}")
    print("by class: " + ", ".join(f"{k}={by_class[k]}" for k in CLASS_ORDER))
    print(f"ranked (non-boundary, size<={args.max_size}): {len(ranked)}  -> {args.out}")
    for position, row in enumerate(ranked[: args.top], 1):
        print(
            f"{position:3d} {row.address:#010x} {row.klass:10s} size={row.size:4d} "
            f"callers={row.callers:4d} game={row.game_callers:4d} sites={row.sites:4d} "
            f"depth={row.depth} {row.confidence}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
