# SPDX-License-Identifier: GPL-3.0-or-later
"""T1497 corpus check for the aligned data pointer rule of the T567 caller audit.

The rule: a pointer-shaped dword that starts at a non multiple of 4 address inside a DATA
section is a straddle of neighbouring constants, not a code pointer. This script counts every
unaligned in-scope dword (a data section dword whose value is strictly inside a lifted function
body) and looks for a counterexample, an unaligned address that code could load as a pointer:

- an instruction operand (imm32 or disp32, any instruction of any lifted body) equal to it,
- an unaligned table base operand walked in 4 byte steps (a `[reg*4 + table]` read) that reaches it,
- an aligned dword anywhere in the image equal to its address (a pointer table slot).

Exit status 1 when a counterexample exists that the rule would still apply to (the audit
withholds the rule for any address whose own 4 byte value occurs in the image). Linear decode
per lifted body is a superset of the
reachable code, so a false counterexample is possible, a missed one only through computed
addresses (`base + offset` arithmetic), the stated residual.
"""

from __future__ import annotations

import argparse
import bisect
import json
import sys
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from tools.harness.image import GuestImage

DATA_SECTIONS = (".rdata", ".data", ".data1")


def data_ranges(xbe_path: Path) -> list[tuple[str, int, int]]:
    """(name, first va, end va) of every data section (initialised bytes only).

    (T1252 R17) Also every section whose XBE flags lack EXECUTABLE (inserted resource files such as
    `$$XTIMAGE`, `dsstdfx`, `xbreadfail_*`, `.XTLID`: bitmaps, fonts, audio). No code lives there
    (`functions.csv` holds no function start inside them, checked by
    `tests/test_t1252_r17_resource_sections.py` against the owner XBE when present), so the same
    aligned data pointer rule applies, with a stronger argument than for `.rdata`.
    """
    from tools.xbe import parse_xbe

    xbe = parse_xbe(xbe_path.read_bytes())
    return [
        (s.name, s.virtual_addr, s.virtual_addr + min(s.raw_size, s.virtual_size))
        for s in xbe.sections
        if s.name in DATA_SECTIONS or not s.executable
    ]


def operand_values(
    entries: list[int], image: GuestImage, limit: int
) -> tuple[dict[int, dict[str, int]], dict[int, int]]:
    """(operand value -> hits by kind, indexed memory displacement -> hits) per lifted body.

    Kinds: `mem4` a 4 byte memory operand (a dword load or store at the address, `lea` aside),
    `memN` a memory operand of another width (byte, word, float, string op), `taken` an
    immediate or `lea` (the address escapes as a value, what it points at is not known).
    """
    from tools.replace.audit import _make_decoder

    decoder = _make_decoder()
    plain: dict[int, dict[str, int]] = {}
    scaled: dict[int, int] = {}
    for position, entry in enumerate(entries):
        end = entries[position + 1] if position + 1 < len(entries) else limit
        end = min(end, image.base + len(image.data))
        raw = image.data[entry - image.base : end - image.base]
        cursor = 0
        while cursor < len(raw):
            insn = next(iter(decoder.disasm(raw[cursor : cursor + 16], entry + cursor, 1)), None)
            if insn is None:
                cursor += 1
                continue
            cursor += insn.size
            for op in insn.operands:
                if op.type == 2:  # CS_OP_IMM
                    if insn.mnemonic.startswith(("j", "call", "loop")):
                        continue  # capstone reports the resolved target of a relative branch
                    kind, value = "taken", op.imm & 0xFFFFFFFF
                elif op.type == 3 and op.mem.disp:  # CS_OP_MEM with a displacement
                    value = op.mem.disp & 0xFFFFFFFF
                    kind = (
                        "taken" if insn.mnemonic == "lea" else ("mem4" if op.size == 4 else "memN")
                    )
                else:
                    continue
                hits = plain.setdefault(value, {})
                hits[kind] = hits.get(kind, 0) + 1
                if op.type == 3 and op.mem.index:
                    scaled[value] = scaled.get(value, 0) + 1
    return plain, scaled


