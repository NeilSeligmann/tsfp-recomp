# SPDX-License-Identifier: GPL-3.0-or-later
"""T1486: LLM-drafted replacement functions, gated by the tools.replace proof.

    python -m tools.llm_replacement_draft screen --band 0x80000-0x100000
    python -m tools.llm_replacement_draft draft  --candidates tmp/llm-draft/candidates.json \
        --openai-key-file .secrets/openai-api-key.txt --max-usd 2 --limit 10

`screen` lists unregistered, call-free, register-input-free functions in a band whose
hypothetical caller audit is eligible. `draft` sends each candidate's disassembly (function
text only) to the OpenAI client, writes the C draft into a scratch game directory, compiles it
at O0 and O3 with the project warnings, runs `tools.replace prove --only-va` at O0 and O3
and accepts the draft ONLY if the proof gate passes. A compile failure or a DISAGREE is fed
back to the model for at most `--max-repairs` repairs. Accepted sources are written to the
accept directory and nowhere else: a draft that fails the proof never leaves the scratch
directory. Integration into src/game stays a reviewed, manual step (docs/t1486-*.md).

The key is only ever passed as a path to the client constructor, never read here.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from tools.vector_whitelist import admissible_vector_instruction, touches_vector

FUNCTIONS_PATH = Path("generated/retail/functions.csv")
NAMES_PATH = Path("tools/data/function_names.csv")
GAME_DIR = Path("src/game")
DOCS_DIR = Path("docs")
DEFAULT_XBE = Path("build/default.xbe")
SCRATCH_ROOT = Path("tmp/llm-draft")
SEED = 20261006
# Documented legitimate seeding (docs/t77-round4-low.md): the cases-per-function ladder. The
# first rung is the harness default. A rung is tried only when the previous one reported
# 0 DISAGREE and failed on reach (verdicts, coverage, null-agree). Never a seed search.
CASES_LADDER: tuple[int | None, ...] = (None, 16, 4096, 65536)
MIN_VERDICTS = 100
MIN_COVERAGE = 0.9
MAX_NULL_AGREE = 0.9
FEWSHOT = (
    (0x00074670, "game_round4_low_a.c"),
    (0x0006B7E0, "game_round4_low_a.c"),
    (0x000426E0, "game_round4_low_b.c"),
)

SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {
        "c_source": {"type": "string"},
        "registration_kind": {"type": "string", "enum": ["GAME_REPLACE", "GAME_REPLACE_EXACT"]},
        "arg_layout": {"type": "string"},
        "notes": {"type": "string"},
        "unsure": {"type": "array", "items": {"type": "string"}},
    },
    "required": ["c_source", "registration_kind", "arg_layout", "notes", "unsure"],
    "additionalProperties": False,
}


@dataclass
class Insn:
    address: int
    raw: bytes
    mnemonic: str
    operands: str
    line: str


@dataclass
class Function:
    va: int
    size: int
    insns: list[Insn] = field(default_factory=list)

    @property
    def end(self) -> int:
        return self.va + self.size


def _capstone() -> Any:
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    return md


def load_functions(
    functions_csv: Path, code_at: Callable[[int, int], bytes], low: int = 0, high: int = 1 << 32
) -> dict[int, Function]:
    """Decode every function of `functions_csv` (entry_va,size_bytes) with capstone.

    A function whose linear decode does not consume exactly `size_bytes` is kept with an
    empty instruction list so it can never become a candidate."""
    md = _capstone()
    functions: dict[int, Function] = {}
    with functions_csv.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            va, size = int(row["entry_va"], 16), int(row["size_bytes"])
            if not low <= va < high:
                continue
            func = Function(va, size)
            code = code_at(va, size)
            offset = 0
            for item in md.disasm(code, va):
                func.insns.append(
                    Insn(item.address, bytes(item.bytes), item.mnemonic, item.op_str, "")
                )
                offset += item.size
            if offset != size:
                func.insns = []
            functions[va] = func
    return functions


def load_names(path: Path = NAMES_PATH) -> dict[int, str]:
    if not path.is_file():
        return {}
    with path.open(newline="", encoding="utf-8") as handle:
        return {int(row["entry_va"], 16): row["name"] for row in csv.DictReader(handle)}


def registered_vas(game_dir: Path = GAME_DIR) -> set[int]:
    from tools.replace.scan import scan_directory

    return {registration.va for registration in scan_directory(game_dir)}


def mentioned_in_receipts(docs_dir: Path = DOCS_DIR) -> set[int]:
    """Every 0x000XXXXX address named in any docs/t77-*.md receipt (accepted or rejected)."""
    found: set[int] = set()
    for path in docs_dir.glob("t77-*.md"):
        for match in re.finditer(r"0x([0-9A-Fa-f]{6,8})\b", path.read_text(encoding="utf-8")):
            found.add(int(match.group(1), 16))
    return found


# ---------------------------------------------------------------------------------------
# Screening
# ---------------------------------------------------------------------------------------

_BAD_GROUPS = {
    "fpu",
    "mmx",
    "sse1",
    "sse2",
    "sse3",
    "ssse3",
    "sse41",
    "sse42",
    "avx",
    "privilege",
    "int",
    "iret",
    "interrupt",
    "call",
}
_SKIP_READS = {"esp", "eip", "eflags", "rip"}
_PARENT = {
    "al": "eax",
    "ah": "eax",
    "ax": "eax",
    "cl": "ecx",
    "ch": "ecx",
    "cx": "ecx",
    "dl": "edx",
    "dh": "edx",
    "dx": "edx",
    "bl": "ebx",
    "bh": "ebx",
    "bx": "ebx",
    "si": "esi",
    "di": "edi",
    "bp": "ebp",
    "sp": "esp",
}


def _full(name: str) -> str:
    return _PARENT.get(name, name)


def screen_function(
    func: Function, md: Any, min_insns: int = 5, max_insns: int = 80
) -> dict[str, Any] | None:
    """Return facts for a call-free, register-input-free, in-body-jump function or None."""
    if not min_insns <= len(func.insns) <= max_insns:
        return None
    if func.insns[-1].mnemonic != "ret":
        return None
    written: set[str] = set()
    inputs: set[str] = set()
    stack_args = 0
    esp_delta = 0
    ebp_delta: int | None = None
    ret_pop = 0
    for insn in func.insns:
        decoded = list(md.disasm(insn.raw, insn.address))
        if len(decoded) != 1 or decoded[0].size != len(insn.raw):
            return None
        item = decoded[0]
        groups = {item.group_name(group) for group in item.groups}
        bad = groups & _BAD_GROUPS
        if touches_vector(item):
            if not admissible_vector_instruction(item):
                return None
            bad -= {"sse1", "sse2"}
        if bad or item.mnemonic in {"call", "int3", "int", "hlt", "cli", "sti"}:
            return None
        if "fs:" in insn.operands or "gs:" in insn.operands or item.mnemonic.startswith("f"):
            return None
        if item.mnemonic.startswith("j"):
            if not insn.operands.startswith("0x"):
                return None
            target = int(insn.operands.split()[0], 16)
            if not func.va <= target < func.end:
                return None
        read, write = item.regs_access()
        full = {_full(item.reg_name(reg)) for reg in read} - _SKIP_READS
        operands = insn.operands.replace(" ", "").split(",")
        xor_idiom = item.mnemonic in {"xor", "sub", "sbb"} and len(set(operands)) == 1
        if item.mnemonic == "push":
            full -= {"ebx", "esi", "edi", "ebp"}
        if not xor_idiom:
            inputs |= full - written
        written |= {_full(item.reg_name(reg)) for reg in write}
        if item.mnemonic == "push":
            esp_delta -= 4
        elif item.mnemonic == "pop":
            esp_delta += 4
        elif item.mnemonic in {"sub", "add"} and insn.operands.startswith("esp,"):
            amount = int(insn.operands.split(",")[1].strip(), 0)
            esp_delta += amount if item.mnemonic == "add" else -amount
        elif item.mnemonic == "mov" and operands == ["ebp", "esp"]:
            ebp_delta = esp_delta
        for base, delta in (("esp", esp_delta), ("ebp", ebp_delta)):
            if delta is None:
                continue
            for hit in re.finditer(rf"\[{base}(?:\s*\+\s*(0x[0-9a-f]+|\d+))?\]", insn.operands):
                offset = int(hit.group(1), 0) if hit.group(1) else 0
                if base == "esp":
                    index = (offset + delta) // 4 - 1
                    valid = offset + delta >= 4
                else:
                    index = (offset - 8) // 4
                    valid = offset >= 8
                if valid and index >= 0:
                    stack_args = max(stack_args, index + 1)
        if item.mnemonic == "ret" and insn.operands.strip():
            ret_pop = max(ret_pop, int(insn.operands.strip(), 0))
    if inputs & {"eax", "ecx", "edx", "ebx", "esi", "edi", "ebp"}:
        return None
    return {
        "va": func.va,
        "size": func.size,
        "insns": len(func.insns),
        "stack_args_hint": ret_pop // 4 if ret_pop else stack_args,
        "ret_pop": ret_pop,
    }


def screen(
    functions: dict[int, Function],
    low: int,
    high: int,
    registered: set[int],
    excluded: set[int],
    audit: Callable[[list[dict[str, Any]]], dict[int, bool]] | None,
) -> list[dict[str, Any]]:
    md = _capstone()
    cands = []
    for va in sorted(functions):
        if not low <= va < high or va in registered or va in excluded:
            continue
        facts = screen_function(functions[va], md)
        if facts is not None:
            cands.append(facts)
    if audit is not None:
        verdicts = audit(cands)
        cands = [fact for fact in cands if verdicts.get(fact["va"])]
    cands.sort(key=lambda fact: (fact["insns"], fact["va"]))
    return cands


def audit_eligibility(cands: list[dict[str, Any]], xbe: Path, gen_dir: Path) -> dict[int, bool]:
    """Hypothetical caller audit (ecx/edx scratch) through tools.replace.audit (import only)."""
    from tools.harness.image import build_guest_image
    from tools.replace import audit as audit_module
    from tools.replace.manifest import ManifestEntry

    entries = [
        ManifestEntry(
            va=fact["va"],
            name="hypothetical",
            convention="stdcall" if fact["ret_pop"] else "cdecl",
            stack_args=fact["stack_args_hint"],
            returns="eax",
            scratch=("ecx", "edx"),
            source="hypothetical.c",
        )
        for fact in cands
    ]
    results = audit_module.audit_all(entries, gen_dir, build_guest_image(xbe))
    return {
        item.va: bool(
            item.eligible
            and item.direct_sites >= 1
            and item.tail_jumps == 0
            and item.data_references == 0
            and item.unresolved == 0
        )
        for item in results
    }


# ---------------------------------------------------------------------------------------
# Prompt
# ---------------------------------------------------------------------------------------


def disassembly_text(func: Function) -> str:
    return "\n".join(f"{i.address:#010x}  {i.mnemonic:8} {i.operands}".rstrip() for i in func.insns)


def callers_of(functions: dict[int, Function], va: int, names: dict[int, str]) -> list[str]:
    found = []
    for caller_va in sorted(functions):
        for insn in functions[caller_va].insns:
            if insn.mnemonic == "call" and insn.operands.strip() == f"{va:#x}":
                found.append(names.get(caller_va, f"FUN_{caller_va:08X}"))
                break
    return found


def header_rules(game_dir: Path = GAME_DIR) -> str:
    """The registry rules, taken verbatim from the project's own source headers."""
    text = (game_dir / "game_replace.h").read_text(encoding="utf-8")
    start = text.index("/* SPDX")
    comment = text[start : text.index("*/", start) + 2]
    guest = (game_dir / "game_guest.h").read_text(encoding="utf-8")
    helpers = "\n".join(line for line in guest.splitlines() if line.startswith("static inline"))
    return (
        comment + "\n\nGuest memory helpers available (from game_guest.h, there are no 16-bit "
        "helpers: read two bytes with guest_read8, never over-read with guest_read32):\n" + helpers
    )


