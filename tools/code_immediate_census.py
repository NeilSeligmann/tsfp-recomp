# SPDX-License-Identifier: GPL-3.0-or-later
"""Find code immediates and data dwords naming function entries the lift misses.

Companion of ``tools/data_record_census.py`` (T822 part b).  That tool matches
data dwords against *decoded* function starts, which cannot find an entry the
disassembler never decoded (T1128: ``push 0x5FCF0``).  This tool decodes every
instruction of every decoded function span with Capstone, collects immediate
operands (``push imm``, ``mov reg/mem, imm``, ``lea``-less constants) and
aligned initialized-data dwords whose value lands in an executable section,
and classifies each value against the dispatcher and the decoded spans:

* ``dispatched``: exact dispatcher row (covered, never reported as a gap).
* ``decoded_not_dispatched``: exact decoded start, no dispatcher row.
* ``interior``: inside a dispatched or decoded span, not a start (a data
  pointer into code, a jump-table landing pad or an overlap seed case).
* ``orphan``: inside no decoded span at all.  Reported with entry-shape
  evidence (padding before, prologue bytes, a RET before the next padding)
  so a reviewer can judge it.  This is a candidate census, never a claim.

Output is JSON on stdout.  Nothing here reads owner files other than the XBE
passed on the command line, and nothing is written to tracked paths.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from bisect import bisect_right
from pathlib import Path

import capstone
from capstone import x86_const

from tools.data_record_census import DATA_SECTION_NAMES, parse_dispatch

PADDING = {0xCC, 0x90, 0x00}
PROLOGUES = (
    b"\x55\x8b\xec",  # push ebp; mov ebp, esp
    b"\x83\xec",  # sub esp, imm8
    b"\x81\xec",  # sub esp, imm32
    b"\x53",
    b"\x56",
    b"\x57",
    b"\x8b\x44\x24",  # mov eax, [esp+x]
    b"\x8b\x4c\x24",
    b"\x8b\x54\x24",
    b"\x6a",
    b"\x64\xa1",
    b"\x33\xc0",
    b"\x32\xc0",
    b"\x8b\xff",
)


def _int(value: str | int) -> int:
    return value if isinstance(value, int) else int(value, 16)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Image:
    """Virtual-address view of the XBE's file-backed sections."""

    def __init__(self, data: bytes, sections: list[dict]) -> None:
        self.data = data
        self.sections = [
            (
                s["name"],
                _int(s["virtual_addr"]),
                min(int(s["raw_size"]), int(s["virtual_size"])),
                _int(s["raw_addr"]),
                bool(s.get("executable")),
            )
            for s in sections
        ]

    def exec_section(self, va: int) -> str | None:
        for name, base, size, _raw, executable in self.sections:
            if executable and base <= va < base + size and name not in DATA_SECTION_NAMES:
                return name
        return None

    def read(self, va: int, size: int) -> bytes:
        for _name, base, ssize, raw, _x in self.sections:
            if base <= va < base + ssize:
                end = min(va + size, base + ssize)
                return self.data[raw + va - base : raw + end - base]
        return b""


def linear_decode(image: Image, va: int, limit: int) -> dict:
    """Recursive descent from ``va`` inside the gap ``[va, limit)``.

    Follows fall-through and direct conditional/unconditional branches that stay
    inside the gap.  ``ok`` is true when every path ends in RET, a tail JMP out
    of the gap (or an indirect JMP) or INT3, no instruction is invalid, no path
    falls into ``limit`` (the next decoded span) and the extent is contiguous
    enough to be one function.  ``end`` is the highest instruction end reached.
    The controls 0x5FCF0 (end 0x5FDB4) and 0x191B10 (end 0x191B97) are tested.
    """
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    seen: set[int] = set()
    pending = [va]
    end = va
    ok = True
    reason = ""
    terminals: set[str] = set()
    while pending and ok:
        cursor = pending.pop()
        while ok:
            if cursor in seen:
                break
            if not (va <= cursor < limit):
                ok, reason = False, f"falls out of gap at {cursor:#x}"
                break
            decoded = list(md.disasm(image.read(cursor, 16), cursor, 1))
            if not decoded:
                ok, reason = False, f"invalid instruction at {cursor:#x}"
                break
            insn = decoded[0]
            seen.add(cursor)
            end = max(end, cursor + insn.size)
            target = None
            if insn.operands and insn.operands[0].type == x86_const.X86_OP_IMM:
                target = insn.operands[0].imm & 0xFFFFFFFF
            if insn.mnemonic in ("ret", "int3", "hlt"):
                terminals.add(insn.mnemonic)
                break
            if insn.mnemonic == "jmp":
                if target is not None and va <= target < limit:
                    cursor = target
                    continue
                terminals.add("tail-jmp")
                break
            if insn.mnemonic.startswith("j") and target is not None:
                if va <= target < limit:
                    pending.append(target)
                else:
                    terminals.add("tail-jcc")
            cursor += insn.size
    return {
        "ok": ok and bool(terminals),
        "instructions": len(seen),
        "end": end,
        "terminals": sorted(terminals),
        "reason": reason,
    }


