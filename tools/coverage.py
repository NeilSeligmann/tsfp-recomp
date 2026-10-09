# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure decompilation progress, and refuse to report naming as one number.

WHY THERE IS NO SINGLE "FUNCTIONS NAMED" PERCENTAGE HERE. The retail function
table carries 658 non-placeholder names against 11,685 `FUN_*`/`thunk_FUN_*`
placeholders, which averages to "5.3% named". That number is worthless, because
every one of the 658 is XDK or CRT library code and none of it is the
decompilation target:

  * 657 of the 658 sit above 0x370000, in the statically linked library tail.
  * The one exception, `_atol` at 0x0006a580, is a 5-byte import thunk.
  * The 363 FLIRT names are XDK-pattern matches, so they are library by
    construction.
  * Of 9,713 functions whose entry is inside `.text`, 465 carry library evidence
    and 9,248 do not. **Zero of those 9,248 are named.**

So the honest statement is that game-code naming is 0.00%, and an aggregate badge
would bury that under library names we did not write and never have to. Every
naming figure this module produces is split library vs game, and `NamingSplit`
has no combined accessor on purpose.

HOW THE LIBRARY/GAME BOUNDARY IS DERIVED. Two measurements, because a single
address cutoff does not exist:

1. **Section level, and clean.** In retail, `.text` is [0x00012000, 0x003d337c)
   and *every* linked XDK section -- D3D, XGRPH, DSOUND, XONLINE, XNET, XMV, XPP,
   DOLBY -- starts above it, the lowest at 0x003d3380. So the 2,630 functions
   whose entry falls outside `.text` are library with no ambiguity, by section
   membership alone. That is where the 12,343 total splits 9,713 / 2,630.

2. **Inside `.text` there is NO clean cutoff, so classification is per-function.**
   `.XTLID`-known addresses inside `.text` are tightly clustered in
   [0x0037c97b, 0x003d187e] -- 96 of them, all in the top 3.5% of the section --
   which is what tempts a cutoff near 0x370000. But FLIRT matches library code as
   low as 0x00019d40, and `_atol` sits at 0x0006a580, so a cutoff would
   misclassify CRT helpers and import thunks scattered right through the game
   region. A function inside `.text` is therefore classified library if it has
   *any* of three independent kinds of evidence, and game otherwise:

     `.XTLID`-known   95 entries   exact linker-emitted library ids
     FLIRT-matched   363 entries   XDK 5849 pattern match
     already named    82 entries   not covered by either of the above

   The third source is not redundant and omitting it gets the answer wrong. The
   82 are overwhelmingly 5-6 byte import thunks Ghidra named from the import
   table (`_atol`, `XcSHAUpdate`, `XcSHAInit`, `XcSHAFinal`, `XcHMAC`, `DbgPrint`,
   `ExQueryNonVolatileSetting`, `GetTickCount`) plus CRT bodies (`_fclose`,
   `_strstr`, `_strncpy`), none of which FLIRT or `.XTLID` flagged. Counting them
   as game code would report 82 named game functions, every one of them false.

   Union: 465 library-evidenced, leaving 9,248 game functions.

   The rule generalises as "a non-placeholder name in this binary is a library
   name", which is a claim about this binary rather than about decompilation in
   general. It is checked two ways -- the 657/658 address concentration, and
   inspection of the remainder -- and it will stop holding the moment the project
   names its first game function, which is exactly the event the game-code badge
   exists to show.

WHY `.text` COVERAGE MUST BE FILTERED TO `.text`. The function CSV covers the
whole image, so a coverage figure computed over all 12,343 rows counts library
bytes against a `.text` denominator. An earlier attempt did that and produced
more bytes-in-gaps than there was unclaimed space, which is impossible on its
face and is the signature of this specific mistake. `text_coverage` clips every
span to the half-open `.text` range before merging.

AND WHY IT USES THE BODY EXTENT, NOT `entry + size_bytes`. `size_bytes` is an
address *count*, and 963 retail functions have non-contiguous bodies, so
`[entry, entry + size_bytes)` ends in the wrong place for every one of them. Using
the extent `[entry, body_max_va + 1)` reproduces the committed measurement exactly
-- 81.60% claimed, 724,474 B unclaimed, 1,248 unclaimed regions of >=32 bytes --
where `entry + size_bytes` gives 81.36% / 733,774 B / 1,290 and counts the
interior gaps of fragmented functions as missing code. The extent is also the
conservative direction for a backlog: it claims more, so it proposes fewer
missing functions.

STALENESS IS A FIRST-CLASS OUTPUT. A badge that silently goes stale is worse than
no badge, so a metric whose artifacts are absent renders as `unknown` in grey and
never carries its last value forward. `metrics_document` records, per metric, the
date measured, the commit measured at, and which artifacts were present.

NO TIMESTAMP GOES INSIDE AN SVG. `render_badge` is a pure function of (label,
value, colour), so regenerating without a real change produces byte-identical
files and no diff churn. The timestamp belongs in `metrics.json` alone.
"""

from __future__ import annotations

import csv
import dataclasses
import hashlib
import json
import re
import subprocess
import sys
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from dataclasses import replace as with_fields
from datetime import date
from pathlib import Path
from typing import TypeVar
from xml.sax.saxutils import escape, quoteattr

from tools.codediff.boundaries import (
    Function,
    count_call_sites,
    load_function_table,
    load_functions,
    preceded_by_terminator,
)
from tools.ghidra.quality import QualityReport, score_export
from tools.replace.input_abi import input_contract, register_inputs, validate_inputs
from tools.replace.proof_contract import (
    FP_SCALAR_MATRIX_FILE,
    fp_scalar_closure,
    fp_scalar_gate_ok,
    fp_scalar_record,
    is_fp_scalar_contract,
    validate_document,
)
from tools.xbe.model import Xbe
from tools.xbe.parser import parse_xbe

# --------------------------------------------------------------------------- #
# Measured constants, each with its provenance.
# --------------------------------------------------------------------------- #

#: Whole-binary function total predicted by the PS2 build's `JR $ra` census.
#: context.md 6e. WHOLE-BINARY on both sides: this counts PS2's linked Sony SDK
#: just as 12,343 counts Xbox's linked XDK, so the comparison is only valid at
#: whole-binary level and is never made against the 9,713 `.text` subset.
PREDICTED_FUNCTION_TOTAL = 13461

#: Name of the section holding Free Radical's own code plus the CRT/XAPI tail.
TEXT_SECTION_NAME = ".text"

#: Smallest unclaimed `.text` region worth reporting as a probably-missing
#: function. 32 bytes is the floor the committed 1,248 figure was measured at.
DEFAULT_MIN_REGION_BYTES = 32

# --------------------------------------------------------------------------- #
# Classification vocabulary.
# --------------------------------------------------------------------------- #

REGION_GAME = "game"
REGION_LIBRARY = "library"

#: Evidence that made a function library code, strongest first. The order is the
#: precedence `classify_functions` applies, so a function outside `.text` reports
#: `xdk-section` even if it is also `.XTLID`-known.
EVIDENCE_SECTION = "xdk-section"
EVIDENCE_XTLID = "xtlid"
EVIDENCE_FLIRT = "flirt"
EVIDENCE_NAMED = "named"
EVIDENCE_NONE = "none"
#: A tracked per-function decision in `tools/data/function_classification.csv` (T1565).
EVIDENCE_OVERRIDE = "override"

#: Ghidra's placeholder function names. `thunk_FUN_*` is a placeholder too: it
#: names the *shape* (a thunk) and not the function, so treating it as named would
#: credit 86 functions nobody has identified.
#:
#: `CODEDIFF_<hex>` belongs here for the same reason, and its absence was
#: over-reporting. `tools/ghidra/CreateFunctionsAt.java` deliberately gives that prefix
#: to the cross-build candidate functions so they are identifiable as MACHINE-PROPOSED
#: rather than confirmed -- and the metric was then counting all 70 of them as
#: recovered names, inflating library naming from 26.1% to 28.3%. A name that exists
#: to say "nobody has identified this yet" must not be read as an identification.
PLACEHOLDER_NAME = re.compile(r"^(?:thunk_)?(?:FUN|SUB|LAB|CODEDIFF)_[0-9a-fA-F]+$")


def is_placeholder_name(name: str) -> bool:
    """Whether `name` is a Ghidra placeholder rather than a recovered name."""
    return PLACEHOLDER_NAME.match(name) is not None


@dataclass(frozen=True)
class Classified:
    """One function, with its region and the evidence that put it there."""

    function: Function
    region: str
    evidence: str

    @property
    def is_named(self) -> bool:
        return not is_placeholder_name(self.function.name)


def classify_functions(
    functions: Iterable[Function],
    *,
    text_lo: int,
    text_hi: int,
    xtlid_addresses: frozenset[int],
    flirt_addresses: frozenset[int],
    class_overrides: Mapping[int, str] | None = None,
) -> tuple[Classified, ...]:
    """Split a function table into game code and library code, per function.

    `text_lo`/`text_hi` are the half-open `.text` virtual address range. An entry
    outside it is library by section membership; an entry inside it is library
    when `.XTLID` knows it, FLIRT matched it, or it already carries a
    non-placeholder name. See the module docstring for why all three are needed
    and why no address cutoff is used.

    `class_overrides` maps an entry VA inside `.text` to a decided region (T1565): the
    tracked table wins over the three heuristics, because it records a per-function body
    review. An override on an entry outside `.text` is an error, section membership is
    hard evidence.
    """
    overrides = class_overrides or {}
    result: list[Classified] = []
    for function in functions:
        entry = function.entry_va
        if not (text_lo <= entry < text_hi):
            if entry in overrides:
                raise ValueError(f"classification override 0x{entry:08x} is outside .text")
            result.append(Classified(function, REGION_LIBRARY, EVIDENCE_SECTION))
        elif entry in overrides:
            result.append(Classified(function, overrides[entry], EVIDENCE_OVERRIDE))
        elif entry in xtlid_addresses:
            result.append(Classified(function, REGION_LIBRARY, EVIDENCE_XTLID))
        elif entry in flirt_addresses:
            result.append(Classified(function, REGION_LIBRARY, EVIDENCE_FLIRT))
        elif not is_placeholder_name(function.name):
            result.append(Classified(function, REGION_LIBRARY, EVIDENCE_NAMED))
        else:
            result.append(Classified(function, REGION_GAME, EVIDENCE_NONE))
    return tuple(result)


# --------------------------------------------------------------------------- #
# Classification overrides (T1565).
# --------------------------------------------------------------------------- #
#
# `tools/data/function_classification.csv` (entry_va,region,reason) records per-function
# decisions that beat the `.XTLID`/FLIRT/named heuristics: a FLIRT false positive that is
# really game code (region game) or CRT code the heuristics miss (region library). Every
# row needs a reason. A malformed row, a duplicate or a stale VA raises: a silently
# dropped decision would move a denominator back without anyone noticing.

DEFAULT_CLASS_OVERRIDES = Path("tools/data/function_classification.csv")
CLASS_OVERRIDE_COLUMNS = ("entry_va", "region", "reason")


def load_class_overrides(path: Path, known_vas: frozenset[int]) -> dict[int, str]:
    """Read the classification overrides as entry VA -> region. Bad rows raise."""
    reader = csv.reader(path.read_bytes().decode("utf-8").splitlines())
    header = next(reader, None)
    if header is None or tuple(header) != CLASS_OVERRIDE_COLUMNS:
        raise ValueError(
            f"{path}: header must be {','.join(CLASS_OVERRIDE_COLUMNS)}, got {header!r}"
        )
    rows: dict[int, str] = {}
    for number, row in enumerate(reader, start=2):
        if not row:
            continue
        where = f"{path.name}:{number}"
        if len(row) != len(CLASS_OVERRIDE_COLUMNS):
            raise ValueError(f"{where}: expected 3 columns, got {len(row)}")
        va_text, region, reason = (cell.strip() for cell in row)
        try:
            va = int(va_text, 16)
        except ValueError:
            raise ValueError(f"{where}: entry_va {va_text!r} is not hex") from None
        if region not in (REGION_GAME, REGION_LIBRARY):
            raise ValueError(f"{where}: region {region!r} is not game or library")
        if not reason:
            raise ValueError(f"{where}: {va_text} has no reason")
        if va not in known_vas:
            raise ValueError(f"{where}: {va_text} is not a function in the table (stale VA)")
        if va in rows:
            raise ValueError(f"{where}: {va_text} is listed twice")
        rows[va] = region
    return rows


def class_overrides_for_root(root: Path, functions: Iterable[Function] | None) -> dict[int, str]:
    path = root / DEFAULT_CLASS_OVERRIDES
    if functions is None or not path.is_file():
        return {}
    return load_class_overrides(path, frozenset(function.entry_va for function in functions))


@dataclass(frozen=True)
class NamingSplit:
    """Named-function counts, split library vs game.

    Deliberately has no combined `named_percent`: see the module docstring. The
    two populations are different work and averaging them is the dishonest
    reading this whole module exists to prevent.
    """

    game_total: int
    game_named: int
    library_total: int
    library_named: int
    library_in_text: int
    library_outside_text: int

    @property
    def game_percent(self) -> float:
        return 100.0 * self.game_named / self.game_total if self.game_total else 0.0

    @property
    def library_percent(self) -> float:
        return 100.0 * self.library_named / self.library_total if self.library_total else 0.0

    @property
    def game_unnamed(self) -> int:
        return self.game_total - self.game_named

    @property
    def library_unnamed(self) -> int:
        return self.library_total - self.library_named


def naming_split(classified: Iterable[Classified]) -> NamingSplit:
    """Count named and unnamed functions in each region."""
    game_total = game_named = 0
    library_total = library_named = 0
    in_text = outside_text = 0
    for item in classified:
        if item.region == REGION_GAME:
            game_total += 1
            game_named += item.is_named
        else:
            library_total += 1
            library_named += item.is_named
            if item.evidence == EVIDENCE_SECTION:
                outside_text += 1
            else:
                in_text += 1
    return NamingSplit(
        game_total=game_total,
        game_named=game_named,
        library_total=library_total,
        library_named=library_named,
        library_in_text=in_text,
        library_outside_text=outside_text,
    )


# --------------------------------------------------------------------------- #
# Function-name overlay (T1260).
# --------------------------------------------------------------------------- #
#
# `generated/retail/functions.csv` is a Ghidra export and carries only placeholders
# for game code. Names chosen in `src/game` replacements live in the tracked table
# `tools/data/function_names.csv` (entry_va,name,confidence,evidence). It is applied
# AFTER `classify_functions`, never before: the classifier treats any
# non-placeholder name as library evidence (EVIDENCE_NAMED), so overlaying first
# would move every newly named game function into the library population. Only
# rows whose VA is region game raise the game-named count; a library VA in the
# overlay is ignored and reported, so library naming stays a separate number.
# Every rejected row is reported loudly (CLI stderr, metrics.json, non-zero exit).

DEFAULT_NAME_OVERLAY = Path("tools/data/function_names.csv")
DEFAULT_FUNCTION_OVERRIDES = Path("tools/data/function_overrides.csv")
DEFAULT_FUNCTION_ADDITIONS = Path("tools/data/function_additions.csv")
NAME_OVERLAY_COLUMNS = ("entry_va", "name", "confidence", "evidence")
NAME_CONFIDENCES = ("MEASURED", "xemu-level", "INFERRED", "FABRICATED")
_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


@dataclass(frozen=True)
class NameRow:
    entry_va: int
    name: str
    confidence: str
    evidence: str


@dataclass(frozen=True)
class NameOverlay:
    """Accepted rows plus every rejection, with the file's identity for metrics.json."""

    path: str
    sha256: str
    row_count: int
    rows: Mapping[int, NameRow]
    rejections: tuple[str, ...]


def load_function_names(
    path: Path, known_vas: frozenset[int], *, label: str | None = None
) -> NameOverlay:
    """Read the overlay. Bad rows are rejected with a reason, never silently skipped."""
    raw = path.read_bytes()
    sha = hashlib.sha256(raw).hexdigest()
    rejections: list[str] = []
    parsed: list[tuple[int, NameRow]] = []
    reader = csv.reader(raw.decode("utf-8").splitlines())
    header = next(reader, None)
    if header is None or tuple(header) != NAME_OVERLAY_COLUMNS:
        raise ValueError(f"{path}: header must be {','.join(NAME_OVERLAY_COLUMNS)}, got {header!r}")
    row_count = 0
    for number, row in enumerate(reader, start=2):
        if not row:
            continue
        row_count += 1
        where = f"{path.name}:{number}"
        if len(row) != len(NAME_OVERLAY_COLUMNS):
            rejections.append(f"{where}: expected 4 columns, got {len(row)}")
            continue
        va_text, name, confidence, evidence = (cell.strip() for cell in row)
        try:
            va = int(va_text, 16)
        except ValueError:
            rejections.append(f"{where}: entry_va {va_text!r} is not hex")
            continue
        if not name:
            rejections.append(f"{where}: {va_text} has an empty name")
        elif is_placeholder_name(name):
            rejections.append(f"{where}: {va_text} name {name!r} is placeholder-shaped")
        elif _IDENTIFIER.match(name) is None:
            rejections.append(f"{where}: {va_text} name {name!r} is not a C identifier")
        elif confidence not in NAME_CONFIDENCES:
            rejections.append(
                f"{where}: {va_text} confidence {confidence!r} is not one of "
                f"{', '.join(NAME_CONFIDENCES)}"
            )
        elif not evidence:
            rejections.append(f"{where}: {va_text} {name} has no evidence")
        elif va not in known_vas:
            rejections.append(f"{where}: {va_text} is not a function in functions.csv (stale VA)")
        else:
            parsed.append((number, NameRow(va, name, confidence, evidence)))
    by_va: dict[int, list[int]] = {}
    by_name: dict[str, list[int]] = {}
    for number, row in parsed:
        by_va.setdefault(row.entry_va, []).append(number)
        by_name.setdefault(row.name, []).append(number)
    dropped: set[int] = set()
    for va, numbers in by_va.items():
        if len(numbers) > 1:
            rejections.append(
                f"{path.name}: duplicate VA 0x{va:08x} on lines {numbers}, all rejected"
            )
            dropped.update(numbers)
    for name, numbers in by_name.items():
        if len(numbers) > 1:
            rejections.append(
                f"{path.name}: duplicate name {name!r} on lines {numbers}, all rejected"
            )
            dropped.update(numbers)
    rows = {row.entry_va: row for number, row in parsed if number not in dropped}
    return NameOverlay(label or path.as_posix(), sha, row_count, rows, tuple(rejections))


