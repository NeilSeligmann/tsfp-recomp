# SPDX-License-Identifier: GPL-3.0-or-later
"""T123 census of unresolved interior continuation targets.

The lift emits an empty stub for every address that translated code calls or
tail-branches to but that the function detector never recorded as a function
start (``gen/recomp_stubs_unresolved.c``). Most of those addresses land inside
a recorded body. Body bounds alone prove nothing about the landing site: the
address may not even be an instruction boundary of the owner's linear decode,
and code there may consume frame slots or flags the continuation never set up.

This tool regenerates the census that previously lived only in /tmp and
machine-classifies every unresolved address:

* ``EXTERIOR``      not inside any recorded body (bounds say nothing either way)
* ``UNDECODABLE``   inside a body but not on a boundary of its linear decode
* ``UNSAFE``        boundary-valid, but reads inherited flags, an EBP-addressed
                    slot, or an ESP slot it did not push, before writing them
* ``SAFE_LOOKING``  boundary-valid with no inherited frame/flags reads before
                    the scan ends (still unproven: inherited general registers
                    and anything past the scan horizon stay unverified)

Nothing is recovered here. The output is evidence for deciding whether any
class justifies recovery work later.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import json
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

import capstone
from capstone import x86 as cs_x86

CLASS_SAFE = "SAFE_LOOKING"
CLASS_UNSAFE = "UNSAFE"
CLASS_UNDECODABLE = "UNDECODABLE"
CLASS_EXTERIOR = "EXTERIOR"

# EFLAGS read/write masks built from capstone's own per-instruction metadata.
# TEST_* bits are reads. MODIFY/RESET/SET/UNDEFINED/PRIOR bits are writes
# (an UNDEFINED flag is still not the inherited value, so it counts as a
# write for read-before-write purposes).
_FLAGS_READ_MASK = 0
_FLAGS_WRITE_MASK = 0
for _name in dir(cs_x86):
    if not _name.startswith("X86_EFLAGS_"):
        continue
    _value = getattr(cs_x86, _name)
    if _name.startswith("X86_EFLAGS_TEST_"):
        _FLAGS_READ_MASK |= _value
    elif _name.startswith(
        (
            "X86_EFLAGS_MODIFY_",
            "X86_EFLAGS_RESET_",
            "X86_EFLAGS_SET_",
            "X86_EFLAGS_UNDEFINED_",
            "X86_EFLAGS_PRIOR_",
        )
    ):
        _FLAGS_WRITE_MASK |= _value

_UNCONDITIONAL_ENDS = {"ret", "retn", "retf", "jmp", "ljmp", "int3", "hlt", "ud2", "iret", "iretd"}
_CALLEE_SAVED = {cs_x86.X86_REG_EBX: "ebx", cs_x86.X86_REG_ESI: "esi", cs_x86.X86_REG_EDI: "edi"}

_EBP_REGS = {cs_x86.X86_REG_EBP, cs_x86.X86_REG_BP}
_ESP_REGS = {cs_x86.X86_REG_ESP, cs_x86.X86_REG_SP}


def _disassembler() -> capstone.Cs:
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return md


def linear_decode(body: bytes, start: int) -> list:
    """Linear-sweep decode of one recorded body. Stops at the first byte
    capstone cannot decode, exactly like the lifter's linear view: anything
    past a decode gap has no proven boundary."""
    md = _disassembler()
    out = []
    offset = 0
    while offset < len(body):
        decoded = list(md.disasm(body[offset:], start + offset, count=1))
        if not decoded:
            break
        insn = decoded[0]
        out.append(insn)
        offset += insn.size
    return out


def _is_cond_jump(insn: capstone.CsInsn) -> bool:
    return capstone.CS_GRP_JUMP in insn.groups and insn.mnemonic not in ("jmp", "ljmp")


def _jump_target(insn: capstone.CsInsn) -> int | None:
    if capstone.CS_GRP_JUMP not in insn.groups and insn.mnemonic != "call":
        return None
    ops = insn.operands
    if len(ops) == 1 and ops[0].type == cs_x86.X86_OP_IMM:
        return ops[0].imm
    return None


def entry_shape(instructions: list, target: int, noreturn_starts: frozenset = frozenset()) -> dict:
    """Describe how the owner's linear decode arrives at ``target``."""
    pred = None
    for insn in instructions:
        if insn.address + insn.size == target:
            pred = insn
            break
    shape = "mid_block"
    if pred is None:
        shape = "no_linear_predecessor"
    elif pred.mnemonic == "call":
        callee = _jump_target(pred)
        if callee is not None and callee in noreturn_starts:
            shape = "after_noreturn_call"
        else:
            shape = "after_call"
    elif pred.mnemonic in _UNCONDITIONAL_ENDS:
        shape = "block_start"
    elif _is_cond_jump(pred):
        shape = "cond_fallthrough"
    backward = False
    branched = False
    for insn in instructions:
        if insn.mnemonic == "call":
            continue
        jump = _jump_target(insn)
        if jump == target:
            branched = True
            if insn.address > target:
                backward = True
    return {
        "entry_shape": shape,
        "is_loop_head": backward,
        "is_branch_target_in_body": branched,
        "predecessor": None
        if pred is None
        else f"0x{pred.address:08X} {pred.mnemonic} {pred.op_str}".rstrip(),
    }


