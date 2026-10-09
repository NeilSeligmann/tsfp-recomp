# SPDX-License-Identifier: GPL-3.0-or-later
"""Report function entries one build's analysis missed, using the other build.

Normalises both XBEs' `.text`, matches runs between them, then pushes the donor's
function table through the resulting address mapping. A predicted entry the target
has no function for is a candidate the target MISSED. See
`tools/codediff/boundaries.py` for why run boundaries themselves are useless as
function boundaries and the address mapping is the usable product.

Everything is written to stdout, one record per line, so the report pipes.

Usage, demo as donor and retail as target:

    ./.venv/bin/python -m tools.codediff.cli \\
        tmp/oxm-extract/disc46/tsdemo_cd.xbe tmp/oxm-extract/retail/default.xbe \\
        --donor-functions generated/demo/functions.csv \\
        --target-functions generated/retail/functions.csv

THE NULLS ARE PART OF THE REPORT. An agreement percentage with nothing to compare it
against is not interpretable, so two null rates are recomputed from the same inputs
every run and printed alongside: the rate at which an arbitrary address inside a
matched run happens to be a target function entry, and the rate at which an arbitrary
target `.text` address happens to be a `call rel32` target. The enrichment factors
printed next to them are the actual claims. On the demo/retail pair those nulls come
out at 0.2956% and 0.2273%, against 98.3% agreement and 47.2% of candidates
call-targeted, so roughly 333x and 208x.

They are recomputed rather than quoted precisely because an earlier hand measurement
put the first null at 0.2% and so claimed 490x. It does not reproduce under any exact
definition; see `tools/codediff/boundaries.py` for the variants that were tried. A
null carried around as a constant is a null nobody will notice going stale.
"""

from __future__ import annotations

import argparse
import csv
from bisect import bisect_left
from dataclasses import dataclass
from pathlib import Path

from tools.codediff.boundaries import (
    Candidate,
    Correspondence,
    Function,
    correspond,
    count_call_sites,
    filter_plausible,
    load_functions,
)
from tools.codediff.match import MatchedRun, coverage, find_runs
from tools.codediff.normalise import DEFAULT_SECTION_NAME, NormalisedText, normalise_text
from tools.xbe import parse_xbe

DEFAULT_MIN_INSNS = 8
DEFAULT_TOP = 20

CANDIDATE_CSV_HEADER = ("target_va", "call_sites", "donor_va", "donor_size", "donor_name")


@dataclass(frozen=True)
class Build:
    """One build's `.text`, normalised, with its function table alongside."""

    label: str
    path: Path
    text: NormalisedText
    functions: list[Function]

    @property
    def entry_vas(self) -> set[int]:
        return {function.entry_va for function in self.functions}


def load_build(label: str, xbe_path: Path, csv_path: Path) -> Build:
    """Parse an XBE, normalise its `.text`, and read its function-bounds CSV.

    `.text` bytes are taken as `data[raw_addr : raw_addr + raw_size]`, and the image
    range passed to the normaliser is `[base_address, base_address + size_of_image)`,
    so an immediate pointing anywhere into the loaded image is treated as a pointer
    and blanked.
    """
    data = xbe_path.read_bytes()
    xbe = parse_xbe(data)
    section = xbe.section_by_name(DEFAULT_SECTION_NAME)
    if section is None:
        raise ValueError(f"{xbe_path}: no {DEFAULT_SECTION_NAME} section")
    body = data[section.raw_addr : section.raw_addr + section.raw_size]
    text = normalise_text(
        body,
        section.virtual_addr,
        xbe.base_address,
        xbe.base_address + xbe.size_of_image,
        section_name=section.name,
    )
    return Build(label=label, path=xbe_path, text=text, functions=load_functions(csv_path))


def entry_density_in_runs(runs: list[MatchedRun], target_entries: set[int]) -> float:
    """Fraction of target addresses covered by `runs` that are target entries.

    This is the null for `Correspondence.agreement`. Agreement asks how often a
    *donor entry* maps onto a target entry; this asks how often an arbitrary mapped
    address does, which is what the same measurement reduces to when the donor table
    carries no information. Computed exactly rather than sampled, so it does not need
    a seed to be reproducible.
    """
    total = sum(run.length for run in runs)
    if total <= 0:
        return 0.0
    ordered = sorted(target_entries)
    hits = 0
    for run in runs:
        lo = bisect_left(ordered, run.retail_va)
        hi = bisect_left(ordered, run.retail_va + run.length)
        hits += hi - lo
    return hits / total


