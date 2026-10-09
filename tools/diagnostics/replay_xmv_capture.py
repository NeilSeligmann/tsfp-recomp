"""Replay a private T538 codec entry; no captured retail data belongs in Git.

This is the TS538A01 format, not a general savestate. DF-clear and physical MMX
seeding are explicit seams. An uncaptured access or altered instruction refuses
the run. The comparison covers the selected output triplet for one wrapper call.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import time
import zlib
from pathlib import Path

import unicorn
from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_INVALID, UC_MODE_32, Uc, UcError
from unicorn import x86_const as x86

from tools.xbe import parse_xbe

XBE_SHA256 = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
MAX_CAPTURE_BYTES = 64 * 1024 * 1024


def checked_bytes(path: Path, expected: str, limit: int) -> bytes:
    if path.stat().st_size > limit:
        raise ValueError(f"{path.name}: input exceeds byte bound")
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"{path.name}: SHA256 mismatch")
    return data


REGISTER_NAMES = ("EAX", "ECX", "EDX", "EBX", "ESI", "EDI", "EBP", "ESP")


def load_capture(
    capture: bytes, image: object, flags: int, mmx_poison: int
) -> tuple[Uc, tuple[int, ...], int, list[tuple[int, int, int]]]:
    """Map a TS538A01 capture into a fresh Unicorn instance and seed its registers."""
    if capture[:8] != b"TS538A01" or len(capture) < 328:
        raise ValueError("unsupported or truncated capture header")
    registers = struct.unpack_from("<10I", capture, 8)
    if registers[0] != 0x447E5E:
        raise ValueError("capture is not the measured codec wrapper entry")
    mmx = struct.unpack_from("<8Q", capture, 48)
    fp_metadata = struct.unpack_from("<4I", capture, 304)
    mxcsr = struct.unpack_from("<I", capture, 320)[0]
    count = struct.unpack_from("<I", capture, 324)[0]
    if not 0 < count <= 1024:
        raise ValueError("invalid mapping count")
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    offset = 328
    mappings = []
    for _ in range(count):
        if offset + 12 > len(capture):
            raise ValueError("truncated mapping header")
        base, size, permissions = struct.unpack_from("<3I", capture, offset)
        offset += 12
        if not size or (base | size) & 0xFFF or base < 0x10000 or base + size > 2**32:
            raise ValueError("invalid guest mapping extent")
        if not permissions & 1 or permissions & ~7 or offset + size > len(capture):
            raise ValueError("capture must provide every byte of a readable mapping")
        if any(base < old + length and old < base + size for old, length, _ in mappings):
            raise ValueError("overlapping captured mappings")
        # Native guest image is RW because lifted C never executes its bytes.
        # Original execution adds X only to image-overlapping captured mappings.
        executable = base < image.base_address + image.size_of_image
        uc.mem_map(base, size, permissions | (4 if executable else 0))
        uc.mem_write(base, capture[offset : offset + size])
        offset += size
        mappings.append((base, size, permissions))
    if offset != len(capture):
        raise ValueError("trailing or inconsistent capture bytes")
    for name, value in zip(REGISTER_NAMES, registers[1:9], strict=True):
        uc.reg_write(getattr(x86, f"UC_X86_REG_{name}"), value)
    uc.reg_write(x86.UC_X86_REG_EFLAGS, flags)
    uc.reg_write(x86.UC_X86_REG_MXCSR, mxcsr)
    uc.reg_write(x86.UC_X86_REG_FPCW, fp_metadata[1])
    for index, value in enumerate(mmx):
        uc.reg_write(getattr(x86, f"UC_X86_REG_FP{index}"), (value ^ mmx_poison, 0xFFFF))
    for index in range(8):
        value = int.from_bytes(capture[112 + index * 16 : 128 + index * 16], "little")
        uc.reg_write(getattr(x86, f"UC_X86_REG_XMM{index}"), value)
    return uc, registers, count, mappings


def replay(args: argparse.Namespace) -> dict:
    capture = checked_bytes(args.capture, args.capture_sha256, MAX_CAPTURE_BYTES)
    # T611: a seeded replay compares the whole captured memory instead of one native frame.
    native = (
        checked_bytes(args.native_frame, args.native_sha256, 4 * 1024 * 1024)
        if getattr(args, "native_frame", None) is not None
        else None
    )
    retail = checked_bytes(args.xbe, XBE_SHA256, 8 * 1024 * 1024)
    image = parse_xbe(retail)
    uc, registers, count, mappings = load_capture(capture, image, args.flags, args.mmx_poison)
    # T611: edits applied to the loaded state before the first instruction (seeded replay).
    for edit in getattr(args, "edits", ()):
        edit(uc, registers, mappings)
    names = REGISTER_NAMES
    return_address, decoder = struct.unpack("<2I", uc.mem_read(registers[8], 8))
    result = {
        "capture_sha256": args.capture_sha256,
        "native_sha256": getattr(args, "native_sha256", None),
        "xbe_sha256": XBE_SHA256,
        "unicorn_version": unicorn.__version__,
        "entry": registers[0],
        "return_address": return_address,
        "decoder": decoder,
        "entry_registers": dict(zip(names, registers[1:9], strict=True)),
        "flags_seed": args.flags,
        "direction_flag_clear": not bool(args.flags & 0x400),
        "mmx_poison": args.mmx_poison,
        "virtual_x87_restored": False,
        "map_count": count,
        "instruction_limit": getattr(args, "instruction_limit", 20_000_000),
        "timeout_seconds": getattr(args, "timeout_seconds", 30),
        "instructions": 0,
        "returned": False,
        "missing_access": None,
        "instruction_mismatch": None,
        "visited_entries": {},
    }
    visited: dict[int, tuple[int, bytes]] = {}
    last_pcs: list[int] = []
    # T538: control-flow edges (previous executed address, next one), kept only on request.
    collect_edges = getattr(args, "collect_edges", False)
    edges: set[tuple[int, int]] = set()
    previous = [0]

    def invalid(u: Uc, access: int, address: int, size: int, value: int, _: object) -> bool:
        result["missing_access"] = {
            "access": access,
            "address": address,
            "size": size,
            "value": value,
            "eip": u.reg_read(x86.UC_X86_REG_EIP),
        }
        return False

    def instruction(u: Uc, address: int, size: int, _: object) -> None:
        result["instructions"] += 1
        last_pcs.append(address)
        del last_pcs[:-32]
        if collect_edges:
            if previous[0]:
                edges.add((previous[0], address))
            previous[0] = address
        if address == return_address:
            result["returned"] = True
            u.emu_stop()
            return
        if address not in visited:
            raw_offset = image.va_to_offset(address)
            live = bytes(u.mem_read(address, size))
            if raw_offset is None or retail[raw_offset : raw_offset + size] != live:
                result["instruction_mismatch"] = {"address": address, "size": size}
                u.emu_stop()
                return
            visited[address] = (size, live)
        if address in (0x447E5E, 0x4468E1, 0x4473BB, 0x44704F, 0x44CB37):
            entries = result["visited_entries"]
            entries[hex(address)] = entries.get(hex(address), 0) + 1

    uc.hook_add(UC_HOOK_MEM_INVALID, invalid)
    uc.hook_add(UC_HOOK_CODE, instruction)
    started = time.monotonic()
    try:
        uc.emu_start(
            registers[0],
            0xFFFFFFFF,
            timeout=result["timeout_seconds"] * 1_000_000,
            count=result["instruction_limit"],
        )
    except UcError as error:
        result["unicorn_error"] = str(error)
    result["elapsed_seconds"] = time.monotonic() - started
    result["last_pcs"] = last_pcs
    result["final_registers"] = {
        name: uc.reg_read(getattr(x86, f"UC_X86_REG_{name}")) for name in (*names, "EIP")
    }
    decoder_before = capture_decoder(capture, mappings, decoder)
    result["decoder_before_sha256"] = hashlib.sha256(decoder_before).hexdigest()
    result["decoder_after_sha256"] = hashlib.sha256(uc.mem_read(decoder, 0x168)).hexdigest()
    disassembler = Cs(CS_ARCH_X86, CS_MODE_32)
    result["unique_executed_pcs"] = len(visited)
    result["executed_x87_instructions"] = [
        {"address": address, "mnemonic": insn.mnemonic, "operands": insn.op_str}
        for address, (_, data) in sorted(visited.items())
        for insn in disassembler.disasm(data, address)
        if insn.mnemonic.startswith("f")
    ]
    after_run = getattr(args, "after_run", None)
    if after_run is not None:
        after_run(uc, mappings, result)
    if result["returned"] and native is not None:
        cols, rows = struct.unpack("<2I", uc.mem_read(decoder + 0xDC, 8))
        if not 0 < cols <= 256 or not 0 < rows <= 256:
            raise ValueError("invalid decoded dimensions")
        fields = (0xEC, 0xF0, 0xF4) if args.output_triplet == 0 else (0xFC, 0x100, 0x104)
        pointers = [struct.unpack("<I", uc.mem_read(decoder + field, 4))[0] for field in fields]
        output = b"".join(
            bytes(uc.mem_read(pointer, cols * rows * (256 if index == 0 else 64)))
            for index, pointer in enumerate(pointers)
        )
        different = [
            index for index, (a, b) in enumerate(zip(output, native, strict=False)) if a != b
        ]
        result["output"] = {
            "triplet": args.output_triplet,
            "pointers": pointers,
            "bytes": len(output),
            "sha256": hashlib.sha256(output).hexdigest(),
            "crc32": f"{zlib.crc32(output):08X}",
            "native_crc32": f"{zlib.crc32(native):08X}",
            "length_equal": len(output) == len(native),
            "different_bytes": len(different),
            "first_differences": different[:20],
        }
    if collect_edges:
        result["visited"] = {address: size for address, (size, _) in visited.items()}
        result["edges"] = sorted(edges)
        result["code"] = {address: data.hex() for address, (_, data) in visited.items()}
    result["matched"] = bool(
        result["returned"]
        and (
            native is None
            or (
                result.get("output", {}).get("length_equal")
                and result["output"]["different_bytes"] == 0
            )
        )
        and result["missing_access"] is None
        and result["instruction_mismatch"] is None
    )
    return result


def capture_decoder(capture: bytes, mappings: list[tuple[int, int, int]], decoder: int) -> bytes:
    offset = 328
    for base, size, _ in mappings:
        offset += 12
        if base <= decoder and decoder + 0x168 <= base + size:
            return capture[offset + decoder - base : offset + decoder - base + 0x168]
        offset += size
    raise ValueError("decoder extent absent from capture")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--capture-sha256", required=True)
    parser.add_argument("--native-frame", type=Path, required=True)
    parser.add_argument("--native-sha256", required=True)
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--flags", type=lambda value: int(value, 0), default=0x202)
    parser.add_argument("--mmx-poison", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--output-triplet", type=int, choices=(0, 1), default=1)
    args = parser.parse_args()
    if not 0 <= args.flags < 2**32 or not 0 <= args.mmx_poison < 2**64:
        parser.error("register seed outside representation")
    try:
        result = replay(args)
    except (OSError, ValueError, UcError) as error:
        print(json.dumps({"matched": False, "refusal": str(error)}, indent=2))
        return 2
    print(json.dumps(result, indent=2))
    return 0 if result["matched"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
