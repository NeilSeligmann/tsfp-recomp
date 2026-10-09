# SPDX-License-Identifier: GPL-3.0-or-later
"""T1533 audit: data dwords naming real function starts the lift does not start.

T1530 class: an original function reachable only through a data dword (vtable or
record table) that the disassembler folded into a larger function, so the lift
has no dispatch entry and the first indirect call stops the game.

Every aligned dword in the file-backed non-executable sections whose value is a
.text address is classified:

* ``listed``         exact start in the disassembler function list and dispatched
* ``not_dispatched`` exact start in the function list but absent from the dispatch table
* ``swallowed``      real function start (see below) strictly inside a listed body
* ``uncovered``      real function start inside no listed body
* ``rejected``       not a real function start (case label, mid-instruction, data)

A target is a *real function start* when the byte before it is INT3 padding or
the previous decoded instruction of the owning linear sweep is a terminal
(``ret``/``jmp``), the first instruction is not INT3, and a linear decode reaches
a terminal within ``MAX_BODY`` bytes without an invalid instruction.  The body size is
measured by that ret scan and compared with the func_id size estimate.

    python -m tools.data_dword_function_audit XBE LIFT_DIR --expected-xbe-sha256 HASH [--json OUT]
"""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

from tools.xbe import parse_xbe

MAX_BODY = 0x4000
#: Game data sections. The XDK library sections (D3D, XGRPH, DSOUND, XONLINE, XNET, XMV,
#: XPP) and DOLBY/XON_RD mix code, immediates, EH records and strings, so a dword there
#: is not evidence of a table (measured in T1533, see docs/t-data-dword-function-audit.md).
DATA_SECTIONS = frozenset({".rdata", ".data", ".data1"})
TERMINAL = {"ret", "retf", "jmp", "int3", "ud2"}


def parse_dispatch(text: str) -> set[int]:
    """Xbox VAs of the generated ``g_recomp_table`` initializer."""
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL)
    match = re.search(r"g_recomp_table\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\};", text, re.DOTALL)
    if match is None:
        raise ValueError("dispatcher has no g_recomp_table initializer")
    return {int(v, 16) for v in re.findall(r"\{\s*(0x[0-9A-Fa-f]+)u,", match.group(1))}


