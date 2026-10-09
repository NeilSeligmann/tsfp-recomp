# SPDX-License-Identifier: GPL-3.0-or-later
"""LLM-assisted naming pipeline, T1485 pilot (proposer, grounding, verifier, acceptance).

    python -m tools.llm_name_pipeline bundles --lo 0x80000 --hi 0x100000
    python -m tools.llm_name_pipeline --openai-key-file KEY --max-usd 3 propose --set holdout
    python -m tools.llm_name_pipeline --openai-key-file KEY --max-usd 3 verify  --set holdout
    python -m tools.llm_name_pipeline --openai-key-file KEY --max-usd 1 judge-holdout
    python -m tools.llm_name_pipeline report-holdout
    python -m tools.llm_name_pipeline --openai-key-file KEY --max-usd 10 propose --set pilot
    python -m tools.llm_name_pipeline --openai-key-file KEY --max-usd 10 verify  --set pilot
    python -m tools.llm_name_pipeline accept --threshold 0.6 [--apply]

Stages: (1) bundle per function, (2) proposer (gpt-5.5, strict json_schema), (3) grounding
(programmatic, no LLM: every cited ref must literally exist in the bundle, the name must pass
the loader rules), (4) verifier in REFUTE mode (second gpt-5.5 pass), optional Jev pre-filter
(never an accept signal), (5) acceptance and append through tools.name_candidates.
Work files are gitignored tmp/t1485/. Only per-function bundles (names, short identifiers,
a disassembly excerpt of at most 80 instructions) leave the machine, never files, owner paths
or hashes. Key files are passed to tools.llm clients as paths and never read here.
"""

from __future__ import annotations

import argparse
import csv
import json
import random
import re
import sys
from collections import Counter, defaultdict
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from tools.llm.openai_client import OpenAIClient

WORK = Path("tmp/t1485")
NAMES = Path("tools/data/function_names.csv")
QUEUE = Path("generated/llm-naming-queue.csv")
MAX_INSN = 80
MIN_CONFIDENCE = 0.6
CALIBRATED_THRESHOLD = 0.85  # hold-out calibration, docs/t1485-llm-naming-pilot.md
JEV_DROP_BELOW = 0.25
EVIDENCE_KINDS = ["callee", "string", "global", "table", "structure"]
VERDICTS = ["holds", "weak", "contradicted"]
SAME_ROLE = ["yes", "partial", "no"]

IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
PLACEHOLDER = re.compile(r"^(?:thunk_)?(?:FUN|SUB|LAB|CODEDIFF)_[0-9a-fA-F]+$")
SHORT_STRING = re.compile(r"^[A-Za-z_][A-Za-z0-9_.:/\-]{2,39}$")
#: Hex address or offset tokens inside a name (`_6b98b4_`, `_0x64`), the weak address-shaped class.
ADDRESS_TOKEN = re.compile(r"(?:^|_)(?:0x[0-9a-fA-F]+|(?=[0-9a-f]*[0-9])[0-9a-f]{5,8})(?:_|$)")
SHAPE_TOKENS = {"wrapper", "via"}
SHAPE_PREFIXES = (
    "game_global_",
    "game_call_",
    "game_address_of",
    "game_get_global_",
    "game_obj_set_field",
)

PROPOSER_SCHEMA = {
    "type": "object",
    "properties": {
        "name": {"type": "string"},
        "role_summary": {"type": "string"},
        "evidence_facts": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "kind": {"type": "string", "enum": EVIDENCE_KINDS},
                    "ref": {"type": "string"},
                    "claim": {"type": "string"},
                },
                "required": ["kind", "ref", "claim"],
                "additionalProperties": False,
            },
        },
        "confidence": {"type": "number"},
        "unsure": {"type": "boolean"},
    },
    "required": ["name", "role_summary", "evidence_facts", "confidence", "unsure"],
    "additionalProperties": False,
}
VERIFIER_SCHEMA = {
    "type": "object",
    "properties": {"verdict": {"type": "string", "enum": VERDICTS}, "reason": {"type": "string"}},
    "required": ["verdict", "reason"],
    "additionalProperties": False,
}
JUDGE_SCHEMA = {
    "type": "object",
    "properties": {
        "same_role": {"type": "string", "enum": SAME_ROLE},
        "reason": {"type": "string"},
    },
    "required": ["same_role", "reason"],
    "additionalProperties": False,
}

