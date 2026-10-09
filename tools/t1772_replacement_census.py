# SPDX-License-Identifier: GPL-3.0-or-later
"""T1772: replacement opportunity census over every game function (static, no proof run).

    python -m tools.t1772_replacement_census --private-root /workspace \
        --scratch-dir tmp/t1772/call --out-dir docs/data/t1772-replacement-census

Reuses the CURRENT recognizers: the pilot static classifier (`tools.pilot_lists`, incl. the T1594
vector whitelist), the T1534 live-call-closure screen with the ABI recognizer and the caller audit
(`tools.call_draft_lists.run`, as T1578/T1603 do) and the hypothetical leaf caller audit
(`tools.replace.audit`). Population = the 10,368 functions the coverage tool classifies as game
(`tools.subsystems.loader.load_facts`, region game), NOT the older `pilot_lists.game_functions()`
set. Adds multi-label blocker flags (so the yield of a blocker class when solved alone can be
computed), the cause of every decode mismatch, the measured baseline harness outcome per VA
(`generated/harness/results.csv`, the file behind the snapshot `judgeable` number) and the
per-band ready-to-draft work lists. Nothing owner-private is written: addresses, sizes and counts.
Gates, `game_replace.h`, the proof snapshot and badges are not touched.
"""

from __future__ import annotations

import argparse
import collections
import csv
import json
import os
import re
import struct
import sys
import types
from pathlib import Path
from typing import Any

from tools import call_draft_lists as cdl
from tools import llm_replacement_draft as draft
from tools import name_additions as na
from tools import pilot_lists as pilot
from tools import t1601_ui_hud_mapedit_lists as t1601
from tools import t1772_census_report as report
from tools.vector_whitelist import (
    touches_vector,
    vector_body_admissible,
    vector_scalar_fp_body_admissible,
)

BANDS = (
    ("T1475", 0x00000000, 0x00080000),
    ("T1476", 0x00080000, 0x00100000),
    ("T1477", 0x00100000, 0x00180000),
    ("T1478", 0x00180000, 0x00280000),
    ("T1479", 0x00280000, 0x00380000),
    ("T1480", 0x00380000, 0x00470000),
    ("beyond", 0x00470000, 1 << 32),
)
HELD = {"T1475", "T1476"}
LIST_BANDS = ("T1477", "T1478", "T1479", "T1480")
BUCKETS = (
    ("1-4", 1, 4),
    ("5-20", 5, 20),
    ("21-60", 21, 60),
    ("61-150", 61, 150),
    ("151+", 151, 1 << 30),
)
STEP2B = frozenset({"divss", "cvttss2si", "movaps"})
LIST_SIZE = 25
CLASS_CSV = Path("tools/data/function_subsystems.csv")
BASELINE = Path("generated/harness/results.csv")

#: first-blocker order of the census waterfall (the pilot order, plus the call screen and audit)
ORDER = (
    "proven",
    "registered-unproven",
    "decode-mismatch",
    "size-over-300",
    "too-short",
    "x87",
    "vector-unadmitted",
    "privileged-or-segment",
    "self-modifying-code",
    "indirect-jump",
    "jump-out-of-body",
    "call-screen",
    "leaf-caller-audit",
    "pilot-heuristic",
    "eligible",
)


def band_of(va: int) -> str:
    return next(name for name, lo, hi in BANDS if lo <= va < hi)


def bucket_of(count: int) -> str:
    return next((name for name, lo, hi in BUCKETS if lo <= count <= hi), "n/a")


def norm_reason(text: str) -> str:
    """Call-screen refusal text without VAs, as a stable class key."""
    text = re.sub(r"0x[0-9a-fA-F]+", "VA", text)
    text = re.sub(r"function VA ", "", text)
    return text.strip()


def read_subsystems(path: Path) -> dict[int, tuple[str, str]]:
    if not path.is_file():
        return {}
    with path.open(newline="", encoding="utf-8") as handle:
        return {
            int(row["address"], 16): (row["subsystem"], row["confidence"])
            for row in csv.DictReader(handle)
        }


def read_baseline(path: Path) -> dict[int, dict[str, Any]]:
    """Per VA outcome of the lifted-only baseline harness run behind the snapshot `judgeable`."""
    if not path.is_file():
        return {}
    verdict_outcomes = {"AGREE", "DISAGREE", "SUBJECT-FAULTED"}
    per: dict[int, dict[str, Any]] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            va = int(row["va"], 16)
            entry = per.setdefault(va, {"verdict": False, "faulted": 0, "agree": 0, "diag": ""})
            outcome = row["outcome"]
            has_case = bool((row["case_index"] or "").strip())
            if outcome in verdict_outcomes and has_case:
                entry["verdict"] = True
                if outcome == "AGREE":
                    entry["agree"] += 1
            elif outcome == "ORACLE-FAULTED" and has_case:
                entry["faulted"] += 1
            elif outcome.startswith("SKIPPED") and row["diagnosis"]:
                entry["diag"] = row["diagnosis"]
    return per


