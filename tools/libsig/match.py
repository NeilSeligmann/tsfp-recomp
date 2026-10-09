# SPDX-License-Identifier: GPL-3.0-or-later
"""Scan an image for library functions by full masked-body comparison."""

from __future__ import annotations

import struct
from collections import Counter, defaultdict
from collections.abc import Iterable
from dataclasses import dataclass, field

from tools.flirt.cli import normalise_symbol
from tools.libsig.coff import REL_DIR32, REL_REL32
from tools.libsig.db import Signature, SignatureDb

# Below this many fixed (unrelocated) bytes a body is flagged weak. The value is
# justified by measurement in docs/libsig.md.
WEAK_FIXED_BYTES = 16

# Below this many fixed bytes a body (`ret`, `jmp rel32`) says almost nothing: shifting
# every entry by one byte matched 1292 of 1293 addresses on such bodies. Such matches
# are `tiny`: dropped from output unless a reloc-target check (refine) confirms them.
TINY_FIXED_BYTES = 4

REASON_RELOC_TARGET = "reloc-target"
REASON_DATA_ADDRESS = "data-address"
REASON_XTLID = "xtlid"
REASON_ALT_BUILD = "alt-build"

# Confidences whose name the bytes did not settle: the ones `.XTLID` may promote. `refine` and
# `resolve_by_data` skip `alias_group`: one body with several names offers no reloc or addend to
# tell them apart, so those passes could never narrow it.
UNDECIDED = ("ambiguous", "alias_group", "weak", "tiny")

# A data symbol's game VA is only trusted when at least this many high matches vote for it and
# none disagrees: one vote could be one wrong match.
MIN_VA_VOTES = 2

# Padding bytes the linker may leave after a function.
PADDING = {0xCC, 0x90, 0x00}


class Image:
    """Executable address space: list of (va, bytes) segments."""

    def __init__(
        self, segments: Iterable[tuple[int, bytes]], names: dict[int, str] | None = None
    ) -> None:
        self.segments = sorted(segments)
        self.names = names or {}  # segment base -> XBE section name

    def section_of(self, va: int) -> str:
        for base, blob in self.segments:
            if base <= va < base + len(blob):
                return self.names.get(base, "")
        return ""

    def read(self, va: int, size: int) -> bytes | None:
        for base, blob in self.segments:
            if base <= va and va + size <= base + len(blob):
                return blob[va - base : va - base + size]
        return None


@dataclass
class Match:
    address: int
    names: list[str]  # distinct symbol names (aliases)
    libs: list[str]
    builds: list[str]
    body_len: int
    fixed_bytes: int
    confidence: str  # high | weak | ambiguous | alias_group | resolved | tiny | propagated
    signature: Signature | None = None
    all_names: list[str] = field(default_factory=list)  # before the section filter
    signatures: list[Signature] = field(default_factory=list)  # every verified body
    extra: dict[str, str] = field(default_factory=dict)

    @property
    def symbol(self) -> str:
        return self.names[0] if len(self.names) == 1 else ""


def _set_reason(match: Match, reason: str) -> None:
    """Record why a match was narrowed, except on an `alt-build` row: that provenance stays."""
    if match.extra.get("reason") != REASON_ALT_BUILD:
        match.extra["reason"] = reason


def _verify(image: Image, va: int, sig: Signature) -> bool:
    blob = image.read(va, len(sig.data))
    if blob is None:
        return False
    # Compare masked as big integers: one C-level pass instead of a byte loop.
    return int.from_bytes(blob, "little") & int.from_bytes(sig.mask, "little") == int.from_bytes(
        sig.data, "little"
    )


