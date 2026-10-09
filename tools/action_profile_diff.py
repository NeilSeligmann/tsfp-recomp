#!/usr/bin/env python3
# ruff: noqa: E501
"""T1495: rank the functions that run in one played action but not in the idle control or the other actions.

Input: a session folder written by `python -m tools.action_profile session` (profile.txt.<phase> files from
`--cpu-profile` or `--cpu-profile-wall`, plus session.json). The sample addresses are resolved with addr2line
(tools/cpu_profile_report.py) into functions of the lifted host (`sub_XXXXXXXX` is the guest function at VA
0xXXXXXXXX) and checked against the shared function table (tools.codediff.boundaries.load_function_table).

For a scenario X of group G the references are the idle control(s) of G and the other scenarios of G.
  rate(f, phase)  = samples in f / all samples of the phase
  lift            = (rate_X + e) / (max reference rate + e)         e = 0.5 / samples_X
  presence        = share of the references in which f has at least one sample
  score           = samples_X * log2(lift) * (1 - presence / 2)     only when lift >= --min-lift and samples_X >= --min-samples
A function that is hot in the idle control has lift near 1 and never ranks. Evidence strength: MEASURED that the function
was sampled during the action, INFERRED that it implements it (animation and background work share the same time).

Usage: python -m tools.action_profile_diff SESSION_DIR [--top 15] [--all-frames] [--host tsfp_host]
Writes SESSION_DIR/candidates.csv (entry_va,suggested_scope,scenario,lift,samples,existing_name,...) and names_template.csv.
"""

from __future__ import annotations

import argparse
import collections
import csv
import json
import math
import re
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from tools import cpu_profile_report as report
from tools.action_stats import rate_test

ROOT = Path(__file__).resolve().parent.parent
SCENARIO_FILE = ROOT / "tools/data/action_scenarios.json"
NAMES_FILE = ROOT / "tools/data/function_names.csv"
FUNCTIONS_FILE = Path("generated/retail/functions.csv")
GUEST_RE = re.compile(r"^sub_([0-9A-Fa-f]{8})$")
REPEAT_RE = re.compile(r"-r\d+$")
SKIP_SUFFIXES = (".ms", ".pid", ".ack", ".phase")


@dataclass
class Row:
    function: str
    scenario: str
    samples: int
    rate: float  # samples per exposure unit (second when known, else per sample of the phase)
    lift: float
    presence: float  # share of the other scenarios of the group that also sample it
    score: float  # the Poisson z against the idle control
    reference_rate: float
    p_value: float = 1.0
    tier: str = "candidate"  # candidate | weak
    also_active_in: tuple[str, ...] = ()
    exclusivity: float = 1.0  # 1 - share of the other ACTIONS that also sample it
    unit: str = "sample"


def phase_scenario(phase: str, scenario_ids: set[str]) -> str | None:
    """The scenario a phase file belongs to (`ingame-fire`, `ingame-fire-r2`), None for navigation, interrupted and tail."""
    base = REPEAT_RE.sub("", phase).replace(
        LABEL_FILE_SEP, "@", 1
    )  # `ingame-fire--shotgun` is the file of `ingame-fire@shotgun`
    scenario, label = split_key(base)
    if scenario not in scenario_ids or (label is not None and not LABEL_RE.fullmatch(label)):
        return None
    return base


LABEL_RE = re.compile(r"[a-z0-9_]+")
LABEL_FILE_SEP = "--"  # the host drops '@' from a phase name, so a labeled phase is saved as FILE.<scenario>--<label>


def split_key(key: str) -> tuple[str, str | None]:
    """`ingame-fire@shotgun` -> (`ingame-fire`, `shotgun`), a plain id -> (id, None). T1502 per-phase labels."""
    base, sep, label = key.partition("@")
    return base, (label if sep else None)


