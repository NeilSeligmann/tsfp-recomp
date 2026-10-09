# SPDX-License-Identifier: GPL-3.0-or-later
"""T1595: static drafting lists for the 53 functions the T1594 vector whitelist made draftable.

Static only (no proof, no API). Population: the 53 VAs of docs/evidence/vector-whitelist.md.
Exclusions: proven, failed, registered, rejected and any VA named by an existing drafting,
pilot or evidence artefact. Entries use the T1534 row format. The recipe adds the vector state
flags (`live_vector_state`, which `tools.replace prove` only accepts together with
`--live-call-closure` and `--only-va`, so the closure flag is on for these leaf roots too).

    python -m tools.t1595_vector_lists --private-root /workspace \\
        --out-dir docs/data/t1595-vector-lists
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from dataclasses import asdict
from pathlib import Path
from typing import Any

from tools import call_draft_lists as cdl
from tools import llm_replacement_draft as draft
from tools import pilot_lists as pilot
from tools.harness.callclosure import LiveClosureError, discover_live_call_closure
from tools.harness.image import build_guest_image
from tools.replace.audit import audit_all
from tools.replace.straddle_corpus import data_ranges
from tools.vector_whitelist import admissible_vector_instruction, touches_vector

WHITELIST_DOC = Path("docs/evidence/vector-whitelist.md")
PER_LIST = 14
HEX = re.compile(r"(?:0x|sub_)([0-9a-fA-F]{6,8})\b")
SCAN_FILES = (
    "docs/t-*list*.md",
    "docs/t-af-draft-list001.md",
    "docs/t-semantic-review-drafted-bodies.md",
    "docs/data/t154[0-9]*",
)
SCAN_DIRS = (
    "docs/data/t1534-call-draft-lists",
    "docs/data/t-pilot-lists",
    "docs/data/t1541-list002",
    "docs/data/t1557-refusal-lists",
    "docs/data/t1578-newly-reachable-lists",
    "docs/data/t1583-reach-list001",
    "docs/data/t1587-af-kill-lists",
    "docs/data/t1588-af-list001",
    "docs/evidence/t1543",
)
# Files that name the 53 by construction (this task and T1594) or are a refusal population.
SELF_FILES = {
    "docs/evidence/vector-whitelist.md",
    "docs/t-vec-draft-list001.md",
}
SELF_PREFIXES = ("docs/data/t1595-vector-lists/",)
T1534_POPULATION = {
    "pre-audit-dispositions.json",
    "report.json",
    "pins-before.json",
    "pins-after.json",
}


# Mirrors the `vector_comparison` block tools.harness.cli writes into a vector proof closure.
VECTOR_COMPARISON = {
    "enabled": True,
    "registers": "all8 XMM raw128-bit exact equality; missing output refuses verdict",
    "mxcsr": "exact16-bit equality; movement/bitwise preserve control/status",
    "input_stream": "vector-v1 SHA256(seed,index,VA,register), first16 little-endian bytes",
    "refused": "MMX/EMMS, arithmetic, aligned-memory movement, unlisted vector forms",
}


def population() -> list[int]:
    text = WHITELIST_DOC.read_text(encoding="utf-8")
    block = text.split("The 53:", 1)[1].split("Reproduce:", 1)[0]
    vas = sorted({int(m, 16) for m in re.findall(r"0x([0-9A-Fa-f]{8})", block)})
    if len(vas) != 53:
        raise SystemExit(f"expected 53 whitelist VAs, parsed {len(vas)}")
    return vas


def mentions(candidates: set[int]) -> dict[int, list[str]]:
    """Map candidate VA to the artefacts that name it (rejected, drafted, proven, listed)."""
    files: list[Path] = []
    for directory in SCAN_DIRS:
        files += [p for p in Path(directory).rglob("*") if p.is_file()]
    for pattern in SCAN_FILES:
        for p in Path(".").glob(pattern):
            files += [q for q in p.rglob("*") if q.is_file()] if p.is_dir() else [p]
    found: dict[int, list[str]] = {}
    for path in sorted(set(files)):
        name = path.as_posix()
        if name in SELF_FILES or name.startswith(SELF_PREFIXES):
            continue
        if path.name in T1534_POPULATION and "t1534" in name:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for va in {int(m, 16) for m in HEX.findall(text)} & candidates:
            found.setdefault(va, []).append(name)
    return found


def closure_capability_vector(items: list[Any], text: tuple[int, int]) -> None:
    """closure_capability with the T1594 whitelist: only admitted vector forms pass."""
    plain = []
    for item in items:
        if touches_vector(item):
            if not admissible_vector_instruction(item):
                raise cdl.Refusal("closure-vector-not-whitelisted:" + item.mnemonic)
        else:
            plain.append(item)
    cdl.closure_capability(plain, text)


def run(private: Path, out: Path) -> int:
    from tools.name_additions import World

    if out.exists() and any(out.glob("list-*.json")):
        raise SystemExit("refuse overwrite: remove the previous lists first")
    out.mkdir(parents=True, exist_ok=True)
    pop = population()
    pins = {
        str(p): cdl.sha(p)
        for p in (
            private / "build/default.xbe",
            private / "generated/retail/functions.csv",
            WHITELIST_DOC,
            Path("docs/data/replace-proof-snapshot.json"),
        )
    }
    world = World(private)
    md = draft._capstone()
    functions = draft.load_functions(private / "generated/retail/functions.csv", world.read)
    proven, failed = pilot.snapshot_sets()
    registered = draft.registered_vas(Path("src/game"))
    rejected = pilot.rejected_vas()
    named = mentions(set(pop))
    sizes = {va: f.size for va, f in functions.items()}
    names = {va: f"sub_{va:08X}" for va in sizes}
    recognizer = cdl.ABIRecognizer(sizes, world.read)
    dispositions: list[dict[str, Any]] = []
    candidates: list[dict[str, Any]] = []
    for va in pop:
        row: dict[str, Any] = {"va": f"0x{va:08x}"}
        if va in proven:
            row.update(status="excluded", reason="proven")
        elif va in failed:
            row.update(status="excluded", reason="failed-in-snapshot")
        elif va in registered:
            row.update(status="excluded", reason="registered")
        elif va in rejected:
            row.update(status="excluded", reason="rejected-before")
        elif va in named:
            row.update(status="excluded", reason="named-elsewhere", files=named[va][:6])
        elif va not in functions:
            row.update(status="excluded", reason="decode-mismatch")
        else:
            f = functions[va]
            try:
                meaningful = sum(i.mnemonic not in {"nop", "int3"} for i in f.insns)
                closure = discover_live_call_closure(
                    va, sizes=sizes, names=names, read_code=world.read, allow_vector=True
                )
                closure_insns = 0
                for node in closure.nodes:
                    items = list(md.disasm(world.read(node.va, node.size), node.va))
                    closure_capability_vector(items, (world.text_lo, world.text_hi))
                    closure_insns += len(items)
                abi = recognizer.infer(va)
                if abi.inputs not in cdl.SUPPORTED:
                    raise cdl.Refusal("unsupported-register-input-set:" + ",".join(abi.inputs))
                code = world.read(va, f.size)
                document = closure.document(
                    xbe_sha256=pins[str(private / "build/default.xbe")],
                    functions_sha256=pins[str(private / "generated/retail/functions.csv")],
                )
                document["vector_comparison"] = VECTOR_COMPARISON
                row.update(
                    abi=abi.document(),
                    name=names[va],
                    known_name=world.names.get(va),
                    provenance={"pins": "pins-before.json", "status": "static only"},
                    limitations=[
                        "Not original/native proof or admission",
                        "Vector state is whitelist-only: movups movss movlps movhps movhlps "
                        "movlhps xorps andps andnps orps",
                        "Existing fault channel does not certify full faulty continuation",
                    ],
                    source=f"t1595_{va:08x}.c",
                    size=f.size,
                    bytes=code.hex(),
                    sha256=hashlib.sha256(code).hexdigest(),
                    meaningful_insns=meaningful,
                    closure_nodes=len(closure.nodes),
                    closure_insns=closure_insns,
                    call_sites=sum(i.mnemonic == "call" for i in f.insns),
                    simplicity=pilot.simplicity(f),
                    disassembly=draft.disassembly_text(f),
                    closure=document,
                    stratum="vector-state",
                    recipe={
                        "live_call_closure": True,
                        "live_vector_state": True,
                        "live_call_boundaries": [],
                        "fixture_providers": [],
                        "count": 600,
                        "seeds": [20261001, 20261006],
                        "optimizations": [0, 3],
                        "scratch": [],
                        "input_contract": abi.document(),
                    },
                    status="pending-caller-audit",
                )
                candidates.append(row)
            except (LiveClosureError, cdl.Refusal) as error:
                row.update(status="excluded", reason="draft-screen:" + str(error))
        dispositions.append(row)
    entries = [cdl.manifest_entry(row) for row in candidates]
    image = build_guest_image(private / "build/default.xbe")
    audits = audit_all(
        entries,
        private / "generated/lifted/gen",
        image,
        data_ranges=[(lo, hi) for _, lo, hi in data_ranges(private / "build/default.xbe")],
    )
    eligible: list[dict[str, Any]] = []
    for row, audit in zip(candidates, audits, strict=True):
        row["audit"] = asdict(audit)
        row["audit_contract"] = cdl.manifest_entry(row).as_json()
        if cdl.caller_eligible(audit):
            row["status"] = "static-draft-eligible-NOT-proof"
            eligible.append(row)
        else:
            row.update(status="excluded", reason="caller-audit-gates")
    ranked = sorted(eligible, key=cdl.rank_key)
    lists = [
        {
            "number": n // PER_LIST + 1,
            "partial": len(ranked[n : n + PER_LIST]) < PER_LIST,
            "count": len(ranked[n : n + PER_LIST]),
            "functions": ranked[n : n + PER_LIST],
            "provenance": {
                "pins": "pins-before.json",
                "report": "report.json",
                "status": "static draft eligibility only; no proof/admission",
            },
        }
        for n in range(0, len(ranked), PER_LIST)
    ]
    if len({r["va"] for r in ranked}) != len(ranked):
        raise SystemExit("duplicate VA across lists")
    for item in lists:
        (out / f"list-{item['number']:03d}.json").write_text(
            json.dumps(item, indent=2, sort_keys=True) + "\n"
        )
    (out / "pins-before.json").write_text(json.dumps(pins, indent=2, sort_keys=True) + "\n")
    report = {
        "population": len(pop),
        "eligible": len(eligible),
        "lists": [item["count"] for item in lists],
        "dispositions": [
            {k: v for k, v in r.items() if k in {"va", "status", "reason", "files"}}
            for r in dispositions
        ],
    }
    (out / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "dispositions"}))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--private-root", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    return run(args.private_root.resolve(), args.out_dir)


if __name__ == "__main__":
    raise SystemExit(main())
