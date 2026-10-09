# SPDX-License-Identifier: GPL-3.0-or-later
"""T1478 appendix: EXACT caller audit and original-only baseline screen.

Runs only `UnicornOracle` on the original bytes (no subject, replacement or fixed proof
tuple), so it consumes zero fixed tuples. For each row of the census appendix it records
the EXACT caller audit (empty scratch tuple, EAX return, the row's declared register
inputs) and, for both fixed seeds, how many of the 600 random cases, the argument-edge
cases and the provider cases return at least once. A root for which no case returns in
either seed is "fixture-domain fault-only" and is rejected without spending a tuple.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
from pathlib import Path

from tools.harness.image import build_guest_image
from tools.harness.oracle import UnicornOracle
from tools.harness.providers import iter_provider_cases
from tools.harness.seeding import edge_selectors, generate_cases, make_edge_case
from tools.replace.manifest import ManifestEntry
from tools.t1772_replacement_census import caller_audit_rows

SEEDS = (20261001, 20261006)
DEFAULT_LIST = "docs/data/t1772-replacement-census/band-T1478/appendix-previously-mentioned.json"


def parse_va(text: str) -> int:
    return int(text, 16)


def entry_for(row: dict) -> ManifestEntry:
    abi = row["abi"]
    inputs = tuple(abi.get("register_inputs") or ())
    return ManifestEntry(
        parse_va(row["va"]),
        "hypothetical",
        abi["convention"],
        abi["stack_args"],
        "eax",
        (),
        "hypothetical.c",
        inputs or None,
    )


def screen_seed(oracle: UnicornOracle, row: dict, seed: int) -> dict:
    va, size = parse_va(row["va"]), row["size"]
    abi = row["abi"]
    inputs = tuple(abi.get("register_inputs") or ())
    counts: collections.Counter = collections.Counter()
    faults: collections.Counter = collections.Counter()
    reached: set[int] = set()
    slots = abi["stack_args"] + len(inputs)
    cases = [("random", c) for c in generate_cases(seed, [(va, size)], 600)]
    cases += [
        (
            "edge",
            make_edge_case(
                seed,
                ordinal,
                va,
                size,
                register_args=len(inputs),
                stack_args=abi["stack_args"],
                register_names=inputs or None,
                selector=selector,
            ),
        )
        for ordinal, selector in enumerate(edge_selectors(slots, va))
    ]
    cases += list(iter_provider_cases(seed, va, size))
    for kind, case in cases:
        outcome = oracle.run(case)
        counts[kind] += 1
        if outcome.faulted:
            faults[outcome.fault] += 1
        else:
            counts["returned"] += 1
        reached.update(outcome.reach.covered_vas)
    return {
        "seed": seed,
        "counts": dict(counts),
        "faults": dict(faults),
        "reached_instructions": len(reached),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", default=DEFAULT_LIST, help="census list/appendix JSON")
    parser.add_argument("--xbe", default="build/default.xbe")
    parser.add_argument("--gen-dir", default="generated/lifted/gen")
    parser.add_argument("--va", action="append", default=[], help="restrict to these VAs")
    parser.add_argument("--out", required=True, help="receipt JSON path")
    parser.add_argument("--audit-only", action="store_true", help="skip the Unicorn baseline")
    args = parser.parse_args()
    document = json.loads(Path(args.list).read_text())
    rows = document["functions"] if isinstance(document, dict) else document
    wanted = {parse_va(v) for v in args.va}
    if wanted:
        rows = [r for r in rows if parse_va(r["va"]) in wanted]
    entries = [entry_for(r) for r in rows]
    audits = caller_audit_rows(entries, Path(args.gen_dir), Path(args.xbe))
    image = build_guest_image(Path(args.xbe))
    oracle = None if args.audit_only else UnicornOracle(image.data)
    records = []
    for row in rows:
        va = parse_va(row["va"])
        record = {
            "va": row["va"],
            "abi": row["abi"],
            "tier": row["tier"],
            "caller_audit": audits[va],
            "body_sha256": hashlib.sha256(image.code_at(va, row["size"])).hexdigest(),
            "seeds": [],
            "fixed_tuples_consumed": 0,
        }
        if oracle is not None and audits[va]["ok"]:
            record["seeds"] = [screen_seed(oracle, row, seed) for seed in SEEDS]
            returned = [s["counts"].get("returned", 0) for s in record["seeds"]]
            record["status"] = "FAULT-ONLY" if not any(returned) else "BASELINE-RETURNED"
        else:
            record["status"] = "AUDIT-REFUSED" if not audits[va]["ok"] else "AUDIT-ONLY"
        records.append(record)
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(records, indent=2) + "\n")
        returned = [s["counts"].get("returned", 0) for s in record["seeds"]]
        print(row["va"], record["status"], audits[va]["sites"], returned, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