class Matcher:
    def __init__(self, db: SignatureDb) -> None:
        self.db = db
        self.index: dict[int, dict[bytes, list[Signature]]] = defaultdict(lambda: defaultdict(list))
        for sig in db.signatures.values():
            if sig.prefix_len:
                self.index[sig.prefix_len][sig.data[: sig.prefix_len]].append(sig)

    def match_at(self, image: Image, va: int) -> Match | None:
        head = image.read(va, 4) or b""
        hits: list[Signature] = []
        for plen, table in self.index.items():
            sigs = table.get(head[:plen]) if len(head) >= plen else None
            if sigs:
                hits.extend(s for s in sigs if _verify(image, va, s))
        if not hits:
            return None
        return self._classify(va, hits, image.section_of(va))

    @staticmethod
    def _classify(va: int, hits: list[Signature], xbe_section: str = "") -> Match:
        origins = {o for s in hits for o in s.origins}
        every_name = sorted({o[0] for o in origins})
        # The libs name the linker section each body lives in (`.text`, `D3D`,
        # `DSOUND`...) and the XBE keeps those sections apart, so an origin from a
        # different section than the one holding this address cannot be the source.
        # Applied only when it leaves something, so it can narrow but never erase.
        in_section = {o for o in origins if o[4] == xbe_section}
        origins = in_section or origins
        names = sorted({o[0] for o in origins})
        libs = sorted({o[1] for o in origins})
        builds = sorted({o[3] for o in origins})
        best = max(hits, key=lambda s: len(s.data))
        if best.fixed_bytes < TINY_FIXED_BYTES and all(
            h.fixed_bytes < TINY_FIXED_BYTES for h in hits
        ):
            confidence = "tiny"
        elif len(names) > 1:
            # One verified body behind every name (same masked bytes, reloc targets and addends)
            # means no evidence can ever pick: an alias group. Several bodies might be told apart.
            bodies = [h for h in hits if any(o in origins for o in h.origins)]
            confidence = "alias_group" if len(bodies) == 1 else "ambiguous"
        elif best.fixed_bytes < WEAK_FIXED_BYTES:
            confidence = "weak"
        else:
            confidence = "high"
        return Match(
            va,
            names,
            libs,
            builds,
            len(best.data),
            best.fixed_bytes,
            confidence,
            best,
            every_name,
            list(hits),
        )

    def scan_entries(self, image: Image, entries: Iterable[int]) -> dict[int, Match]:
        out = {}
        for va in entries:
            found = self.match_at(image, va)
            if found:
                out[va] = found
        return out

    def scan_all(self, image: Image) -> dict[int, Match]:
        """Try every byte offset (slow path): index hits become verified candidates."""
        candidates: set[int] = set()
        for base, blob in image.segments:
            for plen, table in self.index.items():
                for offset in range(len(blob) - plen + 1):
                    if blob[offset : offset + plen] in table:
                        candidates.add(base + offset)
        return self.scan_entries(image, sorted(candidates))


def _rel32_dest(image: Image, address: int, offset: int) -> int | None:
    raw = image.read(address + offset, 4)
    if raw is None:
        return None
    return (address + offset + 4 + struct.unpack("<i", raw)[0]) & 0xFFFFFFFF


def refine(image: Image, matches: dict[int, Match], function_symbols: set[str]) -> int:
    """Narrow ambiguous and weak matches by the relocation targets they name.

    Thunks share masked bytes yet differ in the symbol their REL32 names. For each
    verified body, every REL32 reloc whose game-side destination is already named
    (high, resolved or weak: a weak name is unique, only short) must name a symbol in that
    destination's name set. Bodies that contradict a named destination are dropped; if the
    remaining bodies carry one name the match becomes `resolved`. At least one confirmed reloc
    is required, so a body without checkable relocs is never promoted. Repeats to a fixed point.
    Returns the number of matches promoted to `resolved`.
    """
    promoted = 0
    changed = True
    while changed:
        changed = False
        for address, match in matches.items():
            if match.confidence not in ("ambiguous", "weak", "tiny") or not match.signatures:
                continue
            kept = []
            for signature in match.signatures:
                confirmed = contradicted = False
                for offset, kind, target, _ in signature.relocs:
                    if kind != REL_REL32 or target not in function_symbols:
                        continue
                    dest = _rel32_dest(image, address, offset)
                    other = matches.get(dest) if dest is not None else None
                    if other is None or other.confidence not in ("high", "resolved", "weak"):
                        continue
                    # A `resolved` destination was narrowed to one name by its own evidence, so
                    # its pre-narrowing alias set (all_names) proves nothing about its thunks.
                    if target in other.names or (
                        other.confidence == "high" and target in other.all_names
                    ):
                        confirmed = True
                    else:
                        contradicted = True
                if confirmed and not contradicted:
                    kept.append(signature)
            names = sorted({o[0] for sig in kept for o in sig.origins})
            if kept and len(names) == 1:
                match.names = names
                match.signatures = kept
                match.confidence = "resolved"
                _set_reason(match, REASON_RELOC_TARGET)
                promoted += 1
                changed = True
    return promoted


def _dir32_observed(image: Image, address: int, offset: int, addend: int) -> int | None:
    """Symbol VA implied by the 4-byte value in the game at a DIR32 reloc: value - addend."""
    raw = image.read(address + offset, 4)
    if raw is None:
        return None
    return (struct.unpack("<I", raw)[0] - addend) & 0xFFFFFFFF


