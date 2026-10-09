#!/usr/bin/env python3
# ruff: noqa: E501
"""T1502: find the indirect call targets that run more in one played action than in the idle control.

Input: a session folder written by `python -m tools.action_profile session` (default `--source census`): the files
`census.txt.<phase>` that the host writes at every mark (`--census-icalls --census-phases`, src/host/function_census.c,
format: a `# census-phase 1` and a `# calls N targets M overflow K first_present A last_present B` header and one
`target 0xVA calls C presents P first_present F last_present L in_last I first_caller 0xR thread T` line per target)
plus session.json. A session that holds only the CPU sampler files (`profile.txt.<phase>`) has no census: the tool says so.

Ranking (owner sessions showed that real signal is SHARED between actions, so exclusivity is information, not a gate):
every action X is compared with the IDLE control of its group only (menu, ingame; with no idle control, with the pooled
other scenarios and a warning). Rates are per second when session.json holds the phase seconds, else per present spanned
by the phase. For a target, p and z come from the exact Poisson comparison of X's count with the idle count over their
exposures (tools/action_stats.py, one sided: is X's rate above idle's). The target is a CANDIDATE when its rate is above
idle's with p <= --max-p, calls >= --min-calls in >= --min-presents distinct presents, and (called in idle) at least
--min-ratio times idle's rate. Everything else with a rate above idle's is a WEAK SIGNAL: the top --weak of them per
scenario are printed under that heading and written to candidates.csv (tier `weak`). Rank is by z, then calls, then address.
Columns: kind `exclusive` (in no other scenario of the group), `idle-absent` (not in idle, also in other actions) or `rate`
(in idle at a lower rate); `also_active_in` lists the other actions that call it; `exclusivity` = 1 - their share.

LIMITS, printed with the report: only INDIRECT calls and indirect tail jumps are seen (function pointers, `call [reg]`).
A function reached only by direct calls shows through its indirect caller, so a candidate is the INDIRECT ENTRY POINT
(handler, state function, widget callback) and its direct callees are unattributed. Call counts depend on the frame
rate. Evidence strength: MEASURED that the target was called during the action, INFERRED that it implements it.

Usage: python -m tools.action_census_diff SESSION_DIR [--top 15] [--weak 15] [--max-p 0.05] [--min-calls 3]
       python -m tools.action_census_diff compare SESSION_DIR SESSION_DIR ...   (see tools/action_compare.py)
Writes SESSION_DIR/candidates.csv and names_template.csv (the function_names.csv format).
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from tools.action_profile_diff import (
    FUNCTIONS_FILE,
    NAMES_FILE,
    SCENARIO_FILE,
    load_names,
    phase_key,
    register_key,
    suggested_scope,
)
from tools.action_stats import rate_test

CENSUS_NAME = "census.txt"
SKIP_SUFFIXES = (".pid", ".ack", ".phase")
TARGET_RE = re.compile(
    r"^target 0x([0-9A-Fa-f]+) calls (\d+) presents (\d+) first_present (\d+) last_present (\d+) in_last (\d+) "
    r"first_caller 0x([0-9A-Fa-f]+) thread (\d+)\s*$"
)
HEADER_RE = re.compile(
    r"^# calls (\d+) targets (\d+) overflow (\d+) first_present (\d+) last_present (\d+)"
)
NO_INDIRECT_NOTE = (
    "Only INDIRECT calls are counted (direct calls are not seen). A function reached only by direct calls shows through "
    "its indirect caller, so a candidate is the INDIRECT entry point (handler, state function, widget callback) and its "
    "direct callees are unattributed."
)


@dataclass
class TargetStat:
    calls: int = 0
    presents: int = 0
    first_caller: int = 0
    thread: int = 0


@dataclass
class Phase:
    """One scenario: the census files of its phases merged (a redo `-r2` adds to it)."""

    span: int = 0  # presents the phase covered
    seconds: float = 0.0  # wall seconds of the phase from session.json, 0 when unknown
    overflow: int = 0
    total_calls: int = 0
    files: int = 0
    targets: dict[int, TargetStat] = field(default_factory=dict)


@dataclass
class Candidate:
    target: int
    scenario: str
    tier: str  # candidate | weak
    kind: str  # exclusive | idle-absent | rate
    calls: int
    presents: int
    rate: float  # calls per unit
    idle_rate: float
    unit: str  # second | present
    per_present: float
    ratio: float
    p_value: float
    z_score: float
    exclusivity: float
    also_active_in: tuple[str, ...]
    first_caller: int
    thread: int


def parse_phase(path: Path) -> tuple[Phase, str | None]:
    """One census phase file. A missing, empty or malformed file is an empty phase with a warning text."""
    phase = Phase()
    try:
        text = path.read_text()
    except (OSError, UnicodeDecodeError):
        return phase, f"{path.name}: unreadable"
    header = False
    first = last = 0
    seen_presents = 0
    for line in text.splitlines():
        match = HEADER_RE.match(line)
        if match:
            header = True
            phase.total_calls = int(match.group(1))
            phase.overflow = int(match.group(3))
            first, last = int(match.group(4)), int(match.group(5))
            continue
        row = TARGET_RE.match(line)
        if row is None:
            continue
        target = int(row.group(1), 16)
        stat = phase.targets.setdefault(target, TargetStat())
        stat.calls += int(row.group(2))
        stat.presents += int(row.group(3))
        stat.first_caller = int(row.group(7), 16)
        stat.thread = int(row.group(8))
        seen_presents = max(seen_presents, int(row.group(3)))
    if not header:
        return phase, f"{path.name}: no census header (empty or truncated)"
    phase.span = max(last - first + 1 if last >= first else 0, seen_presents)
    phase.files = 1
    return phase, None


def merge(into: Phase, other: Phase) -> None:
    into.span += other.span
    into.seconds += other.seconds
    into.overflow += other.overflow
    into.total_calls += other.total_calls
    into.files += other.files
    for target, stat in other.targets.items():
        merged = into.targets.setdefault(target, TargetStat())
        merged.calls += stat.calls
        merged.presents += stat.presents
        merged.first_caller = merged.first_caller or stat.first_caller
        merged.thread = merged.thread or stat.thread


def discover(session: Path, scenarios: dict[str, dict]) -> tuple[dict[str, Phase], list[str]]:
    """The merged census per scenario id, and warnings. Uses session.json when present, else the file names."""
    warnings: list[str] = []
    ids = set(scenarios)
    meta_file = session / "session.json"
    meta = json.loads(meta_file.read_text()) if meta_file.is_file() else {}
    saved = {p.get("file", p["name"]): p for p in meta.get("phases", [])}
    phases: dict[str, Phase] = {}
    for path in sorted(session.glob(f"{CENSUS_NAME}.*")):
        if path.name.endswith(SKIP_SUFFIXES):
            continue
        name = path.name[len(CENSUS_NAME) + 1 :]
        info = saved.get(name)
        if info is not None and (not info.get("saved", True) or info["kind"] == "navigation"):
            continue
        scenario = phase_key(info, name, ids)
        if scenario is None or not register_key(scenarios, scenario):
            continue
        phase, warning = parse_phase(path)
        if warning:
            warnings.append(warning)
            continue
        phase.seconds = float((info or {}).get("seconds") or 0.0)
        merge(phases.setdefault(scenario, Phase()), phase)
    for scenario in meta.get("scenario_order", []):
        if register_key(scenarios, scenario) and scenario not in phases:
            warnings.append(f"no census for {scenario} (skipped, not saved or empty)")
    for scenario, phase in phases.items():
        if phase.overflow:
            warnings.append(
                f"{scenario}: {phase.overflow} indirect calls hit a full 4096-target table and are not attributed"
            )
        if not phase.targets:
            warnings.append(f"{scenario}: the census saw no indirect call")
    return phases, warnings


def exposure(phase: Phase, reference: Phase) -> tuple[float, float, str]:
    """(exposure of phase, of the reference, unit): seconds when both know them, else the presents spanned."""
    if phase.seconds > 0 and reference.seconds > 0:
        return phase.seconds, reference.seconds, "second"
    return float(phase.span), float(reference.span), "present"


def pooled(members: list[Phase]) -> Phase:
    total = Phase()
    for member in members:
        merge(total, member)
    return total


def rank(
    phases: dict[str, Phase],
    scenarios: dict[str, dict],
    min_calls: int = 3,
    min_presents: int = 2,
    min_ratio: float = 3.0,
    max_p: float = 0.05,
    weak: int = 15,
) -> dict[str, list[Candidate]]:
    """Per action scenario its candidates then its top weak signals, best first (see the module docstring)."""
    result: dict[str, list[Candidate]] = {}
    for name, phase in phases.items():
        info = scenarios[name]
        if info["kind"] == "control" or not phase.targets or phase.span <= 0:
            result[name] = []
            continue
        group = info["group"]
        others = [
            other
            for other in phases
            if other != name and scenarios[other]["group"] == group and phases[other].targets
        ]
        idles = [other for other in others if scenarios[other]["kind"] == "control"]
        actions = [other for other in others if scenarios[other]["kind"] != "control"]
        reference = pooled([phases[other] for other in (idles or others)])
        if reference.span <= 0:
            reference = Phase(span=1)  # no usable reference at all: every rate is above zero
        scale_phase, scale_reference, unit = exposure(phase, reference)
        rows: list[Candidate] = []
        for target, stat in phase.targets.items():
            ref_stat = reference.targets.get(target)
            ref_calls = ref_stat.calls if ref_stat else 0
            rate = stat.calls / scale_phase if scale_phase > 0 else 0.0
            ref_rate = ref_calls / scale_reference if scale_reference > 0 else 0.0
            if rate <= ref_rate:
                continue
            p_value, z_score = rate_test(stat.calls, scale_phase, ref_calls, scale_reference)
            ratio = rate / ref_rate if ref_rate > 0 else float("inf")
            gate = (
                stat.calls >= min_calls
                and stat.presents >= min_presents
                and p_value <= max_p
                and (ref_calls == 0 or ratio >= min_ratio)
            )
            also = tuple(sorted(other for other in actions if target in phases[other].targets))
            in_ref = ref_calls > 0
            kind = "rate" if in_ref else ("idle-absent" if also else "exclusive")
            rows.append(
                Candidate(
                    target,
                    name,
                    "candidate" if gate else "weak",
                    kind,
                    stat.calls,
                    stat.presents,
                    rate,
                    ref_rate,
                    unit,
                    stat.calls / phase.span,
                    ratio,
                    p_value,
                    z_score,
                    1.0 - len(also) / max(len(actions), 1),
                    also,
                    stat.first_caller,
                    stat.thread,
                )
            )
        rows.sort(key=lambda row: (-row.z_score, -row.calls, row.target))
        kept = [row for row in rows if row.tier == "candidate"]
        kept += [row for row in rows if row.tier == "weak"][:weak]
        result[name] = kept
    return result


def load_table(path: Path) -> tuple[list[int], list[tuple[int, int]]] | None:
    """Sorted function entries and (entry, end) spans of the shared function table, None when the export is absent."""
    if not path.is_file():
        return None
    from tools.codediff.boundaries import load_function_table_for_export

    functions = sorted(load_function_table_for_export(path), key=lambda item: item.entry_va)
    entries = [function.entry_va for function in functions]
    spans = [
        (function.entry_va, function.entry_va + max(function.size_bytes, 1))
        for function in functions
    ]
    return entries, spans


def containing(table: tuple[list[int], list[tuple[int, int]]] | None, target: int) -> str:
    """`entry` when the target is a function entry, the owning entry when it falls inside one, else ``."""
    if table is None:
        return ""
    entries, spans = table
    index = bisect.bisect_right(entries, target) - 1
    if index < 0:
        return ""
    entry, end = spans[index]
    if entry == target:
        return "entry"
    return f"inside 0x{entry:08X}" if target < end else ""


def evidence(candidate: Candidate) -> str:
    where = f"{candidate.calls} calls in {candidate.presents} presents"
    if candidate.kind == "exclusive":
        what = f"called only during {candidate.scenario} ({where})"
    elif candidate.kind == "idle-absent":
        what = f"called during {candidate.scenario} and not in the idle control, also in {', '.join(candidate.also_active_in)} ({where})"
    else:
        what = (
            f"called during {candidate.scenario} at {candidate.ratio:.1f}x the idle rate ({where})"
        )
    return (
        f"host census: {what}; z {candidate.z_score:.1f}, p {candidate.p_value:.2g}; owner session, MEASURED call, "
        "name INFERRED; indirect entry point only, direct callees unattributed"
    )


def write_outputs(
    session: Path,
    ranked: dict[str, list[Candidate]],
    scenarios: dict[str, dict],
    names: dict[int, str],
    table: tuple[list[int], list[tuple[int, int]]] | None,
) -> list[Path]:
    candidates = session / "candidates.csv"
    template = session / "names_template.csv"
    with candidates.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "entry_va",
                "suggested_scope",
                "scenario",
                "tier",
                "kind",
                "calls",
                "presents",
                "rate",
                "idle_rate",
                "unit",
                "calls_per_present",
                "p",
                "z",
                "exclusivity",
                "also_active_in",
                "existing_name",
                "function_table",
                "first_caller",
                "thread",
            ]
        )
        for scenario, rows in ranked.items():
            for row in rows:
                writer.writerow(
                    [
                        f"0x{row.target:08X}",
                        suggested_scope(scenario, scenarios[scenario]["group"]),
                        scenario,
                        row.tier,
                        row.kind,
                        row.calls,
                        row.presents,
                        f"{row.rate:.4f}",
                        f"{row.idle_rate:.4f}",
                        f"per_{row.unit}",
                        f"{row.per_present:.3f}",
                        f"{row.p_value:.3g}",
                        f"{row.z_score:.2f}",
                        f"{row.exclusivity:.2f}",
                        ";".join(row.also_active_in),
                        names.get(row.target, ""),
                        containing(table, row.target),
                        f"0x{row.first_caller:08X}",
                        row.thread,
                    ]
                )
    with template.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["entry_va", "name", "confidence", "evidence"])
        seen: set[int] = set()
        for scenario, rows in ranked.items():
            for row in rows:
                if row.tier != "candidate" or row.target in names or row.target in seen:
                    continue
                seen.add(row.target)
                writer.writerow(
                    [
                        f"0x{row.target:08X}",
                        f"{suggested_scope(scenario, scenarios[scenario]['group'])}_ROLE",
                        "INFERRED",
                        evidence(row),
                    ]
                )
    return [candidates, template]


def print_table(rows: list[Candidate], names: dict[int, str], top: int) -> None:
    print(
        f"  {'target':12s} {'kind':12s} {'calls':>7s} {'rate':>9s} {'idle':>9s} {'z':>7s} {'p':>8s} {'excl':>5s}  current name; also in"
    )
    for row in rows[:top]:
        also = f"; also in {', '.join(row.also_active_in)}" if row.also_active_in else ""
        print(
            f"  0x{row.target:08X}  {row.kind:12s} {row.calls:7d} {row.rate:9.3f} {row.idle_rate:9.3f} {row.z_score:7.1f} "
            f"{row.p_value:8.2g} {row.exclusivity:5.2f}  {names.get(row.target, '(unnamed)')}{also}"
        )


def print_report(
    ranked: dict[str, list[Candidate]],
    scenarios: dict[str, dict],
    phases: dict[str, Phase],
    names: dict[int, str],
    top: int,
) -> None:
    for scenario, rows in ranked.items():
        info = scenarios[scenario]
        phase = phases[scenario]
        seconds = f", {phase.seconds:.0f} s" if phase.seconds else ""
        print(
            f"\n== {scenario} ({info['title']}, {info['kind']}, {phase.total_calls} indirect calls, "
            f"{len(phase.targets)} targets, {phase.span} presents{seconds}) =="
        )
        if info["kind"] == "control":
            print("  control: used as the reference, no candidates")
            continue
        candidates = [row for row in rows if row.tier == "candidate"]
        weak = [row for row in rows if row.tier == "weak"]
        unit = rows[0].unit if rows else "present"
        print(f"  rates are calls per {unit}; z and p: Poisson, X above idle")
        if candidates:
            print("  candidates:")
            print_table(candidates, names, top)
        else:
            print("  no candidate target passed the gate")
        if weak:
            print("  weak signals (rate above idle, below the candidate gate):")
            print_table(weak, names, top)
    print(
        "\nNaming rows (tools/data/function_names.csv): name game_<scenario>_<role>, confidence INFERRED, evidence "
        "'host census: called only during <scenario> (N calls in P presents)'. names_template.csv has them prefilled (candidates only)."
    )
    print("NOTE: " + NO_INDIRECT_NOTE)


def has_sampler_only(session: Path) -> bool:
    return any(
        not path.name.endswith(SKIP_SUFFIXES + (".ms",)) for path in session.glob("profile.txt.*")
    )


def main(argv: list[str] | None = None) -> int:
    if argv is None:
        argv = sys.argv[1:]
    if argv and argv[0] == "compare":
        from tools import action_compare

        return action_compare.main(argv[1:])
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("session_dir", type=Path)
    parser.add_argument("--top", type=int, default=15)
    parser.add_argument("--min-calls", type=int, default=3)
    parser.add_argument("--min-presents", type=int, default=2)
    parser.add_argument("--min-ratio", type=float, default=3.0)
    parser.add_argument("--max-p", type=float, default=0.05)
    parser.add_argument("--weak", type=int, default=15, help="weak signals kept per scenario")
    parser.add_argument(
        "--functions",
        type=Path,
        default=FUNCTIONS_FILE,
        help="function table export (default generated/retail/functions.csv)",
    )
    args = parser.parse_args(argv)
    session = args.session_dir
    if not session.is_dir():
        print(f"not a folder: {session}", file=sys.stderr)
        return 2
    scenarios = {s["id"]: s for s in json.loads(SCENARIO_FILE.read_text())["scenarios"]}
    phases, warnings = discover(session, scenarios)
    if not phases:
        if has_sampler_only(session):
            print(
                f"This session has no census: {session} holds only CPU sampler files (profile.txt.<phase>). "
                "Analyse it with `python -m tools.action_profile analyze` or record a new session with "
                "`python -m tools.action_profile session --group menu --source census ...`.",
                file=sys.stderr,
            )
            return 3
        print(
            f"no census files found in {session} (expected census.txt.<scenario>)", file=sys.stderr
        )
        return 2
    names = load_names(NAMES_FILE)
    table = load_table(args.functions)
    ranked = rank(
        phases, scenarios, args.min_calls, args.min_presents, args.min_ratio, args.max_p, args.weak
    )
    if not any(scenarios[s]["kind"] == "control" for s in phases):
        warnings.append(
            "no idle control census: a target common to every action cannot be told apart from an action target"
        )
    print_report(ranked, scenarios, phases, names, args.top)
    for path in write_outputs(session, ranked, scenarios, names, table):
        print(f"wrote {path}")
    for warning in warnings:
        print(f"warning: {warning}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
