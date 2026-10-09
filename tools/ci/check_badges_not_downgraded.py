# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail when docs/badges would lose evidence the base already has (T1462).

    python3 -m tools.ci.check_badges_not_downgraded                  # index vs HEAD (pre-commit)
    python3 -m tools.ci.check_badges_not_downgraded --new worktree --base origin/main  # CI

Checks, on `docs/badges/metrics.json` of the NEW side against the BASE ref:

* no metric that is `known` in the base is unknown or absent in the new side
  (the f91fb37f failure: proven flipped to `unknown` from a worktree without artifacts);
* the proven metric's numerator did not fall by more than 10 percent, unless its note carries
  `DENOMINATOR CHANGE` or `SNAPSHOT REFRESH` (a documented re-measurement, 2026-10-07: the 160 to
  0 publication of a stale snapshot would have been caught here);
* every metric's SVG matches its metrics.json value, label and colour exactly.

`ALLOW_BADGE_DOWNGRADE=1` (or --allow-downgrade) accepts a deliberate downgrade.
Stdlib plus tools.coverage only, so it runs in the bare pre-commit environment.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

from tools.coverage import metric_losses, numerator_drops, render_badge

METRICS = "docs/badges/metrics.json"
BADGE_DIR = "docs/badges"


def git_show(spec: str) -> str | None:
    result = subprocess.run(  # noqa: S603 -- fixed argv, no shell
        ["git", "show", spec],
        capture_output=True,
        text=True,
        check=False,
        timeout=60,
    )
    return result.stdout if result.returncode == 0 else None


def read_side(side: str, relative: str) -> str | None:
    """`relative` on `index`, `worktree`, or a git revision."""
    if side == "worktree":
        path = Path(relative)
        return path.read_text(encoding="utf-8") if path.is_file() else None
    return git_show(f":{relative}" if side == "index" else f"{side}:{relative}")


def svg_mismatches(metrics: dict[str, dict[str, object]], side: str) -> list[str]:
    """Badges whose SVG is not byte-identical to what metrics.json says it renders."""
    problems: list[str] = []
    for key, metric in sorted(metrics.items()):
        badge = f"{BADGE_DIR}/{metric.get('badge')}"
        svg = read_side(side, badge)
        if svg is None:
            problems.append(f"{key}: {badge} is missing")
            continue
        expected = render_badge(str(metric["label"]), str(metric["value"]), str(metric["colour"]))
        if svg != expected:
            problems.append(
                f"{key}: {badge} does not render metrics.json ({metric['label']}: "
                f"{metric['value']}); the SVG and the metrics were committed from different runs"
            )
    return problems


def check(base: str, new: str) -> list[str]:
    """Every problem, as a line. Empty means the new side is acceptable."""
    new_text = read_side(new, METRICS)
    if new_text is None:
        return []  # docs/badges is not part of this change on the index side
    new_metrics = json.loads(new_text)["metrics"]
    problems: list[str] = []
    base_text = git_show(f"{base}:{METRICS}")
    if base_text is not None:
        base_metrics = json.loads(base_text)["metrics"]
        problems += metric_losses(base_metrics, new_metrics, denominators=False)
        problems += numerator_drops(base_metrics, new_metrics)
    problems += svg_mismatches(new_metrics, new)
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default="HEAD", help="ref holding the accepted badges")
    parser.add_argument(
        "--new", default="index", help="index (default), worktree, or a git revision"
    )
    parser.add_argument("--allow-downgrade", action="store_true")
    args = parser.parse_args(argv)
    problems = check(args.base, args.new)
    if not problems:
        print(f"badges: OK ({args.new} vs {args.base})")
        return 0
    allowed = args.allow_downgrade or os.environ.get("ALLOW_BADGE_DOWNGRADE") == "1"
    print(f"badges: {len(problems)} problem(s) ({args.new} vs {args.base}):", file=sys.stderr)
    for line in problems:
        print(f"  - {line}", file=sys.stderr)
    print(
        "A known badge must never be replaced by unknown. Regenerate where generated/ and the "
        "XBE exist, or restore the base badges. Set ALLOW_BADGE_DOWNGRADE=1 to override "
        "deliberately.",
        file=sys.stderr,
    )
    return 0 if allowed else 1


if __name__ == "__main__":
    raise SystemExit(main())
