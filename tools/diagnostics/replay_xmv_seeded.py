"""T611: run the XMV codec wrapper on seeded state, native lift against the original instructions.

T538 proved the lifted wrapper `0x447E5E` on the 609 frames the disc plays. 32 of its conditional
jumps saw one outcome there. This tool edits the captured entry state (decoder flag bytes
`+0xA0..+0xA5`, the macroblock grid size, the bit reader window and the packet words) and runs BOTH
sides on the SAME edited state:

* native: `tsfp_host --seed-xmv-entry N --seed-xmv-patch ... --seed-xmv-result FILE` applies the
  edits at the Nth wrapper entry, calls the lifted `sub_00447E5E` itself and writes every 4 KiB page
  of the captured mappings that the call changed (the same boot also writes the unedited capture of
  that entry);
* original: the captured state with the same edits (grammar of `src/host/xmv_seed_patch.h`) runs the
  certified retail XBE bytes on bare Unicorn (`replay_xmv_capture.replay`), every mapped byte
  afterwards is read.

A trial MATCHES when both returned, the edited entry states have the same CRC-32,
EBX/ESI/EDI/EBP/ESP agree and no captured byte outside the dead stack below the entry ESP
differs. Branch coverage is credited only from matching trials, against the one-sided list of
`docs/data/t538-xmv-driver-proof.json`. Captures and result files are disc data: they live in a
temporary directory that is deleted, nothing from them is stored.

Exit status: 0 no trial failed, 1 a trial failed (state mismatch or one-sided fault), 2 an input is
missing or unusable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace

from capstone import CS_ARCH_X86, CS_GRP_JUMP, CS_MODE_32, CS_OP_IMM, Cs

from tools.diagnostics.replay_xmv_capture import replay

HOST_FLAGS = [
    "--ac97-ready",
    "--headless-streams",
    "--headless-buffers",
    "--headless-listener",
    "--headless-second-vblank",
    "--native-shader-assembler",
    "--native-xmv",
    "--headless-movie-audio",
    "--couple-vblank-effects",
    "--check-vblank-quiescence",
    "--trace-xmv",
    "--thread-timeout",
    "120000",
    "--vblank-owner-waits",
    "429",
    "--overlay-consume",
]
PATCH_BYTES = 128
NUMBER_LIMIT = 0x10000000
REGISTERS = ("EAX", "ECX", "EDX", "EBX", "ESI", "EDI", "EBP", "ESP")
CHECKED_REGISTERS = ("EBX", "ESI", "EDI", "EBP", "ESP")
PAGE = 4096
SIGFPE = 8
# Branch sides no input can take, each argued from the disassembly
# (docs/t611-xmv-driver-branches.md).
# They are NOT exercised, so they stay out of the equality claim, and a trial reaching one would be
# a finding.
INFEASIBLE_SIDES = {
    0x44775C: (
        "taken",
        "eax is 0, 1 or 2 here (0x448F08 returns 0, 1 or 2, then either modulo 3 or untouched), "
        "so eax-2 != 0 after the two decrements is impossible",
    ),
    0x447DB5: (
        "fallthrough",
        "every path to 0x447DB1 has [ebp+0x2c] == 0 and reached 0x447D2F through "
        "test [ebp+0xc],0x40 != 0, so the test cannot be zero",
    ),
    0x44A68B: (
        "taken",
        "[ebp+0x20] is the bit 0x4473BB reads at 0x447B45 only when the macroblock vector x or y "
        "is "
        "odd, and 0x44A662 gets those two low bits as [ebp+0x18] and [ebp+0x1c], so the bit cannot "
        "be set with both zero",
    ),
}
FAILURES = ("state_mismatch", "native_fault_only", "original_fault_only")
# Decoder object fields (docs/xmv-contracts.md section 12.1) and the stream window the edits aim at.
FLAG_BASE = 0xA0
COLUMNS, ROWS = 0xDC, 0xE0
CACHE, BITS_LEFT, NEXT_WORD = 0x0, 0x4, 0x8
# Flag bytes every disc movie shows in this state (T538 branch census), so the edit flips them.
FLAG_DISC_VALUE = (1, 0, 1, 0, 1, 1)


@dataclass(frozen=True)
class Patch:
    """One edit, the grammar of `xmv_seed_patch.h`: dec+OFF=HEX, esp+OFF=HEX, dec@PTR+OFF=HEX."""

    base: str
    pointer_offset: int
    offset: int
    data: bytes

    def spec(self) -> str:
        head = f"dec@{self.pointer_offset:#x}+" if self.base == "ptr" else f"{self.base}+"
        return f"{head}{self.offset:#x}={self.data.hex()}"


def parse_number(text: str) -> tuple[int, str]:
    if not text or not text[0].isdigit():
        raise ValueError(f"bad number in {text!r}")
    hexadecimal = text[:2].lower() == "0x" and len(text) > 2
    digits = "0123456789abcdefABCDEF" if hexadecimal else "0123456789"
    start = 2 if hexadecimal else 0
    end = start
    while end < len(text) and text[end] in digits:
        end += 1
    if end == start:
        raise ValueError(f"bad number in {text!r}")
    value = int(text[start:end], 16 if hexadecimal else 10)
    if value >= NUMBER_LIMIT:
        raise ValueError(f"number out of range in {text!r}")
    return value, text[end:]


def parse_patch(text: str) -> Patch:
    if text.startswith("dec@"):
        pointer, rest = parse_number(text[4:])
        if not rest.startswith("+"):
            raise ValueError(f"bad patch {text!r}")
        offset, rest = parse_number(rest[1:])
        base = "ptr"
    elif text.startswith(("dec+", "esp+")):
        base, pointer = text[:3], 0
        offset, rest = parse_number(text[4:])
    else:
        raise ValueError(f"bad patch {text!r}")
    if not rest.startswith("="):
        raise ValueError(f"bad patch {text!r}")
    digits = rest[1:]
    if (
        not re.fullmatch(r"[0-9A-Fa-f]*", digits)
        or not digits
        or len(digits) % 2
        or len(digits) // 2 > PATCH_BYTES
    ):
        raise ValueError(f"bad patch bytes in {text!r}")
    return Patch(base, pointer, offset, bytes.fromhex(digits))


def word(value: int) -> bytes:
    return struct.pack("<I", value & 0xFFFFFFFF)


def dec_patch(offset: int, data: bytes) -> str:
    return Patch("dec", 0, offset, data).spec()


def stream_window(frame_type: int, quantizer: int, skip_mode: int, filler: int) -> list[str]:
    """Bit reader state whose next bits are: frame type (1), quantizer (5), skip mode (2), `filler`.

    The reader hands out bit `bits_left - 1` of the cache next; 31 is a word minus its top bit."""
    cache = (frame_type << 30) | (quantizer << 25) | (skip_mode << 23) | (filler & 0x7FFFFF)
    return [dec_patch(CACHE, word(cache)), dec_patch(BITS_LEFT, word(31))]


def named_trials() -> list[dict]:
    """Deterministic trials aimed at the T611 branch groups. Entry 0 is an I frame, 2 a P frame."""
    trials: list[dict] = []

    def add(name: str, entry: int, patches: list[str], aim: str) -> None:
        trials.append({"name": name, "entry": entry, "patches": patches, "aim": aim})

    add("control_i", 0, [], "no edit, the T538 case on the new harness")
    add("control_p", 2, [], "no edit, the T538 case on the new harness")
    for entry, kind in ((0, "i"), (2, "p")):
        for index in range(6):
            flipped = 1 - FLAG_DISC_VALUE[index]
            add(
                f"flag_a{index}_{kind}",
                entry,
                [dec_patch(FLAG_BASE + index, bytes([flipped]))],
                f"decoder flag +0xA{index} flipped",
            )
        add(
            f"flags_all_{kind}",
            entry,
            [dec_patch(FLAG_BASE, bytes(1 - value for value in FLAG_DISC_VALUE))],
            "all six decoder flags flipped",
        )
        add(f"cols0_{kind}", entry, [dec_patch(COLUMNS, word(0))], "zero macroblock columns")
        add(f"rows0_{kind}", entry, [dec_patch(ROWS, word(0))], "zero macroblock rows")
    for mode in (1, 2, 3):
        for dims, patches in (
            ("rows0", [dec_patch(ROWS, word(0))]),
            ("cols0", [dec_patch(COLUMNS, word(0))]),
        ):
            add(
                f"skip{mode}_{dims}",
                2,
                [*stream_window(1, 8, mode, 0x1234), *patches],
                f"P frame, skip mode {mode}, {dims}",
            )
    for columns, rows in ((4, 8), (8, 4), (1, 32), (2, 16), (32, 1)):
        add(
            f"skip1_{columns}x{rows}",
            2,
            [
                *stream_window(1, 8, 1, 0),
                dec_patch(COLUMNS, word(columns)),
                dec_patch(ROWS, word(rows)),
            ],
            "P frame, skip bitmap of exactly 32 bits per word",
        )
    for quantizer in (4, 5, 12, 20, 21, 31):
        add(
            f"p_quant{quantizer}",
            2,
            stream_window(1, quantizer, 0, 0x2AAAAA),
            f"P quantizer {quantizer}",
        )
        add(
            f"i_quant{quantizer}",
            0,
            [dec_patch(CACHE, word(quantizer << 18)), dec_patch(BITS_LEFT, word(31))],
            f"I quantizer {quantizer}",
        )
    for left in range(0, 8):
        add(
            f"i_bitsleft{left}",
            0,
            [dec_patch(BITS_LEFT, word(left))],
            f"I frame, {left} bits left in the cache",
        )
    return trials


def packet_noise(generator: random.Random, blocks: int) -> list[str]:
    """`blocks` times 128 random bytes over the start of the packet the decoder is reading."""
    return [
        Patch("ptr", NEXT_WORD, 128 * block, generator.randbytes(128)).spec()
        for block in range(blocks)
    ]


def pframe_trials(count: int, seed: int) -> list[dict]:
    """Seeded P frames: a valid header window (skip mode 0), up to 1 KiB of random macroblocks."""
    generator = random.Random(seed)  # noqa: S311 - reproducible test input, not security
    trials = []
    for index in range(count):
        window = stream_window(1, generator.randrange(0, 21), 0, generator.getrandbits(23))
        patches = [*window, *packet_noise(generator, generator.randrange(1, 9))]
        trials.append(
            {
                "name": f"pframe_{seed}_{index:03d}",
                "entry": generator.choice((2, 3, 5, 8, 20, 50, 100)),
                "patches": patches,
                "aim": "P frame, valid header, random macroblock data",
            }
        )
    return trials


def random_trials(count: int, seed: int) -> list[dict]:
    """Seeded random edits: the stream window, packet words, flags and a reduced grid."""
    generator = random.Random(seed)  # noqa: S311 - reproducible test input, not security
    trials = []
    for index in range(count):
        entry = generator.choice((0, 1, 2, 3, 6, 11, 40, 100))
        patches: list[str] = []
        roll = generator.random()
        if roll < 0.7:
            cache = generator.getrandbits(32)
            patches.append(dec_patch(CACHE, word(cache)))
            patches.append(
                dec_patch(
                    BITS_LEFT, word(generator.choice((31, 31, 31, generator.randrange(0, 32))))
                )
            )
            patches.append(
                Patch(
                    "ptr", NEXT_WORD, 0, generator.randbytes(generator.choice((16, 64, 128)))
                ).spec()
            )
        if generator.random() < 0.4:
            patches.append(dec_patch(FLAG_BASE, bytes(generator.getrandbits(1) for _ in range(6))))
        if generator.random() < 0.25:
            patches.append(dec_patch(COLUMNS, word(generator.choice((0, 1, 2, 4, 8, 11)))))
        if generator.random() < 0.25:
            patches.append(dec_patch(ROWS, word(generator.choice((0, 1, 2, 4, 8, 9)))))
        trials.append(
            {
                "name": f"random_{seed}_{index:03d}",
                "entry": entry,
                "patches": patches,
                "aim": "random",
            }
        )
    return trials


def read_result(data: bytes) -> dict:
    if data[:8] != b"TS611R01":
        raise ValueError("not a TS611R01 result")
    ordinal, crc = struct.unpack_from("<2I", data, 8)
    registers = struct.unpack_from("<8I", data, 16)
    (count,) = struct.unpack_from("<I", data, 48)
    offset = 52
    mappings = []
    for _ in range(count):
        base, size, changed = struct.unpack_from("<3I", data, offset)
        offset += 12
        pages = {}
        for _ in range(changed):
            (page,) = struct.unpack_from("<I", data, offset)
            pages[page] = data[offset + 4 : offset + 4 + PAGE]
            offset += 4 + PAGE
        mappings.append((base, size, pages))
    if offset != len(data):
        raise ValueError("trailing or truncated result bytes")
    return {
        "ordinal": ordinal,
        "crc": crc,
        "registers": dict(zip(REGISTERS, registers, strict=True)),
        "mappings": mappings,
    }


def apply_patch(uc: object, registers: tuple[int, ...], patch: Patch) -> None:
    esp = registers[8]
    decoder = struct.unpack("<I", uc.mem_read(esp + 4, 4))[0]
    base = esp if patch.base == "esp" else decoder
    if patch.base == "ptr":
        base = struct.unpack("<I", uc.mem_read(decoder + patch.pointer_offset, 4))[0]
    uc.mem_write(base + patch.offset, patch.data)


def branch_sides(
    code: dict[int, bytes], edges: set[tuple[int, int]], addresses: set[int]
) -> dict[int, set[str]]:
    """Which outcomes each listed conditional jump showed in these edges."""
    successors: dict[int, set[int]] = {}
    for source, target in edges:
        if source in addresses:
            successors.setdefault(source, set()).add(target)
    disassembler = Cs(CS_ARCH_X86, CS_MODE_32)
    disassembler.detail = True
    sides: dict[int, set[str]] = {}
    for address in addresses & code.keys():
        for insn in disassembler.disasm(code[address], address):
            if (
                insn.address != address
                or CS_GRP_JUMP not in insn.groups
                or insn.operands[0].type != CS_OP_IMM
            ):
                continue
            seen = successors.get(address, set())
            found = sides.setdefault(address, set())
            if insn.operands[0].imm in seen:
                found.add("taken")
            if address + insn.size in seen:
                found.add("fallthrough")
    return sides


def run_native(job: dict, scratch: Path) -> tuple[Path, Path, subprocess.CompletedProcess[str]]:
    captures, result = scratch / "captures", scratch / "result.bin"
    captures.mkdir()
    hdd = scratch / "hdd"
    hdd.mkdir()
    command = [
        job["host"],
        job["xbe"],
        "--hdd",
        str(hdd),
        *HOST_FLAGS,
        "--disc",
        job["disc"],
        "--capture-xmv-entries",
        str(captures),
        "--capture-xmv-entry-at",
        str(job["entry"]),
        "--seed-xmv-entry",
        str(job["entry"]),
        "--seed-xmv-result",
        str(result),
    ]
    for patch in job["patches"]:
        command += ["--seed-xmv-patch", patch]
    completed = subprocess.run(  # noqa: S603
        command,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=job["host_timeout"],
        check=False,
    )
    return captures / f"entry_{job['entry']:05d}.bin", result, completed


def compare(
    uc: object, mappings: list[tuple[int, int, int]], native: dict, entry_esp: int, pre: list[bytes]
) -> dict:
    """Bytes of every captured mapping after the original run against the native post state."""
    different, live, first = 0, 0, []
    for (base, size, _), before, (nbase, nsize, pages) in zip(
        mappings, pre, native["mappings"], strict=True
    ):
        if (base, size) != (nbase, nsize):
            raise ValueError("native and captured mappings differ")
        expected = bytearray(before)
        for page, content in pages.items():
            expected[page * PAGE : (page + 1) * PAGE] = content
        actual = bytes(uc.mem_read(base, size))
        if actual == expected:
            continue
        for page in range(size // PAGE):
            low, high = page * PAGE, (page + 1) * PAGE
            if actual[low:high] == expected[low:high]:
                continue
            for index in range(low, high):
                if actual[index] != expected[index]:
                    different += 1
                    # The stack below the entry ESP is the callee's dead frame, never read again.
                    if not (base <= entry_esp < base + size and base + index < entry_esp):
                        live += 1
                        if len(first) < 8:
                            first.append(
                                {
                                    "address": hex(base + index),
                                    "original": actual[index],
                                    "native": expected[index],
                                }
                            )
    return {
        "different_bytes": different,
        "different_live_bytes": live,
        "first_live_differences": first,
    }


def parse_native_fault(report: str) -> dict | None:
    """The host stop report of a guest thread killed by a host signal: signal, address, ESP, EAX."""
    start = report.find("stopped: host fault")
    if start < 0:
        return None
    block = report[start : start + 1500]
    fault: dict = {}
    for key, pattern in (
        ("signal", r"signal\s+(\d+)"),
        ("address", r"fault address\s+0x([0-9A-Fa-f]+)"),
        ("esp", r"guest esp\s+0x([0-9A-Fa-f]+)"),
        ("eax", r"guest eax\s+0x([0-9A-Fa-f]+)"),
    ):
        match = re.search(pattern, block)
        if match:
            fault[key] = int(match.group(1), 10 if key == "signal" else 16)
    return fault if "signal" in fault else None


def run_trial(job: dict) -> dict:
    """One trial: native boot, original replay of the same edited state, compare, branch outcomes.

    `outcome` is one of: match, state_mismatch, same_fault (both sides died the same way, which is
    agreement but not equality evidence), native_fault_only, original_fault_only, original_limit
    (the original hit the instruction bound, inconclusive) and no_capture (the boot never reached
    the entry)."""
    row = {"name": job["name"], "entry": job["entry"], "aim": job["aim"], "patches": job["patches"]}
    scratch = Path(tempfile.mkdtemp(prefix="t611-"))
    try:
        capture_path, result_path, completed = run_native(job, scratch)
        row["native_exit"] = completed.returncode
        if not capture_path.is_file():
            row.update({"matched": False, "outcome": "no_capture"})
            return row
        native = read_result(result_path.read_bytes()) if result_path.is_file() else None
        native_fault = parse_native_fault(completed.stdout + completed.stderr)
        capture_bytes = capture_path.read_bytes()
        patches = [parse_patch(text) for text in job["patches"]]
        state: dict = {}

        def edit(
            uc: object, registers: tuple[int, ...], mappings: list[tuple[int, int, int]]
        ) -> None:
            for patch in patches:
                apply_patch(uc, registers, patch)
            state["pre"] = [bytes(uc.mem_read(base, size)) for base, size, _ in mappings]
            state["esp"] = registers[8]
            crc = 0
            for chunk in state["pre"]:
                crc = zlib.crc32(chunk, crc)
            state["crc"] = crc

        def after_run(uc: object, mappings: list[tuple[int, int, int]], result: dict) -> None:
            if result["returned"] and native is not None:
                state["comparison"] = compare(uc, mappings, native, state["esp"], state["pre"])
                state["registers"] = dict(result["final_registers"])

        arguments = SimpleNamespace(
            capture=capture_path,
            capture_sha256=hashlib.sha256(capture_bytes).hexdigest(),
            native_frame=None,
            xbe=Path(job["xbe"]),
            flags=0x202,
            mmx_poison=0,
            output_triplet=1,
            collect_edges=True,
            edits=[edit],
            after_run=after_run,
            instruction_limit=job["instruction_limit"],
            timeout_seconds=job["replay_timeout"],
        )
        original = replay(arguments)
        comparison = state.get("comparison")
        registers_equal = comparison is not None and all(
            state["registers"][name] == native["registers"][name] for name in CHECKED_REGISTERS
        )
        faulted = (
            original["missing_access"] is not None or original.get("unicorn_error") is not None
        )
        row.update(
            {
                "original_returned": original["returned"],
                "original_instructions": original["instructions"],
                "original_missing_access": original["missing_access"],
                "original_unicorn_error": original.get("unicorn_error"),
                "native_fault": None
                if native_fault is None
                else {
                    name: value if name == "signal" else hex(value)
                    for name, value in native_fault.items()
                },
                "pre_state_crc_equal": None
                if native is None
                else state.get("crc") == native["crc"],
                "registers_equal": registers_equal,
                "comparison": comparison,
            }
        )
        if native is not None:
            row["native_registers"] = {
                name: f"{native['registers'][name]:#x}" for name in REGISTERS
            }
        if original["instruction_mismatch"] is not None:
            row["outcome"] = "state_mismatch"
        elif original["returned"] and native is not None:
            equal = (
                row["pre_state_crc_equal"]
                and registers_equal
                and comparison["different_live_bytes"] == 0
            )
            row["outcome"] = "match" if equal else "state_mismatch"
        elif original["returned"]:
            row["outcome"] = "native_fault_only"
        elif native is not None and faulted:
            row["outcome"] = "original_fault_only"
        elif native is not None:
            row["outcome"] = "original_limit"
        elif faulted and native_fault is not None:
            access = original["missing_access"]
            divide = original.get("unicorn_error") is not None and native_fault["signal"] == SIGFPE
            same_address = access is not None and access["address"] == native_fault.get("address")
            row["outcome"] = "same_fault" if divide or same_address else "native_fault_only"
            row["fault_kind"] = "divide error" if divide else "invalid access"
            row["fault_guest_esp_equal"] = (
                native_fault.get("esp") == original["final_registers"]["ESP"]
            )
            if row["outcome"] != "same_fault":
                row["note"] = "both sides faulted, not the same way"
        elif faulted:
            row["outcome"] = "native_fault_only"
            row["note"] = "the original faulted, the native run left no result and no signal report"
        else:
            row["outcome"] = "original_limit"
        row["matched"] = row["outcome"] == "match"
        if row["matched"]:
            row["_code"] = original["code"]
            row["_edges"] = [list(edge) for edge in original["edges"]]
        return row
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def compact_row(row: dict) -> dict:
    """The numbers of a matching trial; every other outcome keeps its full evidence."""
    if not row["matched"]:
        return row
    keep = ("name", "entry", "outcome", "original_instructions")
    small = {key: row[key] for key in keep}
    if not row["name"].startswith("random_") and not row["name"].startswith("pframe_"):
        small["patches"] = row["patches"]  # the named trials are short and are the specification
    return small


def environment(args: argparse.Namespace) -> dict:
    """What the numbers were measured with."""
    import unicorn  # noqa: PLC0415

    return {
        "xbe_sha256": hashlib.sha256(args.xbe.read_bytes()).hexdigest(),
        "unicorn_version": unicorn.__version__,
        "random_seed": args.random_seed,
        "random_trials": args.random,
        "pframe_trials": args.pframe,
        "instruction_limit": args.instruction_limit,
    }


def load_baseline(path: Path) -> dict[int, set[str]]:
    """The T538 one-sided driver branches minus the dead compares: address -> sides already seen."""
    data = json.loads(path.read_text())
    dead = {int(item, 16) for item in data["branches"]["statically_dead_constant_compare"]}
    seen = {}
    for item in data["branches"]["one_sided"]:
        address = int(item["address"], 16)
        if address not in dead:
            seen[address] = {name for name in ("taken", "fallthrough") if item[name]}
    return seen


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--host", type=Path, required=True, help="tsfp_host built with --seed-xmv-entry"
    )
    parser.add_argument("--xbe", type=Path, required=True, help="the certified retail default.xbe")
    parser.add_argument("--disc", type=Path, required=True, help="the Xbox disc image")
    parser.add_argument(
        "--baseline", type=Path, required=True, help="docs/data/t538-xmv-driver-proof.json"
    )
    parser.add_argument(
        "--random", type=int, default=0, help="number of seeded random trials (default 0)"
    )
    parser.add_argument("--random-seed", type=int, default=611, help="seed of the random trials")
    parser.add_argument("--pframe", type=int, default=0, help="number of seeded P frame trials")
    parser.add_argument("--no-named", action="store_true", help="skip the deterministic trials")
    parser.add_argument(
        "--only", action="append", default=[], help="run only trials whose name contains this"
    )
    parser.add_argument("--jobs", type=int, default=8, help="parallel trials (default 8)")
    parser.add_argument("--host-timeout", type=int, default=240, help="seconds per host boot")
    parser.add_argument(
        "--instruction-limit", type=int, default=400_000_000, help="original instructions per trial"
    )
    parser.add_argument(
        "--replay-timeout", type=int, default=180, help="seconds per original replay"
    )
    parser.add_argument(
        "--compact",
        action="store_true",
        help="drop the edits of matching random trials from the summary",
    )
    parser.add_argument("--out", type=Path, help="write the summary JSON here (default stdout)")
    args = parser.parse_args()
    for path in (args.host, args.xbe, args.disc, args.baseline):
        if not path.is_file():
            print(f"missing input {path}", file=sys.stderr)
            return 2
    trials = [
        *([] if args.no_named else named_trials()),
        *random_trials(args.random, args.random_seed),
        *pframe_trials(args.pframe, args.random_seed),
    ]
    if args.only:
        trials = [trial for trial in trials if any(part in trial["name"] for part in args.only)]
    if not trials:
        print("no trials selected", file=sys.stderr)
        return 2
    for trial in trials:
        for text in trial["patches"]:
            parse_patch(text)  # fail before any boot
        trial.update(
            host=str(args.host),
            xbe=str(args.xbe),
            disc=str(args.disc),
            host_timeout=args.host_timeout,
            instruction_limit=args.instruction_limit,
            replay_timeout=args.replay_timeout,
        )
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        rows = list(pool.map(run_trial, trials))
    baseline = load_baseline(args.baseline)
    covered: dict[int, set[str]] = {address: set(sides) for address, sides in baseline.items()}
    credited: dict[int, str] = {}
    code_union: set[int] = set()
    for row in rows:
        code = row.pop("_code", None)
        edges = row.pop("_edges", None)
        if code is None or not row["matched"]:
            continue
        decoded = {int(address): bytes.fromhex(data) for address, data in code.items()}
        code_union.update(decoded)
        for address, sides in branch_sides(
            decoded, {tuple(edge) for edge in edges}, set(baseline)
        ).items():
            if sides - covered[address]:
                credited.setdefault(address, row["name"])
            covered[address] |= sides
    both = {"taken", "fallthrough"}
    still = {
        hex(address): sorted(both - sides)
        for address, sides in sorted(covered.items())
        if sides != both
    }
    summary = {
        "trials": len(rows),
        "outcomes": {
            name: sum(1 for row in rows if row["outcome"] == name)
            for name in sorted({r["outcome"] for r in rows})
        },
        "matched": sum(1 for row in rows if row["matched"]),
        "failed": [row for row in rows if row["outcome"] in FAILURES],
        "inconclusive": [
            row["name"] for row in rows if row["outcome"] in ("original_limit", "no_capture")
        ],
        "baseline_one_sided_driver_branches": len(baseline),
        "now_both_sided": len(baseline) - len(still),
        "first_credited_by": {hex(address): name for address, name in sorted(credited.items())},
        "infeasible_sides": {
            hex(address): {"side": side, "reason": reason}
            for address, (side, reason) in sorted(INFEASIBLE_SIDES.items())
        },
        "unexplained_one_sided": {
            address: sides
            for address, sides in still.items()
            if INFEASIBLE_SIDES.get(int(address, 16), ("",))[0:1] != tuple(sides)
        },
        "still_one_sided": still,
        "per_trial": rows,
    }
    summary["environment"] = environment(args)
    if args.compact:
        summary["per_trial"] = [compact_row(row) for row in rows]
    text = json.dumps(summary, indent=1)
    if args.out:
        args.out.write_text(text + "\n")
    else:
        print(text)
    return 1 if summary["failed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