@dataclass(frozen=True)
class OverlayApplication:
    classified: tuple[Classified, ...]
    applied: int
    ignored_library: tuple[int, ...]
    #: Overlay names equal to an existing non-placeholder name of a different function.
    collisions: tuple[str, ...]


def apply_name_overlay(
    classified: Iterable[Classified], overlay: NameOverlay
) -> OverlayApplication:
    """Rename GAME functions from the overlay. Library VAs are ignored and reported."""
    items = tuple(classified)
    existing = {
        item.function.name: item.function.entry_va
        for item in items
        if not is_placeholder_name(item.function.name)
    }
    result: list[Classified] = []
    applied = 0
    ignored: list[int] = []
    collisions: list[str] = []
    for item in items:
        row = overlay.rows.get(item.function.entry_va)
        if row is None:
            result.append(item)
        elif item.region != REGION_GAME:
            ignored.append(row.entry_va)
            result.append(item)
        elif existing.get(row.name, row.entry_va) != row.entry_va:
            collisions.append(f"0x{row.entry_va:08x} {row.name} collides with existing name")
            result.append(item)
        else:
            applied += 1
            result.append(with_fields(item, function=with_fields(item.function, name=row.name)))
    return OverlayApplication(tuple(result), applied, tuple(ignored), tuple(collisions))


# --------------------------------------------------------------------------- #
# Library-name overlay (T1506).
# --------------------------------------------------------------------------- #
#
# `function_names.csv` is the GAME overlay and ignores library rows by design. Library
# names live in a separate tracked table, `tools/data/library_names.csv`, with the same
# four columns and row rules. It is applied after classification, only to rows whose VA is
# classified library and still carries a placeholder name, and it raises naming-library
# only. A game-region VA in this file is ignored and reported, so it can never raise the
# game figure, and the library denominator never changes (classification precedes it).
# Library names must not carry the `game_` prefix, which marks game-code names.

DEFAULT_LIBRARY_NAME_OVERLAY = Path("tools/data/library_names.csv")
GAME_NAME_PREFIX = "game_"


def load_library_names(
    path: Path, known_vas: frozenset[int], *, label: str | None = None
) -> NameOverlay:
    """Read the library overlay: the game overlay's row rules plus no `game_` prefix."""
    overlay = load_function_names(path, known_vas, label=label)
    prefixed = {
        va: row for va, row in overlay.rows.items() if row.name.startswith(GAME_NAME_PREFIX)
    }
    if not prefixed:
        return overlay
    reasons = tuple(
        f"{path.name}: 0x{va:08x} name {row.name!r} has the game_ prefix, library names must not"
        for va, row in sorted(prefixed.items())
    )
    rows = {va: row for va, row in overlay.rows.items() if va not in prefixed}
    return NameOverlay(
        overlay.path, overlay.sha256, overlay.row_count, rows, (*overlay.rejections, *reasons)
    )


@dataclass(frozen=True)
class LibraryOverlayApplication:
    classified: tuple[Classified, ...]
    applied: int
    #: Rows whose VA is game code: ignored, never counted.
    ignored_game: tuple[int, ...]
    #: Rows for a library function that already has a non-placeholder name.
    ignored_named: tuple[int, ...]
    collisions: tuple[str, ...]


def apply_library_name_overlay(
    classified: Iterable[Classified], overlay: NameOverlay
) -> LibraryOverlayApplication:
    """Name still-unnamed LIBRARY functions from the library overlay."""
    items = tuple(classified)
    existing = {
        item.function.name: item.function.entry_va
        for item in items
        if not is_placeholder_name(item.function.name)
    }
    result: list[Classified] = []
    applied = 0
    ignored_game: list[int] = []
    ignored_named: list[int] = []
    collisions: list[str] = []
    for item in items:
        row = overlay.rows.get(item.function.entry_va)
        if row is None:
            result.append(item)
        elif item.region != REGION_LIBRARY:
            ignored_game.append(row.entry_va)
            result.append(item)
        elif item.is_named:
            ignored_named.append(row.entry_va)
            result.append(item)
        elif existing.get(row.name, row.entry_va) != row.entry_va:
            collisions.append(f"0x{row.entry_va:08x} {row.name} collides with existing name")
            result.append(item)
        else:
            applied += 1
            result.append(with_fields(item, function=with_fields(item.function, name=row.name)))
    return LibraryOverlayApplication(
        tuple(result), applied, tuple(ignored_game), tuple(ignored_named), tuple(collisions)
    )


def apply_overlays(
    classified: Iterable[Classified],
    name_overlay: NameOverlay | None,
    library_overlay: NameOverlay | None,
) -> tuple[Classified, ...]:
    """Game overlay first, then the library overlay. Each touches only its own region."""
    result = tuple(classified)
    if name_overlay is not None:
        result = apply_name_overlay(result, name_overlay).classified
    if library_overlay is not None:
        result = apply_library_name_overlay(result, library_overlay).classified
    return result


# --------------------------------------------------------------------------- #
# `.text` coverage.
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class Gap:
    """An unclaimed run of `.text`, i.e. bytes no recovered function covers."""

    start_va: int
    length: int

    @property
    def end_va(self) -> int:
        """One past the last unclaimed byte."""
        return self.start_va + self.length


@dataclass(frozen=True)
class TextCoverage:
    """How much of `.text` sits inside a recovered function."""

    total_bytes: int
    claimed_bytes: int
    gaps: tuple[Gap, ...]

    @property
    def unclaimed_bytes(self) -> int:
        return self.total_bytes - self.claimed_bytes

    @property
    def claimed_percent(self) -> float:
        return 100.0 * self.claimed_bytes / self.total_bytes if self.total_bytes else 0.0

    def gaps_at_least(self, floor: int) -> tuple[Gap, ...]:
        """Gaps of at least `floor` bytes, in address order."""
        return tuple(gap for gap in self.gaps if gap.length >= floor)


def text_coverage(functions: Iterable[Function], *, text_lo: int, text_hi: int) -> TextCoverage:
    """Merge function body extents clipped to `.text` and report the gaps.

    Each function contributes `[entry_va, body_max_va + 1)` clipped to
    `[text_lo, text_hi)`, so library functions living above `.text` contribute
    nothing and cannot inflate the numerator. Overlapping extents are merged, so
    the claimed total is a byte count and not a sum of sizes.

    `claimed + sum(gap lengths) == total` holds by construction, which is the
    invariant the earlier unfiltered version violated.
    """
    spans: list[tuple[int, int]] = []
    for function in functions:
        start = max(function.entry_va, text_lo)
        end = min(function.body_max_va + 1, text_hi)
        if start < end:
            spans.append((start, end))
    spans.sort()

    merged: list[list[int]] = []
    for start, end in spans:
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])

    claimed = sum(end - start for start, end in merged)
    gaps: list[Gap] = []
    cursor = text_lo
    for start, end in merged:
        if start > cursor:
            gaps.append(Gap(cursor, start - cursor))
        cursor = end
    if cursor < text_hi:
        gaps.append(Gap(cursor, text_hi - cursor))

    return TextCoverage(
        total_bytes=max(text_hi - text_lo, 0), claimed_bytes=claimed, gaps=tuple(gaps)
    )


def size_buckets(
    classified: Iterable[Classified], *, edges: Sequence[int] = (16, 64, 256, 1024, 4096)
) -> tuple[tuple[str, int], ...]:
    """Count functions per size bucket, so the backlog shows where work sits.

    Returns `(label, count)` in ascending size order with a final open-ended
    bucket. Labels are stable strings, because they end up in a committed file.
    """
    counts = [0] * (len(edges) + 1)
    for item in classified:
        size = item.function.size_bytes
        index = len(edges)
        for position, edge in enumerate(edges):
            if size < edge:
                index = position
                break
        counts[index] += 1
    labels: list[str] = []
    previous = 0
    for edge in edges:
        labels.append(f"{previous}-{edge - 1} B")
        previous = edge
    labels.append(f">= {previous} B")
    return tuple(zip(labels, counts, strict=True))


