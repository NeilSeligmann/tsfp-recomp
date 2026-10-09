#!/usr/bin/env python3
# ruff: noqa: E501
"""T1727: attribute the census targets of the ingame-collide scenario to physics/collision functions, against the idle control.

Input: a session folder already ranked by `python -m tools.action_census_diff SESSION` (candidates.csv, every row ranked
against the idle control of its group). This tool keeps the rows of the collide scenario and labels each one:

  subsystem   tools/data/function_subsystems.csv places the function in engine/physics (the table is optional, local)
  phys-name   the existing function name has a physics token (collision, raycast, sweep, wall, contact, impact, physics,
              grenade, bounce, gravity, jump, ground, floor, trace)
  other       in the census, no physics evidence

`--require-physics` exits 1 when no collide row is a physics function. Evidence: MEASURED that the target was called more than at
idle, the role is INFERRED (indirect entry points only, direct callees unattributed).

Usage: python -m tools.physics_attribution SESSION_DIR [--scenario ingame-collide] [--require-physics] [--table PATH]
Writes SESSION_DIR/physics_attribution.csv.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections.abc import Callable
from pathlib import Path

SCENARIO = "ingame-collide"
LEAF = "engine/physics"
TABLE = Path("tools/data/function_subsystems.csv")
PHYS_NAME_RE = re.compile(
    r"collision|raycast|_ray_|sweep|_wall|contact|impact|physics|grenade|bounce|gravity|_jump|_ground|_floor|_trace"
)


def load_placement(path: Path) -> dict[int, str]:
    """Function address -> subsystem for the engine/physics rows; empty when the local table is absent."""
    if not path.is_file():
        return {}
    with path.open(newline="", encoding="utf-8") as handle:
        return {
            int(row["address"], 16): row["subsystem"]
            for row in csv.DictReader(handle)
            if row["subsystem"] == LEAF or row["subsystem"].startswith(LEAF + "/")
        }


def classify(entry_va: int, name: str, placement: dict[int, str]) -> tuple[str, str]:
    """(label, detail) of one census target."""
    if entry_va in placement:
        return "subsystem", placement[entry_va]
    match = PHYS_NAME_RE.search(name)
    if match:
        return "phys-name", match.group(0)
    return "other", ""


def attribute(
    rows: list[dict[str, str]], placement: dict[int, str], scenario: str = SCENARIO
) -> list[dict[str, str]]:
    """Every row of the scenario with its label, physics rows first (candidate before weak, then z)."""
    out: list[dict[str, str]] = []
    for row in rows:
        if row.get("scenario") != scenario:
            continue
        label, detail = classify(int(row["entry_va"], 16), row.get("existing_name", ""), placement)
        out.append({**row, "phys_label": label, "phys_detail": detail})
    out.sort(
        key=lambda r: (
            r["phys_label"] == "other",
            r["tier"] != "candidate",
            -float(r.get("z") or 0.0),
            r["entry_va"],
        )
    )
    return out


def run(
    session: Path,
    scenario: str,
    require_physics: bool,
    table: Path = TABLE,
    out: Callable[[str], object] = print,
) -> int:
    path = session / "candidates.csv"
    if not path.is_file():
        out(f"no {path}: run `python -m tools.action_census_diff {session}` first")
        return 2
    with path.open(newline="", encoding="utf-8") as handle:
        rows = attribute(list(csv.DictReader(handle)), load_placement(table), scenario)
    if not rows:
        out(f"no census row for {scenario} (phase skipped, or nothing above the idle control)")
        return 1 if require_physics else 0
    with (session / "physics_attribution.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    hits = [r for r in rows if r["phys_label"] != "other"]
    out(
        f"{scenario} against ingame-idle: {len(rows)} targets above idle, {len(hits)} physics attributed"
    )
    for r in hits:
        out(
            f"  {r['entry_va']} {r['phys_label']:<10} {r['tier']:<9} calls {r['calls']:>6} "
            f"z {r['z']:>6} {r['phys_detail']} {r.get('existing_name', '')[:70]}"
        )
    for r in [r for r in rows if r["phys_label"] == "other"][:10]:
        out(
            f"  {r['entry_va']} other      {r['tier']:<9} calls {r['calls']:>6} z {r['z']:>6} {r.get('existing_name', '')[:70]}"
        )
    out(
        "MEASURED: called above the idle rate. Roles INFERRED, indirect entry points only (direct callees unattributed)."
    )
    return 1 if require_physics and not hits else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("session_dir", type=Path)
    parser.add_argument("--scenario", default=SCENARIO)
    parser.add_argument("--require-physics", action="store_true")
    parser.add_argument("--table", type=Path, default=TABLE)
    args = parser.parse_args(argv)
    return run(args.session_dir, args.scenario, args.require_physics, args.table)


if __name__ == "__main__":
    sys.exit(main())
