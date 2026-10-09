# SPDX-License-Identifier: GPL-3.0-or-later
"""Plain-text and JSON views of a `Report`. Addresses, counts and names only."""

from __future__ import annotations

from tools.initmap.compare import Comparison
from tools.initmap.crosscheck import Summary
from tools.initmap.image import Image
from tools.initmap.liftparse import FunctionIndex
from tools.initmap.report import (
    PHASE_BY_ADDRESS,
    PHASE_FRAME,
    PHASE_PRE_FRAME,
    PHASE_PRE_MAIN,
    ModuleRow,
    OrdinalRow,
    Report,
)
from tools.initmap.walk import (
    HIT_ADDRESS_TAKEN,
    HIT_DATA_IMPORT,
    HIT_HAZARD,
    HIT_KERNEL,
    HIT_MMIO,
    HIT_NO_BODY,
    HIT_UNRESOLVED,
    HIT_XDK,
)

PHASES = (PHASE_PRE_MAIN, PHASE_PRE_FRAME, PHASE_FRAME, PHASE_BY_ADDRESS)
MODULE_ORDER = ("D3D", "DSOUND", "XPP", "XNET", "XONLINE", "XGRPH", "XMV")


def _hex(value: int | None) -> str:
    return "-" if value is None else f"{value:#010x}"


def _convention(row: ModuleRow) -> str:
    if row.convention is None:
        return "refused"
    registers = f"+{row.register_args}reg" if row.register_args else ""
    return f"{row.convention}/{row.stack_args}{registers}"


def render_summary(report: Report, index: FunctionIndex, image: Image) -> list[str]:
    walk = report.walk
    boundaries = report.boundaries
    lines = [
        "== walk",
        f"functions with a lifted body        {len(index.spans)}",
        f"functions expanded by the walk      {len(walk.frames)}",
        f"always-executed functions           {len(report.always)}",
        f"main first expanded at tick         {boundaries.main}",
        f"frame loop first expanded at tick   {boundaries.frame_loop}",
        f"functions with no reachable exit    {len(walk.shapes_without_exit)}",
        f"functions with unclassified jumps   {len(walk.unparsed)}",
        f"kernel thunk slots                  {len(image.xbe.kernel_import_ordinals)}",
        "",
        "hits by kind and phase (a hit is one site met, not one distinct target)",
        "kind                  " + "".join(f"{phase:>13}" for phase in PHASES),
    ]
    for kind in (
        HIT_XDK,
        HIT_KERNEL,
        HIT_UNRESOLVED,
        HIT_NO_BODY,
        HIT_DATA_IMPORT,
        HIT_MMIO,
        HIT_HAZARD,
        HIT_ADDRESS_TAKEN,
    ):
        per_phase = report.phase_counts.get(kind, {})
        lines.append(f"{kind:<22}" + "".join(f"{per_phase.get(phase, 0):>13}" for phase in PHASES))
    return lines


def render_init_entries(report: Report) -> list[str]:
    lines = ["", "== init list (one row per call, in call order)"]
    for entry in report.init_entries:
        sections = ", ".join(f"{name} {len(addresses)}" for name, addresses in entry.xdk.items())
        first = ", ".join(
            f"{name} {len(addresses)}" for name, addresses in entry.first_reached.items()
        )
        lines.append(
            f"{entry.index:>2} {_hex(entry.target)} args {entry.argument_words} "
            f"funcs {entry.functions:>3} xdk[{sections or '-'}] first[{first or '-'}] "
            f"kernel {len(entry.kernel)} unimpl {entry.unimplemented} "
            f"data {entry.data_imports} unresolved {entry.unresolved} stubs {entry.no_body}"
        )
    return lines


def _chain(row: ModuleRow, depth: int) -> str:
    if depth <= 0:
        return ""
    tail = row.chain[-depth:]
    return " via " + ">".join(f"{address:x}" for address in tail)


