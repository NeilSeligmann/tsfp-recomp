"""Pin the measured integer continuation graph to one authentic private image."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

from tools.name_candidates import Image

ROOT = Path(__file__).resolve().parents[2]
IDENTITY = "guest-ret-pilot-v2"
IMAGE_SHA = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
SPANS = ((0x3E240, 27), (0x3E7C7, 17), (0x12E60, 39), (0x29AF0, 40))
ENTRIES = (
    0x3E240,
    0x3E245,
    0x3E24B,
    0x3E24D,
    0x3E24F,
    0x3E255,
    0x3E25A,
    0x3E7C7,
    0x3E7CC,
    0x3E7D2,
    0x3E7D5,
    0x3E7D7,
    0x12E79,
    0x29B15,
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def describe(xbe: Path) -> dict[str, object]:
    if digest(xbe) != IMAGE_SHA:
        raise ValueError("pilot image identity mismatch")
    image, decoder = Image(xbe), Cs(CS_ARCH_X86, CS_MODE_32)
    blocks, decoded = [], {}
    for start, size in SPANS:
        body = image.read(start, size)
        instructions = list(decoder.disasm(body, start))
        if sum(i.size for i in instructions) != size:
            raise ValueError("pilot span is not complete instructions")
        decoded.update({i.address: i for i in instructions})
        blocks.append({"va": start, "size": size, "sha256": hashlib.sha256(body).hexdigest()})
    if any(va not in decoded for va in ENTRIES):
        raise ValueError("pilot instruction boundary missing")
    if decoded[0x12E79].bytes.hex() != "ffd0" or decoded[0x29B15].bytes.hex() != "c20800":
        raise ValueError("pilot indirect/RETN instruction mismatch")
    sources = [ROOT / "src/host/recomp_ret_pilot.h", ROOT / "src/host/recomp_ret_pilot.c"]
    sources.extend(sorted((ROOT / "tools/retpilot").glob("*.py")))
    return {
        "identity": IDENTITY,
        "abi": 2,
        "layout": 2,
        "image_sha256": IMAGE_SHA,
        "sources": {str(p.relative_to(ROOT)): digest(p) for p in sources},
        "blocks": blocks,
        "entries": list(ENTRIES),
        "stop_before": 0x3E7D8,
        "instructions": {
            str(va): {
                "bytes": decoded[va].bytes.hex(),
                "text": f"{decoded[va].mnemonic} {decoded[va].op_str}".strip(),
            }
            for va in ENTRIES
        },
        "view_abi": 2,
        "compiled_eflags_mask": 0xED7,
        "required_state": 1,
        "capabilities": {
            "compiled_state": "flat integer only; TF/debug events unexecuted",
            "fallback_state": "flat integer plus imported TF single-step; other modes unexecuted",
            "permissions": "guest R=1,W=2,X=4; compiled fetch requires R|X",
            "code_identity": (
                "image SHA plus view/page generations and actual per-instruction bytes"
            ),
        },
        "domain": (
            "exclusive versioned mapping; pinned integer bytes; unexecuted capability handoff"
        ),
    }


def generate(xbe: Path, output: Path) -> Path:
    profile = describe(xbe)
    output.mkdir(parents=True, exist_ok=True)
    path = output / "profile.json"
    path.write_text(json.dumps(profile, sort_keys=True, indent=2) + "\n")
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, default=ROOT / "build/default.xbe")
    parser.add_argument("--out", type=Path, default=ROOT / "generated/ret-pilot/v2")
    args = parser.parse_args()
    print(generate(args.xbe, args.out))


if __name__ == "__main__":
    main()
