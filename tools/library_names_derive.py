# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive tools/data/library_names.csv rows from call-graph structure (T1506).

    /workspace/.venv/bin/python -m tools.library_names_derive --xbe build/default.xbe [--write]

Two row sources, both reading only the user's own XBE and the recovered function table:

1. Thunk-target rule. A NAMED library function of at most 40 bytes whose last instruction is
   `jmp rel32` to an UNNAMED library function in a linked XDK section (the XNET and XONLINE
   export thunks `mov ecx,[global]; jmp impl`). The target is the implementation of that
   public API: `<Api>_Impl`. When the target is itself a guard stub (a short function that
   tests ecx and tail-jumps on to another unnamed function), the stub is `<Api>_Guard` and
   the function behind it `<Api>_Impl`. Only targets reached from exactly one named thunk
   qualify. Confidence INFERRED: the public name is exact, the internal method name is unknown.
2. CRT pins, `CRT_PINS` (off with `--without-crt-pins`): each verified against the call targets in
   its body. They are classified library by `tools/data/function_classification.csv` (T1565).
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import defaultdict
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path

import capstone

from tools.xbe.parser import parse_xbe

XDK_SECTIONS = frozenset({"D3D", "XGRPH", "DSOUND", "XONLINE", "XNET", "XMV", "XPP", "DOLBY"})
THUNK_MAX_BYTES = 40
GUARD_MAX_BYTES = 40
COLUMNS = ("entry_va", "name", "confidence", "evidence")
_PLACEHOLDER = re.compile(r"^(?:thunk_)?(?:FUN|SUB|LAB|CODEDIFF)_[0-9a-fA-F]+$")


@dataclass(frozen=True)
class CrtPin:
    entry_va: int
    name: str
    confidence: str
    evidence: str
    #: Call targets in order that the body must contain, so a stale VA cannot be named.
    calls: tuple[int, ...]


CRT_PINS = (
    CrtPin(
        0x3D0109,
        "__atodbl",
        "INFERRED",
        "MEASURED pin: calls ___strgtold12 (0x3d0524) then the double __ld12tod (0x3d00dd, "
        "parameter table 53/11/1023); name from the public MSVC CRT",
        (0x3D0524, 0x3D00DD),
    ),
    CrtPin(
        0x3D0136,
        "__atoflt",
        "INFERRED",
        "MEASURED pin: calls ___strgtold12 (0x3d0524) then the float __ld12tod (0x3d00f3, "
        "parameter table 24/8/127); name from the public MSVC CRT",
        (0x3D0524, 0x3D00F3),
    ),
    CrtPin(
        0x3D029C,
        "__fltout",
        "INFERRED",
        "MEASURED pin: calls ___dtold (0x3d01e2) then $I10_OUTPUT (0x3d0c04), fills the static "
        "STRFLT at 0x772b30; name from the public MSVC CRT",
        (0x3D01E2, 0x3D0C04),
    ),
    CrtPin(
        0x3D0434,
        "_ValidateRead",
        "INFERRED",
        "MEASURED pin: wraps IsBadReadPtr (0x3d17b0), neighbour of the named _ValidateExecute; "
        "name from the public MSVC CRT",
        (0x3D17B0,),
    ),
    CrtPin(
        0x3D044F,
        "_ValidateWrite",
        "INFERRED",
        "MEASURED pin: wraps IsBadWritePtr (0x3d1815), neighbour of the named _ValidateExecute; "
        "name from the public MSVC CRT",
        (0x3D1815,),
    ),
)


def is_placeholder(name: str) -> bool:
    return _PLACEHOLDER.match(name) is not None


def thunk_target(code: bytes, entry_va: int) -> int | None:
    """Target of the final `jmp rel32`, or `None` when the body does not end in one."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    last = None
    for insn in md.disasm(code, entry_va):
        last = insn
    if last is None or last.mnemonic != "jmp" or not last.op_str.startswith("0x"):
        return None
    return int(last.op_str, 16)


def is_guard(code: bytes, entry_va: int) -> bool:
    """A short stub testing ecx in its first instructions, ending in a tail jump."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    first = [f"{i.mnemonic} {i.op_str}" for i in md.disasm(code, entry_va)][:4]
    return "test ecx, ecx" in first and thunk_target(code, entry_va) is not None


