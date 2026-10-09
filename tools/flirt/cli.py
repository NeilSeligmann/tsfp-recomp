# SPDX-License-Identifier: GPL-3.0-or-later
"""Propose XDK library names for recovered functions, and measure the proposals.

Reads one or more IDA `.pat` files from a local, uncommitted directory, matches them
against the retail XBE's `.text` at the entry points in
`generated/retail/functions.csv`, and writes a proposal CSV. Nothing is applied to
any disassembler project: this tool produces a reviewable file and stops.

Usage::

    ./.venv/bin/python -m tools.flirt.cli \\
        tmp/oxm-extract/retail/default.xbe \\
        --patterns tmp/flirt \\
        --functions generated/retail/functions.csv \\
        --xtlid-db tmp/flirt/xtlid.xml \\
        --out generated/retail/flirt_names.csv \\
        --ghidra-names generated/retail/flirt_names.txt

THE HEADLINE OF THE REPORT IS THE `.XTLID` AGREEMENT RATE, printed first and on
purpose. `.XTLID` names 294 addresses exactly, from data already inside the user's
own executable and resolved against an independently built open database. The
matcher is given the function *table* but never those names, so every address where
both have an opinion is a free held-out test. Agreement there estimates precision
everywhere else; disagreement is listed in full, never summarised to a count,
because the identity of what a matcher gets wrong is what tells you whether the
error is cosmetic (a decoration the comparison failed to normalise) or fatal (two
genuinely different functions).

A SECOND NULL IS PRINTED ALONGSIDE: the agreement rate under
`--length-check none --ignore-crc`, i.e. with both of FLIRT's own corroborating
checks switched off. If precision barely moves when the CRC is discarded, the CRC
is not doing any work in this dataset and the headline rate is resting on 32 bytes
of prologue, which is a much weaker claim than it looks.

PROVENANCE. The `.pat` files are a local analysis aid and must never be committed;
see `docs/provenance.md` and `tools/flirt/__init__.py`. The output CSV is written
under `generated/`, which `.gitignore` blocks in full. Pass `--check-paths` to have
this tool refuse to write anywhere that is not covered by an ignore rule.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

from tools.codediff.boundaries import Function, load_functions
from tools.flirt.match import (
    MIN_SIGNIFICANT_BYTES,
    EntryMatch,
    LengthCheck,
    MatchReport,
    PatternIndex,
    match_entries,
)
from tools.flirt.pattern import Pattern, parse_pattern_line
from tools.xbe import parse_xbe
from tools.xtlid import XtlidName, parse_xtlid_db, resolve_xtlid

TEXT_SECTION = ".text"

#: Columns of the proposal CSV, in order.
PROPOSAL_CSV_COLUMNS = (
    "entry_va",
    "proposed_name",
    "library",
    "confidence",
    "corroborated_by_xtlid",
)

#: Paths the proposal CSV may be written to without `--check-paths` complaining.
#: `generated/` and `tmp/` are both blocked wholesale by `.gitignore`.
IGNORED_PREFIXES = ("generated/", "tmp/", "scratch/", "build/")

DEFAULT_TOP = 25


@dataclass(frozen=True)
class Proposal:
    """One row of the proposal CSV."""

    entry_va: int
    proposed_name: str
    library: str
    confidence: str
    corroborated_by_xtlid: str

    def as_row(self) -> tuple[str, str, str, str, str]:
        return (
            f"{self.entry_va:#010x}",
            self.proposed_name,
            self.library,
            self.confidence,
            self.corroborated_by_xtlid,
        )


@dataclass
class Disagreement:
    """One address where FLIRT and `.XTLID` both have a name and they differ."""

    entry_va: int
    flirt_name: str
    xtlid_name: str
    library: str
    xtlid_library: str


@dataclass
class XtlidComparison:
    """FLIRT's proposals scored against the `.XTLID` ground truth."""

    overlap: int = 0
    """Addresses where FLIRT proposed a confident name and `.XTLID` knows one."""

    exact: int = 0
    """Of those, byte-identical names."""

    normalised: int = 0
    """Of those, equal after stripping compiler decoration. Includes `exact`."""

    disagreements: list[Disagreement] = field(default_factory=list)
    xtlid_known: int = 0
    """Addresses `.XTLID` names that are also in the function table at all."""

    xtlid_missed: int = 0
    """Of those, ones FLIRT proposed no confident name for. Recall, not precision."""

    @property
    def agreement(self) -> float:
        """`normalised / overlap`, or 0.0 with no overlap.

        Reported on the normalised comparison because the exact one is dominated by
        calling-convention decoration: FLIRT names come from `.lib` symbol tables
        and carry MSVC's `_name@4` adornment, while the `.XTLID` database stores
        undecorated source names. A decoration difference is not a wrong answer.
        Both numerators are printed so the gap between them is visible.
        """
        if self.overlap == 0:
            return 0.0
        return self.normalised / self.overlap