def render_modules(report: Report, top: int, chain_depth: int = 0) -> list[str]:
    lines = ["", "== XDK backlog in first-reach order, per module"]
    for section in MODULE_ORDER:
        rows = report.modules.get(section, [])
        total = report.surface_totals.get(section, 0)
        reach = report.distinct_reached.get(section, {})
        lines.append(
            f"-- {section}: {len(rows)} of {total} addresses reached "
            + " ".join(f"{phase} {reach.get(phase, 0)}" for phase in PHASES)
        )
        shown = rows if top <= 0 else rows[:top]
        for position, row in enumerate(shown, start=1):
            lines.append(
                f"  {position:>2} {_hex(row.address)} {row.phase:<10} {row.certainty:<11} "
                f"{_convention(row):<16} {row.name or '-':<34} "
                f"from {_hex(row.first_site)} in {_hex(row.first_function)} "
                f"sites {row.sites}{_chain(row, chain_depth)}"
            )
    return lines


def _ordinal_line(position: int, row: OrdinalRow) -> str:
    state = "done" if row.implemented else "MISSING"
    categories = ",".join(row.categories) or "-"
    return (
        f"  {position:>3} {row.ordinal:>3} {row.name:<34} {state:<8} {row.phase:<10} "
        f"{row.certainty:<11} from {_hex(row.first_site)} in {_hex(row.first_function)} "
        f"sites {row.sites} {categories}"
    )


def render_ordinals(report: Report, implemented: frozenset[int], image: Image) -> list[str]:
    imported = set(image.xbe.kernel_import_ordinals)
    reached = {row.ordinal for row in report.ordinals}
    missing = sorted(imported - implemented)
    lines = [
        "",
        "== kernel ordinals by first reach",
        f"imported {len(imported)}, implemented-and-imported {len(imported & implemented)}, "
        f"imported-and-missing {len(missing)}, implemented-but-not-imported "
        f"{sorted(implemented - imported)}",
        f"reached by a call {len(reached)}, of the missing ones {len(reached & set(missing))}",
        "-- missing ordinals only, in first-reach order",
    ]
    position = 0
    for row in report.ordinals:
        if not row.implemented:
            position += 1
            lines.append(_ordinal_line(position, row))
    lines.append("-- imported, missing, never called by the walk")
    never = [ordinal for ordinal in missing if ordinal not in reached]
    lines.append("  " + " ".join(str(ordinal) for ordinal in never))
    return lines


def render_risks(report: Report) -> list[str]:
    lines = ["", "== risks"]
    for category, rows in sorted(report.risks.items()):
        lines.append(f"-- kernel calls: {category}")
        for row in rows:
            lines.append(
                f"  {row.ordinal:>3} {row.name:<34} {'done' if row.implemented else 'MISSING':<8} "
                f"{row.phase:<10} {row.certainty:<11} from {_hex(row.first_site)}"
            )
    boundaries = report.boundaries
    for category, entries in sorted(report.risk_hits.items()):
        lines.append(f"-- {category}")
        seen: set[tuple[str, int]] = set()
        for hit, label in entries:
            key = (label, hit.function)
            if key in seen:
                continue
            seen.add(key)
            lines.append(
                f"  {boundaries.phase(hit):<10} {label:<34} in {_hex(hit.function)} "
                f"{'conditional' if hit.conditional else 'always'}"
            )
    return lines


def render_comparison(label: str, result: Comparison) -> list[str]:
    return [
        f"-- {label}",
        f"runtime calls {result.runtime_calls}, distinct sites {result.runtime_sites}, "
        f"found by the walk {result.found_sites} ({result.coverage:.1%})",
        f"pairwise order agreement {result.concordant_pairs} of {result.pairs} "
        f"({result.concordance:.1%}), null model for a random order 50.0%",
        f"baseline, the same sites sorted by guest address {result.address_order_concordant_pairs} "
        f"of {result.pairs} ({result.address_order_concordance:.1%})",
        f"agreement among sites labelled always {result.certain_concordant_pairs} of "
        f"{result.certain_pairs} ({result.certain_concordance:.1%})",
        f"matched only through a tail-jump wrapper {result.matched_through_wrapper}",
        f"missed sites {len(result.missed)}",
        *(
            f"  missed #{sequence} {kind} {value:#x} from {return_va:#010x}"
            for sequence, kind, value, return_va in result.missed[:40]
        ),
    ]


