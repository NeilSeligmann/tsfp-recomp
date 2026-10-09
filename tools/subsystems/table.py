# SPDX-License-Identifier: GPL-3.0-or-later
"""The committed table `tools/data/function_subsystems.csv`: one row per function.

Columns exactly `address,subsystem,confidence,evidence`. `address` is `0x%08x` like
`tools/data/function_names.csv`, rows are sorted by address, `subsystem` is a path of the
subsystem tree or `unknown`, `confidence` is MEASURED, INFERRED or UNKNOWN. `unknown` and
UNKNOWN go together. Output is byte stable: LF line ends, no timestamps.
"""

from __future__ import annotations

import csv
import io
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from tools.subsystems.rules import CONFIDENCES, UNKNOWN
from tools.subsystems.tree import UNKNOWN_PATH, Tree

TABLE_PATH = Path("tools/data/function_subsystems.csv")
TABLE_COLUMNS = ("address", "subsystem", "confidence", "evidence")


@dataclass(frozen=True)
class Row:
    va: int
    subsystem: str
    confidence: str
    evidence: str


def format_address(va: int) -> str:
    return f"0x{va:08x}"


def render_table(rows: Iterable[Row]) -> str:
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(TABLE_COLUMNS)
    for row in sorted(rows, key=lambda r: r.va):
        writer.writerow([format_address(row.va), row.subsystem, row.confidence, row.evidence])
    return buffer.getvalue()


def parse_table(text: str, *, label: str = "function_subsystems.csv") -> list[Row]:
    reader = csv.reader(io.StringIO(text))
    header = next(reader, None)
    if header is None or tuple(header) != TABLE_COLUMNS:
        raise ValueError(f"{label}: header must be {','.join(TABLE_COLUMNS)}, got {header!r}")
    rows: list[Row] = []
    for number, cells in enumerate(reader, start=2):
        if len(cells) != len(TABLE_COLUMNS):
            raise ValueError(f"{label}:{number}: expected {len(TABLE_COLUMNS)} columns")
        address, subsystem, confidence, evidence = cells
        try:
            va = int(address, 16)
        except ValueError:
            raise ValueError(f"{label}:{number}: address {address!r} is not hex") from None
        rows.append(Row(va, subsystem, confidence, evidence))
    return rows


def read_table(path: Path) -> list[Row]:
    return parse_table(path.read_text(encoding="utf-8"), label=path.name)


def validate_rows(rows: Iterable[Row], tree: Tree) -> list[str]:
    """Every problem of a table: vocabulary, tree membership, ordering, uniqueness."""
    errors: list[str] = []
    previous = -1
    seen: set[int] = set()
    for row in rows:
        where = format_address(row.va)
        if row.va in seen:
            errors.append(f"{where}: duplicate address")
        elif row.va < previous:
            errors.append(f"{where}: not sorted by address")
        seen.add(row.va)
        previous = max(previous, row.va)
        if row.confidence not in CONFIDENCES:
            errors.append(f"{where}: confidence {row.confidence!r} not in {CONFIDENCES}")
        if (row.subsystem == UNKNOWN_PATH) != (row.confidence == UNKNOWN):
            errors.append(f"{where}: subsystem {row.subsystem!r} with confidence {row.confidence}")
        if row.subsystem != UNKNOWN_PATH and row.subsystem not in tree:
            errors.append(f"{where}: subsystem {row.subsystem!r} is not in the tree")
        if not row.evidence:
            errors.append(f"{where}: empty evidence")
    return errors
