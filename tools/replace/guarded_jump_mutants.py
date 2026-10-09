# SPDX-License-Identifier: GPL-3.0-or-later
"""T1576 runtime mutants of a guarded-jump draft, run THROUGH the prove pipeline.

Four named families, each applied to the draft's own entry function (one site at a time, the
original untouched): one arm, default arm, tail target, index bound. A mutant is KILLED only by a
named gate failing in a real `tools.replace prove` run of the mutated draft (the harness exit
status is non-zero because of DISAGREE or a guarded-jump session failure, or the guard refuses
the draft). A mutant that does not compile is NOT-A-MUTANT, a run that did not produce a proof
is an ERROR, a run that passes every gate SURVIVED (never silently counted as a kill).

    python -m tools.replace.guarded_jump_mutants --game-dir DIR --draft FILE --only-va 0x... \
        --out OUT.json -- <prove arguments after `prove`>
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from tools.replace import arm_witness as aw
from tools.replace.guarded_jump_contract import select_root

KILLED, SURVIVED, ERROR, NOT_A_MUTANT = "killed", "survived", "error", "not-a-mutant"
CASE_LABEL = re.compile(r"\bcase\s+(0[xX][0-9A-Fa-f]+|\d+)\s*:")
DEFAULT_LABEL = re.compile(r"\bdefault\s*:")


@dataclass(frozen=True)
class Mutant:
    name: str
    family: str
    source: str
    note: str


def _span(source: str, function: str) -> tuple[int, int]:
    text = aw._function_text(source, function)
    start = source.index(text)
    return start, start + len(text)


def _swap(source: str, function: str, old: re.Pattern[str] | str, new: str) -> str | None:
    start, end = _span(source, function)
    body = source[start:end]
    pattern = old if isinstance(old, re.Pattern) else re.compile(re.escape(old))
    mutated, count = pattern.subn(new, body, count=1)
    return source[:start] + mutated + source[end:] if count else None


def generate(
    source: str,
    function: str,
    *,
    bound: int | None,
    tail_targets: tuple[int, ...] = (),
) -> list[Mutant]:
    """Single-site mutants of the entry function. Families that do not apply are omitted."""
    out: list[Mutant] = []

    def add(name: str, family: str, mutated: str | None, note: str) -> None:
        if mutated is not None and mutated != source:
            out.append(Mutant(name, family, mutated, note))

    start, end = _span(source, function)
    body = source[start:end]
    labels = list(CASE_LABEL.finditer(body))
    if labels:
        first = labels[0]
        value = int(first.group(1), 0)
        add(
            "one-arm-wrong-value",
            "one-arm",
            source[: start + first.start()]
            + f"case {value + 100}:"
            + source[start + first.end() :],
            "first case label renamed outside the table: its slot falls to the default arm",
        )
    if DEFAULT_LABEL.search(body):
        add(
            "default-label-removed",
            "default-arm",
            _swap(source, function, DEFAULT_LABEL, ""),
            "no default label: the witness refuses the draft",
        )
    sentinel = re.search(r"0[xX][Ff]{8}[uU]?", body)
    if sentinel is not None:
        add(
            "default-selector-to-slot0",
            "default-arm",
            _swap(source, function, re.compile(re.escape(sentinel.group(0))), "0u"),
            "an out-of-range index now selects slot 0 instead of the default arm",
        )
    if bound is not None:
        for delta, label in ((-1, "bound-minus-one"), (1, "bound-plus-one")):
            pattern = re.compile(rf"(?:0[xX]0*{bound:X}|\b{bound})([uU]?)\b", re.IGNORECASE)
            replacement = f"0x{bound + delta:X}\\1"
            add(
                f"index-{label}",
                "index-bound",
                _swap(source, function, pattern, replacement),
                f"the bound literal 0x{bound:X} becomes 0x{bound + delta:X}",
            )
    for target in tail_targets:
        pattern = re.compile(rf"0[xX]0*{target:X}[uU]?\b", re.IGNORECASE)
        add(
            f"tail-target-{target:08X}-plus-one",
            "tail-target",
            _swap(source, function, pattern, f"0x{target + 1:08X}u"),
            "the typed tail adapter calls a different (interior) address",
        )
    if tail_targets:
        add(
            "tail-argument-dropped",
            "tail-target",
            _swap(source, function, re.compile(r"guest_write32\(g_esp \+ 4u,[^;]*;"), "(void)0;"),
            "the replacement no longer forwards the rewritten argument slot to the tail target",
        )
    return out


def root_of(prove_args: list[str]) -> int:
    """The root VA named by `--only-va` in the prove arguments (fail closed when absent)."""
    values = [prove_args[i + 1] for i, a in enumerate(prove_args[:-1]) if a == "--only-va"]
    if len(values) != 1:
        raise SystemExit("prove arguments must name exactly one --only-va root")
    return int(values[0], 0)


def classify(
    returncode: int, proof: dict | None, log_tail: str, root_va: int | str
) -> tuple[str, str]:
    """(status, gate) for one mutant run."""
    if "error:" in log_tail and proof is None and "compil" in log_tail.lower():
        return NOT_A_MUTANT, "build"
    if proof is None:
        return (KILLED, "guard-refusal") if "ABORT" in log_tail else (ERROR, "no-proof")
    try:
        fn = select_root(proof, root_va)
    except ValueError:
        return ERROR, "root-not-in-proof"
    witness = proof.get("arm_witness") or {}
    if fn["disagree"] or fn["subject_faulted"]:
        return KILLED, "harness-disagree"
    if witness.get("failure_count"):
        kinds = sorted({failure["kind"] for failure in witness["failures"]})
        return KILLED, "guard:" + ",".join(kinds)
    return (SURVIVED, "all-gates-passed") if returncode == 0 else (KILLED, "other-gate")


def run_mutant(
    mutant: Mutant,
    *,
    base_game_dir: Path,
    draft_name: str,
    scratch: Path,
    prove_args: list[str],
    timeout: int,
    entry: tuple[str, ...] = ("-m", "tools.replace"),
) -> dict[str, object]:
    game = scratch / mutant.name / "game"
    shutil.rmtree(scratch / mutant.name, ignore_errors=True)
    shutil.copytree(base_game_dir, game)
    (game / draft_name).write_text(mutant.source, encoding="utf-8")
    out_dir = scratch / mutant.name / "out"
    command = [
        sys.executable,
        *entry,
        "prove",
        *prove_args,
        "--game-dir",
        str(game),
        "--work-dir",
        str(scratch / mutant.name / "work"),
        "--out-dir",
        str(out_dir),
    ]
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"name": mutant.name, "status": ERROR, "gate": "timeout"}
    proof_path = next(out_dir.rglob("proof.json"), None)
    proof = json.loads(proof_path.read_text(encoding="utf-8")) if proof_path else None
    status, gate = classify(
        done.returncode, proof, (done.stderr + done.stdout)[-4000:], root_of(prove_args)
    )
    return {
        "name": mutant.name,
        "family": mutant.family,
        "note": mutant.note,
        "status": status,
        "gate": gate,
        "returncode": done.returncode,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--draft", type=str, required=True, help="draft file name in --game-dir")
    parser.add_argument("--function", required=True, help="registered function name (FN)")
    parser.add_argument("--bound", type=lambda v: int(v, 0), default=None)
    parser.add_argument("--tail-target", type=lambda v: int(v, 0), action="append", default=[])
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=1700)
    parser.add_argument("--list", action="store_true", help="list the mutants and stop")
    parser.add_argument(
        "--entry",
        nargs="+",
        default=["-m", "tools.replace"],
        help="python arguments that run the prove CLI (default: -m tools.replace)",
    )
    parser.add_argument("prove_args", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    prove_args = [a for a in args.prove_args if a != "--"]
    source = (args.game_dir / args.draft).read_text(encoding="utf-8")
    mutants = generate(
        source, args.function, bound=args.bound, tail_targets=tuple(args.tail_target)
    )
    if args.list:
        for mutant in mutants:
            print(f"{mutant.family:12} {mutant.name}: {mutant.note}")
        return 0
    results = [
        run_mutant(
            m,
            base_game_dir=args.game_dir,
            draft_name=args.draft,
            scratch=args.scratch,
            prove_args=prove_args,
            timeout=args.timeout,
            entry=tuple(args.entry),
        )
        for m in mutants
    ]
    summary = {
        status: sum(r["status"] == status for r in results)
        for status in (KILLED, SURVIVED, ERROR, NOT_A_MUTANT)
    }
    document = {"schema": 1, "mutants": results, "summary": summary}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary))
    return 0 if not summary[SURVIVED] and not summary[ERROR] else 1


if __name__ == "__main__":
    raise SystemExit(main())