def extract_example(game_dir: Path, filename: str, va: int) -> str:
    """The function, its comment and its registration line from a proven source."""
    text = (game_dir / filename).read_text(encoding="utf-8")
    registration = re.search(rf"^GAME_REPLACE(?:_EXACT)?\({va:08X},.*$", text, re.MULTILINE)
    if registration is None:
        raise ValueError(f"no registration for {va:#x} in {filename}")
    begin = text.rfind(f"/* 0x{va:08X}", 0, registration.start())
    if begin < 0:
        begin = text.rfind("\n\n", 0, registration.start()) + 2
    return text[begin : registration.end()]


SYSTEM_PROMPT = (
    "You write readable C replacements for x86 (32-bit, Xbox) functions in a decompilation "
    "project. A harness runs the ORIGINAL machine code and your C on thousands of inputs and "
    "compares registers and memory writes, so exact behaviour matters: conditions, signed "
    "versus unsigned compares, operand widths (8 versus 32 bit), memory access order, and "
    "the exact addresses touched. Output strict JSON only."
)

RULES = (
    '- One self-contained translation unit starting with `#include "game_replace.h"`. '
    "Plain C11, no other includes. It must compile with -Wall -Wextra -Wpedantic -Wshadow "
    "-Wconversion -Wstrict-prototypes -Werror at -O0 and -O3.\n"
    "- All guest memory goes through guest_read32/guest_read8/guest_write32/guest_write8 "
    "with fixed guest addresses written as 0x........u constants. Never cast addresses to "
    "pointers.\n"
    "- Write one `static uint32_t NAME(...)` (or `static void`) function and ONE "
    "registration line `GAME_REPLACE(XXXXXXXX, cdecl|stdcall, N, u32|void, NAME)` with the "
    "eight UPPER CASE hex digits of the function address. N is the number of 32-bit stack "
    "arguments. Use stdcall exactly when the original ends in `ret N` (N = bytes / 4), "
    "cdecl when it ends in plain `ret`. This function has no register arguments (ecx and "
    "edx are not read before being written), so do not use thiscall or fastcall.\n"
    "- GAME_REPLACE: eax carries the result (use `u32`), or `void` if the function never "
    "sets a meaningful eax. ecx and edx are treated as scratch. Use GAME_REPLACE_EXACT "
    "only if a caller-visible register output other than eax is genuinely needed (rare, "
    "avoid).\n"
    "- Arguments arrive as the function's uint32_t parameters in stack order. Mind "
    "signedness: cast to int32_t where the original uses signed compares (jl, jg, movsx, "
    "sar, idiv).\n"
    "- Reproduce every memory read and write the original performs (including reads whose "
    "result is only used on one path, and the order of writes). Do not add reads the "
    "original does not do. Callee-saved registers (ebx, esi, edi, ebp) are restored by the "
    "original, ignore them.\n"
)


