#!/usr/bin/env python3
# ruff: noqa: E501
"""T1741: attribute the census targets of the enemy-encounter scenario to AI functions, against the idle control.

Input: a session folder of `python -m tools.action_profile session --only ingame-idle,ingame-enemy-encounter` that was
already ranked by `python -m tools.action_census_diff SESSION` (it wrote candidates.csv, every row ranked against the idle
control of its group). This tool keeps the rows of the encounter scenario and labels each one:

  class-tick   entry address is one of the T1740 per-class tick handlers (docs/t-ai-behavior.md, selector low:high)
  ai-name      the existing function name has an AI token (`_ai_`, `char_class`, `char_low_class`, `char_high_class`,
               `actor_ai`, `alert`, `nav_class`)
  other        in the census, no AI evidence

`--require-ai` exits 1 when no encounter row is an AI function (the scenario produced no AI census). Evidence: MEASURED that
the target was called more than at idle, the role is INFERRED (indirect entry points only, direct callees unattributed).

Usage: python -m tools.ai_attribution SESSION_DIR [--scenario ingame-enemy-encounter] [--require-ai]
Writes SESSION_DIR/ai_attribution.csv.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections.abc import Callable
from pathlib import Path

SCENARIO = "ingame-enemy-encounter"
AI_NAME_RE = re.compile(r"_ai_|char_class|char_low_class|char_high_class|actor_ai|_alert|nav_class")
# docs/t-ai-behavior.md "Finite class and state catalogue": tick entry -> selector low:high (INFERRED roles, MEASURED addresses)
CLASS_TICKS: dict[int, str] = {
    0x2603F0: "0:0x4000000",
    0x260940: "4:0 type 0xD6EC0",
    0x261750: "0x20000000:0",
    0x262720: "0x800000:0",
    0x262B20: "0x400000:0",
    0x262C20: "0:0x800",
    0x262D40: "0:4",
    0x263260: "0:8",
    0x263710: "0:0x10",
    0x263870: "0:0x20",
    0x263D00: "0:2",
    0x263E20: "0:0x2000000",
    0x2648C0: "0:1",
    0x264A10: "0:0x8000",
    0x2651A0: "0:0x10000",
    0x265820: "0x1000:0",
    0x265960: "4:0 type 0x51",
    0x265CC0: "0:0x400000",
    0x266620: "0x80:0",
    0x266B10: "0x800:0",
    0x266DD0: "8:0",
    0x267450: "0x2000000:0",
    0x267790: "4:0 active target",
    0x2682A0: "0x80000:0",
    0x269240: "0x100:0",
    0x26A7A0: "0x80000000:0",
    0x26B7A0: "0:0x4000",
    0x26C0E0: "0x200:0",
    0x26C630: "0x8000000:0",
    0x2A0500: "0x8000:0",
    0x2A1F30: "0x10000:0",
    0x2A29B0: "0x20000:0",
    0x2A4120: "0x40000:0",
}


def classify(entry_va: int, name: str) -> tuple[str, str]:
    """(label, detail) of one census target."""
    if entry_va in CLASS_TICKS:
        return "class-tick", f"selector {CLASS_TICKS[entry_va]}"
    match = AI_NAME_RE.search(name)
    if match:
        return "ai-name", match.group(0)
    return "other", ""


def attribute(rows: list[dict[str, str]], scenario: str = SCENARIO) -> list[dict[str, str]]:
    """Every row of the scenario with its label, AI rows first (candidate before weak, then z)."""
    out: list[dict[str, str]] = []
    for row in rows:
        if row.get("scenario") != scenario:
            continue
        label, detail = classify(int(row["entry_va"], 16), row.get("existing_name", ""))
        out.append({**row, "ai_label": label, "ai_detail": detail})
    out.sort(
        key=lambda r: (
            r["ai_label"] == "other",
            r["tier"] != "candidate",
            -float(r.get("z") or 0.0),
            r["entry_va"],
        )
    )
    return out


def run(
    session: Path, scenario: str, require_ai: bool, out: Callable[[str], object] = print
) -> int:
    path = session / "candidates.csv"
    if not path.is_file():
        out(f"no {path}: run `python -m tools.action_census_diff {session}` first")
        return 2
    with path.open(newline="", encoding="utf-8") as handle:
        rows = attribute(list(csv.DictReader(handle)), scenario)
    if not rows:
        out(f"no census row for {scenario} (phase skipped, or nothing above the idle control)")
        return 1 if require_ai else 0
    with (session / "ai_attribution.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    ai_rows = [r for r in rows if r["ai_label"] != "other"]
    out(
        f"{scenario} against ingame-idle: {len(rows)} targets above idle, {len(ai_rows)} AI attributed"
    )
    for r in ai_rows:
        out(
            f"  {r['entry_va']} {r['ai_label']:<10} {r['tier']:<9} {r['kind']:<11} calls {r['calls']:>6} "
            f"z {r['z']:>6} {r['ai_detail']} {r.get('existing_name', '')[:70]}"
        )
    others = [r for r in rows if r["ai_label"] == "other"][:10]
    if others:
        out("  top non-AI targets (no AI evidence, may still be AI callees):")
        for r in others:
            out(
                f"  {r['entry_va']} other      {r['tier']:<9} calls {r['calls']:>6} z {r['z']:>6} {r.get('existing_name', '')[:70]}"
            )
    out(
        "MEASURED: called above the idle rate. Roles INFERRED, indirect entry points only (direct callees unattributed)."
    )
    return 1 if require_ai and not ai_rows else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("session_dir", type=Path)
    parser.add_argument("--scenario", default=SCENARIO)
    parser.add_argument("--require-ai", action="store_true")
    args = parser.parse_args(argv)
    return run(args.session_dir, args.scenario, args.require_ai)


if __name__ == "__main__":
    sys.exit(main())