def register_key(scenarios: dict[str, dict], key: str) -> bool:
    """True when the key is a known scenario. A labeled key `X@label` of a known X is added to `scenarios` as its own
    scenario row (same group and kind as X, so it is ranked against the group's idle control), never merged with X."""
    if key in scenarios:
        return True
    base, label = split_key(key)
    if label is None or base not in scenarios or not LABEL_RE.fullmatch(label):
        return False
    info = dict(scenarios[base])
    info.update(base=base, label=label, title=f"{info['title']} [{label}]")
    scenarios[key] = info
    return True


def phase_key(info: dict | None, phase: str, scenario_ids: set[str]) -> str | None:
    """The scenario row of a saved phase: session.json scenario (+ label) when known, else parsed from the file name."""
    if info and info.get("scenario"):
        label = info.get("label")
        return f"{info['scenario']}@{label}" if label else info["scenario"]
    return phase_scenario(phase, scenario_ids)


def pool(
    counts: dict[str, collections.Counter[str]], totals: dict[str, int], members: list[str]
) -> tuple[collections.Counter[str], int]:
    merged: collections.Counter[str] = collections.Counter()
    for member in members:
        merged.update(counts[member])
    return merged, sum(totals.get(member, 0) for member in members)


def rank(
    counts: dict[str, collections.Counter[str]],
    totals: dict[str, int],
    scenarios: dict[str, dict],
    min_lift: float = 3.0,
    min_samples: int = 3,
    seconds: dict[str, float] | None = None,
    max_p: float = 0.05,
    min_rate: float = 0.2,
    weak: int = 15,
) -> dict[str, list[Row]]:
    """Per action scenario the candidate functions, then its top weak signals, best first.

    T1502 rule (owner sessions: real signal is shared between actions, so exclusivity is information, not a gate): X is
    compared with the IDLE control of its group only (the pooled other scenarios, and a warning from the caller, when the
    group has none). Rates are per second when `seconds` has both phases, else per sample of the phase. p and z: exact
    Poisson comparison (tools/action_stats.py), one sided, X above the reference. CANDIDATE: rate above the reference, p <=
    max_p, (samples >= min_samples or rate >= min_rate per second) and, when the reference has the function, lift >= min_lift.
    WEAK: rate above the reference but not a candidate (the top `weak` by z are kept).
    """
    result: dict[str, list[Row]] = {}
    seconds = seconds or {}
    for name, count in counts.items():
        total = totals.get(name, 0)
        group = scenarios[name]["group"]
        others = [
            other
            for other in counts
            if other != name and scenarios[other]["group"] == group and totals.get(other, 0) > 0
        ]
        idles = [other for other in others if scenarios[other]["kind"] == "control"]
        actions = [other for other in others if scenarios[other]["kind"] != "control"]
        if total <= 0 or scenarios[name]["kind"] == "control":  # a control is only a reference
            result[name] = []
            continue
        references = idles or others
        ref_count, ref_total = pool(counts, totals, references)
        use_seconds = (
            seconds.get(name, 0.0) > 0
            and bool(references)
            and all(seconds.get(r, 0.0) > 0 for r in references)
        )
        expo = seconds[name] if use_seconds else float(total)
        ref_expo = sum(seconds[r] for r in references) if use_seconds else float(ref_total)
        rows: list[Row] = []
        for function, samples in count.items():
            reference_samples = ref_count[function] if references else 0
            rate = samples / expo
            reference_rate = reference_samples / ref_expo if ref_expo > 0 else 0.0
            if references and rate <= reference_rate:
                continue
            p_value, z_score = rate_test(samples, expo, reference_samples, ref_expo)
            lift = rate / reference_rate if reference_rate > 0 else math.inf
            floor = samples >= min_samples or (use_seconds and rate >= min_rate)
            gate = floor and p_value <= max_p and (reference_samples == 0 or lift >= min_lift)
            presence = (
                sum(1 for other in others if counts[other][function] > 0) / len(others)
                if others
                else 0.0
            )
            also = tuple(sorted(other for other in actions if counts[other][function] > 0))
            exclusivity = 1.0 - len(also) / len(actions) if actions else 1.0
            rows.append(
                Row(
                    function,
                    name,
                    samples,
                    rate,
                    lift,
                    presence,
                    z_score,
                    reference_rate,
                    p_value,
                    "candidate" if gate else "weak",
                    also,
                    exclusivity,
                    "second" if use_seconds else "sample",
                )
            )
        rows.sort(key=lambda row: (-row.score, -row.samples, row.function))
        kept = [row for row in rows if row.tier == "candidate"]
        kept += [row for row in rows if row.tier == "weak"][:weak]
        result[name] = kept
    return result