@dataclass
class ContractScan:
    """Read-before-write facts observed from the target forward."""

    flags_read_before_write: bool = False
    ebp_dependent: bool = False
    esp_slot_read: bool = False
    caller_slot_writes: bool = False
    inherited_gpr_reads: list = field(default_factory=list)
    steps: int = 0
    calls_crossed: int = 0
    end_reason: str = "cap"
    first_unsafe: str | None = None

    def unsafe(self) -> bool:
        return self.flags_read_before_write or self.ebp_dependent or self.esp_slot_read


def scan_contract(instructions: list, target: int, *, max_steps: int = 200) -> ContractScan:
    """Walk the linear decode from ``target`` and record which inherited
    state (flags, EBP addressing, caller stack slots) is read before this
    continuation writes it. Follows fallthrough only. A call clobbers flags
    and the caller-saved registers but leaves the frame question open, so
    the walk continues through it."""
    result = ContractScan()
    index = next((i for i, insn in enumerate(instructions) if insn.address == target), None)
    if index is None:
        result.end_reason = "target_not_on_boundary"
        return result

    flags_written = False
    ebp_written = False
    # Bytes this continuation pushed below the entry ESP. Negative depth
    # means it has already released stack it never allocated, so the live
    # ESP sits inside the caller's frame.
    own_depth = 0
    # Entry-relative offsets (0 = the slot ESP pointed at on entry, the
    # return address of the lifted transfer) this continuation has itself
    # written. A later read of a written slot is not an inherited read.
    written_slots: set[int] = set()
    gpr_written = set()
    gpr_read = set()

    def mark(reason: str, insn: capstone.CsInsn) -> None:
        if result.first_unsafe is None:
            result.first_unsafe = (
                f"0x{insn.address:08X} {insn.mnemonic} {insn.op_str}".rstrip() + f" [{reason}]"
            )

    for insn in instructions[index : index + max_steps]:
        result.steps += 1
        mnemonic = insn.mnemonic

        # Inherited flags: capstone marks the flags this instruction tests.
        if not flags_written and (insn.eflags & _FLAGS_READ_MASK):
            if not result.flags_read_before_write:
                result.flags_read_before_write = True
                mark("reads inherited flags", insn)
        if insn.eflags & _FLAGS_WRITE_MASK:
            flags_written = True

        try:
            regs_read, regs_written = insn.regs_access()
        except capstone.CsError:
            regs_read, regs_written = (), ()

        # EBP dependence: any read of the inherited EBP value except a plain
        # save (push ebp never dereferences it).
        if not ebp_written and mnemonic != "push":
            if any(reg in _EBP_REGS for reg in regs_read):
                if not result.ebp_dependent:
                    result.ebp_dependent = True
                    mark("uses inherited ebp", insn)
        if any(reg in _EBP_REGS for reg in regs_written):
            ebp_written = True

        # Inherited callee-saved register reads (information only, not an
        # UNSAFE trigger by the T123 definition).
        for reg, name in _CALLEE_SAVED.items():
            if reg in regs_read and reg not in gpr_written and name not in gpr_read:
                gpr_read.add(name)
            if reg in regs_written:
                gpr_written.add(reg)

        # Stack slots, tracked as offsets relative to the entry ESP so a
        # slot the continuation wrote itself stops counting as inherited.
        def slot_read(offset: int, reason: str, insn: capstone.CsInsn = insn) -> None:
            if offset >= 0 and offset not in written_slots and not result.esp_slot_read:
                result.esp_slot_read = True
                mark(reason, insn)

        def slot_write(offset: int) -> None:
            if offset >= 0:
                written_slots.add(offset)
                result.caller_slot_writes = True

        if mnemonic == "push":
            slot_write(-own_depth - 4)
            own_depth += 4
        elif mnemonic == "pop":
            slot_read(-own_depth, "pops a caller-owned slot")
            own_depth -= 4
        elif mnemonic in ("ret", "retn"):
            # The terminal return consumes the return address the caller
            # pushed for this very transfer: that is the one inherited slot
            # every continuation legitimately owns. A return at any other
            # depth consumes some other caller-owned slot instead.
            if own_depth != 0:
                slot_read(-own_depth, "returns through a caller-owned slot")
            result.end_reason = "ret"
            break
        elif mnemonic == "leave":
            # leave = mov esp, ebp / pop ebp: full inherited-frame use,
            # already caught by the EBP read above.
            own_depth = 0
        elif (
            mnemonic in ("sub", "add")
            and insn.operands
            and insn.operands[0].type == cs_x86.X86_OP_REG
            and insn.operands[0].reg in _ESP_REGS
            and len(insn.operands) == 2
            and insn.operands[1].type == cs_x86.X86_OP_IMM
        ):
            amount = insn.operands[1].imm
            own_depth += amount if mnemonic == "sub" else -amount
        elif mnemonic not in ("lea", "nop", "call"):
            for op in insn.operands:
                if op.type != cs_x86.X86_OP_MEM:
                    continue
                if op.mem.base not in _ESP_REGS:
                    continue
                if op.mem.index != 0:
                    # An indexed slot has no static offset. A read through
                    # it may touch any caller slot, so stay conservative.
                    if (op.access & capstone.CS_AC_READ) and not result.esp_slot_read:
                        result.esp_slot_read = True
                        mark("reads an indexed esp slot", insn)
                    continue
                offset = op.mem.disp - own_depth
                if op.access & capstone.CS_AC_READ:
                    slot_read(offset, "reads a caller-owned esp slot")
                if op.access & capstone.CS_AC_WRITE:
                    slot_write(offset)

        if mnemonic == "call":
            # The callee owns flags and the caller-saved registers afterwards.
            result.calls_crossed += 1
            flags_written = True
        elif mnemonic in _UNCONDITIONAL_ENDS:
            result.end_reason = mnemonic
            break
    else:
        result.end_reason = "cap" if result.steps >= max_steps else "body_end"

    result.inherited_gpr_reads = sorted(gpr_read)
    return result


