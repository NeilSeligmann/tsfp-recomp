# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit the name overlay with Jev (screen) and gpt-5.5 (judge), T1484.

    python -m tools.llm_name_audit bundles --root /workspace
    python -m tools.llm_name_audit screen   --jev-key-file KEY --max-usd 1
    python -m tools.llm_name_audit calibrate --openai-key-file KEY --max-usd 4
    python -m tools.llm_name_audit review --openai-key-file KEY --max-usd 4
        # optional: --threshold-b 0.65 --threshold-mean 0.5
    python -m tools.llm_name_audit report

Work files live in gitignored tmp/t1484/. Only per-function bundles (names, evidence
text, string excerpts, short disassembly excerpt) are sent, never files or hashes.
Key files are passed to tools.llm clients as paths and never read here.
Read-only on function_names.csv. Output tables: generated/name-review-queue.csv.
"""

from __future__ import annotations

import argparse
import csv
import json
import random
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

WORK = Path("tmp/t1484")
NAMES = Path("tools/data/function_names.csv")
QUEUE = Path("generated/name-review-queue.csv")
QUESTIONS = {
    ("a"): (
        "Does the evidence line state concrete, verifiable facts "
        "(instructions, callees, strings, sizes) that justify this exact "
        "function name?"
    ),
    ("b"): (
        "Does the function name describe the behaviour or role of the "
        "function, rather than merely an address, a size or a code shape?"
    ),
    ("c"): (
        "Is the function name consistent with the facts stated in the "
        "evidence line, with no contradiction between them?"
    ),
}
VERDICTS = ["supported", "plausible", "unsupported", "wrong"]
BAD = {"unsupported", "wrong"}
GPT_SCHEMA = {
    "type": "object",
    "properties": {
        "verdict": {"type": "string", "enum": VERDICTS},
        "better_name": {"type": "string"},
        "reason": {"type": "string"},
    },
    "required": ["verdict", "better_name", "reason"],
    "additionalProperties": False,
}
GPT_SYSTEM = (
    "You audit function names in a decompiled Xbox (x86-32) game. Given a function's "
    "name, the evidence text that justified it, its size, named callees/callers, "
    "referenced strings, global addresses and a disassembly excerpt, judge the name. "
    "Verdicts: supported (disassembly and context clearly back the name), plausible "
    "(consistent but not proven), unsupported (the evidence does not justify the "
    "name, or it is merely an address or shape), wrong (the code does something "
    "else). Names are snake_case starting with game_. Give better_name only if you "
    "can propose a clearly better snake_case name (empty string otherwise). reason is"
    " at most 40 words. Judge only from the data given."
)


def read_rows(path: Path = NAMES) -> list[dict]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def disasm_lines(instructions: Any, known: dict[int, str], imm_type: Any, cap: int) -> list[str]:
    out = []
    for index, insn in enumerate(instructions):
        if index >= cap:
            out.append("...")
            break
        note = ""
        if insn.mnemonic == "call" and insn.operands and insn.operands[0].type == imm_type:
            note = "  ; " + known.get(insn.operands[0].imm, "unnamed")
        out.append(f"{insn.mnemonic} {insn.op_str}{note}")
    return out


def make_bundle(
    row: dict,
    size: int,
    callees: list[str],
    callers: list[str],
    strings: list[str],
    globs: list[int],
    kernel: list[str],
    disasm: list[str],
) -> dict:
    return {
        "va": row["entry_va"],
        "name": row["name"],
        "confidence": row["confidence"],
        "evidence": row["evidence"],
        "size": size,
        "callees": sorted(set(callees)),
        "callers": sorted(set(callers)),
        "strings": strings[:3],
        "globals": [f"0x{g:08x}" for g in sorted(globs)[:6]],
        "kernel": sorted(kernel),
        "disasm": disasm,
    }


def jev_state(bundle: dict) -> str:
    parts = [f"Function name: {bundle['name']}", f"Evidence: {bundle['evidence']}"]
    if bundle["callees"]:
        parts.append("Named callees: " + ", ".join(bundle["callees"][:12]))
    return "\n".join(parts)


def gpt_prompt(bundle: dict) -> str:
    lines = [
        f"name: {bundle['name']}",
        f"confidence label: {bundle['confidence']}",
        f"evidence: {bundle['evidence']}",
        f"size bytes: {bundle['size']}",
        "named callees: " + (", ".join(bundle["callees"][:16]) or "none"),
        "named callers: " + (", ".join(bundle["callers"][:8]) or "none"),
        "kernel imports: " + (", ".join(bundle["kernel"]) or "none"),
        "strings: " + (" | ".join(bundle["strings"]) or "none"),
        "globals: " + (", ".join(bundle["globals"]) or "none"),
        "disassembly excerpt:",
        *("  " + line for line in bundle["disasm"]),
    ]
    return "\n".join(lines)


def combine(scores: dict) -> dict:
    values = [scores[q] for q in QUESTIONS]
    return {**scores, "min": min(values), "mean": sum(values) / len(values)}


def auc(scores: list[float], positives: list[bool]) -> float | None:
    """P(score of a non-positive > score of a positive): Mann-Whitney, ties half.
    Positives are the bad rows, a good screen gives LOW scores to them, so AUC>0.5 is good."""
    pos = [s for s, p in zip(scores, positives, strict=False) if p]
    neg = [s for s, p in zip(scores, positives, strict=False) if not p]
    if not pos or not neg:
        return None
    wins = sum((n > p) + 0.5 * (n == p) for p in pos for n in neg)
    return wins / (len(pos) * len(neg))


def precision_recall(scores: list[float], positives: list[bool], threshold: float) -> dict:
    flagged = [s < threshold for s in scores]
    tp = sum(f and p for f, p in zip(flagged, positives, strict=False))
    fp = sum(f and not p for f, p in zip(flagged, positives, strict=False))
    fn = sum((not f) and p for f, p in zip(flagged, positives, strict=False))
    return {
        "threshold": threshold,
        "flagged": tp + fp,
        "tp": tp,
        "fp": fp,
        "fn": fn,
        "precision": tp / (tp + fp) if tp + fp else None,
        "recall": tp / (tp + fn) if tp + fn else None,
    }


def pick_sample(
    rows: list[dict], scores: dict[str, float], n_strat: int, n_low: int, n_high: int, seed: int
) -> list[str]:
    """Stratified random sample by (confidence, name prefix) plus lowest and highest scored."""
    rng = random.Random(seed)
    by_stratum: dict[tuple, list[str]] = {}
    for row in rows:
        prefix = "_".join(row["name"].split("_")[:2])
        by_stratum.setdefault((row["confidence"], prefix), []).append(row["entry_va"])
    chosen: list[str] = []
    strata = sorted(by_stratum.items())
    total = sum(len(v) for _, v in strata)
    for _, vas in strata:
        quota = max(1, round(n_strat * len(vas) / total)) if len(vas) else 0
        chosen += rng.sample(vas, min(quota, len(vas)))
    rng.shuffle(chosen)
    chosen = chosen[:n_strat]
    ranked = sorted(scores, key=lambda va: (scores[va], va))
    for va in ranked[:n_low] + ranked[::-1][:n_high]:
        if va not in chosen:
            chosen.append(va)
    return chosen


def read_jsonl(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    return [
        json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()
    ]


def cmd_bundles(args: Any) -> int:
    from capstone.x86 import X86_OP_IMM

    from tools import name_graph

    image, funcs, known, _game, info, callers, _owners, md = name_graph.build(args.root, args.xbe)
    rows = read_rows(args.names)
    WORK.mkdir(parents=True, exist_ok=True)
    with (WORK / "bundles.jsonl").open("w", encoding="utf-8") as out:
        for row in rows:
            va = int(row["entry_va"], 16)
            if va not in info:
                bundle = make_bundle(row, 0, [], [], [], [], [], [])
            else:
                calls, globs, kern, strs = info[va]
                size = funcs[va]
                lines = disasm_lines(
                    image.body(md, va, size).instructions, known, X86_OP_IMM, args.max_insn
                )
                bundle = make_bundle(
                    row,
                    size,
                    [known[c] for c in calls if c in known and known[c] != row["name"]],
                    [known[c] for c in callers.get(va, ()) if c in known],
                    strs,
                    list(globs),
                    list(kern),
                    lines,
                )
            out.write(json.dumps(bundle) + "\n")
    print(f"wrote {len(rows)} bundles to {WORK / 'bundles.jsonl'}")
    return 0


def make_clients(args: Any, which: str) -> Any:
    from tools.llm.common import Cache, Ledger, read_key

    cache, ledger = Cache(Path(args.cache_dir), not args.no_cache), Ledger(Path(args.ledger))
    key = read_key(args.jev_key_file if which == "jev" else args.openai_key_file)
    if which == "jev":
        from tools.llm.jev_client import JevClient

        return JevClient(
            key=key, concurrency=args.concurrency, max_usd=args.max_usd, cache=cache, ledger=ledger
        )
    from tools.llm.openai_client import OpenAIClient

    return OpenAIClient(
        key=key,
        model=args.model,
        effort=args.effort,
        concurrency=args.concurrency,
        max_usd=args.max_usd,
        cache=cache,
        ledger=ledger,
        max_output_tokens=3000,
    )


def screen_one(client: Any, bundle: dict) -> dict:
    from tools.llm.jev_client import noul

    questions = {q: noul(text) for q, text in QUESTIONS.items()}
    result = client.ask(jev_state(bundle), questions)
    scores = {q: float(result.answers[q]["noul"]) for q in QUESTIONS}
    return {"va": bundle["va"], **combine(scores)}


def cmd_screen(args: Any) -> int:
    bundles = read_jsonl(WORK / "bundles.jsonl")
    done = {r["va"] for r in read_jsonl(WORK / "jev.jsonl")}
    todo = [b for b in bundles if b["va"] not in done]
    client = make_clients(args, "jev")
    with (
        (WORK / "jev.jsonl").open("a", encoding="utf-8") as out,
        ThreadPoolExecutor(args.concurrency) as pool,
    ):
        for record in pool.map(lambda b: screen_one(client, b), todo):
            out.write(json.dumps(record) + "\n")
    print(f"screened {len(todo)} rows, process spend ${client.ledger.process_usd:.4f}")
    return 0


def judge_one(client: Any, bundle: dict) -> dict:
    result = client.complete(gpt_prompt(bundle), GPT_SYSTEM, GPT_SCHEMA, "name_audit")
    return {"va": bundle["va"], **result.parsed, "usd": result.usd}


def run_gpt(args: Any, vas: list[str]) -> None:
    bundles = {b["va"]: b for b in read_jsonl(WORK / "bundles.jsonl")}
    done = {r["va"] for r in read_jsonl(WORK / "gpt.jsonl")}
    todo = [bundles[v] for v in vas if v not in done]
    client = make_clients(args, "openai")
    with (
        (WORK / "gpt.jsonl").open("a", encoding="utf-8") as out,
        ThreadPoolExecutor(args.concurrency) as pool,
    ):
        for record in pool.map(lambda b: judge_one(client, b), todo):
            out.write(json.dumps(record) + "\n")
    print(f"judged {len(todo)} rows, process spend ${client.ledger.process_usd:.4f}")


def jev_scores(key: str = "min") -> dict[str, float]:
    return {r["va"]: r[key] for r in read_jsonl(WORK / "jev.jsonl")}


def cmd_calibrate(args: Any) -> int:
    rows = read_rows(args.names)
    sample = pick_sample(
        rows, jev_scores(args.score_key), args.n_strat, args.n_low, args.n_high, args.seed
    )
    (WORK / "calibration-sample.json").write_text(json.dumps(sample))
    run_gpt(args, sample)
    return 0


def flagged_vas(jev: dict[str, dict], threshold_b: float, threshold_mean: float) -> list[str]:
    """Rows the screen flags: low question b (behaviour/role) OR low combined mean. Worst first."""
    hits = [va for va, r in jev.items() if r["b"] < threshold_b or r["mean"] < threshold_mean]
    return sorted(hits, key=lambda va: (jev[va]["mean"] + jev[va]["b"], va))


def cmd_review(args: Any) -> int:
    jev = {r["va"]: r for r in read_jsonl(WORK / "jev.jsonl")}
    flagged = flagged_vas(jev, args.threshold_b, args.threshold_mean)
    judged = {r["va"] for r in read_jsonl(WORK / "gpt.jsonl")}
    print(f"{len(flagged)} flagged, {len([v for v in flagged if v not in judged])} still to judge")
    run_gpt(args, flagged)
    return 0


def metrics(jev: dict, gpt: dict, vas: list[str]) -> dict:
    vas = [v for v in vas if v in gpt and v in jev]
    out = {}
    for key in ["a", "b", "c", "min", "mean"]:
        s, p = [jev[v][key] for v in vas], [gpt[v]["verdict"] in BAD for v in vas]
        out[key] = {
            "n": len(vas),
            "bad": sum(p),
            "auc": auc(s, p),
            "thresholds": [
                precision_recall(s, p, t) for t in (0.25, 0.4, 0.5, 0.6, 0.65, 0.7, 0.75, 0.9)
            ],
        }
    return out


def band_stats(jev: dict, gpt: dict, vas: list[str], key: str, edges: list[float]) -> list[dict]:
    rows = []
    for lo, hi in zip(edges, edges[1:], strict=False):
        band = [v for v in vas if v in gpt and v in jev and lo <= jev[v][key] < hi]
        counts = {k: sum(gpt[v]["verdict"] == k for v in band) for k in VERDICTS}
        rows.append(
            {
                "lo": lo,
                "hi": hi,
                "n": len(band),
                **counts,
                "bad_rate": (counts["unsupported"] + counts["wrong"]) / len(band) if band else None,
            }
        )
    return rows


def cmd_report(args: Any) -> int:
    jev = {r["va"]: r for r in read_jsonl(WORK / "jev.jsonl")}
    gpt = {r["va"]: r for r in read_jsonl(WORK / "gpt.jsonl")}
    names = {r["entry_va"]: r for r in read_rows(args.names)}
    sample = json.loads((WORK / "calibration-sample.json").read_text())
    judged = list(gpt)
    stats = {
        "sample_all": metrics(jev, gpt, sample),
        "sample_strat_only": metrics(jev, gpt, sample[: args.n_strat]),
        "judged_all": metrics(jev, gpt, judged),
        "bands_mean": band_stats(
            jev, gpt, sample[: args.n_strat], "mean", [0, 0.4, 0.5, 0.6, 0.7, 0.8, 1.01]
        ),
        "bands_b": band_stats(
            jev, gpt, sample[: args.n_strat], "b", [0, 0.4, 0.55, 0.7, 0.8, 0.9, 1.01]
        ),
        "verdicts_judged": {k: sum(g["verdict"] == k for g in gpt.values()) for k in VERDICTS},
        "judged": len(gpt),
        "screened": len(jev),
    }
    (WORK / "stats.json").write_text(json.dumps(stats, indent=1))
    QUEUE.parent.mkdir(parents=True, exist_ok=True)
    with QUEUE.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["entry_va", "name", "jev_scores", "gpt_verdict", "better_name", "reason"])
        for va in sorted(gpt):
            if gpt[va]["verdict"] in BAD and va in jev:
                j = jev[va]
                writer.writerow(
                    [
                        va,
                        names[va]["name"],
                        f"a={j['a']:.3f} b={j['b']:.3f} c={j['c']:.3f}",
                        gpt[va]["verdict"],
                        gpt[va]["better_name"],
                        gpt[va]["reason"],
                    ]
                )
    print(f"wrote {WORK / 'stats.json'} and {QUEUE}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--names", type=Path, default=NAMES)
    parser.add_argument("--jev-key-file", default=".secrets/typesafe-ai.txt")
    parser.add_argument("--openai-key-file", default=".secrets/openai-api-key.txt")
    parser.add_argument("--max-usd", type=float, default=1.0)
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--no-cache", action="store_true")
    parser.add_argument("--cache-dir", default="tmp/llm-cache")
    parser.add_argument("--ledger", default="tmp/llm-usage.jsonl")
    parser.add_argument("--model", default="gpt-5.5")
    parser.add_argument("--effort", default="low", choices=["low", "medium", "high"])
    sub = parser.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bundles")
    b.add_argument("--root", type=Path, default=Path("."))
    b.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    b.add_argument("--max-insn", type=int, default=60)
    sub.add_parser("screen")
    c = sub.add_parser("calibrate")
    c.add_argument("--n-strat", type=int, default=150)
    c.add_argument("--n-low", type=int, default=100)
    c.add_argument("--n-high", type=int, default=50)
    c.add_argument("--seed", type=int, default=1484)
    c.add_argument("--score-key", default="mean", choices=["a", "b", "c", "min", "mean"])
    r = sub.add_parser("review")
    r.add_argument("--threshold-b", type=float, default=0.65)
    r.add_argument("--threshold-mean", type=float, default=0.5)
    rp = sub.add_parser("report")
    rp.add_argument("--n-strat", type=int, default=150)
    args = parser.parse_args(argv)
    return {
        "bundles": cmd_bundles,
        "screen": cmd_screen,
        "calibrate": cmd_calibrate,
        "review": cmd_review,
        "report": cmd_report,
    }[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