def build_prompt(
    facts: dict[str, Any],
    func: Function,
    suggested_name: str | None,
    callers: list[str],
    rules: str,
    examples: list[tuple[str, str]],
) -> str:
    naming = (
        "- Name the helper `"
        + (suggested_name or "game_<descriptive_snake_case>")
        + "`"
        + (
            " (the project's existing overlay name, use it exactly)."
            if suggested_name
            else " (choose a neutral descriptive name from observed behaviour, prefix `game_`)."
        )
        + " Put a short comment above it stating the address and what it does, in plain "
        "words, no claims about game meaning you cannot see.\n"
        "- Fill `arg_layout` with a one line description of the stack arguments and return, "
        "`notes` with any behaviour you want a reviewer to check, and `unsure` with the list "
        "of things you could not determine (empty when none)."
    )
    parts = [
        "## Project rules for a replacement file (from the repo's own headers)\n" + rules,
        "## Additional rules\n" + RULES + naming,
    ]
    for index, (disasm, source) in enumerate(examples, 1):
        parts.append(
            f"## Proven example {index}\nOriginal disassembly:\n{disasm}\n\nReplacement:\n{source}"
        )
    pop = f"ret {facts['ret_pop']}" if facts["ret_pop"] else "plain ret"
    parts.append(
        f"## Function to replace\nAddress {facts['va']:#010x}, {facts['size']} bytes, "
        f"{facts['insns']} instructions, call-free. Hint from a static scan: "
        f"{facts['stack_args_hint']} stack argument(s) ({pop}).\n"
        f"Named callers: {', '.join(callers) if callers else 'none named'}.\n"
        "Named callees: none (call-free). Overlay name: "
        f"{suggested_name or 'none yet'}.\n\nDisassembly:\n{disassembly_text(func)}"
    )
    return "\n\n".join(parts)


