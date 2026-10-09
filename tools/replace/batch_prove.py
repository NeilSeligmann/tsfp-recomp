# SPDX-License-Identifier: GPL-3.0-or-later
"""Batch proof driver (T1560): run the per-function proof tuples of a whole list at once.

For every VA of a list (a T1534 `list-00N.json` or a pilot `list-N.txt`) the driver runs the
tuples the pilot docs prescribe, seeds 20261001 and 20261006 at -O0 and -O3 with 600 cases
(`--live-call-closure` where the JSON recipe says so), through the existing
`python -m tools.replace prove` entry point. Tuples run in parallel, each pinned with `taskset`
to its own core slot. The pilot acceptance gate is then applied to every `proof.json` (the
harness itself exits 0 even when the gate fails) and a results file plus markdown table are
written.

It does NOT write the C replacement and does not judge semantic quality, mutation QA still
applies. T1481 policy: no best-of-N, no seed search, a failed tuple stays failed. The optional
`--ladder` runs the documented cases-per-function rungs only for reach failures with 0 DISAGREE
and records every rung. A missing proof.json, a crash or a timeout is an error, never a pass.

    python -m tools.replace.batch_prove docs/data/t1534-call-draft-lists/list-001.json \
        --out-dir tmp/batch/l1 --draft src/game/game_calls_list001.c --cores 0-5 \
        --xbe build/default.xbe
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import queue
import re
import shlex
import shutil
import subprocess
import sys
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

SCHEMA = 1
SEEDS = (20261001, 20261006)
OPT_LEVELS = (0, 3)
DEFAULT_CASES = 600
LADDER_RUNGS = (16, 4096, 65536)
RESERVED_CORES = frozenset(range(24, 32))
SUPPORT_FILES = (
    "game_replace.h",
    "game_guest.h",
    "x87_flags.h",
    "game_registry.c",
    "game_manifest.c",
)
DEFAULT_XBE = Path("build/default.xbe")
DEFAULT_TIMEOUT = 1700

# The pilot gate (docs/t-cheap-model-pilot.md). Thresholds are inclusive on the passing side.
MIN_VERDICTS = 100
MIN_COVERAGE = 0.9
MAX_NULL_AGREE = 0.9
GATE_ORDER = (
    "malformed",
    "disagree",
    "subject_faults",
    "verdicts",
    "coverage",
    "null_agree",
    "near_vacuous",
    "replaced",
)
# Failures a larger or smaller case budget could change (the ladder is allowed only for these).
REACH_GATES = frozenset({"verdicts", "coverage", "null_agree", "near_vacuous"})

Runner = Callable[[list[str], int], tuple[int, str]]


@dataclass
class Gate:
    passed: bool
    failing: list[str] = field(default_factory=list)
    reasons: list[str] = field(default_factory=list)

    @property
    def first_failing(self) -> str | None:
        return self.failing[0] if self.failing else None

    @property
    def reach_failure(self) -> bool:
        return bool(self.failing) and set(self.failing) <= REACH_GATES


def _number(entry: dict[str, Any], key: str) -> float | None:
    value = entry.get(key)
    if isinstance(value, bool) or not isinstance(value, int | float) or math.isnan(value):
        return None
    return float(value)


def evaluate_gate(entry: dict[str, Any] | None) -> Gate:
    """Apply the pilot gate to one proof.json function entry. Anything unreadable fails."""
    if not isinstance(entry, dict):
        return Gate(False, ["malformed"], ["no proof entry"])
    failing: set[str] = set()
    reasons: list[str] = []

    def fail(gate: str, why: str) -> None:
        failing.add(gate)
        reasons.append(why)

    values: dict[str, float] = {}
    for key in ("disagree", "subject_faulted", "verdicts", "coverage", "null_agree_rate"):
        number = _number(entry, key)
        if number is None:
            fail("malformed", f"{key} missing or not a number")
        else:
            values[key] = number
    for key in ("near_vacuous", "replaced_confirmed"):
        if not isinstance(entry.get(key), bool):
            fail("malformed", f"{key} missing or not a bool")
    if "malformed" in failing:
        return Gate(False, ["malformed"], reasons)
    if values["disagree"] != 0:  # gate:disagree
        fail("disagree", f"{int(values['disagree'])} DISAGREE")
    if values["subject_faulted"] != 0:  # gate:subject_faults
        fail("subject_faults", f"{int(values['subject_faulted'])} subject faults")
    if values["verdicts"] < MIN_VERDICTS:  # gate:verdicts
        fail("verdicts", f"verdicts {int(values['verdicts'])} < {MIN_VERDICTS}")
    if values["coverage"] < MIN_COVERAGE:  # gate:coverage
        fail("coverage", f"coverage {values['coverage']:.4f} < {MIN_COVERAGE}")
    if values["null_agree_rate"] > MAX_NULL_AGREE:  # gate:null_agree
        fail("null_agree", f"null-agree {values['null_agree_rate']:.4f} > {MAX_NULL_AGREE}")
    if entry["near_vacuous"]:  # gate:near_vacuous
        fail("near_vacuous", "near-vacuous")
    if not entry["replaced_confirmed"]:  # gate:replaced
        fail("replaced", "replacement not confirmed in the dispatch table")
    ordered = [gate for gate in GATE_ORDER if gate in failing]
    return Gate(not ordered, ordered, reasons)


# ---------------------------------------------------------------- list input


@dataclass
class Target:
    va: int
    seeds: tuple[int, ...] = SEEDS
    opts: tuple[int, ...] = OPT_LEVELS
    cases: int = DEFAULT_CASES
    live_call_closure: bool = False
    providers: tuple[str, ...] = ()
    boundaries: tuple[str, ...] = ()
    shape: str = "-"
    live_vector_state: bool = False
    #: T1620: "legacy" (default) or "fp-scalar-v1"; only meaningful with live_vector_state
    vector_mode: str = "legacy"
    #: T1773: also run the synthesized-domain provider (evidence class kept separate)
    synth_domain: bool = False
    #: T1773: predeclared sha256 of this root's synthesized domain artifact ("" = no pin)
    synth_pin: str = ""
    synth_domain_version: str = "t1773-synth-v1"
    static_jump_tables: bool = False

    @property
    def mode(self) -> str:
        if self.live_vector_state:
            return "vector" if self.vector_mode == "legacy" else "vector-fp"
        return "live" if self.live_call_closure else "leaf"


def _shape(recipe: dict[str, Any]) -> str:
    contract = recipe.get("input_contract") or {}
    inputs = contract.get("input_abi") or "none"
    return f"{contract.get('convention', '?')} {contract.get('stack_args', '?')} in:{inputs}"


def vector_flag(recipe: dict[str, Any], vector: bool) -> bool:
    """T1595 fail-closed vector selection. `vector` is the explicit --live-vector-state option.

    Without the option a recipe asking for vector state is refused (never silently honoured or
    dropped). With it every recipe must ask for it, hold a complete live closure and name no
    opaque boundary (tools.replace prove refuses the same combinations).
    """
    value = recipe.get("live_vector_state", False)
    if not isinstance(value, bool):
        raise SystemExit("recipe live_vector_state must be a JSON boolean")
    if value and not vector:
        raise SystemExit("recipe requests live_vector_state: pass --live-vector-state")
    if not vector:
        return False
    if not value:
        raise SystemExit("--live-vector-state needs live_vector_state in every recipe")
    if recipe.get("live_call_closure") is not True or recipe.get("live_call_boundaries"):
        raise SystemExit("live_vector_state needs live_call_closure and no live_call_boundaries")
    return True


def synth_flag(recipe: dict[str, Any], synth: bool) -> bool:
    """T1773 fail-closed synth-domain selection. `synth` is the explicit --synth-domain option.

    A recipe asking for it without the option is refused. With the option a recipe lacking the
    key is allowed (the option applies to every row), but one that says `false` is refused.
    """
    value = recipe.get("synth_domain")
    if value is not None and not isinstance(value, bool):
        raise SystemExit("recipe synth_domain must be a JSON boolean")
    if value is True and not synth:
        raise SystemExit("recipe requests synth_domain: pass --synth-domain")
    if synth and value is False:
        raise SystemExit("--synth-domain conflicts with a recipe that sets synth_domain false")
    return synth


def static_table_flag(recipe: dict[str, Any], enabled: bool) -> bool:
    """Require explicit opt-in; refuse conflicting or malformed recipe declarations."""
    value = recipe.get("static_jump_tables")
    if value is not None and not isinstance(value, bool):
        raise SystemExit("recipe static_jump_tables must be a JSON boolean")
    if value is True and not enabled:
        raise SystemExit("recipe requests static_jump_tables: pass --static-jump-tables")
    if enabled and value is False:
        raise SystemExit("--static-jump-tables conflicts with recipe static_jump_tables false")
    return enabled


VECTOR_MODES = ("legacy", "fp-scalar-v1")


def declared_vector_mode(path: Path) -> str | None:
    """T1623: the vector mode a list requires, or None when it declares none.

    A `.txt` list declares it with a `# vector-mode: MODE` comment line, a `.json` list with a
    top-level `vector_mode` string. A list that declares a mode must be run with exactly that
    `--vector-mode` (see `check_declared_mode`), so a scalar-float list can never be proved by
    accident in the legacy mode. Lists without a declaration behave as before."""
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".json":
        value = json.loads(text).get("vector_mode")
        if value is None:
            return None
        if value not in VECTOR_MODES:
            raise SystemExit(f"{path}: unknown vector_mode {value!r}")
        return str(value)
    found = [
        match.group(1)
        for line in text.splitlines()
        if (match := re.fullmatch(r"#\s*vector-mode:\s*(\S+)", line.strip()))
    ]
    if not found:
        return None
    if len(found) != 1 or found[0] not in VECTOR_MODES:
        raise SystemExit(f"{path}: expected one `# vector-mode: MODE` line, got {found!r}")
    return found[0]


def check_declared_mode(path: Path, vector_mode: str, vector: bool) -> None:
    """Fail closed when a list's declared vector mode does not match the command line."""
    declared = declared_vector_mode(path)
    if declared is None:
        return
    if declared == "legacy":
        if vector_mode != "legacy":
            raise SystemExit(f"{path}: list declares vector-mode legacy: drop --vector-mode")
        return
    if not vector or vector_mode != declared:
        raise SystemExit(
            f"{path}: list declares vector-mode {declared}: "
            f"pass --live-vector-state --vector-mode {declared}"
        )