PROPOSER_SYSTEM = (
    "You name functions of a decompiled Xbox (x86-32) game, one function "
    "at a time. You get an evidence "
    "bundle: size, named callees and callers (names only), short string identifiers it references, "
    "globals it touches with the names of other functions that use them, "
    "vtable or table membership, "
    "kernel imports and a disassembly excerpt. Name the function ONLY when the bundle and the code "
    "establish its role. Prefer unsure=true (name empty) over guessing: an unnamed function costs "
    "nothing, a wrong name misleads. Never name a function after an "
    "address, a size, a global address, "
    "an offset or a code shape; name it after what it does for the game. Naming convention: "
    "game_<domain>_<verb>_<object> in snake_case, a valid C identifier, "
    "game_ prefix, no hex tokens, "
    "no dots, no thunk_/FUN_ forms, unique. Follow the existing vocabulary "
    "below (domains and examples "
    "of accepted names). role_summary is at most 30 words. evidence_facts "
    "lists the concrete facts that "
    "justify the name: kind is callee (a callee or caller name from the bundle), string (a string "
    "identifier), global (a global address like 0x005xxxxx from the "
    "bundle), table (a table address "
    "from the bundle) or structure (a literal operand or offset that "
    "appears in the disassembly, for "
    "example 0x10); ref MUST be copied exactly from the bundle, claim says what the fact shows. "
    "confidence is 0 to 1 and must reflect how sure you are that the name is correct."
)
VERIFIER_SYSTEM = (
    "You are a skeptical reviewer of a proposed function name in a decompiled Xbox (x86-32) game. "
    "Try to REFUTE the proposal using the disassembly and bundle. verdict: "
    "holds (the code does what "
    "the name and role summary say, nothing contradicts it, the cited "
    "facts are real and relevant), "
    "weak (plausible but not established, over-specific, names a guessed "
    "behaviour, or the facts do "
    "not carry the claim), contradicted (the code does something else, "
    "wrong direction, wrong object "
    "or wrong count). Check conditionals the name ignores, inverted tests, "
    "and claims beyond the code. "
    "reason is at most 40 words."
)
JUDGE_SYSTEM = (
    "You compare two names for the same function. Given the established "
    "name with its evidence and a "
    "proposed name with its role summary, answer whether they describe the same role: yes (same "
    "behaviour and object, differences are wording or granularity), "
    "partial (overlapping but one is "
    "clearly broader, narrower or about a different aspect), no (different "
    "role). reason at most 30 words."
)


# ---------------------------------------------------------------------------- bundles


def read_rows(path: Path = NAMES) -> list[dict]:
    with Path(path).open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def short_strings(strings: list[str]) -> list[str]:
    """Only short identifier-like strings leave the machine (no sentences, paths, long text)."""
    out = []
    for text in strings:
        if SHORT_STRING.match(text) and text not in out:
            out.append(text)
    return out[:6]


def address_shaped(name: str) -> bool:
    return (
        bool(ADDRESS_TOKEN.search(name))
        or bool(SHAPE_TOKENS & set(name.split("_")))
        or name.startswith(SHAPE_PREFIXES)
    )


def make_bundle(
    va: int,
    size: int,
    callees: list[str],
    callers: list[str],
    strings: list[str],
    globals_: dict[int, list[str]],
    tables: list[dict],
    kernel: list[str],
    disasm: list[str],
) -> dict:
    return {
        "va": f"0x{va:08x}",
        "size": size,
        "callees": sorted(set(callees))[:20],
        "callers": sorted(set(callers))[:12],
        "strings": short_strings(strings),
        "globals": [
            {
                "addr": f"0x{addr:08x}",
                "users": sorted(set(users), key=lambda n: (address_shaped(n), n))[:6],
            }
            for addr, users in sorted(globals_.items())[:8]
        ],
        "tables": tables[:3],
        "kernel": sorted(set(kernel)),
        "disasm": disasm[: MAX_INSN + 1],
    }


def bundle_text(bundle: dict) -> str:
    """The exact text that is sent for a bundle. Whitelisted fields only."""
    lines = [
        f"size bytes: {bundle['size']}",
        "named callees: " + (", ".join(bundle["callees"]) or "none"),
        "named callers: " + (", ".join(bundle["callers"]) or "none"),
        "kernel imports: " + (", ".join(bundle["kernel"]) or "none"),
        "string identifiers: " + (", ".join(bundle["strings"]) or "none"),
    ]
    for item in bundle["globals"]:
        lines.append(
            f"global {item['addr']} also used by: "
            + (", ".join(item["users"]) or "no named function")
        )
    for table in bundle["tables"]:
        lines.append(
            f"table {table['table']} slot {table['slot']} of {table['slots']}, named siblings: "
            + (", ".join(table["siblings"]) or "none")
        )
    lines.append("disassembly excerpt:")
    lines += ["  " + text for text in bundle["disasm"]]
    return "\n".join(lines)