def repair_prompt(previous: dict[str, Any], failure: str) -> str:
    return (
        "Your previous replacement failed the gate. Fix it and answer with the same JSON "
        "shape. Re-read the disassembly: the harness result below is authoritative. A "
        "divergence example lists the case inputs (registers, stack, memory the original "
        "read) and which side wrote or returned what.\n\nPrevious c_source:\n"
        + previous["c_source"]
        + "\n\nGate output:\n"
        + failure
    )


# ---------------------------------------------------------------------------------------
# Gate
# ---------------------------------------------------------------------------------------


@dataclass
class GateResult:
    passed: bool
    stage: str  # structure compile-oN prove-oN reach-oN ok
    detail: str = ""
    category: str = ""  # compile, disagree, reach, audit, structure, ok
    metrics: dict[str, Any] = field(default_factory=dict)
    cases_per_function: int | None = None
    runs: list[dict[str, Any]] = field(default_factory=list)


def check_structure(source: str, va: int) -> str:
    """Cheap pre-checks: exactly one registration, for this address, upper case digits."""
    matches = re.findall(r"\bGAME_REPLACE(?:_EXACT)?\(\s*([0-9A-Fa-f]{8})\s*,", source)
    if len(matches) != 1:
        return f"expected exactly one GAME_REPLACE registration, found {len(matches)}"
    if int(matches[0], 16) != va or matches[0] != matches[0].upper():
        return f"registration must use the upper case digits {va:08X}"
    if '#include "game_replace.h"' not in source:
        return 'missing #include "game_replace.h"'
    return ""


