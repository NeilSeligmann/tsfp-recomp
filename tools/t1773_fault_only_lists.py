# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the T1773 fault-only work lists (batches of 25 per census band).

Input (all relative to the repository root):

* `tmp/t1772/final/census.csv` and `rows.json` (T1772 census scratch output, not tracked).
* `docs/data/t1773-fault-only-lists/campaign-history.json` (tracked): the per-root campaign status
  read from the Codex and Claude campaign records, one row per census fault-only root.
* `docs/data/t1773-fault-only-lists/probe-faults.json` (tracked): the T1773 probe of the default
  stream, 100 cases of seed 20261001 per root, top oracle fault sites.
* `docs/data/t1773-provider-synthesis/{heldout,development}-results.json` and `sample-split.json`.

A root is listed unless it is registered or proven, was rejected after a verdict-producing run,
or is
one of the four held-out roots that no provider can lift. Everything else, including roots rejected
as fixture-domain fault-only (0 verdicts), stays in. A rejection whose every fixed tuple
stayed below
the 100-verdict floor while the oracle faulted (`fixture_domain_verdict_floor`, the campaign label
"fixture-domain verdict floor") is the same fixture-domain class and stays in; `--strict-literal`
excludes those too. Lists are sorted by VA inside a band.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

HELD_OUT_FAILURES = {
    0x36FC0: (
        "callee-returned allocator pointer (call 0x3e440 then rep stosd), "
        "0 verdicts under the synthesized domain"
    ),
    0x69790: (
        "null-page read makes 2 of 17 instructions unreachable for any input, "
        "coverage ceiling 0.8824"
    ),
    0x321C00: "loop gated by a field the function zeroed just before, coverage ceiling 0.8475",
    0x36DBD0: "null-agree 0.943 on seed 20261006 (a property of the original), near the 0.9 line",
}
EXCLUDING_STATUSES = {"PROVEN_OR_REGISTERED", "REJECTED_AFTER_VERDICTS"}
BATCH = 25
DATA = "docs/data/t1773-fault-only-lists"


def load_json(path: str) -> object:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def synth_reach(results: dict, va: int) -> dict | None:
    row = results.get(f"0x{va:08x}")
    if row is None:
        return None
    out: dict = {"domain_sha256": row["domain_sha256"], "seeds": {}}
    for seed, pair in row["seeds"].items():
        synth = pair["synth"]
        out["seeds"][seed] = {
            "verdicts": synth["verdicts"],
            "coverage": synth["coverage"],
            "null_agree": synth["null_agree_rate"],
            "reach_gate_pass": synth["reach_gate_pass"],
            "failing": synth["failing"],
            "default_verdicts": pair["default"]["verdicts"],
        }
    return out


def fault_site(probe: dict | None) -> dict | None:
    if not probe or not probe["sites"]:
        return None
    eip, address, count = probe["sites"][0]
    return {
        "source": "T1773 probe, 100 default cases, seed 20261001, max_insns 20000",
        "eip": eip,
        "fault_address": address,
        "cases_of_100": count,
        "fault_classes": probe["faults"],
    }


