# SPDX-License-Identifier: GPL-3.0-or-later
"""Archive a completed fixed batch and copy admitted bodies without repairing them."""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
from pathlib import Path

from tools.replace.scan import scan_text


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("batch", type=int, choices=(1, 2, 3))
    args = parser.parse_args()
    batch = args.batch
    root = Path("docs/data/t1478-census")
    scratch = Path(f"tmp/bp{batch:03}")
    results = json.loads((scratch / "results.json").read_text())
    for suffix in ("json", "md"):
        shutil.copyfile(scratch / f"results.{suffix}", root / f"list{batch:03}-results.{suffix}")
    statuses = json.loads((root / "candidate-status.json").read_text())
    by_va = {r["va"]: r for r in statuses}
    for row in results["functions"]:
        record = by_va[row["va"]]
        runs = [r for r in results["tuples"] if r["va"] == row["va"]]
        assert len(runs) == 6
        assert {(r["seed"], r["opt"], r["cases"]) for r in runs} == {
            (seed, opt, 600) for seed in (20261001, 20261006) for opt in (0, 2, 3)
        }
        record["status"] = row["outcome"]
        record["fixed_tuples_consumed"] = 6
        record["tuples"] = runs
        failures = set(g for r in runs for g in r.get("failing", []))
        record["rejection_class"] = (
            None
            if row["outcome"] == "ADMITTED"
            else (
                "null-agree" if "null_agree" in failures else "other: " + ",".join(sorted(failures))
            )
        )
    (root / "candidate-status.json").write_text(json.dumps(statuses, indent=2) + "\n")
    draft_path = root / f"list{batch:03}-draft.c.txt"
    draft = draft_path.read_text()
    registrations = scan_text(draft, draft_path.name)
    names = {}
    with Path("tools/data/function_names.csv").open() as handle:
        for row in csv.DictReader(handle):
            names[int(row["entry_va"], 16)] = row["name"]
    chunks = re.split(r"(?=GAME_REPLACE_EXACT\()", draft)
    source = chunks[0]
    integrated = []
    for body, registration in zip(chunks[1:], registrations, strict=True):
        va = registration.va
        if by_va[f"0x{va:08x}"]["status"] != "ADMITTED":
            continue
        name = names.get(va, registration.function)
        assert re.fullmatch(r"[A-Za-z_]\w*", name)
        source += body.replace(registration.function, name, 1)
        integrated.append(dict(va=f"0x{va:08x}", draft_name=registration.function, name=name))
    # Unused local helpers are removed, never a body statement changed.
    for helper in ("guest_read16", "guest_write16"):
        if source.count(helper) == 1:
            source = re.sub(r"static [^\n]*\b" + helper + r"\([^\n]*\n", "", source)
    output = Path(f"src/game/game_census_180000_list{batch:03}.c")
    if integrated:
        output.write_text(source)
    (root / f"list{batch:03}-integration.json").write_text(json.dumps(integrated, indent=2) + "\n")
    print("archived", batch, "integrated", integrated, flush=True)


if __name__ == "__main__":
    main()
