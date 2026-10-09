# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-subsystem counts, rollups to ancestors and the renderers (text, csv, json, markdown).

The model is `Report`: one `Entry` per tree node in file order (own counters and the rollup of
the node and everything below it), an `unknown` counter for functions no rule placed, the
denominators and the XBE hash. It is built either from the live inputs (`build_report`) or from
the committed JSON (`Report.from_json`, the offline fallback), and every renderer works on it.

Counters: `functions` (rows), `named` (non-placeholder name per `tools.coverage`), `registered`
(a native replacement is registered for it in `src/game`), `proven` (replaced and proven according
to the tracked proof snapshot replay, game region only), `measured` and `inferred` (confidence
split). UNKNOWN rows only appear in the `unknown` line. A counter whose source is unavailable (a
stale proof snapshot, no `src/game`) is listed in `Report.unavailable` with the reason and is
printed `n/a`, never as a guessed number. There is deliberately no timestamp or commit in the
output, so it only changes with the data.
"""

from __future__ import annotations

import csv
import io
import json
from collections import Counter
from collections.abc import Collection, Iterable, Mapping
from dataclasses import dataclass, field

from tools.subsystems.rules import INFERRED, MEASURED
from tools.subsystems.table import Row
from tools.subsystems.tree import UNKNOWN_PATH, Tree

SCHEMA = 1
FIELDS = ("functions", "named", "registered", "proven", "measured", "inferred")
COMMAND = (
    ".venv/bin/python -m tools.subsystems report --write "
    "--xbe tmp/oxm-extract/retail/default.xbe --generated-dir generated/retail"
)


@dataclass(frozen=True)
class Counts:
    functions: int = 0
    named: int = 0
    registered: int = 0
    proven: int = 0
    measured: int = 0
    inferred: int = 0

    def __add__(self, other: Counts) -> Counts:
        return Counts(**{f: getattr(self, f) + getattr(other, f) for f in FIELDS})

    def as_dict(self) -> dict[str, int]:
        return {f: getattr(self, f) for f in FIELDS}

    @classmethod
    def from_dict(cls, data: Mapping[str, int]) -> Counts:
        return cls(**{f: int(data[f]) for f in FIELDS})


@dataclass(frozen=True)
class Entry:
    path: str
    title: str
    kind: str
    task: str
    depth: int
    own: Counts
    rollup: Counts


@dataclass(frozen=True)
class Report:
    entries: tuple[Entry, ...]
    unknown: Counts
    functions: int
    game: int
    library: int
    xbe_sha256: str = ""
    #: counter name -> why it is not available (printed `n/a`).
    unavailable: Mapping[str, str] = field(default_factory=dict)
    #: Only set for the text header; not part of the machine twin.
    notes: tuple[str, ...] = field(default=(), compare=False)

    @property
    def total(self) -> Counts:
        top = [e.rollup for e in self.entries if e.depth == 0]
        return sum(top, self.unknown)

    @property
    def classified(self) -> int:
        return self.total.functions - self.unknown.functions

    def reconciles(self) -> bool:
        return self.total.functions == self.functions == self.game + self.library

    def to_json_dict(self) -> dict[str, object]:
        return {
            "schema": SCHEMA,
            "generator": "python -m tools.subsystems report --write",
            "xbe_sha256": self.xbe_sha256,
            "denominator": {
                "functions": self.functions,
                "game": self.game,
                "library": self.library,
            },
            "unavailable": dict(sorted(self.unavailable.items())),
            "totals": {**self.total.as_dict(), "unknown": self.unknown.functions},
            "unknown": self.unknown.as_dict(),
            "subsystems": [
                {
                    "path": e.path,
                    "title": e.title,
                    "kind": e.kind,
                    "task": e.task,
                    "depth": e.depth,
                    "own": e.own.as_dict(),
                    "rollup": e.rollup.as_dict(),
                }
                for e in self.entries
            ],
        }

    @classmethod
    def from_json(cls, data: Mapping[str, object]) -> Report:
        if data.get("schema") != SCHEMA:
            raise ValueError(f"subsystem coverage json: schema {data.get('schema')!r} != {SCHEMA}")
        denominator = data["denominator"]
        subsystems = data["subsystems"]
        unknown = data["unknown"]
        unavailable = data.get("unavailable", {})
        assert isinstance(denominator, dict) and isinstance(subsystems, list)
        assert isinstance(unknown, dict) and isinstance(unavailable, dict)
        entries = tuple(
            Entry(
                s["path"],
                s["title"],
                s["kind"],
                s["task"],
                int(s["depth"]),
                Counts.from_dict(s["own"]),
                Counts.from_dict(s["rollup"]),
            )
            for s in subsystems
        )
        return cls(
            entries,
            Counts.from_dict(unknown),
            int(denominator["functions"]),
            int(denominator["game"]),
            int(denominator["library"]),
            str(data.get("xbe_sha256", "")),
            {str(k): str(v) for k, v in unavailable.items()},
        )

    def with_notes(self, *notes: str) -> Report:
        return Report(
            self.entries,
            self.unknown,
            self.functions,
            self.game,
            self.library,
            self.xbe_sha256,
            self.unavailable,
            notes,
        )


def tally(
    rows: Iterable[Row],
    named: Collection[int],
    proven: Collection[int],
    registered: Collection[int] = (),
) -> tuple[dict[str, Counts], Counts]:
    """(own counters by tree path, the `unknown` counter) from table rows."""
    own: dict[str, Counter[str]] = {}
    unknown: Counter[str] = Counter()
    for row in rows:
        if row.subsystem == UNKNOWN_PATH:
            bucket = unknown
        else:
            bucket = own.setdefault(row.subsystem, Counter())
        bucket["functions"] += 1
        bucket["named"] += row.va in named
        bucket["registered"] += row.va in registered
        bucket["proven"] += row.va in proven
        bucket["measured"] += row.confidence == MEASURED
        bucket["inferred"] += row.confidence == INFERRED
    return (
        {path: Counts(**{f: c[f] for f in FIELDS}) for path, c in own.items()},
        Counts(**{f: unknown[f] for f in FIELDS}),
    )


def build_report(
    tree: Tree,
    rows: Iterable[Row],
    named: Collection[int],
    proven: Collection[int],
    *,
    game: int,
    library: int,
    registered: Collection[int] = (),
    unavailable: Mapping[str, str] | None = None,
    xbe_sha256: str = "",
) -> Report:
    listed = list(rows)
    own, unknown = tally(listed, named, proven, registered)
    stray = sorted(set(own) - set(tree.by_path))
    if stray:
        raise ValueError(f"rows placed outside the tree: {stray}")
    rolled = tree.rollup({path: counts.as_dict() for path, counts in own.items()})
    entries = tuple(
        Entry(
            node.path,
            node.title,
            node.kind,
            node.task,
            node.depth,
            own.get(node.path, Counts()),
            Counts.from_dict(rolled[node.path]),
        )
        for node in tree.nodes
    )
    return Report(entries, unknown, len(listed), game, library, xbe_sha256, dict(unavailable or {}))


# --------------------------------------------------------------------------- #
# Renderers.
# --------------------------------------------------------------------------- #


def _pct(part: int, whole: int) -> str:
    return f"{100 * part / whole:.1f}%" if whole else "n/a"


def _cell(report: Report, counts: Counts, name: str) -> str:
    return "n/a" if name in report.unavailable else f"{getattr(counts, name):,}"


def render_text(report: Report, *, tree: bool = False) -> str:
    """Text table. Flat: nodes with own rows. `tree`: indented, umbrellas show their rollup."""

    def tree_label(entry: Entry) -> str:
        label = "  " * entry.depth + entry.path.rsplit("/", 1)[-1]
        label += " +" if entry.kind == "umbrella" else ""
        return label + (f" [{entry.task}]" if entry.task else "")

    def flat_label(entry: Entry) -> str:
        return entry.path + (f" [{entry.task}]" if entry.task else "")

    label_of = tree_label if tree else flat_label
    label_width = max(
        [
            len("subsystem"),
            len("TOTAL"),
            len("unknown"),
            *(len(label_of(e)) for e in report.entries),
        ]
    )
    header = f"{'subsystem':<{label_width}}  " + "  ".join(f"{c:>10}" for c in FIELDS)
    lines = [header, "-" * len(header)]

    def line(label: str, counts: Counts) -> str:
        cells = "  ".join(f"{_cell(report, counts, c):>10}" for c in FIELDS)
        return f"{label:<{label_width}}  {cells}"

    for entry in report.entries:
        if tree:
            lines.append(line(label_of(entry), entry.rollup))
        elif entry.own.functions:
            lines.append(line(label_of(entry), entry.own))
    lines += [line("unknown", report.unknown), "-" * len(header), line("TOTAL", report.total)]
    total = report.total
    lines += [
        "",
        f"denominator {report.functions:,} functions = {report.game:,} game + "
        f"{report.library:,} library; classified {report.classified:,} "
        f"({_pct(report.classified, total.functions)}), unclassified (UNKNOWN) "
        f"{report.unknown.functions:,}; reconciles: {'yes' if report.reconciles() else 'NO'}",
        f"confidence: MEASURED {total.measured:,}, INFERRED {total.inferred:,}, "
        f"UNKNOWN {report.unknown.functions:,}",
    ]
    for name, reason in sorted(report.unavailable.items()):
        lines.append(f"n/a: {name} is unavailable: {reason}")
    if tree:
        lines.append("'+' marks an umbrella: its line is the rollup of everything below it.")
    lines.extend(report.notes)
    return "\n".join(lines) + "\n"


CSV_COLUMNS = (
    "path", "title", "kind", "task",
    "own_functions", "own_named", "own_registered", "own_proven", "own_measured", "own_inferred",
    "functions", "named", "registered", "proven", "measured", "inferred",
)  # fmt: skip


def render_csv(report: Report) -> str:
    """Machine readable rows. An unavailable counter is written empty."""
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(CSV_COLUMNS)

    def values(counts: Counts) -> list[str | int]:
        return ["" if f in report.unavailable else getattr(counts, f) for f in FIELDS]

    for e in report.entries:
        writer.writerow([e.path, e.title, e.kind, e.task, *values(e.own), *values(e.rollup)])
    unknown = values(report.unknown)
    writer.writerow(
        [
            UNKNOWN_PATH,
            "No rule or anchor justifies a subsystem",
            "unclassified",
            "",
            *unknown,
            *unknown,
        ]  # fmt: skip
    )
    total = values(report.total)
    writer.writerow(["TOTAL", "All functions of the shared table", "total", "", *total, *total])
    return buffer.getvalue()


def render_json(report: Report) -> str:
    return json.dumps(report.to_json_dict(), indent=2, sort_keys=True) + "\n"


def render_markdown(report: Report) -> str:
    total = report.total
    out = [
        "# Subsystem coverage",
        "",
        "GENERATED by `tools/subsystems` (decomp tree). Do not edit by hand. Regenerate:",
        "",
        "```sh",
        COMMAND,
        "```",
        "",
        "Machine-readable twins: `docs/data/subsystem-coverage.csv` and "
        "`docs/data/subsystem-coverage.json`. `python -m tools.subsystems check` fails when "
        "any of them or this file is stale (and the placement table "
        "`tools/data/function_subsystems.csv` when it is present locally).",
        "",
        "Inputs: the shared function table (the denominator of the badges, game plus library), "
        "`tools/data/function_names.csv`, `tools/data/library_names.csv`, `.XTLID`/FLIRT and the "
        "section evidence of `tools/coverage.py`, the subsystem tree "
        "`tools/data/subsystem_tree.csv`, the placement table `tools/data/function_subsystems.csv` "
        "(gitignored because the repository hook rejects a tracked per-address dump, regenerated "
        "deterministically by `python -m tools.subsystems seed`), the registrations of `src/game` "
        "and the tracked proof snapshot `docs/data/replace-proof-snapshot.json` (replayed, never a "
        "live harness run).",
        "",
        "## Reconciliation",
        "",
        f"{report.functions:,} functions = {report.game:,} game + {report.library:,} library. "
        f"Placed in a subsystem: {report.classified:,} "
        f"({_pct(report.classified, total.functions)}), unknown: {report.unknown.functions:,}. "
        f"Confidence: MEASURED {total.measured:,}, INFERRED {total.inferred:,}. "
        f"Reconciles with the table: {'yes' if report.reconciles() else 'NO'}.",
        "",
    ]
    for name, reason in sorted(report.unavailable.items()):
        out += [f"`{name}` is n/a in this snapshot: {reason}.", ""]
    out += [
        "## Subsystem tree (umbrellas are rollups of everything below them)",
        "",
        "| subsystem | title | functions | named | registered | proven | MEASURED | INFERRED |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]

    def row(label: str, title: str, c: Counts) -> str:
        cells = " | ".join(
            _cell(report, c, f) for f in ("functions", "named", "registered", "proven")
        )
        return f"| {label} | {title} | {cells} | {c.measured:,} | {c.inferred:,} |"

    for e in report.entries:
        name = "&nbsp;&nbsp;" * e.depth + f"`{e.path.rsplit('/', 1)[-1]}`"
        if e.kind == "umbrella":
            name = f"**{name}**"
        if e.task:
            name += f" ({e.task})"
        out.append(row(name, e.title, e.rollup))
    out += [
        row("`unknown`", "No rule or anchor justifies a subsystem", report.unknown),
        row("**TOTAL**", "", total),
        "",
        "## Reading the numbers",
        "",
        "- `named`: non-placeholder name after the game and library overlays "
        "(`tools.coverage.is_placeholder_name`).",
        "- `registered`: a native replacement is registered for the function in `src/game`.",
        "- `proven`: replaced and proven in the tracked proof snapshot. It is an upper bound, "
        "as in `docs/coverage-by-subsystem.md`, and it is `n/a` whenever the snapshot is stale "
        "(the same rule as the badges); no library function counts.",
        "- `MEASURED` is rare by design: an address anchor from a document that measured it on "
        "the running game, a `function_names.csv` row whose own confidence is MEASURED, a "
        "library function matched by a signature (.XTLID or FLIRT) or a recorded classification "
        "row. Everything else placed is `INFERRED`. A rule never produces MEASURED.",
        "- A subsystem path that is an umbrella can own rows too (for example "
        "`gameplay/modes` when no child is decided); the rollup adds them to the children.",
        "",
        "## Limitations",
        "",
        "- Placement follows name prefixes, a few address lists and the call graph, all "
        "INFERRED, and the names themselves are INFERRED. See `tools/subsystems/rules.py` for "
        "the exact rules and which ones are boundary decisions of the decomp tree.",
        "- Without the XBE, `python -m tools.subsystems report` prints this committed snapshot "
        "(`docs/data/subsystem-coverage.json`) instead of recomputing it.",
        "",
    ]
    return "\n".join(out)
