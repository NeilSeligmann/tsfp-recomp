# SPDX-License-Identifier: GPL-3.0-or-later
"""Emit the donor's function table as structured data, plus the lifecycle index.

Keeping the 3,976 donor functions as a plain artefact means later matching work
does not re-parse `stdump` output, and the matching corpus can be versioned and
diffed independently of the extraction.

THE LIFECYCLE INDEX IS THE POINT. Free Radical used a rigid per-subsystem
convention — `<sys>Make`, `<sys>End`, `<sys>Preload`, `<sys>Reset`,
`<sys>ResetBefore`, `<sys>ResetAfter`, `<sys>Restart`, `<sys>RestartBefore`,
`<sys>RestartAfter` — and a central dispatcher calls them in link order. That makes
them the cheapest naming attack available on TSFP: find one dispatcher, read its
call list in order, and a few dozen names fall out in a single pass, then recurse
from there.

Names transfer as hypotheses, not facts. The donor is the previous game.
"""

from __future__ import annotations

import argparse
import itertools
import json
import re
from collections import defaultdict
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

from tools.stabs import FunctionDef, parse_functions

#: The lifecycle suffixes, longest first so `ResetBefore` is not read as `Reset`.
LIFECYCLE_SUFFIXES = (
    "RestartBefore",
    "RestartAfter",
    "ResetBefore",
    "ResetAfter",
    "Preload",
    "Restart",
    "Reset",
    "Make",
    "End",
)

RE_SUBSYSTEM = re.compile(r"/game/([^/]+)/")


@dataclass(frozen=True)
class LifecycleEntry:
    subsystem: str
    suffix: str
    name: str
    address: int


def subsystem_of(function: FunctionDef) -> str | None:
    """The engine subsystem directory a function came from, if it is engine code."""
    if not function.source_file:
        return None
    match = RE_SUBSYSTEM.search(function.source_file)
    return match.group(1) if match else None


def lifecycle_entries(functions: Iterable[FunctionDef]) -> list[LifecycleEntry]:
    """Find every `<prefix><LifecycleSuffix>` function.

    Matching is on the name rather than the source directory, because the prefix is
    not always the directory name — `bossMakeAll` lives in `boss/` but `obMake`
    may not live in `ob/`. The prefix as written is what a TSFP call graph will
    show, so that is what gets recorded.
    """
    found: list[LifecycleEntry] = []
    for function in functions:
        name = function.name
        if not name:
            continue
        for suffix in LIFECYCLE_SUFFIXES:
            if name.endswith(suffix) and len(name) > len(suffix):
                prefix = name[: -len(suffix)]
                # A lowercase-initial prefix is the convention; anything else is
                # coincidence, e.g. a function that merely ends in "End".
                if not prefix[0].islower():
                    continue
                found.append(
                    LifecycleEntry(
                        subsystem=prefix,
                        suffix=suffix,
                        name=name,
                        address=function.address,
                    )
                )
                break
    return found


def lifecycle_index(functions: Iterable[FunctionDef]) -> dict[str, dict[str, int]]:
    """Map prefix -> {suffix: address} for the lifecycle functions."""
    index: dict[str, dict[str, int]] = defaultdict(dict)
    for entry in lifecycle_entries(functions):
        index[entry.subsystem][entry.suffix] = entry.address
    return dict(sorted(index.items()))


def to_records(functions: Iterable[FunctionDef]) -> list[dict[str, object]]:
    """Render functions as JSON-ready records, sorted by address."""
    records = [
        {
            "address": function.address,
            "size": function.size,
            "name": function.name,
            "signature": function.signature,
            "source_file": function.source_file,
            "subsystem": subsystem_of(function),
            "named_params": function.has_named_params,
        }
        for function in functions
    ]
    return sorted(records, key=lambda record: record["address"])  # type: ignore[arg-type,return-value]


