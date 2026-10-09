# SPDX-License-Identifier: GPL-3.0-or-later
"""Report every switch jump table recoverable from an XBE, and the residue that is not.

Decodes each exported function's own byte range, resolves `jmp dword ptr [idx*4 + imm]`
against the `cmp`/`ja` range check in front of it, and prints the recovered tables, the
unresolved residue broken down by cause, and a false-positive null. See
`tools/jumptables/recover.py` for why the decode is anchored at function entries rather
than swept linearly, and why the tables are in `.text` on this binary.

Usage:

    ./.venv/bin/python -m tools.jumptables.cli \\
        tmp/oxm-extract/retail/default.xbe \\
        --functions generated/retail-deep/functions.csv \\
        --tables-csv generated/jumptables/tables.csv \\
        --unresolved-csv generated/jumptables/unresolved.csv

THE NULL IS PART OF THE REPORT. "We recovered 560 tables" is not interpretable without
knowing how often the same entry-scan rule fires on an address that is not a table at
all, so the null is recomputed exhaustively over every 4-aligned address of each
`--null-sections` region on every run and printed alongside. It is never quoted from a
previous run: a null carried around as a constant is a null nobody will notice going
stale. Recovered tables are excluded from it, because a real table firing the rule is
the signal rather than the noise, and the null is printed on TWO exclusion scopes:
skipping just the table bases answers "how often does the rule fire at an arbitrary
aligned address", while skipping the whole span of every recovered table answers "how
often does the rule fire on data belonging to no table we recovered". The first is
dominated by the interiors of our own tables, so neither number alone is honest.

`--marker-functions` takes the set of functions some other analysis failed on, and
reports how many of them this module resolves. That intersection is the actual claim
about progress, and it is deliberately the caller's set rather than one invented here.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from tools.codediff.boundaries import Function, load_functions
from tools.jumptables.recover import (
    DEFAULT_BOUND_WINDOW,
    DEFAULT_MIN_ENTRIES,
    DEFAULT_SCAN_LIMIT,
    FORM_DIRECT,
    FORM_INDEX_INDIRECTION,
    LENGTH_FROM_BOUND,
    Image,
    JumpTable,
    NullResult,
    Recovery,
    false_positive_null,
    image_from_xbe,
    recover_image,
    scan_targets,
)

DEFAULT_TOP = 20
DEFAULT_NULL_SECTIONS = (".text", ".rdata", ".data")

#: The two exclusion scopes the null is reported on. See `_describe_nulls`.
NULL_SCOPE_BASES = "bases only"
NULL_SCOPE_SPANS = "whole spans"

TABLE_CSV_HEADER = (
    "jump_va",
    "function_va",
    "form",
    "table_va",
    "entry_count",
    "bound",
    "length_source",
    "index_table_va",
    "suspect",
    "in_gap",
    "targets",
)
UNRESOLVED_CSV_HEADER = ("jump_va", "function_va", "cause", "in_gap", "detail")

MARKER_CSV_COLUMN = "func_va"

#: Longest histogram bar printed, in characters. Purely cosmetic.
BAR_WIDTH = 40


def load_marker_functions(path: Path) -> list[int]:
    """Read a single-column `func_va` CSV of function entry VAs, in file order.

    A row that does not parse raises `ValueError` naming the file and line rather than
    being skipped, for the same reason `load_functions` does: a marker set silently
    missing rows would understate the intersection and make this module look worse than
    it is, which is the wrong direction for a measurement to fail in.
    """
    vas: list[int] = []
    with path.open(encoding="utf-8", newline="") as stream:
        reader = csv.reader(stream)
        header = next(reader, None)
        if header is None:
            raise ValueError(f"{path}: file is empty, expected a {MARKER_CSV_COLUMN} header")
        if [column.strip() for column in header] != [MARKER_CSV_COLUMN]:
            raise ValueError(f"{path}:1: unexpected header {header!r}, expected ['func_va']")
        for line, row in enumerate(reader, start=2):
            if not row:
                continue
            if len(row) != 1:
                raise ValueError(f"{path}:{line}: expected 1 field, got {len(row)}: {row!r}")
            try:
                vas.append(int(row[0].strip(), 16))
            except ValueError:
                raise ValueError(
                    f"{path}:{line}: {MARKER_CSV_COLUMN} is not hex: {row[0]!r}"
                ) from None
    return vas


def write_tables_csv(path: Path, tables: list[JumpTable]) -> int:
    """Write `tables` to `path`, ascending by `(jump_va, table_va)`. Returns the count.

    Addresses are 0x-prefixed hex zero-padded to 8 digits, matching
    `tools/ghidra/ExportFunctionBounds.java`, so this table joins the function table on
    address text alone. `targets` is a space-separated list in table order, which
    preserves the index-to-target mapping a consumer needs. `suspect` is `|`-joined so
    the field survives a CSV reader that does not split on anything else.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    ordered = sorted(tables, key=lambda table: (table.jump_va, table.table_va))
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(TABLE_CSV_HEADER)
        for table in ordered:
            writer.writerow(
                (
                    f"{table.jump_va:#010x}",
                    "" if table.function_va is None else f"{table.function_va:#010x}",
                    table.form,
                    f"{table.table_va:#010x}",
                    table.entry_count,
                    "" if table.bound is None else table.bound,
                    table.length_source,
                    "" if table.index_table_va is None else f"{table.index_table_va:#010x}",
                    "|".join(table.suspect),
                    "true" if table.in_gap else "false",
                    " ".join(f"{target:#010x}" for target in table.targets),
                )
            )
    return len(ordered)


