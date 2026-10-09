# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent original/native ordinary and stack-alias controls for T1782.

No proof tuple is executed. Uses the frozen original-derived domain as input;
expected registers and full changed memory come only from retail instructions.
The relocated-stack controls put a successful root's reads/writes on argument,
local and saved-register slots while preserving its actual return address.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import struct
import subprocess
from dataclasses import replace
from pathlib import Path
from typing import Any

import capstone

from tools.harness.image import GuestImage, build_guest_image
from tools.harness.model import Case, ExecResult
from tools.harness.oracle import UnicornOracle
from tools.harness.seeding import GUEST_LO, GUEST_SPAN, SENTINEL
from tools.harness.synth_domain import derive

DATA = Path("docs/data/t1782-synth-group-b")


def build(sources: list[Path], opt: int, work: Path, mutant: int = 0) -> ctypes.CDLL:
    dest = work / f"native-o{opt}-m{mutant:x}.so"
    cmd = [
        "cc",
        "-std=c11",
        f"-O{opt}",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-shared",
        "-fPIC",
        "-Isrc/game",
    ]
    if mutant:
        cmd += [f"-DSYNTH_MUTANT_VA=0x{mutant:x}u"]
    cmd += ["tests/c/t1782/native_bridge.c", "src/game/game_registry.c"]
    for source in sources:
        cmd += ["-x", "c", str(source)]
    cmd += ["-o", str(dest)]
    subprocess.run(cmd, check=True, capture_output=True, timeout=120)
    lib = ctypes.CDLL(str(dest.resolve()))
    lib.native_memory.restype = ctypes.c_void_p
    lib.native_execute.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_int]
    return lib


def check(lib: ctypes.CDLL, image: bytes, case: Case, result: ExecResult) -> list[str]:
    ptr = lib.native_memory()
    initial = bytearray(image)
    for address, data in case.patches:
        initial[address - GUEST_LO : address - GUEST_LO + len(data)] = data
    initial_bytes = bytes(initial)
    ctypes.memmove(ptr + GUEST_LO, initial_bytes, len(initial_bytes))
    regs = (ctypes.c_uint32 * 8)(*case.regs)
    assert lib.native_execute(case.va, regs, case.df) == 0
    problems = []
    if tuple(regs) != result.regs:
        problems.append(f"registers native={list(regs)} original={list(result.regs)}")
    for address, value in result.writes.items():
        initial[address - GUEST_LO] = value
    actual = ctypes.string_at(ptr + GUEST_LO, GUEST_SPAN)
    if actual != bytes(initial):
        offsets = [
            i + GUEST_LO for i, (a, b) in enumerate(zip(actual, initial, strict=True)) if a != b
        ]
        problems.append(f"memory {len(offsets)} different bytes; first={offsets[:12]}")
    return problems


def saved_slots(image: GuestImage, case: Case, result: ExecResult) -> list[dict[str, Any]]:
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    offset = 0
    slots = []
    for insn in md.disasm(image.code_at(case.va, case.size), case.va):
        if insn.address not in result.reach.covered_vas:
            continue
        if insn.mnemonic == "sub" and insn.op_str.startswith("esp, "):
            offset -= int(insn.op_str.split(", ")[1], 0)
        if insn.mnemonic == "add" and insn.op_str.startswith("esp, "):
            offset += int(insn.op_str.split(", ")[1], 0)
        if insn.mnemonic == "push":
            offset -= 4
            reg = insn.op_str
            from tools.harness.model import REG_NAMES

            if reg in REG_NAMES:
                address = case.esp + offset
                value = case.regs[REG_NAMES.index(reg)]
                initial = bytearray(image.data)
                for a, data in case.patches:
                    initial[a - GUEST_LO : a - GUEST_LO + len(data)] = data
                final = bytes(
                    result.writes.get(address + i, initial[address + i - GUEST_LO])
                    for i in range(4)
                )
                slot_value = int.from_bytes(final, "little")
                slots.append(
                    dict(
                        reg=reg,
                        address=hex(address),
                        entry=value,
                        final_slot=slot_value,
                        exit=result.regs[REG_NAMES.index(reg)],
                        overwritten=slot_value != value,
                        saved_slot_load_count=sum(
                            a <= address + 3 and address < a + n for a, n, _ in result.reach.loads
                        ),
                    )
                )
        if insn.mnemonic == "pop":
            offset += 4
        if insn.mnemonic == "ret":
            break
    return slots