def bundle_refs(bundle: dict) -> dict[str, set[str]]:
    """Every identifier a proposal may cite, per evidence kind."""
    disasm = "\n".join(bundle["disasm"])
    return {
        "callee": set(bundle["callees"]) | set(bundle["callers"]) | set(bundle["kernel"]),
        "string": set(bundle["strings"]),
        "global": {item["addr"] for item in bundle["globals"]},
        "table": {table["table"] for table in bundle["tables"]}
        | {sib for table in bundle["tables"] for sib in table["siblings"]},
        "structure": {
            "__disasm__": disasm
        },  # structure refs are substring-checked in the disassembly
    }


def norm_hex(text: str) -> str:
    text = text.strip().lower()
    if re.fullmatch(r"0x[0-9a-f]+", text):
        return f"0x{int(text, 16):08x}" if len(text) > 6 else text
    return text


# ---------------------------------------------------------------------------- grounding


def name_problems(name: str, taken: set[str]) -> list[str]:
    """Loader rules (tools.coverage.load_function_names) plus pipeline rules."""
    problems = []
    if not name:
        return ["empty name"]
    if PLACEHOLDER.match(name):
        problems.append("placeholder-shaped")
    if not IDENTIFIER.match(name):
        problems.append("not a C identifier")
    if "." in name:
        problems.append("contains a dot")
    if not name.startswith("game_"):
        problems.append("no game_ prefix")
    if name in taken:
        problems.append("duplicate name")
    if address_shaped(name):
        problems.append("address or shape shaped name")
    return problems


NUMBER = re.compile(r"(?<![\w.])(-?0x[0-9a-fA-F]+|-?\d+)(?![\w.])")


def numeric(text: str) -> int | None:
    try:
        return int(text.strip(), 0)
    except ValueError:
        return None


def in_disasm(ref: str, disasm: str) -> bool:
    """A structure ref is real when it is a number that appears in the excerpt (hex or decimal
    spelling, capstone prints small values in decimal) or text that appears verbatim in it."""
    value = numeric(ref)
    if value is not None:
        return value in {numeric(m) for m in NUMBER.findall(disasm)} - {None}
    return len(ref) >= 3 and ref.lower() in disasm.lower()


def ground(proposal: dict, bundle: dict, taken: set[str]) -> list[str]:
    """Programmatic grounding. Returns the list of failures, empty when grounded."""
    problems = name_problems(proposal.get("name", ""), taken)
    facts = proposal.get("evidence_facts") or []
    refs = bundle_refs(bundle)
    anchored = 0
    for fact in facts:
        kind, ref = fact.get("kind"), str(fact.get("ref", "")).strip()
        if kind not in EVIDENCE_KINDS or not ref:
            problems.append(f"malformed fact {kind!r}")
            continue
        disasm = refs["structure"]["__disasm__"]
        if kind == "structure":
            ok = in_disasm(ref, disasm)
        elif kind == "global" or kind == "table":
            ok = (
                norm_hex(ref) in {norm_hex(r) for r in refs[kind]}
                or ref in refs[kind]
                or (kind == "global" and numeric(ref) is not None and in_disasm(ref, disasm))
            )
        else:
            ok = ref in refs[kind]
        if not ok:
            problems.append(f"fabricated {kind} ref {ref!r}")
        elif kind != "structure":
            anchored += 1
    if len(facts) < 2:
        problems.append("fewer than 2 evidence facts")
    if facts and anchored == 0 and not any(p.startswith("fabricated") for p in problems):
        problems.append("no callee, string, global or table anchor")
    if len(proposal.get("role_summary", "").split()) > 40:
        problems.append("role summary too long")
    return problems


def accept_decision(
    proposal: dict,
    grounding: list[str],
    verdict: str | None,
    threshold: float = MIN_CONFIDENCE,
    jev_b: float | None = None,
) -> tuple[bool, str]:
    """Accept = grounded AND verifier holds AND proposer not unsure AND confidence >= threshold.
    jev_b is only a pre-filter (drop below JEV_DROP_BELOW), never an accept signal."""
    if proposal.get("unsure"):
        return False, "proposer unsure"
    if grounding:
        return False, "grounding: " + "; ".join(grounding[:3])
    if jev_b is not None and jev_b < JEV_DROP_BELOW:
        return False, f"jev prefilter b={jev_b:.2f}"
    if float(proposal.get("confidence", 0)) < threshold:
        return False, f"confidence {proposal.get('confidence')} below {threshold}"
    if verdict != "holds":
        return False, f"verifier {verdict}"
    return True, "accepted"