#: Trailing `@<digits>` is MSVC's stdcall/fastcall argument-byte suffix.
_STDCALL_SUFFIX = re.compile(r"@\d+$")


def normalise_symbol(name: str) -> str:
    """Reduce a symbol to the bare identifier, for comparing across name sources.

    The same function reaches us under several spellings. A `.lib` symbol table
    gives MSVC's decorated form; the `.XTLID` database gives the undecorated source
    name. Normalising means a comparison measures whether the two sources picked the
    same *function*, which is the question, rather than whether they picked the same
    *decoration*, which is not.

    Applied in order: an MSVC C++ mangled name (`?` then the identifier then `@@`)
    is reduced to its identifier; otherwise a single leading `_` or `@`
    (cdecl/fastcall) is dropped and a trailing `@<digits>` (stdcall) with it.
    Comparison is case sensitive, because Win32 naming is and two symbols differing
    only in case are two symbols.

    This is deliberately conservative. It does not undo full C++ mangling, so two
    overloads of one C++ name normalise to the same string -- which is why a
    normalised agreement is reported next to the exact one rather than instead of
    it.
    """
    text = name.strip()
    if text.startswith("?"):
        identifier = text[1:].split("@@", 1)[0]
        return identifier.split("@", 1)[0] if identifier else text
    if text[:1] in ("_", "@"):
        text = text[1:]
    return _STDCALL_SUFFIX.sub("", text)


def load_patterns(paths: Sequence[Path]) -> tuple[list[Pattern], list[str]]:
    """Parse every `.pat` file named, or every `.pat` inside a directory named.

    Returns (patterns, errors). A line that does not parse becomes one entry in
    `errors` rather than an exception, because a third-party pattern set will
    contain constructs this parser does not claim to support and aborting the whole
    run on the first of them would make the tool useless. The library API
    (`parse_pattern_line`) still raises; the *count and the reasons* are reported
    here, which is the opposite of silently skipping them.

    Files are visited in sorted order so the resulting pattern order, and therefore
    every tie-break downstream, is reproducible.
    """
    files: list[Path] = []
    for path in paths:
        if path.is_dir():
            files.extend(sorted(path.rglob("*.pat")))
        else:
            files.append(path)

    patterns: list[Pattern] = []
    errors: list[str] = []
    for file in sorted(set(files)):
        library = file.stem
        for number, line in enumerate(file.read_text(encoding="latin-1").splitlines(), start=1):
            try:
                pattern = parse_pattern_line(line, library=library, source_line=number)
            except ValueError as error:
                errors.append(str(error))
                continue
            if pattern is not None:
                patterns.append(pattern)
    return patterns, errors


def load_text(xbe_path: Path) -> tuple[bytes, int]:
    """The retail XBE's `.text` bytes and the virtual address of its first byte."""
    data = xbe_path.read_bytes()
    xbe = parse_xbe(data)
    section = xbe.section_by_name(TEXT_SECTION)
    if section is None:
        raise ValueError(f"{xbe_path}: no {TEXT_SECTION} section")
    body = data[section.raw_addr : section.raw_addr + section.raw_size]
    return body, section.virtual_addr


def load_xtlid_names(xbe_path: Path, db_path: Path) -> dict[int, XtlidName]:
    """Resolve the XBE's own `.XTLID` records against the open name database.

    Reuses `tools/xtlid.py` wholesale. The database is fetched, never committed; see
    `tools/ghidra/setup.sh` for the pinned URL.
    """
    xbe = parse_xbe(xbe_path.read_bytes())
    database = parse_xtlid_db(db_path.read_text(encoding="utf-8", errors="replace"))
    resolved, _ = resolve_xtlid(xbe.xtlid, database)
    return resolved


