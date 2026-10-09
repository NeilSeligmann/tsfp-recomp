# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line of `python -m tools.subsystems`: seed, report, check.

XBE DEPENDENCY. The function table (`generated/retail/functions.csv`, gitignored), the XBE
(the pinned retail `default.xbe`) and the proof snapshot are needed to SEED and to recompute the
report. `seed` and `check` need the XBE. `report` without an XBE (none given and none under
`tmp/oxm-extract/retail/`, `extracted/` or `generated/retail/`) prints the committed snapshot
`docs/data/subsystem-coverage.json`; with no snapshot either it exits 2 with a message.
`check` without an XBE only runs the offline consistency checks of the committed files.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from tools.coverage import REGION_GAME
from tools.subsystems.callgraph import build_graph
from tools.subsystems.loader import (
    DEFAULT_GENERATED,
    DEFAULT_PROOF_SNAPSHOT,
    Loaded,
    MissingInputs,
    find_xbe,
    load_facts,
    load_proven,
    load_registered,
)
from tools.subsystems.report import (
    Report,
    build_report,
    render_csv,
    render_json,
    render_markdown,
    render_text,
    tally,
)
from tools.subsystems.rules import MEASURED
from tools.subsystems.seed import SeedResult, rule_hits, seed
from tools.subsystems.table import (
    TABLE_PATH,
    Row,
    format_address,
    read_table,
    render_table,
    validate_rows,
)
from tools.subsystems.tree import DEFAULT_TREE, UNKNOWN_PATH, Tree, load_tree

DOC_PATH = Path("docs/subsystem-coverage.md")
CSV_PATH = Path("docs/data/subsystem-coverage.csv")
JSON_PATH = Path("docs/data/subsystem-coverage.json")
EXIT_STALE = 1
EXIT_MISSING = 2


@dataclass(frozen=True)
class Paths:
    root: Path
    table: Path
    tree: Path

    @classmethod
    def of(cls, args: argparse.Namespace) -> Paths:
        root = Path(args.root)
        return cls(root, root / args.table, root / args.tree_file)


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--root", default=".", help="repository root (default: cwd)")
    parser.add_argument("--xbe", help="the pinned retail default.xbe (default: search --root)")
    parser.add_argument(
        "--generated-dir",
        default=str(DEFAULT_GENERATED),
        help="dir with functions.csv and flirt_names.csv (relative to --root unless absolute)",
    )
    parser.add_argument(
        "--proof-snapshot",
        default=str(DEFAULT_PROOF_SNAPSHOT),
        help="tracked proof snapshot (default: %(default)s)",
    )
    parser.add_argument("--table", default=str(TABLE_PATH), help="the placement table")
    parser.add_argument("--tree-file", default=str(DEFAULT_TREE), help="the subsystem tree")


