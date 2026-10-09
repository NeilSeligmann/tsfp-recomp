# SPDX-License-Identifier: GPL-3.0-or-later
"""Jev subsystem map of every game function (T1504).

    python -m tools.jev_subsystem_map bundles
    python -m tools.jev_subsystem_map classify --key-file KEY --max-usd 1.0 [--limit N]
    python -m tools.jev_subsystem_map report
    python -m tools.jev_subsystem_map inspect --va 0x00012380 [--va ...]
    python -m tools.jev_subsystem_map sample --n-confident 20 --n-low 20 --seed 1504

`bundles` builds one compact evidence text per game function (named callees with counts,
named callers, referenced strings, kernel imports, name prefixes of the other users of its
globals, function table neighbours). The function's own name is kept in a separate field
and is never part of the text sent to Jev, so the prediction is independent of it.
`classify` asks ONE Jev choice question per function (fixed subsystem list, deterministic
option shuffle, then the reversed order via `JevClient.ask(order_check=True)`). `report`
writes the per-function CSV, size tables, the named-vs-prefix mismatches and the unnamed
work queue. Work files live in gitignored tmp/t1504, queues in generated/. Read-only on
function_names.csv. The key file is only ever passed as a path to tools.llm.
"""

from __future__ import annotations

import argparse
import csv
import json
import random
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

WORK = Path("tmp/t1504")
NAMES = Path("tools/data/function_names.csv")
RESULT_CSV = WORK / "subsystem-map.csv"
QUEUE_CSV = Path("generated/t1504-unnamed-by-subsystem.csv")
MISMATCH_ALL = Path("generated/t1504-subsystem-mismatches.csv")
MISMATCH_DOC = Path("docs/data/t1504-subsystem-mismatches.csv")
MISMATCH_DOC_MAX_LINES = 300
CONFIDENT_P = 0.8

# Subsystem -> short description shown to Jev next to the option name.
SUBSYSTEMS: dict[str, str] = {
    "hud": "in-game heads-up display, ammo, radar",
    "menu": "front end menus and pages",
    "ui_text": "UI widgets, fonts, text drawing",
    "net_online": "networking, Xbox Live, packets, clans",
    "session": "game session, rules, modes, teams, players",
    "anim": "skeletal animation and blending",
    "object": "generic game objects and entity lists",
    "weapon": "weapons, firing, reload, damage",
    "projectile": "bullets, grenades, missiles in flight",
    "particle": "particle systems and emitters",
    "effect": "visual effects, explosions, decals, lights",
    "weather": "weather, rain, snow, sky",
    "physics": "rigid body physics, forces, vehicles",
    "collision": "collision detection, ray casts, hit tests",
    "ai": "computer controlled characters, pathfinding",
    "audio": "sound, music, voices",
    "save": "saving, loading, profiles, storage",
    "input": "controller input, buttons, sticks",
    "mapedit": "the in-game map editor",
    "scoreboard": "scores, statistics, rankings",
    "level_script": "level scripts, triggers, cutscenes",
    "init_loader": "startup, loading levels and assets",
    "d3d_render": "Direct3D rendering, meshes, textures, camera",
    "math": "vectors, matrices, trigonometry, random numbers",
    "memory_alloc": "memory allocation, pools, copying memory",
    "string_text": "string handling, formatting, parsing",
    "other": "none of the above or unclear",
}

