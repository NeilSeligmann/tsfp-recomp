# SPDX-License-Identifier: GPL-3.0-or-later
"""Paired EXACT audit on the archived T1772 static-clean population (no byte output)."""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
from pathlib import Path
from unittest.mock import patch

from tools import call_draft_lists as cdl
from tools.harness.image import build_guest_image
from tools.replace import audit
from tools.replace.manifest import ManifestEntry


def measure(private: Path, rows_path: Path, calls_path: Path, out: Path) -> dict:
    rows = json.loads(rows_path.read_text())
    calls = json.loads(calls_path.read_text())["dispositions"]
    entries = {}
    for row in rows.values():
        if (
            row["status"] == "open"
            and row["calls"] == 0
            and row["first"] in {"eligible", "leaf-caller-audit"}
        ):
            va = row["va"]
            entries[va] = ManifestEntry(va, f"sub_{va:08X}", "cdecl", 0, "eax", (), "measure")
    for row in calls:
        if "audit_contract" in row:
            contract = row["audit_contract"]
            va = int(contract["va"], 16)
            if rows.get(f"0x{va:08X}", {}).get("status") != "open":
                continue
            entries[va] = ManifestEntry(
                va,
                contract["name"],
                contract["convention"],
                contract["stack_args"],
                contract["returns"],
                (),
                "measure",
            )
    xbe = private / "tmp/oxm-extract/retail/default.xbe"
    functions = private / "generated/retail/functions.csv"
    image = build_guest_image(xbe)
    kwargs = dict(data_ranges=[(lo, hi) for _, lo, hi in cdl.data_ranges(xbe)], functions=functions)
    # Same image, original index, reachability and budgets on both sides.
    # The sole delta is consuming authenticated flag summaries.
    with patch.object(audit.RegisterSummarySource, "summarize_flags", return_value=None):
        before = audit.audit_all(
            entries.values(), private / "generated/lifted/gen", image, **kwargs
        )
    after = audit.audit_all(entries.values(), private / "generated/lifted/gen", image, **kwargs)
    old = {a.va: a for a in before}
    reason_counts = collections.Counter()
    newly = []
    lost = []
    changed_sites = []
    unresolved = []
    for current in after:
        previous = old[current.va]
        if cdl.caller_eligible(previous) and not cdl.caller_eligible(current):
            lost.append(f"0x{current.va:08X}")
        prior_sites = {v.return_va: v for v in previous.flags}
        reasons = set()
        for site in current.flags:
            prior = prior_sites[site.return_va]
            if prior.verdict == "unresolved":
                unresolved.append(
                    {
                        "root": f"0x{current.va:08X}",
                        "return_va": f"0x{site.return_va:08X}",
                        "before_reason_code": prior.reason_code,
                        "before_reason": prior.reason,
                        "after_verdict": site.verdict,
                        "after_reason_code": site.reason_code,
                        "after_reason": site.reason,
                    }
                )
            if prior.verdict != "safe" and site.verdict == "safe":
                assert site.proof_path, "new SAFE site missing proof path"
                reasons.add(prior.reason_code)
                changed_sites.append(
                    {
                        "root": f"0x{current.va:08X}",
                        "return_va": f"0x{site.return_va:08X}",
                        "before": prior.reason,
                        "before_reason_code": prior.reason_code,
                        "proof_path": site.proof_path,
                    }
                )
        if not cdl.caller_eligible(previous) and cdl.caller_eligible(current):
            newly.append({"root": f"0x{current.va:08X}", "reasons": sorted(reasons)})
            reason_counts.update(reasons)
    result = {
        "confidence": "MEASURED static original-byte audit; no execution or flag-equality claim",
        "population": len(entries),
        "before_eligible": sum(map(cdl.caller_eligible, before)),
        "after_eligible": sum(map(cdl.caller_eligible, after)),
        "lost": lost,
        "newly_eligible": newly,
        "newly_eligible_by_reason": dict(sorted(reason_counts.items())),
        "newly_safe_sites": changed_sites,
        "unresolved_sites": unresolved,
        "before_unresolved_by_reason": dict(
            sorted(
                collections.Counter(
                    v.reason_code for a in before for v in a.flags if v.verdict == "unresolved"
                ).items()
            )
        ),
        "pins": {
            str(p.relative_to(private)) if p.is_relative_to(private) else p.name: hashlib.sha256(
                p.read_bytes()
            ).hexdigest()
            for p in (xbe, functions, rows_path, calls_path)
        },
        "budgets": {
            key: getattr(audit, key)
            for key in (
                "MAX_INSNS",
                "MAX_FLAG_STATES",
                "MAX_FLAG_CALL_DEPTH",
                "MAX_FLAG_CLIMB_DEPTH",
                "MAX_CLEAN_STATES",
            )
        },
    }
    assert not lost, "previously eligible roots lost"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(
        json.dumps(
            {
                k: v
                for k, v in result.items()
                if k not in {"unresolved_sites", "newly_safe_sites", "pins"}
            },
            sort_keys=True,
        )
    )
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--private-root", type=Path, default=Path("/workspace"))
    parser.add_argument("--rows", type=Path, required=True)
    parser.add_argument("--calls", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    measure(args.private_root, args.rows, args.calls, args.out)


if __name__ == "__main__":
    main()
