# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the lifecycle-order alignment end to end and print the checks before the names.

THE OUTPUT ORDER IS DELIBERATE. The dispatcher survey comes first, then the cross-checks,
then the proposals and only then the counts. Anyone reading this output should have seen
whether the self-checks passed before seeing a single proposed name, because the names
are worthless if they did not, and a list of plausible-looking identifiers at the top of
a report is how an unverified guess ends up applied to a decompilation.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from tools.codediff.boundaries import load_functions
from tools.seqalign.align import Alignment, ScoringModel
from tools.seqalign.crosscheck import GroupAssignment, parallel_dispatchers, run_cross_checks
from tools.seqalign.dispatcher import rank_dispatchers, survey_premise
from tools.seqalign.donor import load_donor_order, shared_subsystems
from tools.seqalign.pipeline import align_group, build_group_input, select_dispatchers
from tools.seqalign.propose import check_known_names, propose_names, write_proposals
from tools.seqalign.sequences import (
    build_thunk_map,
    extract_call_sequences,
    first_occurrences,
    global_fan_in,
    resolve_sequence,
    sizes_by_entry,
)
from tools.xbe import parse_xbe

TEXT_SECTION = ".text"


def _median(values: list[float]) -> float:
    """Median of a non-empty list. Callers guard emptiness, so this does not."""
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2 == 1:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2.0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="tools.seqalign.cli",
        description=(
            "Propose names for x86 TSFP functions by aligning a dispatcher's call order "
            "against the TS2 donor's lifecycle link order."
        ),
        epilog=(
            "The donor is MIPS and the target x86, so no byte-level evidence exists and "
            "tools/codediff does not apply. Read the CROSS-CHECKS section of the output "
            "before reading the proposals: when it says FAIL, every confidence is capped "
            "and no name in the CSV should be applied."
        ),
    )
    parser.add_argument("--xbe", type=Path, required=True, help="target XBE (retail default.xbe)")
    parser.add_argument(
        "--functions",
        type=Path,
        required=True,
        help="ExportFunctionBounds.java CSV for the target",
    )
    parser.add_argument("--donor", type=Path, required=True, help="gen_donor_symbols.py --out JSON")
    parser.add_argument("--out", type=Path, default=None, help="proposals CSV to write")
    parser.add_argument(
        "--pin",
        action="append",
        default=[],
        metavar="SUFFIX=VA",
        help="pin a suffix group to a dispatcher VA, e.g. Make=0x0012a340; repeatable",
    )
    parser.add_argument(
        "--top", type=int, default=20, help="dispatcher candidates to list (default 20)"
    )
    parser.add_argument(
        "--groups",
        type=int,
        default=3,
        help="how many suffix groups to align when not pinned (default 3)",
    )
    parser.add_argument(
        "--min-distinct",
        type=int,
        default=15,
        help="distinct targets a candidate needs to enter the premise survey (default 15)",
    )
    parser.add_argument(
        "--size-weight",
        type=float,
        default=ScoringModel().size_weight,
        help="weight of the size term; 0 makes the substitution score uniform",
    )
    parser.add_argument(
        "--gap-open", type=float, default=ScoringModel().gap_open, help="gap-open penalty (<= 0)"
    )
    parser.add_argument(
        "--gap-extend",
        type=float,
        default=ScoringModel().gap_extend,
        help="per-element gap penalty (<= 0)",
    )
    return parser


