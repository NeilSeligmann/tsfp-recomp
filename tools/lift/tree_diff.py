# SPDX-License-Identifier: GPL-3.0-or-later
"""Function-level diff of two lifted `gen` trees, for the re-lift acceptance report (T421, T554).

A re-lift moves functions between chunks whenever one body changes length, so a file diff is
unreadable. This splits every `recomp_*.c` chunk into function bodies keyed by name and reports
which bodies were added, removed or changed, which of the other generated files differ, and each
tree's digest (`tools.harness.provenance.tree_digest`).

Run: ``python -m tools.lift.tree_diff OLD_GEN NEW_GEN [--show-bodies]``
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from tools.harness.provenance import tree_digest

_HEADER = re.compile(r"^void (sub_[0-9A-Fa-f]{8})\(void\)")
_DOC_START = "/**"


def function_bodies(gen_dir: Path) -> dict[str, str]:
    """Function name to its text (doc comment included), over every numbered chunk."""
    bodies: dict[str, list[str]] = {}
    for chunk in sorted(gen_dir.glob("recomp_[0-9]*.c")):
        name = None
        pending: list[str] = []
        for line in chunk.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(_DOC_START):
                name = None
                pending = [line]
                continue
            match = _HEADER.match(line)
            if match:
                name = match.group(1).upper().replace("SUB_", "sub_")
                bodies[name] = pending + [line]
                pending = []
                continue
            if name is not None:
                bodies[name].append(line)
            else:
                pending.append(line)
    return {name: "\n".join(lines).rstrip() for name, lines in bodies.items()}


@dataclass
class TreeDiff:
    old_digest: str
    new_digest: str
    old_count: int
    new_count: int
    added: list[str] = field(default_factory=list)
    removed: list[str] = field(default_factory=list)
    changed: list[str] = field(default_factory=list)
    other_files: list[str] = field(default_factory=list)

    @property
    def identical_bodies(self) -> bool:
        return not (self.added or self.removed or self.changed)


def diff_trees(old: Path, new: Path) -> TreeDiff:
    before, after = function_bodies(old), function_bodies(new)
    result = TreeDiff(
        old_digest=tree_digest(old),
        new_digest=tree_digest(new),
        old_count=len(before),
        new_count=len(after),
        added=sorted(set(after) - set(before)),
        removed=sorted(set(before) - set(after)),
        changed=sorted(name for name in set(before) & set(after) if before[name] != after[name]),
    )
    names = {p.name for p in old.iterdir() if p.is_file()} | {
        p.name for p in new.iterdir() if p.is_file()
    }
    for name in sorted(n for n in names if not re.match(r"recomp_[0-9]", n)):
        a, b = old / name, new / name
        if not (a.is_file() and b.is_file()) or a.read_bytes() != b.read_bytes():
            result.other_files.append(name)
    return result


def render(result: TreeDiff, old: Path, new: Path, show_bodies: bool) -> list[str]:
    lines = [
        f"old tree digest {result.old_digest}",
        f"new tree digest {result.new_digest}",
        f"function bodies {result.old_count} old, {result.new_count} new",
        f"added {len(result.added)}, removed {len(result.removed)}, changed {len(result.changed)}",
    ]
    lines += [f"  added   {name}" for name in result.added]
    lines += [f"  removed {name}" for name in result.removed]
    lines += [f"  changed {name}" for name in result.changed]
    lines.append("other generated files that differ: " + (", ".join(result.other_files) or "none"))
    if show_bodies and result.changed:
        before, after = function_bodies(old), function_bodies(new)
        for name in result.changed:
            lines.append(f"--- {name}")
            lines += difflib.unified_diff(
                before[name].splitlines(), after[name].splitlines(), "old", "new", lineterm="", n=1
            )
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.lift.tree_diff", description=__doc__.split("\n\n")[0]
    )
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    parser.add_argument(
        "--show-bodies", action="store_true", help="print the unified diff of each changed body"
    )
    args = parser.parse_args(argv)
    result = diff_trees(args.old, args.new)
    print("\n".join(render(result, args.old, args.new, args.show_bodies)))
    return 0 if result.identical_bodies else 1


if __name__ == "__main__":
    sys.exit(main())