def classify_interior_target(
    body: bytes,
    body_start: int,
    target: int,
    *,
    noreturn_starts: frozenset = frozenset(),
    max_steps: int = 200,
) -> dict:
    """Classify one unresolved address inside one recorded body."""
    instructions = linear_decode(body, body_start)
    boundaries = {insn.address for insn in instructions}
    decoded_end = instructions[-1].address + instructions[-1].size if instructions else body_start
    record: dict = {
        "boundary_valid": target in boundaries,
        "decoded_end": f"0x{decoded_end:08X}",
    }
    if target not in boundaries:
        record["class"] = CLASS_UNDECODABLE
        record["undecodable_reason"] = (
            "linear decode stopped before the target"
            if target >= decoded_end
            else "target is not an instruction start of the linear decode"
        )
        return record
    record.update(entry_shape(instructions, target, noreturn_starts))
    scan = scan_contract(instructions, target, max_steps=max_steps)
    record["contract"] = {
        "flags_read_before_write": scan.flags_read_before_write,
        "ebp_dependent": scan.ebp_dependent,
        "esp_slot_read": scan.esp_slot_read,
        "caller_slot_writes": scan.caller_slot_writes,
        "inherited_gpr_reads": scan.inherited_gpr_reads,
        "steps": scan.steps,
        "calls_crossed": scan.calls_crossed,
        "end_reason": scan.end_reason,
        "first_unsafe": scan.first_unsafe,
    }
    record["class"] = CLASS_UNSAFE if scan.unsafe() else CLASS_SAFE
    return record