def build(args: argparse.Namespace) -> dict:
    census = {
        int(r["va"], 16): r
        for r in csv.DictReader(open(args.census, newline="", encoding="utf-8"))
        if r["baseline"] == "fault-only" and r["tier"]
    }
    rows = load_json(args.rows)
    history = {int(h["va"], 16): h for h in load_json(f"{DATA}/campaign-history.json")}
    probe = {int(k, 16): v for k, v in load_json(f"{DATA}/probe-faults.json").items()}
    split = load_json("docs/data/t1773-provider-synthesis/sample-split.json")
    heldout = {int(x, 16) for x in split["heldout"]}
    reach = {}
    reach.update(
        load_json("docs/data/t1773-provider-synthesis/development-results.json")["results"]
    )
    reach.update(load_json("docs/data/t1773-provider-synthesis/heldout-results.json")["results"])
    missing = sorted(set(census) - set(history))
    if missing:
        raise SystemExit("campaign history missing for " + ", ".join(hex(v) for v in missing))
    listed: dict[str, list[dict]] = {}
    excluded: list[dict] = []
    for va in sorted(census):
        row = census[va]
        hist = history[va]
        reason = None
        if va in HELD_OUT_FAILURES:
            reason = {"class": "held-out failure", "detail": HELD_OUT_FAILURES[va]}
        elif hist["status"] in EXCLUDING_STATUSES and not (
            hist.get("fixture_domain_verdict_floor") and not args.strict_literal
        ):
            reason = {
                "class": hist["status"],
                "detail": hist.get("verdict_numbers") or hist["notes"],
            }
        if reason:
            excluded.append(
                {
                    "va": f"0x{va:08X}",
                    "band": row["band"],
                    "reason": reason,
                    "evidence": hist["evidence"],
                }
            )
            continue
        entry = {
            "va": f"0x{va:08X}",
            "size": int(row["size"]),
            "insns": int(row["insns"]),
            "meaningful": int(row["meaningful"]),
            "subsystem": row["subsystem"],
            "tier": row["tier"],
            "prerequisite": row["prereq"] or "none",
            "census_mention": row["mention"] or "fresh",
            "why_eligible": rows[f"0x{va:08x}"]["why"] if f"0x{va:08x}" in rows else "",
            "why_fault_only": hist["why_fault_only"],
            "oracle_fault_site": fault_site(probe.get(va)),
            "campaign_fault_site": hist.get("recorded_fault_site"),
            "campaign_status": hist["status"],
            "fixture_domain_verdict_floor": bool(hist.get("fixture_domain_verdict_floor")),
            "campaign_evidence": hist["evidence"],
            "campaign_notes": hist["notes"],
            "synth_sample": "heldout" if va in heldout else "development",
            "synth_reach": synth_reach(reach, va),
        }
        listed.setdefault(row["band"], []).append(entry)
    return {"listed": listed, "excluded": excluded, "census": len(census)}


def write(args: argparse.Namespace, built: dict) -> dict:
    out = Path(args.out_dir)
    groups = {
        key: [band_list.strip() for band_list in value.split(",")]
        for key, value in (item.split("=", 1) for item in args.group)
    }
    group_of: dict[str, str] = {}
    for letter, names in groups.items():
        for name in names:
            group_of[name] = letter
    summary: dict = {
        "census_fault_only": built["census"],
        "bands": {},
        "groups": {},
        "excluded": len(built["excluded"]),
    }
    for band, entries in sorted(built["listed"].items()):
        band_dir = out / f"band-{band}"
        band_dir.mkdir(parents=True, exist_ok=True)
        for old in band_dir.glob("list-*.json"):
            old.unlink()
        batches = [entries[i : i + BATCH] for i in range(0, len(entries), BATCH)]
        files = []
        for number, batch in enumerate(batches, 1):
            name = f"list-{number:03d}.json"
            tag = f"{band}/{name}"
            document = {
                "task": "T1773",
                "band": band,
                "kind": "fault-only-synth-domain",
                "number": number,
                "partial": len(batch) < BATCH,
                "count": len(batch),
                "task_group": group_of.get(tag, group_of.get(band, "")),
                "usage": (
                    "batch_prove LIST --out-dir tmp/bp/N --draft FILE.c --cores A-B "
                    "--xbe build/default.xbe --synth-domain"
                ),
                "status": (
                    "static eligibility plus original-only synth reach (T1773); "
                    "no proof, no admission"
                ),
                "functions": batch,
            }
            (band_dir / name).write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
            files.append(
                {
                    "file": f"{DATA}/band-{band}/{name}",
                    "count": len(batch),
                    "group": document["task_group"],
                }
            )
        summary["bands"][band] = {"roots": len(entries), "lists": files}
        for item in files:
            summary["groups"].setdefault(item["group"], {"roots": 0, "lists": []})
            summary["groups"][item["group"]]["roots"] += item["count"]
            summary["groups"][item["group"]]["lists"].append(item["file"])
    (out / "excluded.json").write_text(
        json.dumps(built["excluded"], indent=1) + "\n", encoding="utf-8"
    )
    (out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--census", default="tmp/t1772/final/census.csv")
    parser.add_argument("--rows", default="tmp/t1772/final/rows.json")
    parser.add_argument("--out-dir", default=DATA)
    parser.add_argument(
        "--strict-literal",
        action="store_true",
        help="also exclude rejections that only failed the verdict floor with oracle faults",
    )
    parser.add_argument(
        "--group",
        action="append",
        default=[],
        help="LETTER=comma separated band ids or band/list-NNN.json tags, e.g. A=T1475,T1476",
    )
    args = parser.parse_args()
    summary = write(args, build(args))
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