def accepts(metrics: dict[str, Any]) -> list[str]:
    """The reasons a proof entry fails the acceptance gate (empty list means pass)."""
    reasons = []
    if metrics.get("disagree", 0) != 0:
        reasons.append(f"{metrics['disagree']} DISAGREE")
    if metrics.get("subject_faulted", 0) != 0:
        reasons.append("subject faults")
    if metrics.get("verdicts", 0) < MIN_VERDICTS:
        reasons.append(f"verdicts {metrics.get('verdicts', 0)} < {MIN_VERDICTS}")
    if metrics.get("coverage", 0.0) < MIN_COVERAGE:
        reasons.append(f"coverage {metrics.get('coverage', 0.0):.2f} < {MIN_COVERAGE}")
    if metrics.get("null_agree_rate", 1.0) > MAX_NULL_AGREE:
        reasons.append(f"null-agree {metrics.get('null_agree_rate', 1.0):.2f} > {MAX_NULL_AGREE}")
    if metrics.get("near_vacuous"):
        reasons.append("near-vacuous")
    if not metrics.get("replaced_confirmed", False):
        reasons.append("replacement not confirmed in the dispatch table")
    return reasons


def format_divergences(metrics: dict[str, Any] | None, output: str = "", limit: int = 5) -> str:
    """The harness' own divergence lines (stdout `DISAGREE va case N: ...`) and JSON examples."""
    lines = [line[:300] for line in output.splitlines() if line.startswith("DISAGREE")][:limit]
    examples = (metrics or {}).get("divergence_examples") or []
    return ("\n".join(lines) + "\n" + json.dumps(examples[:limit], default=str))[:6000]


def run_command(command: list[str], timeout: int) -> tuple[int, str]:
    full = [
        "taskset",
        "-c",
        "28-31",
        "nice",
        "-n",
        "19",
        "timeout",
        "--kill-after=5s",
        str(timeout),
        *command,
    ]
    completed = subprocess.run(full, capture_output=True, text=True, check=False)
    return completed.returncode, completed.stdout + completed.stderr


