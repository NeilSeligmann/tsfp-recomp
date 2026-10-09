# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerate the coverage badges, `metrics.json` and the backlog.

    uv run python -m tools.coverage_cli                 # measure and write everything
    uv run python -m tools.coverage_cli --check         # report only, write nothing
    uv run python -m tools.coverage_cli --build demo    # a different generated/ subtree

WHAT IS WRITTEN WHERE, AND WHY THE SPLIT MATTERS:

  docs/badges/*.svg       committed. Self-contained, no external service.
  docs/badges/metrics.json  committed. Carries the date, commit and artifact list.
  docs/backlog.md         committed. COUNTS AND AGGREGATES ONLY.
  generated/<build>/unnamed_functions.csv   gitignored. Per-address, never committed.
  generated/<build>/undefined_regions.csv   gitignored. Per-address, never committed.

The per-address files are bulk analysis output derived from the user's own
executable, and this repository is deliberately code-only, so they go to
`generated/` where `tools/ci/check-no-disc-data.sh` and `.gitignore` keep them out
of the index.

A missing artifact is reported LOUDLY on stderr and renders its metrics as grey
`unknown` badges. It never carries a previous value forward: a badge that silently
goes stale is worse than no badge. The one thing it reads of its own previous output is
the tracked docs/badges/metrics.json, to REFUSE (exit 3) a write that would turn a known
metric unknown or move a denominator unexplained, unless --allow-downgrade (T1462). The
hand-decompiled & proven metric is recomputed from the tracked proof snapshot when the
gitignored generated/replace artifacts are absent, never copied from a previous run.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import TextIO

from tools.coverage import (
    DEFAULT_MIN_REGION_BYTES,
    REPLACEMENT_KEY,
    SNAPSHOT_REFRESH_MARK,
    Artifacts,
    CoverageReport,
    build_report,
    discover_artifacts,
    git_commit,
    load_inputs,
    metric_losses,
    metrics_document,
    numerator_drops,
    parse_xbe,
    read_text_bytes,
    render_backlog,
    render_badge_table,
    render_summary,
    write_badges,
    write_undefined_regions_csv,
    write_unnamed_functions_csv,
)

DEFAULT_BADGE_DIR = Path("docs/badges")
DEFAULT_BACKLOG = Path("docs/backlog.md")
METRICS_FILENAME = "metrics.json"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Measure decompilation coverage; write badges, metrics.json and the backlog."
    )
    parser.add_argument("--root", type=Path, default=Path("."), help="repository root (default: .)")
    parser.add_argument(
        "--build",
        default="retail",
        help="generated/<build>/ subtree to read artifacts from (default: retail)",
    )
    parser.add_argument(
        "--xbe",
        type=Path,
        default=None,
        help="target XBE; by default the usual extraction paths are searched",
    )
    parser.add_argument(
        "--replace-dir",
        type=Path,
        default=None,
        help="directory holding the replacement manifest.json and proof.json "
        "(default: <root>/generated/replace). Without both, the hand-decompiled & proven "
        "metric renders as unknown",
    )
    parser.add_argument(
        "--cmake-build",
        type=Path,
        default=None,
        help="configured CMake build directory, asked for the C suite count via "
        "`ctest -N` (default: <root>/build if it exists). NOT the same thing as "
        "--build, which names a generated/<name>/ artifact subtree. Without it "
        "the count falls back to counting add_test( lines, which UNDERCOUNTS "
        "any suite registered inside a foreach()",
    )
    parser.add_argument(
        "--badge-dir",
        type=Path,
        default=None,
        help=f"where to write SVG badges (default: <root>/{DEFAULT_BADGE_DIR})",
    )
    parser.add_argument(
        "--backlog",
        type=Path,
        default=None,
        help=f"where to write the backlog (default: <root>/{DEFAULT_BACKLOG})",
    )
    parser.add_argument(
        "--min-region-bytes",
        type=int,
        default=DEFAULT_MIN_REGION_BYTES,
        help=(
            "smallest unclaimed .text region to report as probably-missing code "
            f"(default: {DEFAULT_MIN_REGION_BYTES})"
        ),
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="measure and print, write nothing; exits non-zero if any metric is unknown",
    )
    parser.add_argument(
        "--no-lists",
        action="store_true",
        help="skip the gitignored per-address CSVs under generated/",
    )
    parser.add_argument(
        "--no-collect-tests",
        action="store_true",
        help="do not run pytest to count tests (the test count renders as unknown)",
    )
    parser.add_argument(
        "--python",
        default=None,
        help="interpreter used to collect the Python test count "
        "(default: <root>/.venv/bin/python when it exists, else the running interpreter)",
    )
    parser.add_argument(
        "--allow-downgrade",
        action="store_true",
        help="overwrite the tracked docs/badges even when that turns a known metric unknown "
        "or moves a denominator unexplained (T1462). Without it such a write is refused",
    )
    parser.add_argument(
        "--snapshot-refresh",
        metavar="TEXT",
        default=None,
        help="mark the proven badge's note 'SNAPSHOT REFRESH: TEXT' so a deliberate drop above "
        "10 percent is accepted by the downgrade guard and tools.ci.check_badges_not_downgraded",
    )
    parser.add_argument(
        "--readme-row",
        action="store_true",
        help="print the markdown badge row for README.md and exit",
    )
    return parser


def warn_missing(
    report: CoverageReport, artifacts: Artifacts, *, stream: TextIO | None = None
) -> None:
    """Print a loud warning for every absent artifact and unknown metric."""
    out = stream if stream is not None else sys.stderr
    missing = artifacts.missing
    unknown = report.unknown_metrics
    if not missing and not unknown:
        return
    print("", file=out)
    print("!" * 72, file=out)
    print("!! COVERAGE REGENERATED WITH MISSING ARTIFACTS", file=out)
    if missing:
        print("!! absent:", ", ".join(missing), file=out)
    for metric in unknown:
        print(
            f"!!   {metric.key}: unknown"
            + (f" (needs {', '.join(metric.missing)})" if metric.missing else "")
            + (f" (not measured because {metric.unknown_reason})" if metric.unknown_reason else ""),
            file=out,
        )
    print(
        "!! Those badges render as grey 'unknown'. No previous value is carried forward.",
        file=out,
    )
    print("!" * 72, file=out)
    print("", file=out)


def warn_undercounting(report: CoverageReport, *, stream: TextIO | None = None) -> None:
    """Print every reason a metric may read LOW rather than unknown.

    A separate warning from `warn_missing` because it is a different failure. A missing
    artifact makes a badge grey, which is self-announcing. An undercount makes a badge
    GREEN AND WRONG -- it reads as a stall, or as progress that is smaller than it is,
    and nothing about the number says so.

    `Inputs.ordinal_warnings` existed, was populated, had a test pinning that it gets
    populated, and WAS PRINTED NOWHERE. The defence against a silent undercount was
    itself silent, which is how the badge once read 10 ordinals where 15 were
    registered -- the thing that was supposed to catch that had no output path.
    """
    out = stream if stream is not None else sys.stderr
    reasons = [*report.inputs.ordinal_warnings, *report.inputs.c_suite_warnings]
    overlay = report.inputs.name_overlay
    if overlay is not None:
        reasons.extend(f"name overlay REJECTED: {reason}" for reason in overlay.rejections)
    library_overlay = report.inputs.library_name_overlay
    if library_overlay is not None:
        reasons.extend(
            f"library name overlay REJECTED: {reason}" for reason in library_overlay.rejections
        )
    if not reasons:
        return
    print("", file=out)
    print("!" * 72, file=out)
    print("!! A METRIC MAY BE UNDERCOUNTING. These badges are not grey, they are wrong.", file=out)
    for reason in reasons:
        print(f"!!   {reason}", file=out)
    print("!" * 72, file=out)
    print("", file=out)


def warn_snapshot(report: CoverageReport, *, stream: TextIO | None = None) -> None:
    """Print every loud disagreement between the live proof and the tracked snapshot."""
    out = stream if stream is not None else sys.stderr
    evaluation = report.replacement
    if evaluation is None or not evaluation.disagreements:
        return
    print("", file=out)
    print("!" * 72, file=out)
    print("!! THE TRACKED PROOF SNAPSHOT DISAGREES WITH THE LIVE PROOF (live is used)", file=out)
    for line in evaluation.disagreements:
        print(f"!!   {line}", file=out)
    print("!! Either the snapshot was edited or is stale. Do not commit it as is.", file=out)
    print("!" * 72, file=out)
    print("", file=out)


def write_per_address_lists(
    report: CoverageReport, artifacts: Artifacts, *, root: Path, build: str, min_region_bytes: int
) -> list[str]:
    """Write the gitignored per-address CSVs. Returns what was written, for logging."""
    written: list[str] = []
    out_dir = root / "generated" / build
    if report.classified:
        rows = write_unnamed_functions_csv(out_dir / "unnamed_functions.csv", report.classified)
        written.append(f"{out_dir / 'unnamed_functions.csv'} ({rows} rows, gitignored)")
    if report.coverage is not None and artifacts.xbe is not None:
        data = artifacts.xbe.read_bytes()
        text, base_va = read_text_bytes(data, parse_xbe(data))
        rows = write_undefined_regions_csv(
            out_dir / "undefined_regions.csv",
            report.coverage.gaps_at_least(min_region_bytes),
            text=text,
            base_va=base_va,
        )
        written.append(f"{out_dir / 'undefined_regions.csv'} ({rows} rows, gitignored)")
    return written


def tracked_losses(
    badge_dir: Path, tracked_dir: Path, metrics_path: Path, document: dict[str, object]
) -> list[str]:
    """Losses against the previous tracked `metrics.json`, only when writing the tracked dir."""
    try:
        if badge_dir.resolve() != tracked_dir.resolve() or not metrics_path.is_file():
            return []
        previous = json.loads(metrics_path.read_text(encoding="utf-8")).get("metrics", {})
    except (OSError, ValueError):
        return []
    current = document.get("metrics", {})
    assert isinstance(current, dict)
    return [
        *metric_losses(previous, current, denominators=True),
        *numerator_drops(previous, current),
    ]


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    root: Path = args.root
    artifacts = discover_artifacts(
        root, build=args.build, xbe=args.xbe, replace_dir=args.replace_dir
    )
    cmake_build = args.cmake_build
    if cmake_build is None:
        default_build = root / "build"
        cmake_build = default_build if default_build.is_dir() else None
    inputs = load_inputs(
        artifacts,
        root=root,
        python=args.python,
        collect_tests=not args.no_collect_tests,
        cmake_build=cmake_build,
    )
    report = build_report(inputs)

    if args.readme_row:
        print(render_badge_table(report.metrics))
        return 0

    print(render_summary(report))
    warn_missing(report, artifacts)
    warn_undercounting(report)
    warn_snapshot(report)

    if args.check:
        return 1 if report.unknown_metrics else 0

    badge_dir: Path = args.badge_dir or (root / DEFAULT_BADGE_DIR)
    backlog: Path = args.backlog or (root / DEFAULT_BACKLOG)
    commit = git_commit(root)
    measured_on = datetime.now(tz=UTC).date()

    document = metrics_document(report, measured_on=measured_on, commit=commit, artifacts=artifacts)
    if args.snapshot_refresh is not None:
        proven_entry = document["metrics"].get(REPLACEMENT_KEY)  # type: ignore[union-attr]
        if isinstance(proven_entry, dict):
            proven_entry["note"] = (
                f"{proven_entry['note']} {SNAPSHOT_REFRESH_MARK}: {args.snapshot_refresh}"
            )
    metrics_path = badge_dir / METRICS_FILENAME
    losses = tracked_losses(badge_dir, root / DEFAULT_BADGE_DIR, metrics_path, document)
    if losses and not args.allow_downgrade:
        print("", file=sys.stderr)
        print("!" * 72, file=sys.stderr)
        print(
            f"!! REFUSING to overwrite {badge_dir}: this run would LOSE evidence the tracked "
            "metrics.json has:",
            file=sys.stderr,
        )
        for line in losses:
            print(f"!!   {line}", file=sys.stderr)
        print(
            "!! Run where the artifacts exist (generated/ symlinked, XBE found), or pass "
            "--allow-downgrade to accept the loss. Nothing was written.",
            file=sys.stderr,
        )
        print("!" * 72, file=sys.stderr)
        return 3
    paths = write_badges(report.metrics, badge_dir)
    metrics_path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    backlog.parent.mkdir(parents=True, exist_ok=True)
    backlog.write_text(
        render_backlog(
            report,
            measured_on=measured_on,
            commit=commit,
            min_region_bytes=args.min_region_bytes,
        ),
        encoding="utf-8",
    )

    print("")
    print(f"wrote {len(paths)} badges to {badge_dir}")
    print(f"wrote {metrics_path}")
    print(f"wrote {backlog}")
    if not args.no_lists:
        for line in write_per_address_lists(
            report,
            artifacts,
            root=root,
            build=args.build,
            min_region_bytes=args.min_region_bytes,
        ):
            print(f"wrote {line}")
    if report.inputs.name_overlay is not None and report.inputs.name_overlay.rejections:
        return 2
    if (
        report.inputs.library_name_overlay is not None
        and report.inputs.library_name_overlay.rejections
    ):
        return 2
    if report.replacement is not None and report.replacement.disagreements:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