def dedupe_accepts(items: list[dict]) -> list[dict]:
    """Within one batch the first (highest confidence, then lowest VA) proposal keeps a name."""
    seen: set[str] = set()
    out = []
    for item in sorted(items, key=lambda i: (-float(i["proposal"]["confidence"]), i["va"])):
        if item["proposal"]["name"] in seen:
            item["reason"] = "duplicate of another accepted proposal"
            item["accepted"] = False
            continue
        seen.add(item["proposal"]["name"])
        out.append(item)
    return out


# ---------------------------------------------------------------------------- prompts


def vocabulary(
    rows: list[dict],
    domains: int = 45,
    examples: int = 25,
    seed: int = 1485,
    exclude: frozenset[str] = frozenset(),
) -> str:
    """Domain tokens with counts and example names from the overlay (not address-shaped).
    `exclude` keeps hidden hold-out names out of the examples (a leak found in the first run)."""
    clean = [
        r["name"]
        for r in rows
        if not address_shaped(r["name"])
        and r["name"].startswith("game_")
        and r["name"] not in exclude
    ]
    counts = Counter(n.split("_")[1] for n in clean if n.count("_") >= 2)
    rng = random.Random(seed)
    sample = sorted(rng.sample(clean, min(examples, len(clean))))
    return (
        "Existing domains (token after game_, count): "
        + ", ".join(f"{d}({c})" for d, c in counts.most_common(domains))
        + ". Example accepted names: "
        + ", ".join(sample)
        + "."
    )


def proposer_prompt(bundle: dict) -> str:
    return f"function {bundle['va']}\n" + bundle_text(bundle)


def verifier_prompt(bundle: dict, proposal: dict) -> str:
    facts = "\n".join(
        f"  - {f['kind']} {f['ref']}: {f['claim']}" for f in proposal["evidence_facts"]
    )
    return (
        f"function {bundle['va']}\n{bundle_text(bundle)}\n\nproposed name: {proposal['name']}\n"
        f"role summary: {proposal['role_summary']}\ncited facts:\n{facts}"
    )


def judge_prompt(truth: dict, proposal: dict) -> str:
    return (
        f"established name: {truth['name']}\nestablished evidence: {truth['evidence']}\n\n"
        f"proposed name: {proposal['name']}\nproposed role summary: {proposal['role_summary']}"
    )


def jev_state(proposal: dict) -> str:
    facts = "; ".join(
        f"{f['kind']} {f['ref']}: {f['claim']}" for f in proposal["evidence_facts"][:6]
    )
    return f"Function name: {proposal['name']}\nRole: {proposal['role_summary']}\nEvidence: {facts}"


JEV_QUESTION = (
    "Does the function name describe the behaviour or role of the "
    "function, rather than merely an address, a size or a code shape?"
)


# ---------------------------------------------------------------------------- IO helpers


def read_jsonl(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    return [
        json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()
    ]


def append_jsonl(path: Path, record: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record) + "\n")


def files(set_name: str) -> dict[str, Path]:
    return {
        key: WORK / f"{set_name}-{key}.jsonl"
        for key in ("bundles", "proposals", "verdicts", "jev", "judge")
    }


def make_openai(args: argparse.Namespace) -> OpenAIClient:
    from tools.llm.common import Cache, Ledger, read_key
    from tools.llm.openai_client import OpenAIClient

    return OpenAIClient(
        key=read_key(args.openai_key_file),
        model=args.model,
        effort=args.effort,
        concurrency=args.concurrency,
        max_usd=args.max_usd,
        cache=Cache(Path(args.cache_dir), not args.no_cache),
        ledger=Ledger(Path(args.ledger)),
        max_output_tokens=3000,
    )


def run_stage(
    args: argparse.Namespace, todo: list, work: Callable[[dict], dict], out_path: Path, label: str
) -> None:
    """Run work(item) over todo with a thread pool, append each result as it completes."""
    from tools.llm.common import CostCapError

    stopped = False
    with ThreadPoolExecutor(args.concurrency) as pool:
        futures = [pool.submit(work, item) for item in todo]
        done = errors = 0
        for future in futures:
            try:
                append_jsonl(out_path, future.result())
                done += 1
            except CostCapError as exc:
                stopped = True
                print(f"cost cap reached: {exc}", file=sys.stderr)
                break
            except Exception as exc:  # one failed row must not stop the run, it is retried on rerun
                errors += 1
                print(f"{label} error: {type(exc).__name__}: {str(exc)[:160]}", file=sys.stderr)
        if stopped:
            for future in futures:
                future.cancel()
    print(f"{label}: {done} done, {errors} errors, stopped={stopped}")