def derive_thunk_rows(
    functions: Mapping[int, tuple[str, int]],
    read: Callable[[int, int], bytes],
    section_of: Callable[[int], str | None],
) -> list[tuple[int, str, str, str]]:
    """Rows `(va, name, confidence, evidence)` from the thunk-target rule."""
    reached: dict[int, list[tuple[int, str]]] = defaultdict(list)
    for va, (name, size) in sorted(functions.items()):
        if is_placeholder(name) or size > THUNK_MAX_BYTES or section_of(va) not in XDK_SECTIONS:
            continue
        target = thunk_target(read(va, size), va)
        if target is None or target not in functions:
            continue
        if is_placeholder(functions[target][0]) and section_of(target) in XDK_SECTIONS:
            reached[target].append((va, name))
    rows: list[tuple[int, str, str, str]] = []
    for target, sources in sorted(reached.items()):
        if len({name for _, name in sources}) != 1 or len(sources) != 1:
            continue
        source_va, api = sources[0]
        size = functions[target][1]
        code = read(target, size)
        onward = thunk_target(code, target)
        behind = functions.get(onward) if onward is not None else None
        if (
            size <= GUARD_MAX_BYTES
            and is_guard(code, target)
            and onward is not None
            and behind is not None
            and is_placeholder(behind[0])
            and section_of(onward) in XDK_SECTIONS
        ):
            rows.append(
                (
                    target,
                    f"{api}_Guard",
                    "INFERRED",
                    f"null-ecx guard stub tail-jumped to by named thunk {api} (0x{source_va:08x}), "
                    f"leading to 0x{onward:08x}",
                )
            )
            rows.append(
                (
                    onward,
                    f"{api}_Impl",
                    "INFERRED",
                    f"implementation behind guard 0x{target:08x} of named thunk {api} "
                    f"(0x{source_va:08x}); internal method name unknown",
                )
            )
        else:
            rows.append(
                (
                    target,
                    f"{api}_Impl",
                    "INFERRED",
                    f"sole tail-jmp target of named thunk {api} (0x{source_va:08x}); "
                    "internal method name unknown",
                )
            )
    return rows


WRAPPER_MAX_BYTES = 64


