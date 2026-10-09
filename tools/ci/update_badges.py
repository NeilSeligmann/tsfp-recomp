# SPDX-License-Identifier: GPL-3.0-or-later
"""Refresh CI-measurable badges; retain dated binary evidence when unavailable.

The hand-decompiled & proven badge is CI-measurable since T1462: CI has no gitignored
generated/replace, but tools.coverage replays the tracked docs/data/replace-proof-snapshot.json
over the checked-out src/game. Metrics that need the XBE are still retained from the previous
tracked metrics.json. The workflow then runs tools.ci.check_badges_not_downgraded before publishing.
"""

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from tools.agents.taskmeta import parse
from tools.coverage import STALE_SNAPSHOT_ADVICE, STALE_SNAPSHOT_MARK, render_badge


def stale_snapshot_warning(metrics: dict[str, dict[str, object]]) -> str | None:
    """The CI warning when the proven metric could not be replayed from a stale snapshot.

    The badge then keeps the previous evidence (known=False plus a missing artifact), so the log
    is the only place the staleness is visible.
    """
    proven = metrics.get("decompiled-proven")
    if proven is None or proven.get("known") or STALE_SNAPSHOT_MARK not in str(proven.get("note")):
        return None
    return f"{STALE_SNAPSHOT_MARK}: {STALE_SNAPSHOT_ADVICE}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake-build", default="build")
    args = parser.parse_args()
    destination = Path("docs/badges")
    previous = json.loads((destination / "metrics.json").read_text())
    with tempfile.TemporaryDirectory() as temporary:
        output = Path(temporary)
        subprocess.run(
            [
                sys.executable,
                "-m",
                "tools.coverage_cli",
                "--root",
                ".",
                "--cmake-build",
                args.cmake_build,
                "--badge-dir",
                str(output),
                "--backlog",
                str(output / "backlog.md"),
                "--no-lists",
            ],
            check=True,
        )
        measured = json.loads((output / "metrics.json").read_text())
        warning = stale_snapshot_warning(measured["metrics"])  # before retention swaps metrics
        retained = []
        for key, metric in measured["metrics"].items():
            old = previous["metrics"].get(key)
            if not metric["known"] and metric["artifacts_missing"] and old and old["known"]:
                measured["metrics"][key] = old
                retained.append(key)
            else:
                shutil.copyfile(output / metric["badge"], destination / metric["badge"])
        measured["retained_evidence_metrics"] = retained
        measured["unknown_metrics"] = [
            key for key, metric in measured["metrics"].items() if not metric["known"]
        ]
        ledger = parse(Path("docs/tasks.md").read_text())
        tasks = [t for t in ledger.tasks.values() if ledger.resolve(t.id, "milestone") == "M5"]
        done = sum(not task.is_open for task in tasks)
        value = f"{done}/{len(tasks)} tasks"
        (destination / "decomp-status.svg").write_text(
            render_badge("M5 decomp tasks", value, "#007ec6")
        )
        measured["decomp_task_status"] = {
            "done": done,
            "total": len(tasks),
            "commit": measured["commit"],
            "note": "M5 task completion; not a percentage of functions decompiled.",
        }
        (destination / "metrics.json").write_text(
            json.dumps(measured, indent=2, sort_keys=True) + "\n"
        )
        print("Retained dated evidence:", ", ".join(retained) or "none")
        if warning is not None:
            print(f"WARNING: {warning}", file=sys.stderr)
            print(f"::warning::{warning}")
        proven = measured["metrics"].get("decompiled-proven")
        if proven is not None:
            # Which evidence the proven badge came from is part of the CI log (T1462).
            print(
                "decompiled-proven:",
                proven["value"],
                "-",
                proven["note"].split(" Judgeable")[0][-260:],
            )


if __name__ == "__main__":
    main()