# ---------------------------------------------------------------------------- commands


def cmd_bundles(args: argparse.Namespace) -> int:
    from capstone.x86 import X86_OP_IMM

    from tools import name_graph
    from tools.llm_name_audit import disasm_lines
    from tools.name_tables import scan_tables

    image, funcs, known, game, info, callers, owners, md = name_graph.build(args.root, args.xbe)
    tables: dict[int, dict] = {}
    for table, members in scan_tables(image, funcs, 2):
        for slot, member in enumerate(members):
            tables.setdefault(
                member,
                {
                    "table": f"0x{table:08x}",
                    "slot": slot,
                    "slots": len(members),
                    "siblings": sorted({known[m] for m in members if m in known and m != member})[
                        :6
                    ],
                },
            )
    rows = read_rows(args.root / NAMES)
    named = {int(r["entry_va"], 16) for r in rows}

    def build(va: int) -> dict:
        calls, globs, kern, strs = info[va]
        users = {g: [known[o] for o in owners[g] if o != va and o in known] for g in globs}
        # A recursive call must not reveal the function's own name (hold-out leak found in run 1).
        view = {k: v for k, v in known.items() if k != va}
        lines = disasm_lines(image.body(md, va, funcs[va]).instructions, view, X86_OP_IMM, MAX_INSN)
        return make_bundle(
            va,
            funcs[va],
            [known[c] for c in calls if c in known],
            [known[c] for c in callers.get(va, ()) if c in known],
            strs,
            users,
            [tables[va]] if va in tables else [],
            list(kern),
            lines,
        )

    WORK.mkdir(parents=True, exist_ok=True)
    pilot = sorted(
        v
        for v in game
        if args.lo <= v < args.hi and v in funcs and v not in known and v not in named
    )
    with files("pilot")["bundles"].open("w", encoding="utf-8") as out:
        for va in pilot:
            out.write(json.dumps(build(va)) + "\n")
    print(f"pilot: {len(pilot)} unnamed game functions in 0x{args.lo:x}-0x{args.hi:x}")
    truth = select_holdout(
        rows, args.hold_lo, args.hold_hi, args.holdout, args.seed, args.judged_file
    )
    with files("holdout")["bundles"].open("w", encoding="utf-8") as out:
        for row in truth:
            va = int(row["entry_va"], 16)
            if va in info and va in funcs:
                out.write(json.dumps(build(va)) + "\n")
    (WORK / "holdout-truth.json").write_text(
        json.dumps({r["entry_va"]: r for r in truth}, indent=1)
    )
    print(f"holdout: {len(truth)} named rows hidden, truth in {WORK / 'holdout-truth.json'}")
    return 0


def select_holdout(
    rows: list[dict], lo: int, hi: int, count: int, seed: int, judged_file: Path | None
) -> list[dict]:
    """Named, hand-evidenced, not address-shaped, not source-seeded,
    not judged bad by the T1484 audit.
    Rows the T1484 judge called supported come first (stronger ground truth), then unjudged rows."""
    verdicts = {}
    if judged_file and Path(judged_file).is_file():
        verdicts = {r["va"]: r["verdict"] for r in read_jsonl(Path(judged_file))}
    pool = []
    for row in rows:
        va = int(row["entry_va"], 16)
        if not lo <= va < hi or address_shaped(row["name"]) or "src/" in row["evidence"]:
            continue
        if row["confidence"] not in (
            "INFERRED",
            "VERIFIED",
            "TRACED",
            "MEASURED",
            "HIGH",
            "MEDIUM",
        ):
            continue
        if verdicts.get(row["entry_va"]) in ("unsupported", "wrong", "plausible"):
            continue
        if row["name"].count("_") < 2 or len(row["evidence"]) < 40:
            continue
        pool.append(row)
    rng = random.Random(seed)
    strong = [r for r in pool if verdicts.get(r["entry_va"]) == "supported"]
    weak = [r for r in pool if verdicts.get(r["entry_va"]) != "supported"]
    rng.shuffle(strong)
    rng.shuffle(weak)
    return sorted((strong + weak)[:count], key=lambda r: r["entry_va"])


def load_bundles(set_name: str) -> list[dict]:
    return read_jsonl(files(set_name)["bundles"])


def anchor_order(bundle: dict) -> tuple[int, int]:
    """Best anchored first (named callee, string or table), then smallest: cheapest useful rows."""
    anchored = bool(bundle["callees"] or bundle["strings"] or bundle["tables"])
    return (0 if anchored else 1, bundle["size"])


