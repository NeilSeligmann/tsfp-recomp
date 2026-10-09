# SPDX-License-Identifier: GPL-3.0-or-later
"""Find corresponding runs of code between two differently-linked builds.

Retail `default.xbe` and the OXM 46 demo `tsdemo_cd.xbe` were compiled from
substantially the same engine eight weeks apart and linked at different addresses.
Where a run of instructions matches between them, the *ends* of that run are
external evidence about function boundaries: our decompiler recovered 12,343
functions against ~13,461 predicted, so every independent boundary signal counts.

ALGORITHM -- anchor and extend.

1. Everything happens at **instruction granularity**. A match that begins or ends
   part-way through an instruction is worthless as boundary evidence, so runs are
   only ever cut on decoded instruction boundaries.
2. Each instruction is reduced to a token of `(length, masked)` -- the
   layout-independent part of its encoding. `len(masked) == length` is already an
   invariant of `NormalisedText`, so the length is strictly redundant; it is hashed
   anyway as cheap insurance against a normaliser that ever breaks that invariant.
3. Tokens are interned to small integers and a rolling polynomial hash gives one
   fingerprint per window of `k` consecutive instructions. Interning is used rather
   than the builtin `hash()` because CPython salts `hash(bytes)` per process, which
   would make the output non-reproducible across runs.
4. **Only fingerprints that are unique in BOTH images may anchor a match.** This is
   the single most important rule here. Compilers emit enormous amounts of identical
   boilerplate -- register-save prologues, `xor eax, eax; ret`, import thunk tables,
   vtable-dispatch stubs -- so a repeated k-gram has no single correct partner. Any
   pairing picked from an ambiguous set is a guess wearing the costume of a
   measurement, and it would feed a *confidently wrong* boundary into the report.
   Repeated boilerplate is therefore dropped as an anchor; it is still recovered as
   part of a run when a neighbouring unique anchor extends across it, which is the
   only way to pair the right copy with the right copy.
5. Each surviving anchor is extended maximally backwards and forwards while tokens
   agree. Extension also refuses to cross a discontinuity in either image: if the
   next instruction does not start exactly where the current one ends (an
   `undecodable` hole), the run stops there, because the covered byte length would
   otherwise include bytes that were never compared.
6. Candidate runs are reduced to a non-overlapping set so each input instruction,
   in either image, belongs to at most one emitted run. TIE-BREAK: candidates are
   taken greedily in order of descending instruction count, then ascending `a`
   offset, then ascending `b` offset; a candidate overlapping an already-taken run
   in *either* image is dropped whole rather than truncated. Truncating would move a
   run boundary to a position no evidence supports, which is precisely the thing
   this module exists to avoid. The ordering is total, so the output is
   reproducible.
7. Runs shorter than `min_insns` instructions are discarded.

Hash collisions cannot produce a false match: every anchor window is verified token
by token before it is extended. A collision can only cost us an anchor (by making a
genuinely unique fingerprint look repeated), which fails safe.

`find_runs` is pure -- no I/O, no globals, no mutation of its arguments -- and
near-linear in instruction count. The images hold on the order of a million
instructions each, so every step is dict-based; nothing here is pairwise.
"""

from __future__ import annotations

from bisect import bisect_left, bisect_right
from dataclasses import dataclass

from tools.codediff.normalise import Insn, NormalisedText

KGRAM_INSNS = 8
"""Instructions per anchor fingerprint.

Chosen as 8 because it is the smallest window that is specific enough to be unique
in practice while still being short enough to anchor a small function. x86 averages
around 3.5 bytes per instruction in this code, so 8 instructions is roughly 28 bytes
of masked encoding -- comparable to the 32-byte windows already measured on these
binaries, and well past the point where a window is just a prologue. Going lower
(4) makes common prologue and epilogue sequences non-unique, and the uniqueness rule
above then throws away the very anchors we need. Going higher (16) sets a floor on
the shortest discoverable function, since nothing shorter than `k` instructions can
ever be anchored. 8 also matches the default `min_insns`, so by default every run
long enough to be reported is long enough to be found.
"""

_FNV_PRIME = 0x100000001B3
_MASK64 = (1 << 64) - 1


@dataclass(frozen=True)
class MatchedRun:
    """One run of code found at different addresses in two builds.

    `length` is the byte span covered in the `a` image; it is identical in `b`,
    because a run only grows while instruction lengths agree.
    """

    demo_va: int
    retail_va: int
    length: int
    insn_count: int