def write_unresolved_csv(path: Path, recovery: Recovery) -> int:
    """Write the unresolved residue to `path`, ascending by `jump_va`. Returns the count."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(UNRESOLVED_CSV_HEADER)
        for site in recovery.unresolved:
            writer.writerow(
                (
                    f"{site.jump_va:#010x}",
                    "" if site.function_va is None else f"{site.function_va:#010x}",
                    site.cause,
                    "true" if site.in_gap else "false",
                    site.detail,
                )
            )
    return len(recovery.unresolved)


def section_list(value: str) -> tuple[str, ...]:
    """Parse a comma-separated section-name list, dropping empty entries."""
    return tuple(name.strip() for name in value.split(",") if name.strip())


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Recover MSVC switch jump tables from an XBE, anchored at exported function "
            "entries, and report the indirect jumps that remain unresolved."
        )
    )
    parser.add_argument("xbe", type=Path, help="XBE to analyse")
    parser.add_argument(
        "--functions",
        type=Path,
        required=True,
        metavar="CSV",
        help="ExportFunctionBounds.java CSV for this XBE; decoding is anchored at its entries",
    )
    parser.add_argument(
        "--tables-csv",
        type=Path,
        default=None,
        metavar="CSV",
        help=f"also write recovered tables here ({','.join(TABLE_CSV_HEADER)})",
    )
    parser.add_argument(
        "--unresolved-csv",
        type=Path,
        default=None,
        metavar="CSV",
        help=f"also write the unresolved residue here ({','.join(UNRESOLVED_CSV_HEADER)})",
    )
    parser.add_argument(
        "--no-adopt-gaps",
        dest="adopt_gaps",
        action="store_false",
        help=(
            "stop each function's decode at its own body_max_va instead of continuing to "
            "the next function entry. Ghidra truncates a body AT the switch it failed to "
            "recover, so this hides every later dispatch in that function"
        ),
    )
    parser.add_argument(
        "--null-sections",
        type=section_list,
        default=DEFAULT_NULL_SECTIONS,
        metavar="NAMES",
        help=(
            "comma-separated sections to compute the false-positive null over "
            f"(default {','.join(DEFAULT_NULL_SECTIONS)})"
        ),
    )
    parser.add_argument(
        "--marker-functions",
        type=Path,
        default=None,
        metavar="CSV",
        help=(
            "single-column `func_va` CSV of function entries another analysis failed on; "
            "reports how many of them this module resolves"
        ),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=DEFAULT_TOP,
        metavar="N",
        help=f"tables to tabulate, by entry count (default {DEFAULT_TOP})",
    )
    parser.add_argument(
        "--bound-window",
        type=int,
        default=DEFAULT_BOUND_WINDOW,
        metavar="N",
        help=f"instructions to look back for the cmp/ja pair (default {DEFAULT_BOUND_WINDOW})",
    )
    parser.add_argument(
        "--min-entries",
        type=int,
        default=DEFAULT_MIN_ENTRIES,
        metavar="N",
        help=f"shortest table an unbounded scan may accept (default {DEFAULT_MIN_ENTRIES})",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    data = args.xbe.read_bytes()
    image = image_from_xbe(data)
    functions = load_functions(args.functions)
    print(_describe_image(args.xbe, image, functions))

    recovery = recover_image(
        image,
        functions,
        adopt_gaps=args.adopt_gaps,
        bound_window=args.bound_window,
        min_entries=args.min_entries,
    )
    print(_describe_tables(recovery))
    print(_describe_histogram(recovery))
    print(_describe_causes(recovery))
    print(_describe_nulls(image, recovery, args.null_sections, args.min_entries))
    if args.marker_functions is not None:
        print(_describe_markers(recovery, load_marker_functions(args.marker_functions)))
    print(_tabulate(recovery.tables, args.top))

    if args.tables_csv is not None:
        written = write_tables_csv(args.tables_csv, recovery.tables)
        print(f"\nwrote {written:,} tables to {args.tables_csv}")
    if args.unresolved_csv is not None:
        written = write_unresolved_csv(args.unresolved_csv, recovery)
        print(f"wrote {written:,} unresolved sites to {args.unresolved_csv}")
    return 0


def _describe_image(path: Path, image: Image, functions: list[Function]) -> str:
    lines = [
        f"{path.name}: {len(image.regions)} sections, {len(functions):,} exported functions",
        f"  {'section':<9}{'base_va':<12}{'virtual':>10}{'raw':>10}  code",
    ]
    for region in image.regions:
        lines.append(
            f"  {region.name:<9}{region.base_va:#010x}  {region.virtual_size:>10,}"
            f"{len(region.data):>10,}  {'yes' if region.executable else 'no'}"
        )
    inside = sum(1 for function in functions if image.region_at(function.entry_va) is not None)
    lines.append(f"  {inside:,} of {len(functions):,} function entries land inside a section")
    return "\n".join(lines)


def _describe_tables(recovery: Recovery) -> str:
    tables = recovery.tables
    direct = sum(1 for table in tables if table.form == FORM_DIRECT)
    indirection = sum(1 for table in tables if table.form == FORM_INDEX_INDIRECTION)
    bounded = sum(1 for table in tables if table.length_source == LENGTH_FROM_BOUND)
    entries = sum(table.entry_count for table in tables)
    suspect = recovery.suspect_tables
    share = len(suspect) / len(tables) if tables else 0.0
    hosts = len(recovery.functions_with_tables)
    lines = [
        f"\ntables     {len(tables):,} recovered in {hosts:,} functions",
        f"  {FORM_DIRECT:<18}{direct:>7,}",
        f"  {FORM_INDEX_INDIRECTION:<18}{indirection:>7,}",
        f"  length from bound {bounded:>7,}  ({_share(bounded, len(tables)):.1%} of tables)",
        f"  entries total     {entries:>7,}",
        f"  distinct targets  {len(recovery.distinct_targets):>7,}",
        f"suspect    {len(suspect):,} tables carry at least one flag ({share:.1%})",
    ]
    in_gap = [table for table in tables if table.in_gap]
    gap_entries = sum(table.entry_count for table in in_gap)
    lines.insert(
        6,
        f"  in a Ghidra gap   {len(in_gap):>7,}  ({gap_entries:,} entries; the jump sits past "
        "its function's body_max_va)",
    )
    flags: dict[str, int] = {}
    for table in suspect:
        for flag in table.suspect:
            flags[flag] = flags.get(flag, 0) + 1
    for flag, count in sorted(flags.items(), key=lambda item: (-item[1], item[0])):
        lines.append(f"  {flag:<26}{count:>7,}")
    lines.append(
        "  flagged, never truncated: a dropped entry is a miscompile, a flag is a question"
    )
    return "\n".join(lines)


def _describe_histogram(recovery: Recovery) -> str:
    histogram = recovery.entry_count_histogram()
    if not histogram:
        return "\nno tables, so no entry-count histogram"
    peak = max(histogram.values())
    lines = ["\nentry-count histogram", f"  {'entries':>7}{'tables':>8}"]
    for entries, count in histogram.items():
        bar = "#" * max(1, round(BAR_WIDTH * count / peak))
        lines.append(f"  {entries:>7}{count:>8,}  {bar}")
    return "\n".join(lines)


def _describe_causes(recovery: Recovery) -> str:
    counts = recovery.cause_counts()
    total = len(recovery.unresolved)
    lines = [f"\nunresolved residue  {total:,} indirect jumps this module cannot resolve"]
    for cause, count in counts.items():
        lines.append(f"  {cause:<26}{count:>7,}  {_share(count, total):>6.1%}")
    if not counts:
        lines.append("  none")
    return "\n".join(lines)


def _describe_nulls(
    image: Image,
    recovery: Recovery,
    sections: tuple[str, ...],
    min_entries: int,
) -> str:
    """The exhaustive false-positive null per section, on both exclusion scopes.

    TWO ROWS PER SECTION, BECAUSE THEY ANSWER TWO QUESTIONS AND NEITHER ALONE IS HONEST.
    `bases only` skips the recovered table bases and nothing else, so the INTERIOR
    4-aligned offsets of those same tables stay in the denominator and fire the rule for
    the same reason their base does. That number answers "how often does the rule fire at
    an arbitrary aligned address" and is mostly a measurement of our own tables.
    `whole spans` skips every byte of every recovered target table and byte index table,
    which answers "how often does the rule fire on data belonging to no table we
    recovered" -- the actual false-positive rate. Both are recomputed every run.

    The observed rate is the share of the table bases actually recovered in that section
    that fire the same unbounded scan rule the null applies to every aligned address. The
    two numbers are therefore the same measurement on two populations, which is the only
    way an enrichment factor means anything, so it is printed against both scopes.
    """
    bases = frozenset(table.table_va for table in recovery.tables)
    spans = _recovered_spans(recovery)
    lines = [
        "\nfalse-positive null (exhaustive over every 4-aligned address, recomputed every run)",
        f"  {'section':<9}{'excluded':<13}{'tested':>11}{'fired':>8}{'rate':>10}"
        f"{'skipped':>9}{'bases':>7}{'fire':>6}  enrichment",
    ]
    for name in sections:
        observed, counted = _observed_rate(image, name, bases, min_entries)
        scopes = (
            (NULL_SCOPE_BASES, _null_for(image, name, min_entries, bases, ())),
            (NULL_SCOPE_SPANS, _null_for(image, name, min_entries, bases, spans)),
        )
        for scope, result in scopes:
            factor = observed / result.rate if result.rate > 0.0 else 0.0
            enrichment = f"{factor:,.0f}x" if factor else "n/a (null is zero)"
            lines.append(
                f"  {result.region_name:<9}{scope:<13}{result.offsets_tested:>11,}"
                f"{result.offsets_fired:>8,}{result.rate:>10.5%}{result.excluded:>9,}"
                f"{counted:>7,}{observed:>6.0%}  {enrichment}"
            )
    lines.append(f"  {NULL_SCOPE_BASES:<13}only the recovered table bases are skipped, so the")
    lines.append(f"  {'':<13}fires still include our own tables' interiors")
    lines.append(f"  {NULL_SCOPE_SPANS:<13}every byte of every recovered target table and byte")
    lines.append(f"  {'':<13}index table is skipped: the real false-positive rate")
    lines.append("  bases = recovered table bases in that section; fire = how many of them fire")
    return "\n".join(lines)


def _recovered_spans(recovery: Recovery) -> tuple[tuple[int, int], ...]:
    """Half-open VA spans of every recovered target table and byte index table.

    Both kinds count as "a table we recovered": an aligned offset inside a byte index
    table satisfies the scan rule just as readily as one inside a target table, and
    calling such a fire a false positive would be counting our own output against us.
    """
    spans: list[tuple[int, int]] = []
    for table in recovery.tables:
        spans.append((table.table_va, table.table_end_va))
        if table.index_table_va is not None and table.index_bytes:
            spans.append((table.index_table_va, table.index_table_va + len(table.index_bytes)))
    return tuple(spans)


def _null_for(
    image: Image,
    name: str,
    min_entries: int,
    bases: frozenset[int],
    spans: tuple[tuple[int, int], ...],
) -> NullResult:
    return false_positive_null(
        image,
        name,
        min_entries=min_entries,
        scan_limit=DEFAULT_SCAN_LIMIT,
        exclude=bases,
        exclude_spans=spans,
    )


def _observed_rate(
    image: Image, name: str, bases: frozenset[int], min_entries: int
) -> tuple[float, int]:
    """(share of recovered bases in `name` that fire the scan rule, how many there are)."""
    region = next((candidate for candidate in image.regions if candidate.name == name), None)
    if region is None:
        return 0.0, 0
    inside = [base for base in sorted(bases) if region.contains(base)]
    if not inside:
        return 0.0, 0
    fired = sum(
        1
        for base in inside
        if scan_targets(image, base, limit=DEFAULT_SCAN_LIMIT, min_entries=min_entries)
    )
    return fired / len(inside), len(inside)


def _describe_markers(recovery: Recovery, markers: list[int]) -> str:
    """How much of a caller-supplied failure set this module resolves."""
    wanted = frozenset(markers)
    resolved = recovery.functions_with_tables & wanted
    tables = [table for table in recovery.tables if table.function_va in wanted]
    sites = [site for site in recovery.unresolved if site.function_va in wanted]
    lines = [
        f"\nmarker functions  {len(wanted):,} supplied",
        f"  with a table    {len(resolved):,}  ({_share(len(resolved), len(wanted)):.1%})",
        f"  tables found    {len(tables):,}",
        f"  entries found   {sum(table.entry_count for table in tables):,}",
        f"  unresolved      {len(sites):,} indirect jumps inside them",
    ]
    counts: dict[str, int] = {}
    for site in sites:
        counts[site.cause] = counts.get(site.cause, 0) + 1
    for cause, count in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
        lines.append(f"    {cause:<24}{count:>7,}  {_share(count, len(sites)):>6.1%}")
    return "\n".join(lines)


def _tabulate(tables: list[JumpTable], top: int) -> str:
    """Tables by descending entry count, ties ascending by `jump_va`."""
    if not tables:
        return "\nno tables"
    ranked = sorted(tables, key=lambda table: (-table.entry_count, table.jump_va))[: max(0, top)]
    lines = [
        f"\ntop {len(ranked)} tables by entry count",
        f"  {'jump_va':<12}{'function':<12}{'table_va':<12}{'n':>4}{'bound':>7}  "
        f"{'form':<18}suspect",
    ]
    for table in ranked:
        function = "" if table.function_va is None else f"{table.function_va:#010x}"
        bound = "" if table.bound is None else str(table.bound)
        lines.append(
            f"  {table.jump_va:#010x}  {function:<12}{table.table_va:#010x}"
            f"  {table.entry_count:>4}{bound:>7}  {table.form:<18}{'|'.join(table.suspect)}"
        )
    return "\n".join(lines)


def _share(part: int, whole: int) -> float:
    """`part / whole`, or 0.0 when `whole` is zero rather than dividing by it."""
    if whole == 0:
        return 0.0
    return part / whole


if __name__ == "__main__":
    raise SystemExit(main())