Runner = Callable[[list[str], int], tuple[int, str]]


def prove_once(
    game_dir: Path,
    work: Path,
    out: Path,
    va: int,
    opt: int,
    cases: int | None,
    xbe: Path,
    base_dir: Path,
    run: Runner,
) -> tuple[int, str, dict[str, Any] | None]:
    command = [
        sys.executable,
        "-m",
        "tools.replace",
        "prove",
        "--game-dir",
        str(game_dir),
        "--work-dir",
        str(work),
        "--base-dir",
        str(base_dir),
        "--out-dir",
        str(out),
        "--only-va",
        f"{va:#010x}",
        "--opt-level",
        str(opt),
        "--xbe",
        str(xbe),
        "--seed",
        str(SEED),
    ]
    if cases is not None:
        command += ["--cases-per-function", str(cases)]
    code, output = run(command, 1700)
    proof = out / "partial" / "proof.json"
    metrics = None
    if proof.is_file():  # the harness exits 1 on a DISAGREE but still writes the proof
        for entry in json.loads(proof.read_text(encoding="utf-8")).get("functions", []):
            if int(entry["va"], 16) == va:
                metrics = entry
    return code, output, metrics


def gate_draft(
    source: str,
    va: int,
    scratch: Path,
    *,
    xbe: Path = DEFAULT_XBE,
    game_dir: Path = GAME_DIR,
    run: Runner = run_command,
    ladder: tuple[int | None, ...] = CASES_LADDER,
) -> GateResult:
    """Compile and prove one draft at O0 and O3. The only function that can say `passed`."""
    problem = check_structure(source, va)
    if problem:
        return GateResult(False, "structure", problem, "structure")
    gdir = scratch / "game"
    if gdir.exists():
        shutil.rmtree(gdir)
    gdir.mkdir(parents=True)
    for name in (
        "game_replace.h",
        "game_guest.h",
        "x87_flags.h",
        "game_registry.c",
        "game_manifest.c",
    ):
        shutil.copy(game_dir / name, gdir / name)
    (gdir / f"llm_draft_{va:08X}.c").write_text(source, encoding="utf-8")
    base = scratch.parent / "stub-base"
    result = GateResult(False, "prove-o0")
    cases_used: int | None = None
    for opt in (0, 3):
        stage = f"o{opt}"
        rungs = ladder if opt == 0 else (cases_used,)
        for cases in rungs:
            out = scratch / f"out-{stage}-{cases}"
            code, output, metrics = prove_once(
                gdir, scratch / f"work-{stage}", out, va, opt, cases, xbe, base, run
            )
            record: dict[str, Any] = {"opt": opt, "cases_per_function": cases, "exit": code}
            if metrics is not None:
                record["metrics"] = {
                    key: metrics.get(key)
                    for key in (
                        "verdicts",
                        "agree",
                        "disagree",
                        "coverage",
                        "null_agree_rate",
                        "near_vacuous",
                        "subject_faulted",
                        "replaced_confirmed",
                        "cases",
                    )
                }
            result.runs.append(record)
            if metrics is None and code != 0:
                audit_bad = "caller resource audit" in output
                result.stage = f"compile-{stage}"
                result.category = "audit" if audit_bad else "compile"
                result.detail = output[-4000:]
                return result
            reasons = accepts(metrics or {})
            result.metrics[stage] = metrics or {}
            if not reasons:
                cases_used = cases
                break
            disagree = (metrics or {}).get("disagree", 0) > 0
            if disagree or opt == 3:
                result.stage = f"prove-{stage}"
                result.category = "disagree" if disagree else "reach"
                result.detail = "; ".join(reasons) + "\n" + format_divergences(metrics, output)
                return result
            result.detail = "; ".join(reasons)
        else:
            result.stage, result.category = f"reach-{stage}", "reach"
            return result
    result.passed, result.stage, result.category = True, "ok", "ok"
    result.cases_per_function = cases_used
    return result


# ---------------------------------------------------------------------------------------
# Pipeline
# ---------------------------------------------------------------------------------------