def call_target_density(call_sites: dict[int, int], lo: int, hi: int) -> float:
    """Fraction of addresses in `[lo, hi)` that any `call rel32` targets.

    This is the null for "how many candidates are call-targeted". Distinct targets,
    not sites, because a candidate is scored on whether it is targeted at all.
    """
    if hi <= lo:
        return 0.0
    targeted = sum(1 for va in call_sites if lo <= va < hi)
    return targeted / (hi - lo)


def enrichment(observed: float, null: float) -> float:
    """`observed / null`, or 0.0 when the null is zero rather than dividing by it."""
    if null <= 0.0:
        return 0.0
    return observed / null


def write_candidates_csv(path: Path, candidates: list[Candidate]) -> int:
    """Write `candidates` to `path` as CSV, ascending by `target_va`. Returns the count.

    This is the machine-readable half of the report, fed to
    `tools/ghidra/CreateFunctionsAt.java` to push the predictions back into Ghidra.
    `target_va` and `donor_va` are 0x-prefixed hex zero-padded to 8 digits, matching
    `ExportFunctionBounds.java`, so the two tables join on address text alone.
    `donor_size` is decimal, for the same reason.
    """
    if path.parent != Path(""):
        path.parent.mkdir(parents=True, exist_ok=True)
    ordered = sorted(candidates, key=lambda c: c.target_va)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(CANDIDATE_CSV_HEADER)
        for candidate in ordered:
            writer.writerow(
                (
                    f"{candidate.target_va:#010x}",
                    candidate.call_sites,
                    f"{candidate.donor_va:#010x}",
                    candidate.donor_size,
                    candidate.donor_name,
                )
            )
    return len(ordered)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Predict function entries the target build's analysis missed, by mapping "
            "the donor build's function table through matched code runs."
        )
    )
    parser.add_argument("donor_xbe", type=Path, help="XBE whose function table is trusted")
    parser.add_argument("target_xbe", type=Path, help="XBE whose missed entries are wanted")
    parser.add_argument(
        "--donor-functions",
        type=Path,
        required=True,
        metavar="CSV",
        help="ExportFunctionBounds.java CSV for the donor XBE",
    )
    parser.add_argument(
        "--target-functions",
        type=Path,
        required=True,
        metavar="CSV",
        help="ExportFunctionBounds.java CSV for the target XBE",
    )
    parser.add_argument(
        "--min-insns",
        type=int,
        default=DEFAULT_MIN_INSNS,
        metavar="N",
        help=f"shortest run to accept, in instructions (default {DEFAULT_MIN_INSNS})",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=DEFAULT_TOP,
        metavar="N",
        help=f"candidates to tabulate, by call-site count (default {DEFAULT_TOP})",
    )
    parser.add_argument(
        "--candidates-csv",
        type=Path,
        default=None,
        metavar="CSV",
        help=(
            "also write the plausible candidates to this CSV "
            f"({','.join(CANDIDATE_CSV_HEADER)}); stdout is unchanged"
        ),
    )
    parser.add_argument(
        "--include-implausible",
        action="store_true",
        help=(
            "write candidates the plausibility gate rejected as well. These are "
            "mostly mid-function labels and creating them DESTROYS correct function "
            "boundaries, so this is for analysis of the gate itself, not for feeding "
            "back into a decompiler"
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    donor = load_build("donor", args.donor_xbe, args.donor_functions)
    target = load_build("target", args.target_xbe, args.target_functions)
    for build in (donor, target):
        print(_describe(build))

    runs = find_runs(donor.text, target.text, min_insns=args.min_insns)
    covered = sum(run.length for run in runs)
    fraction = coverage(runs, len(donor.text.data))
    print(
        f"\n{len(runs):,} matched runs at min_insns={args.min_insns}, covering "
        f"{covered:,} bytes = {fraction:.2%} of the donor's {donor.text.section_name}"
    )

    target_entries = target.entry_vas
    call_sites = count_call_sites(target.text.data, target.text.base_va)
    result = correspond(runs, donor.functions, target_entries, call_sites=call_sites)
    print(_describe_correspondence(result, target))
    print(_describe_nulls(result, runs, target_entries, call_sites, target))
    plausible, implausible = filter_plausible(
        result.candidates, target.text.data, target.text.base_va
    )
    print(
        f"\nplausibility gate (a terminating instruction must end where the entry begins)\n"
        f"  kept      {len(plausible):,} of {len(result.candidates):,}\n"
        f"  rejected  {len(implausible):,} as mid-function labels"
    )
    print(_tabulate(plausible, args.top))

    if args.candidates_csv is not None:
        # Default to the gated set. Applying a mid-function candidate does not merely
        # waste effort, it DESTROYS a correct boundary: measured, ten such candidates
        # reduced a correct function to a 1-4 byte stub, the worst turning 603 bytes
        # into 1. Emitting the rejects by default would invite exactly that.
        emit = result.candidates if args.include_implausible else plausible
        written = write_candidates_csv(args.candidates_csv, emit)
        gated = "ALL, INCLUDING IMPLAUSIBLE" if args.include_implausible else "plausible only"
        print(f"\nwrote {written:,} candidates to {args.candidates_csv} ({gated})")
    return 0


def _describe(build: Build) -> str:
    text = build.text
    return (
        f"{build.label:<7}{build.path.name:<16} {text.section_name} {len(text.data):,} bytes "
        f"at {text.base_va:#010x}, {len(text.insns):,} instructions, "
        f"{len(text.undecodable):,} undecodable, {len(build.functions):,} functions"
    )


def _describe_correspondence(result: Correspondence, target: Build) -> str:
    text = target.text
    lo, hi = text.base_va, text.base_va + len(text.data)
    inside = sum(1 for candidate in result.candidates if lo <= candidate.target_va < hi)
    targeted = sum(1 for candidate in result.candidates if candidate.call_sites > 0)
    total = len(result.candidates)
    share = targeted / total if total else 0.0
    return (
        f"\nmapped      {result.mapped:,} donor entries landed inside a run\n"
        f"agreed      {result.agreed:,} of those are already a target function entry\n"
        f"agreement   {result.agreement:.1%}\n"
        f"candidates  {total:,} predicted entries the target is missing "
        f"({inside:,} inside {text.section_name})\n"
        f"calltargets {targeted:,} of {total:,} ({share:.1%}) are reached by a `call rel32`"
    )


def _describe_nulls(
    result: Correspondence,
    runs: list[MatchedRun],
    target_entries: set[int],
    call_sites: dict[int, int],
    target: Build,
) -> str:
    """The two null rates and the enrichments over them. See the module docstring."""
    text = target.text
    lo, hi = text.base_va, text.base_va + len(text.data)
    entry_null = entry_density_in_runs(runs, target_entries)
    call_null = call_target_density(call_sites, lo, hi)
    total = len(result.candidates)
    targeted = sum(1 for candidate in result.candidates if candidate.call_sites > 0)
    call_observed = targeted / total if total else 0.0
    rows = (
        (
            "an arbitrary mapped address is a target entry",
            entry_null,
            result.agreement,
            "agreement",
        ),
        (
            f"an arbitrary {text.section_name} address is a call target",
            call_null,
            call_observed,
            "call-targeting",
        ),
    )
    lines = ["\nnull calibration (recomputed from these inputs, never quoted)"]
    for label, null, observed, what in rows:
        lines.append(
            f"  {label:<52}{null:>8.4%}   {what} is {enrichment(observed, null):,.0f}x that null"
        )
    lines.append("  a count of call sites over-counts, because 0xE8 occurs inside other")
    lines.append("  instructions and inside data, so it is evidence and never proof")
    return "\n".join(lines)


def _tabulate(candidates: list[Candidate], top: int) -> str:
    """Candidates by descending call-site count, ties ascending by `target_va`."""
    if not candidates:
        return "\nno candidates"
    ranked = sorted(candidates, key=lambda c: (-c.call_sites, c.target_va))[: max(0, top)]
    lines = [
        f"\ntop {len(ranked)} candidates by call-site count",
        f"  {'target_va':<12}{'calls':>6}  {'donor_va':<12}{'size':>7}  donor name",
    ]
    for candidate in ranked:
        lines.append(
            f"  {candidate.target_va:#010x}  {candidate.call_sites:>6,}  "
            f"{candidate.donor_va:#010x}  {candidate.donor_size:>7,}  {candidate.donor_name}"
        )
    return "\n".join(lines)


if __name__ == "__main__":
    raise SystemExit(main())