def recipe_synth_pin(recipe: dict, va: int, synth: bool) -> str:
    path = recipe.get("synth_domain_pin")
    if path is None:
        return ""
    if not synth or not isinstance(path, str):
        raise SystemExit("synth_domain_pin requires --synth-domain and an artifact path")
    from .synth_contract import artifact_pin

    try:
        return artifact_pin(Path(path), va, committed=True)
    except ValueError as error:
        raise SystemExit(str(error)) from error


def load_list(
    path: Path,
    cases: int | None,
    vector: bool = False,
    synth: bool = False,
    static_tables: bool = False,
) -> list[Target]:
    """A T1534 list JSON (per-row recipe) or a pilot .txt list (one 0x VA per line)."""
    text = path.read_text(encoding="utf-8")
    targets: list[Target] = []
    if path.suffix == ".json":
        for row in json.loads(text)["functions"]:
            recipe = row.get("recipe") or {}
            targets.append(
                Target(
                    va=int(row["va"], 16),
                    seeds=tuple(int(seed) for seed in recipe.get("seeds", SEEDS)),
                    opts=tuple(int(opt) for opt in recipe.get("optimizations", OPT_LEVELS)),
                    cases=cases or int(recipe.get("count", DEFAULT_CASES)),
                    live_call_closure=bool(recipe.get("live_call_closure", False)),
                    providers=tuple(recipe.get("fixture_providers", ())),
                    boundaries=tuple(recipe.get("live_call_boundaries", ())),
                    shape=_shape(recipe),
                    live_vector_state=vector_flag(recipe, vector),
                    synth_domain=synth_flag(recipe, synth),
                    synth_pin=recipe_synth_pin(recipe, int(row["va"], 16), synth),
                    static_jump_tables=static_table_flag(recipe, static_tables),
                )
            )
    else:
        for line in text.splitlines():
            line = line.split("#")[0].strip()
            if line:
                targets.append(
                    Target(
                        va=int(line, 16),
                        cases=cases or DEFAULT_CASES,
                        live_call_closure=vector,
                        live_vector_state=vector,
                        synth_domain=synth,
                    )
                )
    if not targets:
        raise SystemExit(f"{path}: no VAs")
    if len({target.va for target in targets}) != len(targets):
        raise SystemExit(f"{path}: duplicate VAs")
    for target in targets:
        target.static_jump_tables = static_tables
        if static_tables and (target.live_call_closure or target.live_vector_state):
            raise SystemExit("--static-jump-tables requires call-free non-vector recipes")
    return targets


