# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify data-referenced code addresses missing from the function table (T1266).

    python -m tools.verify_function_additions --xbe build/default.xbe            # report
    python -m tools.verify_function_additions --xbe build/default.xbe --write    # regenerate CSVs

`generated/retail/functions.csv` (a Ghidra export) misses code reached only through data
tables. A candidate is a dword in `.rdata/.data/.data1` that points into an executable
section at an address that is not a listed entry and not inside a listed function span.
A candidate is ACCEPTED only when ALL hold:

  * it is not inside a listed function span (and its extent overlaps no listed byte range),
  * a flow-following capstone walk from it decodes cleanly (no invalid opcode, no I/O,
    privileged or far-transfer instruction, no `add [eax], al` zero-run) and every path
    ends in ret, a tail jump out of the walked body, or an indirect jump,
  * the walk never runs into a listed function or off the executable section,
  * it is directly preceded by a terminator, a listed function's end, or an accepted
    function's end (otherwise it is more likely a mid-function label),
  * it is not an interior instruction of an earlier accepted candidate (jump-table or
    shared-tail target).

Everything else is REJECTED with a reason category. Paths are relative to the repo root.
Only addresses, counts and short evidence tags are written, never byte dumps.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import struct
import sys
from collections import Counter, defaultdict
from collections.abc import Callable
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from tools.codediff.boundaries import (
    ADDITION_CSV_COLUMNS,
    DEFAULT_ADDITIONS,
    DEFAULT_OVERRIDES,
    load_function_table,
    preceded_by_terminator,
)
from tools.xbe.parser import parse_xbe

DATA_SECTIONS = (".rdata", ".data", ".data1")
DEFAULT_REJECTS = Path("generated/retail/function_addition_rejects.csv")  # gitignored bulk output
MAX_BYTES = 0x10000
MAX_INSNS = 20000
#: Mnemonics that never occur in Xbox user code: reaching one means the stream is data.
BAD_MNEMONICS = frozenset(
    {
        "in", "out", "ins", "insb", "insw", "insd", "outs", "outsb", "outsw", "outsd", "hlt",
        "cli", "sti", "iret", "iretd", "arpl", "lcall", "ljmp", "lret", "int", "into",
        "sysenter", "sysexit", "syscall", "sysret", "wrmsr", "rdmsr", "lgdt", "lidt", "ltr",
        "lldt", "bound", "aaa", "aas", "daa", "das", "aam", "aad", "salc", "enter", "leave16",
        "les", "lds", "pop es", "push es", "push ss", "pop ss", "push ds", "pop ds", "push cs",
    }
)  # fmt: skip


def _decode_flow(
    md: Cs,
    code: bytes,
    base: int,
    entry: int,
    listed_starts: set[int],
    listed_spans: list[tuple[int, int]],
    enclosing: Callable[[int], int | None],
    read_dword: Callable[[int], int | None],
) -> tuple[str, int, int, str]:
    """Walk control flow from `entry`. Returns (status, end_exclusive, insns, end_kind).

    status is "ok" or a reject category.
    """

    def in_listed(target: int) -> bool:
        return target in listed_starts or enclosing(target) is not None

    work = [entry]
    seen: dict[int, int] = {}
    end = entry
    kinds: set[str] = set()
    while work:
        addr = work.pop()
        while True:
            if addr in seen:
                break
            if not base <= addr < base + len(code):
                return "runs_off_section", end, len(seen), ""
            if addr != entry and addr in listed_starts:
                return "falls_into_listed", end, len(seen), ""
            if enclosing(addr) is not None:
                return "runs_into_listed", end, len(seen), ""
            insns = list(md.disasm(code[addr - base : addr - base + 16], addr, 1))
            if not insns:
                return "invalid_opcode", end, len(seen), ""
            insn = insns[0]
            if insn.mnemonic in BAD_MNEMONICS or insn.bytes[:2] == b"\x00\x00":
                return "data_like_instruction", end, len(seen), ""
            seen[addr] = insn.size
            if len(seen) > MAX_INSNS or addr + insn.size - entry > MAX_BYTES:
                return "too_large", end, len(seen), ""
            end = max(end, addr + insn.size)
            mnem = insn.mnemonic
            nxt = addr + insn.size
            if mnem in ("ret", "retn", "retf"):
                kinds.add("ret")
                break
            target = None
            if insn.operands and insn.operands[0].type == X86_OP_IMM:
                target = insn.operands[0].imm
            if mnem == "jmp":
                if target is None:
                    mem = insn.operands[0] if insn.operands else None
                    kinds.add("indirect_jmp")
                    # switch table: jmp [reg*4 + table]; follow in-body table targets
                    if mem is not None and mem.type == X86_OP_MEM and mem.mem.scale == 4:
                        for tgt in _table_targets(read_dword, mem.mem.disp, entry):
                            if tgt not in seen:
                                work.append(tgt)
                    break
                if target < entry or target >= entry + MAX_BYTES or in_listed(target):
                    kinds.add("tail_jmp")
                else:
                    work.append(target)
                    kinds.add("jmp")
                break
            if mnem.startswith("j") or mnem in ("loop", "loope", "loopne", "jecxz", "jcxz"):
                if target is not None and entry <= target < entry + MAX_BYTES:
                    if not in_listed(target):
                        work.append(target)
                addr = nxt
                continue
            addr = nxt
    if not kinds:
        return "no_terminator", end, len(seen), ""
    return "ok", end, len(seen), "+".join(sorted(kinds))