class Image:
    """Read access to the XBE by virtual address."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.xbe = parse_xbe(data)

    def read(self, va: int, size: int) -> bytes:
        off = self.xbe.va_to_offset(va)
        if off is None:
            return b""
        return self.data[off : off + size]


def measure_body(image: Image, start: int, limit: int = MAX_BODY) -> dict | None:
    """Ret scan: linear decode from ``start``; None when it is not plausible code."""
    code = image.read(start, limit)
    if not code or code[0] == 0xCC:
        return None
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    pc = 0
    farthest = 0
    rets = []
    count = 0
    while pc < len(code):
        insns = list(md.disasm(code[pc : pc + 15], start + pc, 1))
        if not insns:
            return None
        ins = insns[0]
        count += 1
        mnem = ins.mnemonic
        if mnem == "add" and ins.op_str.startswith("byte ptr [") and count <= 16:
            return None  # zero bytes or data decoded as code
        if mnem.startswith("j") and ins.op_str.startswith("0x"):
            target = int(ins.op_str, 16)
            if start <= target < start + limit:
                farthest = max(farthest, target - start)
        pc += ins.size
        if mnem in ("ret", "retf"):
            rets.append(start + pc - ins.size)
        if mnem in TERMINAL and pc > farthest:
            # A real body ends at a terminal no later branch jumps beyond.
            nxt = code[pc : pc + 1]
            if mnem == "int3" or nxt in (b"\xcc", b""):
                return {"size": pc, "insns": count, "rets": rets}
            if mnem in ("ret", "retf", "jmp"):
                return {"size": pc, "insns": count, "rets": rets}
    return None


def preceded_by_boundary(image: Image, va: int) -> str | None:
    prev = image.read(va - 1, 1)
    if prev == b"\xcc":
        return "int3_pad"
    tail = image.read(va - 3, 3)
    if len(tail) == 3 and tail[2] == 0xC3:
        return "after_ret"
    if len(tail) == 3 and tail[0] == 0xC2 and False:
        return None
    if len(tail) == 3 and image.read(va - 3, 1) == b"\xc2":
        return "after_ret_imm"
    return None


def owner_boundary(image: Image, owner: int, value: int) -> str | None:
    """Mnemonic of the owner's linear-decode instruction before ``value``.

    None when ``value`` is not an instruction boundary of the owner's sweep
    (mid-instruction hit), otherwise the previous mnemonic.
    """
    code = image.read(owner, value - owner)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    pc = 0
    prev = ""
    while pc < len(code):
        insns = list(md.disasm(code[pc : pc + 15], owner + pc, 1))
        if not insns:
            return None
        prev = insns[0].mnemonic
        pc += insns[0].size
    return prev if pc == len(code) else None


def in_owner_table(image: Image, dwords: list[tuple[str, int]], owner: int, owner_end: int) -> bool:
    """True when a neighbour dword of any reference also points into the owner.

    Consecutive code pointers into one function are a switch jump table, whose
    targets are case labels (interior), not function starts.
    """
    for _name, va in dwords:
        for delta in (-4, 4):
            raw = image.read(va + delta, 4)
            if len(raw) != 4:
                continue
            (other,) = struct.unpack("<I", raw)
            # A case label is interior: no padding or terminal in front of it.
            if owner < other < owner_end and preceded_by_boundary(image, other) is None:
                return True
    return False


def classify(
    image: Image,
    starts: list[int],
    bodies: dict[int, int],
    dispatch: set[int],
    sections_text: tuple[int, int],
    value: int,
    dwords: list[tuple[str, int]] = (),
) -> dict | None:
    lo, hi = sections_text
    if not lo <= value < hi:
        return None
    startset = set(starts)
    if value in startset:
        return {"class": "listed" if value in dispatch else "not_dispatched"}
    index = bisect.bisect_right(starts, value) - 1
    owner = starts[index] if index >= 0 else None
    covered = owner is not None and value < bodies[owner]
    boundary = preceded_by_boundary(image, value)
    body = measure_body(image, value) if boundary else None
    if boundary is None or body is None:
        return {"class": "rejected", "owner": owner if covered else None, "boundary": boundary}
    if covered and in_owner_table(image, dwords, owner, bodies[owner]):
        return {"class": "rejected", "owner": owner, "boundary": boundary, "why": "switch_table"}
    prev = owner_boundary(image, owner, value) if covered else "(uncovered)"
    if covered and (prev is None or prev not in TERMINAL):
        return {"class": "rejected", "owner": owner, "boundary": boundary, "why": "owner_sweep"}
    return {
        "class": "swallowed" if covered else "uncovered",
        "owner_prev": prev,
        "owner": owner if covered else None,
        "owner_end": bodies[owner] if covered else None,
        "boundary": boundary,
        "real_size": body["size"],
        "insns": body["insns"],
        "rets": body["rets"],
    }


def scan_dwords(
    image: Image, sections: frozenset[str] = DATA_SECTIONS
) -> dict[int, list[tuple[str, int]]]:
    """value -> (section, dword VA) over every file backed section but .text.

    The XBE marks .rdata/.data executable too, so the flag cannot select data."""
    refs: dict[int, list[int]] = defaultdict(list)
    xbe = image.xbe
    text = [s for s in xbe.sections if s.name == ".text"][0]
    lo, hi = text.virtual_addr, text.virtual_addr + text.virtual_size
    for s in xbe.sections:
        if s.name not in sections:  # .text immediates are the T1152 census, not tables
            continue
        size = min(s.raw_size, s.virtual_size)
        first = (-s.virtual_addr) & 3
        blob = image.data[s.raw_addr : s.raw_addr + size]
        for delta in range(first, size - 3, 4):
            (v,) = struct.unpack_from("<I", blob, delta)
            if lo <= v < hi:
                refs[v].append((s.name, s.virtual_addr + delta))
    return refs


def audit(
    image: Image,
    functions: list[dict],
    dispatch: set[int],
    estimates: dict[int, dict],
    sections: frozenset[str] = DATA_SECTIONS,
) -> dict:
    items = sorted((int(f["start"], 16), int(f["end"], 16)) for f in functions)
    starts = [s for s, _ in items]
    bodies = dict(items)
    text = [s for s in image.xbe.sections if s.name == ".text"][0]
    span = (text.virtual_addr, text.virtual_addr + text.virtual_size)
    out = defaultdict(list)
    for value, dwords in sorted(scan_dwords(image, sections).items()):
        row = classify(image, starts, bodies, dispatch, span, value, dwords)
        if row is None:
            continue
        row = dict(row, target=value, dwords=dwords)
        est = estimates.get(value)
        if est is not None:
            row["estimate"] = {"size": est.get("size"), "method": est.get("method")}
        out[row["class"]].append(row)
    return out


def seed_rows(image: Image, rows: list[dict], xbe_sha: str) -> list[dict]:
    """lift_thread_entries.json rows (hash-bound spans) for audited starts."""
    out = []
    for row in sorted(rows, key=lambda r: r["target"]):
        start, size = row["target"], row["real_size"]
        dword_text = ", ".join(f"0x{a:08X}" for _, a in row["dwords"])
        out.append(
            {
                "start": f"0x{start:08X}",
                "observed": False,
                "xbe_sha256": xbe_sha.lower(),
                "original_span": f"0x{start:08X}..0x{start + size:08X}",
                "original_sha256": hashlib.sha256(image.read(start, size)).hexdigest(),
                "note": (
                    f"T1533 audit: data dword(s) {dword_text} "
                    f"name this start; {row['class']} "
                    + (f"inside 0x{row['owner']:08X}" if row.get("owner") else "outside every body")
                    + f", real body {size} bytes by ret scan ({len(row['rets'])} RET)."
                ),
            }
        )
    return out


def _load(path: Path) -> object:
    return json.loads(path.read_text())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbe", type=Path)
    parser.add_argument("lift_dir", type=Path, help="lift output (disasm/, func_id/, gen/)")
    parser.add_argument("--expected-xbe-sha256", required=True)
    parser.add_argument("--json", type=Path, help="write the full result here")
    parser.add_argument(
        "--sections",
        default=",".join(sorted(DATA_SECTIONS)),
        help="comma separated sections to scan (default: game data sections)",
    )
    parser.add_argument(
        "--emit-seeds",
        type=Path,
        help="write lift_thread_entries.json style rows for swallowed/uncovered starts",
    )
    args = parser.parse_args(argv)
    data = args.xbe.read_bytes()
    if hashlib.sha256(data).hexdigest() != args.expected_xbe_sha256.lower():
        parser.error("XBE SHA-256 mismatch")
    image = Image(data)
    functions = _load(args.lift_dir / "disasm/functions.json")
    dispatch = parse_dispatch((args.lift_dir / "gen/recomp_dispatch.c").read_text())
    ident = _load(args.lift_dir / "func_id/identified_functions.json")
    estimates = {int(e["start"], 16): e for e in ident}
    result = audit(image, functions, dispatch, estimates, frozenset(args.sections.split(",")))
    summary = {k: len(v) for k, v in result.items()}
    print(json.dumps(summary, indent=2, sort_keys=True))
    if args.emit_seeds:
        rows = [r for k in ("swallowed", "uncovered") for r in result.get(k, [])]
        args.emit_seeds.write_text(
            json.dumps(seed_rows(image, rows, args.expected_xbe_sha256), indent=2) + "\n"
        )
    if args.json:
        args.json.write_text(json.dumps(result, indent=1, default=hex))
    return 0


if __name__ == "__main__":
    sys.exit(main())
