"""T538: replay every captured XMV codec entry on the original instructions, total the coverage.

Runs `replay_xmv_capture.replay` (bare Unicorn over the certified XBE bytes, no substitutes) once
per `entry_NNNNN.bin` that `tsfp_host --capture-xmv-entries` wrote and compares each output triplet
with the native `frame_NNNNN.yuv` of the same number (`--dump-xmv-frames`). It reports numbers only:
per entry the match, instruction count and wrapper call counts, then the union of executed
instruction addresses, the conditional branch and indirect jump outcomes, and the string and
EFLAGS-writing instructions seen over all packets. Captures and frames are the user's disc data:
keep both directories outside the repository, nothing here stores pixels.

Exit status: 0 every entry matched, 1 at least one did not, 2 an input is missing or unusable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from types import SimpleNamespace

from capstone import CS_ARCH_X86, CS_GRP_JUMP, CS_MODE_32, CS_OP_IMM, Cs

from tools.diagnostics.replay_xmv_capture import replay

ENTRY_FILE = re.compile(r"entry_(\d{5})\.bin$")
FLAG_WRITERS = ("std", "cld", "popfd", "popf", "sahf")
STRING_OPERATIONS = (
    "movsb",
    "movsw",
    "movsd",
    "stosb",
    "stosw",
    "stosd",
    "cmpsb",
    "cmpsd",
    "lodsb",
    "lodsd",
    "scasb",
    "scasd",
)
CALLS = ("0x447e5e", "0x4468e1", "0x4473bb", "0x44704f", "0x44cb37")


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def replay_one(job: tuple[str, str, str, int, int, str]) -> dict:
    capture, frame, xbe, flags, poison, label = job
    arguments = SimpleNamespace(
        capture=Path(capture),
        capture_sha256=sha256_of(Path(capture)),
        native_frame=Path(frame),
        native_sha256=sha256_of(Path(frame)),
        xbe=Path(xbe),
        flags=flags,
        mmx_poison=poison,
        output_triplet=1,
        collect_edges=True,
    )
    try:
        result = replay(arguments)
    except (OSError, ValueError) as error:
        return {
            "set": label,
            "ordinal": int(ENTRY_FILE.search(capture).group(1)),
            "refusal": str(error),
        }
    result["set"] = label
    result["ordinal"] = int(ENTRY_FILE.search(capture).group(1))
    return result


def classify(code: dict[int, bytes], edges: set[tuple[int, int]]) -> dict:
    """Branch outcomes: each conditional jump's two sides and each indirect jump's targets."""
    disassembler = Cs(CS_ARCH_X86, CS_MODE_32)
    disassembler.detail = True
    successors: dict[int, set[int]] = {}
    for source, target in edges:
        successors.setdefault(source, set()).add(target)
    conditional, indirect, one_sided = 0, [], []
    flag_writers, string_ops = [], []
    for address, data in sorted(code.items()):
        for insn in disassembler.disasm(data, address):
            if insn.mnemonic in FLAG_WRITERS:
                flag_writers.append(f"{address:#x}:{insn.mnemonic}")
            if insn.mnemonic.split()[-1] in STRING_OPERATIONS:
                string_ops.append(f"{address:#x}:{insn.mnemonic}")
            if CS_GRP_JUMP not in insn.groups or insn.mnemonic in ("jmp", "ljmp"):
                if insn.mnemonic == "jmp" and insn.operands[0].type != CS_OP_IMM:
                    indirect.append((address, len(successors.get(address, ()))))
                continue
            if insn.operands[0].type != CS_OP_IMM:
                continue
            conditional += 1
            seen = successors.get(address, set())
            taken = insn.operands[0].imm in seen
            fallen = (address + insn.size) in seen
            if not (taken and fallen):
                one_sided.append(
                    {
                        "address": hex(address),
                        "mnemonic": insn.mnemonic,
                        "taken": taken,
                        "fallthrough": fallen,
                    }
                )
    return {
        "direction_or_eflags_writers_executed": flag_writers,
        "string_instructions_executed": string_ops,
        "conditional_jumps_executed": conditional,
        "one_sided": one_sided,
        "both_sided": conditional - len(one_sided),
        "indirect_jumps": [{"address": hex(a), "targets_seen": n} for a, n in indirect],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--set",
        dest="sets",
        action="append",
        required=True,
        metavar="LABEL=CAPTURES:FRAMES",
        help="a run: its directory of entry_NNNNN.bin and of frame_NNNNN.yuv, repeatable",
    )
    parser.add_argument("--xbe", type=Path, required=True, help="the certified retail default.xbe")
    parser.add_argument("--jobs", type=int, default=8, help="parallel replays (default 8)")
    parser.add_argument("--flags", type=lambda value: int(value, 0), default=0x202)
    parser.add_argument("--mmx-poison", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--first", type=int, default=0, help="lowest entry number replayed")
    parser.add_argument("--last", type=int, default=10**9, help="highest entry number replayed")
    parser.add_argument("--out", type=Path, help="write the summary JSON here (default stdout)")
    args = parser.parse_args()
    if not args.xbe.is_file():
        print(f"missing xbe {args.xbe}", file=sys.stderr)
        return 2
    jobs, unpaired = [], {}
    for spec in args.sets:
        label, _, directories = spec.partition("=")
        captures, _, frames = directories.partition(":")
        if not label or not captures or not frames:
            parser.error(f"--set {spec!r} is not LABEL=CAPTURES:FRAMES")
        found = sorted(
            (int(m.group(1)), path)
            for path in Path(captures).glob("entry_*.bin")
            if (m := ENTRY_FILE.search(path.name)) and args.first <= int(m.group(1)) <= args.last
        )
        for ordinal, path in found:
            frame = Path(frames) / f"frame_{ordinal:05d}.yuv"
            if not frame.is_file():
                unpaired.setdefault(label, []).append(ordinal)  # host stopped inside this call
                continue
            jobs.append((str(path), str(frame), str(args.xbe), args.flags, args.mmx_poison, label))
    if not jobs:
        print("no captures found", file=sys.stderr)
        return 2
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        results = sorted(
            pool.map(replay_one, jobs), key=lambda item: (item["set"], item["ordinal"])
        )
    code: dict[int, bytes] = {}
    edges: set[tuple[int, int]] = set()
    entries = []
    for item in results:
        row = {"set": item["set"], "ordinal": item["ordinal"], "matched": bool(item.get("matched"))}
        if "refusal" in item:
            row["refusal"] = item["refusal"]
        else:
            for address, data in item["code"].items():
                code[int(address)] = bytes.fromhex(data)
            edges.update(map(tuple, item["edges"]))
            row.update(
                {
                    "instructions": item["instructions"],
                    "returned": item["returned"],
                    "calls": {name: item["visited_entries"].get(name, 0) for name in CALLS},
                    "x87_instructions": len(item["executed_x87_instructions"]),
                    "missing_access": item["missing_access"],
                    "instruction_mismatch": item["instruction_mismatch"],
                    "different_bytes": item.get("output", {}).get("different_bytes"),
                    "crc32": item.get("output", {}).get("crc32"),
                }
            )
        entries.append(row)
    summary = {
        "entries": len(entries),
        "entries_without_native_frame": unpaired,
        "matched": sum(1 for row in entries if row["matched"]),
        "mismatched": [row for row in entries if not row["matched"]],
        "instructions_total": sum(row.get("instructions", 0) for row in entries),
        "x87_instructions_total": sum(row.get("x87_instructions", 0) for row in entries),
        "unique_instruction_addresses": len(code),
        "unique_edges": len(edges),
        "wrapper_calls_seen": {
            name: sorted({row["calls"][name] for row in entries if "calls" in row})
            for name in CALLS
        },
        "branches": classify(code, edges),
        "per_entry": entries,
    }
    text = json.dumps(summary, indent=1)
    if args.out:
        args.out.write_text(text + "\n")
    else:
        print(text)
    return 0 if summary["matched"] == summary["entries"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