def render_text(
    report: Report,
    index: FunctionIndex,
    image: Image,
    implemented: frozenset[int],
    top: int,
    comparisons: list[tuple[str, Comparison]] | None = None,
    chain_depth: int = 0,
) -> str:
    lines = render_summary(report, index, image)
    lines += render_init_entries(report)
    lines += render_modules(report, top, chain_depth)
    lines += render_ordinals(report, implemented, image)
    lines += render_risks(report)
    if comparisons:
        lines += ["", "== validation against the host's recorded trace"]
        for label, result in comparisons:
            lines += render_comparison(label, result)
    return "\n".join(lines) + "\n"


def to_json(report: Report) -> dict[str, object]:
    """The report as plain data, for a diff or a test to read."""
    return {
        "boundaries": {
            "main_tick": report.boundaries.main,
            "frame_loop_tick": report.boundaries.frame_loop,
        },
        "init_entries": [
            {
                "index": entry.index,
                "target": f"{entry.target:#010x}",
                "argument_words": entry.argument_words,
                "functions": entry.functions,
                "xdk": {
                    name: [f"{address:#010x}" for address in addresses]
                    for name, addresses in entry.xdk.items()
                },
                "first_reached": {
                    name: [f"{address:#010x}" for address in addresses]
                    for name, addresses in entry.first_reached.items()
                },
                "kernel": entry.kernel,
                "unimplemented": entry.unimplemented,
                "data_imports": entry.data_imports,
                "unresolved": entry.unresolved,
                "stubs": entry.no_body,
            }
            for entry in report.init_entries
        ],
        "modules": {
            section: [
                {
                    "address": f"{row.address:#010x}",
                    "name": row.name,
                    "convention": row.convention,
                    "stack_args": row.stack_args,
                    "register_args": row.register_args,
                    "phase": row.phase,
                    "certainty": row.certainty,
                    "first_site": row.first_site,
                    "sites": row.sites,
                }
                for row in rows
            ]
            for section, rows in report.modules.items()
        },
        "ordinals": [
            {
                "ordinal": row.ordinal,
                "name": row.name,
                "implemented": row.implemented,
                "phase": row.phase,
                "certainty": row.certainty,
                "first_site": row.first_site,
                "sites": row.sites,
                "categories": list(row.categories),
            }
            for row in report.ordinals
        ],
    }


def render_crosscheck(summary: Summary, limit: int = 12) -> list[str]:
    """The lifted C's call edges against a fresh decode of the image's bytes."""
    lines = [
        "",
        "== call edges against an independent decode of the image",
        f"functions checked {summary.checked}, call lists identical {summary.agree}, "
        f"sweeps that did not account for every byte {summary.incomplete}, "
        f"functions with a byte the decoder could not read {summary.with_skipped_bytes}",
        f"functions with at least one call in the bytes {summary.with_calls}, of which identical "
        f"{summary.with_calls_agree}",
        f"instruction count equals the lifter's header {summary.count_matches} of "
        f"{summary.count_checked}",
        f"calls by the lifted C {summary.calls_lifted}, by the byte decode {summary.calls_swept}",
        f"functions that disagree {len(summary.disagreeing)}",
    ]
    for verdict in summary.disagreeing[:limit]:
        lines.append(
            f"  {verdict.entry:#010x} lifted {sum(verdict.lifted_calls.values())}+"
            f"{verdict.lifted_indirect} indirect, bytes {sum(verdict.swept.calls.values())}+"
            f"{verdict.swept.indirect_calls} indirect, skipped bytes {verdict.swept.skipped_bytes}"
        )
    return lines