def cmd_propose(args: argparse.Namespace) -> int:
    bundles = load_bundles(args.set)
    out = files(args.set)["proposals"]
    done = {r["va"] for r in read_jsonl(out)}
    todo = [b for b in sorted(bundles, key=anchor_order) if b["va"] not in done]
    if args.limit:
        todo = todo[: args.limit]
    client = make_openai(args)
    hidden = frozenset()
    if args.set == "holdout":
        hidden = frozenset(
            r["name"] for r in json.loads((WORK / "holdout-truth.json").read_text()).values()
        )
    system = PROPOSER_SYSTEM + "\n\n" + vocabulary(read_rows(args.names), exclude=hidden)

    def work(bundle: dict) -> dict:
        result = client.complete(proposer_prompt(bundle), system, PROPOSER_SCHEMA, "name_proposal")
        return {"va": bundle["va"], "proposal": result.parsed, "usd": result.usd}

    run_stage(args, todo, work, out, "propose")
    print(f"process spend ${client.ledger.process_usd:.4f}")
    return 0


def taken_names(args: argparse.Namespace) -> set[str]:
    return {r["name"] for r in read_rows(args.names)}


def hidden_names(set_name: str) -> dict[str, str]:
    """Hold-out VA -> the hidden existing name (its own name is not a duplicate for itself)."""
    path = WORK / "holdout-truth.json"
    if set_name != "holdout" or not path.is_file():
        return {}
    return {va: row["name"] for va, row in json.loads(path.read_text()).items()}


def cmd_verify(args: argparse.Namespace) -> int:
    bundles = {b["va"]: b for b in load_bundles(args.set)}
    paths = files(args.set)
    taken = taken_names(args)
    hidden = hidden_names(args.set)
    done = {r["va"] for r in read_jsonl(paths["verdicts"])}
    todo = []
    for record in read_jsonl(paths["proposals"]):
        proposal = record["proposal"]
        if record["va"] not in bundles:
            continue  # stale proposal from an earlier bundle selection
        if (
            record["va"] in done
            or proposal["unsure"]
            or ground(proposal, bundles[record["va"]], taken - {hidden.get(record["va"])})
        ):
            continue  # unsure or ungrounded rows are rejected without a verifier call
        todo.append(record)
    if args.limit:
        todo = todo[: args.limit]
    client = make_openai(args)

    def work(record: dict) -> dict:
        prompt = verifier_prompt(bundles[record["va"]], record["proposal"])
        result = client.complete(prompt, VERIFIER_SYSTEM, VERIFIER_SCHEMA, "name_verdict")
        return {"va": record["va"], **result.parsed, "usd": result.usd}

    run_stage(args, todo, work, paths["verdicts"], "verify")
    print(f"process spend ${client.ledger.process_usd:.4f}")
    return 0


def cmd_jev(args: argparse.Namespace) -> int:
    from tools.llm.common import Cache, Ledger, read_key
    from tools.llm.jev_client import JevClient, noul

    paths = files(args.set)
    done = {r["va"] for r in read_jsonl(paths["jev"])}
    todo = [
        r
        for r in read_jsonl(paths["proposals"])
        if r["va"] not in done and not r["proposal"]["unsure"]
    ]
    client = JevClient(
        key=read_key(args.jev_key_file),
        concurrency=args.concurrency,
        max_usd=args.max_usd,
        cache=Cache(Path(args.cache_dir), not args.no_cache),
        ledger=Ledger(Path(args.ledger)),
    )

    def work(record: dict) -> dict:
        result = client.ask(jev_state(record["proposal"]), {"b": noul(JEV_QUESTION)})
        return {"va": record["va"], "b": float(result.answers["b"]["noul"])}

    run_stage(args, todo, work, paths["jev"], "jev")
    return 0


def cmd_judge_holdout(args: argparse.Namespace) -> int:
    truth = json.loads((WORK / "holdout-truth.json").read_text())
    paths = files("holdout")
    done = {r["va"] for r in read_jsonl(paths["judge"])}
    todo = [
        r
        for r in read_jsonl(paths["proposals"])
        if r["va"] not in done and r["va"] in truth and not r["proposal"]["unsure"]
    ]
    client = make_openai(args)

    def work(record: dict) -> dict:
        result = client.complete(
            judge_prompt(truth[record["va"]], record["proposal"]),
            JUDGE_SYSTEM,
            JUDGE_SCHEMA,
            "same_role",
        )
        return {"va": record["va"], **result.parsed, "usd": result.usd}

    run_stage(args, todo, work, paths["judge"], "judge")
    return 0


