# SPDX-License-Identifier: GPL-3.0-or-later
"""T1620: corpus MXCSR invariant for fp-scalar-v1.

The mode pins MXCSR at 0x1F80 and masks the sticky status bits 0x3F. That is only justified if
the title never executes an instruction that reads or changes MXCSR (`ldmxcsr`, `stmxcsr`) or
saves/restores the whole FP/SSE context (`fxsave`, `fxrstor`, `xsave`, `xrstor`). This tool
measures that on the pinned XBE:

1. every function of functions.csv is decoded (capstone, 32-bit) and any of the six mnemonics
   counts as a hit,
2. every executable section is scanned byte-wise for `0F AE` followed by a memory-form ModRM
   (mod != 3) whose reg field is fxsave(0), fxrstor(1), ldmxcsr(2), stmxcsr(3), xsave(4),
   xrstor(5) or xsaveopt(6). A hit passes only through the address-keyed benign allowlist
   below, each entry pinned to the exact 8 surrounding bytes. `mod == 3` encodes lfence,
   mfence and sfence (benign), and `clflush` (reg 7) is not an MXCSR instruction.

A nonzero count voids fp-scalar-v1 (the fixed word and the status mask are no longer
justified). The pinned XBE sha256 prefix is checked first.

    python -m tools.mxcsr_scan --xbe XBE --functions generated/retail/functions.csv
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

PINNED_XBE_SHA256_PREFIX = "3cfd001a84fc3e08"
MXCSR_MNEMONICS = frozenset(
    {
        "ldmxcsr",
        "stmxcsr",
        "fxsave",
        "fxrstor",
        "fxsave64",
        "fxrstor64",
        "xsave",
        "xsave64",
        "xsaveopt",
        "xsaveopt64",
        "xsavec",
        "xsaves",
        "xrstor",
        "xrstor64",
        "xrstors",
    }
)
#: reg field of `0F AE /r` (mod != 3) that touches MXCSR or the FP/SSE context
RAW_REGS = {
    0: "fxsave",
    1: "fxrstor",
    2: "ldmxcsr",
    3: "stmxcsr",
    4: "xsave",
    5: "xrstor",
    6: "xsaveopt",
}


@dataclass(frozen=True)
class Benign:
    """One address-keyed benign raw hit: the 8 bytes starting at `address` pin the context."""

    address: int
    context_hex: str
    reason: str


#: Filled from the measured scan (see docs/evidence/t1620/scalar-fp-admission.md). Every entry
#: is verified against the bytes, so a drifted image cannot hide behind it.
BENIGN_RAW: tuple[Benign, ...] = (
    Benign(
        0x00537C91,
        "0fae0faf0f000000",
        ".data (flagged executable) u16 table 0x0FA9..0x0FAF, no function range covers it",
    ),
)


def raw_hits(
    sections: list[tuple[int, bytes]], allowlist: tuple[Benign, ...] = BENIGN_RAW
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    """(unexplained hits, allowlisted hits) of the byte scan over executable section bytes."""
    allowed = {item.address: item for item in allowlist}
    bad: list[dict[str, object]] = []
    good: list[dict[str, object]] = []
    for base, data in sections:
        position = data.find(b"\x0f\xae")
        while position != -1:
            if position + 2 < len(data):
                modrm = data[position + 2]
                reg = (modrm >> 3) & 7
                if (modrm >> 6) != 3 and reg in RAW_REGS:
                    address = base + position
                    context = data[position : position + 8].hex()
                    row = {
                        "address": f"0x{address:08x}",
                        "instruction": RAW_REGS[reg],
                        "context": context,
                    }
                    entry = allowed.get(address)
                    if entry is not None and entry.context_hex == context:
                        row["reason"] = entry.reason
                        good.append(row)
                    else:
                        bad.append(row)
            position = data.find(b"\x0f\xae", position + 1)
    return bad, good


def decoded_hits(
    functions: list[tuple[int, int]], read: Callable[[int, int], bytes]
) -> tuple[int, int, list[dict[str, object]]]:
    """Decode every function; returns (functions scanned, instructions decoded, hits)."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    hits: list[dict[str, object]] = []
    instructions = 0
    for va, size in functions:
        code = read(va, size)
        for insn in decoder.disasm(code, va):
            instructions += 1
            if insn.mnemonic.lower() in MXCSR_MNEMONICS:
                hits.append({"address": f"0x{insn.address:08x}", "instruction": insn.mnemonic})
    return len(functions), instructions, hits


def scan(xbe_path: Path, functions_csv: Path) -> dict[str, object]:
    from tools.xbe import parse_xbe

    raw = xbe_path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    pinned = digest.startswith(PINNED_XBE_SHA256_PREFIX)
    xbe = parse_xbe(raw)
    sections: list[tuple[int, bytes]] = []
    for section in xbe.sections:
        if section.executable and section.raw_size > 0:
            length = min(section.raw_size, section.virtual_size)
            sections.append(
                (section.virtual_addr, raw[section.raw_addr : section.raw_addr + length])
            )

    def read(va: int, size: int) -> bytes:
        for base, data in sections:
            if base <= va and va + size <= base + len(data):
                return data[va - base : va - base + size]
        return b""

    with functions_csv.open(newline="", encoding="utf-8") as handle:
        functions = [
            (int(row["entry_va"], 16), int(row["size_bytes"])) for row in csv.DictReader(handle)
        ]
    scanned, instructions, decoded = decoded_hits(functions, read)
    bad, good = raw_hits(sections)
    return {
        "xbe_sha256_prefix": digest[:16],
        "xbe_pinned": pinned,
        "executable_sections": [
            {"address": f"0x{base:08x}", "bytes": len(data)} for base, data in sections
        ],
        "functions_decoded": scanned,
        "instructions_decoded": instructions,
        "decoded_hits": decoded,
        "raw_hits_unexplained": bad,
        "raw_hits_allowlisted": good,
        "count": len(decoded) + len(bad),
        "passed": pinned and not decoded and not bad,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--functions", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=None, help="write the JSON result here")
    args = parser.parse_args(argv)
    result = scan(args.xbe, args.functions)
    text = json.dumps(result, indent=2, sort_keys=True)
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