def build_payload(functions: list[FunctionDef], *, donor: str) -> dict[str, object]:
    records = to_records(functions)
    engine = [r for r in records if r["subsystem"]]
    per_subsystem: dict[str, int] = defaultdict(int)
    for record in engine:
        per_subsystem[str(record["subsystem"])] += 1
    return {
        "donor": donor,
        "note": (
            "Donor is TimeSplitters 2; the target is TimeSplitters: Future Perfect. "
            "Names transfer as hypotheses, addresses and offsets do not."
        ),
        "function_count": len(records),
        "engine_function_count": len(engine),
        "named_param_count": sum(1 for r in records if r["named_params"]),
        "subsystems": dict(sorted(per_subsystem.items())),
        "lifecycle": lifecycle_index(functions),
        "lifecycle_link_order": lifecycle_link_order(functions),
        "functions": records,
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Emit the donor function table and lifecycle index as JSON.",
        epilog="Produce the input with: stdump functions SLUS_999.99 > functions.cpp",
    )
    parser.add_argument("--functions", type=Path, required=True, help="stdump functions output")
    parser.add_argument("--out", type=Path, required=True, help="JSON file to write")
    parser.add_argument(
        "--donor",
        default="TS2 US OPM Demo 53 (SLUS_999.99, 2001-10-05)",
        help="donor build description recorded in the output",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    text = args.functions.read_text(encoding="utf-8", errors="replace")
    functions = parse_functions(text.splitlines())
    payload = build_payload(functions, donor=args.donor)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=1), encoding="utf-8")

    lifecycle = payload["lifecycle"]
    assert isinstance(lifecycle, dict)
    print(
        f"{payload['function_count']} functions "
        f"({payload['engine_function_count']} engine, "
        f"{payload['named_param_count']} with named params) -> {args.out}"
    )
    print(f"{len(payload['subsystems'])} subsystems")  # type: ignore[arg-type]
    print(
        f"{len(lifecycle)} lifecycle prefixes, "
        f"{sum(len(v) for v in lifecycle.values())} lifecycle functions"
    )
    return 0


def lifecycle_link_order(functions: Iterable[FunctionDef]) -> dict[str, list[str]]:
    """Per suffix, the subsystem prefixes in address order — i.e. link order.

    MEASURED INVARIANT: these sequences are mutually consistent. Across every pair
    of suffix groups in the donor, the relative order of shared prefixes agrees
    100% of the time (1,485 pairs for Reset vs Restart, 1,128 for End vs Reset, and
    so on for all 15 pairs). The linker emits each translation unit's functions
    together and in one order, so every lifecycle group is the same subsystem
    sequence restricted to its own members.

    That is what makes this useful on TSFP. A dispatcher there calls its subsystems
    in sequence, so matching becomes aligning two ordered lists rather than
    identifying functions one at a time -- and because several suffix groups give
    independent views of the same order, any alignment can be cross-checked against
    the others instead of taken on trust.
    """
    per_suffix: dict[str, list[tuple[int, str]]] = defaultdict(list)
    for entry in lifecycle_entries(functions):
        per_suffix[entry.suffix].append((entry.address, entry.subsystem))
    return {
        suffix: [prefix for _, prefix in sorted(pairs)]
        for suffix, pairs in sorted(per_suffix.items())
    }


def order_concordance(left: Sequence[str], right: Sequence[str]) -> tuple[int, int]:
    """Count (agreeing, total) ordered pairs shared by two sequences.

    A concordance below 1.0 on donor data would mean the link-order assumption is
    wrong, so this is the check that keeps the alignment approach honest rather
    than merely plausible.
    """
    common = [item for item in left if item in set(right)]
    rank_left = {item: index for index, item in enumerate(left)}
    rank_right = {item: index for index, item in enumerate(right)}
    agree = total = 0
    for first, second in itertools.combinations(common, 2):
        total += 1
        if (rank_left[first] < rank_left[second]) == (rank_right[first] < rank_right[second]):
            agree += 1
    return agree, total


if __name__ == "__main__":
    raise SystemExit(main())