def find_runs(a: NormalisedText, b: NormalisedText, *, min_insns: int = 8) -> list[MatchedRun]:
    """Match runs of instructions between two normalised sections.

    `a` is the first image (reported as `demo_va`), `b` the second (`retail_va`).
    Returns maximal, mutually non-overlapping runs of at least `min_insns`
    instructions, ordered by ascending `demo_va`. See the module docstring for the
    uniqueness requirement on anchors and for the overlap tie-break.
    """
    threshold = max(1, min_insns)
    # k must not exceed the reporting threshold, or a caller lowering `min_insns`
    # would silently get the k=8 floor instead of the shorter runs they asked for.
    k = max(1, min(KGRAM_INSNS, threshold))

    tokens: dict[tuple[int, bytes], int] = {}
    ids_a = _intern(a.insns, tokens)
    ids_b = _intern(b.insns, tokens)
    if len(ids_a) < k or len(ids_b) < k:
        return []

    joined_a = _joined(a.insns)
    joined_b = _joined(b.insns)
    fps_a = _fingerprints(ids_a, k)
    fps_b = _fingerprints(ids_b, k)
    pos_a = _first_unique_position(fps_a)
    pos_b = _first_unique_position(fps_b)

    candidates: list[tuple[int, int, int]] = []
    # Diagonal (ib - ia) -> the `a` span of the run most recently grown on it, so
    # the O(run length) anchors inside one long run do not each re-extend it.
    grown: dict[int, tuple[int, int]] = {}

    for ia, fingerprint in enumerate(fps_a):
        if pos_a.get(fingerprint, -1) != ia:
            continue  # repeated in `a`: ambiguous, and so unusable as an anchor.
        ib = pos_b.get(fingerprint, -1)
        if ib < 0:
            continue  # absent from `b`, or repeated there.
        diagonal = ib - ia
        already = grown.get(diagonal)
        if already is not None and already[0] <= ia and ia + k <= already[1]:
            continue
        if not _window_agrees(ids_a, joined_a, ia, ids_b, joined_b, ib, k):
            continue  # hash collision, or a hole inside the window.

        start_a, start_b, count = _extend(ids_a, joined_a, ia, ids_b, joined_b, ib, k)
        grown[diagonal] = (start_a, start_a + count)
        if count >= threshold:
            candidates.append((count, start_a, start_b))

    return _resolve(candidates, a.insns, b.insns)


def coverage(runs: list[MatchedRun], total_bytes: int) -> float:
    """Fraction of `total_bytes` covered by `runs`, in [0.0, 1.0].

    `runs` is assumed to be the non-overlapping output of `find_runs`, so the
    covered byte count is a plain sum. The result is clamped, and a non-positive
    `total_bytes` yields 0.0 rather than raising.
    """
    if total_bytes <= 0:
        return 0.0
    covered = sum(run.length for run in runs)
    return min(1.0, covered / total_bytes)


def run_boundaries(runs: list[MatchedRun]) -> tuple[set[int], set[int]]:
    """(start VAs, end VAs) in the `a` image, as candidate function boundaries.

    CONVENTION: a start VA is the address of the run's first byte. An end VA is
    **exclusive** -- one past the run's last byte, i.e. `demo_va + length`. So a run
    at VA 0x1000 of length 0x20 contributes start 0x1000 and end 0x1020, and 0x1020
    is the first byte *after* the run. This is deliberately the same half-open
    convention as a Python slice, and it means the end VA of one run equals the start
    VA of an immediately following one, which is what a shared function boundary
    looks like. Downstream comparisons against function tables must use the same
    convention; an off-by-one here corrupts every boundary statistic silently.
    """
    starts = {run.demo_va for run in runs}
    ends = {run.demo_va + run.length for run in runs}
    return starts, ends


def _intern(insns: list[Insn], tokens: dict[tuple[int, bytes], int]) -> list[int]:
    """Reduce instructions to small ints, equal iff their masked encodings match."""
    out: list[int] = []
    for insn in insns:
        key = (insn.length, insn.masked)
        value = tokens.get(key)
        if value is None:
            value = len(tokens)
            tokens[key] = value
        out.append(value)
    return out


def _joined(insns: list[Insn]) -> list[bool]:
    """`out[i]` is True when instruction `i+1` starts exactly where `i` ends.

    False marks an `undecodable` hole, which a run must not span: the bytes in the
    hole were never compared, so counting them as covered would be a lie.
    """
    out = [False] * len(insns)
    for i in range(len(insns) - 1):
        out[i] = insns[i].offset + insns[i].length == insns[i + 1].offset
    return out