def run(xbe_path: Path, gen_dir: Path) -> dict:
    from tools.harness.image import build_guest_image
    from tools.replace.audit import build_caller_index
    from tools.xbe import parse_xbe

    image = build_guest_image(xbe_path)
    entries = build_caller_index(gen_dir).entries
    entry_set = set(entries)
    ranges = data_ranges(xbe_path)
    base, data = image.base, image.data
    text = parse_xbe(xbe_path.read_bytes()).section_by_name(".text")
    text_end = text.virtual_addr + text.virtual_size
    plain, scaled = operand_values(entries, image, text_end)

    def interior(value: int) -> bool:
        slot = bisect.bisect_right(entries, value) - 1
        if slot < 0 or value == entries[slot]:
            return False
        end = entries[slot + 1] if slot + 1 < len(entries) else text_end
        return value < end

    low_pointer, high_pointer = entries[0], text_end

    def dword(address: int) -> int:
        at = address - base
        return int.from_bytes(data[at : at + 4], "little")

    per_section: dict[str, int] = {}
    in_scope: list[int] = []
    entry_valued = 0
    aligned_slots = 0
    for name, low, high in ranges:
        count = 0
        for address in range(low, high - 3):
            value = dword(address)
            if not low_pointer <= value < high_pointer:
                continue
            if address % 4 == 0:
                aligned_slots += 1
            elif interior(value):
                count += 1
                in_scope.append(address)
            elif value in entry_set:
                entry_valued += 1
        per_section[name] = count
    scope = set(in_scope)
    counter: list[dict[str, str]] = []
    address_taken: list[int] = []
    taken = 0
    for address in in_scope:
        hits = plain.get(address, {})
        if hits.get("mem4"):
            counter.append({"address": hex(address), "why": "4 byte memory operand reads it"})
        if hits.get("taken"):
            taken += 1
            address_taken.append(address)
    tables = sorted(v for v in scaled if v % 4 and any(lo <= v < hi for _, lo, hi in ranges))
    for table in tables:
        walk = table
        while walk in scope or low_pointer <= dword(walk) < high_pointer:
            if walk in scope:
                counter.append({"address": hex(walk), "why": f"indexed table {table:#x}"})
            walk += 4
    pointer_slots = 0
    for address in in_scope:
        needle = address.to_bytes(4, "little")
        position = data.find(needle)
        while position != -1:
            slot = base + position
            if slot % 4 == 0 and any(lo <= slot < hi for _, lo, hi in ranges):
                pointer_slots += 1
                counter.append({"address": hex(address), "why": f"aligned data slot {slot:#x}"})
            position = data.find(needle, position + 1)
    from tools.replace.audit import _address_escapes

    applies = [a for a in in_scope if not _address_escapes(data, a)]
    escaped_counter = [c for c in counter if int(c["address"], 16) in set(applies)]
    return {
        "counterexamples_not_withheld_by_rule": escaped_counter,
        "data_sections": [(n, hex(lo), hex(hi)) for n, lo, hi in ranges],
        "lifted_entries": len(entries),
        "aligned_code_pointer_shaped_data_dwords": aligned_slots,
        "unaligned_in_scope_per_section": per_section,
        "unaligned_in_scope_total": len(in_scope),
        "unaligned_entry_valued_not_in_scope": entry_valued,
        "operand_values_decoded": len(plain),
        "unaligned_data_table_bases": [hex(v) for v in tables],
        "aligned_data_slots_pointing_at_in_scope": pointer_slots,
        "address_taken_in_scope": taken,
        "address_taken_addresses": [hex(a) for a in address_taken],
        "counterexamples": counter,
        "rule_applies_to": len(applies),
        "rule_withheld_for": len(in_scope) - len(applies),
        "in_scope_addresses": [hex(a) for a in in_scope],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    parser.add_argument("--gen-dir", type=Path, default=Path("generated/lifted/gen"))
    parser.add_argument("--out", type=Path, default=None, help="write the full JSON here")
    args = parser.parse_args(argv)
    result = run(args.xbe, args.gen_dir)
    if args.out:
        args.out.write_text(json.dumps(result, indent=1))
    big = ("in_scope_addresses", "address_taken_addresses")
    summary = {k: v for k, v in result.items() if k not in big}
    print(json.dumps(summary, indent=1))
    return 1 if result["counterexamples_not_withheld_by_rule"] else 0


if __name__ == "__main__":
    sys.exit(main())