# game_<prefix>_ -> subsystems that the prefix is allowed to be predicted as. A prefix that
# is a generic noun (get, set, table, global, player, char, scene, mode, static, ...) is NOT
# listed: no subsystem is implied, so no mismatch is ever raised for it.
_BROAD_OBJECT = {
    "object", "anim", "weapon", "projectile", "particle", "effect", "weather", "physics",
    "collision", "ai", "level_script", "init_loader", "session", "other", "math",
}  # fmt: skip
PREFIX_TO_SUBSYSTEMS: dict[str, frozenset[str]] = {
    p: frozenset(s)
    for p, s in {
        "hud": {"hud", "ui_text"},
        "menu": {"menu", "ui_text", "session", "save", "net_online", "scoreboard", "input"},
        "ui": {"ui_text", "menu", "hud", "scoreboard", "input"},
        "net": {"net_online", "session"},
        "online": {"net_online", "session"},
        "fesl": {"net_online", "session"},
        "clan": {"net_online", "session", "menu"},
        "sound": {"audio"},
        "audio": {"audio"},
        "effect": {"effect", "particle", "weather", "d3d_render"},
        "particle": {"particle", "effect", "d3d_render"},
        "decal": {"effect", "particle", "d3d_render"},
        "anim": {"anim", "object"},
        "weapon": {"weapon", "projectile", "object"},
        "weather": {"weather", "effect", "particle"},
        "collision": {"collision", "physics"},
        "hit": {"collision", "weapon", "physics", "projectile"},
        "save": {"save", "init_loader"},
        "input": {"input"},
        "mapedit": {"mapedit"},
        "scoreboard": {"scoreboard", "hud"},
        "level": {"level_script", "init_loader", "object", "session"},
        "script": {"level_script", "object"},
        "cutscene": {"level_script", "anim", "d3d_render"},
        "launch": {"init_loader", "session"},
        "pak": {"init_loader", "save"},
        "d3d": {"d3d_render"},
        "render": {"d3d_render", "effect"},
        "texture": {"d3d_render"},
        "math": {"math"},
        "matrix4x4": {"math"},
        "memory": {"memory_alloc"},
        "alloc": {"memory_alloc"},
        "pool": {"memory_alloc", "object"},
        "text": {"string_text", "ui_text"},
        "ai": {"ai", "object"},
        "object": _BROAD_OBJECT,
    }.items()
}
GENERIC_CALLEES = {"game_ftol_adjust", "__floor_default", "__ceil_default", "_sprintf"}
MAX_CALLEES = 12
MAX_CALLERS = 12
MAX_STRINGS = 4
MAX_GLOBAL_PREFIXES = 6
MAX_TABLE_NEIGHBOURS = 4
QUESTION_TEXT = (
    "Which subsystem of the game does the function described in the state belong to? "
    "Judge from the names of the functions it calls and is called by, its strings, "
    "kernel imports, the name prefixes of other users of its globals and the function "
    "table it sits in."
)


def prefix_of(name: str) -> str:
    """First vocabulary token: `game_hud_draw` -> `game_hud`, `memcpy` -> `memcpy`."""
    tokens = name.split("_")
    if tokens[0] == "game" and len(tokens) > 1:
        return f"game_{tokens[1]}"
    return tokens[0]


def prefix_family(name: str) -> str | None:
    """The `<x>` of `game_<x>_...`, or None when the name has no game_ prefix."""
    tokens = name.split("_")
    return tokens[1] if tokens[0] == "game" and len(tokens) > 2 else None


def acceptable_subsystems(name: str) -> frozenset[str] | None:
    """Subsystems that the name's prefix family allows, None when the prefix implies none."""
    family = prefix_family(name)
    return PREFIX_TO_SUBSYSTEMS.get(family) if family else None


def counted(names: list[str], limit: int) -> str:
    """`a x3, b` from a list with repeats: most frequent first, then alphabetical."""
    tally = Counter(names)
    ranked = sorted(tally.items(), key=lambda item: (-item[1], item[0]))[:limit]
    return ", ".join(name if count == 1 else f"{name} x{count}" for name, count in ranked)