def compare_with_xtlid(
    matches: Sequence[EntryMatch],
    xtlid: dict[int, XtlidName],
    entry_vas: set[int],
) -> XtlidComparison:
    """Score confident FLIRT proposals against the `.XTLID` names for the same VAs.

    `entry_vas` is the full function table, used only to compute how many `.XTLID`
    addresses were even testable: an `.XTLID` name at an address our analysis has no
    function for is outside what the matcher was asked to do, and counting it as a
    miss would conflate the matcher's recall with Ghidra's.
    """
    comparison = XtlidComparison()
    proposed = {match.entry_va: match for match in matches if match.confident_name is not None}

    for entry_va in sorted(xtlid):
        if entry_va not in entry_vas:
            continue
        comparison.xtlid_known += 1
        match = proposed.get(entry_va)
        if match is None:
            comparison.xtlid_missed += 1
            continue
        truth = xtlid[entry_va]
        flirt_name = match.confident_name
        assert flirt_name is not None
        comparison.overlap += 1
        if flirt_name == truth.name:
            comparison.exact += 1
            comparison.normalised += 1
            continue
        if normalise_symbol(flirt_name) == normalise_symbol(truth.name):
            comparison.normalised += 1
            continue
        comparison.disagreements.append(
            Disagreement(
                entry_va=entry_va,
                flirt_name=flirt_name,
                xtlid_name=truth.name,
                library="|".join(match.libraries),
                xtlid_library=truth.library,
            )
        )
    return comparison


def build_proposals(
    matches: Sequence[EntryMatch],
    xtlid: dict[int, XtlidName],
    *,
    include_ambiguous: bool,
) -> list[Proposal]:
    """Turn matches into CSV rows, ascending by address.

    An ambiguous match is never confident. With `include_ambiguous` it is emitted
    with `confidence=ambiguous` and every candidate name joined by `|`, so a human
    can look at it; without, it is left out entirely. Either way it is not a name.

    `confidence` is `high` when at least one matching pattern carried a CRC that was
    verified, and `medium` when the match rests on pattern and tail bytes alone.
    There is no `low`: a match either met every check the pattern could offer or it
    did not match.
    """
    proposals: list[Proposal] = []
    for match in sorted(matches, key=lambda item: item.entry_va):
        truth = xtlid.get(match.entry_va)
        name = match.confident_name
        if name is None:
            if not include_ambiguous:
                continue
            proposals.append(
                Proposal(
                    entry_va=match.entry_va,
                    proposed_name="|".join(match.proposed_names),
                    library="|".join(match.libraries),
                    confidence="ambiguous",
                    corroborated_by_xtlid=_corroboration(match.proposed_names, truth),
                )
            )
            continue
        proposals.append(
            Proposal(
                entry_va=match.entry_va,
                proposed_name=name,
                library="|".join(match.libraries),
                confidence="high" if match.crc_verified else "medium",
                corroborated_by_xtlid=_corroboration((name,), truth),
            )
        )
    return proposals


