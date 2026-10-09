# SPDX-License-Identifier: GPL-3.0-or-later
"""Match FLIRT patterns against code bytes at known function entry points.

WHY ONLY AT ENTRY POINTS. A FLIRT pattern describes a function *from its first
byte*, so the only addresses where a match means anything are addresses that are
function entries. Sliding the same patterns over every byte of a 3.9 MB `.text` is
both ~120,000 times more work and strictly worse evidence: a 32-byte pattern with
twenty wildcards will hit mid-function addresses by coincidence, and there is then
nothing to distinguish those hits from real ones. Our function table
(`generated/retail/functions.csv`, 12,343 entries) is exactly the restriction that
makes the technique cheap and sound at once. This is what FLIRT itself does.

HOW A MATCH IS DECIDED, in the order the checks run, cheapest first:

  1. **Prefix index.** Patterns are bucketed by their longest concrete leading
     prefix, so a given address only ever compares against patterns that could
     possibly match it. Patterns whose *first* byte is a wildcard cannot be
     bucketed and are candidates everywhere; see `PatternIndex.unindexed`.
  2. **Leading bytes**, wildcards skipped.
  3. **CRC16** over the `crc_length` bytes following byte 32, when `crc_length` is
     non-zero. This is the check that does most of the discriminating work, because
     it constrains up to 255 bytes that the pattern field does not reach.
  4. **Tail bytes**, wildcards skipped, from `32 + crc_length` onward.
  5. **Total length**, per `LengthCheck`.

AMBIGUITY IS A RESULT, NOT A NUISANCE. Library code is full of functions that are
byte-identical over their first 32 bytes, and whole archives are shipped in
several flavours (single/multithreaded, debug/release) whose patterns overlap. When
two patterns proposing *different* names both match an address, that address has no
name: emitting either one would be a coin flip presented as a fact. `EntryMatch`
keeps every match and `confident_name` returns `None` in that case. Several patterns
proposing the *same* name is not ambiguity, it is corroboration, and stays
confident.

Everything here is pure. Nothing reads a file or touches a disassembler.
"""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from enum import StrEnum

from tools.flirt.pattern import PATTERN_BYTES, WILDCARD, NameKind, Pattern, crc16

#: Prefix lengths the index buckets on, longest first. A pattern is filed under the
#: longest of these its concrete prefix covers, so lookups try each in turn. Four
#: bytes is enough to make buckets small on real x86 (prologues vary in the first
#: four bytes far more than in the first one) while keeping the index small.
PREFIX_TIERS: tuple[int, ...] = (4, 2, 1)

#: Constrained bytes a pattern must carry to be indexed at all. MEASURED on the real
#: XDK 5849 set against the retail XBE -- sweeping this is not a tuning exercise, it
#: strictly dominates having no floor:
#:
#:     floor  patterns  matched  ambiguous  confident  .XTLID  null@shift+1  null@random
#:       0      24472      678       317       361     75/75        0            1
#:       4      22177      416        53       363     75/75        0            0
#:       6      21170      402        45       357     75/75        0            0
#:       8      20042      362        42       320     75/75        0            0
#:      12      18321      338        25       313     73/73        0            0
#:
#: A floor of 4 produces MORE confident names than no floor (363 vs 361) while
#: cutting ambiguity by a factor of six, retaining every one of the 75 held-out
#: `.XTLID` cases, and taking the random-address null to zero. The reason it adds
#: names rather than costing them is that a 1-3 byte pattern is not evidence about
#: anything on x86 -- it matches hundreds of unrelated addresses -- and its only real
#: effect was to collide with good patterns and poison them into ambiguity. One
#: pattern constraining six bytes was proposing a single name at 27 distinct
#: addresses; another constraining two bytes proposed one name at four.
#:
#: 4 rather than 8 because the floor should be the point where the evidence becomes
#: non-vacuous, not the point where the answer looks tidiest, and everything from 4
#: upwards already has clean nulls.
MIN_SIGNIFICANT_BYTES = 4


class LengthCheck(StrEnum):
    """How strictly a pattern's `total_length` must agree with reality."""

    NONE = "none"
    """Ignore the field. Here to be measured against, not to be used."""

    FITS = "fits"
    """The function must fit inside the code region from the match address.

    The default. A pattern claiming a 900-byte function cannot match 40 bytes
    before the end of `.text`, and rejecting that costs nothing and is never wrong.
    """

    EXACT = "exact"
    """`total_length` must equal the length our own analysis recorded.

    The strongest check available, and too strong to trust as a default: our
    function sizes come from Ghidra, which pads, splits non-contiguous bodies and
    occasionally stops early, so an exact mismatch is as often our error as the
    pattern's. Reported alongside `FITS` so the cost of strictness is visible
    rather than assumed.
    """