def make_bundle(
    va: int,
    size: int,
    own_name: str,
    callee_names: list[str],
    caller_names: list[str],
    strings: list[str],
    kernel: list[str],
    global_user_names: list[str],
    table: dict | None,
) -> dict:
    """Plain-data bundle. `own_name` is stored but `bundle_state` never uses it."""
    return {
        "va": f"0x{va:08x}",
        "size": size,
        "own_name": own_name,
        "callees": [n for n in callee_names if n not in GENERIC_CALLEES],
        "callers": sorted(set(caller_names)),
        "strings": [s[:40] for s in strings[:MAX_STRINGS]],
        "kernel": sorted(set(kernel)),
        "global_users": [prefix_of(n) for n in global_user_names],
        "table": table,
    }


def bundle_is_empty(bundle: dict) -> bool:
    keys = ("callees", "callers", "strings", "kernel", "global_users")
    if any(bundle[key] for key in keys):
        return False
    table = bundle["table"]
    return not (table and table["neighbours"])


def bundle_state(bundle: dict) -> str:
    """The text Jev sees. Never contains the function's own name or its address."""
    lines = ["A function from an Xbox game, decompiled without symbols."]
    if bundle["callees"]:
        lines.append("Named callees: " + counted(bundle["callees"], MAX_CALLEES))
    if bundle["callers"]:
        shown = bundle["callers"][:MAX_CALLERS]
        extra = len(bundle["callers"]) - len(shown)
        lines.append("Named callers: " + ", ".join(shown) + (f" (+{extra} more)" if extra else ""))
    if bundle["kernel"]:
        lines.append("Kernel imports: " + ", ".join(bundle["kernel"]))
    if bundle["strings"]:
        lines.append("Strings: " + " | ".join(bundle["strings"]))
    if bundle["global_users"]:
        lines.append(
            "Other functions using its globals, by name prefix: "
            + counted(bundle["global_users"], MAX_GLOBAL_PREFIXES)
        )
    table = bundle["table"]
    if table and table["neighbours"]:
        lines.append(
            f"Entry {table['slot'] + 1} of {table['count']} in a function pointer table, "
            "neighbouring entries: " + ", ".join(table["neighbours"])
        )
    return "\n".join(lines)


def subsystem_question() -> dict:
    from tools.llm.jev_client import choice

    return choice(QUESTION_TEXT, SUBSYSTEMS)


def classify_record(bundle: dict, answer: dict) -> dict:
    """Condense one Jev choice answer (with order_check fields) into a result row."""
    probs = answer.get("probabilities_mean") or answer.get("probabilities") or {}
    ranked = sorted(probs.items(), key=lambda item: (-item[1], item[0]))
    top, p = ranked[0] if ranked else ("other", 0.0)
    second, p2 = ranked[1] if len(ranked) > 1 else ("", 0.0)
    return {
        "va": bundle["va"],
        "predicted": top,
        "p": round(p, 4),
        "second": second,
        "p2": round(p2, 4),
        "confidence": round(float(answer.get("confidence", 0.0)), 4),
        "order_consistent": bool(answer.get("order_consistent", False)),
        "choice_forward": answer.get("choice", ""),
        "p_forward": round((answer.get("probabilities") or {}).get(top, 0.0), 4),
        "p_swapped": round((answer.get("probabilities_swapped") or {}).get(top, 0.0), 4),
    }


def classify_one(client: Any, bundle: dict, seed: int) -> dict:
    """One request pair (forward and reversed option order) for one function."""
    qid = f"sub_{bundle['va']}"
    result = client.ask(bundle_state(bundle), {qid: subsystem_question()}, seed, order_check=True)
    return {**classify_record(bundle, result.answers[qid]), "usd": result.usd}


def is_confident(row: dict, min_p: float = CONFIDENT_P) -> bool:
    return bool(row["order_consistent"]) and float(row["p"]) >= min_p


def is_mismatch(row: dict, own_name: str, min_p: float = CONFIDENT_P) -> bool:
    """A confident prediction outside what the name's prefix family allows."""
    allowed = acceptable_subsystems(own_name)
    if not allowed or not is_confident(row, min_p):
        return False
    return row["predicted"] not in allowed