def evaluate(set_name: str, args: argparse.Namespace, threshold: float) -> list[dict]:
    """Join proposals, grounding, verdicts and Jev into one decision per bundle."""
    bundles = {b["va"]: b for b in load_bundles(set_name)}
    paths = files(set_name)
    verdicts = {r["va"]: r for r in read_jsonl(paths["verdicts"])}
    jev = {r["va"]: r["b"] for r in read_jsonl(paths["jev"])}
    taken = taken_names(args)
    hidden = hidden_names(set_name)
    out = []
    for record in read_jsonl(paths["proposals"]):
        va, proposal = record["va"], record["proposal"]
        if va not in bundles:
            continue
        grounding = ground(proposal, bundles[va], taken - {hidden.get(va)})
        verdict = verdicts.get(va, {}).get("verdict")
        ok, reason = accept_decision(proposal, grounding, verdict, threshold, jev.get(va))
        out.append(
            {
                "va": va,
                "proposal": proposal,
                "grounding": grounding,
                "verdict": verdict,
                "verdict_reason": verdicts.get(va, {}).get("reason", ""),
                "jev_b": jev.get(va),
                "accepted": ok,
                "reason": reason,
            }
        )
    return out


def ratio(num: int, den: int) -> str:
    return f"{num}/{den} = {num / den:.2f}" if den else "n/a"


def cmd_report_holdout(args: argparse.Namespace) -> int:
    judge = {r["va"]: r for r in read_jsonl(files("holdout")["judge"])}
    truth = json.loads((WORK / "holdout-truth.json").read_text())
    rows = evaluate("holdout", args, args.threshold)
    proposed = [r for r in rows if not r["proposal"]["unsure"]]
    print(f"holdout rows {len(rows)}, unsure {len(rows) - len(proposed)}, proposed {len(proposed)}")
    ungrounded = [r for r in proposed if r["grounding"]]
    print(f"proposed but ungrounded {len(ungrounded)}")
    judged = [r for r in proposed if r["va"] in judge]
    tally = Counter(judge[r["va"]]["same_role"] for r in judged)
    print(
        f"same role over proposed: {dict(tally)} -> match(yes) {ratio(tally['yes'], len(judged))}, "
        f"yes+partial {ratio(tally['yes'] + tally['partial'], len(judged))}; "
        f"yes over all rows {ratio(tally['yes'], len(rows))}"
    )
    holds = [r for r in judged if r["verdict"] == "holds"]
    holds_no = [r for r in holds if judge[r["va"]]["same_role"] == "no"]
    holds_partial = [r for r in holds if judge[r["va"]]["same_role"] == "partial"]
    print(
        f"verifier holds {len(holds)}: same-role no {len(holds_no)}, partial {len(holds_partial)}"
    )
    print("threshold sweep (accept = grounded + holds + not unsure + confidence >= t):")
    sweep = []
    for t in (0.0, 0.5, 0.6, 0.7, 0.75, 0.8, 0.85, 0.9):
        acc = [r for r in evaluate("holdout", args, t) if r["accepted"] and r["va"] in judge]
        yes = sum(judge[r["va"]]["same_role"] == "yes" for r in acc)
        part = sum(judge[r["va"]]["same_role"] == "partial" for r in acc)
        print(
            f"  t={t:.2f}: accepted {len(acc)} ({ratio(len(acc), len(rows))} of rows), "
            f"yes {ratio(yes, len(acc))}, yes+partial "
            f"{ratio(yes + part, len(acc))}, no {len(acc) - yes - part}"
        )
        sweep.append({"t": t, "accepted": len(acc), "yes": yes, "partial": part})
    by_conf = defaultdict(list)
    for r in judged:
        by_conf[min(int(r["proposal"]["confidence"] * 10), 9) / 10].append(
            judge[r["va"]]["same_role"] == "yes"
        )
    print(
        "yes rate by proposer confidence bin: "
        + ", ".join(f"{b:.1f}:{sum(v)}/{len(v)}" for b, v in sorted(by_conf.items()))
    )
    if args.dump:
        with Path(args.dump).open("w", encoding="utf-8") as handle:
            for r in proposed:
                j = judge.get(r["va"], {})
                t = truth[r["va"]]
                handle.write(
                    json.dumps(
                        {
                            "va": r["va"],
                            "existing": t["name"],
                            "proposed": r["proposal"]["name"],
                            "summary": r["proposal"]["role_summary"],
                            "conf": r["proposal"]["confidence"],
                            "verdict": r["verdict"],
                            "same_role": j.get("same_role"),
                            "reason": j.get("reason"),
                            "grounding": r["grounding"],
                        }
                    )
                    + "\n"
                )
        print(f"dumped to {args.dump}")
    return 0