@dataclass
class Attempt:
    kind: str  # first, repair1, ...
    usd: float
    seconds: float
    stage: str
    category: str
    passed: bool
    detail: str


@dataclass
class Outcome:
    va: int
    accepted: bool
    attempts: list[Attempt]
    cases_per_function: int | None = None
    source: str = ""
    metrics: dict[str, Any] = field(default_factory=dict)
    arg_layout: str = ""
    notes: str = ""
    unsure: list[str] = field(default_factory=list)
    runs: list[dict[str, Any]] = field(default_factory=list)


Gate = Callable[[str, int, Path], GateResult]


def process_candidate(
    client: Any,
    facts: dict[str, Any],
    prompt: str,
    gate: Gate,
    accept_dir: Path,
    scratch: Path,
    max_repairs: int = 1,
    repair_effort: str | None = "medium",
) -> Outcome:
    """Draft, gate, repair. Writes into `accept_dir` only when the gate says passed."""
    va = facts["va"]
    attempts: list[Attempt] = []
    current_prompt = prompt
    answer: dict[str, Any] = {}
    base_effort = getattr(client, "effort", None)
    for index in range(max_repairs + 1):
        kind = "first" if index == 0 else f"repair{index}"
        started = time.time()
        if base_effort is not None:
            client.effort = (repair_effort or base_effort) if index else base_effort
        result = client.complete(current_prompt, SYSTEM_PROMPT, SCHEMA, "replacement_draft")
        answer = result.parsed
        verdict = gate(answer["c_source"], va, scratch / kind)
        attempts.append(
            Attempt(
                kind,
                result.usd,
                time.time() - started,
                verdict.stage,
                verdict.category,
                verdict.passed,
                verdict.detail[:1500],
            )
        )
        if verdict.passed:
            accept_dir.mkdir(parents=True, exist_ok=True)
            (accept_dir / f"{va:08X}.c").write_text(answer["c_source"], encoding="utf-8")
            if base_effort is not None:
                client.effort = base_effort
            return Outcome(
                va,
                True,
                attempts,
                verdict.cases_per_function,
                answer["c_source"],
                verdict.metrics,
                answer["arg_layout"],
                answer["notes"],
                list(answer["unsure"]),
                verdict.runs,
            )
        if verdict.category in {"reach", "audit"}:
            break  # not a logic fault the model can repair from the harness text
        current_prompt = prompt + "\n\n" + repair_prompt(answer, verdict.detail)
    if base_effort is not None:
        client.effort = base_effort
    return Outcome(va, False, attempts, source=answer.get("c_source", ""))


def make_client(args: argparse.Namespace) -> Any:
    from tools.llm.common import Cache, Ledger, read_key
    from tools.llm.openai_client import OpenAIClient

    return OpenAIClient(
        key=read_key(args.openai_key_file),
        model=args.model,
        effort=args.effort,
        max_output_tokens=args.max_output_tokens,
        max_usd=args.max_usd,
        concurrency=1,
        cache=Cache(Path(args.cache_dir), not args.no_cache),
        ledger=Ledger(Path(args.ledger)),
    )


def guest_image(xbe: str) -> Any:
    from tools.harness.image import build_guest_image

    return build_guest_image(Path(xbe))


def cmd_screen(args: argparse.Namespace) -> int:
    low, high = (int(part, 16) for part in args.band.split("-"))
    functions = load_functions(Path(args.functions), guest_image(args.xbe).code_at)
    audit = (
        None
        if args.no_audit
        else lambda cands: audit_eligibility(cands, Path(args.xbe), Path(args.gen_dir))
    )
    cands = screen(
        functions, low, high, registered_vas(Path(args.game_dir)), mentioned_in_receipts(), audit
    )
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(cands, indent=1), encoding="utf-8")
    print(f"{len(cands)} candidate(s) in {args.band} written to {args.out}")
    return 0


def spent_usd(ledger: Path) -> float:
    from tools.llm.status import summarize

    return sum(entry["usd"] for entry in summarize(ledger).values())