@dataclass(frozen=True)
class EntryMatch:
    """Every pattern that matched one function entry."""

    entry_va: int
    patterns: tuple[Pattern, ...]
    """In index order, which is pattern-file order: deterministic across runs."""

    @property
    def proposed_names(self) -> tuple[str, ...]:
        """Distinct offset-0 public names proposed here, sorted.

        Sorted rather than first-seen so that the ambiguity report reads the same
        whichever order the pattern files were loaded in.
        """
        return tuple(
            sorted({name for name in (p.entry_name for p in self.patterns) if name is not None})
        )

    @property
    def libraries(self) -> tuple[str, ...]:
        """Distinct libraries that matched, sorted."""
        return tuple(sorted({pattern.library for pattern in self.patterns}))

    @property
    def ambiguous(self) -> bool:
        """Whether the matches disagree about the name."""
        return len(self.proposed_names) > 1

    @property
    def confident_name(self) -> str | None:
        """The single agreed name, or `None` if there is disagreement or none at all."""
        names = self.proposed_names
        return names[0] if len(names) == 1 else None

    @property
    def crc_verified(self) -> bool:
        """Whether at least one matching pattern carried a CRC that was checked.

        A match with `crc_length == 0` rests on the 32 pattern bytes and the tail
        alone. That is weaker, and the distinction is surfaced rather than averaged
        away because it is the main thing separating a solid proposal from a guess.
        """
        return any(pattern.crc_length > 0 for pattern in self.patterns)

    @property
    def significant_bytes(self) -> int:
        """The strongest matching pattern's constrained-byte count."""
        return max(pattern.significant_bytes for pattern in self.patterns)

    def extra_names(self) -> tuple[tuple[int, str], ...]:
        """(offset, name) for public names at non-zero offsets, sorted and deduped.

        These name *other* addresses covered by the matched module -- static helpers
        and the next function along, mostly. They are returned separately from
        `confident_name` because applying them means trusting the offset arithmetic
        as well as the match, and because `^` references are excluded: a reference
        records where a symbol is *used*, so naming that address after it would be
        straightforwardly wrong.
        """
        found: set[tuple[int, str]] = set()
        for pattern in self.patterns:
            for name in pattern.names:
                if name.kind is NameKind.PUBLIC and name.offset != 0:
                    found.add((name.offset, name.name))
        return tuple(sorted(found))


@dataclass
class MatchReport:
    """What a whole matching pass found, with the denominators to read it against."""

    entries_tested: int
    """Function entries that had enough bytes behind them to be tested at all."""

    entries_skipped: int
    """Entries outside the code region supplied, so never tested."""

    matches: list[EntryMatch] = field(default_factory=list)
    """Entries with at least one matching pattern, ascending by `entry_va`."""

    patterns_indexed: int = 0
    used_patterns: set[tuple[str, int]] = field(default_factory=set)
    """(library, source_line) of every pattern that matched somewhere."""

    rejected_by_crc: int = 0
    """Candidates whose leading bytes matched but whose CRC did not.

    The single most useful diagnostic number in the report. If this is zero across
    a real run, the CRC is not actually being exercised -- either every pattern has
    `crc_length == 0` or the algorithm is wrong in a way that happens to accept.
    If it is enormous relative to matches, the CRC algorithm is wrong in the
    direction that rejects everything, and the apparent precision is really just
    absence.
    """

    rejected_by_tail: int = 0
    rejected_by_length: int = 0

    @property
    def matched(self) -> int:
        return len(self.matches)

    @property
    def ambiguous(self) -> int:
        return sum(1 for match in self.matches if match.ambiguous)

    @property
    def confident(self) -> int:
        return sum(1 for match in self.matches if match.confident_name is not None)

    @property
    def unused_patterns(self) -> int:
        return self.patterns_indexed - len(self.used_patterns)


class PatternIndex:
    """Patterns bucketed by concrete leading prefix, for lookup by code bytes.

    Construction is linear in the pattern count. Lookup is a handful of dict probes
    plus whatever `unindexed` holds, so the cost of a pattern whose first byte is a
    wildcard is paid at every single address -- which is the honest cost, since such
    a pattern really can match anywhere.

    Patterns constraining fewer than `min_significant_bytes` bytes are DROPPED rather
    than indexed, and counted in `too_weak`. See `MIN_SIGNIFICANT_BYTES` for the
    measurement behind the default; dropping them improves every number that matters,
    including the count of names produced.
    """

    def __init__(
        self, patterns: Iterable[Pattern], *, min_significant_bytes: int = MIN_SIGNIFICANT_BYTES
    ) -> None:
        self._buckets: dict[tuple[int, bytes], list[Pattern]] = {}
        self.unindexed: list[Pattern] = []
        self.count = 0
        self.too_weak = 0
        self.min_significant_bytes = min_significant_bytes
        for pattern in patterns:
            if pattern.significant_bytes < min_significant_bytes:
                self.too_weak += 1
                continue
            self.count += 1
            prefix = pattern.concrete_prefix
            tier = next((width for width in PREFIX_TIERS if len(prefix) >= width), 0)
            if tier == 0:
                self.unindexed.append(pattern)
                continue
            self._buckets.setdefault((tier, prefix[:tier]), []).append(pattern)

    def candidates(self, code: bytes, offset: int) -> list[Pattern]:
        """Patterns that could match at `offset`, in a deterministic order.

        Returned longest-tier-first then in insertion order, so the same inputs
        always produce the same candidate sequence and therefore the same
        `EntryMatch.patterns` ordering.
        """
        found: list[Pattern] = []
        for width in PREFIX_TIERS:
            key = (width, code[offset : offset + width])
            if len(key[1]) < width:
                continue
            bucket = self._buckets.get(key)
            if bucket is not None:
                found.extend(bucket)
        found.extend(self.unindexed)
        return found


