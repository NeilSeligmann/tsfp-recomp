# SPDX-License-Identifier: GPL-3.0-or-later
"""Join a libsig match CSV to the current function table, classification and names (T1641).

Question answered: of the functions still unnamed today, how many are library code according to
libsig, and where does libsig disagree with the tracked game/library classification?

The join is read-only and local. Inputs are the shared function table (export, overrides,
additions), the `.XTLID`/FLIRT/override classification, the tracked name overlays and a libsig
match CSV (`tools.libsig.cli match`). The per-address detail goes to `generated/` or `tmp/`
(`tools/libsig/guard.py`), never to a tracked path: it carries match confidence and the lib
file names. Only counts are printed for the tracked report.

Evidence tiers for "this address is library code" (the symbol name is irrelevant to the region):

- `strong`: the matched body has at least 16 fixed (unrelocated) bytes, or the name is confirmed by
  the game's own `.XTLID` record. Every candidate name of the match is library code, so alias
  groups and ambiguous sets count.
- `short`: a unique or narrowed name on a body of under 16 fixed bytes (`weak`, or `resolved` on a
  short thunk). A trivial body can be written by anyone, so this is a candidate only.
- `propagated`: named only because a matched caller's REL32 targets it.
- `none`: no row.

Reclassification (`--apply`, T1641) turns two kinds of evidence into `function_classification.csv`
rows (region `library`) and moves the existing INFERRED names of those functions to
`library_names.csv` with a `lib_` prefix:

- basis `strong`: a game-classified `.text` function whose libsig tier is `strong` against a
  library the XBE's own library table lists (`LINKED_LIB_STEMS`). No symbol name is read into any
  tracked file (provenance: `docs/native-libraries.md` section 7), only the family, the confidence
  label and the fixed-byte count.
- basis `run`: a maximal run of at most `RUN_MAX` consecutive game-classified `.text` functions (at
  most `RUN_MAX_MATCHED` when each member has a libsig match in a linked library) that has a library
  function immediately before and after it, where "library" is the `strong` set plus the existing
  library region. Linker order keeps one library's objects together, so a function bracketed on both
  sides is part of the run (EH funclets, short thunks, `mov edi,edi` offset entries).

Call-graph evidence (`--calls`) scans every function body of the table for `E8`/`E9` rel32
targets that land on a table entry and reports, per function, callers and callees by region.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import Counter, defaultdict
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from tools.codediff.boundaries import Function, load_function_table
from tools.coverage import (
    DEFAULT_CLASS_OVERRIDES,
    DEFAULT_FUNCTION_ADDITIONS,
    DEFAULT_FUNCTION_OVERRIDES,
    DEFAULT_LIBRARY_NAME_OVERLAY,
    DEFAULT_NAME_OVERLAY,
    REGION_GAME,
    REGION_LIBRARY,
    Classified,
    apply_overlays,
    classify_functions,
    load_class_overrides,
    load_function_names,
    load_library_names,
    read_flirt_addresses,
    text_range,
)
from tools.libsig.guard import check_output_path
from tools.xbe.parser import parse_xbe

#: Default naming bands: the T1465 to T1471 task bands, then the CRT/XDK tail of `.text`.
DEFAULT_BAND_EDGES = (
    0x00012000,
    0x00080000,
    0x00100000,
    0x00180000,
    0x00200000,
    0x00280000,
    0x00300000,
    0x00370000,
)

STRONG_FIXED_BYTES = 16
#: Library files the XBE's library table names (XAPILIB, D3D8, XGRAPHC, DSOUND, XONLINES, XVOICE,
#: XMV, LIBC, LIBCMT; XBOXKRNL has no code). A match only against other files in the XDK (dmusic,
#: d3dx8, xacteng, uix, xperf...) is a compiler-idiom collision, not a statement about this title.
LINKED_LIB_STEMS = frozenset(
    {"xapilib", "d3d8", "xgraphics", "dsound", "xonlines", "xvoice", "xmv", "libc", "libcmt"}
)
#: Longest run of consecutive non-library `.text` functions, bracketed by library functions on both
#: sides, that is taken as part of the library run.
RUN_MAX = 2
#: Longest such run when every member also carries a libsig match (strong or short) in a linked
#: library: position plus a per-function match, as for the 12 XVOICE accessors of T1641.
RUN_MAX_MATCHED = 8
LIBRARY_NAME_PREFIX = "lib_"
GAME_NAME_PREFIX = "game_"
CONFIDENCE_ORDER = ("high", "resolved", "alias_group", "ambiguous", "weak", "propagated", "none")
TIERS = ("strong", "short", "propagated", "none")

DETAIL_COLUMNS = (
    "entry_va",
    "size_bytes",
    "section",
    "band",
    "region",
    "evidence",
    "named",
    "confidence",
    "tier",
    "reason",
    "fixed_bytes",
    "libs",
    "symbol",
    "alias_set",
    "callers_game",
    "callers_library",
    "callees_game",
    "callees_library",
    "kind",
)


@dataclass(frozen=True)
class MatchRow:
    confidence: str
    reason: str
    fixed_bytes: int
    libs: str
    symbol: str
    alias_set: str


def tier_of(row: MatchRow | None) -> str:
    """Evidence tier of one match row (see the module docstring)."""
    if row is None:
        return "none"
    if row.confidence == "propagated":
        return "propagated"
    if row.reason == "xtlid" or row.fixed_bytes >= STRONG_FIXED_BYTES:
        return "strong"
    return "short"


def band_label(va: int, section: str, edges: Sequence[int], text_hi: int) -> str:
    """`0xLO-0xHI` for a `.text` address, `section:<name>` outside `.text`."""
    if section != ".text":
        return f"section:{section or 'unmapped'}"
    bounds = [*edges, text_hi]
    for low, high in zip(bounds, bounds[1:], strict=False):
        if low <= va < high:
            return f"0x{low:08x}-0x{high:08x}"
    return f"0x{bounds[0]:08x}-below"


def read_matches(path: Path) -> dict[int, MatchRow]:
    """Match CSV rows by address. `fixed_bytes` is the last column (added by T1641)."""
    rows: dict[int, MatchRow] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if "fixed_bytes" not in (reader.fieldnames or ()):
            raise SystemExit(f"{path}: no fixed_bytes column, rerun `tools.libsig.cli match`")
        for raw in reader:
            rows[int(raw["address"], 16)] = MatchRow(
                raw["confidence"],
                raw["reason"],
                int(raw["fixed_bytes"] or 0),
                raw["lib"],
                raw["symbol"],
                raw["alias_set"],
            )
    return rows


def section_of(xbe_sections: Sequence[tuple[str, int, int]], va: int) -> str:
    for name, low, size in xbe_sections:
        if low <= va < low + size:
            return name
    return ""


def classify_current(
    root: Path, xbe_path: Path
) -> tuple[tuple[Classified, ...], list[tuple[str, int, int]], int, bytes, int]:
    """The current classification with both name overlays applied, as `tools.coverage` builds it.

    Returns the classified functions, the XBE sections as `(name, va, size)`, `.text` end, the XBE
    bytes and the image base for body scans.
    """
    generated = root / "generated" / "retail"
    data = xbe_path.read_bytes()
    xbe = parse_xbe(data)
    text_lo, text_hi = text_range(xbe)
    table = tuple(
        load_function_table(
            generated / "functions.csv",
            root / DEFAULT_FUNCTION_OVERRIDES,
            root / DEFAULT_FUNCTION_ADDITIONS,
        )
    )
    known = frozenset(function.entry_va for function in table)
    classified = classify_functions(
        table,
        text_lo=text_lo,
        text_hi=text_hi,
        xtlid_addresses=frozenset(entry.address for entry in xbe.xtlid),
        flirt_addresses=read_flirt_addresses(generated / "flirt_names.csv"),
        class_overrides=load_class_overrides(root / DEFAULT_CLASS_OVERRIDES, known),
    )
    game = load_function_names(root / DEFAULT_NAME_OVERLAY, known)
    library = load_library_names(root / DEFAULT_LIBRARY_NAME_OVERLAY, known)
    classified = apply_overlays(classified, game, library)
    sections = [(s.name, s.virtual_addr, s.virtual_size) for s in xbe.sections]
    return classified, sections, text_hi, data, xbe.base_address


def call_edges(
    functions: Iterable[Function], data: bytes, xbe_sections_raw: Sequence[tuple[int, int, int]]
) -> dict[int, set[int]]:
    """caller entry -> callee entries, from `E8`/`E9` rel32 inside each table body.

    Sound only as evidence: a data byte `E8` can fake an edge, but the target must also be a table
    entry, which makes a coincidence rare. `xbe_sections_raw` is `(va, raw_addr, raw_size)`.
    """
    entries = {function.entry_va for function in functions}
    edges: dict[int, set[int]] = defaultdict(set)

    def file_offset(va: int) -> int | None:
        for base, raw, size in xbe_sections_raw:
            if base <= va < base + size:
                return raw + va - base
        return None

    for function in functions:
        start = function.entry_va
        end = max(function.body_max_va, start)
        offset = file_offset(start)
        if offset is None:
            continue
        for index in range(0, end - start + 1 - 4):
            if data[offset + index] in (0xE8, 0xE9):
                rel = int.from_bytes(
                    data[offset + index + 1 : offset + index + 5], "little", signed=True
                )
                target = (start + index + 5 + rel) & 0xFFFFFFFF
                if target in entries and target != start:
                    edges[start].add(target)
    return edges


@dataclass
class Detail:
    item: Classified
    section: str
    band: str
    match: MatchRow | None
    callers_game: int = 0
    callers_library: int = 0
    callees_game: int = 0
    callees_library: int = 0
    kind: str = ""

    @property
    def tier(self) -> str:
        return tier_of(self.match)

    @property
    def confidence(self) -> str:
        return self.match.confidence if self.match else "none"

    @property
    def named(self) -> bool:
        return self.item.is_named


def kind_of(detail: Detail) -> str:
    """Relation between the tracked region and libsig, the disagreement taxonomy of T1641."""
    region = detail.item.region
    tier = detail.tier
    if tier == "none":
        return "no-libsig-row"
    if region == REGION_LIBRARY:
        return "agree-library" if tier in ("strong", "propagated") else "library-short-match"
    if tier == "strong":
        return "game-named-libsig-library" if detail.named else "game-unnamed-libsig-library"
    if tier == "propagated":
        return "game-propagated-only"
    return "game-short-match"


def build_details(
    classified: Sequence[Classified],
    sections: Sequence[tuple[str, int, int]],
    text_hi: int,
    matches: Mapping[int, MatchRow],
    edges: Mapping[int, set[int]] | None,
    band_edges: Sequence[int],
) -> list[Detail]:
    region_of = {item.function.entry_va: item.region for item in classified}
    callers: dict[int, set[int]] = defaultdict(set)
    if edges:
        for caller, callees in edges.items():
            for callee in callees:
                callers[callee].add(caller)
    details: list[Detail] = []
    for item in classified:
        va = item.function.entry_va
        detail = Detail(
            item,
            section_of(sections, va),
            "",
            matches.get(va),
        )
        detail.band = band_label(va, detail.section, band_edges, text_hi)
        if edges is not None:
            detail.callers_game = sum(region_of.get(c) == REGION_GAME for c in callers.get(va, ()))
            detail.callers_library = sum(
                region_of.get(c) == REGION_LIBRARY for c in callers.get(va, ())
            )
            detail.callees_game = sum(region_of.get(c) == REGION_GAME for c in edges.get(va, ()))
            detail.callees_library = sum(
                region_of.get(c) == REGION_LIBRARY for c in edges.get(va, ())
            )
        detail.kind = kind_of(detail)
        details.append(detail)
    return details


@dataclass
class Summary:
    totals: Counter[str] = field(default_factory=Counter)
    unnamed_by_band: dict[str, Counter[str]] = field(default_factory=lambda: defaultdict(Counter))
    kinds: Counter[str] = field(default_factory=Counter)
    unnamed_confidence: dict[str, Counter[str]] = field(
        default_factory=lambda: defaultdict(Counter)
    )


def summarise(details: Sequence[Detail]) -> dict[str, object]:
    """Counts only. Keys are stable so the tracked report can be regenerated."""
    totals: Counter[str] = Counter()
    kinds: Counter[str] = Counter()
    named_kinds: Counter[str] = Counter()
    unnamed_by_band: dict[str, Counter[str]] = defaultdict(Counter)
    unnamed_confidence: dict[str, Counter[str]] = defaultdict(Counter)
    for d in details:
        region = d.item.region
        totals[f"{region}_total"] += 1
        totals[f"{region}_{'named' if d.named else 'unnamed'}"] += 1
        kinds[f"{region}/{d.kind}"] += 1
        if d.named:
            named_kinds[f"{region}/{d.kind}"] += 1
            continue
        unnamed_by_band[d.band][f"{region}"] += 1
        unnamed_by_band[d.band][f"{region}:tier={d.tier}"] += 1
        unnamed_confidence[region][d.confidence] += 1
    return {
        "totals": dict(sorted(totals.items())),
        "kinds": dict(sorted(kinds.items())),
        "named_kinds": dict(sorted(named_kinds.items())),
        "unnamed_by_band": {k: dict(sorted(v.items())) for k, v in sorted(unnamed_by_band.items())},
        "unnamed_confidence": {
            k: dict(sorted(v.items())) for k, v in sorted(unnamed_confidence.items())
        },
    }


def write_detail(path: Path, details: Sequence[Detail]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(DETAIL_COLUMNS)
        for d in details:
            m = d.match
            writer.writerow(
                [
                    f"0x{d.item.function.entry_va:08x}",
                    d.item.function.size_bytes,
                    d.section,
                    d.band,
                    d.item.region,
                    d.item.evidence,
                    "named" if d.named else "unnamed",
                    d.confidence,
                    d.tier,
                    m.reason if m else "",
                    m.fixed_bytes if m else "",
                    m.libs if m else "",
                    m.symbol if m else "",
                    m.alias_set if m else "",
                    d.callers_game,
                    d.callers_library,
                    d.callees_game,
                    d.callees_library,
                    d.kind,
                ]
            )


def render_markdown(summary: Mapping[str, object]) -> str:
    """Counts as markdown tables for the tracked report."""
    lines: list[str] = []
    totals = summary["totals"]
    assert isinstance(totals, dict)
    lines.append("| population | total | named | unnamed |")
    lines.append("|---|---|---|---|")
    for region in (REGION_GAME, REGION_LIBRARY):
        lines.append(
            f"| {region} | {totals.get(region + '_total', 0)} | "
            f"{totals.get(region + '_named', 0)} | {totals.get(region + '_unnamed', 0)} |"
        )
    lines.append("")
    by_band = summary["unnamed_by_band"]
    assert isinstance(by_band, dict)
    lines.append(
        "| band | unnamed game | of which libsig strong | short | propagated | "
        "unnamed library | of which strong | short | propagated |"
    )
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for band, cells in by_band.items():
        row = [band, cells.get("game", 0)]
        for tier in ("strong", "short", "propagated"):
            row.append(cells.get(f"game:tier={tier}", 0))
        row.append(cells.get("library", 0))
        for tier in ("strong", "short", "propagated"):
            row.append(cells.get(f"library:tier={tier}", 0))
        lines.append("| " + " | ".join(str(c) for c in row) + " |")
    return "\n".join(lines)


# --------------------------------------------------------------------------- #
# Reclassification plan (T1641).
# --------------------------------------------------------------------------- #

BASIS_STRONG = "strong"
BASIS_RUN = "run"
DOC = "docs/t1641-libsig-rematch.md"


def linked_families(libs: str) -> list[str]:
    """Linked library stems in a `lib1.lib|lib2.lib` field (debug builds fold into release)."""
    found: set[str] = set()
    for lib in libs.split("|"):
        stem = lib.removesuffix(".lib")
        for candidate in (stem, stem.removesuffix("d")):
            if candidate in LINKED_LIB_STEMS:
                found.add(candidate)
    return sorted(found)


@dataclass(frozen=True)
class Reclass:
    entry_va: int
    basis: str
    reason: str


def plan_reclassification(
    details: Sequence[Detail], *, run_max: int = RUN_MAX, run_max_matched: int = RUN_MAX_MATCHED
) -> list[Reclass]:
    """Game-classified `.text` functions that are library code, with the evidence class.

    Pure: depends only on the details. The reason text carries no symbol name.
    """
    text = sorted(
        (d for d in details if d.section == ".text"), key=lambda d: d.item.function.entry_va
    )
    found: dict[int, Reclass] = {}
    for d in text:
        match = d.match
        if d.item.region != REGION_GAME or match is None or d.tier != "strong":
            continue
        families = linked_families(match.libs)
        if not families:
            continue
        found[d.item.function.entry_va] = Reclass(
            d.item.function.entry_va,
            BASIS_STRONG,
            f"T1641 libsig full-body match, {'/'.join(families)} "
            f"({match.fixed_bytes} fixed bytes, {match.confidence}) in the 5849 libs; {DOC}",
        )
    is_library = [
        d.item.region == REGION_LIBRARY or d.item.function.entry_va in found for d in text
    ]
    start = 0
    while start < len(text):
        if is_library[start]:
            start += 1
            continue
        end = start
        while end < len(text) and not is_library[end]:
            end += 1
        # text[start:end] is a maximal run of non-library functions.
        length = end - start
        matched = all(
            d.match is not None and d.tier in ("strong", "short") and linked_families(d.match.libs)
            for d in text[start:end]
        )
        bracketed = start > 0 and end < len(text)
        if bracketed and (length <= run_max or (matched and length <= run_max_matched)):
            how = "each with a libsig match in a linked library" if length > run_max else "position"
            for d in text[start:end]:
                va = d.item.function.entry_va
                found[va] = Reclass(
                    va,
                    BASIS_RUN,
                    f"T1641 library run: {length} function(s) between libsig-strong library "
                    f"functions with no other game function between ({how}); {DOC}",
                )
        start = end
    return sorted(found.values(), key=lambda r: r.entry_va)


def library_name_for(old_name: str) -> str:
    """`game_xxx` -> `lib_xxx`. A name without the game prefix keeps its text under `lib_`."""
    return LIBRARY_NAME_PREFIX + old_name.removeprefix(GAME_NAME_PREFIX)


def read_rows(path: Path) -> list[list[str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.reader(handle))


def merge_classification(path: Path, plan: Sequence[Reclass]) -> int:
    """Add library rows for the plan to the classification table, existing rows stay in place."""
    rows = read_rows(path)
    header, body = rows[0], rows[1:]
    present = {int(row[0], 16) for row in body if row}
    added = [
        [f"0x{item.entry_va:08x}", REGION_LIBRARY, item.reason]
        for item in plan
        if item.entry_va not in present
    ]
    # Existing rows keep their order (peers append to the tail, a re-sort would move them and
    # conflict on every merge); the new rows follow in address order.
    merged = [*body, *sorted(added, key=lambda row: int(row[0], 16))]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(merged)
    return len(added)


def move_names(
    names_path: Path,
    library_path: Path,
    plan: Sequence[Reclass],
    keep_game_row: frozenset[int],
) -> tuple[int, int, int]:
    """Give the planned functions library names.

    Each planned function with a row in the game overlay gets a `lib_` row in the library overlay
    (same confidence, evidence extended with the old name). The game row is removed unless the VA
    is in `keep_game_row` (a registered `src/game` replacement, which `seed_function_names` owns).
    The game overlay is edited line by line, so every other row keeps its exact bytes.
    Returns (library rows added, game rows removed, game rows kept).
    """
    planned = {item.entry_va: item for item in plan}
    raw = names_path.read_bytes().decode("utf-8")
    lines = raw.splitlines(keepends=True)
    taken_names = {row[1] for row in read_rows(library_path)[1:] if len(row) > 1}
    library_rows = read_rows(library_path)
    header, library_body = library_rows[0], library_rows[1:]
    present = {int(row[0], 16) for row in library_body if row}
    kept_lines: list[str] = [lines[0]]
    added: list[list[str]] = []
    removed = kept = 0
    for line in lines[1:]:
        row = next(csv.reader([line.rstrip("\r\n")]), [])
        if len(row) != 4:
            kept_lines.append(line)
            continue
        try:
            va = int(row[0], 16)
        except ValueError:
            kept_lines.append(line)
            continue
        item = planned.get(va)
        if item is None:
            kept_lines.append(line)
            continue
        new_name = library_name_for(row[1])
        if va not in present and new_name not in taken_names:
            taken_names.add(new_name)
            added.append(
                [
                    f"0x{va:08x}",
                    new_name,
                    row[2],
                    f"{row[3]}; T1641 reclassified library ({item.basis}), renamed from "
                    f"{row[1]}; {DOC}",
                ]
            )
        if va in keep_game_row:
            kept += 1
            kept_lines.append(line)
        else:
            removed += 1
    names_path.write_bytes("".join(kept_lines).encode("utf-8"))
    merged = [*library_body, *sorted(added, key=lambda row: int(row[0], 16))]
    with library_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(merged)
    return len(added), removed, kept


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", maxsplit=1)[0])
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--xbe", type=Path, default=Path("tmp/oxm-extract/retail/default.xbe"))
    parser.add_argument("--matches", type=Path, required=True, help="libsig match CSV")
    parser.add_argument(
        "--out", type=Path, help="per-address detail CSV (under tmp/ or generated/)"
    )
    parser.add_argument("--json", type=Path, help="counts as JSON (under tmp/ or generated/)")
    parser.add_argument("--calls", action="store_true", help="add call-graph counts per function")
    parser.add_argument(
        "--apply",
        action="store_true",
        help="write the reclassification into tools/data (classification and both name overlays)",
    )
    parser.add_argument("--allow-outside-ignored", action="store_true")
    args = parser.parse_args(argv)
    out = check_output_path(args.out, args.allow_outside_ignored) if args.out else None
    json_out = check_output_path(args.json, args.allow_outside_ignored) if args.json else None

    classified, sections, text_hi, data, _ = classify_current(args.root, args.xbe)
    matches = read_matches(args.matches)
    edges = None
    if args.calls:
        xbe = parse_xbe(data)
        raw = [(s.virtual_addr, s.raw_addr, s.raw_size) for s in xbe.sections if s.raw_size]
        edges = call_edges([item.function for item in classified], data, raw)
    details = build_details(classified, sections, text_hi, matches, edges, DEFAULT_BAND_EDGES)
    summary = summarise(details)
    plan = plan_reclassification(details)
    print(
        f"reclassification plan: {len(plan)} functions, "
        + ", ".join(
            f"{basis} {sum(1 for r in plan if r.basis == basis)}"
            for basis in (BASIS_STRONG, BASIS_RUN)
        )
    )
    if args.apply:
        from tools.coverage import read_game_source_tree

        tree = read_game_source_tree(args.root / "src" / "game")
        added = merge_classification(args.root / DEFAULT_CLASS_OVERRIDES, plan)
        library_rows, removed, kept = move_names(
            args.root / DEFAULT_NAME_OVERLAY,
            args.root / DEFAULT_LIBRARY_NAME_OVERLAY,
            plan,
            frozenset(tree.registrations),
        )
        print(
            f"applied: {added} classification rows, {library_rows} library names, "
            f"{removed} game rows removed, {kept} kept (registered replacements)"
        )
    if out:
        write_detail(out, details)
    if json_out:
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(render_markdown(summary))
    print(json.dumps(summary["kinds"], indent=1, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