def symbol_va_votes(image: Image, matches: dict[int, Match]) -> dict[str, Counter[int]]:
    """Votes for each DIR32 target symbol's game VA, from `high` unique-name matches only.

    Rows from an alternate build (`alt-build`) never vote: their data layout is another build's.

    A match votes once per symbol, and only if all of its DIR32 relocs to that symbol (across
    every verified body) imply the same VA.
    """
    votes: dict[str, Counter[int]] = defaultdict(Counter)
    for address, match in matches.items():
        if match.confidence != "high" or len(match.names) != 1:
            continue
        if match.extra.get("reason") == REASON_ALT_BUILD:
            continue
        implied: dict[str, set[int | None]] = defaultdict(set)
        for signature in match.signatures:
            for offset, kind, target, addend in signature.relocs:
                if kind == REL_DIR32:
                    implied[target].add(_dir32_observed(image, address, offset, addend))
        for target, vas in implied.items():
            if len(vas) == 1 and None not in vas:
                votes[target][next(iter(vas))] += 1
    return votes


def known_symbol_vas(votes: dict[str, Counter[int]]) -> dict[str, int]:
    """Symbols whose votes are unanimous and at least MIN_VA_VOTES strong."""
    return {
        symbol: next(iter(counter))
        for symbol, counter in votes.items()
        if len(counter) == 1 and sum(counter.values()) >= MIN_VA_VOTES
    }


def resolve_by_data(image: Image, matches: dict[int, Match]) -> int:
    """Narrow ambiguous, weak and tiny matches by the data addresses their DIR32 relocs name.

    The db keeps each DIR32's in-place addend, so a body is `symbol + addend`; bodies that
    differ only there (`SetRenderState_*`, vtable-setting destructors) collide on the masked
    bytes. The game value minus the addend must equal the symbol's VA, which is learned from
    unanimous votes of `high` matches (see `symbol_va_votes`). A body is kept only if every DIR32
    whose symbol VA is known agrees and at least one does. If the kept bodies carry one name
    the match becomes `resolved` (reason `data-address`). Returns the number promoted.
    """
    known = known_symbol_vas(symbol_va_votes(image, matches))
    promoted = 0
    for address, match in matches.items():
        if match.confidence not in ("ambiguous", "weak", "tiny") or not match.signatures:
            continue
        kept = []
        for signature in match.signatures:
            confirmed = contradicted = False
            for offset, kind, target, addend in signature.relocs:
                if kind != REL_DIR32 or target not in known:
                    continue
                if _dir32_observed(image, address, offset, addend) == known[target]:
                    confirmed = True
                else:
                    contradicted = True
            if confirmed and not contradicted:
                kept.append(signature)
        names = sorted({o[0] for sig in kept for o in sig.origins} & set(match.names))
        if kept and len(names) == 1:
            match.names = names
            match.signatures = kept
            match.confidence = "resolved"
            _set_reason(match, REASON_DATA_ADDRESS)
            promoted += 1
    return promoted


def resolve_by_xtlid(
    matches: dict[int, Match], xtlid_names: dict[int, str]
) -> tuple[int, list[tuple[int, str, list[str]]]]:
    """Name an undecided match from the game's own `.XTLID` record, never from `.XTLID` alone.

    `.XTLID` is an independent source (the linker's own table, names from XboxDev/xtlid), so
    where the bytes leave a candidate set (a weak match, an alias set) and the record at that
    address names exactly one member of it, after `normalise_symbol`, the match becomes
    `resolved` with reason `xtlid`. The pre-narrowing set stays in `extra["alias_set"]` (the
    bytes do not decide it). An `.XTLID` name outside the candidate set promotes nothing and is
    returned as a conflict `(address, xtlid name, candidates)`: it is evidence of a wrong
    match or a wrong database, never a naming. Returns (promoted, conflicts).
    """
    promoted = 0
    conflicts: list[tuple[int, str, list[str]]] = []
    for address, match in matches.items():
        if match.confidence not in UNDECIDED or address not in xtlid_names:
            continue
        truth = normalise_symbol(xtlid_names[address])
        members = [name for name in match.names if normalise_symbol(name) == truth]
        if len(members) != 1:
            if not members:
                conflicts.append((address, xtlid_names[address], list(match.names)))
            continue  # several members normalise alike (C++ overloads): not decided
        if len(match.names) > 1:
            match.extra["alias_set"] = "|".join(match.names)
        match.names = members
        match.signatures = [
            s for s in match.signatures if any(o[0] == members[0] for o in s.origins)
        ]
        match.confidence = "resolved"
        _set_reason(match, REASON_XTLID)
        promoted += 1
    return promoted, conflicts