# --------------------------------------------------------------------------
# Real-tree wiring


def parse_unresolved_stub_addresses(stub_source: str) -> list[int]:
    return sorted(
        int(match, 16) for match in re.findall(r"void sub_([0-9A-Fa-f]{8})\(void\)", stub_source)
    )


class XbeImage:
    """Minimal VA reader over the raw XBE using the lift's own analysis."""

    def __init__(self, xbe_path: Path, analysis: dict) -> None:
        self.data = xbe_path.read_bytes()
        self.sha256 = hashlib.sha256(self.data).hexdigest()
        self.sections = []
        for section in analysis["sections"]:
            self.sections.append(
                (
                    int(section["virtual_addr"], 16),
                    int(section["virtual_addr"], 16) + section["raw_size"],
                    int(section["raw_addr"], 16),
                )
            )
        self.sections.sort()

    def read(self, va: int, size: int) -> bytes | None:
        for virtual_start, virtual_end, raw in self.sections:
            if virtual_start <= va and va + size <= virtual_end:
                offset = raw + (va - virtual_start)
                return self.data[offset : offset + size]
        return None

    def count_address_taken(self, va: int) -> int:
        return self.data.count(struct.pack("<I", va))


def run_census(xbe_path: Path, lifted: Path, max_steps: int) -> dict:
    stub_path = lifted / "gen" / "recomp_stubs_unresolved.c"
    addresses = parse_unresolved_stub_addresses(stub_path.read_text(encoding="utf-8"))
    analysis = json.loads((lifted / "xbe_analysis.json").read_text())
    functions = json.loads((lifted / "disasm" / "functions.json").read_text())
    image = XbeImage(xbe_path, analysis)

    xref_types: dict[int, dict] = {}
    xrefs_path = lifted / "disasm" / "xrefs.json"
    if xrefs_path.is_file():
        wanted = set(addresses)
        for xref in json.loads(xrefs_path.read_text()):
            to = int(xref["to"], 16)
            if to in wanted:
                bucket = xref_types.setdefault(to, {})
                bucket[xref["type"]] = bucket.get(xref["type"], 0) + 1

    bodies = sorted((int(f["start"], 16), int(f["start"], 16) + f["size"], f) for f in functions)
    starts = [b[0] for b in bodies]
    noreturn_starts = frozenset(int(f["start"], 16) for f in functions if f.get("noreturn"))

    records = []
    counts = {CLASS_SAFE: 0, CLASS_UNSAFE: 0, CLASS_UNDECODABLE: 0, CLASS_EXTERIOR: 0}
    shape_counts: dict[str, int] = {}
    for address in addresses:
        owner = None
        pivot = bisect.bisect_right(starts, address) - 1
        for j in range(max(0, pivot - 8), pivot + 1):
            body_start, body_end, info = bodies[j]
            if body_start < address < body_end:
                owner = (body_start, body_end, info)
        record: dict = {
            "address": f"0x{address:08X}",
            "name": f"sub_{address:08X}",
            "address_taken_count": image.count_address_taken(address),
            "incoming_xrefs": xref_types.get(address, {}),
        }
        if owner is None:
            record["class"] = CLASS_EXTERIOR
            record["owner"] = None
        else:
            body_start, body_end, info = owner
            body = image.read(body_start, body_end - body_start)
            record["owner"] = {
                "start": info["start"],
                "end": f"0x{body_end:08X}",
                "name": info["name"],
                "detection_method": info.get("detection_method", ""),
                "noreturn": bool(info.get("noreturn")),
            }
            if body is None:
                record["class"] = CLASS_UNDECODABLE
                record["undecodable_reason"] = "body bytes unreadable"
            else:
                record.update(
                    classify_interior_target(
                        body,
                        body_start,
                        address,
                        noreturn_starts=noreturn_starts,
                        max_steps=max_steps,
                    )
                )
        counts[record["class"]] += 1
        if "entry_shape" in record:
            shape_counts[record["entry_shape"]] = shape_counts.get(record["entry_shape"], 0) + 1
        records.append(record)

    interior = len(addresses) - counts[CLASS_EXTERIOR]
    return {
        "tool": "tools/interior_census.py",
        "task": "T123",
        "xbe_sha256": image.sha256,
        "counts": {
            "unresolved": len(addresses),
            "interior": interior,
            "exterior": counts[CLASS_EXTERIOR],
            "safe_looking": counts[CLASS_SAFE],
            "unsafe": counts[CLASS_UNSAFE],
            "undecodable": counts[CLASS_UNDECODABLE],
        },
        "entry_shape_counts": dict(sorted(shape_counts.items())),
        "targets": records,
    }