def decode_cause(code: bytes, va: int, md: Any, body_max_va: int) -> str:
    """Why a linear decode of the function bytes does not consume exactly its size."""
    items = list(md.disasm(code, va))
    consumed = sum(i.size for i in items)
    size = len(code)
    if body_max_va and body_max_va >= va + size:
        noncontiguous = True
    else:
        noncontiguous = False
    table_jump = any(
        i.mnemonic == "jmp" and re.search(r"\*[48]\s*\+\s*0x[0-9a-f]+\]", i.op_str) for i in items
    )
    tail = code[consumed:]
    if consumed >= size:
        return "decode-overshoot"
    if table_jump and len(tail) >= 4:
        words = [struct.unpack_from("<I", tail, k)[0] for k in range(0, len(tail) - 3, 4)]
        near = [w for w in words if va <= w < va + size + 0x4000]
        if len(near) >= max(2, len(words) // 2):
            return "embedded-jump-table"
        return "table-jump-other-tail"
    if noncontiguous:
        return "noncontiguous-body"
    if len(tail) < 15 and not items:
        return "undecodable-start"
    if set(tail) <= {0xCC, 0x90, 0x00}:
        return "trailing-padding-undecoded"
    if len(tail) < 15:
        return "truncated-final-instruction"
    words = [struct.unpack_from("<I", tail, k)[0] for k in range(0, len(tail) - 3, 4)]
    if words and all(0x10000 <= w < 0x800000 for w in words[: min(4, len(words))]):
        return "embedded-pointer-data"
    return "embedded-data-or-bad-bounds"


def static_labels(func: Any, md: Any, text_range: tuple[int, int]) -> dict[str, Any]:
    """All independent static flags of a decoded function (the pilot collects only the first)."""
    labels: list[str] = []
    items = []
    for insn in func.insns:
        got = list(md.disasm(insn.raw, insn.address))
        if len(got) != 1:
            return {"labels": ["decode-mismatch"], "vector": "", "calls": 0, "meaningful": 0}
        items.append((insn, got[0]))
    plain = [i for _, i in items]
    groups = [{i.group_name(g) for g in i.groups} for i in plain]
    meaningful = sum(i.mnemonic not in {"nop", "int3"} for i in plain)
    if func.size > 300:
        labels.append("size-over-300")
    if meaningful < 5:
        labels.append("too-short")
    if any(i.mnemonic.startswith("f") or "fpu" in g for i, g in zip(plain, groups, strict=True)):
        labels.append("x87")
    vector = ""
    if any(touches_vector(i) for i in plain):
        if vector_body_admissible(plain):
            vector = "vector-state"
        elif vector_scalar_fp_body_admissible(plain):
            vector = "fp-scalar-v1"
        elif vector_scalar_fp_body_admissible(plain, STEP2B):
            vector = "fp-scalar-step2b"
        else:
            vector = "unadmitted"
            labels.append("vector-unadmitted")
    priv = {"privilege", "int", "iret", "interrupt"}
    if any(
        g & priv
        or "fs:" in n.operands
        or "gs:" in n.operands
        or "cs:" in n.operands
        or i.mnemonic in {"hlt", "cli", "sti", "in", "out", "int3", "ud2"}
        for (n, i), g in zip(items, groups, strict=True)
    ):
        labels.append("privileged-or-segment")
    lo, hi = text_range
    for n, i in items:
        dest = n.operands.split(",")[0]
        if i.mnemonic in {"call", "jmp", "ret"} or i.mnemonic.startswith("j"):
            continue
        hit = pilot.MEM_ABS.search(dest)
        if "[" in dest and hit and lo <= int(hit.group(1), 16) < hi and i.mnemonic != "cmp":
            if i.mnemonic not in {"test", "push"}:
                labels.append("self-modifying-code")
                break
    calls = sum(1 for _, i in items if i.mnemonic == "call")
    if calls:
        labels.append("calls")
    if any(i.mnemonic == "call" and not n.operands.startswith("0x") for n, i in items):
        labels.append("indirect-call")
    if any(n.mnemonic.startswith("j") and not n.operands.startswith("0x") for n, _ in items):
        labels.append("indirect-jump")
    for n, _ in items:
        if (n.mnemonic.startswith("j") or n.mnemonic == "loop") and n.operands.startswith("0x"):
            target = int(n.operands.split()[0], 16)
            if not func.va <= target < func.end:
                labels.append("jump-out-of-body")
                break
    if plain and plain[-1].mnemonic not in {"ret", "retn"}:
        labels.append("no-near-ret")
    inputs = pilot.register_input(func, md)
    if inputs:
        labels.append("input:" + ",".join(sorted(inputs)))
        if tuple(sorted(inputs, key=cdl.REGS.index)) not in cdl.SUPPORTED:
            labels.append("unsupported-input-set")
    if (
        pilot.stores_through_pointer(func)
        and sum(n.mnemonic in pilot.BITOPS for n in func.insns) >= 3
    ):
        labels.append("pointer-writing-bit-packer")
    if pilot.loop_with_memory(func):
        labels.append("loop-with-memory-effects")
    return {"labels": labels, "vector": vector, "calls": calls, "meaningful": meaningful}


def first_static_blocker(labels: list[str]) -> str:
    """Waterfall class for the pilot order (size is its own class here, applied after decode)."""
    for name in (
        "size-over-300",
        "too-short",
        "x87",
        "vector-unadmitted",
        "privileged-or-segment",
        "self-modifying-code",
        "indirect-jump",
        "jump-out-of-body",
    ):
        if name in labels:
            return name
    return ""


def caller_audit_rows(entries: list[Any], gen_dir: Path, xbe: Path) -> dict[int, dict[str, Any]]:
    """Caller audit with the T1534 predicate and the production data ranges."""
    from tools.harness.image import build_guest_image
    from tools.replace import audit as audit_module

    results = audit_module.audit_all(
        entries,
        gen_dir,
        build_guest_image(xbe),
        data_ranges=[(lo, hi) for _, lo, hi in cdl.data_ranges(xbe)],
    )
    out: dict[int, dict[str, Any]] = {}
    for item in results:
        why = []
        if not item.eligible:
            why.append("audit-not-eligible")
        if item.direct_sites < 1:
            why.append("no-direct-call-site")
        if item.tail_jumps:
            why.append("tail-jump-caller")
        if item.data_references:
            why.append("data-reference")
        if item.unsafe:
            why.append("unsafe-caller")
        if item.unresolved:
            why.append("unresolved-caller")
        out[item.va] = {"ok": cdl.caller_eligible(item), "why": why, "sites": item.direct_sites}
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--private-root", type=Path, required=True)
    ap.add_argument("--scratch-dir", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--baseline", type=Path, default=None)
    ap.add_argument("--subsystems", type=Path, default=None)
    ap.add_argument("--profiles-root", type=Path, default=None, help="owner session profiles")
    ap.add_argument("--host-root", type=Path, default=None, help="private host builds (addr2line)")
    ap.add_argument("--range", default="", help="LO-HI hex window for a quick smoke run")
    ap.add_argument(
        "--from-rows", action="store_true", help="rebuild tables and lists from the scratch rows"
    )
    ap.add_argument(
        "--reuse-cdl", type=Path, default=None, help="reuse a finished call-screen scratch dir"
    )
    ap.add_argument("--no-call-screen", action="store_true", help="skip the call screen (smoke)")
    args = ap.parse_args()
    os.setpriority(os.PRIO_PROCESS, 0, 19)
    if args.from_rows:
        rows = report.load(args.scratch_dir / "rows.json")
        call_report = json.loads((args.scratch_dir / "call_report.json").read_text())
        report.write_all(rows, call_report, args.out_dir, args.scratch_dir)
        return 0
    return run(args)


def load_merged_functions(world: Any, md: Any) -> dict[int, draft.Function]:
    """Decode every entry of the MERGED function table (functions.csv + overrides + additions).

    `draft.load_functions` reads `functions.csv` only, so the 1,980 `function_additions.csv`
    entries (and the sizes corrected by `function_overrides.csv`) never reach the screens and
    are counted as decode mismatches by the pilot tooling. Here they are decoded like the rest.
    """
    out: dict[int, draft.Function] = {}
    for entry in world.table:
        va, size = entry.entry_va, entry.size_bytes
        func = draft.Function(va, size)
        offset = 0
        for item in md.disasm(world.read(va, size), va):
            func.insns.append(
                draft.Insn(item.address, bytes(item.bytes), item.mnemonic, item.op_str, "")
            )
            offset += item.size
        if offset != size:
            func.insns = []
        out[va] = func
    return out


def unknown_callee_kind(
    failing: int, conventions: Any, sizes: dict[int, int], world: Any, md: Any
) -> str:
    """Nature of the callees whose pop amount is unknown inside one refused function."""
    size = sizes.get(failing)
    if size is None:
        return "failing-node-without-bound"
    kinds: list[str] = []
    for insn in md.disasm(world.read(failing, size), failing):
        if insn.mnemonic != "call" or not insn.op_str.startswith("0x"):
            continue
        target = int(insn.op_str, 16)
        if conventions.pop_bytes(target) is not None:
            continue
        tsize = sizes.get(target)
        if tsize is None:
            kinds.append("target-not-a-function-entry")
            continue
        body = list(md.disasm(world.read(target, tsize), target))
        if len(body) == 1 and body[0].mnemonic == "jmp" and "[" in body[0].op_str:
            kinds.append("import-thunk-jmp-through-memory")
        elif body and body[-1].mnemonic not in {"ret", "retn"}:
            kinds.append("callee-no-near-ret")
        else:
            kinds.append("callee-pop-undetermined")
    return kinds[0] if kinds else "none-found"


def indirect_call_kind(
    node: int, sizes: dict[int, int], world: Any, md: Any, iat: tuple[int, int]
) -> str:
    """Operand shape of the first indirect call inside one refused function."""
    from capstone.x86_const import X86_OP_MEM, X86_OP_REG

    size = sizes.get(node)
    if size is None:
        return "unknown-node"
    for insn in md.disasm(world.read(node, size), node):
        if insn.mnemonic not in {"call", "lcall"} or insn.op_str.startswith("0x"):
            continue
        op = insn.operands[0] if insn.operands else None
        if op is None:
            return "other"
        if op.type == X86_OP_REG:
            return "register-target"
        if op.type == X86_OP_MEM:
            if not op.mem.base and not op.mem.index:
                slot = op.mem.disp & 0xFFFFFFFF
                return "kernel-import-slot" if iat[0] <= slot < iat[1] else "absolute-global-slot"
            return "register-relative-slot"
        return "other"
    return "none-found"


def run(args: argparse.Namespace) -> int:
    private = args.private_root.resolve()
    pilot.SIZE_ALLOWED = 1 << 30  # size is a separate class in the census
    from tools.harness.callclosure import LiveClosureError, discover_live_call_closure
    from tools.harness.callstub import CalleeConventions
    from tools.replace.manifest import ManifestEntry
    from tools.subsystems.loader import load_facts
    from tools.vector_whitelist import admissible_scalar_fp_instruction

    snapshot = Path("docs/data/replace-proof-snapshot.json")
    loaded = load_facts(
        Path("."),
        private / "build/default.xbe",
        private / "generated/retail",
        proof_snapshot=snapshot,
    )
    game = sorted(f.va for f in loaded.facts if f.is_game)
    library = {f.va for f in loaded.facts if not f.is_game}
    if args.range:
        lo, hi = (int(x, 16) for x in args.range.split("-"))
        game = [va for va in game if lo <= va < hi]
    world = na.World(private)
    md = draft._capstone()
    functions = load_merged_functions(world, md)
    additions = set(world.additions)
    body_max: dict[int, int] = {}
    with (private / "generated/retail/functions.csv").open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            body_max[int(row["entry_va"], 16)] = int(row["body_max_va"], 16)
    snap = json.loads(snapshot.read_text(encoding="utf-8"))
    proven_all = {int(i["va"], 16) for i in snap["functions"] if i["gate"] is None}
    proven = proven_all & set(game)
    snap_failed = {
        int(i["va"], 16)
        for i in snap["functions"]
        if i["gate"] not in (None, "not-a-game-function")
    }
    registered = draft.registered_vas(Path("src/game"))
    receipts = pilot.rejected_vas()
    listed = t1601.mentions(t1601.DRAFT_GLOBS)
    subsystems = read_subsystems(args.subsystems or (private / CLASS_CSV))
    baseline = read_baseline(args.baseline or (private / BASELINE))
    text_range = (world.text_lo, world.text_hi)
    executed: set[int] = set()
    if args.profiles_root is not None and args.host_root is not None:
        executed = t1601.census_targets(args.profiles_root)
        sampled, _files = t1601.sampled_functions(args.profiles_root, args.host_root)
        executed |= sampled

    rec: dict[int, dict[str, Any]] = {}
    for va in game:
        func = functions.get(va)
        size = func.size if func else world.size.get(va, 0)
        row: dict[str, Any] = {
            "va": va,
            "size": size,
            "band": band_of(va),
            "subsystem": subsystems.get(va, ("", ""))[0],
            "addition": va in additions,
            "executed": va in executed,
            "labels": [],
            "vector": "",
            "calls": 0,
            "insns": 0,
            "meaningful": 0,
            "mention": "listed" if va in listed else ("receipt" if va in receipts else ""),
            "first": "",
            "tier": "",
            "why": "",
            "cause": "",
            "node": "",
        }
        if va in proven:
            row["status"] = "proven"
            row["first"] = "proven"
        elif va in registered:
            row["status"] = "registered-unproven"
            row["first"] = "registered-unproven"
            row["status_detail"] = (
                "snapshot-failed" if va in snap_failed else "no-proof-in-snapshot"
            )
        else:
            row["status"] = "open"
        rec[va] = row
        b = baseline.get(va)
        row["baseline"] = (
            "absent" if b is None else ("verdict" if b["verdict"] else "skip:" + (b["diag"] or "?"))
        )
        if b is not None and b["verdict"] is False and b["faulted"]:
            row["baseline"] = "fault-only"

    open_vas = [va for va in game if rec[va]["status"] == "open"]
    for va in game:
        func = functions.get(va)
        if func is not None and func.insns:
            info = static_labels(func, md, text_range)
            rec[va].update(
                labels=info["labels"],
                vector=info["vector"],
                calls=info["calls"],
                insns=len(func.insns),
                meaningful=info["meaningful"],
            )
    for va in open_vas:
        row = rec[va]
        func = functions.get(va)
        if func is None or not func.insns or "decode-mismatch" in row["labels"]:
            row["labels"] = ["decode-mismatch"]
            row["first"] = "decode-mismatch"
            row["cause"] = decode_cause(world.read(va, row["size"]), va, md, body_max.get(va, 0))
            continue
        row["first"] = first_static_blocker(row["labels"])

    # --- stage C: call-bearing / register-input strata through the T1534 call screen
    def pilot_pool(va: int) -> bool:
        r = rec[va]
        return (
            not r["first"]
            and r["vector"] in ("", "vector-state")
            and ("calls" in r["labels"] or any(x.startswith("input:") for x in r["labels"]))
        )

    call_pop = {va for va in open_vas if pilot_pool(va)}
    cdl_dir = args.reuse_cdl if args.reuse_cdl is not None else args.scratch_dir / "cdl"
    call_rows: dict[int, dict[str, Any]] = {}
    call_report: dict[str, Any] = {"dispositions": []}
    if call_pop and not args.no_call_screen:
        original_init = na.World.__init__
        original_rejected = pilot.rejected_vas
        original_load = draft.load_functions

        def init(self: Any, *a: Any, **k: Any) -> None:
            original_init(self, *a, **k)
            self.size = {va: s for va, s in self.size.items() if va in call_pop}

        na.World.__init__ = init  # type: ignore[method-assign]
        pilot.rejected_vas = lambda: set()  # population already filtered above
        draft.load_functions = lambda *a, **k: functions  # merged table incl. additions
        try:
            if args.reuse_cdl is None:
                cdl.run(private, args.scratch_dir / "cdl")
        finally:
            na.World.__init__ = original_init  # type: ignore[method-assign]
            pilot.rejected_vas = original_rejected
            draft.load_functions = original_load
        call_report = json.loads((cdl_dir / "report.json").read_text())
        for disp in call_report["dispositions"]:
            va = int(disp["va"], 16)
            if disp["status"] == "refused":
                rec[va]["first"] = "call-screen"
                rec[va]["cause"] = norm_reason(disp.get("reason", ""))
                m = re.search(r"function (0x[0-9a-fA-F]{8})", disp.get("reason", ""))
                rec[va]["node"] = m.group(1).upper().replace("0X", "0x") if m else ""
        for path in sorted(cdl_dir.glob("list-*.json")):
            for item in json.loads(path.read_text())["functions"]:
                call_rows[int(item["va"], 16)] = item
    for va, item in call_rows.items():
        row = rec[va]
        row["first"] = "eligible"
        row["tier"] = "B-call" if item["call_sites"] else "A-leaf-inputs"
        abi = item["abi"]
        row["abi"] = abi
        row["closure_nodes"] = item["closure_nodes"]
        row["closure_insns"] = item["closure_insns"]
        row["audit_sites"] = item["audit"]["direct_sites"]
        if item["call_sites"]:
            row["why"] = (
                f"live-call-closure {item['closure_nodes']} nodes, {abi['convention']} "
                f"{abi['stack_args']} args, inputs {','.join(abi['register_inputs']) or 'none'}, "
                f"caller audit eligible ({item['audit']['direct_sites']} direct sites)"
            )
        else:
            row["why"] = (
                f"leaf, inputs {','.join(abi['register_inputs']) or 'none'} ({abi['convention']} "
                f"{abi['stack_args']} args), caller audit eligible "
                f"({item['audit']['direct_sites']} sites)"
            )
        row["simplicity"] = item["simplicity"]

    # --- callee analysis for callee-convention-unknown refusals
    sizes = {va: f.size for va, f in functions.items()}
    names = {va: f"sub_{va:08X}" for va in sizes}
    conventions = CalleeConventions(sizes, world.read, draft._capstone())
    for va in open_vas:
        r = rec[va]
        if r["first"] == "call-screen" and "callee-convention-unknown" in r["cause"] and r["node"]:
            r["unknown_callee"] = unknown_callee_kind(
                int(r["node"], 16), conventions, sizes, world, md
            )

    iat_lo = world.xbe.kernel_thunk_addr
    iat_hi = iat_lo + 4 * (len(world.xbe.kernel_import_ordinals) + 1)
    for va in open_vas:
        r = rec[va]
        if r["first"] == "call-screen" and "indirect-call" in r["cause"] and r["node"]:
            r["indirect_call"] = indirect_call_kind(
                int(r["node"], 16), sizes, world, md, (iat_lo, iat_hi)
            )

    # --- stage V: call-bearing roots refused only for vector state, closure with the whitelist
    recognizer = cdl.ABIRecognizer(sizes, world.read)
    vec_cands = [
        va
        for va in open_vas
        if (rec[va]["first"] == "call-screen" and "sse-mmx" in rec[va]["cause"])
        or (
            not rec[va]["first"]
            and rec[va]["vector"] in ("fp-scalar-v1",)
            and (
                "calls" in rec[va]["labels"]
                or any(x.startswith("input:") for x in rec[va]["labels"])
            )
        )
    ]
    vec_pass: dict[int, dict[str, Any]] = {}
    vec_reasons: dict[int, str] = {}
    for va in vec_cands:
        outcome = ""
        for mode in ("", "fp-scalar-v1"):
            try:
                nodes: set[int] = set()

                def bounded(address: int, count: int, nodes: set[int] = nodes) -> bytes:
                    nodes.add(address)
                    if len(nodes) > 128:
                        raise LiveClosureError("closure-node-bound-128")
                    return world.read(address, count)

                closure = discover_live_call_closure(
                    va,
                    sizes=sizes,
                    names=names,
                    read_code=bounded,
                    allow_vector=True,
                    vector_mode=mode,
                )
                for node in closure.nodes:
                    items = list(md.disasm(world.read(node.va, node.size), node.va))
                    cdl.closure_capability([i for i in items if not touches_vector(i)], text_range)
                abi = recognizer.infer(va)
                if abi.inputs not in cdl.SUPPORTED:
                    raise cdl.Refusal("unsupported-register-input-set:" + ",".join(abi.inputs))
                vec_pass[va] = {
                    "abi": abi.document(),
                    "mode": mode or "vector-state",
                    "nodes": len(closure.nodes),
                }
                outcome = ""
                break
            except (LiveClosureError, cdl.Refusal) as error:
                outcome = norm_reason(str(error))
        if va not in vec_pass:
            vec_reasons[va] = outcome
    vec_entries = [
        ManifestEntry(
            va=va,
            name=f"sub_{va:08X}",
            convention=v["abi"]["convention"],
            stack_args=v["abi"]["stack_args"],
            returns="eax",
            scratch=(),
            source=f"t1772_{va:08x}.c",
            register_inputs=tuple(v["abi"]["register_inputs"]) or None,
        )
        for va, v in vec_pass.items()
    ]
    vec_audit: dict[int, bool] = {}
    if vec_entries:
        from tools.harness.image import build_guest_image
        from tools.replace import audit as audit_module

        results = audit_module.audit_all(
            vec_entries,
            private / "generated/lifted/gen",
            build_guest_image(private / "build/default.xbe"),
            data_ranges=[(lo, hi) for _, lo, hi in cdl.data_ranges(private / "build/default.xbe")],
        )
        vec_audit = {item.va: cdl.caller_eligible(item) for item in results}
    for va in vec_cands:
        r = rec[va]
        r["vec_stage"] = (
            "closure+abi pass, caller audit " + ("eligible" if vec_audit.get(va) else "ineligible")
            if va in vec_pass
            else "refused: " + vec_reasons.get(va, "?")
        )
        if not (va in vec_pass and vec_audit.get(va)) and not r["first"]:
            r["first"] = "call-screen"
            r["cause"] = "vector-closure: " + (vec_reasons.get(va) or "caller-audit-gates")
        if va in vec_pass and vec_audit.get(va):
            r["first"] = "eligible"
            v = vec_pass[va]
            r["tier"] = "V-vector-call"
            r["vector_mode"] = v["mode"]
            r["abi"] = v["abi"]
            r["closure_nodes"] = v["nodes"]
            r["closure_insns"] = r["insns"]
            r["why"] = (
                f"vector closure ({v['mode']}, {v['nodes']} nodes), {v['abi']['convention']} "
                f"{v['abi']['stack_args']} args, caller audit eligible (T1772 probe)"
            )
            r["simplicity"] = 0

    # --- stage L: call-free, register-input-free leaves: draft screen + hypothetical caller audit
    leaf_pop = [
        va
        for va in open_vas
        if not rec[va]["first"]
        and rec[va]["vector"] in ("", "vector-state", "fp-scalar-v1")
        and "calls" not in rec[va]["labels"]
        and not any(x.startswith("input:") for x in rec[va]["labels"])
    ]
    gen_dir, xbe_path = private / "generated/lifted/gen", private / "build/default.xbe"

    def leaf_screen(
        vas: list[int],
    ) -> tuple[dict[int, dict[str, Any]], list[int], dict[int, Any], dict[int, Any]]:
        """Draft screen then the EXACT and the ecx/edx-scratch caller audits for leaf roots."""
        facts: dict[int, dict[str, Any]] = {}
        rejected: list[int] = []
        original_admissible = draft.admissible_vector_instruction
        for va in vas:
            if rec[va]["vector"] == "fp-scalar-v1":
                draft.admissible_vector_instruction = lambda i: admissible_scalar_fp_instruction(i)
            got = draft.screen_function(functions[va], md, pilot.MIN_INSNS, 10_000)
            draft.admissible_vector_instruction = original_admissible
            if got is None:
                rejected.append(va)
            else:
                facts[va] = got

        def entries_for(scratch: tuple[str, ...]) -> list[Any]:
            return [
                ManifestEntry(
                    va=va,
                    name="hypothetical",
                    convention="stdcall" if f["ret_pop"] else "cdecl",
                    stack_args=f["stack_args_hint"],
                    returns="eax",
                    scratch=scratch,
                    source="hypothetical.c",
                )
                for va, f in facts.items()
            ]

        exact = caller_audit_rows(entries_for(()), gen_dir, xbe_path)  # GAME_REPLACE_EXACT
        scratch = caller_audit_rows(entries_for(("ecx", "edx")), gen_dir, xbe_path)
        return facts, rejected, exact, scratch

    other_blockers = {
        "too-short",
        "x87",
        "vector-unadmitted",
        "privileged-or-segment",
        "self-modifying-code",
        "indirect-jump",
        "jump-out-of-body",
        "no-near-ret",
        "unsupported-input-set",
    }
    large_pop = [
        va
        for va in open_vas
        if rec[va]["first"] == "size-over-300"
        and rec[va]["size"] <= 600
        and not (other_blockers & set(rec[va]["labels"]))
        and rec[va]["vector"] in ("", "vector-state")
        and not rec[va]["calls"]
        and not any(x.startswith("input:") for x in rec[va]["labels"])
    ]
    large = set(large_pop)
    facts, rejected_leaf, audits, scratch_audits = leaf_screen(leaf_pop + large_pop)
    for va in rejected_leaf:
        if va in large:
            continue  # stays size-over-300
        rec[va]["first"] = "pilot-heuristic"
        rec[va]["cause"] = "screen-function-reject"
    for va in facts:
        row = rec[va]
        aud = audits.get(va, {"ok": False, "why": ["audit-missing"], "sites": 0})
        row["scratch_audit_ok"] = scratch_audits.get(va, {"ok": False})["ok"]
        if not aud["ok"]:
            if va in large:
                continue  # stays size-over-300
            row["first"] = "leaf-caller-audit"
            row["cause"] = "+".join(aud["why"])
            continue
        hard = [
            x
            for x in row["labels"]
            if x in {"pointer-writing-bit-packer", "loop-with-memory-effects"}
        ]
        row["first"] = "eligible"
        row["tier"] = "C-leaf-hard" if hard else "A-leaf"
        if row["vector"] == "fp-scalar-v1":
            row["tier"] = "F-fp-leaf"
        if va in large:
            row["tier"] = "D-leaf-large"
        row["audit_sites"] = aud["sites"]
        row["abi"] = {
            "convention": "stdcall" if facts[va]["ret_pop"] else "cdecl",
            "stack_args": facts[va]["stack_args_hint"],
            "register_inputs": [],
        }
        row["closure_nodes"] = 1
        row["closure_insns"] = row["insns"]
        row["why"] = (
            f"leaf (0 calls, no register inputs), caller audit eligible "
            f"({aud['sites']} direct sites)"
            + (f", heuristic flags {','.join(hard)}" if hard else "")
        )
        row["simplicity"] = pilot.simplicity(functions[va])

    # --- validation: the same leaf screen over the roots that are already registered
    def leaf_candidate(va: int) -> bool:
        r = rec[va]
        return (
            bool(r["insns"])
            and not first_static_blocker([x for x in r["labels"] if x != "too-short"])
            and r["vector"] in ("", "vector-state", "fp-scalar-v1")
            and not r["calls"]
            and not any(x.startswith("input:") for x in r["labels"])
        )

    validation: dict[str, Any] = {}
    reg_leaf = [va for va in game if rec[va]["status"] != "open" and leaf_candidate(va)]
    if reg_leaf and not args.no_call_screen:
        vfacts, vrejected, vexact, vscratch = leaf_screen(reg_leaf)
        outcome: dict[str, collections.Counter[str]] = collections.defaultdict(collections.Counter)
        for va in reg_leaf:
            kind = rec[va]["status"]
            if va in vrejected:
                outcome[kind]["screen-function-reject"] += 1
            elif vexact.get(va, {"ok": False})["ok"]:
                outcome[kind]["census-eligible"] += 1
            else:
                outcome[kind]["caller-audit:" + "+".join(vexact[va]["why"])] += 1
            if va in vscratch and vscratch[va]["ok"]:
                outcome[kind]["(also ecx/edx-scratch audit ok)"] += 1
        validation["leaf_roots"] = {k: dict(v.most_common()) for k, v in outcome.items()}
        validation["leaf_roots_total"] = len(reg_leaf)

    # --- stage T: static jump tables (T1576 domain). The harness CLI has --static-jump-tables
    # for roots, `tools.replace prove` and `batch_prove` do not pass it.
    from tools.harness.static_tables import prove_static_tables

    blockers_other = {
        "size-over-300",
        "too-short",
        "x87",
        "vector-unadmitted",
        "privileged-or-segment",
        "self-modifying-code",
        "jump-out-of-body",
        "no-near-ret",
        "unsupported-input-set",
    }
    jt_roots = [
        va
        for va in open_vas
        if "indirect-jump" in rec[va]["labels"]
        and not (blockers_other & set(rec[va]["labels"]))
        and not rec[va]["calls"]
        and not any(x.startswith("input:") for x in rec[va]["labels"])
    ]
    jt_provable = []
    for va in jt_roots:
        code = world.read(va, rec[va]["size"])
        try:
            if prove_static_tables(va, rec[va]["size"], code, world.read) is not None:
                jt_provable.append(va)
        except (ValueError, IndexError):
            pass
    jt_entries = []
    for va in jt_provable:
        last = list(md.disasm(world.read(va, rec[va]["size"]), va))[-1]
        pop = int(last.op_str, 0) if last.op_str.startswith("0x") else 0
        jt_entries.append(
            ManifestEntry(
                va=va,
                name="hypothetical",
                convention="stdcall" if pop else "cdecl",
                stack_args=pop // 4,
                returns="eax",
                scratch=(),
                source="hypothetical.c",
            )
        )
    jt_audit = (
        caller_audit_rows(
            jt_entries, private / "generated/lifted/gen", private / "build/default.xbe"
        )
        if jt_entries
        else {}
    )
    jt_ok = [va for va in jt_provable if jt_audit.get(va, {"ok": False})["ok"]]
    closure_jump_roots = [
        va
        for va in open_vas
        if rec[va]["first"] == "call-screen" and "unsupported jump" in rec[va]["cause"]
    ]
    nodes_checked: dict[int, bool] = {}
    for va in closure_jump_roots:
        node = int(rec[va]["node"], 16) if rec[va]["node"] else 0
        if node and node not in nodes_checked and node in sizes:
            try:
                nodes_checked[node] = (
                    prove_static_tables(
                        node,
                        sizes[node],
                        world.read(node, sizes[node]),
                        world.read,
                        allow_calls=True,
                    )
                    is not None
                )
            except (ValueError, IndexError):
                nodes_checked[node] = False
    for va in jt_ok:
        rec[va]["jump_table_eligible"] = True
    jump_tables = {
        "root_candidates_only_indirect_jump_blocker": len(jt_roots),
        "root_static_table_provable": len(jt_provable),
        "root_static_table_and_caller_audit_ok": len(jt_ok),
        "root_examples": [f"0x{v:08X}" for v in jt_ok[:5]],
        "closure_jump_refusals": len(closure_jump_roots),
        "closure_jump_failing_nodes": len(nodes_checked),
        "closure_jump_nodes_static_table_provable": sum(nodes_checked.values()),
        "closure_jump_roots_with_provable_node": sum(
            1
            for va in closure_jump_roots
            if rec[va]["node"] and nodes_checked.get(int(rec[va]["node"], 16))
        ),
    }

    # --- what-if: function extents repaired (table size_bytes shorter than the contiguous body)
    repair: dict[int, int] = {}
    for va in open_vas:
        r = rec[va]
        if r["cause"] != "noncontiguous-body":
            continue
        span = body_max.get(va, 0) - va + 1
        items = list(md.disasm(world.read(va, span), va)) if span > r["size"] else []
        inner = [e for e in world.entries if va < e < va + span]
        if items and sum(i.size for i in items) == span and items[-1].mnemonic in ("ret", "retn"):
            if not inner:
                repair[va] = span
    extent: dict[str, Any] = {
        "noncontiguous_body_rows": sum(rec[v]["cause"] == "noncontiguous-body" for v in open_vas),
        "clean_linear_span_ending_in_ret_no_inner_entries": len(repair),
        "span_minus_size_histogram": dict(
            collections.Counter(min(repair[v] - rec[v]["size"], 16) for v in repair).most_common()
        ),
    }
    saved = {va: (functions[va], dict(rec[va])) for va in repair}
    for va, span in repair.items():
        func = draft.Function(va, span)
        for item in md.disasm(world.read(va, span), va):
            func.insns.append(
                draft.Insn(item.address, bytes(item.bytes), item.mnemonic, item.op_str, "")
            )
        functions[va] = func
        info = static_labels(func, md, text_range)
        rec[va].update(
            size=span,
            labels=info["labels"],
            vector=info["vector"],
            calls=info["calls"],
            insns=len(func.insns),
            meaningful=info["meaningful"],
        )
    extent["first_static_blocker"] = dict(
        collections.Counter(
            first_static_blocker(rec[v]["labels"]) or "none" for v in repair
        ).most_common()
    )
    leafy = [
        v
        for v in repair
        if not first_static_blocker(rec[v]["labels"])
        and rec[v]["vector"] in ("", "vector-state", "fp-scalar-v1")
        and not rec[v]["calls"]
        and not any(x.startswith("input:") for x in rec[v]["labels"])
    ]
    extent["static_clean_leaf_candidates"] = len(leafy)
    extent["static_clean_call_or_input_candidates"] = sum(
        1
        for v in repair
        if not first_static_blocker(rec[v]["labels"])
        and rec[v]["vector"] in ("", "vector-state")
        and (rec[v]["calls"] or any(x.startswith("input:") for x in rec[v]["labels"]))
    )
    if leafy and not args.no_call_screen:
        efacts, erejected, eexact, _escr = leaf_screen(leafy)
        extent["leaf_eligible_after_repair"] = sum(
            1 for v in efacts if eexact.get(v, {"ok": False})["ok"]
        )
        extent["leaf_screen_rejected"] = len(erejected)
        extent["leaf_caller_audit_failed"] = sum(
            1 for v in efacts if not eexact.get(v, {"ok": False})["ok"]
        )
        extent["leaf_eligible_examples"] = [
            f"0x{v:08X}" for v in sorted(efacts) if eexact.get(v, {"ok": False})["ok"]
        ][:6]
    extent["candidates_file"] = "extent-repair-candidates.json"
    extent["_repair"] = {f"0x{v:08X}": repair[v] for v in sorted(repair)}
    for va, (func, old) in saved.items():
        functions[va] = func
        rec[va].clear()
        rec[va].update(old)

    # --- what-if: the T1577 stack-taint guards removed (UNSOUND simulation, upper bound only)
    whatif: dict[str, Any] = {}
    taint = [
        va
        for va in open_vas
        if rec[va]["first"] == "call-screen"
        and ("stack-derived-pointer" in rec[va]["cause"] or "stack-derived-ebp" in rec[va]["cause"])
    ]
    if taint:
        source = Path(cdl.__file__).read_text(encoding="utf-8")
        for guard in ("abi-stack-derived-ebp", "abi-stack-derived-pointer"):
            source = source.replace(f'raise Refusal("{guard}")', "pass")
        namespace = types.ModuleType("cdl_whatif")
        sys.modules["cdl_whatif"] = namespace  # dataclasses resolves annotations through it
        exec(compile(source, "cdl_whatif", "exec"), namespace.__dict__)  # noqa: S102
        relaxed = namespace.ABIRecognizer(sizes, world.read)
        survivors: dict[int, Any] = {}
        reasons: collections.Counter[str] = collections.Counter()
        for va in taint:
            try:
                abi = relaxed.infer(va)
                if abi.inputs not in cdl.SUPPORTED:
                    raise cdl.Refusal("unsupported-register-input-set")
                survivors[va] = abi
            except ValueError as error:  # Refusal of the exec'd copy is a different class
                reasons[str(error).split(":")[0]] += 1
        audited = 0
        if survivors:
            from tools.harness.image import build_guest_image
            from tools.replace import audit as audit_module

            entries_ = [
                ManifestEntry(
                    va=va,
                    name=f"sub_{va:08X}",
                    convention=a.document()["convention"],
                    stack_args=a.stack_args,
                    returns="eax",
                    scratch=(),
                    source=f"t1772_{va:08x}.c",
                    register_inputs=tuple(a.inputs) or None,
                )
                for va, a in survivors.items()
            ]
            results_ = audit_module.audit_all(
                entries_,
                gen_dir,
                build_guest_image(xbe_path),
                data_ranges=[(lo, hi) for _, lo, hi in cdl.data_ranges(xbe_path)],
            )
            audited = sum(cdl.caller_eligible(item) for item in results_)
        whatif["stack_taint_guards_removed"] = {
            "roots_refused_by_guard": len(taint),
            "abi_infers_without_guard": len(survivors),
            "then_caller_audit_eligible": audited,
            "other_refusals_after_removal": dict(reasons.most_common()),
        }
    call_report["validation"] = validation
    call_report["whatif"] = whatif
    call_report["jump_tables"] = jump_tables
    call_report["extent_repair"] = extent

    for va in game:
        row = rec[va]
        if row["status"] == "open" and not row["first"]:
            row["first"] = (
                "fp-scalar-step2b" if row["vector"] == "fp-scalar-step2b" else "unclassified"
            )
        row["prereq"] = ""
        if row["first"] == "eligible":
            parts = []
            if row["tier"] in ("B-call", "V-vector-call"):
                parts.append("live-call-closure")
            if row["abi"]["register_inputs"]:
                parts.append("input-" + "_".join(row["abi"]["register_inputs"]))
            if row["vector"] or row.get("vector_mode"):
                parts.append(row["vector"] or row["vector_mode"])
            row["prereq"] = "+".join(parts) or "none"

    write_outputs(args, rec, game, library, proven_all, registered, snap_failed, loaded, call_pop)
    (args.scratch_dir / "call_report.json").write_text(json.dumps(call_report), encoding="utf-8")
    report.write_all(rec, call_report, args.out_dir, args.scratch_dir)
    return 0


def write_outputs(
    args: argparse.Namespace,
    rec: dict[int, dict[str, Any]],
    game: list[int],
    library: set[int],
    proven: set[int],
    registered: set[int],
    snap_failed: set[int],
    loaded: Any,
    call_pop: set[int],
) -> None:
    """Persist the per-function rows (scratch, ignored) for `--from-rows` and the report."""
    args.scratch_dir.mkdir(parents=True, exist_ok=True)
    (args.scratch_dir / "rows.json").write_text(
        json.dumps({f"0x{va:08X}": rec[va] for va in game}, sort_keys=True), encoding="utf-8"
    )


if __name__ == "__main__":
    raise SystemExit(main())
