#!/usr/bin/env python3
# ruff: noqa: E501
"""T1502: compare several action sessions (one per weapon) to split generic action code from weapon-specific code.

Owner Map Maker weapon sessions: one session folder per weapon (names `mm-pistol`, `mm-shotgun`, `mm-sniper`, `mm-rocket`,
`mm-flamethrower`, `mm-minigun`, `mm-grenade`, `mm-melee`, plus `mm-bots` and `mm-nobots`), each recorded with
`--only ingame-idle,ingame-fire,ingame-reload` (or ingame-melee). For every action scenario the tool takes, per session, the
candidates against THAT session's own idle control (the ranking of tools/action_census_diff.py for census phase files, of
tools/action_profile_diff.py for sampler files), then classifies each target over the sessions that have the action:

  common        a candidate in EVERY session: generic code of the action (fire, reload, melee) that every weapon runs
  unique        a candidate in exactly ONE session and in no other session's action phase at all: weapon-specific code,
                the valuable ones, named `game_<weapon>_<action>_ROLE`
  unique-rate   a candidate in exactly one session but also seen at a lower rate in another: weapon-leaning code
  partial       a candidate in some but not all sessions (a weapon family, or a missed pass)

Outputs in `--out` (default: the folder holding the first session): `candidates_compare.csv` (one row per scenario and target
with the class, the sessions, and per session `<label>_count`, `<label>_rate` and `<label>_presents`) and
`names_template_compare.csv` (the function_names.csv format, for the unnamed `common` and `unique` rows). A census session
gives targets that are INDIRECT entry points only (direct callees unattributed), see action_census_diff.py.

One session, many weapons (T1502 labels): `compare SESSION --by-label` takes a session recorded with labeled phases
(`--only ingame-idle,ingame-fire:pistol,ingame-fire:shotgun,ingame-reload:shotgun,ingame-melee:bat`), splits every labeled
action X into its labels and classifies each target of X over the labels that have X, against the session's single idle control:
common (a candidate under every label: generic firing code), unique (one label only: weapon-specific, the valuable ones),
unique-rate and partial as above. `candidates_compare.csv` then has a `weapon` column (the label of a unique row) and the names
template says `game_<label>_<action>_ROLE` with evidence `host census: <action> only with <label> (N calls in P presents)`.

Usage: python -m tools.action_census_diff compare SESSION_DIR SESSION_DIR ... [--scenarios ingame-fire,ingame-reload]
       [--source auto|census|sampler] [--host tsfp_host (sampler)] [--out DIR] [--top 20]
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from tools import action_census_diff as census
from tools import action_profile_diff as sampler

DEFAULT_SCENARIOS = ("ingame-fire", "ingame-reload", "ingame-melee")
WEAPON_RE = re.compile(r"(?:^|-)mm-([A-Za-z0-9]+)")


@dataclass
class Metric:
    count: int
    rate: float
    presents: int | None  # distinct presents (census), None for samples


@dataclass
class SessionData:
    label: str
    kind: str  # census | sampler
    candidates: dict[str, dict[int, Metric]] = field(default_factory=dict)
    seen: dict[str, set[int]] = field(default_factory=dict)
    notes: list[str] = field(default_factory=list)


@dataclass
class CompareRow:
    scenario: str
    target: int
    klass: str
    sessions: tuple[str, ...]  # the sessions in which it is a candidate
    total: int  # sessions that have the scenario
    metrics: dict[str, Metric]


def session_label(session: Path) -> str:
    """`mm-pistol` for a folder named `20261007-120000-mm-pistol` or `mm-pistol`, else the folder name."""
    match = WEAPON_RE.search(session.name)
    return f"mm-{match.group(1)}" if match else session.name


def weapon_of(label: str) -> str:
    return label[3:] if label.startswith("mm-") else label


def action_of(scenario: str) -> str:
    return scenario.split("-", 1)[1] if "-" in scenario else scenario


def load_census(session: Path, scenarios: dict[str, dict]) -> SessionData | None:
    phases, warnings = census.discover(session, scenarios)
    if not phases:
        return None
    ranked = census.rank(phases, scenarios, weak=0)
    data = SessionData(session_label(session), "census", notes=warnings)
    for scenario, rows in ranked.items():
        data.seen[scenario] = set(phases[scenario].targets)
        data.candidates[scenario] = {
            row.target: Metric(row.calls, row.per_present, row.presents)
            for row in rows
            if row.tier == "candidate"
        }
    return data


def load_sampler(
    session: Path, scenarios: dict[str, dict], host: Path | None
) -> SessionData | None:
    files, warnings = sampler.discover(session, scenarios)
    if not files:
        return None
    counts, totals, more = sampler.count_functions(files, host, False)
    ranked = sampler.rank(
        counts, totals, scenarios, seconds=sampler.scenario_seconds(session), weak=0
    )
    data = SessionData(session_label(session), "sampler", notes=warnings + more)
    for scenario, rows in ranked.items():
        data.seen[scenario] = {
            int(match.group(1), 16)
            for function in counts[scenario]
            if (match := sampler.GUEST_RE.match(function))
        }
        data.candidates[scenario] = {}
        for row in rows:
            match = sampler.GUEST_RE.match(row.function)
            if row.tier == "candidate" and match:
                data.candidates[scenario][int(match.group(1), 16)] = Metric(
                    row.samples, row.rate, None
                )
    return data


def split_by_label(data: SessionData, scenarios: dict[str, dict]) -> list[SessionData]:
    """One pseudo session per label of a labeled session: `ingame-fire@shotgun` becomes scenario `ingame-fire` of `shotgun`."""
    per_label: dict[str, SessionData] = {}
    for key in sorted(data.candidates):
        base, label = sampler.split_key(key)
        if label is None or scenarios[key]["kind"] == "control":
            continue
        part = per_label.setdefault(label, SessionData(label, data.kind, notes=data.notes))
        part.candidates[base] = data.candidates[key]
        part.seen[base] = data.seen.get(key, set())
    return list(per_label.values())


def classify(
    sessions: list[SessionData],
    scenarios: list[str],
) -> list[CompareRow]:
    rows: list[CompareRow] = []
    for scenario in scenarios:
        having = [data for data in sessions if scenario in data.candidates]
        if len(having) < 2:
            continue
        targets = sorted({target for data in having for target in data.candidates[scenario]})
        for target in targets:
            owners = tuple(data.label for data in having if target in data.candidates[scenario])
            elsewhere = [
                data.label
                for data in having
                if data.label not in owners and target in data.seen.get(scenario, set())
            ]
            if len(owners) == len(having):
                klass = "common"
            elif len(owners) == 1:
                klass = "unique" if not elsewhere else "unique-rate"
            else:
                klass = "partial"
            metrics = {
                data.label: data.candidates[scenario][target]
                for data in having
                if target in data.candidates[scenario]
            }
            rows.append(CompareRow(scenario, target, klass, owners, len(having), metrics))
    order = {"unique": 0, "unique-rate": 1, "partial": 2, "common": 3}
    rows.sort(
        key=lambda row: (
            row.scenario,
            order[row.klass],
            -max(metric.rate for metric in row.metrics.values()),
            row.target,
        )
    )
    return rows


def evidence(row: CompareRow, scenarios: dict[str, dict], by_label: bool = False) -> str:
    action = action_of(row.scenario)
    if by_label and row.klass == "unique":
        label = row.sessions[0]
        metric = row.metrics[label]
        presents = f" in {metric.presents} presents" if metric.presents is not None else ""
        unit = "calls" if metric.presents is not None else "samples"
        source = "census" if metric.presents is not None else "trace"
        return (
            f"host {source}: {action} only with {label} ({metric.count} {unit}{presents}); one owner session with labeled phases, "
            "MEASURED, name INFERRED; indirect entry point only, direct callees unattributed"
        )
    if row.klass == "common":
        counts = ", ".join(f"{label} {metric.count} calls" for label, metric in row.metrics.items())
        what = f"called in every one of the {row.total} sessions' {action} and not in their idle ({counts})"
    else:
        label = row.sessions[0]
        metric = row.metrics[label]
        presents = f" in {metric.presents} presents" if metric.presents is not None else ""
        what = (
            f"called only in the {label} session's {action} ({metric.count} {'calls' if metric.presents is not None else 'samples'}{presents}) "
            f"and in no other weapon session's {action}, not in idle"
        )
    return (
        f"host {'census' if next(iter(row.metrics.values())).presents is not None else 'trace'}: {what}; owner sessions, MEASURED, "
        "name INFERRED; indirect entry point only, direct callees unattributed"
    )


def write_outputs(
    out: Path,
    rows: list[CompareRow],
    labels: list[str],
    names: dict[int, str],
    table: tuple[list[int], list[tuple[int, int]]] | None,
    scenarios: dict[str, dict],
    by_label: bool = False,
) -> list[Path]:
    out.mkdir(parents=True, exist_ok=True)
    candidates = out / "candidates_compare.csv"
    template = out / "names_template_compare.csv"
    with candidates.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        header = [
            "entry_va",
            "scenario",
            "class",
            *(["weapon"] if by_label else []),
            "candidate_in",
            "sessions_with_scenario",
            "existing_name",
            "function_table",
        ]
        for label in labels:
            header += [f"{label}_count", f"{label}_rate", f"{label}_presents"]
        writer.writerow(header)
        for row in rows:
            line = [
                f"0x{row.target:08X}",
                row.scenario,
                row.klass,
                *(
                    [row.sessions[0] if row.klass in ("unique", "unique-rate") else ""]
                    if by_label
                    else []
                ),
                ";".join(row.sessions),
                row.total,
                names.get(row.target, ""),
                census.containing(table, row.target),
            ]
            for label in labels:
                metric = row.metrics.get(label)
                line += (
                    [
                        metric.count,
                        f"{metric.rate:.4f}",
                        "" if metric.presents is None else metric.presents,
                    ]
                    if metric
                    else ["", "", ""]
                )
            writer.writerow(line)
    with template.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["entry_va", "name", "confidence", "evidence"])
        seen: set[int] = set()
        for row in rows:
            if row.klass not in ("common", "unique") or row.target in names or row.target in seen:
                continue
            seen.add(row.target)
            action = action_of(row.scenario)
            name = (
                f"game_{action}_ROLE"
                if row.klass == "common"
                else f"game_{weapon_of(row.sessions[0])}_{action}_ROLE"
            )
            writer.writerow(
                [f"0x{row.target:08X}", name, "INFERRED", evidence(row, scenarios, by_label)]
            )
    return [candidates, template]


def print_report(
    rows: list[CompareRow], names: dict[int, str], top: int, labels: list[str]
) -> None:
    print(f"sessions: {', '.join(labels)}")
    for scenario in sorted({row.scenario for row in rows}):
        mine = [row for row in rows if row.scenario == scenario]
        print(f"\n== {scenario} ({mine[0].total} sessions) ==")
        for klass, heading in (
            ("unique", "weapon-specific (a candidate in one session, absent from the others)"),
            (
                "unique-rate",
                "weapon-leaning (a candidate in one session, seen at a lower rate in another)",
            ),
            ("partial", "some sessions only"),
            ("common", "generic (a candidate in every session)"),
        ):
            group = [row for row in mine if row.klass == klass]
            if not group:
                continue
            print(f"  {heading}: {len(group)}")
            for row in group[:top]:
                detail = ", ".join(
                    f"{label} {metric.count}c {metric.rate:.2f}/{'pres' if metric.presents is not None else 'unit'}"
                    + (f" {metric.presents}p" if metric.presents is not None else "")
                    for label, metric in row.metrics.items()
                )
                print(f"    0x{row.target:08X}  {names.get(row.target, '(unnamed)'):40s} {detail}")
    print(
        "\nThe census sees INDIRECT calls only: a candidate is an indirect entry point and its direct callees are unattributed."
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.action_census_diff compare",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("sessions", type=Path, nargs="+", help="session folders, one per weapon")
    parser.add_argument(
        "--scenarios",
        default=None,
        help="actions to compare (default fire, reload, melee; every labeled action with --by-label)",
    )
    parser.add_argument(
        "--by-label",
        action="store_true",
        help="ONE session recorded with labeled phases (ingame-fire:pistol ...): compare its labels",
    )
    parser.add_argument("--source", choices=["auto", "census", "sampler"], default="auto")
    parser.add_argument("--host", type=Path, help="tsfp_host that produced sampler profiles")
    parser.add_argument(
        "--out", type=Path, help="output folder (default: the folder holding the first session)"
    )
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--functions", type=Path, default=sampler.FUNCTIONS_FILE)
    args = parser.parse_args(argv)
    scenarios = {s["id"]: s for s in json.loads(sampler.SCENARIO_FILE.read_text())["scenarios"]}
    wanted = [
        name.strip()
        for name in (args.scenarios or ",".join(DEFAULT_SCENARIOS)).split(",")
        if name.strip()
    ]
    unknown = [name for name in wanted if name not in scenarios]
    if unknown:
        print(f"unknown scenario(s) {unknown}", file=sys.stderr)
        return 2
    missing = [path for path in args.sessions if not path.is_dir()]
    if missing:
        print(f"not a folder: {missing[0]}", file=sys.stderr)
        return 2
    if args.by_label and len(args.sessions) != 1:
        print("--by-label compares the labels of exactly one session folder", file=sys.stderr)
        return 2
    if len(args.sessions) < 2 and not args.by_label:
        print("compare needs at least two session folders", file=sys.stderr)
        return 2
    loaded: list[SessionData] = []
    for source in ("census", "sampler") if args.source == "auto" else (args.source,):
        loaded = []
        for path in args.sessions:
            data = (
                load_census(path, scenarios)
                if source == "census"
                else load_sampler(path, scenarios, args.host)
            )
            if data is None:
                break
            loaded.append(data)
        else:
            break
        loaded = []
    if not loaded:
        print(
            "not every session has the same kind of data (census.txt.<phase> for all, or profile.txt.<phase> for all): "
            + ", ".join(path.name for path in args.sessions),
            file=sys.stderr,
        )
        return 3
    if args.by_label:
        loaded = split_by_label(loaded[0], scenarios)
        if len(loaded) < 2:
            print(
                "the session has fewer than two labels (record it with --only ingame-idle,ingame-fire:pistol,ingame-fire:shotgun,...)",
                file=sys.stderr,
            )
            return 3
        if not args.scenarios:
            wanted = sorted({scenario for data in loaded for scenario in data.candidates})
    labels = [data.label for data in loaded]
    if len(set(labels)) != len(labels):
        print(f"two sessions have the same label {labels}, rename a folder", file=sys.stderr)
        return 2
    for data in loaded:
        for note in data.notes:
            print(f"warning: {data.label}: {note}")
    rows = classify(loaded, wanted)
    if not rows:
        print(
            "no action scenario in at least two sessions has a candidate (is each session recorded with --only ingame-idle,ingame-fire,...?)"
        )
    names = census.load_names(census.NAMES_FILE)
    table = census.load_table(args.functions)
    print_report(rows, names, args.top, labels)
    out = args.out or (args.sessions[0] if args.by_label else args.sessions[0].parent)
    for path in write_outputs(out, rows, labels, names, table, scenarios, args.by_label):
        print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