def global_headroom_usd() -> float:
    """Remaining room under the shared all-agent ceiling (never raise the ceiling)."""
    from tools.llm.common import global_ceiling_usd, global_spent_usd

    return global_ceiling_usd() - global_spent_usd()


def cmd_draft(args: argparse.Namespace) -> int:
    functions = load_functions(Path(args.functions), guest_image(args.xbe).code_at)
    names = load_names()
    cands = json.loads(Path(args.candidates).read_text(encoding="utf-8"))
    if args.only_va:
        wanted = {int(value, 16) for value in args.only_va}
        cands = [fact for fact in cands if fact["va"] in wanted]
    cands = cands[args.skip : args.skip + args.limit]
    client = make_client(args)
    rules = header_rules(Path(args.game_dir))
    examples = [
        (disassembly_text(functions[va]), extract_example(Path(args.game_dir), name, va))
        for va, name in FEWSHOT
    ]
    scratch_root = Path(args.scratch)
    log_path = scratch_root / "outcomes.jsonl"
    scratch_root.mkdir(parents=True, exist_ok=True)

    def gate(source: str, va: int, scratch: Path) -> GateResult:
        return gate_draft(source, va, scratch, xbe=Path(args.xbe), game_dir=Path(args.game_dir))

    for facts in cands:
        va = facts["va"]
        if spent_usd(Path(args.ledger)) >= args.stop_usd or global_headroom_usd() < 0.25:
            print("stopping: own stop line or the global ceiling headroom reached", flush=True)
            break
        prompt = build_prompt(
            facts, functions[va], names.get(va), callers_of(functions, va, names), rules, examples
        )
        started = time.time()
        outcome = process_candidate(
            client,
            facts,
            prompt,
            gate,
            Path(args.accept_dir),
            scratch_root / f"{va:08X}",
            args.max_repairs,
            args.repair_effort,
        )
        with log_path.open("a", encoding="utf-8") as handle:
            handle.write(
                json.dumps({**asdict(outcome), "wall_seconds": time.time() - started}) + "\n"
            )
        stages = [f"{a.stage}:{a.category}" for a in outcome.attempts]
        print(f"{va:#010x} accepted={outcome.accepted} attempts={stages}", flush=True)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--functions", default=str(FUNCTIONS_PATH))
    common.add_argument("--xbe", default=str(DEFAULT_XBE))
    common.add_argument("--game-dir", default=str(GAME_DIR))
    screen_p = sub.add_parser("screen", parents=[common])
    screen_p.add_argument("--band", default="0x80000-0x100000")
    screen_p.add_argument("--gen-dir", default="generated/lifted/gen")
    screen_p.add_argument("--out", default=str(SCRATCH_ROOT / "candidates.json"))
    screen_p.add_argument("--no-audit", action="store_true")
    draft = sub.add_parser("draft", parents=[common])
    draft.add_argument("--candidates", default=str(SCRATCH_ROOT / "candidates.json"))
    draft.add_argument("--openai-key-file", default=".secrets/openai-api-key.txt")
    draft.add_argument("--model", default="gpt-5.5")
    draft.add_argument(
        "--effort", default="low", choices=["none", "low", "medium", "high", "xhigh"]
    )
    draft.add_argument("--repair-effort", default="medium")
    draft.add_argument("--max-output-tokens", type=int, default=12000)
    draft.add_argument("--max-usd", type=float, default=2.0)
    draft.add_argument("--stop-usd", type=float, default=1.9)
    draft.add_argument("--max-repairs", type=int, default=1)
    draft.add_argument("--limit", type=int, default=10)
    draft.add_argument("--skip", type=int, default=0)
    draft.add_argument("--only-va", action="append", default=[])
    draft.add_argument("--accept-dir", default=str(SCRATCH_ROOT / "accepted"))
    draft.add_argument("--scratch", default=str(SCRATCH_ROOT / "runs"))
    draft.add_argument("--cache-dir", default="tmp/llm-cache")
    draft.add_argument("--ledger", default="tmp/llm-usage.jsonl")
    draft.add_argument("--no-cache", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return {"screen": cmd_screen, "draft": cmd_draft}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