def controls(
    image: GuestImage, oracle: UnicornOracle, row: dict[str, Any]
) -> list[tuple[Case, ExecResult, str]]:
    va = int(row["va"], 16)
    domain = derive(va, row["size"], image)
    ordinary = []
    for ordinal in range(domain.case_count()):
        case = domain.make_case(20261001, ordinal)
        result = oracle.run(case)
        if not result.faulted:
            ordinary.append((case, result, "ordinary"))
        if len(ordinary) == 16:
            break
    assert ordinary, f"{va:x}: no clean controls"
    aliases = []
    seen = set()
    # Choose real accessed addresses, not guessed structure offsets. Exclude the
    # default stack; retain only successful returns after the stack relocation.
    for case, result, _ in ordinary:
        candidates = sorted(set(result.writes) | {a for a, _, _ in result.reach.loads})
        candidates = [
            a for a in candidates if abs(a - case.esp) > 0x200 and GUEST_LO + 0x100 <= a < 0xF00000
        ]
        writes = sorted(a for a in result.writes if abs(a - case.esp) > 0x200)
        ends = [a for i, a in enumerate(writes) if i == len(writes) - 1 or writes[i + 1] != a + 1]
        if len(candidates) > 40:
            candidates = candidates[:20] + candidates[-20:]
        candidates = list(dict.fromkeys(ends + candidates))
        for address in candidates:
            for delta in (-4, 4, 8, 12, 16, 20):
                stack = (address + delta) & ~3
                if stack in seen:
                    continue
                seen.add(stack)
                # Copy the actual root arguments to the relocated frame. Patch
                # last, so domain prefill cannot silently erase the return slot.
                initial = bytearray(image.data)
                for a, data in case.patches:
                    initial[a - GUEST_LO : a - GUEST_LO + len(data)] = data
                count = max(row["abi"]["stack_args"], 16)
                args = bytes(initial[case.esp + 4 - GUEST_LO : case.esp + 4 + 4 * count - GUEST_LO])
                frame = struct.pack("<I", SENTINEL) + args
                regs = list(case.regs)
                regs[4] = stack
                aliased = replace(case, regs=tuple(regs), patches=case.patches + ((stack, frame),))
                original = oracle.run(aliased)
                if original.faulted or original.regs[4] != stack + 4:
                    continue
                aliases.append((aliased, original, "relocated-stack-alias"))
                if len(aliases) >= 48:
                    break
            if len(aliases) >= 48:
                break
        if len(aliases) >= 48:
            break
    assert aliases, f"{va:x}: no valid alias state"
    aliases.sort(
        key=lambda item: sum(slot["overwritten"] for slot in saved_slots(image, item[0], item[1])),
        reverse=True,
    )
    return ordinary + aliases[:12]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--band", choices=("T1477", "T1478"), required=True)
    parser.add_argument("--source", action="append", type=Path)
    parser.add_argument("--opt", action="append", type=int)
    parser.add_argument("--va", action="append", default=[])
    parser.add_argument("--mutants", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    rows = json.loads((DATA / f"{args.band}-audit-list.json").read_text())
    if args.va:
        wanted = {int(v, 16) for v in args.va}
        rows = [r for r in rows if int(r["va"], 16) in wanted]
    sources = args.source or [DATA / "drafts" / (r["va"][2:].lower() + ".c.txt") for r in rows]
    work = Path("tmp/native-t1782")
    work.mkdir(parents=True, exist_ok=True)
    image = build_guest_image(Path("build/default.xbe"))
    oracle = UnicornOracle(image.data, max_insns=200000, record_loads=True)
    prepared = {r["va"]: controls(image, oracle, r) for r in rows}
    records = []
    failed = False
    for opt in args.opt or (0, 2, 3):
        lib = build(sources, opt, work)
        for row in rows:
            tests = prepared[row["va"]]
            issues = []
            for case, result, kind in tests:
                problems = check(lib, image.data, case, result)
                if problems:
                    issues.append(
                        {
                            "seed": case.seed,
                            "index": case.index,
                            "esp": hex(case.esp),
                            "kind": kind,
                            "problems": problems,
                        }
                    )
            receipt = {
                "va": row["va"],
                "opt": opt,
                "ordinary": sum(k == "ordinary" for _, _, k in tests),
                "alias": sum(k != "ordinary" for _, _, k in tests),
                "issues": issues,
                "alias_states": [
                    {
                        "esp": hex(c.esp),
                        "index": c.index,
                        "original_regs": list(r.regs),
                        "entry_regs": list(c.regs),
                        "case_sha256": hashlib.sha256(repr(c).encode()).hexdigest(),
                        "saved_slots": saved_slots(image, c, r),
                        "writes_sha256": hashlib.sha256(
                            json.dumps(r.writes, sort_keys=True).encode()
                        ).hexdigest(),
                    }
                    for c, r, k in tests
                    if k != "ordinary"
                ],
            }
            if args.mutants and opt in (0, 3):
                mutant = build(sources, opt, work, int(row["va"], 16))
                receipt["compiled_mutant_killed"] = any(
                    check(mutant, image.data, c, r) for c, r, _ in tests
                )
                issues += [] if receipt["compiled_mutant_killed"] else ["mutant survived"]
            records.append(receipt)
            failed |= bool(issues)
            args.out.write_text(json.dumps(records, indent=1) + "\n")
            print(
                row["va"],
                f"O{opt}",
                "FAIL" if issues else "PASS",
                len(tests),
                issues[:1],
                flush=True,
            )
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