def make_enclosing(spans: list[tuple[int, int]]) -> Callable[[int], int | None]:
    """`va -> start of a listed span containing it` over start-sorted `(start, end)` spans.

    Uses a prefix-maximum of span ends, so a huge fragmented span still encloses functions
    listed inside it (the nearest preceding span alone would miss it).
    """
    starts = [start for start, _ in spans]
    reach: list[tuple[int, int]] = []
    for start, stop in spans:
        reach.append(reach[-1] if reach and reach[-1][0] >= stop else (stop, start))

    def enclosing(va: int) -> int | None:
        at = bisect.bisect_right(starts, va) - 1
        return reach[at][1] if at >= 0 and reach[at][0] > va else None

    return enclosing


def _table_targets(read_dword: Callable[[int], int | None], table_va: int, entry: int) -> list[int]:
    """In-body targets of a `jmp [reg*4 + table]` switch: leading dwords inside the body window."""
    targets: list[int] = []
    for slot in range(256):
        value = read_dword(table_va + 4 * slot)
        if value is None or not entry <= value < entry + MAX_BYTES:
            break
        targets.append(value)
    return targets


def preceded_by_alignment(md: Cs, code: bytes, base: int, entry: int) -> bool:
    """A terminator followed by at most 15 bytes of compiler alignment padding.

    Decode forward from a possible terminator end; never accept arbitrary backward
    decoding. LEA and MOV must preserve their destination; the canonical
    five-byte ADD EAX,0 padding is allowed only after a terminator, where
    its flag writes are unreachable from the preceding body.
    """
    for distance in range(1, 16):
        start = entry - distance
        if not preceded_by_terminator(code, base, start):
            continue
        offset = start - base
        if offset < 0:
            continue
        cursor = start
        for insn in md.disasm(code[offset : entry - base], start):
            if insn.mnemonic == "nop":
                pass
            elif bytes(insn.bytes) == b"\x05\x00\x00\x00\x00":
                pass  # canonical unreachable five-byte compiler padding
            elif insn.mnemonic == "mov" and len(insn.operands) == 2:
                dst, src = insn.operands
                if not (
                    dst.type == src.type == X86_OP_REG and dst.reg == src.reg and dst.size == 4
                ):
                    break
            elif insn.mnemonic == "lea" and len(insn.operands) == 2:
                dst, src = insn.operands
                if not (
                    dst.type == X86_OP_REG
                    and src.type == X86_OP_MEM
                    and src.mem.base == dst.reg
                    and src.mem.index == 0
                    and src.mem.disp == 0
                    and src.mem.segment == 0
                    and dst.size == 4
                    and src.size == 4
                ):
                    break
            else:
                break
            cursor = insn.address + insn.size
        if cursor == entry:
            return True
    return False