_CSV_FIELDS = [
    "address",
    "class",
    "owner_start",
    "owner_end",
    "owner_name",
    "entry_shape",
    "is_loop_head",
    "is_branch_target_in_body",
    "boundary_valid",
    "flags_read_before_write",
    "ebp_dependent",
    "esp_slot_read",
    "caller_slot_writes",
    "inherited_gpr_reads",
    "end_reason",
    "steps",
    "calls_crossed",
    "address_taken_count",
    "incoming_xrefs",
    "first_unsafe",
    "undecodable_reason",
]


def write_csv(report: dict, path: Path) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=_CSV_FIELDS)
        writer.writeheader()
        for record in report["targets"]:
            contract = record.get("contract", {})
            owner = record.get("owner") or {}
            writer.writerow(
                {
                    "address": record["address"],
                    "class": record["class"],
                    "owner_start": owner.get("start", ""),
                    "owner_end": owner.get("end", ""),
                    "owner_name": owner.get("name", ""),
                    "entry_shape": record.get("entry_shape", ""),
                    "is_loop_head": record.get("is_loop_head", ""),
                    "is_branch_target_in_body": record.get("is_branch_target_in_body", ""),
                    "boundary_valid": record.get("boundary_valid", ""),
                    "flags_read_before_write": contract.get("flags_read_before_write", ""),
                    "ebp_dependent": contract.get("ebp_dependent", ""),
                    "esp_slot_read": contract.get("esp_slot_read", ""),
                    "caller_slot_writes": contract.get("caller_slot_writes", ""),
                    "inherited_gpr_reads": " ".join(contract.get("inherited_gpr_reads", [])),
                    "end_reason": contract.get("end_reason", ""),
                    "steps": contract.get("steps", ""),
                    "calls_crossed": contract.get("calls_crossed", ""),
                    "address_taken_count": record.get("address_taken_count", ""),
                    "incoming_xrefs": " ".join(
                        f"{k}:{v}" for k, v in sorted(record.get("incoming_xrefs", {}).items())
                    ),
                    "first_unsafe": contract.get("first_unsafe") or "",
                    "undecodable_reason": record.get("undecodable_reason", ""),
                }
            )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Census and classification of unresolved interior continuation targets (T123)."
    )
    parser.add_argument(
        "--xbe",
        type=Path,
        default=Path("build/default.xbe"),
        help="retail XBE the lifted tree was produced from",
    )
    parser.add_argument(
        "--lifted",
        type=Path,
        default=Path("generated/lifted"),
        help="lifted output tree containing gen/ and disasm/",
    )
    parser.add_argument(
        "--out-json", type=Path, default=None, help="write the full census JSON here"
    )
    parser.add_argument("--out-csv", type=Path, default=None, help="write the per-address CSV here")
    parser.add_argument(
        "--max-steps", type=int, default=200, help="forward-scan instruction cap per target"
    )
    args = parser.parse_args(argv)

    for required in (
        args.xbe,
        args.lifted / "gen" / "recomp_stubs_unresolved.c",
        args.lifted / "disasm" / "functions.json",
        args.lifted / "xbe_analysis.json",
    ):
        if not required.is_file():
            parser.error(f"missing input: {required}")

    report = run_census(args.xbe, args.lifted, args.max_steps)
    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    if args.out_csv:
        args.out_csv.parent.mkdir(parents=True, exist_ok=True)
        write_csv(report, args.out_csv)

    counts = report["counts"]
    print(
        f"unresolved {counts['unresolved']}  interior {counts['interior']}"
        f"  exterior {counts['exterior']}"
    )
    print(
        f"SAFE_LOOKING {counts['safe_looking']}  UNSAFE {counts['unsafe']}"
        f"  UNDECODABLE {counts['undecodable']}"
    )
    for shape, total in report["entry_shape_counts"].items():
        print(f"  shape {shape}: {total}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
