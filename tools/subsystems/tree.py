# SPDX-License-Identifier: GPL-3.0-or-later
"""The subsystem tree (`tools/data/subsystem_tree.csv`): paths, kinds and rollups.

The tree file is frozen input: `path,title,kind,task`. `path` is a `/` separated path whose
parent path must exist and be an `umbrella`. A `leaf` never has children. `task` is optional.
Rows are kept in file order, which is a depth first listing and is the print order.
"""

from __future__ import annotations

import csv
import io
from collections import defaultdict
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path

DEFAULT_TREE = Path("tools/data/subsystem_tree.csv")
TREE_COLUMNS = ("path", "title", "kind", "task")
KIND_UMBRELLA = "umbrella"
KIND_LEAF = "leaf"
KINDS = (KIND_UMBRELLA, KIND_LEAF)
#: The subsystem of every function no rule or anchor justifies. It is not a tree node.
UNKNOWN_PATH = "unknown"


class TreeError(ValueError):
    """The tree file is malformed."""


@dataclass(frozen=True)
class Node:
    path: str
    title: str
    kind: str
    task: str

    @property
    def parent(self) -> str | None:
        head, sep, _ = self.path.rpartition("/")
        return head if sep else None

    @property
    def depth(self) -> int:
        return self.path.count("/")


class Tree:
    """Validated nodes in file order, with parent, ancestor and descendant lookups."""

    def __init__(self, nodes: Iterable[Node]) -> None:
        self.nodes: tuple[Node, ...] = tuple(nodes)
        self.by_path: dict[str, Node] = {node.path: node for node in self.nodes}
        errors = validate_nodes(self.nodes)
        if errors:
            raise TreeError("; ".join(errors))

    def __contains__(self, path: object) -> bool:
        return path in self.by_path

    def ancestors(self, path: str) -> tuple[str, ...]:
        """Strict ancestors of `path`, nearest first."""
        parts = path.split("/")
        return tuple("/".join(parts[:cut]) for cut in range(len(parts) - 1, 0, -1))

    def is_within(self, path: str, ancestor: str) -> bool:
        """True when `path` equals `ancestor` or lies below it."""
        return path == ancestor or path.startswith(ancestor + "/")

    def rollup(self, own: Mapping[str, Mapping[str, int]]) -> dict[str, dict[str, int]]:
        """Sum `own` counters (path -> field -> count) into every ancestor of each path.

        The result has one entry per tree node, each the node's own counters plus the sum of
        every descendant's. Paths not in the tree are ignored by design: the caller handles
        the `unknown` line separately so it never lands inside an umbrella.
        """
        fields = sorted({field for counters in own.values() for field in counters})
        result = {node.path: dict.fromkeys(fields, 0) for node in self.nodes}
        for path, counters in own.items():
            if path not in self.by_path:
                continue
            for target in (path, *self.ancestors(path)):
                for field, value in counters.items():
                    result[target][field] += value
        return result


def validate_nodes(nodes: Iterable[Node]) -> list[str]:
    """Return every structural problem of the node list (empty when valid)."""
    listed = tuple(nodes)
    errors: list[str] = []
    seen: dict[str, Node] = {}
    for node in listed:
        if not node.path or node.path.startswith("/") or node.path.endswith("/"):
            errors.append(f"bad path {node.path!r}")
        if node.path in seen:
            errors.append(f"duplicate path {node.path}")
        seen[node.path] = node
        if node.kind not in KINDS:
            errors.append(f"{node.path}: kind {node.kind!r} is not one of {', '.join(KINDS)}")
        if not node.title:
            errors.append(f"{node.path}: empty title")
        if node.path == UNKNOWN_PATH:
            errors.append(f"{node.path}: reserved for unclassified functions")
    children: dict[str, list[str]] = defaultdict(list)
    for node in listed:
        parent = node.parent
        if parent is None:
            continue
        parent_node = seen.get(parent)
        if parent_node is None:
            errors.append(f"{node.path}: parent {parent} does not exist")
        elif parent_node.kind != KIND_UMBRELLA:
            errors.append(f"{node.path}: parent {parent} is not an umbrella")
        children[parent].append(node.path)
    for node in listed:
        if node.kind == KIND_LEAF and node.path in children:
            errors.append(f"{node.path}: a leaf has children {children[node.path]}")
    return errors


def parse_tree(text: str, *, label: str = "subsystem_tree.csv") -> Tree:
    reader = csv.reader(io.StringIO(text))
    header = next(reader, None)
    if header is None or tuple(header) != TREE_COLUMNS:
        raise TreeError(f"{label}: header must be {','.join(TREE_COLUMNS)}, got {header!r}")
    nodes: list[Node] = []
    for number, row in enumerate(reader, start=2):
        if not row:
            continue
        if len(row) != len(TREE_COLUMNS):
            raise TreeError(f"{label}:{number}: expected {len(TREE_COLUMNS)} columns")
        path, title, kind, task = (cell.strip() for cell in row)
        nodes.append(Node(path, title, kind, task))
    return Tree(nodes)


def load_tree(path: Path = DEFAULT_TREE) -> Tree:
    return parse_tree(path.read_text(encoding="utf-8"), label=path.name)