def write_proposals(path: Path, proposals: Sequence[Proposal]) -> None:
    """Write the proposal CSV, header included."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(PROPOSAL_CSV_COLUMNS)
        for proposal in proposals:
            writer.writerow(proposal.as_row())


def write_ghidra_names(path: Path, proposals: Sequence[Proposal]) -> None:
    """Write the `address name  # comment` table `tools/ghidra/ApplyNames.java` eats.

    ApplyNames.java does NOT read the proposal CSV. It splits each line on
    whitespace into exactly two fields and strips anything after a `#`, so a CSV row
    would arrive as one unparseable token. This is that adapter, and it is the only
    thing standing between the two formats -- no change to the Java is needed.

    Rows `.XTLID` actively contradicts are dropped rather than commented out: this
    file exists to be applied, and shipping a known-wrong name in it is how a bad
    name ends up in the project. Ambiguous rows are dropped for the same reason.

    SO ARE REPEATED NAMES, and that one is not obvious from a confidence score. A
    name proposed at more than one address is a weak pattern firing repeatedly, not a
    discovery: MEASURED on retail, 6 names account for 43 rows and one appears **27
    times**. Every one of the 75 `.XTLID`-corroborated rows has a unique name, so the
    repeats are cleanly separable and dropping them costs no ground truth -- 363 rows
    become 320, all 75 retained. They stay in the proposal CSV for review; they just
    never reach the file whose purpose is to be applied.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    repeated = Counter(p.proposed_name for p in proposals)
    lines = [
        "# address name  -- generated by tools/flirt/cli.py",
        "# FLIRT pattern matches against XDK library patterns. UNLIKE the .XTLID",
        "# table these are inferences, not facts: review before trusting. Rows the",
        "# .XTLID section contradicts, ambiguous matches, and names proposed at more",
        "# than one address are all excluded here.",
    ]
    for proposal in proposals:
        if proposal.confidence == "ambiguous" or proposal.corroborated_by_xtlid == "false":
            continue
        if repeated[proposal.proposed_name] > 1:
            continue
        lines.append(
            f"{proposal.entry_va:#010x} {proposal.proposed_name}"
            f"  # {proposal.library} {proposal.confidence}"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    """The command line, split out so tests can exercise it without running a match."""
    parser = argparse.ArgumentParser(
        prog="python -m tools.flirt.cli",
        description="Match IDA FLIRT patterns against recovered function entries.",
    )
    parser.add_argument("xbe", type=Path, help="the retail XBE to match against")
    parser.add_argument(
        "--patterns",
        type=Path,
        nargs="+",
        required=True,
        help="`.pat` files, or directories searched recursively for them. "
        "These must live outside the repository or under an ignored path.",
    )
    parser.add_argument(
        "--functions",
        type=Path,
        default=Path("generated/retail/functions.csv"),
        help="function-bounds CSV from tools/ghidra/ExportFunctionBounds.java",
    )
    parser.add_argument(
        "--xtlid-db",
        type=Path,
        default=None,
        help="xtlid.xml, for the ground-truth comparison. Omitting it skips the "
        "single most important measurement this tool makes.",
    )
    parser.add_argument("--out", type=Path, default=None, help="proposal CSV to write")
    parser.add_argument(
        "--ghidra-names",
        type=Path,
        default=None,
        help="also write an ApplyNames.java-compatible `address name` table",
    )
    parser.add_argument(
        "--length-check",
        choices=[item.value for item in LengthCheck],
        default=LengthCheck.FITS.value,
        help="how strictly a pattern's total-length field must agree (default: fits)",
    )
    parser.add_argument(
        "--include-ambiguous",
        action="store_true",
        help="emit ambiguous matches too, marked as such and never as a name",
    )
    parser.add_argument(
        "--check-paths",
        action="store_true",
        help="refuse to write outside a .gitignore-covered directory",
    )
    parser.add_argument(
        "--min-significant-bytes",
        type=int,
        default=MIN_SIGNIFICANT_BYTES,
        help="drop patterns constraining fewer bytes than this, as evidence too thin "
        f"to mean anything (default: {MIN_SIGNIFICANT_BYTES}; see tools/flirt/match.py "
        "for the sweep that chose it)",
    )
    parser.add_argument(
        "--ignore-crc",
        action="store_true",
        help="skip the CRC check, to MEASURE what it is worth by rerunning without "
        "it. Never a reasonable way to produce names.",
    )
    parser.add_argument("--top", type=int, default=DEFAULT_TOP, help="sample rows to print")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.check_paths:
        for target in (args.out, args.ghidra_names):
            if target is not None and not _is_ignored(target):
                parser.error(
                    f"{target} is not under an ignored path "
                    f"({', '.join(IGNORED_PREFIXES)}); refusing to write name data there"
                )

    patterns, errors = load_patterns(args.patterns)
    if not patterns:
        print("no patterns parsed; nothing to match", file=sys.stderr)
        if errors:
            print(f"{len(errors)} lines failed to parse, first few:", file=sys.stderr)
            for message in errors[:5]:
                print(f"  {message}", file=sys.stderr)
        return 1

    code, base_va = load_text(args.xbe)
    functions = load_functions(args.functions)
    entry_vas = [function.entry_va for function in functions]
    lengths = {function.entry_va: function.size_bytes for function in functions}

    index = PatternIndex(patterns, min_significant_bytes=args.min_significant_bytes)
    report = match_entries(
        index,
        code,
        base_va,
        entry_vas,
        length_check=LengthCheck(args.length_check),
        function_lengths=lengths,
        check_crc=not args.ignore_crc,
    )

    xtlid = {} if args.xtlid_db is None else load_xtlid_names(args.xbe, args.xtlid_db)
    comparison = compare_with_xtlid(report.matches, xtlid, set(entry_vas))
    proposals = build_proposals(report.matches, xtlid, include_ambiguous=args.include_ambiguous)

    _print_report(args, patterns, errors, functions, index, report, comparison, proposals)

    if args.out is not None:
        write_proposals(args.out, proposals)
        print(f"\nwrote {len(proposals)} proposals to {args.out}")
    if args.ghidra_names is not None:
        write_ghidra_names(args.ghidra_names, proposals)
        print(f"wrote an ApplyNames.java table to {args.ghidra_names}")
    return 0


def _corroboration(names: Sequence[str], truth: XtlidName | None) -> str:
    """`true`/`false`/`unknown` for the CSV's `corroborated_by_xtlid` column."""
    if truth is None:
        return "unknown"
    wanted = normalise_symbol(truth.name)
    return "true" if any(normalise_symbol(name) == wanted for name in names) else "false"


def _is_ignored(path: Path) -> bool:
    """Whether `path` sits under a directory `.gitignore` blocks wholesale."""
    text = path.as_posix()
    if path.is_absolute():
        return any(f"/{prefix}" in text for prefix in IGNORED_PREFIXES)
    return any(text.startswith(prefix) for prefix in IGNORED_PREFIXES)


def _print_report(
    args: argparse.Namespace,
    patterns: Sequence[Pattern],
    errors: Sequence[str],
    functions: Sequence[Function],
    index: PatternIndex,
    report: MatchReport,
    comparison: XtlidComparison,
    proposals: Sequence[Proposal],
) -> None:
    """Print the measurement, ground truth first."""
    print("=== .XTLID ground truth (read this before anything else) ===")
    if comparison.xtlid_known == 0:
        print("  no overlap measured: pass --xtlid-db to enable the only held-out test")
    else:
        print(f"  .XTLID names in the function table   {comparison.xtlid_known}")
        print(f"  FLIRT also proposed a name there     {comparison.overlap}")
        print(f"  no FLIRT proposal (recall, not precision) {comparison.xtlid_missed}")
        print(f"  agreed, byte identical               {comparison.exact}")
        print(f"  agreed after normalising decoration  {comparison.normalised}")
        print(f"  DISAGREED                            {len(comparison.disagreements)}")
        print(f"  agreement rate                       {comparison.agreement:.1%}")
    if comparison.disagreements:
        print("\n  every disagreement, in full:")
        for item in comparison.disagreements:
            print(
                f"    {item.entry_va:#010x}  flirt={item.flirt_name!r} "
                f"xtlid={item.xtlid_name!r}  [{item.library} vs {item.xtlid_library}]"
            )

    print("\n=== patterns ===")
    print(f"  files/dirs given        {len(args.patterns)}")
    print(f"  patterns parsed         {len(patterns)}")
    print(f"  lines that failed       {len(errors)}")
    for message in errors[:5]:
        print(f"    {message}")
    with_crc = sum(1 for pattern in patterns if pattern.crc_length > 0)
    print(f"  carrying a CRC          {with_crc}")
    print(f"  too weak to index       {index.too_weak} (<{index.min_significant_bytes} bytes)")
    print(f"  indexed                 {index.count}")
    print(f"  wildcard first byte     {len(index.unindexed)}")

    print("\n=== matching ===")
    print(f"  function entries        {len(functions)}")
    print(f"  entries tested          {report.entries_tested}")
    print(f"  entries outside .text   {report.entries_skipped}")
    print(f"  entries matched         {report.matched}")
    print(f"  of those, ambiguous     {report.ambiguous}")
    print(f"  confident names         {report.confident}")
    print(f"  patterns used           {len(report.used_patterns)}")
    print(f"  patterns unused         {report.unused_patterns}")
    print(f"  rejected by CRC         {report.rejected_by_crc}")
    print(f"  rejected by tail        {report.rejected_by_tail}")
    print(f"  rejected by length      {report.rejected_by_length}")

    print(f"\n=== first {min(args.top, len(proposals))} proposals ===")
    for proposal in proposals[: args.top]:
        print("  " + " ".join(proposal.as_row()))


if __name__ == "__main__":
    raise SystemExit(main())
