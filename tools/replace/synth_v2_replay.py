# SPDX-License-Identifier: GPL-3.0-or-later
"""T1779 frozen v2 original-only replay of the T1773 sets (no gate tuning)."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from multiprocessing import get_context
from pathlib import Path
from typing import Any

from tools.codediff.boundaries import load_function_table
from tools.harness import synth_domain
from tools.harness.image import build_guest_image
from tools.harness.oracle import UnicornOracle
from tools.replace.synth_measure import DEFAULT_SEEDS, load_sample, measure_root, summarize

_STATE: dict[str, Any] = {}


def initialize(xbe: str, census: str, analysis_table: str | None) -> None:
    image = build_guest_image(Path(xbe))
    _STATE.update(
        image=image,
        oracle=UnicornOracle(image.data, max_insns=200000),
        sizes={int(r["va"], 16): int(r["size"]) for r in csv.DictReader(Path(census).open())},
        functions={
            f.entry_va: f.size_bytes
            for f in load_function_table(Path("generated/retail/functions.csv"))
        },
    )
    _STATE["code_ranges"] = None
    if analysis_table:
        pinned = synth_domain.load_analysis_table(Path(analysis_table))
        _STATE["functions"] = pinned["function_sizes"]
        _STATE["code_ranges"] = pinned["code_ranges"]


def measure(va: int) -> tuple[int, dict[str, Any]]:
    image, oracle, size = _STATE["image"], _STATE["oracle"], _STATE["sizes"][va]
    domain = synth_domain.derive(
        va,
        size,
        image,
        generator_version="t1773-synth-v2",
        function_sizes=_STATE["functions"],
        code_ranges=_STATE["code_ranges"],
    )
    per_seed = {}
    for seed in DEFAULT_SEEDS:
        rows = {}
        for variant, synth in (("default", None), ("synth", domain)):
            rows[variant] = measure_root(
                oracle, image, va, size, seed, synth=synth, random_cases=600, max_insns_note=200000
            )
        per_seed[str(seed)] = rows
    return va, {
        "domain_sha256": domain.sha256,
        "seeds": per_seed,
        "analysis_limits": domain.document["summary"]["limits"],
        "analysis_code_sha256": domain.document["analysis_code_sha256"],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--analysis-table", default=None)
    parser.add_argument("--xbe", default="build/default.xbe")
    parser.add_argument("--census-csv", default="tmp/t1772/final/census.csv")
    parser.add_argument("--cores", type=int, default=4, choices=range(1, 5))
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    replay = json.loads(Path("docs/data/t1773-provider-synthesis/replay-set.json").read_text())
    call = [int(r["va"], 16) for r in replay["roots"] if r["group"] == "S" and r["body"] == "call"]
    assert len(call) == 111
    sets = {
        "hand_call": sorted(call),
        "heldout": load_sample("heldout"),
        "development": load_sample("development"),
    }
    census = list(csv.DictReader(Path(args.census_csv).open()))
    sets["fault_call"] = sorted(
        int(r["va"], 16)
        for r in census
        if r["baseline"] == "fault-only" and r["tier"] in ("B-call", "V-vector-call")
    )
    assert len(sets["fault_call"]) == 6
    args.out_dir.mkdir(parents=True, exist_ok=True)
    vas = sorted(set(sum(sets.values(), [])))
    results = {}
    # Save each completed root durably; interruption is a partial replay, never admission.
    with get_context("fork").Pool(
        args.cores,
        initializer=initialize,
        initargs=(args.xbe, args.census_csv, args.analysis_table),
    ) as pool:
        for va, row in pool.imap_unordered(measure, vas):
            key = f"{va:#010x}"
            results[key] = row
            (args.out_dir / f"{va:08x}.json").write_text(
                json.dumps(row, sort_keys=True, indent=1) + "\n"
            )
            print(
                f"{len(results)}/{len(vas)} {key} "
                + str([row["seeds"][str(s)]["synth"]["reach_gate_pass"] for s in DEFAULT_SEEDS]),
                flush=True,
            )
    document = {
        "generator_version": "t1773-synth-v2",
        "seeds": list(DEFAULT_SEEDS),
        "random_cases": 600,
        "max_insns": 200000,
        "xbe_sha256": hashlib.sha256(Path(args.xbe).read_bytes()).hexdigest(),
        "sets": {
            name: {
                "vas": [f"{v:#010x}" for v in entries],
                "summary": summarize(
                    {f"{v:#010x}": results[f"{v:#010x}"] for v in entries}, DEFAULT_SEEDS
                ),
            }
            for name, entries in sets.items()
        },
        "results": results,
    }
    (args.out_dir / "results.json").write_text(
        json.dumps(document, sort_keys=True, indent=1) + "\n"
    )
    print(json.dumps(document["sets"], indent=1), flush=True)


if __name__ == "__main__":
    main()