def load_names(path: Path) -> dict[int, str]:
    names: dict[int, str] = {}
    if path.is_file():
        with path.open(newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(line for line in handle if not line.startswith("#")):
                try:
                    names[int(row["entry_va"], 16)] = row["name"]
                except (KeyError, ValueError):
                    continue
    return names


def discover(session: Path, scenarios: dict[str, dict]) -> tuple[dict[str, list[Path]], list[str]]:
    """Profile files per scenario id and warnings. Uses session.json when present, else the file names."""
    warnings: list[str] = []
    ids = set(scenarios)
    found: dict[str, list[Path]] = collections.defaultdict(list)
    meta_file = session / "session.json"
    meta = json.loads(meta_file.read_text()) if meta_file.is_file() else {}
    saved = {p.get("file", p["name"]): p for p in meta.get("phases", [])}
    for path in sorted(session.glob("profile.txt.*")):
        if path.name.endswith(SKIP_SUFFIXES):
            continue
        phase = path.name[len("profile.txt.") :]
        info = saved.get(phase)
        if info is not None and (not info.get("saved", True) or info["kind"] == "navigation"):
            continue
        scenario = phase_key(info, phase, ids)
        if scenario is not None and register_key(scenarios, scenario):
            found[scenario].append(path)
    for path in sorted(session.glob("*.whole")):
        scenario = path.name[: -len(".whole")]
        if scenario in ids:
            found[scenario].append(path)
    for scenario in meta.get("scenario_order", []):
        if register_key(scenarios, scenario) and scenario not in found:
            warnings.append(f"no profile for {scenario} (skipped or not saved)")
    return dict(found), warnings


def scenario_seconds(session: Path) -> dict[str, float]:
    """Wall seconds per scenario from session.json (phases saved for that scenario, repeats added), {} when unknown."""
    meta_file = session / "session.json"
    if not meta_file.is_file():
        return {}
    seconds: dict[str, float] = collections.defaultdict(float)
    for info in json.loads(meta_file.read_text()).get("phases", []):
        if info.get("saved", True) and info.get("kind") != "navigation" and info.get("scenario"):
            key = f"{info['scenario']}@{info['label']}" if info.get("label") else info["scenario"]
            seconds[key] += float(info.get("seconds") or 0.0)
    return dict(seconds)


def read_samples(path: Path) -> tuple[int, str, str, list[tuple[int, list[int]]]]:
    """(base, exe, mode, [(tid, rips)]). A missing, empty or unreadable file is zero samples."""
    try:
        text = path.read_text()
    except (OSError, UnicodeDecodeError):
        return 0, "", "cpu", []
    base, exe, mode = 0, "", "cpu"
    chains: list[tuple[int, list[int]]] = []
    for line in text.splitlines():
        if line.startswith("# base "):
            parts = line.split(" ", 3)
            if len(parts) == 4:
                base, exe = int(parts[2], 16), parts[3]
        elif line.startswith("# mode "):
            mode = line.split()[2]
        elif line and not line.startswith("#"):
            fields = line.split()
            try:
                chains.append((int(fields[0]), [int(value, 16) for value in fields[1:]]))
            except ValueError:
                continue
    return base, exe, mode, chains


def count_functions(
    files: dict[str, list[Path]],
    host: Path | None,
    all_frames: bool,
    resolver: Callable[[str, list[int]], dict[int, str]] | None = None,
) -> tuple[dict[str, collections.Counter[str]], dict[str, int], list[str]]:
    resolver = resolver or report.resolve
    warnings: list[str] = []
    parsed: dict[str, list[tuple[int, str, list[tuple[int, list[int]]]]]] = {}
    wanted: dict[str, set[int]] = collections.defaultdict(set)
    for scenario, paths in files.items():
        for path in paths:
            base, exe, mode, chains = read_samples(path)
            if not chains:
                warnings.append(f"{path.name}: no samples")
                continue
            exe_path = str(host) if host else exe
            size = Path(exe_path).stat().st_size if exe_path and Path(exe_path).is_file() else 0
            limit = max(size * 4, 1 << 28)
            adjust = 1 if mode == "wall" else 0
            offsets = [
                [
                    rip - base - adjust
                    for rip in rips[: (None if all_frames else 1)]
                    if 0 <= rip - base < limit
                ]
                for _tid, rips in chains
            ]
            parsed.setdefault(scenario, []).append(
                (0, exe_path, [(i, [o for o in offs]) for i, offs in enumerate(offsets)])
            )
            for offs in offsets:
                wanted[exe_path].update(offs)
    names: dict[str, dict[int, str]] = {}
    for exe_path, offsets in wanted.items():
        if offsets and (host is not None or Path(exe_path).is_file()):
            names[exe_path] = resolver(exe_path, sorted(offsets))
        elif offsets:
            warnings.append(f"host binary {Path(exe_path).name} not found, pass --host")
    counts: dict[str, collections.Counter[str]] = {}
    totals: dict[str, int] = {}
    for scenario, items in parsed.items():
        counts[scenario] = collections.Counter()
        totals[scenario] = 0
        for _zero, exe_path, rows in items:
            table = names.get(exe_path, {})
            for _index, offs in rows:
                totals[scenario] += 1
                seen = {table.get(offset, "??") for offset in offs}
                for function in seen - {"??"}:
                    counts[scenario][function] += 1
    return counts, totals, warnings


def suggested_scope(scenario: str, group: str) -> str:
    base, label = split_key(scenario)
    rest = base.split("-", 1)[1] if "-" in base else base
    if label:
        return f"game_{label}_{rest.replace('-', '_')}"
    rest = rest.replace("-", "_")
    return f"game_{rest}" if group == "ingame" else f"game_menu_{rest}"


def write_outputs(
    session: Path,
    ranked: dict[str, list[Row]],
    scenarios: dict[str, dict],
    names: dict[int, str],
    known: set[int] | None,
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
                "lift",
                "samples",
                "existing_name",
                "score",
                "presence",
                "in_function_table",
                "tier",
                "rate",
                "p",
                "exclusivity",
                "also_active_in",
            ]
        )
        for scenario, rows in ranked.items():
            for row in rows:
                match = GUEST_RE.match(row.function)
                if match is None:
                    continue
                entry = int(match.group(1), 16)
                writer.writerow(
                    [
                        f"0x{entry:08X}",
                        suggested_scope(scenario, scenarios[scenario]["group"]),
                        scenario,
                        f"{row.lift:.1f}" if math.isfinite(row.lift) else "inf",
                        row.samples,
                        names.get(entry, ""),
                        f"{row.score:.1f}",
                        f"{row.presence:.2f}",
                        "" if known is None else int(entry in known),
                        row.tier,
                        f"{row.rate:.4f}",
                        f"{row.p_value:.3g}",
                        f"{row.exclusivity:.2f}",
                        ";".join(row.also_active_in),
                    ]
                )
    with template.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["entry_va", "name", "confidence", "evidence"])
        for scenario, rows in ranked.items():
            for row in rows:
                match = GUEST_RE.match(row.function)
                if match is None:
                    continue
                entry = int(match.group(1), 16)
                if row.tier != "candidate" or entry in names:
                    continue
                lift = f"{row.lift:.1f}" if math.isfinite(row.lift) else "inf"
                writer.writerow(
                    [
                        f"0x{entry:08X}",
                        f"{suggested_scope(scenario, scenarios[scenario]['group'])}_ROLE",
                        "INFERRED",
                        f"host trace: {'active only during' if not row.also_active_in else 'more active than idle during'} {scenario} (lift {lift}, {row.samples} samples, z {row.score:.1f}){'' if not row.also_active_in else ', also in ' + ', '.join(row.also_active_in)}; owner session, MEASURED activity, name INFERRED from behaviour",
                    ]
                )
    return [candidates, template]