def cmd_accept(args: argparse.Namespace) -> int:
    rows = evaluate("pilot", args, args.threshold)
    accepted = dedupe_accepts([r for r in rows if r["accepted"]])
    ok_vas = {r["va"] for r in accepted}
    rejected = [r for r in rows if r["va"] not in ok_vas]
    unproposed = {b["va"] for b in load_bundles("pilot")} - {r["va"] for r in rows}
    QUEUE.parent.mkdir(parents=True, exist_ok=True)
    with QUEUE.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            ["entry_va", "proposed_name", "confidence", "verdict", "reason", "role_summary"]
        )
        for r in sorted(rejected, key=lambda r: r["va"]):
            writer.writerow(
                [
                    r["va"],
                    r["proposal"]["name"],
                    r["proposal"]["confidence"],
                    r["verdict"] or "",
                    r["reason"],
                    r["proposal"]["role_summary"],
                ]
            )
        for va in sorted(unproposed):
            writer.writerow([va, "", "", "", "not processed (budget)", ""])
    print(
        f"accepted {len(accepted)} of {len(rows)} proposals; queue {QUEUE} has "
        f"{len(rejected) + len(unproposed)} rows"
    )
    spec = WORK / "accepted-spec.txt"
    lines = []
    for r in sorted(accepted, key=lambda r: r["va"]):
        refs = ", ".join(f"{f['ref']}" for f in r["proposal"]["evidence_facts"])
        claims = "; ".join(
            f"{f['claim'].strip().rstrip('.')}" for f in r["proposal"]["evidence_facts"][:3]
        )
        evidence = (
            f"LLM T1485 proposer gpt-5.5 + verifier hold; facts: {refs}; "
            f"{r['proposal']['role_summary'].strip()} {claims}"
        )
        evidence = " ".join(evidence.replace("|", "/").split())
        lines.append(f"{r['va']}|{r['proposal']['name']}|{evidence}")
    spec.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")
    print(f"spec written to {spec}")
    if args.apply:
        from tools.name_candidates import append_names

        append_names(args.root, spec, "INFERRED", source="docs/t1485-llm-naming-pilot.md")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--names", type=Path, default=NAMES)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--openai-key-file", default=".secrets/openai-api-key.txt")
    parser.add_argument("--jev-key-file", default=".secrets/typesafe-ai.txt")
    parser.add_argument("--max-usd", type=float, default=2.0)
    parser.add_argument("--concurrency", type=int, default=6)
    parser.add_argument("--no-cache", action="store_true")
    parser.add_argument("--cache-dir", default="tmp/llm-cache")
    parser.add_argument("--ledger", default="tmp/t1485/ledger.jsonl")
    parser.add_argument("--model", default="gpt-5.5")
    parser.add_argument("--effort", default="low", choices=["low", "medium", "high"])
    sub = parser.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bundles")
    b.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    b.add_argument("--lo", type=lambda s: int(s, 16), default=0x80000)
    b.add_argument("--hi", type=lambda s: int(s, 16), default=0x100000)
    b.add_argument("--hold-lo", type=lambda s: int(s, 16), default=0x60000)
    b.add_argument("--hold-hi", type=lambda s: int(s, 16), default=0x140000)
    b.add_argument("--holdout", type=int, default=120)
    b.add_argument("--seed", type=int, default=1485)
    b.add_argument(
        "--judged-file",
        type=Path,
        default=None,
        help="T1484 gpt.jsonl, prefers rows it judged supported",
    )
    for name in ("propose", "verify", "jev"):
        p = sub.add_parser(name)
        p.add_argument("--set", choices=["holdout", "pilot"], required=True)
        p.add_argument("--limit", type=int, default=0)
    sub.add_parser("judge-holdout")
    r = sub.add_parser("report-holdout")
    r.add_argument("--threshold", type=float, default=MIN_CONFIDENCE)
    r.add_argument("--dump", default=None)
    a = sub.add_parser("accept")
    a.add_argument("--threshold", type=float, default=CALIBRATED_THRESHOLD)
    a.add_argument("--apply", action="store_true")
    args = parser.parse_args(argv)
    handlers = {
        "bundles": cmd_bundles,
        "propose": cmd_propose,
        "verify": cmd_verify,
        "jev": cmd_jev,
        "judge-holdout": cmd_judge_holdout,
        "report-holdout": cmd_report_holdout,
        "accept": cmd_accept,
    }
    return handlers[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