def matches_at(
    pattern: Pattern,
    code: bytes,
    offset: int,
    *,
    length_check: LengthCheck = LengthCheck.FITS,
    function_length: int | None = None,
    check_crc: bool = True,
    report: MatchReport | None = None,
) -> bool:
    """Whether `pattern` matches the function beginning at `code[offset]`.

    `function_length` is our own analysis's size for that function, used only by
    `LengthCheck.EXACT`. When `LengthCheck.EXACT` is requested and no length is
    known, the check degrades to `FITS` rather than to acceptance: an unknown
    length is not evidence of agreement.

    `check_crc=False` skips step 3 entirely. It exists to *measure* what the CRC is
    worth on a given dataset, by rerunning the same pass without it and comparing
    precision, and it is not a reasonable way to run the matcher for real. The
    region is still required to be present, so the two passes test the same
    addresses.

    `report`, when given, has its rejection counters incremented. Attributed to the
    *first* check that failed, so the counters partition the rejections rather than
    double-counting them.
    """
    if offset < 0 or offset >= len(code):
        return False

    if not _leading_matches(pattern, code, offset):
        return False

    crc_end = offset + PATTERN_BYTES + pattern.crc_length
    if pattern.crc_length > 0:
        if crc_end > len(code):
            _bump(report, "rejected_by_length")
            return False
        region = code[offset + PATTERN_BYTES : crc_end]
        if check_crc and crc16(region) != pattern.crc16:
            _bump(report, "rejected_by_crc")
            return False

    if not _tail_matches(pattern, code, crc_end):
        _bump(report, "rejected_by_tail")
        return False

    if not _length_ok(pattern, code, offset, length_check, function_length):
        _bump(report, "rejected_by_length")
        return False

    return True


def match_entries(
    index: PatternIndex,
    code: bytes,
    base_va: int,
    entries: Sequence[int],
    *,
    length_check: LengthCheck = LengthCheck.FITS,
    function_lengths: dict[int, int] | None = None,
    check_crc: bool = True,
) -> MatchReport:
    """Run `index` against every entry VA, returning one `MatchReport`.

    `base_va` is the virtual address of `code[0]`. Entries outside
    `[base_va, base_va + len(code))` are counted in `entries_skipped` and not
    tested, so a function table spanning several sections can be passed whole.

    Entries are visited in ascending VA regardless of the order given, which makes
    `MatchReport.matches` deterministic and lets a caller pass the CSV's own order.
    """
    report = MatchReport(entries_tested=0, entries_skipped=0, patterns_indexed=index.count)
    lengths = {} if function_lengths is None else function_lengths

    for entry_va in sorted(set(entries)):
        offset = entry_va - base_va
        if offset < 0 or offset >= len(code):
            report.entries_skipped += 1
            continue
        report.entries_tested += 1
        hits = [
            pattern
            for pattern in index.candidates(code, offset)
            if matches_at(
                pattern,
                code,
                offset,
                length_check=length_check,
                function_length=lengths.get(entry_va),
                check_crc=check_crc,
                report=report,
            )
        ]
        if not hits:
            continue
        report.matches.append(EntryMatch(entry_va=entry_va, patterns=tuple(hits)))
        for pattern in hits:
            report.used_patterns.add((pattern.library, pattern.source_line))

    return report


def _leading_matches(pattern: Pattern, code: bytes, offset: int) -> bool:
    """Compare the leading pattern bytes, skipping wildcards.

    The bounds test is `offset + len(leading) > len(code)`, not `>=`: a pattern of n
    bytes needs exactly n bytes available, and the off-by-one in the other direction
    silently refuses to match the last function in a section.
    """
    if offset + len(pattern.leading) > len(code):
        return False
    for index, expected in enumerate(pattern.leading):
        if expected is WILDCARD:
            continue
        if code[offset + index] != expected:
            return False
    return True


def _tail_matches(pattern: Pattern, code: bytes, start: int) -> bool:
    """Compare tail bytes, skipping wildcards. A tail running off the end fails."""
    if not pattern.tail:
        return True
    if start + len(pattern.tail) > len(code):
        return False
    for index, expected in enumerate(pattern.tail):
        if expected is WILDCARD:
            continue
        if code[start + index] != expected:
            return False
    return True


def _length_ok(
    pattern: Pattern,
    code: bytes,
    offset: int,
    length_check: LengthCheck,
    function_length: int | None,
) -> bool:
    if length_check is LengthCheck.NONE:
        return True
    if offset + pattern.total_length > len(code):
        return False
    if length_check is LengthCheck.EXACT and function_length is not None:
        return pattern.total_length == function_length
    return True


def _bump(report: MatchReport | None, counter: str) -> None:
    if report is not None:
        setattr(report, counter, getattr(report, counter) + 1)