def print_report(
    ranked: dict[str, list[Row]],
    scenarios: dict[str, dict],
    totals: dict[str, int],
    names: dict[int, str],
    top: int,
) -> None:
    for scenario, rows in ranked.items():
        info = scenarios[scenario]
        print(
            f"\n== {scenario} ({info['title']}, {info['kind']}, {totals.get(scenario, 0)} samples) =="
        )
        if info["kind"] == "control":
            print("  control: used as a reference, no candidates")
            continue
        candidates = [row for row in rows if row.tier == "candidate"]
        weak = [row for row in rows if row.tier == "weak"]
        if not candidates:
            print("  no candidate functions (too few samples or nothing above the idle control)")
        for heading, group in (
            ("candidates", candidates),
            ("weak signals (rate above idle, below the candidate gate)", weak),
        ):
            if not group:
                continue
            print(f"  {heading}:")
            print(
                f"  {'function':16s} {'samples':>7s} {'rate':>9s} {'idle':>9s} {'z':>7s} {'p':>8s} {'excl':>5s}  current name; also in"
            )
            for row in group[:top]:
                match = GUEST_RE.match(row.function)
                current = (
                    names.get(int(match.group(1), 16), "(unnamed)") if match else "(host native)"
                )
                also = f"; also in {', '.join(row.also_active_in)}" if row.also_active_in else ""
                print(
                    f"  {row.function:16s} {row.samples:7d} {row.rate:9.4f} {row.reference_rate:9.4f} {row.score:7.1f} "
                    f"{row.p_value:8.2g} {row.exclusivity:5.2f}  {current}{also}"
                )
    print(
        "\nNaming rows (tools/data/function_names.csv): name game_<scenario>_<role>, confidence INFERRED, evidence "
        "'host trace: active only during <scenario> (lift L, N samples)'. names_template.csv has them prefilled."
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("session_dir", type=Path)
    parser.add_argument("--top", type=int, default=15)
    parser.add_argument(
        "--all-frames",
        action="store_true",
        help="wall profiles: count every frame of the call chain, not only the innermost",
    )
    parser.add_argument(
        "--host",
        type=Path,
        help="the tsfp_host binary that produced the profiles (default: the path in the profile)",
    )
    parser.add_argument("--min-lift", type=float, default=3.0)
    parser.add_argument("--max-p", type=float, default=0.05)
    parser.add_argument(
        "--min-rate",
        type=float,
        default=0.2,
        help="samples per second that pass the floor with fewer than --min-samples",
    )
    parser.add_argument("--weak", type=int, default=15, help="weak signals kept per scenario")
    parser.add_argument("--min-samples", type=int, default=3)
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
    files, warnings = discover(session, scenarios)
    if not files:
        print(
            f"no profile files found in {session} (expected profile.txt.<scenario>)",
            file=sys.stderr,
        )
        return 2
    counts, totals, more = count_functions(files, args.host, args.all_frames)
    warnings += more
    known: set[int] | None = None
    if args.functions.is_file():
        from tools.codediff.boundaries import load_function_table_for_export

        known = {function.entry_va for function in load_function_table_for_export(args.functions)}
    names = load_names(NAMES_FILE)
    ranked = rank(
        counts,
        totals,
        scenarios,
        args.min_lift,
        args.min_samples,
        scenario_seconds(session),
        args.max_p,
        args.min_rate,
        args.weak,
    )
    controls = [s for s in counts if scenarios[s]["kind"] == "control" and totals.get(s, 0) > 0]
    if not controls:
        warnings.append(
            "no idle control profile: functions common to every action cannot be told apart from action functions"
        )
    print_report(ranked, scenarios, totals, names, args.top)
    for path in write_outputs(session, ranked, scenarios, names, known):
        print(f"wrote {path}")
    for warning in warnings:
        print(f"warning: {warning}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
