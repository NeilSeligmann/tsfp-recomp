# SPDX-License-Identifier: GPL-3.0-or-later
"""T1773: derive, verify and measure synthesized input domains (original-only baseline).

Subcommands (all argparse, repo-relative paths):

* `split`    recompute the frozen development/held-out split from the census CSV.
* `derive`   write one hashed domain artifact per root (`<va:08x>.json`).
* `verify`   re-derive from the original bytes and compare with the artifacts on disk.
* `measure`  run the ORIGINAL code only (Unicorn oracle), judge each case with the real
             `ReplacementJudge` against an identity subject (the oracle result itself), apply
             the pilot REACH gates unchanged. Nothing is a proof: no replacement exists.

The identity subject can only turn a clean original run into AGREE, so `verdicts` here is the
number of cases the original ran to completion, an upper bound on what a real replacement run
would judge (a real subject fault or DISAGREE would still be a verdict, never fewer).
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import sys
from collections.abc import Sequence
from dataclasses import replace
from pathlib import Path
from typing import Any

from tools.harness import synth_domain
from tools.harness.image import GuestImage, build_guest_image
from tools.harness.model import Outcome
from tools.harness.oracle import UnicornOracle
from tools.harness.replacement import ReplacementJudge
from tools.harness.results import FunctionReach
from tools.harness.seeding import SeedPolicy, make_case
from tools.replace.batch_prove import evaluate_gate
from tools.replace.manifest import ManifestEntry

DEFAULT_SEEDS = (20261001, 20261006)
HELDOUT = 40
SPLIT_RULE = "t1773-heldout-v1"
SPLIT_FILE = Path("docs/data/t1773-provider-synthesis/sample-split.json")


def split_vas(census_csv: Path) -> tuple[list[int], list[int]]:
    rows = [
        r
        for r in csv.DictReader(census_csv.open(encoding="utf-8"))
        if r["baseline"] == "fault-only" and r["tier"]
    ]
    vas = sorted(int(r["va"], 16) for r in rows)
    order = sorted(
        vas, key=lambda va: hashlib.sha256(f"{SPLIT_RULE}:{va:08x}".encode()).hexdigest()
    )
    return sorted(order[:HELDOUT]), sorted(order[HELDOUT:])


def load_sample(name: str) -> list[int]:
    document = json.loads(SPLIT_FILE.read_text(encoding="utf-8"))
    if name == "all":
        return sorted(int(v, 16) for v in document["heldout"] + document["development"])
    return [int(v, 16) for v in document[name]]


def function_sizes(census_csv: Path) -> dict[int, int]:
    return {
        int(r["va"], 16): int(r["size"]) for r in csv.DictReader(census_csv.open(encoding="utf-8"))
    }


def trailing_ret_pop(code: bytes) -> int:
    """Callee-popped bytes: the immediate of the last `ret imm16` (0xC2), else 0 (cdecl)."""
    import capstone

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    pop = 0
    for insn in md.disasm(code, 0):
        if insn.mnemonic in ("ret", "retn") and insn.operands:
            pop = int(insn.operands[0].imm)
    return pop


def measure_root(
    oracle: UnicornOracle,
    image: GuestImage,
    va: int,
    size: int,
    seed: int,
    *,
    synth: synth_domain.Domain | None,
    random_cases: int,
    max_insns_note: int,
) -> dict[str, Any]:
    code = image.code_at(va, size)
    pop = trailing_ret_pop(code)
    entry = ManifestEntry(
        va=va,
        name=f"sub_{va:08x}",
        convention="stdcall" if pop else "cdecl",
        stack_args=pop // 4,
        returns="u32",
        scratch=(),
        source="t1773-measure",
    )
    judge = ReplacementJudge([entry])
    summary_body = len(list(_decode(code, va)))
    reach = FunctionReach(body_insns=summary_body)
    faults: dict[str, int] = {}
    policy = SeedPolicy()

    def run(case: Any, kind: str) -> None:
        result = oracle.run(case)
        subject = result if result.faulted else replace(result, replaced=True)
        verdict = judge.judge(case, result, subject, body_insns=summary_body, kind=kind)
        reach.covered |= verdict.reach.covered_vas
        reach.insn_counts.append(verdict.reach.insns)
        if verdict.outcome in (Outcome.AGREE, Outcome.DISAGREE, Outcome.SUBJECT_FAULTED):
            reach.verdicts += 1
        if result.fault:
            faults[result.fault] = faults.get(result.fault, 0) + 1

    for k in range(random_cases):
        run(make_case(seed, k, va, size, policy=policy), "random")
    synth_cases = 0
    if synth is not None:
        for ordinal in range(synth.case_count()):
            run(synth.make_case(seed, ordinal, policy=policy), synth_domain.LABEL)
            synth_cases += 1
    evidence = judge.evidence[va]
    row = {
        "cases": evidence.cases,
        "random_cases": evidence.by_kind.get("random", 0),
        "synth_cases": synth_cases,
        "verdicts": evidence.verdicts,
        "synth_verdicts": evidence.verdicts_by_kind.get(synth_domain.LABEL, 0),
        "random_verdicts": evidence.verdicts_by_kind.get("random", 0),
        "agree": evidence.agree,
        "disagree": evidence.disagree,
        "subject_faulted": evidence.subject_faulted,
        "oracle_faulted": evidence.oracle_faulted,
        "coverage": round(reach.coverage, 4),
        "body_insns": summary_body,
        "near_vacuous": reach.near_vacuous,
        "null_agree_rate": round(evidence.null_agree_rate, 4),
        "replaced_confirmed": True,
        "faults": dict(sorted(faults.items())),
    }
    gate = evaluate_gate(row)
    row["reach_gate_pass"] = gate.passed
    row["failing"] = gate.failing
    _ = max_insns_note
    return row


def _decode(code: bytes, va: int) -> Any:
    import capstone

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    return md.disasm(code, va)


def analysis_args(args: argparse.Namespace) -> dict[str, Any]:
    return synth_domain.load_analysis_table(args.analysis_table) if args.analysis_table else {}


def cmd_split(args: argparse.Namespace) -> int:
    held, dev = split_vas(args.census_csv)
    document = json.loads(SPLIT_FILE.read_text(encoding="utf-8"))
    same = held == [int(v, 16) for v in document["heldout"]] and dev == [
        int(v, 16) for v in document["development"]
    ]
    print(f"heldout={len(held)} development={len(dev)} matches-frozen-file={same}")
    return 0 if same else 1


def cmd_derive(args: argparse.Namespace) -> int:
    image = build_guest_image(args.xbe)
    sizes = function_sizes(args.census_csv)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    vas = [int(v, 16) for v in args.va] if args.va else load_sample(args.sample)
    for va in vas:
        domain = synth_domain.derive(
            va, sizes[va], image, generator_version=args.generator_version, **analysis_args(args)
        )
        path = args.out_dir / f"{va:08x}.json"
        path.write_text(
            json.dumps(domain.artifact(), indent=1, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(f"{va:#010x} {domain.sha256}")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    image = build_guest_image(args.xbe)
    bad = 0
    for path in sorted(args.out_dir.glob("*.json")):
        stored = json.loads(path.read_text(encoding="utf-8"))
        va, size = int(stored["va"], 16), stored["size"]
        fresh = synth_domain.derive(
            va, size, image, generator_version=stored["generator_version"], **analysis_args(args)
        )
        ok = fresh.sha256 == stored["sha256"]
        bad += not ok
        print(f"{va:#010x} {'OK' if ok else 'MISMATCH'} {fresh.sha256}")
    return 1 if bad else 0


def cmd_measure(args: argparse.Namespace) -> int:
    image = build_guest_image(args.xbe)
    sizes = function_sizes(args.census_csv)
    oracle = UnicornOracle(image.data, max_insns=args.max_insns)
    vas = [int(v, 16) for v in args.va] if args.va else load_sample(args.sample)
    results: dict[str, Any] = {}
    for va in vas:
        domain = synth_domain.derive(
            va, sizes[va], image, generator_version=args.generator_version, **analysis_args(args)
        )
        per_seed: dict[str, Any] = {}
        for seed in args.seed:
            default = measure_root(
                oracle,
                image,
                va,
                sizes[va],
                seed,
                synth=None,
                random_cases=args.random_cases,
                max_insns_note=args.max_insns,
            )
            synthesized = measure_root(
                oracle,
                image,
                va,
                sizes[va],
                seed,
                synth=domain,
                random_cases=args.random_cases,
                max_insns_note=args.max_insns,
            )
            per_seed[str(seed)] = {"default": default, "synth": synthesized}
        results[f"{va:#010x}"] = {"domain_sha256": domain.sha256, "seeds": per_seed}
        row = per_seed[str(args.seed[0])]
        base, synth = row["default"], row["synth"]
        verdict = "PASS" if synth["reach_gate_pass"] else synth["failing"]
        print(
            f"{va:#010x} default v={base['verdicts']:>4} cov={base['coverage']:.2f}"
            f" | synth v={synth['verdicts']:>4} cov={synth['coverage']:.2f}"
            f" null={synth['null_agree_rate']:.2f} gate={verdict}",
            flush=True,
        )
    summary = summarize(results, args.seed)
    document = {
        "generator_version": args.generator_version,
        "xbe": str(args.xbe),
        "sample": args.sample if not args.va else "explicit",
        "seeds": list(args.seed),
        "random_cases": args.random_cases,
        "max_insns": args.max_insns,
        "summary": summary,
        "results": results,
    }
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(document, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=1, sort_keys=True))
    return 0


def summarize(results: dict[str, Any], seeds: Sequence[int]) -> dict[str, Any]:
    out: dict[str, Any] = {"roots": len(results)}
    for variant in ("default", "synth"):
        for seed in seeds:
            rows = [r["seeds"][str(seed)][variant] for r in results.values()]
            out[f"{variant}_seed{seed}_reach_gate_pass"] = sum(
                1 for r in rows if r["reach_gate_pass"]
            )
            out[f"{variant}_seed{seed}_verdicts_ge_100"] = sum(
                1 for r in rows if r["verdicts"] >= 100
            )
            out[f"{variant}_seed{seed}_any_verdict"] = sum(1 for r in rows if r["verdicts"] > 0)
            out[f"{variant}_seed{seed}_coverage_ge_0.9"] = sum(
                1 for r in rows if r["coverage"] >= 0.9
            )
        both = [
            all(r["seeds"][str(s)][variant]["reach_gate_pass"] for s in seeds)
            for r in results.values()
        ]
        out[f"{variant}_all_seeds_reach_gate_pass"] = sum(both)
    return out


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument(
        "--analysis-table", type=Path, default=None, help="pinned original function-bound metadata"
    )
    parser.add_argument(
        "--generator-version",
        choices=("t1773-synth-v1", "t1773-synth-v2"),
        default="t1773-synth-v1",
    )
    parser.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    parser.add_argument(
        "--census-csv",
        type=Path,
        default=Path("tmp/t1772/final/census.csv"),
        help="T1772 census rows (va,size,baseline,tier), see tools.t1772_replacement_census",
    )
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("split", help="recompute and check the frozen split")
    for name in ("derive", "verify", "measure"):
        p = sub.add_parser(name)
        p.add_argument("--sample", choices=("heldout", "development", "all"), default="heldout")
        p.add_argument("--va", action="append", default=[], help="explicit hex VA (repeatable)")
        if name in ("derive", "verify"):
            p.add_argument("--out-dir", type=Path, required=True)
        if name == "measure":
            p.add_argument("--seed", type=int, action="append", default=None)
            p.add_argument("--random-cases", type=int, default=600)
            p.add_argument("--max-insns", type=int, default=200_000)
            p.add_argument("--out", type=Path, default=None)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "measure" and not args.seed:
        args.seed = list(DEFAULT_SEEDS)
    handler = {
        "split": cmd_split,
        "derive": cmd_derive,
        "verify": cmd_verify,
        "measure": cmd_measure,
    }
    return handler[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