def entry_shape(image: Image, va: int, starts: list[int], span_ends: dict[int, int]) -> dict:
    """Entry-shape evidence for an orphan target."""
    before = image.read(va - 4, 4)
    code = image.read(va, 16)
    pad_before = len(before) == 4 and before[-1] in PADDING
    ret_before = len(before) == 4 and (before[-1] == 0xC3 or before[-3:-2] == b"\xc2")
    prologue = next((p.hex() for p in PROLOGUES if code.startswith(p)), None)
    idx = bisect_right(starts, va) - 1
    prev_end = span_ends[starts[idx]] if idx >= 0 else None
    next_start = starts[idx + 1] if idx + 1 < len(starts) else va + 4096
    return {
        "aligned16": va % 16 == 0,
        "linear": linear_decode(image, va, next_start),
        "bytes": code.hex(),
        "padding_or_ret_before": pad_before or ret_before,
        "prologue_match": prologue,
        "previous_span_end": prev_end,
    }


def collect_code_immediates(image: Image, functions: list[dict]) -> dict[int, list[dict]]:
    """Return {value: [site, ...]} for every immediate landing in executable code."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    found: dict[int, list[dict]] = {}
    for fn in functions:
        start, end = _int(fn["start"]), _int(fn["end"])
        code = image.read(start, end - start)
        for insn in md.disasm(code, start):
            for op in insn.operands:
                if op.type != x86_const.X86_OP_IMM:
                    continue
                value = op.imm & 0xFFFFFFFF
                if insn.mnemonic in ("call", "jmp") or insn.mnemonic.startswith("j"):
                    continue
                if image.exec_section(value) is None:
                    continue
                found.setdefault(value, []).append(
                    {"site": insn.address, "insn": f"{insn.mnemonic} {insn.op_str}", "fn": start}
                )
    return found


def collect_data_dwords(image: Image) -> dict[int, list[dict]]:
    found: dict[int, list[dict]] = {}
    for name, base, size, raw, _x in image.sections:
        if name not in DATA_SECTION_NAMES:
            continue
        for delta in range((-base) & 3, size - 3, 4):
            value = struct.unpack_from("<I", image.data, raw + delta)[0]
            if image.exec_section(value) is not None:
                found.setdefault(value, []).append(
                    {"site": base + delta, "insn": f"dword in {name}"}
                )
    return found


def classify(
    values: dict[int, list[dict]],
    starts: set[int],
    sorted_starts: list[int],
    span_ends: dict[int, int],
    dispatch: set[int],
    image: Image,
) -> dict[str, list[dict]]:
    out: dict[str, list[dict]] = {
        "dispatched": [],
        "decoded_not_dispatched": [],
        "interior": [],
        "orphan": [],
    }
    for value in sorted(values):
        sites = values[value]
        row = {"target": value, "refs": len(sites), "first_sites": sites[:3]}
        if value in dispatch:
            out["dispatched"].append({"target": value, "refs": len(sites)})
        elif value in starts:
            out["decoded_not_dispatched"].append(row)
        else:
            idx = bisect_right(sorted_starts, value) - 1
            inside = idx >= 0 and value < span_ends[sorted_starts[idx]]
            if inside:
                row["inside_span"] = sorted_starts[idx]
                out["interior"].append(row)
            else:
                row["shape"] = entry_shape(image, value, sorted_starts, span_ends)
                out["orphan"].append(row)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbe", type=Path)
    parser.add_argument("analysis_json", type=Path)
    parser.add_argument("functions_json", type=Path)
    parser.add_argument("dispatcher", type=Path)
    parser.add_argument("--expected-xbe-sha256", required=True)
    args = parser.parse_args()

    data = args.xbe.read_bytes()
    digest = sha256(data)
    if digest != args.expected_xbe_sha256.lower():
        parser.error(f"XBE SHA-256 mismatch: {digest}")
    image = Image(data, json.loads(args.analysis_json.read_text())["sections"])
    functions = json.loads(args.functions_json.read_text())
    starts = {_int(f["start"]) for f in functions}
    sorted_starts = sorted(starts)
    span_ends = {_int(f["start"]): _int(f["end"]) for f in functions}
    dispatch = parse_dispatch(args.dispatcher.read_text())

    result = {
        "xbe_sha256": digest,
        "functions_json_sha256": sha256(args.functions_json.read_bytes()),
        "dispatcher_sha256": sha256(args.dispatcher.read_bytes()),
        "decoded_starts": len(starts),
        "dispatcher_starts": len(dispatch),
    }
    for label, values in (
        ("code_immediates", collect_code_immediates(image, functions)),
        ("data_dwords", collect_data_dwords(image)),
    ):
        groups = classify(values, starts, sorted_starts, span_ends, dispatch, image)
        result[label] = {key: len(items) for key, items in groups.items()}
        result[label + "_candidates"] = {
            "decoded_not_dispatched": groups["decoded_not_dispatched"],
            "orphan": groups["orphan"],
            "interior_count": len(groups["interior"]),
        }
    json.dump(result, sys.stdout, indent=1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