def _fingerprints(ids: list[int], k: int) -> list[int]:
    """Rolling polynomial hash of every window of `k` consecutive tokens."""
    count = len(ids) - k + 1
    if count <= 0:
        return []
    top = pow(_FNV_PRIME, k - 1, 1 << 64)
    rolling = 0
    for i in range(k):
        rolling = (rolling * _FNV_PRIME + ids[i] + 1) & _MASK64
    out = [rolling]
    for i in range(k, len(ids)):
        dropped = ((ids[i - k] + 1) * top) & _MASK64
        rolling = ((rolling - dropped) * _FNV_PRIME + ids[i] + 1) & _MASK64
        out.append(rolling)
    return out


def _first_unique_position(fps: list[int]) -> dict[int, int]:
    """Fingerprint -> its only index, or -1 when it occurs more than once."""
    pos: dict[int, int] = {}
    for i, fingerprint in enumerate(fps):
        if fingerprint in pos:
            pos[fingerprint] = -1
        else:
            pos[fingerprint] = i
    return pos


def _window_agrees(
    ids_a: list[int],
    joined_a: list[bool],
    ia: int,
    ids_b: list[int],
    joined_b: list[bool],
    ib: int,
    k: int,
) -> bool:
    """Confirm a fingerprint hit really is `k` matching, contiguous instructions."""
    for j in range(k):
        if ids_a[ia + j] != ids_b[ib + j]:
            return False
    for j in range(k - 1):
        if not joined_a[ia + j] or not joined_b[ib + j]:
            return False
    return True


def _extend(
    ids_a: list[int],
    joined_a: list[bool],
    ia: int,
    ids_b: list[int],
    joined_b: list[bool],
    ib: int,
    k: int,
) -> tuple[int, int, int]:
    """Grow a verified `k`-instruction anchor maximally both ways.

    Returns `(start_a, start_b, insn_count)`.
    """
    start_a, start_b = ia, ib
    while start_a > 0 and start_b > 0:
        if not joined_a[start_a - 1] or not joined_b[start_b - 1]:
            break
        if ids_a[start_a - 1] != ids_b[start_b - 1]:
            break
        start_a -= 1
        start_b -= 1

    end_a, end_b = ia + k, ib + k
    while end_a < len(ids_a) and end_b < len(ids_b):
        if not joined_a[end_a - 1] or not joined_b[end_b - 1]:
            break
        if ids_a[end_a] != ids_b[end_b]:
            break
        end_a += 1
        end_b += 1

    return start_a, start_b, end_a - start_a


def _resolve(
    candidates: list[tuple[int, int, int]],
    insns_a: list[Insn],
    insns_b: list[Insn],
) -> list[MatchedRun]:
    """Reduce candidates to a non-overlapping set, longest first, ties by offset."""
    taken_a = _Occupied()
    taken_b = _Occupied()
    kept: list[tuple[int, int, int]] = []

    for count, start_a, start_b in sorted(candidates, key=lambda c: (-c[0], c[1], c[2])):
        if taken_a.overlaps(start_a, start_a + count):
            continue
        if taken_b.overlaps(start_b, start_b + count):
            continue
        taken_a.add(start_a, start_a + count)
        taken_b.add(start_b, start_b + count)
        kept.append((start_a, start_b, count))

    runs: list[MatchedRun] = []
    for start_a, start_b, count in sorted(kept):
        first = insns_a[start_a]
        last = insns_a[start_a + count - 1]
        runs.append(
            MatchedRun(
                demo_va=first.va,
                retail_va=insns_b[start_b].va,
                length=last.offset + last.length - first.offset,
                insn_count=count,
            )
        )
    return runs


class _Occupied:
    """Disjoint half-open integer intervals, with O(log n) overlap tests."""

    def __init__(self) -> None:
        self._starts: list[int] = []
        self._ends: list[int] = []

    def overlaps(self, lo: int, hi: int) -> bool:
        i = bisect_right(self._starts, lo) - 1
        if i >= 0 and self._ends[i] > lo:
            return True
        j = i + 1
        return j < len(self._starts) and self._starts[j] < hi

    def add(self, lo: int, hi: int) -> None:
        i = bisect_left(self._starts, lo)
        self._starts.insert(i, lo)
        self._ends.insert(i, hi)
