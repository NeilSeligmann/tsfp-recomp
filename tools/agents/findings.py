# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce the completed-task findings archive from a committed ledger snapshot."""

import argparse
import hashlib
import re
import subprocess
from pathlib import Path

from tools.agents.roadmap import TASK_LINE

ARCHIVE = Path("docs/completed-findings.md")
SOURCE = re.compile(r"^Source commit: `([0-9a-f]{40})`$", re.MULTILINE)


def render(source: str) -> tuple[str, int]:
    ledger = subprocess.check_output(["git", "show", f"{source}:docs/tasks.md"], text=True)
    matches = list(TASK_LINE.finditer(ledger))
    entries: dict[str, str] = {}
    for index, match in enumerate(matches):
        task = match.group(2)
        if match.group(1) != "x" and task not in {"T76", "T77"}:
            continue
        end = matches[index + 1].start() if index + 1 < len(matches) else len(ledger)
        block = ledger[match.start() : end].strip()
        entries[task] = entries.get(task, "") + ("\n\n" if task in entries else "") + block
    completed = {match.group(2) for match in matches if match.group(1) == "x"}
    occurrences = sum(match.group(1) == "x" for match in matches)
    digest = hashlib.sha256(ledger.encode()).hexdigest()
    lines = [
        "# Completed findings: historical ledger backfill",
        "",
        f"Source commit: `{source}`",
        f"Ledger SHA-256: `{digest}`",
        f"Coverage: **{len(completed)} unique completed tasks**, "
        f"all **{occurrences} `[x]` records** at this pin.",
        "Also preserves the ongoing T76/T77 records and completed slices; "
        "neither umbrella task is claimed DONE.",
        "",
        "These are verbatim committed ledger records, ordered by task ID. They preserve",
        "conclusions, negative results, validation claims and links already written by",
        "earlier agents. They are **source-only historical evidence**, not a new rerun",
        "or retroactive verification. Numbers, paths and availability are snapshots.",
        "A missing receipt remains missing; an old DONE label does not prove present",
        "correctness or current runtime reachability. Later tasks may supersede these",
        "records. See [decomp reference](decomp-reference.md) for topical interpretation",
        "and [current ledger](tasks.md) for later work and open tasks.",
        "",
        "Regenerate: `python3 -m tools.agents.findings --write --source " + source + "`.",
        "Check exact pinned coverage and contents: `python3 -m tools.agents.findings --check`.",
        "New completions are documented in the live ledger/topical docs; this archive",
        "is intentionally frozen so simultaneous tasks do not invalidate its coverage claim.",
        "",
        "## Task index",
        "",
    ]
    for task in sorted(entries, key=lambda value: int(value[1:])):
        title = entries[task].splitlines()[0]
        title = title.removeprefix("- [x] **").replace("**", "").replace("|", "\\|")
        lines.append(f"- [{task}](#{task.lower()}): {title.removeprefix(task + '. ').strip()}")
    for task in sorted(entries, key=lambda value: int(value[1:])):
        lines.extend(["", f"## {task}", "", entries[task]])
    return "\n".join(lines) + "\n", len(completed)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--write", action="store_true")
    action.add_argument("--check", action="store_true")
    parser.add_argument("--source", help="committed ledger pin; required when creating archive")
    args = parser.parse_args()
    actual = ARCHIVE.read_text() if ARCHIVE.exists() else ""
    source = args.source
    if source is None:
        match = SOURCE.search(actual)
        if match is None:
            parser.error("provide --source or retain the archive's Source commit header")
        source = match.group(1)
    source = subprocess.check_output(
        ["git", "rev-parse", f"{source}^{{commit}}"], text=True
    ).strip()
    expected, count = render(source)
    if args.write:
        ARCHIVE.write_text(expected)
    elif actual != expected:
        print(
            "FAIL: archive differs from the committed completed-task records; regenerate at its pin"
        )
        return 1
    reference = Path("docs/decomp-reference.md")
    for target in re.findall(r"\]\(([^)]+)\)", reference.read_text()):
        if "://" not in target and not (reference.parent / target.split("#")[0]).exists():
            print(f"FAIL: missing decomp reference link: {target}")
            return 1
    print(f"PASS: {count} completed tasks, exact coverage and contents at {source}")
    print("PASS: decomp reference local file links resolve")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