def address_buckets(
    classified: Iterable[Classified], *, text_lo: int, text_hi: int, buckets: int = 8
) -> tuple[tuple[int, int, int], ...]:
    """Count functions per equal slice of `.text`, as `(start_va, end_va, count)`.

    Shows concentration by address, which is what makes the library tail at the
    top of `.text` visible without asserting a cutoff exists.
    """
    span = max(text_hi - text_lo, 0)
    if span == 0 or buckets <= 0:
        return ()
    width = -(-span // buckets)  # ceiling, so the last bucket absorbs the remainder
    counts = [0] * buckets
    for item in classified:
        entry = item.function.entry_va
        if not (text_lo <= entry < text_hi):
            continue
        counts[min((entry - text_lo) // width, buckets - 1)] += 1
    return tuple(
        (text_lo + index * width, min(text_lo + (index + 1) * width, text_hi), count)
        for index, count in enumerate(counts)
    )


# --------------------------------------------------------------------------- #
# Artifacts and inputs.
# --------------------------------------------------------------------------- #

ARTIFACT_XBE = "xbe"
ARTIFACT_FUNCTIONS = "functions.csv"
ARTIFACT_FLIRT = "flirt_names.csv"
ARTIFACT_CANDIDATES = "missed_candidates.csv"
ARTIFACT_QUALITY = "decompiled-c"
ARTIFACT_SOURCE_TREE = "source-tree"
ARTIFACT_XDK_IMPLEMENTATIONS = "xdk-implementation-registry"
ARTIFACT_REPLACE_MANIFEST = "replace/manifest.json"
ARTIFACT_REPLACE_PROOF = "replace/proof.json"
#: Not in `Artifacts.missing`: it only feeds the judgeable figure, and the main
#: replacement metric is computed without it.
ARTIFACT_HARNESS_RESULTS = "harness/results.csv"


@dataclass(frozen=True)
class Inputs:
    """Loaded artifact data. `None` means the artifact was absent.

    Separating this from the loaders is what makes every metric testable from
    small synthetic values, and what makes "artifact missing" a case the tests can
    construct directly rather than by deleting files.
    """

    text_lo: int | None = None
    text_hi: int | None = None
    functions: tuple[Function, ...] | None = None
    xtlid_addresses: frozenset[int] | None = None
    flirt_addresses: frozenset[int] | None = None
    #: Tracked per-function region decisions (T1565), entry VA -> game or library.
    class_overrides: Mapping[int, str] = field(default_factory=dict)
    gated_candidates: int | None = None
    quality: QualityReport | None = None
    imported_ordinals: int | None = None
    #: Binding rows found but not counted, because they break the ORD_ convention.
    ordinal_warnings: list[str] = field(default_factory=list)
    implemented_ordinals: int | None = None
    #: Registered ordinals excluded from `implemented_ordinals` because the title
    #: does not import them. Empty when the import list or the runner was absent.
    ordinals_not_imported: tuple[int, ...] = ()
    xdk_surface: Mapping[str, int] | None = None
    xdk_implemented: int | None = None
    python_tests: int | None = None
    c_suites: int | None = None
    #: Why the C suite count may be low: set when it fell back to counting
    #: `add_test(` lines instead of asking CTest. See `count_c_suites`.
    c_suite_warnings: list[str] = field(default_factory=list)
    predicted_total: int = PREDICTED_FUNCTION_TOTAL
    #: The raw Ghidra export, before the tracked overrides and verified additions (T1266).
    #: `None` when unknown. Used only to state, openly, how far each denominator moved.
    export_functions: tuple[Function, ...] | None = None
    #: Hand-written replacements registered in the compiled executable. `None` is absent.
    replace_manifest: ReplacementManifest | None = None
    #: The harness run in replacement mode against that manifest. `None` is absent.
    replace_proof: ReplacementProof | None = None
    #: Set when the manifest or the proof exists but could not be read. Distinct from
    #: absent, because an unreadable artifact must say WHY it is not being believed.
    replace_manifest_error: str | None = None
    replace_proof_error: str | None = None
    #: Game and library VAs with at least one harness verdict row. `None` is absent.
    harness_verdict_vas: frozenset[int] | None = None
    harness_error: str | None = None
    #: Tracked proof snapshot (T1462), the working tree's registrations and the XBE
    #: sha256 that pins the oracle. Each `None` is absent, with an `_error` if unreadable.
    replace_snapshot: ReplacementSnapshot | None = None
    replace_snapshot_error: str | None = None
    game_tree: GameSourceTree | None = None
    game_tree_error: str | None = None
    xbe_sha256: str | None = None
    #: Tracked name overlay (T1260). `None` when absent or functions.csv is absent.
    name_overlay: NameOverlay | None = None
    #: Tracked library-name overlay (T1506). Counts toward naming-library only.
    library_name_overlay: NameOverlay | None = None

    @property
    def has_text_range(self) -> bool:
        return self.text_lo is not None and self.text_hi is not None


@dataclass(frozen=True)
class Artifacts:
    """Where each artifact lives, or `None` when it is not present on disk."""

    xbe: Path | None = None
    functions: Path | None = None
    flirt_names: Path | None = None
    candidates: Path | None = None
    quality_dir: Path | None = None
    cmakelists: Path | None = None
    src_xbox: Path | None = None
    tests_dir: Path | None = None
    replace_manifest: Path | None = None
    replace_proof: Path | None = None
    #: Tracked, so absent only outside a repository checkout. Never listed as `missing`.
    replace_snapshot: Path | None = None
    game_source: Path | None = None
    harness_results: Path | None = None
    xdk_registry: Path | None = None

    @property
    def missing(self) -> tuple[str, ...]:
        """Artifact names a metric may ask for that are not present."""
        absent: list[str] = []
        if self.xbe is None:
            absent.append(ARTIFACT_XBE)
        if self.functions is None:
            absent.append(ARTIFACT_FUNCTIONS)
        if self.flirt_names is None:
            absent.append(ARTIFACT_FLIRT)
        if self.candidates is None:
            absent.append(ARTIFACT_CANDIDATES)
        if self.quality_dir is None:
            absent.append(ARTIFACT_QUALITY)
        if self.xdk_registry is None:
            absent.append(ARTIFACT_XDK_IMPLEMENTATIONS)
        if self.replace_manifest is None:
            absent.append(ARTIFACT_REPLACE_MANIFEST)
        if self.replace_proof is None:
            absent.append(ARTIFACT_REPLACE_PROOF)
        return tuple(absent)


#: Candidate locations for the target XBE, in preference order. A fresh clone has
#: none of these, which is the normal case this tool must survive.
XBE_SEARCH = (
    Path("tmp/oxm-extract/retail/default.xbe"),
    Path("extracted/default.xbe"),
    Path("generated/retail/default.xbe"),
)


def discover_artifacts(
    root: Path,
    *,
    build: str = "retail",
    xbe: Path | None = None,
    replace_dir: Path | None = None,
) -> Artifacts:
    """Find whatever is present under `root`, reporting absence rather than failing.

    `replace_dir` is where the replacement manifest and proof live, default
    `<root>/generated/replace`. It is NOT under `generated/<build>/` because the
    replacements are hand-written code and the proof is a harness run, neither of which
    is a property of one extracted build. The harness baseline is read from the fixed
    `<root>/generated/harness/results.csv`.
    """

    def present(path: Path) -> Path | None:
        return path if path.exists() else None

    generated = root / "generated" / build
    replace = replace_dir if replace_dir is not None else root / "generated" / "replace"
    found_xbe = present(xbe) if xbe is not None else None
    if found_xbe is None and xbe is None:
        for relative in XBE_SEARCH:
            found_xbe = present(root / relative)
            if found_xbe is not None:
                break
    return Artifacts(
        xbe=found_xbe,
        functions=present(generated / "functions.csv"),
        flirt_names=present(generated / "flirt_names.csv"),
        candidates=present(generated / "missed_candidates.csv"),
        quality_dir=present(generated / "src"),
        cmakelists=present(root / "CMakeLists.txt"),
        src_xbox=present(root / "src" / "xbox"),
        tests_dir=present(root / "tests"),
        replace_manifest=present(replace / "manifest.json"),
        replace_proof=present(replace / "proof.json"),
        replace_snapshot=present(root / DEFAULT_REPLACE_SNAPSHOT),
        game_source=present(root / GAME_SOURCE_DIR),
        harness_results=present(root / "generated" / "harness" / "results.csv"),
        xdk_registry=present(generated / f"{ARTIFACT_XDK_IMPLEMENTATIONS}.json"),
    )


def read_xdk_registry(path: Path, surface_total: int | None) -> int:
    """Verified fully implemented XDK functions from `tools/xdk_implementation_registry.py`.

    Refuses a registry whose surface differs from the image's recovered surface (stale)
    or whose count disagrees with its own rows.
    """
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("kind") != ARTIFACT_XDK_IMPLEMENTATIONS:
        raise ValueError(f"{path} is not an {ARTIFACT_XDK_IMPLEMENTATIONS} document")
    entries = data["entries"]
    count = sum(1 for row in entries if row["verified_full_implementation"])
    if count != data["verified_full_implementation_count"]:
        raise ValueError(f"{path}: verified count disagrees with its entries")
    if surface_total is not None and len(entries) != surface_total:
        raise ValueError(
            f"{path} is stale: {len(entries)} surface rows, image surface is {surface_total}"
        )
    return count


def read_flirt_addresses(path: Path) -> frozenset[int]:
    """Entry addresses from a `tools/flirt` proposal CSV.

    Every row counts, including the ones `write_ghidra_names` drops as repeated:
    a repeated name is weak evidence of *which* library function this is, and
    conclusive evidence that it is library code at all, which is all this needs.
    """
    addresses: set[int] = set()
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None or "entry_va" not in reader.fieldnames:
            raise ValueError(f"{path}: expected an entry_va column, got {reader.fieldnames!r}")
        for line, row in enumerate(reader, start=2):
            raw = (row["entry_va"] or "").strip()
            try:
                addresses.add(int(raw, 16))
            except ValueError:
                raise ValueError(f"{path}:{line}: entry_va is not hex: {raw!r}") from None
    return frozenset(addresses)


def count_csv_rows(path: Path) -> int:
    """Data rows in a CSV, header excluded."""
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle)
        if next(reader, None) is None:
            return 0
        return sum(1 for row in reader if row)


def text_range(xbe: Xbe) -> tuple[int, int]:
    """Half-open virtual address range of `.text`."""
    for section in xbe.sections:
        if section.name == TEXT_SECTION_NAME:
            return section.virtual_addr, section.virtual_addr + section.virtual_size
    raise ValueError(f"image has no {TEXT_SECTION_NAME} section")


def read_text_bytes(data: bytes, xbe: Xbe) -> tuple[bytes, int]:
    """`.text` raw bytes and the virtual address of their first byte."""
    for section in xbe.sections:
        if section.name == TEXT_SECTION_NAME:
            start = section.raw_addr
            return data[start : start + section.raw_size], section.virtual_addr
    raise ValueError(f"image has no {TEXT_SECTION_NAME} section")


#: Matches one row of the ordinal binding tables in `src/xbox/*.c`, which is the
#: single place an implementation is wired to an ordinal. Counting registrations
#: rather than trusting a written-down number is deliberate: context.md 6m records
#: four ordinal numbers being misremembered, one of them pointing at `NtReadFile`.
ORDINAL_BINDING = re.compile(r"\{\s*ORD_[A-Za-z0-9_]+\s*,")


#: A binding-table row whose constant does NOT follow the `ORD_` convention. Such a
#: row is a real registration that this counter would miss, so it is reported rather
#: than ignored -- `kernel_sync.c` originally used `ORDINAL_*` and the badge silently
#: read 10 when 15 ordinals were registered. A metric that undercounts in silence is
#: worse than no metric, because it reads as a stall rather than as a bug.
NONCONFORMING_BINDING = re.compile(r"\{\s*(ORDINAL_[A-Za-z0-9_]+|[A-Z][A-Z0-9_]{6,})\s*,\s*handle_")


def count_registered_ordinals(cmake_build: Path | None) -> frozenset[int] | None:
    """Ordinals the HLE actually registers, asked of `hle_report`, or None if it cannot run.

    THE SOURCE OF TRUTH, for the reason `count_c_suites` asks `ctest -N`: a regex over source
    text counts DECLARATIONS, and it silently missed the whole AV module (62 reported, 66
    registered) because that module names its constants `KERNEL_AV_ORD_*` rather than `ORD_*`.
    The nonconforming-binding warning below did not catch it either. Asking the runner cannot
    drift that way.

    Returns the ordinal NUMBERS rather than a count, so the caller can intersect them
    with the title's import list. A non-numeric token means the output is not the
    ordinal list this expected, so the answer is "cannot say" rather than a guess.
    """
    if cmake_build is None:
        return None
    tool = cmake_build / "hle_report"
    if not tool.is_file():
        return None
    try:
        out = subprocess.run(  # noqa: S603 -- fixed argv, no shell
            [str(tool), "--implemented-ordinals"],
            capture_output=True,
            text=True,
            check=False,
            timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0 or not out.stdout.strip():
        return None
    ordinals: set[int] = set()
    for part in out.stdout.strip().split(","):
        token = part.strip()
        if not token:
            continue
        try:
            ordinals.add(int(token))
        except ValueError:
            return None
    return frozenset(ordinals) if ordinals else None


def implemented_ordinal_count(
    registered: frozenset[int] | None,
    regex_count: int | None,
    imported: frozenset[int] | None,
) -> tuple[int | None, tuple[int, ...]]:
    """The kernel badge numerator, plus the registered ordinals excluded from it.

    The runner's registered set wins over the regex count, as above. When the title's
    import list is known, a registered ordinal the title does not import is EXCLUDED
    from the numerator: the badge reads "of the ordinals TSFP actually imports", and
    counting ordinals 103, 130 and 251 against that denominator over-reported by its
    own definition (104/151 where 101/151 was true, docs/tasks.md T257 finding).
    The excluded ordinals are returned so the badge note can name them.
    """
    if registered is None:
        return regex_count, ()
    if imported is None:
        return len(registered), ()
    return len(registered & imported), tuple(sorted(registered - imported))


def count_implemented_ordinals(src_xbox: Path) -> tuple[int, list[str]]:
    """How many kernel ordinals have real implementations registered.

    Returns `(count, warnings)`. A warning means a binding-table row was found whose
    constant does not use the `ORD_` prefix, so it was NOT counted -- the caller must
    surface that rather than reporting a quietly low number.
    """
    total = 0
    warnings: list[str] = []
    for path in sorted(src_xbox.glob("*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        total += len(ORDINAL_BINDING.findall(text))
        for name in NONCONFORMING_BINDING.findall(text):
            if not name.startswith("ORD_"):
                warnings.append(
                    f"{path.name}: binding '{name}' is not ORD_-prefixed and was not counted"
                )
    return total, warnings


#: `add_test(NAME ...)` is the only thing that makes a C suite run under CTest, so
#: it is what gets counted. Some are inside an `if()`, hence "declared".
CTEST_REGISTRATION = re.compile(r"^\s*add_test\s*\(", re.MULTILINE)

CTEST_LISTED = re.compile(r"^\s*Test\s+#\d+:", re.MULTILINE)


def count_c_suites(cmakelists: Path, *, cmake_build: Path | None = None) -> tuple[int, list[str]]:
    """C test suites that CTest will run. Returns `(count, warnings)`.

    ASKS CTEST WHEN IT CAN, and counts `add_test(` lines only as a fallback. This
    module already states the right principle for Python -- "asks pytest rather than
    counting `def test_` lines ... the number that matters is the one that actually
    runs" -- and then violated it here, which cost 3 suites.

    MEASURED: 23 `add_test(` lines in CMakeLists.txt, 26 tests registered. The four
    GPU suites are declared by a `foreach()` loop whose single `add_test(NAME
    ${gpu_suite} ...)` line registers four tests, so the static count reads 1 where 4
    run. One textual occurrence is not one test, and no regex over CMake source can
    know that without evaluating the CMake.

    The static fallback stays, because the badge has to be producible in a clean
    checkout with no build directory -- but it WARNS, because an undercounting metric
    that stays silent reads as a stall rather than as a bug. That exact failure has
    shipped on this project twice: the badge generator once read 10 ordinals where 15
    were registered, and the first version of `arity_check.py` saw 10 of 20.
    """
    if cmake_build is not None and (cmake_build / "CTestTestfile.cmake").is_file():
        try:
            listed = subprocess.run(  # noqa: S603 -- fixed argv, no shell
                ["ctest", "-N"],
                cwd=cmake_build,
                capture_output=True,
                text=True,
                check=False,
                timeout=120,
            )
        except (OSError, subprocess.SubprocessError) as error:
            return _count_c_suites_statically(
                cmakelists,
                [
                    f"ctest -N could not be run ({error}), so the C suite count is the "
                    f"number of add_test( lines and UNDERCOUNTS any registered in a loop"
                ],
            )
        if listed.returncode == 0:
            count = len(CTEST_LISTED.findall(listed.stdout))
            if count > 0:
                return count, []
        return _count_c_suites_statically(
            cmakelists,
            [
                "ctest -N listed no tests, so the C suite count fell back to the number "
                "of add_test( lines and UNDERCOUNTS any registered in a loop"
            ],
        )
    return _count_c_suites_statically(
        cmakelists,
        [
            "no configured build directory, so the C suite count is the number of "
            "add_test( lines and UNDERCOUNTS any registered in a loop (it read 23 "
            "where 26 tests run)"
        ],
    )


def _count_c_suites_statically(cmakelists: Path, warnings: list[str]) -> tuple[int, list[str]]:
    return len(CTEST_REGISTRATION.findall(cmakelists.read_text(encoding="utf-8"))), warnings


COLLECTED_TESTS = re.compile(r"(\d+) tests? collected")


def collection_interpreter(root: Path, python: str | None) -> str:
    """The interpreter `count_python_tests` runs pytest with.

    An explicit `python` always wins. Otherwise the project venv under `root` is
    preferred, and when it does not exist the CURRENT interpreter stands in: a fresh
    worktree carries no committed `.venv/`, and the old hard-coded
    `./.venv/bin/python` failed there with OSError, which `count_python_tests` maps
    to `None`, which rendered a silently grey "unknown" tests badge (T269).
    """
    if python is not None:
        return python
    venv = root / ".venv" / "bin" / "python"
    if venv.is_file():
        return str(venv)
    return sys.executable


def count_python_tests(root: Path, *, python: str | None = None) -> int | None:
    """Collected Python test count, or `None` if collection could not be run.

    Asks pytest rather than counting `def test_` lines: parametrised tests make the
    two disagree, and the number that matters is the one that actually runs.
    """
    executable = collection_interpreter(root, python)
    try:
        result = subprocess.run(  # noqa: S603 -- fixed argv, no shell
            [executable, "-m", "pytest", "--collect-only", "-q"],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
            timeout=300,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    match = COLLECTED_TESTS.search(result.stdout)
    return int(match.group(1)) if match else None


def load_inputs(
    artifacts: Artifacts,
    *,
    root: Path,
    python: str | None = None,
    collect_tests: bool = True,
    cmake_build: Path | None = None,
) -> Inputs:
    """Read every artifact that is present, leaving absent ones as `None`."""
    text_lo = text_hi = None
    imported_set: frozenset[int] | None = None
    surface: dict[str, int] | None = None
    xtlid: frozenset[int] | None = None

    if artifacts.xbe is not None:
        data = artifacts.xbe.read_bytes()
        xbe = parse_xbe(data)
        text_lo, text_hi = text_range(xbe)
        imported_set = frozenset(xbe.kernel_import_ordinals)
        xtlid = frozenset(entry.address for entry in xbe.xtlid)
        # Imported lazily: it owns the XDK section list and the 0xE8/0xE9 scan, and
        # duplicating either here would be a second thing to keep in step.
        from tools.gen_d3d8_surface import recover_surface

        surface = {
            section: count
            for section, (count, _sites) in recover_surface(data, xbe).summary().items()
        }

    # The shared table (export + tracked overrides + verified additions, T1266). Paths are
    # relative to `root`, so a synthetic root without them sees the raw export.
    export_functions = (
        tuple(load_functions(artifacts.functions)) if artifacts.functions is not None else None
    )
    functions = (
        tuple(
            load_function_table(
                artifacts.functions,
                root / DEFAULT_FUNCTION_OVERRIDES,
                root / DEFAULT_FUNCTION_ADDITIONS,
            )
        )
        if artifacts.functions is not None
        else None
    )
    flirt = (
        read_flirt_addresses(artifacts.flirt_names) if artifacts.flirt_names is not None else None
    )
    candidates = count_csv_rows(artifacts.candidates) if artifacts.candidates is not None else None
    quality = score_export(artifacts.quality_dir) if artifacts.quality_dir is not None else None

    # Counted ONCE. `count_implemented_ordinals` above is called twice for its two
    # halves, which is merely wasteful; doing that here would run `ctest -N` twice.
    c_suites: int | None = None
    c_suite_warnings: list[str] = []
    if artifacts.cmakelists is not None:
        c_suites, c_suite_warnings = count_c_suites(artifacts.cmakelists, cmake_build=cmake_build)

    registered = count_registered_ordinals(cmake_build)
    regex_count, ordinal_warning_list = (
        count_implemented_ordinals(artifacts.src_xbox)
        if artifacts.src_xbox is not None
        else (None, [])
    )
    implemented_count, not_imported = implemented_ordinal_count(
        registered, regex_count, imported_set
    )
    if registered is not None and regex_count is not None and len(registered) != regex_count:
        # Surfaced, not smoothed over: the two counts measure different things and their
        # disagreement is the finding (a module whose constants do not follow ORD_*).
        ordinal_warning_list = [
            *ordinal_warning_list,
            f"the source-text count ({regex_count}) disagrees with the runner's "
            f"({len(registered)}); the runner is used, and the difference is bindings "
            "the regex cannot see",
        ]

    manifest, manifest_error = _read_or_explain(
        artifacts.replace_manifest, read_replacement_manifest
    )
    proof, proof_error = _read_or_explain(artifacts.replace_proof, read_replacement_proof)
    verdict_vas, harness_error = _read_or_explain(
        artifacts.harness_results, read_harness_verdict_vas
    )
    snapshot, snapshot_error = _read_or_explain(
        artifacts.replace_snapshot, read_replacement_snapshot
    )
    game_tree, game_tree_error = _read_or_explain(artifacts.game_source, read_game_source_tree)
    xbe_sha256 = (
        hashlib.sha256(artifacts.xbe.read_bytes()).hexdigest()
        if artifacts.xbe is not None
        else None
    )

    return Inputs(
        text_lo=text_lo,
        text_hi=text_hi,
        functions=functions,
        export_functions=export_functions,
        xtlid_addresses=xtlid,
        flirt_addresses=flirt,
        class_overrides=class_overrides_for_root(root, functions),
        gated_candidates=candidates,
        quality=quality,
        imported_ordinals=len(imported_set) if imported_set is not None else None,
        implemented_ordinals=implemented_count,
        ordinals_not_imported=not_imported,
        ordinal_warnings=ordinal_warning_list,
        xdk_surface=surface,
        # Registration alone does not establish full implementation; only entries
        # the verified registry marks count (tools/xdk_implementation_registry.py).
        xdk_implemented=(
            read_xdk_registry(
                artifacts.xdk_registry, sum(surface.values()) if surface is not None else None
            )
            if artifacts.xdk_registry is not None
            else None
        ),
        python_tests=count_python_tests(root, python=python) if collect_tests else None,
        c_suites=c_suites,
        c_suite_warnings=c_suite_warnings,
        replace_manifest=manifest,
        replace_proof=proof,
        replace_manifest_error=manifest_error,
        replace_proof_error=proof_error,
        harness_verdict_vas=verdict_vas,
        harness_error=harness_error,
        replace_snapshot=snapshot,
        replace_snapshot_error=snapshot_error,
        game_tree=game_tree,
        game_tree_error=game_tree_error,
        xbe_sha256=xbe_sha256,
        name_overlay=_load_overlay(root, functions),
        library_name_overlay=_load_library_overlay(root, functions),
    )


def _load_overlay(root: Path, functions: Iterable[Function] | None) -> NameOverlay | None:
    path = root / DEFAULT_NAME_OVERLAY
    if functions is None or not path.is_file():
        return None
    return load_function_names(
        path,
        frozenset(function.entry_va for function in functions),
        label=DEFAULT_NAME_OVERLAY.as_posix(),
    )


def _load_library_overlay(root: Path, functions: Iterable[Function] | None) -> NameOverlay | None:
    path = root / DEFAULT_LIBRARY_NAME_OVERLAY
    if functions is None or not path.is_file():
        return None
    return load_library_names(
        path,
        frozenset(function.entry_va for function in functions),
        label=DEFAULT_LIBRARY_NAME_OVERLAY.as_posix(),
    )


_Loaded = TypeVar("_Loaded")


def _read_or_explain(
    path: Path | None, reader: Callable[[Path], _Loaded]
) -> tuple[_Loaded | None, str | None]:
    """`(value, None)` when read, `(None, None)` when absent, `(None, why)` when unreadable.

    A present-but-malformed artifact does not abort the whole run: it makes only the
    metrics that need it `unknown`, with the parser's message as the reason.
    """
    if path is None:
        return None, None
    try:
        return reader(path), None
    except ValueError as error:
        return None, str(error)


# --------------------------------------------------------------------------- #
# Decompilation: functions replaced by hand-written C AND proven equivalent.
# --------------------------------------------------------------------------- #
#
# WHAT THIS MEASURES, AND WHY IT IS NOT THE NAMING METRIC. The naming badge reads
# 0.00% of 9,322 and will keep doing so, because the names are not in the binary
# (docs/tasks.md T15). Decompilation succeeds when a game function has been
# REWRITTEN by hand and the rewrite has been shown, by running the differential
# harness in replacement mode, to behave like the original. This metric counts
# exactly those functions. It is a different axis from recompilation (kernel calls
# reached, boundaries dispatched, a frame rendered) and a much longer one.
#
# THE COUNT COMES FROM EXECUTING EVIDENCE, NEVER FROM COUNTING FILES. Two JSON
# artifacts are read and cross-checked: the manifest dumped by the compiled
# executable that links the hand-written code (MEASURED: what is registered), and
# the proof written by the harness run against that manifest (MEASURED: what
# was executed). A function in a source file that is not in the manifest is not
# counted, and a function in the manifest with no passing proof is not counted.
#
# DENOMINATOR: the game functions, i.e. `region == "game"` after
# `classify_functions`. It is the same `NamingSplit.game_total` the naming metric
# uses (9,322 on the pre-T1266 table, 11,105 at T1568 and 10,502 after the T1641
# reclassification, see docs/t1266-function-table-additions.md), reused rather than recomputed.
#
# NULL MODEL: a do-nothing replacement (a function that returns a constant), held
# back by the `null_agree_rate` gate, scores zero proven functions. No other null
# is claimed. The percentage counts FUNCTIONS, not bytes and not behaviour, so it
# must not be compared with the naming percentages or the `.text` claimed figure.

#: Both artifacts declare this schema version. Anything else is not parsed, because
#: a guess at a changed format could turn a gate into a no-op.
REPLACE_SCHEMA = 1

#: The `kind` a proof artifact must declare, so a file from another harness mode is
#: not mistaken for one.
PROOF_KIND = "replacement-proof"

#: Fewest harness VERDICTS (AGREE, DISAGREE or SUBJECT-FAULTED cases, so excluding
#: oracle-faulted ones) a function needs. POLICY, not a measurement, and a floor
#: rather than a confidence bound: a bug confined to a much smaller share of the
#: input space than 1 in `MIN_VERDICTS` is likely to go unseen at this count. It
#: exists so that a function whose inputs mostly crashed the ORIGINAL (see
#: `oracle_faulted`) cannot pass on a handful of surviving cases.
MIN_VERDICTS = 100

#: Least `coverage` the harness may report for a function. POLICY. The harness
#: defines the figure (this module does not recompute it); it is read as the share
#: of the original body the verdict cases exercised. Below it, a rewrite could be
#: wrong in a branch no case ever took and still show zero disagreements.
MIN_COVERAGE = 0.9

#: Greatest `null_agree_rate` allowed: the fraction of verdict cases on which a
#: do-nothing function returning a constant would ALSO have agreed. POLICY. A high
#: value means the inputs cannot tell a correct function from a null one, so zero
#: disagreements proves nothing. This gate is the whole of the null model above.
MAX_NULL_AGREE = 0.9

#: Harness outcomes that are verdicts. Every other outcome (a skip, an
#: oracle fault) is not a judgement on the subject.
VERDICT_OUTCOMES = frozenset({"AGREE", "DISAGREE", "SUBJECT-FAULTED"})

#: Gate names, in the order they are applied. The FIRST failing gate is the one
#: reported, so this order is part of the output and a test pins it.
GATE_NOT_GAME = "not-a-game-function"
GATE_NO_PROOF = "no-proof-entry"
GATE_COUNTS = "counts-inconsistent"
GATE_INPUT_ABI = "input-abi-mismatch"
GATE_REGS = "regs-ignored-exceeds-scratch"
GATE_REPLACED = "replacement-not-confirmed"
GATE_DISAGREE = "disagree"
GATE_SUBJECT_FAULT = "subject-faulted"
GATE_VERDICTS = "too-few-verdicts"
GATE_COVERAGE = "low-coverage"
GATE_VACUOUS = "near-vacuous"
GATE_NULL_AGREE = "null-agree-rate"
#: T1624: last gate, only ever reached by fp-scalar-v1 rows. Kept out of GATE_ORDER (a test pins
#: that tuple) and listed in ALL_GATES for the failure counts.
GATE_FP_SCALAR = "fp-scalar-receipt"
GATE_ORDER = (
    GATE_NOT_GAME,
    GATE_NO_PROOF,
    GATE_COUNTS,
    GATE_INPUT_ABI,
    GATE_REGS,
    GATE_REPLACED,
    GATE_DISAGREE,
    GATE_SUBJECT_FAULT,
    GATE_VERDICTS,
    GATE_COVERAGE,
    GATE_VACUOUS,
    GATE_NULL_AGREE,
)
ALL_GATES = (*GATE_ORDER, GATE_FP_SCALAR)


@dataclass(frozen=True)
class ReplacementFunction:
    """One registered hand-written replacement, as dumped by the compiled executable."""

    va: int
    name: str
    convention: str
    stack_args: int
    scratch: tuple[str, ...]
    source: str
    register_inputs: tuple[str, ...] | None = None

    def __post_init__(self) -> None:
        validate_inputs(self.register_inputs, self.convention, self.stack_args, self.scratch)


@dataclass(frozen=True)
class ReplacementManifest:
    manifest_sha: str
    functions: tuple[ReplacementFunction, ...]


@dataclass(frozen=True)
class ProofEntry:
    """One function's row of the harness replacement-mode proof."""

    va: int
    cases: int
    verdicts: int
    agree: int
    disagree: int
    subject_faulted: int
    oracle_faulted: int
    coverage: float
    body_insns: int
    near_vacuous: bool
    null_agree_rate: float
    replaced_confirmed: bool
    regs_ignored: tuple[str, ...]
    #: Harness reason the function was not judged (empty when it ran). A proof with any
    #: `--only-va` skip is a single-function debug run, never a whole-registry measurement.
    unjudgeable: str = ""
    state_contract: Mapping[str, object] | None = None
    source_closure: Mapping[str, object] | None = None
    source_seed: int | None = None
    #: T1624: fp-scalar-v1 record (mode, MXCSR pins, NaN-pair count and cap, matrix identity),
    #: derived from the receipt in `source_closure`; None for every other proof.
    fp_scalar: Mapping[str, object] | None = None
    fixture_provider_selection: tuple[str, ...] | None = None
    input_contract: Mapping[str, object] | None = None
    named_global_object_contract: Mapping[str, object] | None = None


@dataclass(frozen=True)
class ReplacementProof:
    manifest_sha: str
    entries: tuple[ProofEntry, ...]


@dataclass(frozen=True)
class GateFailure:
    """A manifest function that is not proven, and the first gate it failed."""

    function: ReplacementFunction
    gate: str


@dataclass(frozen=True)
class ReplacementEvaluation:
    """The outcome of judging every manifest function against the gates."""

    game_total: int
    registered: int
    proven: tuple[ReplacementFunction, ...]
    failures: tuple[GateFailure, ...]
    #: Distinct game VAs with at least one harness verdict, or `None` when unknown.
    judgeable: int | None
    judgeable_unknown: str
    #: Where the verdicts came from: `live proof` or `tracked snapshot` (T1462).
    source: str = "live proof"
    snapshot: ReplacementSnapshot | None = None
    #: Registrations the snapshot could not vouch for (changed, new, dropped).
    stale: tuple[StaleReplacement, ...] = ()
    #: Snapshot vs live comparison lines. Lines starting `DISAGREEMENT` are loud.
    cross_check: tuple[str, ...] = ()
    game_total_pinned: bool = False
    xbe_verified: bool = False
    #: Why the live proof was not used when the snapshot was.
    source_note: str = ""

    @property
    def disagreements(self) -> tuple[str, ...]:
        return tuple(line for line in self.cross_check if line.startswith(DISAGREEMENT))

    @property
    def failure_counts(self) -> tuple[tuple[str, int], ...]:
        """`(gate, count)` of first-failing gates, in gate order, zero counts omitted."""
        counts = {gate: 0 for gate in ALL_GATES}
        for failure in self.failures:
            counts[failure.gate] += 1
        return tuple((gate, counts[gate]) for gate in ALL_GATES if counts[gate])


def _field(raw: Mapping[str, object], key: str, where: str) -> object:
    if key not in raw:
        raise ValueError(f"{where}: missing field {key!r}")
    return raw[key]


def _as_int(value: object, key: str, where: str) -> int:
    # `bool` is an `int` subclass, and `true` must not read as a count of 1.
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{where}: {key!r} must be a non-negative integer, got {value!r}")
    return value


def _as_fraction(value: object, key: str, where: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{where}: {key!r} must be a number, got {value!r}")
    number = float(value)
    # Written as a chained comparison so NaN, which fails every comparison, is rejected.
    if not 0.0 <= number <= 1.0:
        raise ValueError(f"{where}: {key!r} must be within [0, 1], got {value!r}")
    return number


def _as_bool(value: object, key: str, where: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f"{where}: {key!r} must be true or false, got {value!r}")
    return value


def _as_str(value: object, key: str, where: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{where}: {key!r} must be a string, got {value!r}")
    return value


def _as_names(value: object, key: str, where: str) -> tuple[str, ...]:
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise ValueError(f"{where}: {key!r} must be a list of strings, got {value!r}")
    return tuple(value)


def _as_va(value: object, where: str) -> int:
    """`0x00059d20`, case-insensitive. Anything that is not hex text is an error."""
    text = _as_str(value, "va", where)
    try:
        return int(text, 16)
    except ValueError:
        raise ValueError(f"{where}: 'va' is not hex: {text!r}") from None


def _load_document(
    path: Path, *, kind: str | None, schemas: tuple[int, ...] = (REPLACE_SCHEMA,)
) -> Mapping[str, object]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: not readable JSON ({error})") from None
    if not isinstance(document, dict):
        raise ValueError(f"{path}: expected a JSON object at the top level")
    if type(document.get("schema")) is not int or document.get("schema") not in schemas:
        raise ValueError(f"{path}: schema is {document.get('schema')!r}, expected one of {schemas}")
    if kind is not None and document.get("kind") != kind:
        raise ValueError(f"{path}: kind is {document.get('kind')!r}, expected {kind!r}")
    return document


def _sha(document: Mapping[str, object], path: Path) -> str:
    sha = _as_str(_field(document, "manifest_sha", str(path)), "manifest_sha", str(path))
    return sha.lower()


def read_replacement_manifest(path: Path) -> ReplacementManifest:
    """Parse the registered-replacement manifest. Raises `ValueError` if malformed.

    A duplicate VA is an error rather than a last-one-wins: two registrations for one
    address mean the manifest cannot say which body is being proven.
    """
    document = _load_document(path, kind=None)
    sha = _sha(document, path)
    if re.fullmatch(r"[0-9a-f]{64}", sha) is None:
        raise ValueError(f"{path}: manifest_sha is not 64 hex digits: {sha!r}")
    raw_functions = _field(document, "functions", str(path))
    if not isinstance(raw_functions, list):
        raise ValueError(f"{path}: 'functions' must be a list")
    functions: list[ReplacementFunction] = []
    seen: set[int] = set()
    for index, raw in enumerate(raw_functions):
        where = f"{path}: functions[{index}]"
        if not isinstance(raw, dict):
            raise ValueError(f"{where}: expected an object")
        va = _as_va(_field(raw, "va", where), where)
        if va in seen:
            raise ValueError(f"{where}: duplicate registration for {va:#010x}")
        seen.add(va)
        functions.append(
            ReplacementFunction(
                va=va,
                name=_as_str(_field(raw, "name", where), "name", where),
                convention=_as_str(_field(raw, "convention", where), "convention", where),
                stack_args=_as_int(_field(raw, "stack_args", where), "stack_args", where),
                scratch=_as_names(_field(raw, "scratch", where), "scratch", where),
                source=_as_str(_field(raw, "source", where), "source", where),
                register_inputs=register_inputs(raw),
            )
        )
    return ReplacementManifest(manifest_sha=sha, functions=tuple(functions))


def _fp_scalar_from(raw: Mapping[str, object], va: int) -> Mapping[str, object] | None:
    """The fp-scalar record of a row: the stored one (snapshot) or derived from its receipt."""
    if raw.get("fp_scalar") is not None:
        return raw["fp_scalar"]  # type: ignore[return-value]
    if raw.get("state_contract") is not None and fp_scalar_closure(raw.get("source_closure")):
        return fp_scalar_record(raw["source_closure"], f"{va:#010x}")
    return None


def _proof_entry_from(raw: Mapping[str, object], va: int, where: str) -> ProofEntry:
    """One proof row, shared by the live proof and the tracked snapshot readers."""
    return ProofEntry(
        va=va,
        cases=_as_int(_field(raw, "cases", where), "cases", where),
        verdicts=_as_int(_field(raw, "verdicts", where), "verdicts", where),
        agree=_as_int(_field(raw, "agree", where), "agree", where),
        disagree=_as_int(_field(raw, "disagree", where), "disagree", where),
        subject_faulted=_as_int(_field(raw, "subject_faulted", where), "subject_faulted", where),
        oracle_faulted=_as_int(_field(raw, "oracle_faulted", where), "oracle_faulted", where),
        coverage=_as_fraction(_field(raw, "coverage", where), "coverage", where),
        body_insns=_as_int(_field(raw, "body_insns", where), "body_insns", where),
        near_vacuous=_as_bool(_field(raw, "near_vacuous", where), "near_vacuous", where),
        null_agree_rate=_as_fraction(
            _field(raw, "null_agree_rate", where), "null_agree_rate", where
        ),
        replaced_confirmed=_as_bool(
            _field(raw, "replaced_confirmed", where), "replaced_confirmed", where
        ),
        regs_ignored=_as_names(_field(raw, "regs_ignored", where), "regs_ignored", where),
        unjudgeable=raw["unjudgeable"] if isinstance(raw.get("unjudgeable"), str) else "",
        input_contract=raw.get("input_contract"),
        named_global_object_contract=raw.get("named_global_object_contract"),
        state_contract=raw.get("state_contract"),
        fixture_provider_selection=(
            tuple(raw["fixture_provider_selection"])
            if raw.get("fixture_provider_selection") is not None
            else None
        ),
        source_closure=raw.get("source_closure"),
        source_seed=raw.get("source_seed") if raw.get("state_contract") is not None else None,
        fp_scalar=_fp_scalar_from(raw, va),
    )


def read_replacement_proof(path: Path) -> ReplacementProof:
    """Parse the harness replacement-mode proof. Raises `ValueError` if malformed.

    A malformed entry makes the WHOLE proof unreadable instead of dropping that
    function: a producer bug that corrupts one row is not evidence about the others.
    """
    document = _load_document(path, kind=PROOF_KIND, schemas=(1, 2))
    validate_document(document)
    sha = _sha(document, path)
    raw_entries = _field(document, "functions", str(path))
    if not isinstance(raw_entries, list):
        raise ValueError(f"{path}: 'functions' must be a list")
    entries: list[ProofEntry] = []
    seen: set[int] = set()
    for index, raw in enumerate(raw_entries):
        where = f"{path}: functions[{index}]"
        if not isinstance(raw, dict):
            raise ValueError(f"{where}: expected an object")
        va = _as_va(_field(raw, "va", where), where)
        if va in seen:
            raise ValueError(f"{where}: duplicate proof entry for {va:#010x}")
        seen.add(va)
        entries.append(_proof_entry_from(raw, va, where))
    return ReplacementProof(manifest_sha=sha, entries=tuple(entries))


def read_harness_verdict_vas(path: Path) -> frozenset[int]:
    """Distinct VAs with at least one harness VERDICT row. Raises `ValueError` if malformed.

    A verdict row is an outcome in `VERDICT_OUTCOMES` WITH a `case_index`. Skip rows are
    excluded by outcome, which matters: MEASURED on the committed baseline, 294
    SKIPPED-UNSUPPORTED rows carry a case_index too, so "empty case_index means skip"
    is not a sufficient test on its own.
    """
    vas: set[int] = set()
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        needed = {"va", "outcome", "case_index"}
        if reader.fieldnames is None or not needed <= set(reader.fieldnames):
            raise ValueError(
                f"{path}: expected columns {sorted(needed)}, got {reader.fieldnames!r}"
            )
        for line, row in enumerate(reader, start=2):
            if row["outcome"] not in VERDICT_OUTCOMES or not (row["case_index"] or "").strip():
                continue
            raw = (row["va"] or "").strip()
            try:
                vas.add(int(raw, 16))
            except ValueError:
                raise ValueError(f"{path}:{line}: va is not hex: {raw!r}") from None
    return frozenset(vas)


def first_failing_gate(
    function: ReplacementFunction, entry: ProofEntry | None, *, is_game: bool
) -> str | None:
    """The first gate `function` fails, or `None` when it is PROVEN.

    Every comparison fails CLOSED: a gate passes only on the stated inequality, so a
    value that is off by one in the wrong direction is not proven.
    """
    if not is_game:
        return GATE_NOT_GAME
    if entry is None:
        return GATE_NO_PROOF
    # Integrity of the proof against itself and against the manifest, not thresholds.
    if entry.verdicts != entry.agree + entry.disagree + entry.subject_faulted:
        return GATE_COUNTS
    if entry.input_contract != input_contract(
        function.register_inputs, function.convention, function.stack_args
    ):
        return GATE_INPUT_ABI
    if not set(entry.regs_ignored) <= set(function.scratch):
        return GATE_REGS
    if not entry.replaced_confirmed:
        return GATE_REPLACED
    if entry.disagree != 0:
        return GATE_DISAGREE
    if entry.subject_faulted != 0:
        return GATE_SUBJECT_FAULT
    if not entry.verdicts >= MIN_VERDICTS:
        return GATE_VERDICTS
    if not entry.coverage >= MIN_COVERAGE:
        return GATE_COVERAGE
    if entry.near_vacuous:
        return GATE_VACUOUS
    if not entry.null_agree_rate <= MAX_NULL_AGREE:
        return GATE_NULL_AGREE
    if (entry.fp_scalar is not None or is_fp_scalar_contract(entry.state_contract)) and not (
        is_fp_scalar_contract(entry.state_contract) and fp_scalar_gate_ok(entry.fp_scalar)
    ):
        return GATE_FP_SCALAR
    return None


def evaluate_replacements(
    manifest: ReplacementManifest,
    proof: ReplacementProof,
    *,
    game_vas: frozenset[int],
    game_total: int,
    verdict_vas: frozenset[int] | None = None,
    judgeable_unknown: str = "",
) -> ReplacementEvaluation:
    """Judge every manifest function. Refuses a proof for a different manifest.

    A proof entry whose VA is not in the manifest is ignored: only what is registered
    can be a replacement. A manifest function with no entry is not proven.
    """
    if proof.manifest_sha != manifest.manifest_sha:
        raise ValueError("proof manifest_sha does not match the manifest: the proof is stale")
    by_va = {entry.va: entry for entry in proof.entries}
    proven: list[ReplacementFunction] = []
    failures: list[GateFailure] = []
    for function in manifest.functions:
        gate = first_failing_gate(function, by_va.get(function.va), is_game=function.va in game_vas)
        if gate is None:
            proven.append(function)
        else:
            failures.append(GateFailure(function, gate))
    return ReplacementEvaluation(
        game_total=game_total,
        registered=len(manifest.functions),
        proven=tuple(proven),
        failures=tuple(failures),
        judgeable=None if verdict_vas is None else len(game_vas & verdict_vas),
        judgeable_unknown=judgeable_unknown,
    )


# --------------------------------------------------------------------------- #
# Tracked proof snapshot (T1462).
#
# `generated/replace/{manifest,proof}.json` are gitignored harness output, so a clean
# clone or CI cannot measure the proven metric from them. The tracked snapshot replays
# ONLY the harness verdicts. Everything that can be recomputed is recomputed from the
# working tree on every run: the registered set (scanned from src/game) and each
# function's source fingerprint. A function counts as proven only when its fingerprint
# still matches and its stored verdict still passes the CURRENT gates; a changed or new
# function drops out as not proven and is listed. See docs/t1462-badge-reproducibility.md.
# --------------------------------------------------------------------------- #

SNAPSHOT_SCHEMA = 1
VECTOR_SNAPSHOT_SCHEMA = 2
SNAPSHOT_KIND = "replacement-proof-snapshot"
DEFAULT_REPLACE_SNAPSHOT = Path("docs/data/replace-proof-snapshot.json")
GAME_SOURCE_DIR = Path("src/game")
SNAPSHOT_COMMAND = "python -m tools.replace.proof_snapshot"
FINGERPRINT_RULE = (
    "sha256('<sha256 of src/game/<source> bytes, CRLF folded to LF>:<headers_digest>'), "
    "headers_digest = sha256 of sorted '<name>:<sha256>' lines over src/game/*.h"
)
#: `GAME_REPLACE(00012820, cdecl, 3, u32, name)`, the `_EXACT` form and the T1519
#: `GAME_REPLACE_EXACT_INPUTS(VA, CC, N, RET, INPUTS, name)` form (one extra field), line start.
REGISTRATION = re.compile(
    r"^GAME_REPLACE(?:_EXACT_INPUTS|_EXACT)?\(\s*([0-9A-Fa-f]+)\s*,[^,]*,[^,]*,[^,]*,(?:[^,)]*,)?\s*(\w+)\s*\)",
    re.MULTILINE,
)
DISAGREEMENT = "DISAGREEMENT"


def _digest(data: bytes) -> str:
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


@dataclass(frozen=True)
class GameSourceTree:
    """The working tree's registrations and source digests. No machine bytes, no XBE."""

    registrations: Mapping[int, tuple[str, str]]
    file_digests: Mapping[str, str]
    headers_digest: str
    #: T1624: sha256 of the tracked fp-scalar matrix result, None when the file is absent.
    fp_matrix_sha256: str | None = None

    def fingerprint(self, source: str) -> str | None:
        digest = self.file_digests.get(source)
        if digest is None:
            return None
        return hashlib.sha256(f"{digest}:{self.headers_digest}".encode()).hexdigest()


def read_game_source_tree(directory: Path) -> GameSourceTree:
    """Scan `directory` (src/game). Raises `ValueError` if absent, empty or ambiguous."""
    if not directory.is_dir():
        raise ValueError(f"{directory}: no such directory")
    sources = sorted(directory.glob("*.c"))
    if not sources:
        raise ValueError(f"{directory}: no .c files")
    # T1768: registrations in an `.inc` file are part of the build's registry too (x87_roots.c
    # includes x87_roots.inc unconditionally, `tools/replace/scan.py` scans `*.inc` and the
    # manifest names the `.inc` as the source), so each `.inc` is its own fingerprinted source.
    sources += sorted(directory.glob("*.inc"))
    registrations: dict[int, tuple[str, str]] = {}
    digests: dict[str, str] = {}
    for path in sources:
        data = path.read_bytes()
        digests[path.name] = _digest(data)
        for match in REGISTRATION.finditer(data.decode("utf-8", errors="replace")):
            va = int(match.group(1), 16)
            if va in registrations:
                raise ValueError(
                    f"{directory}: {va:#010x} is registered twice "
                    f"({registrations[va][0]} and {path.name})"
                )
            registrations[va] = (path.name, match.group(2))
    headers = "\n".join(
        f"{p.name}:{_digest(p.read_bytes())}" for p in sorted(directory.glob("*.h"))
    )
    matrix = directory.parent.parent / FP_SCALAR_MATRIX_FILE
    return GameSourceTree(
        registrations=registrations,
        file_digests=digests,
        headers_digest=hashlib.sha256(headers.encode()).hexdigest(),
        fp_matrix_sha256=hashlib.sha256(matrix.read_bytes()).hexdigest()
        if matrix.is_file()
        else None,
    )


@dataclass(frozen=True)
class SnapshotEntry:
    function: ReplacementFunction
    fingerprint: str
    is_game: bool
    gate: str | None
    proof: ProofEntry
    source_run: str
    source_cases: int


@dataclass(frozen=True)
class ReplacementSnapshot:
    measured_on: str
    commit: str
    xbe_sha256: str
    manifest_sha: str
    headers_digest: str
    game_total: int
    judgeable: int | None
    entries: tuple[SnapshotEntry, ...]


@dataclass(frozen=True)
class StaleReplacement:
    """A registration the snapshot cannot vouch for, and why. Counted as NOT proven."""

    function: ReplacementFunction
    reason: str


def _hex64(value: object, key: str, where: str) -> str:
    text = _as_str(value, key, where).lower()
    if re.fullmatch(r"[0-9a-f]{64}", text) is None:
        raise ValueError(f"{where}: {key!r} is not 64 hex digits: {text!r}")
    return text


def read_replacement_snapshot(path: Path) -> ReplacementSnapshot:
    """Parse the tracked snapshot. Raises `ValueError` if malformed or self-inconsistent.

    The stored `gate` of every entry is recomputed from its stored proof fields with the
    CURRENT gates. A hand-edited verdict (a flipped `gate`, or a zeroed `disagree`
    without the matching `gate`) therefore makes the whole snapshot unreadable.
    """
    document = _load_document(path, kind=None, schemas=(1, VECTOR_SNAPSHOT_SCHEMA))
    validate_document(document)
    where = str(path)
    if document.get("kind") != SNAPSHOT_KIND:
        raise ValueError(f"{path}: kind is {document.get('kind')!r}, expected {SNAPSHOT_KIND!r}")
    judgeable = document.get("judgeable")
    if judgeable is not None:
        judgeable = _as_int(judgeable, "judgeable", where)
    raw_entries = _field(document, "functions", where)
    if not isinstance(raw_entries, list):
        raise ValueError(f"{path}: 'functions' must be a list")
    entries: list[SnapshotEntry] = []
    seen: set[int] = set()
    for index, raw in enumerate(raw_entries):
        at = f"{path}: functions[{index}]"
        if not isinstance(raw, dict):
            raise ValueError(f"{at}: expected an object")
        va = _as_va(_field(raw, "va", at), at)
        if va in seen:
            raise ValueError(f"{at}: duplicate entry for {va:#010x}")
        seen.add(va)
        function = ReplacementFunction(
            va=va,
            name=_as_str(_field(raw, "name", at), "name", at),
            convention=_as_str(_field(raw, "convention", at), "convention", at),
            stack_args=_as_int(_field(raw, "stack_args", at), "stack_args", at),
            scratch=_as_names(_field(raw, "scratch", at), "scratch", at),
            source=_as_str(_field(raw, "source", at), "source", at),
            register_inputs=register_inputs(raw),
        )
        proof = _proof_entry_from(raw, va, at)
        is_game = _as_bool(_field(raw, "is_game", at), "is_game", at)
        gate = _field(raw, "gate", at)
        if gate is not None and not isinstance(gate, str):
            raise ValueError(f"{at}: 'gate' must be null or a gate name, got {gate!r}")
        recomputed = first_failing_gate(function, proof, is_game=is_game)
        if recomputed != gate:
            raise ValueError(
                f"{at}: stored verdict {gate!r} disagrees with {recomputed!r} recomputed from its "
                f"own proof fields for {va:#010x}: the snapshot was edited by hand"
            )
        entries.append(
            SnapshotEntry(
                function=function,
                fingerprint=_hex64(_field(raw, "fingerprint", at), "fingerprint", at),
                is_game=is_game,
                gate=gate,
                proof=proof,
                source_run=_as_str(_field(raw, "source_run", at), "source_run", at),
                source_cases=_as_int(
                    _field(raw, "source_cases_per_function", at), "source_cases_per_function", at
                ),
            )
        )
    return ReplacementSnapshot(
        measured_on=_as_str(_field(document, "measured_on", where), "measured_on", where),
        commit=_as_str(_field(document, "commit", where), "commit", where),
        xbe_sha256=_hex64(_field(document, "xbe_sha256", where), "xbe_sha256", where),
        manifest_sha=_hex64(_field(document, "manifest_sha", where), "manifest_sha", where),
        headers_digest=_hex64(_field(document, "headers_digest", where), "headers_digest", where),
        game_total=_as_int(_field(document, "game_total", where), "game_total", where),
        judgeable=judgeable,
        entries=tuple(entries),
    )


def check_snapshot_oracle(snapshot: ReplacementSnapshot, xbe_sha256: str | None) -> None:
    """Refuse a snapshot proven against a different XBE. `None` means the XBE is absent."""
    if xbe_sha256 is not None and xbe_sha256.lower() != snapshot.xbe_sha256:
        raise ValueError(
            f"the tracked proof snapshot was proven against XBE sha256 {snapshot.xbe_sha256[:12]} "
            f"but this XBE is {xbe_sha256[:12]}, so none of its verdicts apply"
        )


def evaluate_snapshot(
    snapshot: ReplacementSnapshot,
    tree: GameSourceTree,
    *,
    game_vas: frozenset[int] | None = None,
    game_total: int | None = None,
    verdict_vas: frozenset[int] | None = None,
    xbe_sha256: str | None = None,
) -> ReplacementEvaluation:
    """Replay the snapshot's verdicts over the working tree's registrations.

    `game_vas` / `game_total` / `verdict_vas` come from the live split when it exists and
    are otherwise the snapshot's pinned values. Raises `ValueError` for a wrong XBE.
    """
    check_snapshot_oracle(snapshot, xbe_sha256)
    headers_changed = snapshot.headers_digest != tree.headers_digest
    by_va = {entry.function.va: entry for entry in snapshot.entries}
    proven: list[ReplacementFunction] = []
    failures: list[GateFailure] = []
    stale: list[StaleReplacement] = []
    for va in sorted(tree.registrations):
        source, name = tree.registrations[va]
        entry = by_va.get(va)
        if entry is None:
            stale.append(
                StaleReplacement(
                    ReplacementFunction(va, name, "", 0, (), source),
                    "new registration, not in the snapshot",
                )
            )
        elif tree.fingerprint(source) != entry.fingerprint or source != entry.function.source:
            reason = (
                "src/game headers changed since the snapshot"
                if headers_changed
                else f"{source} changed since the snapshot"
            )
            stale.append(StaleReplacement(entry.function, reason))
        elif (
            entry.proof.fp_scalar is not None
            and entry.proof.fp_scalar.get("matrix_sha256") != tree.fp_matrix_sha256
        ):
            stale.append(
                StaleReplacement(
                    entry.function, "fp-scalar matrix result changed since the snapshot"
                )
            )
        else:
            is_game = entry.is_game if game_vas is None else va in game_vas
            gate = first_failing_gate(entry.function, entry.proof, is_game=is_game)
            if gate is None:
                proven.append(entry.function)
            else:
                failures.append(GateFailure(entry.function, gate))
    for va, entry in sorted(by_va.items()):
        if va not in tree.registrations:
            stale.append(StaleReplacement(entry.function, "no longer registered in src/game"))
    judgeable = snapshot.judgeable
    if game_vas is not None and verdict_vas is not None:
        judgeable = len(game_vas & verdict_vas)
    notes: list[str] = []
    if game_total is not None and game_total != snapshot.game_total:
        notes.append(
            f"snapshot game_total {snapshot.game_total} differs from the live {game_total}: "
            f"regenerate with {SNAPSHOT_COMMAND}"
        )
    return ReplacementEvaluation(
        game_total=snapshot.game_total if game_total is None else game_total,
        registered=len(tree.registrations),
        proven=tuple(proven),
        failures=tuple(failures),
        judgeable=judgeable,
        judgeable_unknown="" if judgeable is not None else "not recorded in the snapshot",
        source="tracked snapshot",
        snapshot=snapshot,
        stale=tuple(stale),
        cross_check=tuple(notes),
        game_total_pinned=game_total is None,
        xbe_verified=xbe_sha256 is not None,
    )


def cross_check_snapshot(
    snapshot: ReplacementSnapshot,
    manifest: ReplacementManifest,
    proof: ReplacementProof,
    *,
    game_vas: frozenset[int] | None,
    xbe_sha256: str | None,
) -> tuple[str, ...]:
    """Compare the tracked snapshot with the live proof. Live wins; disagreement is LOUD.

    A snapshot for the same manifest_sha must agree field for field. A different
    manifest_sha only means the snapshot lags the proof (regenerate it), which is said
    plainly but is not a disagreement.
    """
    try:
        check_snapshot_oracle(snapshot, xbe_sha256)
    except ValueError as error:
        return (f"{DISAGREEMENT}: {error}",)
    if snapshot.manifest_sha != manifest.manifest_sha:
        return (
            f"tracked snapshot lags the live proof (snapshot manifest {snapshot.manifest_sha[:12]}"
            f", live {manifest.manifest_sha[:12]}): regenerate it with {SNAPSHOT_COMMAND}",
        )
    live_proof = {entry.va: entry for entry in proof.entries}
    live_functions = {function.va: function for function in manifest.functions}
    messages: list[str] = []
    for entry in snapshot.entries:
        va = entry.function.va
        live = live_proof.get(va)
        function = live_functions.get(va)
        if live is None or function is None:
            messages.append(f"{DISAGREEMENT}: {va:#010x} is in the snapshot but not the live proof")
            continue
        changed = [
            name
            for name in ProofEntry.__dataclass_fields__
            if name != "unjudgeable" and getattr(live, name) != getattr(entry.proof, name)
        ]
        if function != entry.function:
            changed.append("manifest registration")
        if game_vas is not None and (va in game_vas) != entry.is_game:
            changed.append("is_game")
        if changed:
            messages.append(
                f"{DISAGREEMENT}: {va:#010x} {entry.function.name}: snapshot differs from the "
                f"live proof in {', '.join(changed)}"
            )
    return tuple(messages)


@dataclass(frozen=True)
class ReplacementOutcome:
    """Either an evaluation, or the reason there is none."""

    evaluation: ReplacementEvaluation | None
    #: Artifacts that are absent, named as `Artifacts.missing` names them.
    missing: tuple[str, ...]
    #: Why an artifact that IS present is not believed (unreadable, or stale).
    reason: str


def _measure_live(
    inputs: Inputs, classified: tuple[Classified, ...] | None, game_total: int | None
) -> ReplacementOutcome:
    """Evaluate the replacement metric, or say exactly why it cannot be measured.

    `classified` and `game_total` are `None` when the library/game split could not be
    evidenced, in which case the denominator does not exist and the metric is unknown.
    """
    missing: list[str] = []
    reasons: list[str] = []
    if inputs.replace_manifest is None:
        if inputs.replace_manifest_error is not None:
            reasons.append(inputs.replace_manifest_error)
        else:
            missing.append(ARTIFACT_REPLACE_MANIFEST)
    if inputs.replace_proof is None:
        if inputs.replace_proof_error is not None:
            reasons.append(inputs.replace_proof_error)
        else:
            missing.append(ARTIFACT_REPLACE_PROOF)
    if inputs.replace_manifest is not None and inputs.replace_proof is not None:
        if inputs.replace_proof.manifest_sha != inputs.replace_manifest.manifest_sha:
            reasons.append(
                "the proof is STALE: it was run against manifest "
                f"{inputs.replace_proof.manifest_sha[:12]} but the manifest is "
                f"{inputs.replace_manifest.manifest_sha[:12]}, so none of its verdicts apply"
            )
    if inputs.replace_proof is not None:
        partial = [e for e in inputs.replace_proof.entries if PARTIAL_PROOF_MARK in e.unjudgeable]
        if partial:
            reasons.append(
                f"the proof is PARTIAL: {len(partial)} entries were skipped by --only-va, so it "
                "is a single-function debug run and not a whole-registry measurement"
            )
    if (
        classified is None
        or game_total is None
        or missing
        or reasons
        or inputs.replace_manifest is None
        or inputs.replace_proof is None
    ):
        return ReplacementOutcome(None, tuple(missing), "; ".join(reasons))

    unknown = ""
    if inputs.harness_verdict_vas is None:
        unknown = inputs.harness_error or f"{ARTIFACT_HARNESS_RESULTS} absent"
    game_vas = frozenset(
        item.function.entry_va for item in classified if item.region == REGION_GAME
    )
    evaluation = evaluate_replacements(
        inputs.replace_manifest,
        inputs.replace_proof,
        game_vas=game_vas,
        game_total=game_total,
        verdict_vas=inputs.harness_verdict_vas,
        judgeable_unknown=unknown,
    )
    return ReplacementOutcome(evaluation, (), "")


#: Metric note marker for a snapshot that is too old to replay (T1462 follow-up, 2026-10-07).
STALE_SNAPSHOT_MARK = "proof snapshot stale"
STALE_SNAPSHOT_ADVICE = "run the proof refresh (docs/replace-proof-whole-registry.md Snapshot step)"


def _is_changed_fingerprint(reason: str) -> bool:
    """A stale reason that means an existing snapshot function's fingerprint no longer matches."""
    return "changed since the snapshot" in reason


def stale_snapshot_reason(snapshot: ReplacementSnapshot, why: str) -> str:
    """The text of a not-measurable-from-snapshot metric: why, the advice and the last value."""
    proven = sum(1 for entry in snapshot.entries if entry.gate is None)
    return (
        f"{STALE_SNAPSHOT_MARK}: {STALE_SNAPSHOT_ADVICE}. {why}. Last measured value: "
        f"{format_count(proven)} of {format_count(snapshot.game_total)} game functions proven on "
        f"{snapshot.measured_on} (commit {snapshot.commit}, manifest_sha "
        f"{snapshot.manifest_sha[:12]}). That value is not carried forward as a measurement"
    )


def snapshot_header_stale(inputs: Inputs) -> str | None:
    """A live manifest present beside a snapshot that names a different manifest_sha.

    The live proof was not believed (otherwise `measure_replacement` would not be here), and the
    snapshot was proven against another registry, so neither can be replayed.
    """
    snapshot = inputs.replace_snapshot
    manifest = inputs.replace_manifest
    if snapshot is None or manifest is None or snapshot.manifest_sha == manifest.manifest_sha:
        return None
    return stale_snapshot_reason(
        snapshot,
        f"the snapshot was proven against manifest {snapshot.manifest_sha[:12]} but the live "
        f"manifest is {manifest.manifest_sha[:12]}",
    )


def measure_replacement(
    inputs: Inputs, classified: tuple[Classified, ...] | None, game_total: int | None
) -> ReplacementOutcome:
    """Live proof when present and believed, else the tracked snapshot, else the reason.

    The live artifacts take precedence and the snapshot is cross-checked against them. When
    they are absent (clean clone, CI) or not believed, the snapshot's verdicts are replayed
    over the CURRENT working tree: this never carries a previous number, it recomputes one.
    """
    live = _measure_live(inputs, classified, game_total)
    game_vas = (
        frozenset(item.function.entry_va for item in classified if item.region == REGION_GAME)
        if classified is not None
        else None
    )
    if live.evaluation is not None:
        if inputs.replace_snapshot is not None and inputs.replace_manifest and inputs.replace_proof:
            lines = cross_check_snapshot(
                inputs.replace_snapshot,
                inputs.replace_manifest,
                inputs.replace_proof,
                game_vas=game_vas,
                xbe_sha256=inputs.xbe_sha256,
            )
        elif inputs.replace_snapshot_error is not None:
            lines = (f"{DISAGREEMENT}: tracked snapshot rejected: {inputs.replace_snapshot_error}",)
        else:
            lines = ()
        evaluation = dataclasses.replace(live.evaluation, cross_check=lines)
        return ReplacementOutcome(evaluation, (), "")
    snapshot_reason = inputs.replace_snapshot_error or inputs.game_tree_error
    if inputs.replace_snapshot is None or inputs.game_tree is None:
        reason = "; ".join(r for r in (live.reason, snapshot_reason) if r)
        return ReplacementOutcome(None, live.missing, reason)
    stale_header = snapshot_header_stale(inputs)
    if stale_header is not None:
        return ReplacementOutcome(None, (ARTIFACT_REPLACE_PROOF,), stale_header)
    try:
        evaluation = evaluate_snapshot(
            inputs.replace_snapshot,
            inputs.game_tree,
            game_vas=game_vas,
            game_total=game_total,
            verdict_vas=inputs.harness_verdict_vas,
            xbe_sha256=inputs.xbe_sha256,
        )
    except ValueError as error:
        return ReplacementOutcome(
            None,
            (ARTIFACT_REPLACE_PROOF,),
            stale_snapshot_reason(
                inputs.replace_snapshot, f"the snapshot's oracle header disagrees: {error}"
            ),
        )
    changed = sum(1 for item in evaluation.stale if _is_changed_fingerprint(item.reason))
    if changed * 2 > len(inputs.replace_snapshot.entries):
        return ReplacementOutcome(
            None,
            (ARTIFACT_REPLACE_PROOF,),
            stale_snapshot_reason(
                inputs.replace_snapshot,
                f"{changed} of {len(inputs.replace_snapshot.entries)} snapshot functions changed "
                "fingerprint (more than half), so an exact replay would report a number that "
                "measures the edit rather than the proofs"
                + (
                    "; the src/game headers changed"
                    if inputs.replace_snapshot.headers_digest != inputs.game_tree.headers_digest
                    else ""
                ),
            ),
        )
    why_not_live = live.reason or (
        "the live artifacts are absent: " + ", ".join(live.missing) if live.missing else ""
    )
    return ReplacementOutcome(dataclasses.replace(evaluation, source_note=why_not_live), (), "")


# --------------------------------------------------------------------------- #
# Metrics.
# --------------------------------------------------------------------------- #

COLOUR_UNKNOWN = "#9f9f9f"
COLOUR_INFORMATIONAL = "#007ec6"

#: (inclusive lower bound, colour), descending. A percentage picks the first bound
#: it reaches. Thresholds are part of the committed output, so changing one churns
#: every badge it moves -- which is why there is a test pinning each boundary.
COLOUR_THRESHOLDS: tuple[tuple[float, str], ...] = (
    (90.0, "#4c1"),  # bright green
    (75.0, "#97ca00"),  # green
    (50.0, "#a4a61d"),  # yellow-green
    (25.0, "#dfb317"),  # yellow
    (10.0, "#fe7d37"),  # orange
    (0.0, "#e05d44"),  # red, and 0% lands here rather than in grey
)

UNKNOWN_VALUE = "unknown"


def colour_for_percent(percent: float) -> str:
    """Flat-badge colour for a percentage, graded by value."""
    for threshold, colour in COLOUR_THRESHOLDS:
        if percent >= threshold:
            return colour
    return COLOUR_THRESHOLDS[-1][1]


def format_percent(percent: float, *, places: int = 1) -> str:
    """A percentage as badge text. Fixed places, so output is stable."""
    return f"{percent:.{places}f}%"


def format_count(count: int) -> str:
    """Thousands-separated, because these numbers are read by people."""
    return f"{count:,}"


@dataclass(frozen=True)
class Metric:
    """One tracked number, or an explicit absence of one."""

    key: str
    """Badge filename stem and `metrics.json` key."""

    label: str
    """Badge left-hand text."""

    value: str | None
    """Badge right-hand text. `None` renders as `unknown` in grey."""

    note: str
    """What the number means, and any caveat that must travel with it."""

    numerator: int | None = None
    denominator: int | None = None
    percent: float | None = None
    requires: tuple[str, ...] = ()
    missing: tuple[str, ...] = ()
    unknown_reason: str = ""
    """Why an artifact that is PRESENT is not believed (unreadable, stale). Empty otherwise."""

    @property
    def is_unknown(self) -> bool:
        return self.value is None

    @property
    def display_value(self) -> str:
        return UNKNOWN_VALUE if self.value is None else self.value

    @property
    def colour(self) -> str:
        """Grey when unknown, informational blue when there is no percentage."""
        if self.is_unknown:
            return COLOUR_UNKNOWN
        if self.percent is None:
            return COLOUR_INFORMATIONAL
        return colour_for_percent(self.percent)


def _ratio_metric(
    *,
    key: str,
    label: str,
    note: str,
    requires: tuple[str, ...],
    missing: tuple[str, ...],
    numerator: int | None,
    denominator: int | None,
    value: str | None = None,
) -> Metric:
    """A metric that is `unknown` whenever anything it needs is absent.

    This is the single place the unknown rule is implemented, so there is no path
    that can quietly substitute a previous value: a `None` input cannot become a
    number here.
    """
    if missing or numerator is None or denominator is None or denominator == 0:
        return Metric(
            key=key,
            label=label,
            value=None,
            note=note,
            requires=requires,
            missing=missing,
        )
    percent = 100.0 * numerator / denominator
    return Metric(
        key=key,
        label=label,
        value=value if value is not None else format_percent(percent),
        note=note,
        numerator=numerator,
        denominator=denominator,
        percent=percent,
        requires=requires,
    )


def build_metrics(inputs: Inputs) -> tuple[Metric, ...]:
    """Every tracked metric, in badge order.

    A metric whose artifacts are absent is returned with `value=None`. Nothing
    here reads a previous run's output, so there is no mechanism by which a stale
    number could survive into a new report.
    """
    needed_for_split = _absent(
        inputs,
        (
            (ARTIFACT_FUNCTIONS, inputs.functions),
            (ARTIFACT_XBE, inputs.text_lo),
            (ARTIFACT_FLIRT, inputs.flirt_addresses),
        ),
    )

    split: NamingSplit | None = None
    classified_split: tuple[Classified, ...] | None = None
    coverage: TextCoverage | None = None
    library_overlay_note = ""
    if (
        not needed_for_split
        and inputs.functions is not None
        and inputs.text_lo is not None
        and inputs.text_hi is not None
        and inputs.flirt_addresses is not None
    ):
        classified_split = classify_functions(
            inputs.functions,
            text_lo=inputs.text_lo,
            text_hi=inputs.text_hi,
            xtlid_addresses=inputs.xtlid_addresses or frozenset(),
            flirt_addresses=inputs.flirt_addresses,
            class_overrides=inputs.class_overrides,
        )
        classified_split = apply_overlays(
            classified_split, inputs.name_overlay, inputs.library_name_overlay
        )
        if inputs.library_name_overlay is not None:
            library_overlay_note = (
                f" Includes the library-name overlay {inputs.library_name_overlay.path} "
                f"({inputs.library_name_overlay.row_count} rows, "
                f"{len(inputs.library_name_overlay.rows)} accepted, sha256 "
                f"{inputs.library_name_overlay.sha256[:12]}): names INFERRED or MEASURED per row, "
                "counted toward library naming only, never toward game naming. The library "
                "denominator is fixed by classification and does not move."
            )
        split = naming_split(classified_split)
    if inputs.functions is not None and inputs.text_lo is not None and inputs.text_hi is not None:
        coverage = text_coverage(inputs.functions, text_lo=inputs.text_lo, text_hi=inputs.text_hi)

    metrics: list[Metric] = []

    # 1. Functions defined, whole binary on both sides.
    defined_missing = _absent(inputs, ((ARTIFACT_FUNCTIONS, inputs.functions),))
    recovered = len(inputs.functions) if inputs.functions is not None else None
    exported = len(inputs.export_functions) if inputs.export_functions is not None else None
    added = recovered - exported if recovered is not None and exported is not None else 0
    before_total = None
    before_claimed = None
    if (
        added
        and inputs.export_functions is not None
        and inputs.text_lo is not None
        and inputs.text_hi is not None
        and inputs.flirt_addresses is not None
    ):
        before_split = naming_split(
            classify_functions(
                inputs.export_functions,
                text_lo=inputs.text_lo,
                text_hi=inputs.text_hi,
                xtlid_addresses=inputs.xtlid_addresses or frozenset(),
                flirt_addresses=inputs.flirt_addresses,
                class_overrides=inputs.class_overrides,
            )
        )
        before_total = before_split.game_total
        before_claimed = text_coverage(
            inputs.export_functions, text_lo=inputs.text_lo, text_hi=inputs.text_hi
        )
    additions_note = (
        f" DENOMINATOR CHANGE (T1266): {format_count(added)} entries were added to the "
        f"{format_count(exported)}-row Ghidra export through tools/data/function_overrides.csv "
        "and tools/data/function_additions.csv (code reached only through data tables, each "
        "verified by tools/verify_function_additions.py). Totals before and after are not "
        "comparable without this."
        if added
        else ""
    )
    metrics.append(
        _ratio_metric(
            key="functions-defined",
            label="functions recovered",
            note=(
                f"{format_count(recovered)} recovered of ~{format_count(inputs.predicted_total)} "
                "predicted by the PS2 JR $ra census. WHOLE-BINARY on both sides: each side "
                "includes its own platform's statically linked SDK."
                + additions_note
                + (
                    " The PS2 census is therefore no longer an upper bound for this table and the "
                    "percentage can exceed 100%."
                    if recovered is not None and recovered > inputs.predicted_total
                    else ""
                )
                if recovered is not None
                else "No function table present."
            ),
            requires=(ARTIFACT_FUNCTIONS,),
            missing=defined_missing,
            numerator=recovered,
            denominator=inputs.predicted_total,
            value=(
                (
                    f"{format_count(recovered)} ({format_count(exported)} export + "
                    f"{format_count(added)} added)"
                    if added
                    else f"{format_count(recovered)} / ~{format_count(inputs.predicted_total)}"
                )
                if recovered is not None
                else None
            ),
        )
    )

    # 2. `.text` claimed.
    coverage_missing = _absent(
        inputs, ((ARTIFACT_FUNCTIONS, inputs.functions), (ARTIFACT_XBE, inputs.text_lo))
    )
    metrics.append(
        _ratio_metric(
            key="text-claimed",
            label=".text claimed",
            note=(
                f"{format_count(coverage.claimed_bytes)} of "
                f"{format_count(coverage.total_bytes)} bytes sit inside a recovered function "
                f"body extent; {format_count(coverage.unclaimed_bytes)} bytes unclaimed. "
                "Spans are clipped to .text before merging."
                + (
                    f" DENOMINATOR/NUMERATOR CHANGE (T1266): before the verified additions "
                    f"{format_count(before_claimed.claimed_bytes)} of "
                    f"{format_count(before_claimed.total_bytes)} bytes were claimed; the .text "
                    "size is unchanged, the claimed bytes grew."
                    if before_claimed is not None
                    else ""
                )
                if coverage is not None
                else "Needs a function table and the image's section layout."
            ),
            requires=(ARTIFACT_FUNCTIONS, ARTIFACT_XBE),
            missing=coverage_missing,
            numerator=coverage.claimed_bytes if coverage is not None else None,
            denominator=coverage.total_bytes if coverage is not None else None,
        )
    )

    # 3. Naming, split. Game code first, because it is the real number.
    metrics.append(
        _ratio_metric(
            key="naming-game",
            label="game code named",
            note=(
                f"{format_count(split.game_named)} of {format_count(split.game_total)} "
                "Free Radical functions in .text carry a recovered name. This is the "
                "decompilation target; library names are excluded and reported separately."
                + (
                    f" DENOMINATOR CHANGE (T1266): the game total was {format_count(before_total)} "
                    f"before {format_count(added)} table additions (see functions recovered); the "
                    "percentage is not comparable across that change."
                    if before_total is not None
                    else ""
                )
                if split is not None
                else "Needs the function table, the section layout and the FLIRT proposals, "
                "because the library/game split is evidenced per function."
            ),
            requires=(ARTIFACT_FUNCTIONS, ARTIFACT_XBE, ARTIFACT_FLIRT),
            missing=needed_for_split,
            numerator=split.game_named if split is not None else None,
            denominator=split.game_total if split is not None else None,
            value=(
                f"{format_percent(split.game_percent, places=2)} "
                f"({format_count(split.game_named)}/{format_count(split.game_total)})"
                if split is not None
                else None
            ),
        )
    )
    metrics.append(
        _ratio_metric(
            key="naming-library",
            label="library code named",
            note=(
                f"{format_count(split.library_named)} of {format_count(split.library_total)} "
                f"XDK/CRT functions named ({format_count(split.library_outside_text)} in linked "
                f"XDK sections, {format_count(split.library_in_text)} inside .text). NOT the "
                "decompilation target: this is code we identify rather than rewrite."
                + library_overlay_note
                if split is not None
                else "Needs the same three artifacts as the game-code split."
            ),
            requires=(ARTIFACT_FUNCTIONS, ARTIFACT_XBE, ARTIFACT_FLIRT),
            missing=needed_for_split,
            numerator=split.library_named if split is not None else None,
            denominator=split.library_total if split is not None else None,
        )
    )

    # 4. Decompiler-clean.
    quality_missing = _absent(inputs, ((ARTIFACT_QUALITY, inputs.quality),))
    quality = inputs.quality
    metrics.append(
        _ratio_metric(
            key="decompiler-clean",
            label="decompiler-clean",
            note=(
                f"{format_count(quality.clean)} of {format_count(quality.total)} decompiled "
                "functions carry no unaff_*, in_* or control-flow marker. Measured with "
                "Decompiler Parameter ID enabled, which is the default."
                if quality is not None and quality.total
                else "Needs a decompiled C export to scan for markers; none present."
            ),
            requires=(ARTIFACT_QUALITY,),
            missing=quality_missing,
            numerator=quality.clean if quality is not None else None,
            denominator=quality.total if quality is not None else None,
        )
    )

    # 5. Kernel HLE, against what the title actually imports.
    hle_missing = _absent(
        inputs,
        (
            (ARTIFACT_XBE, inputs.imported_ordinals),
            (ARTIFACT_SOURCE_TREE, inputs.implemented_ordinals),
        ),
    )
    metrics.append(
        _ratio_metric(
            key="kernel-hle",
            label="kernel ordinals",
            note=(
                f"{format_count(inputs.implemented_ordinals)} of "
                f"{format_count(inputs.imported_ordinals)} ordinals TSFP actually imports are "
                "implemented. Measured against the import list, not the 371-entry table: the "
                "220 ordinals the title never calls are not work."
                + (
                    " Registered ordinals the title does not import are excluded from the "
                    "count: " + ", ".join(str(o) for o in inputs.ordinals_not_imported) + "."
                    if inputs.ordinals_not_imported
                    else ""
                )
                if inputs.implemented_ordinals is not None and inputs.imported_ordinals is not None
                else "Needs the image's kernel thunk table and src/xbox."
            ),
            requires=(ARTIFACT_XBE, ARTIFACT_SOURCE_TREE),
            missing=hle_missing,
            numerator=inputs.implemented_ordinals,
            denominator=inputs.imported_ordinals,
            value=(
                f"{format_count(inputs.implemented_ordinals)} / "
                f"{format_count(inputs.imported_ordinals)}"
                if inputs.implemented_ordinals is not None and inputs.imported_ordinals is not None
                else None
            ),
        )
    )

    # 6. XDK call surface.
    surface_total = sum(inputs.xdk_surface.values()) if inputs.xdk_surface is not None else None
    surface_missing = _absent(
        inputs,
        (
            (ARTIFACT_XBE, inputs.xdk_surface),
            (ARTIFACT_XDK_IMPLEMENTATIONS, inputs.xdk_implemented),
        ),
    )
    breakdown = (
        ", ".join(
            f"{section} {count}"
            for section, count in sorted(
                inputs.xdk_surface.items(), key=lambda item: (-item[1], item[0])
            )
        )
        if inputs.xdk_surface
        else ""
    )
    metrics.append(
        _ratio_metric(
            key="xdk-surface",
            label="XDK call surface",
            note=(
                "Needs the image, to recover the call surface."
                if surface_total is None
                else (
                    "Needs a verified XDK implementation registry measurement; "
                    "partial handler registrations do not establish fully implemented entries. "
                    f"Recovered call surface: {format_count(surface_total)} functions. "
                    f"Breakdown: {breakdown}. "
                    "Counts are upper bounds recovered by a call-target scan."
                    if inputs.xdk_implemented is None
                    else (
                        f"{format_count(inputs.xdk_implemented)} of {format_count(surface_total)} "
                        "distinct XDK functions the game calls are implemented. "
                        f"Breakdown: {breakdown}. "
                        "Counts are upper bounds recovered by a call-target scan."
                    )
                )
            ),
            requires=(ARTIFACT_XBE, ARTIFACT_XDK_IMPLEMENTATIONS),
            missing=surface_missing,
            numerator=inputs.xdk_implemented,
            denominator=surface_total,
            value=(
                f"{format_count(inputs.xdk_implemented)} / {format_count(surface_total)}"
                if surface_total is not None and inputs.xdk_implemented is not None
                else None
            ),
        )
    )

    # 7. Tests. No denominator exists, so this is informational rather than graded.
    if inputs.python_tests is None or inputs.c_suites is None:
        tests = Metric(
            key="tests",
            label="tests",
            value=None,
            note="Needs a runnable pytest and CMakeLists.txt to count suites.",
            requires=(ARTIFACT_SOURCE_TREE,),
            missing=(ARTIFACT_SOURCE_TREE,),
        )
    else:
        tests = Metric(
            key="tests",
            label="tests",
            value=f"{format_count(inputs.python_tests)} Python + {inputs.c_suites} C",
            note=(
                f"{format_count(inputs.python_tests)} collected Python tests and "
                f"{inputs.c_suites} C suites declared to CTest. No denominator exists, so this "
                "is reported rather than graded."
            ),
            numerator=inputs.python_tests,
            requires=(ARTIFACT_SOURCE_TREE,),
        )
    metrics.append(tests)

    # 8. Decompilation: hand-written replacements proven equivalent. APPENDED, so no
    # existing metric changes position. See the section comment above for the null model.
    proven_metric = replacement_metric(
        measure_replacement(
            inputs, classified_split, split.game_total if split is not None else None
        ),
        split_missing=needed_for_split,
    )
    if before_total is not None and proven_metric.value is not None:
        proven_metric = dataclasses.replace(
            proven_metric,
            note=proven_metric.note
            + f" DENOMINATOR CHANGE (T1266): the game total was {format_count(before_total)} "
            f"before {format_count(added)} table additions, so the percentage is not comparable "
            "across that change.",
        )
    metrics.append(proven_metric)

    return tuple(metrics)


PARTIAL_PROOF_MARK = "--only-va"
REPLACEMENT_KEY = "decompiled-proven"
REPLACEMENT_LABEL = "hand-decompiled & proven"


def replacement_source_text(evaluation: ReplacementEvaluation) -> str:
    """Which evidence the number came from. Part of the metric: honesty about the source."""
    snapshot = evaluation.snapshot
    if snapshot is None or evaluation.source != "tracked snapshot":
        extra = (
            f" Tracked snapshot cross-check: {len(evaluation.cross_check)} finding(s), see the "
            "summary."
            if evaluation.cross_check
            else ""
        )
        return (
            "MEASURED from generated/replace/manifest.json and proof.json, which must carry the "
            "same manifest_sha." + extra
        )
    changed = sum(1 for item in evaluation.stale if "changed" in item.reason)
    new = sum(1 for item in evaluation.stale if item.reason.startswith("new"))
    dropped = sum(1 for item in evaluation.stale if item.reason.startswith("no longer"))
    return (
        "REPLAYED from the tracked docs/data/replace-proof-snapshot.json "
        f"(measured_on {snapshot.measured_on}, commit {snapshot.commit}, manifest_sha "
        f"{snapshot.manifest_sha[:12]}), not from a live harness run: only the harness verdicts "
        "are replayed, while the registered set and every source fingerprint are recomputed "
        f"from src/game now. {format_count(changed)} registered functions changed source and "
        f"{format_count(new)} are new since the snapshot, and count as NOT proven; "
        f"{format_count(dropped)} snapshot entries are no longer registered. "
        + (
            "The game total is PINNED from the snapshot and not recomputed here "
            "(the function table is absent). "
            if evaluation.game_total_pinned
            else "The game total is measured live. "
        )
        + (
            "The XBE sha256 matches the snapshot's. "
            if evaluation.xbe_verified
            else f"The oracle XBE (sha256 {snapshot.xbe_sha256[:12]}) is absent here, so its "
            "identity is pinned by the snapshot and not re-verified. "
        )
        + (
            f"The live proof was not used: {evaluation.source_note}. "
            if evaluation.source_note
            else ""
        )
        + (" ".join(evaluation.cross_check) if evaluation.cross_check else "")
    ).rstrip()


def replacement_note(evaluation: ReplacementEvaluation) -> str:
    """The metric's detail text. Counts only: it is committed in `metrics.json`."""
    failing = ", ".join(f"{gate} {count}" for gate, count in evaluation.failure_counts)
    if evaluation.judgeable is not None:
        judgeable = (
            f"{format_count(evaluation.judgeable)} of {format_count(evaluation.game_total)} "
            "game functions have at least one harness verdict (an UPPER BOUND on what the "
            "harness can judge, because near-vacuous verdicts are included)"
        )
    else:
        judgeable = f"unknown ({evaluation.judgeable_unknown})"
    return (
        f"{format_count(len(evaluation.proven))} of {format_count(evaluation.game_total)} game "
        "functions are hand-written C and proven equivalent to the original by the differential "
        f"harness in replacement mode. {format_count(evaluation.registered)} are registered; "
        + (
            f"the rest failed, counted by FIRST failing gate: {failing}. "
            if failing
            else "none failed a gate. "
        )
        + replacement_source_text(evaluation)
        + f" Judgeable by the harness: {judgeable}. "
        "Null model: a do-nothing function returning a constant, held back by the "
        f"null_agree_rate gate (<= {MAX_NULL_AGREE}), scores zero proven functions. The "
        "percentage counts functions, not bytes or behaviour, so it must not be compared with "
        "the naming percentages."
    )


def replacement_metric(outcome: ReplacementOutcome, *, split_missing: tuple[str, ...]) -> Metric:
    """The decompilation metric: `unknown` unless it was measured from both artifacts."""
    requires = (
        ARTIFACT_REPLACE_MANIFEST,
        ARTIFACT_REPLACE_PROOF,
        ARTIFACT_FUNCTIONS,
        ARTIFACT_XBE,
        ARTIFACT_FLIRT,
    )
    evaluation = outcome.evaluation
    if evaluation is None:
        missing = (*split_missing, *outcome.missing)
        reason = outcome.reason
        return Metric(
            key=REPLACEMENT_KEY,
            label=REPLACEMENT_LABEL,
            value=None,
            note=(
                "Needs a replacement manifest, the harness proof for that same manifest, and "
                "the game/library split. Never carried forward from an earlier run."
                + (f" Not measured because {reason}." if reason else "")
            ),
            requires=requires,
            missing=missing,
            unknown_reason=reason,
        )
    return _ratio_metric(
        key=REPLACEMENT_KEY,
        label=REPLACEMENT_LABEL,
        note=replacement_note(evaluation),
        requires=requires,
        missing=(),
        numerator=len(evaluation.proven),
        denominator=evaluation.game_total,
        value=(
            f"{format_percent(100.0 * len(evaluation.proven) / evaluation.game_total, places=2)} "
            f"({format_count(len(evaluation.proven))}/{format_count(evaluation.game_total)})"
            if evaluation.game_total
            else None
        ),
    )


def _absent(inputs: Inputs, required: Sequence[tuple[str, object]]) -> tuple[str, ...]:
    """Names of the required artifacts whose loaded value is `None`."""
    del inputs
    return tuple(name for name, value in required if value is None)


@dataclass(frozen=True)
class CoverageReport:
    """Everything one measurement run produced."""

    metrics: tuple[Metric, ...]
    classified: tuple[Classified, ...]
    naming: NamingSplit | None
    coverage: TextCoverage | None
    inputs: Inputs
    replacement: ReplacementEvaluation | None = None

    def metric(self, key: str) -> Metric:
        for metric in self.metrics:
            if metric.key == key:
                return metric
        raise KeyError(key)

    @property
    def unknown_metrics(self) -> tuple[Metric, ...]:
        return tuple(metric for metric in self.metrics if metric.is_unknown)

    @property
    def missing_artifacts(self) -> tuple[str, ...]:
        absent: list[str] = []
        for metric in self.metrics:
            for name in metric.missing:
                if name not in absent:
                    absent.append(name)
        return tuple(absent)


def build_report(inputs: Inputs) -> CoverageReport:
    """Compute every metric plus the per-function detail the backlog needs."""
    classified: tuple[Classified, ...] = ()
    split: NamingSplit | None = None
    coverage: TextCoverage | None = None
    if inputs.functions is not None and inputs.text_lo is not None and inputs.text_hi is not None:
        if inputs.flirt_addresses is not None:
            classified = classify_functions(
                inputs.functions,
                text_lo=inputs.text_lo,
                text_hi=inputs.text_hi,
                xtlid_addresses=inputs.xtlid_addresses or frozenset(),
                flirt_addresses=inputs.flirt_addresses,
                class_overrides=inputs.class_overrides,
            )
            classified = apply_overlays(
                classified, inputs.name_overlay, inputs.library_name_overlay
            )
            split = naming_split(classified)
        coverage = text_coverage(inputs.functions, text_lo=inputs.text_lo, text_hi=inputs.text_hi)
    return CoverageReport(
        metrics=build_metrics(inputs),
        classified=classified,
        naming=split,
        coverage=coverage,
        inputs=inputs,
        replacement=measure_replacement(
            inputs, classified if split is not None else None, split.game_total if split else None
        ).evaluation,
    )


# --------------------------------------------------------------------------- #
# Badge rendering. Self-contained SVG, no external service, no timestamp.
# --------------------------------------------------------------------------- #

BADGE_HEIGHT = 20
BADGE_PADDING = 10
BADGE_FONT = "Verdana,Geneva,DejaVu Sans,sans-serif"
BADGE_FONT_SIZE = 11

#: Per-character advance widths at the badge font size, in whole pixels. A real
#: font metric table is not available without a font dependency, and these badges
#: must be generated by a fresh clone with nothing installed, so this approximates
#: it in three buckets. Being approximate is fine; being DETERMINISTIC is not
#: optional, since a committed SVG that shifts by a pixel churns the diff.
_NARROW_CHARS = frozenset(" .,:;'\"`|!ijlt()[]{}-/\\")
_WIDE_CHARS = frozenset("@MWmw%&")


def text_width(text: str) -> int:
    """Approximate rendered width of `text` in pixels. Pure and integral."""
    total = 0
    for character in text:
        if character in _NARROW_CHARS:
            total += 4
        elif character in _WIDE_CHARS:
            total += 10
        else:
            total += 7
    return total


def render_badge(label: str, value: str, colour: str) -> str:
    """A flat label/value badge as a self-contained SVG document.

    Pure: the same arguments always produce byte-identical output. There is
    deliberately no generation timestamp, no version string and no random id
    inside the document, because any of those would make every regeneration a
    diff even when no measurement changed.

    No external request is made and no external service is referenced. This repo
    may live on a private self-hosted Forgejo instance, where a shields.io URL
    cannot read the repository and renders broken.
    """
    label_box = text_width(label) + 2 * BADGE_PADDING
    value_box = text_width(value) + 2 * BADGE_PADDING
    total = label_box + value_box
    # Geometry in tenths of a pixel, matching the SVG attributes below, so no
    # float formatting ever reaches the output.
    label_centre = label_box * 10 // 2
    value_centre = label_box * 10 + value_box * 10 // 2
    label_length = text_width(label) * 10
    value_length = text_width(value) * 10
    title = f"{label}: {value}"
    safe_label = escape(label)
    safe_value = escape(value)
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{total}" height="{BADGE_HEIGHT}"'
        f' role="img" aria-label={quoteattr(title)}>\n'
        f"  <title>{escape(title)}</title>\n"
        '  <linearGradient id="smooth" x2="0" y2="100%">\n'
        '    <stop offset="0" stop-color="#bbb" stop-opacity=".1"/>\n'
        '    <stop offset="1" stop-opacity=".1"/>\n'
        "  </linearGradient>\n"
        f'  <clipPath id="round"><rect width="{total}" height="{BADGE_HEIGHT}" rx="3"'
        ' fill="#fff"/></clipPath>\n'
        '  <g clip-path="url(#round)">\n'
        f'    <rect width="{label_box}" height="{BADGE_HEIGHT}" fill="#555"/>\n'
        f'    <rect x="{label_box}" width="{value_box}" height="{BADGE_HEIGHT}"'
        f' fill="{colour}"/>\n'
        f'    <rect width="{total}" height="{BADGE_HEIGHT}" fill="url(#smooth)"/>\n'
        "  </g>\n"
        # Every coordinate below is in tenths of a pixel, undone by the per-text
        # `scale(.1)`, so the font size must be scaled up to match. Emitting a
        # bare 11 here renders every glyph at 1.1px.
        f'  <g fill="#fff" text-anchor="middle" font-family="{BADGE_FONT}"'
        f' font-size="{BADGE_FONT_SIZE * 10}" text-rendering="geometricPrecision">\n'
        f'    <text x="{label_centre}" y="150" fill="#010101" fill-opacity=".3"'
        f' transform="scale(.1)" textLength="{label_length}">{safe_label}</text>\n'
        f'    <text x="{label_centre}" y="140" transform="scale(.1)"'
        f' textLength="{label_length}">{safe_label}</text>\n'
        f'    <text x="{value_centre}" y="150" fill="#010101" fill-opacity=".3"'
        f' transform="scale(.1)" textLength="{value_length}">{safe_value}</text>\n'
        f'    <text x="{value_centre}" y="140" transform="scale(.1)"'
        f' textLength="{value_length}">{safe_value}</text>\n'
        "  </g>\n"
        "</svg>\n"
    )


def render_metric_badge(metric: Metric) -> str:
    """The badge for one metric. Unknown renders the word `unknown` in grey."""
    return render_badge(metric.label, metric.display_value, metric.colour)


def badge_filename(metric: Metric) -> str:
    return f"{metric.key}.svg"


def write_badges(metrics: Iterable[Metric], out_dir: Path) -> tuple[Path, ...]:
    """Write one SVG per metric. Returns the paths, in metric order."""
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for metric in metrics:
        path = out_dir / badge_filename(metric)
        path.write_text(render_metric_badge(metric), encoding="utf-8")
        written.append(path)
    return tuple(written)


# --------------------------------------------------------------------------- #
# metrics.json -- where staleness is recorded.
# --------------------------------------------------------------------------- #

METRICS_SCHEMA = 1


def git_commit(root: Path) -> str | None:
    """Short HEAD commit, or `None` outside a work tree."""
    try:
        result = subprocess.run(  # noqa: S603 -- fixed argv, no shell
            ["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    commit = result.stdout.strip()
    return commit or None


def name_overlay_record(report: CoverageReport) -> dict[str, object] | None:
    """Overlay identity and outcome for metrics.json, or `None` when no overlay."""
    overlay = report.inputs.name_overlay
    if overlay is None:
        return None
    game = {item.function.entry_va for item in report.classified if item.region == REGION_GAME}
    return {
        "path": overlay.path,
        "sha256": overlay.sha256,
        "rows": overlay.row_count,
        "accepted": len(overlay.rows),
        "rejected": len(overlay.rejections),
        "applied_to_game": sum(1 for va in overlay.rows if va in game),
        "ignored_library": sum(1 for va in overlay.rows if va not in game),
    }


def library_name_overlay_record(report: CoverageReport) -> dict[str, object] | None:
    """Library overlay identity and outcome for metrics.json, `None` when absent."""
    overlay = report.inputs.library_name_overlay
    if overlay is None:
        return None
    library_named = {
        item.function.entry_va for item in report.classified if item.region == REGION_LIBRARY
    }
    return {
        "path": overlay.path,
        "sha256": overlay.sha256,
        "rows": overlay.row_count,
        "accepted": len(overlay.rows),
        "rejected": len(overlay.rejections),
        "counted_in_library": sum(1 for va in overlay.rows if va in library_named),
        "ignored_not_library": sum(1 for va in overlay.rows if va not in library_named),
    }


DENOMINATOR_MARK = "DENOMINATOR CHANGE"


def metric_losses(
    previous: Mapping[str, object], current: Mapping[str, object], *, denominators: bool
) -> list[str]:
    """What a regeneration would lose, as human lines. Both args are `metrics.json` metrics.

    A known value that becomes unknown, or disappears, is a loss. With `denominators`, a
    known value whose denominator moved WITHOUT the new note saying `DENOMINATOR CHANGE` is
    a loss too: the percentage is no longer comparable and nothing explains why (T1462).
    """
    lost: list[str] = []
    for key, old in sorted(previous.items()):
        if not isinstance(old, Mapping) or not old.get("known"):
            continue
        new = current.get(key)
        if not isinstance(new, Mapping):
            lost.append(f"{key}: was {old.get('value')!r}, now absent")
        elif not new.get("known"):
            lost.append(f"{key}: was {old.get('value')!r}, would become unknown")
        elif (
            denominators
            and old.get("denominator") != new.get("denominator")
            and DENOMINATOR_MARK not in str(new.get("note"))
        ):
            lost.append(
                f"{key}: denominator {old.get('denominator')} -> {new.get('denominator')} "
                f"with no '{DENOMINATOR_MARK}' note"
            )
    return lost


SNAPSHOT_REFRESH_MARK = "SNAPSHOT REFRESH"
#: A numerator drop of the proven metric above this fraction needs an explicit marker.
NUMERATOR_DROP_LIMIT = 0.10


def numerator_drops(
    previous: Mapping[str, object], current: Mapping[str, object], *, key: str = REPLACEMENT_KEY
) -> list[str]:
    """The proven metric's numerator fell by more than 10% against the base, unexplained.

    Allowed when the new note carries `DENOMINATOR CHANGE` or `SNAPSHOT REFRESH` (a deliberate,
    documented re-measurement). A known-to-unknown change is `metric_losses`, not this.
    """
    old = previous.get(key)
    new = current.get(key)
    if not isinstance(old, Mapping) or not isinstance(new, Mapping):
        return []
    before, after = old.get("numerator"), new.get("numerator")
    if not (old.get("known") and new.get("known")):
        return []
    if not isinstance(before, int) or not isinstance(after, int) or before <= 0:
        return []
    if (before - after) <= NUMERATOR_DROP_LIMIT * before:
        return []
    note = str(new.get("note"))
    if DENOMINATOR_MARK in note or SNAPSHOT_REFRESH_MARK in note:
        return []
    return [
        f"{key}: numerator fell {before} -> {after} "
        f"({100.0 * (before - after) / before:.1f}% drop, limit "
        f"{100.0 * NUMERATOR_DROP_LIMIT:.0f}%) with no '{SNAPSHOT_REFRESH_MARK}' or "
        f"'{DENOMINATOR_MARK}' marker in the note"
    ]


def metrics_document(
    report: CoverageReport,
    *,
    measured_on: date,
    commit: str | None,
    artifacts: Artifacts | None = None,
) -> dict[str, object]:
    """The `metrics.json` payload.

    Carries a DATE rather than a full timestamp: the date is what makes staleness
    visible, and a second-resolution timestamp would rewrite the committed file on
    every regeneration for no information gain.

    `measured_on` and `commit` are per metric, not just global, so a partial
    regeneration cannot leave a fresh global date vouching for an old number.
    """
    present = ()
    missing = report.missing_artifacts
    if artifacts is not None:
        present = tuple(
            name
            for name in (
                ARTIFACT_XBE,
                ARTIFACT_FUNCTIONS,
                ARTIFACT_FLIRT,
                ARTIFACT_CANDIDATES,
                ARTIFACT_QUALITY,
                ARTIFACT_REPLACE_MANIFEST,
                ARTIFACT_REPLACE_PROOF,
            )
            if name not in artifacts.missing
        )
    entries: dict[str, object] = {}
    for metric in report.metrics:
        entries[metric.key] = {
            "label": metric.label,
            "value": metric.display_value,
            "known": not metric.is_unknown,
            "numerator": metric.numerator,
            "denominator": metric.denominator,
            "percent": None if metric.percent is None else round(metric.percent, 4),
            "colour": metric.colour,
            "badge": badge_filename(metric),
            "measured_on": measured_on.isoformat(),
            "commit": commit,
            "requires": list(metric.requires),
            "artifacts_missing": list(metric.missing),
            "note": metric.note,
        }
    return {
        "schema": METRICS_SCHEMA,
        "measured_on": measured_on.isoformat(),
        "commit": commit,
        "artifacts_present": list(present),
        "artifacts_missing": list(missing),
        "unknown_metrics": [metric.key for metric in report.unknown_metrics],
        "name_overlay": name_overlay_record(report),
        "library_name_overlay": library_name_overlay_record(report),
        "generator": "tools/coverage_cli.py",
        "metrics": entries,
    }


# --------------------------------------------------------------------------- #
# The backlog: aggregates are committable, per-address lists are not.
# --------------------------------------------------------------------------- #

UNNAMED_CSV_COLUMNS = ("entry_va", "size_bytes", "classification", "region")
UNDEFINED_CSV_COLUMNS = ("start_va", "length", "preceded_by_terminator", "call_sites")


def unnamed_functions(classified: Iterable[Classified]) -> tuple[Classified, ...]:
    """Functions still carrying a placeholder name, in address order."""
    return tuple(
        sorted(
            (item for item in classified if not item.is_named),
            key=lambda item: item.function.entry_va,
        )
    )


def write_unnamed_functions_csv(path: Path, classified: Iterable[Classified]) -> int:
    """Per-address unnamed-function list. GOES TO `generated/`, NEVER COMMITTED.

    This is bulk analysis output derived from the user's own executable, and the
    repository is deliberately code-only. Only the counts are committable.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    rows = unnamed_functions(classified)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(UNNAMED_CSV_COLUMNS)
        for item in rows:
            writer.writerow(
                [
                    f"{item.function.entry_va:#010x}",
                    item.function.size_bytes,
                    item.evidence,
                    item.region,
                ]
            )
    return len(rows)


def write_undefined_regions_csv(
    path: Path,
    gaps: Iterable[Gap],
    *,
    text: bytes,
    base_va: int,
    call_sites: Mapping[int, int] | None = None,
) -> int:
    """Per-address unclaimed-`.text` list. GOES TO `generated/`, NEVER COMMITTED.

    `preceded_by_terminator` comes from `tools.codediff.boundaries`, the same gate
    measured at 98.3% on known entries against 4.6% on arbitrary addresses, and
    `call_sites` from `count_call_sites` in the same module. Neither is
    reimplemented here: a second copy of either would drift from the measurement
    that justifies it.

    `call_sites` counts `call rel32` sites targeting ANY address inside the
    region, which is evidence that called code lives in there. It is an upper
    bound for the reasons `count_call_sites` documents. Passing `None` scans
    `text` here; a caller that already has the map should pass it, since the scan
    is over a few megabytes.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    if call_sites is None:
        call_sites = count_call_sites(text, base_va)
    ordered = sorted(gaps, key=lambda gap: gap.start_va)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(UNDEFINED_CSV_COLUMNS)
        for gap in ordered:
            sites = sum(
                count for target, count in call_sites.items() if gap.start_va <= target < gap.end_va
            )
            writer.writerow(
                [
                    f"{gap.start_va:#010x}",
                    gap.length,
                    str(preceded_by_terminator(text, base_va, gap.start_va)).lower(),
                    sites,
                ]
            )
    return len(ordered)


def render_backlog(
    report: CoverageReport,
    *,
    measured_on: date,
    commit: str | None,
    min_region_bytes: int = DEFAULT_MIN_REGION_BYTES,
) -> str:
    """`docs/backlog.md`: counts and aggregates only, no per-address rows.

    The primary number is game-code functions still to name. The 11,685 total
    placeholder count is reported too, but subordinated, because 2,630 of those
    are linked XDK libraries that are nobody's decompilation work.
    """
    lines: list[str] = [
        "# Backlog: what is still to be named, and still to be defined",
        "",
        "<!-- Generated by `tools/coverage_cli.py`. Counts and aggregates only: the",
        "     per-address lists are bulk analysis output derived from the user's own",
        "     executable and live in gitignored `generated/`. -->",
        "",
        f"Measured {measured_on.isoformat()}"
        + (f" at commit `{commit}`." if commit else ", commit unknown."),
        "",
    ]

    missing = report.missing_artifacts
    if missing:
        lines += [
            "> **Artifacts missing, so some figures below are `unknown`:** "
            + ", ".join(f"`{name}`" for name in missing)
            + ".",
            "> Regenerate after producing them rather than reading a gap as a zero.",
            "",
        ]

    split = report.naming
    lines += ["## Still to name", ""]
    if split is None:
        lines += [
            "`unknown`. The library/game split needs the function table, the image's",
            "section layout and the FLIRT proposals, because it is evidenced per",
            "function rather than cut at an address. Without all three this section",
            "would be a guess, so no number is printed.",
            "",
        ]
    else:
        lines += [
            "### Game code: the decompilation target",
            "",
            f"**{format_count(split.game_unnamed)} functions still to name**, of "
            f"{format_count(split.game_total)} game functions in `.text`.",
            "",
            f"Named so far: **{format_count(split.game_named)}** "
            f"(**{format_percent(split.game_percent, places=2)}**).",
            "",
            "### Library code: identified, not rewritten",
            "",
            f"{format_count(split.library_unnamed)} of {format_count(split.library_total)} "
            f"XDK/CRT functions still unnamed "
            f"({format_percent(split.library_percent, places=1)} named).",
            "",
            f"- {format_count(split.library_outside_text)} live in linked XDK sections "
            "(D3D, XGRPH, DSOUND, XONLINE, XNET, XMV, XPP, DOLBY), outside `.text`.",
            f"- {format_count(split.library_in_text)} are inside `.text`: the CRT/XAPI tail, "
            "plus import thunks and compiler intrinsics scattered through the game region.",
            "",
            "### Why these are not added together",
            "",
            "The raw placeholder count is "
            f"{format_count(split.game_unnamed + split.library_unnamed)}, and quoting it as "
            "one figure would mix game naming progress with library identification. "
            "The split is derived per function from "
            "three independent kinds of evidence (`.XTLID` ids, FLIRT pattern matches, and "
            "existing non-placeholder names) because no clean address cutoff exists. "
            "`.XTLID`-known addresses inside `.text` cluster in the top few percent of the "
            "section, but FLIRT matches library code far below that, so a cutoff would "
            "misfile CRT helpers as game code.",
            "",
        ]

    lines += ["## Still to define", ""]
    coverage = report.coverage
    if coverage is None:
        lines += [
            "`unknown`. Needs the function table and the image's section layout.",
            "",
        ]
    else:
        regions = coverage.gaps_at_least(min_region_bytes)
        lines += [
            f"**{format_count(len(regions))} unclaimed `.text` regions of "
            f">={min_region_bytes} bytes**, holding "
            f"{format_count(sum(gap.length for gap in regions))} bytes. These are where the "
            "probably-missing functions are.",
            "",
            f"`.text` is {format_count(coverage.total_bytes)} bytes, of which "
            f"{format_count(coverage.claimed_bytes)} "
            f"({format_percent(coverage.claimed_percent)}) sits inside a recovered function "
            f"body extent and {format_count(coverage.unclaimed_bytes)} does not.",
            "",
            "| unclaimed region size | regions | bytes |",
            "|---|---|---|",
        ]
        for floor in (min_region_bytes, 64, 128, 256, 1024):
            selected = coverage.gaps_at_least(floor)
            lines.append(
                f"| >= {floor} B | {format_count(len(selected))} | "
                f"{format_count(sum(gap.length for gap in selected))} |"
            )
        lines += [
            "",
            "Counted on body *extents* (`entry_va` to `body_max_va`), not "
            "`entry_va + size_bytes`: 963 retail functions have non-contiguous bodies, and "
            "the extent avoids reporting their interior gaps as missing code.",
            "",
        ]

    candidates = report.inputs.gated_candidates
    lines += ["## Cross-build candidates", ""]
    if candidates is None:
        lines += [
            "`unknown`. No `missed_candidates.csv` present.",
            "",
        ]
    else:
        lines += [
            f"**{format_count(candidates)} gated candidate entries** from the demo/retail "
            "cross-build pipeline: addresses the demo's function table predicts that retail's "
            "own analysis has no function at, kept only where a terminating instruction ends "
            "exactly where the entry begins.",
            "",
            "Coverage of that pipeline plateaus near 55% of donor `.text`, so it contributes "
            "on the order of a hundred recoveries, not the full gap. See "
            "[`cross-build-diff.md`](cross-build-diff.md).",
            "",
        ]

    lines += ["## Where the work is concentrated", ""]
    if split is None or report.coverage is None or not report.classified:
        lines += ["`unknown`. Needs the artifacts listed above.", ""]
    else:
        game_unnamed = [
            item for item in report.classified if item.region == REGION_GAME and not item.is_named
        ]
        lines += [
            "### Unnamed game functions by size",
            "",
            "| size | functions |",
            "|---|---|",
        ]
        for label, count in size_buckets(game_unnamed):
            lines.append(f"| {label} | {format_count(count)} |")
        inputs = report.inputs
        if inputs.text_lo is not None and inputs.text_hi is not None:
            lines += [
                "",
                "### All `.text` functions by address range",
                "",
                "Equal eighths of `.text`. The library tail shows up as a naming "
                "concentration in the last slice without any cutoff being asserted.",
                "",
                "| range | functions | of which library-evidenced |",
                "|---|---|---|",
            ]
            library_by_bucket = {
                (start, end): count
                for start, end, count in address_buckets(
                    [item for item in report.classified if item.region == REGION_LIBRARY],
                    text_lo=inputs.text_lo,
                    text_hi=inputs.text_hi,
                )
            }
            for start, end, count in address_buckets(
                report.classified, text_lo=inputs.text_lo, text_hi=inputs.text_hi
            ):
                library = library_by_bucket.get((start, end), 0)
                lines.append(
                    f"| `{start:#010x}`-`{end:#010x}` | {format_count(count)} | "
                    f"{format_count(library)} |"
                )
        lines.append("")

    lines += [
        "## Not in this file, by design",
        "",
        "The per-address lists go to gitignored `generated/`:",
        "",
        f"- `generated/<build>/unnamed_functions.csv`: `{','.join(UNNAMED_CSV_COLUMNS)}`",
        f"- `generated/<build>/undefined_regions.csv`: `{','.join(UNDEFINED_CSV_COLUMNS)}`",
        "",
        "Regenerate with `uv run python -m tools.coverage_cli`.",
        "",
    ]
    return "\n".join(lines)


def render_badge_table(metrics: Iterable[Metric], *, badge_dir: str = "docs/badges") -> str:
    """A markdown row of local badge images, for embedding in README.md."""
    cells = [
        f"![{escape(metric.label)}]({badge_dir}/{badge_filename(metric)})" for metric in metrics
    ]
    return " ".join(cells)


def render_summary(report: CoverageReport) -> str:
    """Human-readable console summary, loudest about what is unknown."""
    lines = ["metric                     value"]
    for metric in report.metrics:
        marker = "  (UNKNOWN)" if metric.is_unknown else ""
        lines.append(f"{metric.label:26s} {metric.display_value}{marker}")
        if metric.unknown_reason:
            lines.append(f"  not measured because {metric.unknown_reason}")
    evaluation = report.replacement
    if evaluation is not None and evaluation.failures:
        lines.append("")
        lines.append("registered replacements that are NOT proven (first failing gate):")
        for failure in evaluation.failures:
            lines.append(f"  {failure.function.va:#010x} {failure.function.name}: {failure.gate}")
    if evaluation is not None and evaluation.stale:
        lines.append("")
        lines.append("registered replacements the snapshot cannot vouch for (NOT proven):")
        for item in evaluation.stale:
            lines.append(f"  {item.function.va:#010x} {item.function.name}: {item.reason}")
    if evaluation is not None and evaluation.cross_check:
        lines.append("")
        lines.append("tracked snapshot cross-check:")
        lines.extend(f"  {line}" for line in evaluation.cross_check)
    return "\n".join(lines)