def read_jsonl(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    lines = path.read_text(encoding="utf-8").splitlines()
    return [json.loads(line) for line in lines if line.strip()]


def read_overlay(path: Path = NAMES) -> dict[int, str]:
    with path.open(newline="", encoding="utf-8") as handle:
        return {int(r["entry_va"], 16): r["name"] for r in csv.DictReader(handle)}


def cmd_bundles(args: Any) -> int:
    from tools import name_graph, name_tables

    image, funcs, known, game_unnamed, info, callers, owners, _md = name_graph.build(
        args.root, args.xbe
    )
    overlay = read_overlay(args.root / NAMES)
    game = set(game_unnamed) | {va for va, n in overlay.items() if n.startswith("game_")}
    member_of: dict[int, tuple[int, int, list[int]]] = {}
    for table_va, members in name_tables.scan_tables(image, funcs, 2):
        for slot, member in enumerate(members):
            member_of.setdefault(member, (table_va, slot, members))
    WORK.mkdir(parents=True, exist_ok=True)
    skipped = 0
    written = 0
    with (WORK / "bundles.jsonl").open("w", encoding="utf-8") as out:
        for va in sorted(game & set(funcs)):
            calls, globs, kern, strs = info[va]
            users: set[int] = set()
            for glob in globs:
                users |= {o for o in owners[glob] if o != va and o in known}
            table = None
            if va in member_of:
                table_va, slot, members = member_of[va]
                near = sorted(
                    (m for m in set(members) if m != va and m in known),
                    key=lambda m: (abs(members.index(m) - slot), m),
                )[:MAX_TABLE_NEIGHBOURS]
                table = {
                    "table": f"0x{table_va:08x}",
                    "slot": slot,
                    "count": len(members),
                    "neighbours": [known[m] for m in near],
                }
            bundle = make_bundle(
                va,
                funcs[va],
                overlay.get(va, ""),
                [known[c] for c in calls if c in known],
                [known[c] for c in callers.get(va, ()) if c in known],
                strs,
                list(kern),
                [known[u] for u in users],
                table,
            )
            bundle["empty"] = bundle_is_empty(bundle)
            skipped += bundle["empty"]
            written += 1
            out.write(json.dumps(bundle) + "\n")
    print(f"wrote {written} game bundles ({skipped} empty) to {WORK / 'bundles.jsonl'}")
    return 0


def cmd_classify(args: Any) -> int:
    from tools.llm.common import Cache, Ledger, LlmError, read_key, redact
    from tools.llm.jev_client import JevClient

    bundles = [b for b in read_jsonl(WORK / "bundles.jsonl") if not b["empty"]]
    done = {r["va"] for r in read_jsonl(WORK / "results.jsonl")}
    todo = [b for b in bundles if b["va"] not in done]
    if args.limit:
        todo = todo[: args.limit]
    client = JevClient(
        key=read_key(args.key_file) if not args.dry_run else "dry-run-key",
        concurrency=args.concurrency,
        max_usd=args.max_usd,
        dry_run=args.dry_run,
        cache=Cache(Path(args.cache_dir), not args.no_cache),
        ledger=Ledger(Path(args.ledger)),
    )
    if args.dry_run:
        tokens = sum(
            client.ask(bundle_state(b), {"q": subsystem_question()}).input_tokens for b in todo
        )
        print(
            f"dry run: {len(todo)} functions, about {tokens} input tokens per pass, x2 for the swap"
        )
        return 0
    failure = ""
    count = 0

    def run(bundle: dict) -> dict | None:
        nonlocal failure
        if failure:
            return None
        try:
            return classify_one(client, bundle, args.seed)
        except LlmError as exc:
            failure = f"{type(exc).__name__}: {redact(exc)}"
            return None

    with (
        (WORK / "results.jsonl").open("a", encoding="utf-8") as out,
        ThreadPoolExecutor(args.concurrency) as pool,
    ):
        for record in pool.map(run, todo):
            if record is not None:
                out.write(json.dumps(record) + "\n")
                count += 1
    print(f"classified {count} of {len(todo)}, process spend ${client.ledger.process_usd:.4f}")
    if failure:
        print(f"STOPPED: {failure}", file=sys.stderr)
        return 3 if "GLOBAL" in failure else 2
    return 0


def merged_rows() -> list[dict]:
    bundles = {b["va"]: b for b in read_jsonl(WORK / "bundles.jsonl")}
    results = {r["va"]: r for r in read_jsonl(WORK / "results.jsonl")}
    rows = []
    for va, bundle in sorted(bundles.items()):
        row = {
            "entry_va": va,
            "size": bundle["size"],
            "own_name": bundle["own_name"],
            "named": int(bool(bundle["own_name"])),
            "status": "skipped_empty" if bundle["empty"] else "no_result",
            "predicted": "",
            "p": "",
            "second": "",
            "p2": "",
            "confidence": "",
            "order_consistent": "",
            "p_forward": "",
            "p_swapped": "",
        }
        if va in results:
            row.update({k: results[va][k] for k in results[va] if k in row})
            row["status"] = "ok"
        rows.append(row)
    return rows


def size_table(rows: list[dict]) -> list[tuple[str, int, int, int]]:
    """(subsystem, all, confident, consistent-only) counts for classified rows."""
    ok = [r for r in rows if r["status"] == "ok"]
    out = []
    for name in sorted(SUBSYSTEMS):
        mine = [r for r in ok if r["predicted"] == name]
        confident = [r for r in mine if is_confident(r)]
        consistent = [r for r in mine if r["order_consistent"]]
        out.append((name, len(mine), len(confident), len(consistent)))
    return sorted(out, key=lambda item: -item[1])


def write_csv(path: Path, header: list[str], records: list[list]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(header)
        writer.writerows(records)


def find_mismatches(rows: list[dict], min_p: float = CONFIDENT_P) -> list[dict]:
    return [
        r
        for r in rows
        if r["status"] == "ok" and r["named"] and is_mismatch(r, r["own_name"], min_p)
    ]


def cmd_report(args: Any) -> int:
    rows = merged_rows()
    header = list(rows[0]) if rows else []
    write_csv(RESULT_CSV, header, [[r[k] for k in header] for r in rows])
    ok = [r for r in rows if r["status"] == "ok"]
    named = [r for r in ok if r["named"]]
    unnamed = [r for r in ok if not r["named"]]
    print(
        f"game functions {len(rows)}, classified {len(ok)}, "
        f"skipped empty {sum(r['status'] == 'skipped_empty' for r in rows)}, "
        f"named {len(named)}, unnamed {len(unnamed)}"
    )
    print("subsystem sizes (all / confident / unnamed / unnamed confident):")
    all_t = {n: (a, c) for n, a, c, _ in size_table(ok)}
    un_t = {n: (a, c) for n, a, c, _ in size_table(unnamed)}
    for name in sorted(all_t, key=lambda n: -all_t[n][0]):
        print(
            f"  {name:13} {all_t[name][0]:5} {all_t[name][1]:5} {un_t[name][0]:5} {un_t[name][1]:5}"
        )
    print(
        f"order consistent: {sum(r['order_consistent'] for r in ok)} of {len(ok)}, "
        f"confident (p>={CONFIDENT_P}): {sum(is_confident(r) for r in ok)}"
    )
    queue = sorted(
        (r for r in unnamed if is_confident(r)),
        key=lambda r: (r["predicted"], -r["p"], r["entry_va"]),
    )
    write_csv(
        QUEUE_CSV,
        ["subsystem", "entry_va", "size_bytes", "p", "confidence", "second", "p2"],
        [
            [
                r["predicted"],
                r["entry_va"],
                r["size"],
                r["p"],
                r["confidence"],
                r["second"],
                r["p2"],
            ]
            for r in queue
        ],
    )
    mism = find_mismatches(ok)
    mapped_named = [r for r in named if acceptable_subsystems(r["own_name"])]
    print(f"named with a mapped prefix: {len(mapped_named)}, confident mismatches: {len(mism)}")
    mismatch_header = ["entry_va", "own_name", "prefix_allows", "predicted", "p", "second", "p2"]
    mismatch_rows = [
        [
            r["entry_va"],
            r["own_name"],
            "/".join(sorted(acceptable_subsystems(r["own_name"]) or ())),
            r["predicted"],
            r["p"],
            r["second"],
            r["p2"],
        ]
        for r in sorted(mism, key=lambda r: r["entry_va"])
    ]
    write_csv(MISMATCH_ALL, mismatch_header, mismatch_rows)
    if len(mismatch_rows) + 1 < MISMATCH_DOC_MAX_LINES:
        write_csv(MISMATCH_DOC, mismatch_header, mismatch_rows)
        print(f"wrote {MISMATCH_DOC} ({len(mismatch_rows)} rows)")
    else:
        print(
            f"mismatch list too long for docs ({len(mismatch_rows)} rows), kept in {MISMATCH_ALL}"
        )
    pairs = Counter((prefix_family(r["own_name"]), r["predicted"]) for r in mism)
    print("top mismatch patterns (prefix, predicted):", pairs.most_common(12))
    print(f"wrote {RESULT_CSV}, {QUEUE_CSV} ({len(queue)} rows)")
    return 0


def cmd_inspect(args: Any) -> int:
    bundles = {b["va"]: b for b in read_jsonl(WORK / "bundles.jsonl")}
    results = {r["va"]: r for r in read_jsonl(WORK / "results.jsonl")}
    for text in args.va:
        va = f"0x{int(text, 16):08x}"
        bundle = bundles[va]
        res = results.get(va, {})
        print(
            f"== {va} own_name={bundle['own_name'] or '-'} size={bundle['size']} "
            f"pred={res.get('predicted')} p={res.get('p')} consistent={res.get('order_consistent')}"
        )
        print(bundle_state(bundle))
    return 0


def cmd_sample(args: Any) -> int:
    rows = [r for r in merged_rows() if r["status"] == "ok"]
    rng = random.Random(args.seed)
    confident = [r for r in rows if is_confident(r)]
    low = [r for r in rows if not is_confident(r)]
    picked = rng.sample(confident, args.n_confident) + rng.sample(low, args.n_low)
    for row in picked:
        print(f"{row['entry_va']} {'C' if is_confident(row) else 'L'}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bundles")
    b.add_argument("--root", type=Path, default=Path("."))
    b.add_argument("--xbe", type=Path, default=Path("build/default.xbe"))
    c = sub.add_parser("classify")
    c.add_argument("--key-file", default=".secrets/typesafe-ai.txt")
    c.add_argument("--max-usd", type=float, default=1.0)
    c.add_argument("--concurrency", type=int, default=8)
    c.add_argument("--seed", type=int, default=1504)
    c.add_argument("--limit", type=int, default=0)
    c.add_argument("--dry-run", action="store_true")
    c.add_argument("--no-cache", action="store_true")
    c.add_argument("--cache-dir", default="tmp/llm-cache")
    c.add_argument("--ledger", default="tmp/llm-usage.jsonl")
    sub.add_parser("report")
    i = sub.add_parser("inspect")
    i.add_argument("--va", action="append", required=True)
    s = sub.add_parser("sample")
    s.add_argument("--n-confident", type=int, default=20)
    s.add_argument("--n-low", type=int, default=20)
    s.add_argument("--seed", type=int, default=1504)
    args = parser.parse_args(argv)
    return {
        "bundles": cmd_bundles,
        "classify": cmd_classify,
        "report": cmd_report,
        "inspect": cmd_inspect,
        "sample": cmd_sample,
    }[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