def _parse_pins(raw: list[str]) -> dict[str, int]:
    pins: dict[str, int] = {}
    for item in raw:
        suffix, _, value = item.partition("=")
        if not suffix or not value:
            raise ValueError(f"--pin expects SUFFIX=VA, got {item!r}")
        pins[suffix] = int(value, 0)
    return pins


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    model = ScoringModel(
        size_weight=args.size_weight, gap_open=args.gap_open, gap_extend=args.gap_extend
    )

    xbe = parse_xbe(args.xbe.read_bytes())
    section = xbe.section_by_name(TEXT_SECTION)
    if section is None:
        print(f"{args.xbe}: no {TEXT_SECTION} section")
        return 1
    data = args.xbe.read_bytes()
    text = data[section.raw_addr : section.raw_addr + section.raw_size]
    base_va = section.virtual_addr

    functions = load_functions(args.functions)
    donor = load_donor_order(args.donor)

    print(f"target  {args.xbe}: {TEXT_SECTION} {len(text)} bytes at 0x{base_va:08x}")
    print(f"bounds  {args.functions}: {len(functions)} functions")
    print(f"donor   {donor.donor}")

    raw = extract_call_sequences(text, base_va, functions)
    thunks = build_thunk_map(text, base_va, functions)
    sequences = {va: resolve_sequence(sequence, thunks) for va, sequence in raw.items()}
    fan_in = global_fan_in(sequences.values())
    target_sizes = sizes_by_entry(functions)
    print(
        f"decoded {sum(s.length for s in sequences.values())} direct calls, "
        f"{sum(s.indirect_calls for s in sequences.values())} indirect, "
        f"{len(thunks.targets)} thunks resolved"
    )

    candidates = rank_dispatchers(sequences.values(), fan_in)
    print(f"\nDISPATCHER CANDIDATES ({len(candidates)} scored, top {args.top})")
    print(
        f"{'entry_va':<12}{'calls':>6}{'distinct':>9}{'lowfan':>7}{'indir':>6}{'bytes':>8}{'score':>9}"
    )
    for candidate in candidates[: args.top]:
        print(
            f"0x{candidate.entry_va:08x}  {candidate.call_count:>5}{candidate.distinct_targets:>9}"
            f"{candidate.low_fan_in_targets:>7}{candidate.indirect_calls:>6}"
            f"{candidate.size_bytes:>8}{candidate.score:>9.1f}"
        )

    premise = survey_premise(sequences.values(), min_distinct=args.min_distinct)
    print(
        f"\nPREMISE TEST -- does any function call its targets in address (link) order?"
        f"\n{len(premise)} candidates with >= {args.min_distinct} distinct targets"
    )
    if premise:
        observed = [result.concordance or 0.0 for result in premise]
        nulls = [result.shuffled_null or 0.0 for result in premise]
        print(f"  observed concordance: median {_median(observed):.4f}, max {max(observed):.4f}")
        print(f"  shuffled null:        median {_median(nulls):.4f}, max {max(nulls):.4f}")
        for threshold in (0.90, 0.80, 0.70):
            hits = sum(1 for value in observed if value >= threshold)
            print(f"  >= {threshold:.2f}: {hits} candidates")
        print("  best 5:")
        for result in premise[:5]:
            print(
                f"    0x{result.entry_va:08x} n={result.distinct_targets:>3} "
                f"concordance {result.concordance:.3f} null {result.shuffled_null:.3f} "
                f"lift {result.lift:+.3f}"
            )

    pair_enrichments: list[float] = []
    top_vas = [candidate.entry_va for candidate in candidates[: args.top]]
    for left_index, left_va in enumerate(top_vas):
        for right_va in top_vas[left_index + 1 :]:
            local = parallel_dispatchers(
                first_occurrences(sequences[left_va].targets),
                first_occurrences(sequences[right_va].targets),
            )
            if local.enrichment is not None and local.shared >= args.min_distinct:
                pair_enrichments.append(local.enrichment)
    if pair_enrichments:
        print(
            f"  parallel-dispatcher co-locality over {len(pair_enrichments)} candidate pairs: "
            f"median enrichment {_median(pair_enrichments):.2f}x, "
            f"max {max(pair_enrichments):.2f}x, "
            f">=3x: {sum(1 for value in pair_enrichments if value >= 3.0)}"
        )

    suffixes = donor.groups()[: args.groups]
    pins = _parse_pins(args.pin)
    chosen: dict[str, int] = {}
    if pins:
        chosen = dict(pins)
        suffixes = [suffix for suffix in donor.groups() if suffix in pins]
    else:
        chosen = {
            suffix: candidate.entry_va
            for suffix, candidate in select_dispatchers(candidates, suffixes).items()
        }
        suffixes = [suffix for suffix in suffixes if suffix in chosen]

    print(f"\nDONOR GROUPS: {[(s, len(donor.link_order[s])) for s in suffixes]}")
    print(f"shared subsystems across those groups: {len(shared_subsystems(donor, suffixes))}")
    for left_index, left in enumerate(suffixes):
        for right in suffixes[left_index + 1 :]:
            agree, total = donor.concordance(left, right)
            rate = "n/a" if total == 0 else f"{agree / total:.3f}"
            print(f"  donor concordance {left} vs {right}: {rate} over {total} pairs")

    groups: dict[str, GroupAssignment] = {}
    alignments: dict[str, Alignment] = {}
    for suffix in suffixes:
        dispatcher_va = chosen[suffix]
        sequence = sequences.get(dispatcher_va)
        if sequence is None:
            print(f"  {suffix}: no function at 0x{dispatcher_va:08x}, skipped")
            continue
        group_input = build_group_input(suffix, donor, sequence, target_sizes)
        assignment, alignment = align_group(group_input, model)
        groups[suffix] = assignment
        alignments[suffix] = alignment
        print(
            f"  {suffix}: donor {len(group_input.donor_order)} vs target "
            f"{len(group_input.targets)} calls from 0x{dispatcher_va:08x} -> "
            f"{assignment.matched} matched, {assignment.gaps} gaps, "
            f"score {assignment.alignment_score:.2f}"
        )

    if not groups:
        print("\nno group could be aligned; nothing to check and nothing to propose")
        return 1

    checks = run_cross_checks(groups)
    print("\nCROSS-CHECKS")
    for result in checks.internal:
        mark = "ok" if result.passed else "!!"
        rate = "n/a" if result.rate is None else f"{result.rate:.3f}"
        print(f"  [{mark}] {result.label}: {rate} over {result.total} pairs")
    for result in checks.cross_group:
        mark = "ok" if result.passed else "!!"
        rate = "n/a" if result.rate is None else f"{result.rate:.3f}"
        print(f"  [{mark}] {result.label}: {rate} over {result.total} pairs")
    for local in checks.colocalities:
        enrichment = "n/a" if local.enrichment is None else f"{local.enrichment:.2f}x"
        print(
            f"  [--] {local.label}: median gap {local.median_gap}, "
            f"null {local.median_null_gap}, enrichment {enrichment} over {local.shared} shared"
        )
    trustworthy, reason = checks.verdict()
    print(f"\nVERDICT: {'TRUSTWORTHY' if trustworthy else 'NOT TRUSTWORTHY'} -- {reason}")

    proposals = propose_names(groups, alignments, checks)
    control = check_known_names(proposals, functions)
    print(
        f"\nGROUND TRUTH: {control.tested} of {control.proposals} proposals land on one of "
        f"{control.known_names_available} already-named functions; "
        f"{control.agreed} agree, {control.disagreed} contradict"
    )
    print(f"  {control.note}")
    for target_va, proposed, known in control.examples[:5]:
        print(f"    0x{target_va:08x}: proposed {proposed}, already known as {known}")

    buckets = {">=0.80": 0, "0.50-0.79": 0, "0.20-0.49": 0, "<0.20": 0}
    for proposal in proposals:
        if proposal.confidence >= 0.80:
            buckets[">=0.80"] += 1
        elif proposal.confidence >= 0.50:
            buckets["0.50-0.79"] += 1
        elif proposal.confidence >= 0.20:
            buckets["0.20-0.49"] += 1
        else:
            buckets["<0.20"] += 1
    print(f"\nPROPOSALS: {len(proposals)} total, by confidence {buckets}")

    if args.out is not None:
        written = write_proposals(args.out, proposals)
        print(f"wrote {written} rows -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