def run(
    xbe_path: Path,
    functions_path: Path,
    additions_path: Path | None = None,
    extra: dict[int, str] | None = None,
) -> tuple[list[dict], list[tuple[int, str, str]], dict]:
    """`additions_path` layers the tracked additions into the listed table (so only NEW
    candidates remain). `extra` maps a non-data-pointed candidate (a called handler) to its
    evidence label; it goes through exactly the same rules."""
    data = xbe_path.read_bytes()
    xbe = parse_xbe(data)
    functions = load_function_table(functions_path, DEFAULT_OVERRIDES, additions_path)
    listed_starts = {f.entry_va for f in functions}
    spans = sorted(
        (f.entry_va, max(f.entry_va + f.size_bytes, f.body_max_va + 1)) for f in functions
    )
    span_starts = [s[0] for s in spans]
    enclosing = make_enclosing(spans)
    sizes = {f.entry_va: f.size_bytes for f in functions}

    def code_of(va: int) -> tuple[int, bytes] | None:
        for base, blob in sections_code.values():
            if base <= va < base + len(blob):
                return base, blob
        return None

    sections_code = {
        s.name: (s.virtual_addr, data[s.raw_addr : s.raw_addr + s.raw_size])
        for s in xbe.sections
        if s.executable and s.virtual_addr < 0x470000
    }
    refs: dict[int, list[int]] = defaultdict(list)
    for section in xbe.sections:
        if section.name not in DATA_SECTIONS:
            continue
        raw = data[section.raw_addr : section.raw_addr + section.raw_size]
        for off in range(0, len(raw) - 3, 4):
            (val,) = struct.unpack_from("<I", raw, off)
            if code_of(val) is not None and val not in listed_starts:
                refs[val].append(section.virtual_addr + off)
    for extra_va in extra or {}:
        if extra_va not in listed_starts:
            refs.setdefault(extra_va, [])

    def read_dword(va: int) -> int | None:
        off = xbe.va_to_offset(va)
        if off is None or off + 4 > len(data):
            return None
        return struct.unpack_from("<I", data, off)[0]

    def mid_function_table(va: int) -> bool:
        """Every reference to `va` sits in a run of code pointers where at least half of the
        other members are mid-function addresses (inside a listed span, not an entry). Such
        a run is a switch/label table, so `va` is a label, not a function entry."""
        if not refs[va]:
            return False  # an extra (called) candidate has no table slot to be a label of
        for slot in refs[va]:
            first = slot
            while code_of(read_dword(first - 4) or 0) is not None:
                first -= 4
            members: list[int] = []
            cursor = first
            while (value := read_dword(cursor)) is not None and code_of(value) is not None:
                members.append(value)
                cursor += 4
            others = [m for m in members if m != va]
            mid = 0
            for member in others:
                if member not in listed_starts and enclosing(member) is not None:
                    mid += 1
            if not others or mid * 2 < len(others):
                return False
        return True

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    accepted: list[dict] = []
    rejects: list[tuple[int, str, str]] = []
    accepted_ends: set[int] = set()
    accepted_spans: list[tuple[int, int]] = []
    for va in sorted(refs):
        # 1. inside a listed function span?
        owner = enclosing(va)
        if owner is not None:
            owner_end = next(stop for start, stop in spans if start == owner)
            fragmented = owner_end - owner > sizes[owner] + 16
            reason = "inside_fragmented_span" if fragmented else "inside_listed_span"
            rejects.append((va, reason, f"owner={owner:#010x}"))
            continue
        if mid_function_table(va):
            rejects.append((va, "mid_function_table_target", f"refs={len(refs[va])}"))
            continue
        # 2. interior of an earlier accepted candidate
        if accepted_spans and accepted_spans[-1][0] < va < accepted_spans[-1][1]:
            rejects.append((va, "interior_of_candidate", f"owner={accepted_spans[-1][0]:#010x}"))
            continue
        base, blob = code_of(va) or (0, b"")
        terminated = preceded_by_terminator(blob, base, va) or preceded_by_alignment(
            md, blob, base, va
        )
        if not terminated:
            ends_here = va in accepted_ends or _is_listed_end(va, spans)
            if not ends_here:
                rejects.append((va, "no_preceding_terminator", ""))
                continue
        status, end, insns, kind = _decode_flow(
            md, blob, base, va, listed_starts, spans, enclosing, read_dword
        )
        if status != "ok":
            rejects.append((va, status, f"insns={insns}"))
            continue
        # interior check against upcoming listed function (extent must stay below next listed)
        size = end - va
        lo_at = bisect.bisect_right(span_starts, va)
        if lo_at < len(spans) and spans[lo_at][0] < end:
            rejects.append((va, "extent_overlaps_listed", f"next={spans[lo_at][0]:#010x}"))
            continue
        first = ",".join(f"{r:#010x}" for r in refs[va][:2])
        label = (
            f"data refs={len(refs[va])} first={first}"
            if refs[va]
            else (extra or {}).get(va, "called target")
        )
        accepted.append(
            {
                "entry_va": va,
                "size_bytes": size,
                "evidence": (
                    f"{label}; flow-decoded {insns} insns "
                    f"to {kind}; preceded by {'terminator' if terminated else 'function end'}"
                ),
            }
        )
        accepted_ends.add(end)
        accepted_spans.append((va, end))
    stats = {
        "candidates": len(refs),
        "accepted": len(accepted),
        "rejected": len(rejects),
        "by_reason": dict(Counter(r[1] for r in rejects)),
        "accepted_bytes": sum(a["size_bytes"] for a in accepted),
    }
    return accepted, rejects, stats