def merge_alt_matches(
    matches: dict[int, Match], alt: dict[int, Match]
) -> tuple[int, list[tuple[int, list[str], list[str]]]]:
    """Add matches from an alternate-build db, only where the primary db named nothing.

    A later QFE of the same base build can fit a function whose primary-build body differs.
    An alternate row is added (reason `alt-build`, its build in `builds`) only where `matches`
    has no row or only a `tiny` one (not a name). Alternate `tiny` rows are kept so the evidence
    passes can still confirm them (the caller drops the unconfirmed). Any other primary row
    (even `ambiguous` or `weak`) is untouched: if the alternate's names share nothing with the
    primary's (names, pre-narrowing set, alias set), the pair is returned as a conflict
    `(address, primary names, alternate names)`. Returns (added, conflicts).
    """
    added = 0
    conflicts: list[tuple[int, list[str], list[str]]] = []
    for address, found in alt.items():
        primary = matches.get(address)
        if primary is not None and primary.confidence != "tiny":
            known = {
                *primary.names,
                *primary.all_names,
                *primary.extra.get("alias_set", "").split("|"),
            }
            if not known & set(found.names):
                conflicts.append((address, list(primary.names), list(found.names)))
            continue
        found.extra["reason"] = REASON_ALT_BUILD
        matches[address] = found
        added += 1
    return added, conflicts


def propagate(
    image: Image,
    matches: dict[int, Match],
    entries: set[int],
    function_symbols: set[str],
) -> tuple[dict[int, Match], list[tuple[int, int, str, list[str]]], int, int]:
    """Cross-check and extend names through REL32 call sites.

    For a unique-name high-confidence match at A whose signature has a REL32 reloc
    naming a function symbol S at offset o, the game's call target is
    T = A+o+4+rel32. If T is matched with a name set not containing S, that is a
    disagreement. If T is an unmatched known function entry, T is named S with
    confidence `propagated` (one hop: propagated names carry no relocs to follow).

    A target named only by a pre-section-filter alias counts as `folded` (identical
    bytes under another name), not as a disagreement.

    Returns (new matches, disagreements, agreement count, folded count).
    """
    disagreements: list[tuple[int, int, str, list[str]]] = []
    agreed = 0
    folded = 0
    added: dict[int, Match] = {}
    for address, match in matches.items():
        if (
            not match.signatures
            or match.confidence not in ("high", "resolved")
            or len(match.names) != 1
        ):
            continue
        # An alternate build's body names its callees, but only this build's matches add names.
        from_alt_build = match.extra.get("reason") == REASON_ALT_BUILD
        # Every verified body has the same mask, so reloc offsets line up; bodies from
        # different library variants may name different targets at one offset.
        targets: dict[int, set[str]] = {}
        for signature in match.signatures:
            for offset, kind, target, _ in signature.relocs:
                if kind == REL_REL32 and target in function_symbols:
                    targets.setdefault(offset, set()).add(target)
        for offset, names in targets.items():
            dest = _rel32_dest(image, address, offset)
            if dest is None:
                continue
            other = matches.get(dest) or added.get(dest)
            if other is not None:
                if names & set(other.names):
                    agreed += 1
                elif names & set(other.all_names):
                    folded += 1  # same bytes under another name: identical code folding
                else:
                    disagreements.append((address, dest, "|".join(sorted(names)), other.names))
            elif dest in entries and len(names) == 1 and not from_alt_build:
                added[dest] = Match(dest, sorted(names), [], [], 0, 0, "propagated")
    return added, disagreements, agreed, folded


SEED_CONFIDENCES = ("high", "resolved")


def select_seeds(
    matches: dict[int, Match], known_entries: set[int], excluded: frozenset[int] = frozenset()
) -> list[Match]:
    """High-confidence unique-name matches at addresses the function table does not have.

    Only `high` and `resolved` qualify (a full masked body of at least 16 fixed bytes, or
    one narrowed by confirmed call targets). Weak, ambiguous, tiny and propagated rows are
    never seeds, and neither is anything already in `known_entries` or in `excluded` (both ends
    of a call-site disagreement).
    Returned sorted by address.
    """
    return sorted(
        (
            m
            for address, m in matches.items()
            if m.confidence in SEED_CONFIDENCES
            and address not in known_entries
            and address not in excluded
        ),
        key=lambda m: m.address,
    )
