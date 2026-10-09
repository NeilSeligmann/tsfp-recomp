# SPDX-License-Identifier: GPL-3.0-or-later
"""Original-only baseline screen; no subject, replacement or fixed proof tuple."""

from __future__ import annotations

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


def main() -> None:
    root = Path("docs/data/t1478-census")
    root.mkdir(parents=True, exist_ok=True)
    rows = []
    for batch in range(1, 4):
        doc = json.loads(
            Path(f"docs/data/t1772-replacement-census/band-T1478/list-{batch:03}.json").read_text()
        )
        assert doc["count"] == len(doc["functions"]) == 25
        for row in doc["functions"]:
            assert row["abi"]["register_inputs"] == []
            rows.append(dict(row, batch=batch))
    entries = [
        ManifestEntry(
            int(r["va"], 16),
            "hypothetical",
            r["abi"]["convention"],
            r["abi"]["stack_args"],
            "eax",
            (),
            "hypothetical.c",
        )
        for r in rows
    ]
    audits = caller_audit_rows(entries, Path("generated/lifted/gen"), Path("build/default.xbe"))
    image = build_guest_image(Path("build/default.xbe"))
    oracle = UnicornOracle(image.data)
    records = []
    for row in sorted(
        rows, key=lambda r: (not r["executed_in_owner_sessions"], r["batch"], r["va"])
    ):
        va, size = int(row["va"], 16), row["size"]
        result = dict(
            va=row["va"],
            batch=row["batch"],
            abi=row["abi"],
            executed_in_owner_sessions=row["executed_in_owner_sessions"],
            caller_audit=audits[va],
            body_sha256=hashlib.sha256(image.code_at(va, size)).hexdigest(),
            seeds=[],
            fixed_tuples_consumed=0,
        )
        assert audits[va]["ok"], result
        for seed in (20261001, 20261006):
            counts = collections.Counter()
            faults = collections.Counter()
            reached = set()
            cases = [("random", c) for c in generate_cases(seed, [(va, size)], 600)]
            cases += [
                (
                    "edge",
                    make_edge_case(
                        seed,
                        n,
                        va,
                        size,
                        register_args=0,
                        stack_args=row["abi"]["stack_args"],
                        selector=s,
                    ),
                )
                for n, s in enumerate(edge_selectors(row["abi"]["stack_args"], va))
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
            result["seeds"].append(
                dict(
                    seed=seed,
                    counts=dict(counts),
                    faults=dict(faults),
                    reached_instructions=len(reached),
                )
            )
        result["status"] = (
            "REJECTED"
            if all(s["counts"].get("returned", 0) == 0 for s in result["seeds"])
            else "BASELINE-RETURNED"
        )
        result["rejection_class"] = (
            "fixture-domain fault-only" if result["status"] == "REJECTED" else None
        )
        records.append(result)
        (root / "list123-baseline.json").write_text(json.dumps(records, indent=2) + "\n")
        print(
            row["va"],
            result["status"],
            [s["counts"].get("returned", 0) for s in result["seeds"]],
            flush=True,
        )


if __name__ == "__main__":
    main()