def derive_wrapper_rows(
    functions: Mapping[int, tuple[str, int]],
    read: Callable[[int, int], bytes],
    section_of: Callable[[int], str | None],
    taken: frozenset[int] = frozenset(),
) -> list[tuple[int, str, str, str]]:
    """Rule 3: a short named wrapper with one direct call or jmp (no branches) and an unnamed
    same-section target that no other function references. Name `<Api>_Impl`."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    refs: dict[int, list[int]] = defaultdict(list)
    for va, (_, size) in functions.items():
        for insn in md.disasm(read(va, size), va):
            if insn.mnemonic in ("call", "jmp") and insn.op_str.startswith("0x"):
                target = int(insn.op_str, 16)
                if target in functions and target != va:
                    refs[target].append(va)
    rows: list[tuple[int, str, str, str]] = []
    for target, callers in sorted(refs.items()):
        if len(callers) != 1 or target in taken or not is_placeholder(functions[target][0]):
            continue
        caller = callers[0]
        name, size = functions[caller]
        section = section_of(target)
        if (
            is_placeholder(name)
            or size > WRAPPER_MAX_BYTES
            or section not in XDK_SECTIONS
            or section_of(caller) != section
        ):
            continue
        insns = list(md.disasm(read(caller, size), caller))
        direct = [i for i in insns if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x")]
        branches = [i for i in insns if i.mnemonic.startswith("j") and i.mnemonic != "jmp"]
        if len(direct) != 1 or branches or int(direct[0].op_str, 16) != target:
            continue
        rows.append(
            (
                target,
                f"{name}_Impl",
                "INFERRED",
                f"sole direct {direct[0].mnemonic} target of branch-free {size}-byte named wrapper "
                f"{name} (0x{caller:08x}), referenced by nothing else; method name unknown",
            )
        )
    return rows


def preserve_curated(
    existing: list[tuple[int, str, str, str]],
    derived: list[tuple[int, str, str, str]],
    section_of: Callable[[int], str | None],
) -> list[tuple[int, str, str, str]]:
    """Existing overlay rows that no rule produced and that a rule could not have produced (T1641).

    The thunk and wrapper rules only fire in the XDK sections, so a `.text` row that is not a CRT
    pin is curated (the T1641 reclassification moves the INFERRED names of library functions in
    `.text` here). `--write` used to rewrite the file from the rules alone and silently drop such
    rows.
    Rows whose VA or name a rule re-derived are not kept twice.
    """
    taken_vas = {row[0] for row in derived}
    taken_names = {row[1] for row in derived}
    return [
        row
        for row in existing
        if row[0] not in taken_vas
        and row[1] not in taken_names
        and section_of(row[0]) not in XDK_SECTIONS
    ]


def read_existing(path: Path) -> list[tuple[int, str, str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.reader(handle)
        next(reader, None)
        return [(int(r[0], 16), r[1], r[2], r[3]) for r in reader if len(r) == 4]


def verify_pin(pin: CrtPin, code: bytes) -> bool:
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    calls = [
        int(i.op_str, 16)
        for i in md.disasm(code, pin.entry_va)
        if i.mnemonic == "call" and i.op_str.startswith("0x")
    ]
    return tuple(calls[: len(pin.calls)]) == pin.calls or all(c in calls for c in pin.calls)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, required=True, help="path to the XBE")
    parser.add_argument(
        "--functions",
        type=Path,
        default=Path("generated/retail/functions.csv"),
        help="recovered function table (default: generated/retail/functions.csv)",
    )
    parser.add_argument(
        "--additions",
        type=Path,
        default=Path("tools/data/function_additions.csv"),
        help="tracked function additions (default: tools/data/function_additions.csv)",
    )
    parser.add_argument(
        "--out", type=Path, default=Path("tools/data/library_names.csv"), help="overlay to write"
    )
    parser.add_argument("--write", action="store_true", help="write --out (default: dry run)")
    parser.add_argument(
        "--drop-curated",
        action="store_true",
        help="do not carry over the curated `.text` rows of the existing --out (T1641)",
    )
    parser.add_argument(
        "--without-crt-pins",
        action="store_true",
        help="skip CRT_PINS. On by default since T1565 moved those VAs to library in "
        "tools/data/function_classification.csv, so the overlay counts them",
    )
    args = parser.parse_args(argv)

    data = args.xbe.read_bytes()
    xbe = parse_xbe(data)
    functions: dict[int, tuple[str, int]] = {}
    with args.functions.open(newline="") as handle:
        for row in csv.DictReader(handle):
            functions[int(row["entry_va"], 16)] = (row["name"], int(row["size_bytes"]))
    with args.additions.open(newline="") as handle:
        for row in csv.DictReader(handle):
            functions.setdefault(
                int(row["entry_va"], 16), (f"FUN_{row['entry_va'][2:]}", int(row["size_bytes"]))
            )

    def read(va: int, size: int) -> bytes:
        offset = xbe.va_to_offset(va)
        return data[offset : offset + size] if offset is not None else b""

    def section_of(va: int) -> str | None:
        for section in xbe.sections:
            if section.virtual_addr <= va < section.virtual_addr + section.virtual_size:
                return section.name
        return None

    rows = derive_thunk_rows(functions, read, section_of)
    rows += derive_wrapper_rows(functions, read, section_of, frozenset(r[0] for r in rows))
    for pin in CRT_PINS if not args.without_crt_pins else ():
        name, size = functions.get(pin.entry_va, ("", 0))
        if is_placeholder(name) and verify_pin(pin, read(pin.entry_va, size)):
            rows.append((pin.entry_va, pin.name, pin.confidence, pin.evidence))
        else:
            print(f"pin 0x{pin.entry_va:08x} {pin.name} not applicable", file=sys.stderr)
    seen: set[object] = set()
    unique = []
    for row in sorted(rows):
        if row[0] in seen or row[1] in seen:
            print(f"duplicate va or name {row[0]:#x} {row[1]} dropped", file=sys.stderr)
            continue
        seen.update((row[0], row[1]))
        unique.append(row)
    if not args.drop_curated:
        kept = preserve_curated(read_existing(args.out), unique, section_of)
        unique = sorted([*unique, *kept])
        print(f"{len(kept)} curated rows carried over")
    print(f"{len(unique)} rows")
    if args.write:
        with args.out.open("w", newline="") as handle:
            writer = csv.writer(handle, lineterminator="\n")
            writer.writerow(COLUMNS)
            for va, name, confidence, evidence in unique:
                writer.writerow((f"0x{va:08x}", name, confidence, evidence))
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