def _is_listed_end(va: int, spans: list[tuple[int, int]]) -> bool:
    idx = bisect.bisect_left(spans, (va, -1)) - 1
    return idx >= 0 and spans[idx][1] == va


def write_additions(path: Path, accepted: list[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(ADDITION_CSV_COLUMNS)
        for item in accepted:
            writer.writerow([f"{item['entry_va']:#010x}", item["size_bytes"], item["evidence"]])


def write_rejects(path: Path, rejects: list[tuple[int, str, str]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["entry_va", "reason", "detail"])
        for va, reason, detail in rejects:
            writer.writerow([f"{va:#010x}", reason, detail])


def merge_additions(path: Path, accepted: list[dict]) -> list[dict]:
    """Existing tracked rows (kept verbatim) plus the new accepted ones, sorted by entry."""
    with path.open(encoding="utf-8", newline="") as handle:
        kept = [
            {
                "entry_va": int(row["entry_va"], 16),
                "size_bytes": int(row["size_bytes"]),
                "evidence": row["evidence"],
            }
            for row in csv.DictReader(handle)
        ]
    known = {row["entry_va"] for row in kept}
    merged = kept + [row for row in accepted if row["entry_va"] not in known]
    return sorted(merged, key=lambda row: row["entry_va"])


def read_extra(path: Path, evidence: str) -> dict[int, str]:
    """Parse a candidate list: one hex VA per line, `#` starts a comment."""
    found: dict[int, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        token = line.split("#", 1)[0].strip()
        if token:
            found[int(token, 16)] = evidence
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    parser.add_argument("--functions", type=Path, default=Path("generated/retail/functions.csv"))
    parser.add_argument("--additions", type=Path, default=DEFAULT_ADDITIONS)
    parser.add_argument("--rejects", type=Path, default=DEFAULT_REJECTS)
    parser.add_argument(
        "--layered",
        action="store_true",
        help="load the tracked additions into the listed table (report only NEW candidates)",
    )
    parser.add_argument(
        "--extra",
        type=Path,
        help="hex VAs, one per line (# comments): called, non-data-pointed candidates",
    )
    parser.add_argument("--extra-evidence", default="called gate handler (T1468)")
    parser.add_argument("--write", action="store_true", help="regenerate the tracked CSVs")
    args = parser.parse_args()
    extra = read_extra(args.extra, args.extra_evidence) if args.extra else None
    accepted, rejects, stats = run(
        args.xbe, args.functions, args.additions if args.layered else None, extra
    )
    for key, value in stats.items():
        print(f"{key}: {value}")
    if args.write:
        if args.layered:
            accepted = merge_additions(args.additions, accepted)
        write_additions(args.additions, accepted)
        write_rejects(args.rejects, rejects)
        print(f"wrote {args.additions} and {args.rejects}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