def add_seed_flags(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--no-callgraph",
        action="store_true",
        help="skip call graph propagation (default: on, about 10 s)",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="python -m tools.subsystems", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    seed_parser = sub.add_parser("seed", help="regenerate tools/data/function_subsystems.csv")
    add_common(seed_parser)
    add_seed_flags(seed_parser)
    seed_parser.add_argument("--out", help="write here instead of --table")
    seed_parser.add_argument("--stats", action="store_true", help="print rule hit counts")

    report_parser = sub.add_parser("report", help="per-subsystem counts")
    add_common(report_parser)
    report_parser.add_argument("--tree", action="store_true", help="indented tree with rollups")
    report_parser.add_argument("--format", choices=("text", "csv", "json"), default="text")
    report_parser.add_argument(
        "--write", action="store_true", help=f"write {DOC_PATH}, {CSV_PATH} and {JSON_PATH}"
    )

    check_parser = sub.add_parser("check", help="fail when the committed outputs are stale")
    add_common(check_parser)
    add_seed_flags(check_parser)
    return parser


def resolve(args: argparse.Namespace) -> tuple[Path | None, Path]:
    root = Path(args.root)
    xbe = find_xbe(root, args.xbe)
    generated = Path(args.generated_dir)
    return xbe, generated if generated.is_absolute() else root / generated


def load(args: argparse.Namespace, xbe: Path, generated: Path) -> Loaded:
    snapshot = Path(args.proof_snapshot)
    root = Path(args.root)
    return load_facts(
        root, xbe, generated, proof_snapshot=snapshot if snapshot.is_absolute() else root / snapshot
    )


def seed_table(loaded: Loaded, tree: Tree, *, callgraph: bool) -> SeedResult:
    callers = taken = None
    if callgraph:
        sizes = {fact.va: fact.size for fact in loaded.facts}
        callers, taken = build_graph(loaded.xbe, loaded.functions_csv, sizes)
    return seed(loaded.facts, tree, callers=callers, address_taken=taken or ())


def print_seed_summary(result: SeedResult, *, stats: bool) -> None:
    confidence = Counter(row.confidence for row in result.rows)
    unknown = sum(1 for row in result.rows if row.subsystem == UNKNOWN_PATH)
    print(
        f"seeded {len(result.rows):,} functions: MEASURED {confidence['MEASURED']:,}, "
        f"INFERRED {confidence['INFERRED']:,}, UNKNOWN {unknown:,}; call graph "
        f"{'on' if result.callgraph_used else 'off'}",
        file=sys.stderr,
    )
    if result.anchors_missing:
        listed = ", ".join(format_address(va) for va in result.anchors_missing)
        print(f"warning: anchors not in the function table: {listed}", file=sys.stderr)
    if stats:
        hits = rule_hits(result.decisions)
        for (group, label, subsystem), count in sorted(
            hits.items(), key=lambda item: (item[0][0], -item[1], item[0][1])
        ):
            print(f"{count:7,}  {group or '-':9}  {label:48}  {subsystem}", file=sys.stderr)


def command_seed(args: argparse.Namespace) -> int:
    paths = Paths.of(args)
    xbe, generated = resolve(args)
    if xbe is None:
        print("seed needs the retail XBE: pass --xbe PATH", file=sys.stderr)
        return EXIT_MISSING
    tree = load_tree(paths.tree)
    result = seed_table(load(args, xbe, generated), tree, callgraph=not args.no_callgraph)
    print_seed_summary(result, stats=args.stats)
    out = Path(args.out) if args.out else paths.table
    out.write_text(render_table(result.rows), encoding="utf-8")
    print(f"wrote {out}", file=sys.stderr)
    return 0


def check_table_matches(rows: Sequence[Row], loaded: Loaded) -> list[str]:
    have = {row.va for row in rows}
    want = {fact.va for fact in loaded.facts}
    problems = []
    if have - want:
        problems.append(f"{len(have - want)} table addresses are not functions (stale table)")
    if want - have:
        problems.append(f"{len(want - have)} functions are missing from the table (stale table)")
    return problems


def compute_report(tree: Tree, rows: Sequence[Row], loaded: Loaded) -> Report:
    named = {fact.va for fact in loaded.facts if not _placeholder(fact.name)}
    game = sum(1 for fact in loaded.facts if fact.region == REGION_GAME)
    proven, proven_why = load_proven(loaded)
    registered, registered_why = load_registered(loaded)
    unavailable = {
        name: why for name, why in (("proven", proven_why), ("registered", registered_why)) if why
    }
    return build_report(
        tree,
        rows,
        named,
        proven,
        game=game,
        library=len(loaded.facts) - game,
        registered=registered,
        unavailable=unavailable,
        xbe_sha256=loaded.xbe_sha256,
    )


def _placeholder(name: str) -> bool:
    from tools.coverage import is_placeholder_name

    return is_placeholder_name(name)


def outputs(report: Report) -> dict[Path, str]:
    return {
        DOC_PATH: render_markdown(report),
        CSV_PATH: render_csv(report),
        JSON_PATH: render_json(report),
    }


def command_report(args: argparse.Namespace) -> int:
    paths = Paths.of(args)
    tree = load_tree(paths.tree)
    xbe, generated = resolve(args)
    if xbe is None:
        if args.write:
            print("--write recomputes the report and needs --xbe PATH", file=sys.stderr)
            return EXIT_MISSING
        committed = paths.root / JSON_PATH
        if not committed.is_file():
            print(
                "no XBE found and no committed snapshot: pass --xbe PATH (and "
                "--generated-dir) or run `report --write` once",
                file=sys.stderr,
            )
            return EXIT_MISSING
        report = Report.from_json(json.loads(committed.read_text(encoding="utf-8")))
        note = f"(committed snapshot {JSON_PATH}, no XBE available to recompute)"
        report = report.with_notes(note)
    else:
        loaded = load(args, xbe, generated)
        if paths.table.is_file():
            rows = read_table(paths.table)
            problems = check_table_matches(rows, loaded) + validate_rows(rows, tree)
            if problems:
                print("\n".join(problems[:20]), file=sys.stderr)
                print("run `python -m tools.subsystems seed`", file=sys.stderr)
                return EXIT_STALE
        else:
            # The table is gitignored (the repository hook rejects a tracked per-address dump), so
            # a fresh checkout seeds it in memory instead of failing.
            rows = seed_table(loaded, tree, callgraph=not getattr(args, "no_callgraph", False)).rows
            print(f"{paths.table}: absent, seeded in memory", file=sys.stderr)
        report = compute_report(tree, rows, loaded)
    if args.write:
        for relative, text in outputs(report).items():
            (paths.root / relative).write_text(text, encoding="utf-8")
            print(f"wrote {relative}", file=sys.stderr)
        return 0
    renderers = {"csv": render_csv, "json": render_json}
    sys.stdout.write(
        renderers[args.format](report)
        if args.format in renderers
        else render_text(report, tree=args.tree)
    )
    return 0


def check_offline(paths: Paths, tree: Tree) -> list[str]:
    """Consistency of the committed files with each other, no XBE needed."""
    problems: list[str] = []
    if not paths.table.is_file():
        return []  # gitignored, regenerated by `seed`; nothing to compare without the XBE
    rows = read_table(paths.table)
    problems += validate_rows(rows, tree)
    committed = paths.root / JSON_PATH
    if not committed.is_file():
        return [*problems, f"{JSON_PATH}: missing"]
    report = Report.from_json(json.loads(committed.read_text(encoding="utf-8")))
    own, unknown = tally(rows, (), ())
    if report.functions != len(rows):
        problems.append(f"{JSON_PATH}: {report.functions} functions but the table has {len(rows)}")
    if (report.unknown.functions, report.unknown.measured, report.unknown.inferred) != (
        unknown.functions,
        unknown.measured,
        unknown.inferred,
    ):
        problems.append(f"{JSON_PATH}: unknown counts differ from the table")
    for entry in report.entries:
        counts = own.get(entry.path)
        have = (entry.own.functions, entry.own.measured, entry.own.inferred)
        want = (counts.functions, counts.measured, counts.inferred) if counts else (0, 0, 0)
        if have != want:
            problems.append(f"{JSON_PATH}: {entry.path} own {have} but the table says {want}")
    return problems


def command_check(args: argparse.Namespace) -> int:
    paths = Paths.of(args)
    tree = load_tree(paths.tree)
    xbe, generated = resolve(args)
    if xbe is None:
        problems = check_offline(paths, tree)
        for problem in problems:
            print(f"stale: {problem}", file=sys.stderr)
        print(
            "partial check only (no XBE): table vocabulary and its agreement with the committed "
            "json. Pass --xbe for the full check.",
            file=sys.stderr,
        )
        return EXIT_STALE if problems else 0
    loaded = load(args, xbe, generated)
    result = seed_table(loaded, tree, callgraph=not args.no_callgraph)
    stale: list[str] = []
    expected = render_table(result.rows)
    if paths.table.is_file() and paths.table.read_text(encoding="utf-8") != expected:
        stale.append(
            str(
                paths.table.relative_to(paths.root)
                if paths.root in paths.table.parents
                else paths.table
            )
        )
    report = compute_report(tree, result.rows, loaded)
    for relative, text in outputs(report).items():
        target = paths.root / relative
        if not target.is_file() or target.read_text(encoding="utf-8") != text:
            stale.append(str(relative))
    for name in stale:
        print(f"stale: {name}", file=sys.stderr)
    if stale:
        print("regenerate: seed, then report --write", file=sys.stderr)
    return EXIT_STALE if stale else 0


COMMANDS = {"seed": command_seed, "report": command_report, "check": command_check}


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return COMMANDS[args.command](args)
    except MissingInputs as error:
        print(str(error), file=sys.stderr)
        return EXIT_MISSING


__all__ = ["main", "MEASURED"]