# ---------------------------------------------------------------- cores


def parse_cores(spec: str) -> list[int]:
    cores: set[int] = set()
    for part in spec.split(","):
        low, _, high = part.strip().partition("-")
        cores.update(range(int(low), int(high or low) + 1))
    return sorted(cores)


def default_cores() -> str:
    """Every allowed core below 24 (cores 24-31 are the shared heavy-compute allocation)."""
    allowed = sorted(set(os.sched_getaffinity(0)) - RESERVED_CORES)
    if not allowed:
        raise SystemExit("no usable core below 24, pass --cores explicitly")
    return ",".join(str(core) for core in allowed)


def make_slots(cores: list[int], per_job: int) -> list[list[int]]:
    if per_job < 1 or len(cores) < per_job:
        raise SystemExit(f"--cores-per-job {per_job} needs at least that many --cores")
    return [cores[i : i + per_job] for i in range(0, len(cores) - per_job + 1, per_job)]


# ---------------------------------------------------------------- tuples


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_digest(game_dir: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(game_dir.glob("*")):
        if path.is_file():
            digest.update(path.name.encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


def assemble_game_dir(support_dir: Path, drafts: list[Path], dest: Path) -> None:
    """Scratch game dir: the support headers/registry plus the draft sources (as `.c`)."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    for name in SUPPORT_FILES:
        shutil.copy(support_dir / name, dest / name)
    for draft in drafts:
        name = draft.name[: -len(".txt")] if draft.name.endswith(".c.txt") else draft.name
        if not name.endswith(".c"):
            raise SystemExit(f"--draft {draft}: expected a .c or .c.txt file")
        shutil.copy(draft, dest / name)


@dataclass
class Job:
    target: Target
    seed: int
    opt: int


def tuple_tag(seed: int, opt: int, cases: int) -> str:
    return f"s{seed}-o{opt}-c{cases}"


def prove_command(
    job: Job, cases: int, paths: dict[str, Path], slot: int, xbe: Path, max_size: int = 512
) -> list[str]:
    target = job.target
    command = [
        sys.executable,
        "-m",
        "tools.replace",
        "prove",
        "--game-dir",
        str(paths["game"]),
        "--work-dir",
        str(paths["work"] / f"slot{slot}-o{job.opt}"),
        "--base-dir",
        str(paths["base"] / f"slot{slot}"),
        "--out-dir",
        str(paths["proof"]),
        "--only-va",
        f"0x{target.va:08x}",
        "--opt-level",
        str(job.opt),
        "--xbe",
        str(xbe),
        "--seed",
        str(job.seed),
        "--cases-per-function",
        str(cases),
    ]
    if max_size != 512:
        command += ["--max-size", str(max_size)]
    if target.static_jump_tables:
        command.append("--static-jump-tables")
    if target.live_call_closure:
        command.append("--live-call-closure")
    if target.live_vector_state:
        command.append("--live-vector-state")
        if target.vector_mode != "legacy":
            command += ["--vector-mode", target.vector_mode]
    if target.synth_domain:
        command.append("--synth-domain")
        if target.synth_domain_version != "t1773-synth-v1":
            command += ["--synth-domain-version", target.synth_domain_version]
        if target.synth_pin:
            command += ["--synth-domain-pin", f"{target.va:#x}={target.synth_pin}"]
    for label in target.providers:
        command += ["--fixture-provider", label]
    for boundary in target.boundaries:
        command += ["--live-call-boundary", boundary]
    return command


def wrap_command(command: list[str], cores: list[int], nice: int, timeout: int) -> list[str]:
    return [
        "taskset",
        "-c",
        ",".join(str(core) for core in cores),
        "nice",
        "-n",
        str(nice),
        "timeout",
        "--kill-after=5s",
        str(timeout),
        *command,
    ]


def run_command(command: list[str], timeout: int) -> tuple[int, str]:
    try:
        done = subprocess.run(
            command, capture_output=True, text=True, check=False, timeout=timeout + 60
        )
    except subprocess.TimeoutExpired:
        return 124, "driver timeout"
    return done.returncode, done.stdout + done.stderr


def tuple_fingerprint(command: list[str], game_digest: str, xbe: Path) -> str:
    """Identity of a tuple: the prove arguments (slot/work/base dirs removed) and the sources."""
    keep: list[str] = []
    skip = False
    for item in command:
        if skip:
            skip = False
        elif item in ("--work-dir", "--base-dir", "--game-dir"):
            skip = True
        else:
            keep.append(item)
    parts: list[Any] = [keep, game_digest, xbe.name]
    if "--vector-mode" in keep and keep[keep.index("--vector-mode") + 1 :][:1] == ["fp-scalar-v1"]:
        # T1624: an fp-scalar-v1 tuple also depends on the matrix result it relies on.
        from tools.replace.proof_contract import fp_scalar_matrix_identity

        parts.append(fp_scalar_matrix_identity(Path(__file__).resolve().parents[2])["sha256"])
    payload = json.dumps(parts, sort_keys=True)
    return hashlib.sha256(payload.encode()).hexdigest()


def read_entry(proof: Path, va: int) -> dict[str, Any] | None:
    try:
        data = json.loads(proof.read_text(encoding="utf-8"))
        for entry in data.get("functions", []):
            if int(entry["va"], 16) == va:
                return entry  # type: ignore[no-any-return]
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return None
    return None


METRIC_KEYS = (
    "verdicts",
    "agree",
    "disagree",
    "subject_faulted",
    "oracle_faulted",
    "coverage",
    "null_agree_rate",
    "near_vacuous",
    "replaced_confirmed",
    "cases",
)


def vector_observed(proof: Path, mode: str = "legacy") -> bool:
    """The proof must record an enabled vector comparison and the exact canonical state contract
    (T1595, T1611: any difference from proof_contract.contract_copy() fails closed)."""
    from tools.replace.proof_contract import (
        FP_SCALAR_MODE,
        contract_copy,
        fp_scalar_contract_copy,
    )

    try:
        closure = json.loads(proof.read_text(encoding="utf-8")).get("live_call_closure")
        comparison = closure["vector_comparison"]
        if mode == FP_SCALAR_MODE:
            return (
                comparison.get("enabled") is True
                and comparison.get("mode") == FP_SCALAR_MODE
                and closure.get("state_contract") == fp_scalar_contract_copy()
            )
        return comparison["enabled"] is True and closure.get("state_contract") == contract_copy()
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return False


def load_synth_pin(directory: Path, va: int) -> str:
    """The artifact sha256 for `va`, after re-hashing the stored document (fail closed)."""
    import hashlib

    from tools.harness.synth_domain import canonical

    path = directory / f"{va:08x}.json"
    try:
        artifact = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise SystemExit(
            f"{path}: predeclared synth-domain artifact unreadable: {error}"
        ) from error
    claimed = artifact.pop("sha256", None) if isinstance(artifact, dict) else None
    actual = hashlib.sha256(canonical(artifact)).hexdigest()
    if claimed != actual or int(str(artifact.get("va")), 16) != va:
        raise SystemExit(f"{path}: artifact hash or root does not match its content")
    return actual


def synth_block_failure(entry: dict[str, Any] | None) -> tuple[str, str] | None:
    """T1773: a --synth-domain tuple needs a self-consistent synth_domain block in its proof
    entry. Returns `(first_failing, reason)` for a missing or inconsistent block, else None."""
    from tools.harness.synth_domain import EVIDENCE_CLASS

    block = entry.get("synth_domain") if isinstance(entry, dict) else None
    if not isinstance(block, dict) or block.get("evidence_class") != EVIDENCE_CLASS:
        return "synth_domain_missing", "proof entry lacks a synthesized-domain evidence block"
    case_stream, executed = block.get("case_stream_sha256"), block.get("executed_stream_sha256")
    if (
        not isinstance(case_stream, str)
        or not case_stream
        or executed != case_stream
        or not isinstance(block.get("domain_sha256"), str)
    ):
        return (
            "synth_domain_stream_mismatch",
            "executed synth-domain case stream differs from the regenerated stream",
        )
    return None


def root_code_reader(xbe: Path, functions_csv: Path) -> Callable[[int], bytes]:
    """Return a reader of a root's exact body bytes (size from functions.csv, code from the XBE).

    An unknown root or a body outside the guest window reads as b"" (refused by the caller)."""
    import csv

    from tools.harness.image import build_guest_image

    image = build_guest_image(xbe)
    with functions_csv.open(newline="", encoding="utf-8") as handle:
        sizes = {int(row["entry_va"], 16): int(row["size_bytes"]) for row in csv.DictReader(handle)}

    def read(va: int) -> bytes:
        return image.code_at(va, sizes[va]) if va in sizes else b""

    return read


def has_vector_closure(target: Target, read: Callable[[int], bytes], mode: str) -> bool:
    """Certify an integer root's real closed callees using prove's closure predicate."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    from tools.harness.callclosure import LiveClosureError, discover_live_call_closure
    from tools.harness.callstub import _direct_target
    from tools.vector_whitelist import has_admitted_scalar_fp, has_admitted_vector

    if not target.live_call_closure or target.boundaries:
        return False
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    pending = [target.va]
    bodies: dict[int, bytes] = {}
    decoded: dict[int, list[Any]] = {}
    while pending:
        va = pending.pop()
        if va in bodies:
            continue
        code = read(va)
        insns = list(md.disasm(code, va)) if code else []
        if not insns or sum(i.size for i in insns) != len(code):
            return False
        bodies[va], decoded[va] = code, insns
        for insn in insns:
            if insn.mnemonic == "call" or (len(insns) == 1 and insn.mnemonic == "jmp"):
                dest = _direct_target(insn)
                if dest is None:
                    return False
                pending.append(dest)
    try:
        closure = discover_live_call_closure(
            target.va,
            sizes={va: len(code) for va, code in bodies.items()},
            names={},
            read_code=lambda va, size: bodies.get(va, b"")[:size],
            allow_vector=True,
            vector_mode="" if mode == "legacy" else mode,
        )
    except LiveClosureError:
        return False
    predicate = has_admitted_vector if mode == "legacy" else has_admitted_scalar_fp
    return any(predicate(decoded[node.va]) for node in closure.nodes)


def require_vector_roots(
    targets: list[Target], read: Callable[[int], bytes], mode: str = "legacy"
) -> None:
    """Require a decoded vector root or a certified live closure containing vector state."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    from tools.vector_whitelist import has_admitted_scalar_fp, has_admitted_vector

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    for target in targets:
        code = read(target.va)
        insns = list(md.disasm(code, target.va)) if code else []
        if not insns or sum(i.size for i in insns) != len(code):
            raise SystemExit(f"0x{target.va:08x}: body unreadable or does not decode exactly")
        if mode != "legacy":
            if not has_admitted_scalar_fp(insns):
                if has_vector_closure(target, read, mode):
                    continue
                raise SystemExit(
                    f"0x{target.va:08x}: --vector-mode {mode} needs a whitelisted vector op"
                )
            continue
        if not has_admitted_vector(insns):
            if has_vector_closure(target, read, mode):
                continue
            raise SystemExit(
                f"0x{target.va:08x}: --live-vector-state needs a whitelisted vector instruction"
            )


def fp_scalar_gate(proof: Path) -> list[str] | None:
    """T1620: the fp-scalar-v1 receipt block must exist and its gate must pass.

    Returns None when it passes, else the failure reasons (a missing block fails closed)."""
    try:
        block = json.loads(proof.read_text(encoding="utf-8"))["live_call_closure"]["fp_scalar"]
        gate = block["gate"]
        if gate["passed"] is True and block.get("build", {}).get("passed") is True:
            return None
        reasons = list(gate["failures"])
        if block.get("build", {}).get("passed") is not True:
            reasons.append("fp-scalar build record missing or failed")
        return reasons or ["fp_scalar gate failed"]
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return ["proof.json has no fp_scalar receipt block"]


def judge_fp_scalar(result: dict[str, Any], proof: Path, gate: Any, mode: str) -> None:
    """T1620: the fp-scalar-v1 verdict. Same order as the legacy chain: the proof must record
    the fp comparison and contract, the fp receipt gate must pass, a non-zero exit with an
    otherwise passing proof is an error, then the ordinary pilot gate decides."""
    if not vector_observed(proof, mode):
        result.update(
            status="error",
            first_failing="vector_not_enabled",
            reasons=["proof.json does not record an enabled vector comparison"],
        )
    elif (reasons := fp_scalar_gate(proof)) is not None:
        result.update(status="fail", first_failing="fp_scalar_gate", reasons=reasons)
    elif result.get("exit") not in (0, None) and gate.passed:
        result.update(
            status="error",
            first_failing="exit",
            reasons=[f"exit code {result['exit']} with an otherwise passing proof"],
        )
    else:
        result["status"] = "pass" if gate.passed else "fail"


def judge(
    result: dict[str, Any],
    proof: Path,
    va: int,
    vector: bool = False,
    mode: str = "legacy",
    synth: bool = False,
    static_tables: bool = False,
) -> dict[str, Any]:
    """Fill status, gate fields and metrics from the proof file. Missing proof is an error."""
    if not proof.is_file():
        result.update(status="error", reasons=["proof.json missing"], first_failing="no_proof")
        return result
    result["proof_sha256"] = sha256_file(proof)
    entry = read_entry(proof, va)
    gate = evaluate_gate(entry)
    if static_tables:
        try:
            block = json.loads(proof.read_text())["static_jump_tables"]
            if not isinstance(block, dict) or not isinstance(block.get("tables"), list):
                raise ValueError("missing table identities")
            tables = block["tables"]
            if not tables or any(
                not isinstance(table, dict)
                or table.get("body_va") != f"0x{va:08x}"
                or not isinstance(table.get("body_sha256"), str)
                or not re.fullmatch(r"[0-9a-f]{64}", table["body_sha256"])
                or not table.get("targets")
                or not table.get("address")
                or not table.get("site")
                for table in tables
            ):
                raise ValueError("missing table identities")
            result["static_jump_tables"] = block
        except (KeyError, ValueError, TypeError):
            result.update(
                status="error",
                first_failing="static_table_identity",
                reasons=["proof lacks static table identities"],
            )
            return result
    result["metrics"] = {k: entry.get(k) for k in METRIC_KEYS} if entry else {}
    result["reasons"] = gate.reasons
    result["first_failing"] = gate.first_failing
    result["failing"] = gate.failing
    result["reach_failure"] = gate.reach_failure
    synth_failure = synth_block_failure(entry) if synth else None
    if synth:
        block = (entry or {}).get("synth_domain")
        block = block if isinstance(block, dict) else {}
        result["synth_domain"] = True
        result["synth_domain_sha256"] = block.get("domain_sha256")
        result["synth_stream_sha256"] = block.get("case_stream_sha256")
    if entry is None:
        result.update(status="error", first_failing="no_entry")
    elif synth_failure is not None:
        result.update(status="error", first_failing=synth_failure[0], reasons=[synth_failure[1]])
    elif vector and mode != "legacy":
        judge_fp_scalar(result, proof, gate, mode)
    elif vector and not vector_observed(proof):
        result.update(
            status="error",
            first_failing="vector_not_enabled",
            reasons=["proof.json does not record an enabled vector comparison"],
        )
    elif result.get("exit") not in (0, None) and gate.passed:
        # the harness exits non-zero for a DISAGREE; a pass with a bad exit is not trusted
        result.update(
            status="error",
            first_failing="exit",
            reasons=[f"exit code {result['exit']} with an otherwise passing proof"],
        )
    else:
        result["status"] = "pass" if gate.passed else "fail"
    return result


def run_tuple(
    job: Job,
    cases: int,
    ctx: Context,
    slot: int,
    cores: list[int],
) -> dict[str, Any]:
    tag = tuple_tag(job.seed, job.opt, cases)
    base = ctx.out_dir / "runs" / f"{job.target.va:08x}" / tag
    paths = {
        "game": ctx.game_dir,
        "work": ctx.out_dir / "work",
        "base": ctx.out_dir / "base",
        "proof": base / "proof",
    }
    command = prove_command(job, cases, paths, slot, ctx.xbe, ctx.max_size)
    fingerprint = tuple_fingerprint(command, ctx.game_digest, ctx.xbe)
    proof = paths["proof"] / "partial" / "proof.json"
    result_file = base / "result.json"
    if not ctx.force:
        cached = load_cached(
            result_file,
            fingerprint,
            proof,
            job.target.va,
            job.target.live_vector_state,
            job.target.vector_mode,
            job.target.synth_domain,
            job.target.static_jump_tables,
        )
        if cached is not None:
            cached["resumed"] = True
            return cached
    if base.exists():
        shutil.rmtree(base)
    paths["proof"].mkdir(parents=True)
    wrapped = wrap_command(command, cores, ctx.nice, ctx.timeout)
    result: dict[str, Any] = {
        "schema": SCHEMA,
        "va": f"0x{job.target.va:08x}",
        "seed": job.seed,
        "opt": job.opt,
        "cases": cases,
        "live_call_closure": job.target.live_call_closure,
        "live_vector_state": job.target.live_vector_state,
        "fingerprint": fingerprint,
        "command": shlex.join(command),
        "resumed": False,
    }
    if job.target.vector_mode != "legacy":
        result["vector_mode"] = job.target.vector_mode
    if job.target.synth_domain:
        result["synth_domain"] = True
    code, output = ctx.runner(wrapped, ctx.timeout)
    result["exit"] = code
    (base / "output.txt").write_text(output[-20000:], encoding="utf-8")
    judge(
        result,
        proof,
        job.target.va,
        job.target.live_vector_state,
        job.target.vector_mode,
        job.target.synth_domain,
        job.target.static_jump_tables,
    )
    result_file.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def load_cached(
    result_file: Path,
    fingerprint: str,
    proof: Path,
    va: int,
    vector: bool = False,
    mode: str = "legacy",
    synth: bool = False,
    static_tables: bool = False,
) -> dict[str, Any] | None:
    """A stored result counts only if it matches this tuple, its proof is intact and it is
    re-judged by the CURRENT gate (a stored status is never trusted)."""
    try:
        cached = json.loads(result_file.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(cached, dict) or cached.get("schema") != SCHEMA:
        return None
    if cached.get("fingerprint") != fingerprint or not proof.is_file():
        return None
    if cached.get("proof_sha256") != sha256_file(proof):
        return None
    rejudged = judge(dict(cached), proof, va, vector, mode, synth, static_tables)
    return rejudged if rejudged["status"] in ("pass", "fail") else None


@dataclass
class Context:
    out_dir: Path
    game_dir: Path
    game_digest: str
    xbe: Path
    runner: Runner
    nice: int
    timeout: int
    force: bool
    ladder: bool
    max_size: int = 512


def run_job(job: Job, ctx: Context, slot: int, cores: list[int]) -> list[dict[str, Any]]:
    """The base tuple, then (--ladder) the documented rungs for a reach failure only."""
    results = [run_tuple(job, job.target.cases, ctx, slot, cores)]
    if ctx.ladder:
        for rung in LADDER_RUNGS:
            last = results[-1]
            if last["status"] != "fail" or not last.get("reach_failure"):
                break
            if rung == job.target.cases:
                continue
            results.append(run_tuple(job, rung, ctx, slot, cores))
    for rung_result in results:
        rung_result["ladder_rung"] = rung_result["cases"] != job.target.cases
    return results


def summarize(target: Target, runs: list[dict[str, Any]]) -> dict[str, Any]:
    """Per function: the final result of each (seed, opt) job. Passes only if ALL pass."""
    finals: dict[tuple[int, int], dict[str, Any]] = {}
    for run in runs:
        finals[(run["seed"], run["opt"])] = run  # later rungs override earlier ones
    expected = [(seed, opt) for seed in target.seeds for opt in target.opts]
    outcome = "ADMITTED"
    first = None
    for key in expected:
        final = finals.get(key)
        if final is None or final["status"] == "error":
            outcome = "ERROR"
        elif final["status"] == "fail" and outcome != "ERROR":
            outcome = "REJECTED"
        if first is None and (final is None or final["status"] != "pass"):
            first = (final or {}).get("first_failing") or "missing"
    return {
        "va": f"0x{target.va:08x}",
        "shape": target.shape,
        "mode": target.mode,
        "outcome": outcome,
        "first_failing": first,
    }


def render_markdown(title: str, functions: list[dict[str, Any]], runs: list[dict[str, Any]]) -> str:
    def cell(metrics: dict[str, Any], key: str, fmt: str) -> str:
        value = metrics.get(key)
        return format(value, fmt) if isinstance(value, int | float) else "?"

    lines = [
        f"# Batch proof results: {title}",
        "",
        "| VA | shape | mode | outcome | first failing gate |",
        "| --- | --- | --- | --- | --- |",
    ]
    for row in functions:
        lines.append(
            f"| {row['va']} | {row['shape']} | {row['mode']} | {row['outcome']} "
            f"| {row['first_failing'] or '-'} |"
        )
    synth = any(run.get("synth_domain") for run in runs)
    lines += [
        "",
        "| VA | seed | opt | cases | verdicts | coverage | null-agree | DISAGREE | status "
        "| first failing gate |" + (" synth-domain (synthesized-domain) |" if synth else ""),
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"
        + (" --- |" if synth else ""),
    ]
    for run in runs:
        metrics = run.get("metrics") or {}
        marker = ""
        if synth:
            digest = run.get("synth_domain_sha256")
            marker = (
                f" {str(digest)[:12] if digest else ('yes' if run.get('synth_domain') else '-')} |"
            )
        lines.append(
            f"| {run['va']} | {run['seed']} | O{run['opt']} | {run['cases']} "
            f"| {cell(metrics, 'verdicts', 'd')} | {cell(metrics, 'coverage', '.3f')} "
            f"| {cell(metrics, 'null_agree_rate', '.3f')} | {cell(metrics, 'disagree', 'd')} "
            f"| {run['status'].upper()} | {run.get('first_failing') or '-'} |{marker}"
        )
    return "\n".join(lines) + "\n"


def execute(
    targets: list[Target], ctx: Context, slots: list[list[int]], workers: int
) -> list[dict[str, Any]]:
    jobs = [Job(t, seed, opt) for t in targets for seed in t.seeds for opt in t.opts]
    free: queue.Queue[int] = queue.Queue()
    for index in range(len(slots)):
        free.put(index)

    def work(job: Job) -> list[dict[str, Any]]:
        slot = free.get()
        try:
            return run_job(job, ctx, slot, slots[slot])
        except Exception as error:  # fail closed: a driver crash is an error result
            return [
                {
                    "schema": SCHEMA,
                    "va": f"0x{job.target.va:08x}",
                    "seed": job.seed,
                    "opt": job.opt,
                    "cases": job.target.cases,
                    "status": "error",
                    "first_failing": "driver_exception",
                    "reasons": [repr(error)],
                    "metrics": {},
                    "ladder_rung": False,
                    **({"synth_domain": True} if job.target.synth_domain else {}),
                }
            ]
        finally:
            free.put(slot)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        nested = list(pool.map(work, jobs))
    return [run for group in nested for run in group]


def dry_run(targets: list[Target], ctx: Context, slots: list[list[int]]) -> None:
    paths = {
        "game": ctx.game_dir,
        "work": ctx.out_dir / "work",
        "base": ctx.out_dir / "base",
    }
    index = 0
    for target in targets:
        for seed in target.seeds:
            for opt in target.opts:
                job = Job(target, seed, opt)
                slot = index % len(slots)
                index += 1
                proof_dir = ctx.out_dir / "runs" / f"{target.va:08x}"
                run_paths = {
                    **paths,
                    "proof": proof_dir / tuple_tag(seed, opt, target.cases) / "proof",
                }
                command = prove_command(job, target.cases, run_paths, slot, ctx.xbe, ctx.max_size)
                print(shlex.join(wrap_command(command, slots[slot], ctx.nice, ctx.timeout)))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("list_file", type=Path, help="list-00N.json (T1534) or list-N.txt (pilot)")
    parser.add_argument("--out-dir", type=Path, required=True, help="results and scratch root")
    parser.add_argument(
        "--draft",
        type=Path,
        action="append",
        default=[],
        help="draft C source(s) to prove (.c or .c.txt), copied into the scratch game dir",
    )
    parser.add_argument("--support-dir", type=Path, default=Path("src/game"))
    parser.add_argument("--xbe", type=Path, default=DEFAULT_XBE)
    parser.add_argument(
        "--max-size",
        type=int,
        default=512,
        help="forward an explicit harness selection size ceiling; default stays 512",
    )
    parser.add_argument("--cores", default=None, help="e.g. 0-5 (default: allowed cores below 24)")
    parser.add_argument("--cores-per-job", type=int, default=1)
    parser.add_argument("--cases", type=int, default=None, help="override the recipe cases")
    parser.add_argument("--only-va", action="append", default=[], metavar="HEX")
    parser.add_argument("--ladder", action="store_true", help="documented rungs 16/4096/65536")
    parser.add_argument("--force", action="store_true", help="re-run tuples with a valid result")
    parser.add_argument("--dry-run", action="store_true", help="print the commands only")
    parser.add_argument("--nice", type=int, default=10)
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT, help="seconds per tuple")
    parser.add_argument(
        "--live-vector-state",
        action="store_true",
        help="T1595: prove with raw XMM/MXCSR observation (recipes must set live_vector_state)",
    )
    parser.add_argument(
        "--vector-mode",
        choices=("legacy", "fp-scalar-v1"),
        default="legacy",
        help="T1620: fp-scalar-v1 admits scalar float arithmetic (needs --live-vector-state)",
    )
    parser.add_argument(
        "--synth-domain",
        action="store_true",
        help="T1773: add the synthesized-domain evidence class to every tuple (recipes may set "
        "synth_domain true; the pilot gate is unchanged)",
    )
    parser.add_argument(
        "--synth-domain-version",
        choices=("t1773-synth-v1", "t1773-synth-v2"),
        default="t1773-synth-v1",
    )
    parser.add_argument(
        "--static-jump-tables",
        action="store_true",
        help="explicit bounded original static-table proof; receipts retain identities",
    )
    parser.add_argument(
        "--synth-domain-pins",
        type=Path,
        default=None,
        help="T1773: directory of predeclared domain artifacts (`python -m "
        "tools.replace.synth_measure derive`); every synth root is pinned to its artifact sha256 "
        "and a missing or tampered artifact is refused",
    )
    parser.add_argument(
        "--x87-contract",
        choices=("inherited-limited-x87-v1", "model-arbitrated-x87-v2"),
        default="inherited-limited-x87-v1",
        help="T1510: opt-in model-arbitrated-x87-v2 (needs --x87-arbiter-ack; refuses until the "
        "x87 replacement ABI exists). Default is the unchanged v1 contract",
    )
    parser.add_argument(
        "--x87-arbiter-ack",
        default=None,
        help="T1510: fingerprint of the x87 arbiter files; required by v2 and invalid with v1",
    )
    parser.add_argument(
        "--codewrite-contract",
        choices=("unicorn-per-case-reset-v1", "faithful-codewrite-arbiter-v2"),
        default="unicorn-per-case-reset-v1",
        help="T1508: opt-in faithful-codewrite-arbiter-v2 (needs --codewrite-arbiter-ack). "
        "Validated here only: the v2 proof of a code-write root runs through tools.replace prove",
    )
    parser.add_argument(
        "--codewrite-arbiter-ack",
        default=None,
        help="T1508: fingerprint of the code-write arbiter files; required by v2, invalid with v1",
    )
    parser.add_argument(
        "--functions",
        type=Path,
        default=Path("generated/retail/functions.csv"),
        help="functions.csv giving each root's size for the --live-vector-state body check",
    )
    parser.add_argument(
        "--allow-reserved-cores", action="store_true", help="permit cores 24-31 in --cores"
    )
    return parser


def main(
    argv: list[str] | None = None,
    runner: Runner = run_command,
    root_code: Callable[[int], bytes] | None = None,
) -> int:
    args = build_parser().parse_args(argv)
    if args.max_size < 1:
        raise SystemExit("--max-size must be positive")
    from tools.replace.x87_arbiter_contract import select as select_x87_contract

    try:
        select_x87_contract(args.x87_contract, args.x87_arbiter_ack)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    from tools.replace.codewrite_arbiter_contract import select as select_codewrite_contract

    try:
        select_codewrite_contract(args.codewrite_contract, args.codewrite_arbiter_ack)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    check_declared_mode(args.list_file, args.vector_mode, args.live_vector_state)
    if args.vector_mode != "legacy" and not args.live_vector_state:
        raise SystemExit("--vector-mode fp-scalar-v1 needs --live-vector-state")
    targets = load_list(
        args.list_file,
        args.cases,
        args.live_vector_state,
        args.synth_domain,
        args.static_jump_tables,
    )
    for target in targets:
        target.vector_mode = args.vector_mode
        target.synth_domain_version = args.synth_domain_version
    if args.synth_domain_pins is not None:
        if not args.synth_domain:
            raise SystemExit("--synth-domain-pins needs --synth-domain")
        for target in targets:
            pin = load_synth_pin(args.synth_domain_pins, target.va)
            if target.synth_pin and target.synth_pin != pin:
                raise SystemExit("recipe and --synth-domain-pins disagree")
            target.synth_pin = pin
    if args.only_va:
        wanted = {int(item, 16) for item in args.only_va}
        targets = [target for target in targets if target.va in wanted]
        if {t.va for t in targets} != wanted:
            raise SystemExit("--only-va names a VA that is not in the list")
    cores = parse_cores(args.cores or default_cores())
    if RESERVED_CORES & set(cores) and not args.allow_reserved_cores:
        raise SystemExit("--cores touches the reserved cores 24-31 (--allow-reserved-cores)")
    slots = make_slots(cores, args.cores_per_job)
    out_dir: Path = args.out_dir
    game_dir = out_dir / "game"
    ctx = Context(
        out_dir,
        game_dir,
        "",
        args.xbe,
        runner,
        args.nice,
        args.timeout,
        args.force,
        args.ladder,
        args.max_size,
    )
    if args.dry_run:
        dry_run(targets, ctx, slots)
        return 0
    for needed in (args.xbe, *args.draft):
        if not needed.is_file():
            raise SystemExit(f"missing input file: {needed}")
    if args.live_vector_state:
        require_vector_roots(
            targets, root_code or root_code_reader(args.xbe, args.functions), args.vector_mode
        )
    out_dir.mkdir(parents=True, exist_ok=True)
    assemble_game_dir(args.support_dir, args.draft, game_dir)
    ctx.game_digest = tree_digest(game_dir)
    runs = execute(targets, ctx, slots, len(slots))
    by_va: dict[str, list[dict[str, Any]]] = {}
    for run in runs:
        by_va.setdefault(run["va"], []).append(run)
    functions = [summarize(t, by_va.get(f"0x{t.va:08x}", [])) for t in targets]
    results = {
        "schema": SCHEMA,
        "list": str(args.list_file),
        "game_digest": ctx.game_digest,
        "gate": {
            "min_verdicts": MIN_VERDICTS,
            "min_coverage": MIN_COVERAGE,
            "max_null_agree": MAX_NULL_AGREE,
        },
        "ladder": args.ladder,
        "functions": functions,
        "tuples": runs,
    }
    (out_dir / "results.json").write_text(
        json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (out_dir / "results.md").write_text(
        render_markdown(args.list_file.name, functions, runs), encoding="utf-8"
    )
    counts = {
        name: sum(1 for f in functions if f["outcome"] == name)
        for name in ("ADMITTED", "REJECTED", "ERROR")
    }
    print(json.dumps(counts), f"-> {out_dir / 'results.md'}")
    if counts["ERROR"]:
        return 2
    return 1 if counts["REJECTED"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
