#!/usr/bin/env python3
# ruff: noqa: E501
"""T1765: attribute the census targets of the Story intro cutscene to cutscene functions, against the idle control.

Input: a session folder of `python -m tools.action_profile session --only ingame-idle,ingame-cutscene` that was already
ranked by `python -m tools.action_census_diff SESSION` (candidates.csv, every row ranked against the idle control of its
group). This tool keeps the rows of the cutscene scenario and labels each one:

  subsystem     tools/data/function_subsystems.csv places the function in data/cutscenes (optional, local, gitignored)
  cutscene-name the existing function name has a `cutscene` / `igcs` / `csinfo` token
  other         in the census, no cutscene evidence

It also reads SESSION/route-events.log (host `--route-event-log` with `--route-log-mem 0x79094C`, the game state word) and
prints the timeline of its values. 0x18 is the cutscene/Story-load hypothesis (INFERRED beyond the owner statements of the
owner session cutscene-20261008-233305: 0x18 spans Story load plus the intro, 0x2 is in level without control at first).
0x16 (name evidence of T1664) was refuted by that session and is kept only as "previously inferred, refuted".
`--require-cutscene` exits 1 when no census row is above idle OR the log is present but never shows 0x18 (a missing log
skips the state check and says so). Known values (MEASURED, docs/t1633-route-events-findings.md): 0x67 boot, 0x65 menu,
0x66 editor, 0x64 in game (preview), plus 0x18 and 0x2 from the owner session (docs/t-data-cutscene-scenario.md).

Usage: python -m tools.cutscene_attribution SESSION_DIR [--scenario ingame-cutscene] [--require-cutscene] [--table PATH]
Writes SESSION_DIR/cutscene_attribution.csv.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections.abc import Callable
from pathlib import Path

from tools import route_events

SCENARIO = "ingame-cutscene"
LEAF = "data/cutscenes"
TABLE = Path("tools/data/function_subsystems.csv")
STATE_ADDRESS = 0x79094C
CUTSCENE_STATE = 0x18
REFUTED_STATE = 0x16
STATE_NAMES = {
    0x67: "boot",
    0x65: "menu",
    0x66: "editor",
    0x64: "in game",
    0x18: "Story load + intro cutscene (INFERRED)",
    0x2: "in level, no control at first (INFERRED)",
    0x16: "previously inferred cutscene value, refuted",
}
EVENT_LOG = "route-events.log"
CUTSCENE_NAME_RE = re.compile(r"cutscene|igcs|csinfo")


def load_placement(path: Path) -> dict[int, str]:
    """Function address -> subsystem for the data/cutscenes rows; empty when the local table is absent."""
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
    match = CUTSCENE_NAME_RE.search(name)
    if match:
        return "cutscene-name", match.group(0)
    return "other", ""


def attribute(
    rows: list[dict[str, str]], placement: dict[int, str], scenario: str = SCENARIO
) -> list[dict[str, str]]:
    """Every row of the scenario with its label, cutscene rows first (candidate before weak, then z)."""
    out: list[dict[str, str]] = []
    for row in rows:
        if row.get("scenario") != scenario:
            continue
        label, detail = classify(int(row["entry_va"], 16), row.get("existing_name", ""), placement)
        out.append({**row, "cut_label": label, "cut_detail": detail})
    out.sort(
        key=lambda r: (
            r["cut_label"] == "other",
            r["tier"] != "candidate",
            -float(r.get("z") or 0.0),
            r["entry_va"],
        )
    )
    return out


def state_timeline(items: list[tuple[str, object]]) -> list[tuple[int, int, int]]:
    """(t_ms, poll, value) of every sample of [0x79094C] in a parsed route event log, in log order."""
    timeline: list[tuple[int, int, int]] = []
    for kind, payload in items:
        if kind != "mem":
            continue
        event: route_events.MemEvent = payload  # type: ignore[assignment]
        try:
            address = int(event.spec.split(":")[0], 16)
        except ValueError:
            continue
        if address != STATE_ADDRESS or event.new == "unreadable":
            continue
        timeline.append((event.t_ms or 0, event.poll or 0, int(event.new, 16)))
    return timeline


def run(
    session: Path,
    scenario: str,
    require_cutscene: bool,
    table: Path = TABLE,
    out: Callable[[str], object] = print,
) -> int:
    path = session / "candidates.csv"
    if not path.is_file():
        out(f"no {path}: run `python -m tools.action_census_diff {session}` first")
        return 2
    with path.open(newline="", encoding="utf-8") as handle:
        rows = attribute(list(csv.DictReader(handle)), load_placement(table), scenario)
    failed = False
    if rows:
        with (session / "cutscene_attribution.csv").open(
            "w", newline="", encoding="utf-8"
        ) as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        cut_rows = [r for r in rows if r["cut_label"] != "other"]
        out(
            f"{scenario} against ingame-idle: {len(rows)} targets above idle, {len(cut_rows)} cutscene attributed"
        )
        for r in cut_rows:
            out(
                f"  {r['entry_va']} {r['cut_label']:<13} {r['tier']:<9} {r['kind']:<11} calls {r['calls']:>6} "
                f"z {r['z']:>6} {r['cut_detail']} {r.get('existing_name', '')[:70]}"
            )
        others = [r for r in rows if r["cut_label"] == "other"][:10]
        if others:
            out("  top non-cutscene targets (no cutscene evidence, may still be cutscene callees):")
            for r in others:
                out(
                    f"  {r['entry_va']} other         {r['tier']:<9} calls {r['calls']:>6} z {r['z']:>6} {r.get('existing_name', '')[:70]}"
                )
        failed = not cut_rows
    else:
        out(f"no census row for {scenario} (phase skipped, or nothing above the idle control)")
        failed = True
    log = session / EVENT_LOG
    if log.is_file():
        timeline = state_timeline(route_events.parse_event_log(log))
        out(f"[0x79094C] timeline ({len(timeline)} samples):")
        for t_ms, poll, value in timeline:
            out(
                f"  t={t_ms} ms poll={poll} {value:#x} {STATE_NAMES.get(value, 'not in the known table')}"
            )
        if not any(value == CUTSCENE_STATE for _t, _p, value in timeline):
            out(
                "  [0x79094C] never read 0x18: the Story load / intro state hypothesis is NOT confirmed by this run"
            )
            failed = True
    else:
        out(
            f"no {log}: [0x79094C] was not recorded (host --route-event-log with --route-log-mem 0x79094C)"
        )
    out(
        "MEASURED: called above the idle rate. Roles INFERRED, indirect entry points only (direct callees unattributed)."
    )
    return 1 if require_cutscene and failed else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("session_dir", type=Path)
    parser.add_argument("--scenario", default=SCENARIO)
    parser.add_argument("--require-cutscene", action="store_true")
    parser.add_argument("--table", type=Path, default=TABLE)
    args = parser.parse_args(argv)
    return run(args.session_dir, args.scenario, args.require_cutscene, args.table)


if __name__ == "__main__":
    sys.exit(main())
