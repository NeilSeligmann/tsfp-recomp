# SPDX-License-Identifier: GPL-3.0-or-later
"""T1773 synthesized input domains.

A `Domain` is a pure function of the ORIGINAL function bytes (entry, size, code) and of the
frozen generator version. It never reads a replacement, a draft or any oracle outcome. Its
cases are ordinary harness `Case` objects (initial registers plus ordered memory patches), so
the oracle and the subject receive the identical state by construction, and invalid, null and
boundary states stay in the mix (a faulting original stays a non-verdict).

Pipeline: `synth_access.summarize` (what the original reads, writes and compares) -> role
assignment (pointer region, table index, scalar, flag mask) -> a canonical document that is
hashed and recorded -> `Domain.make_case(seed, ordinal)` which materialises one scenario from
the document alone, so an artifact on disk regenerates the identical stream.
"""

from __future__ import annotations

import hashlib
import json
from bisect import bisect_right
from collections.abc import Sequence
from dataclasses import dataclass, replace
from functools import lru_cache
from itertools import product
from pathlib import Path
from random import Random
from typing import Any

from .model import REG_NAMES, Case
from .seeding import SCRATCH_BASE, SCRATCH_WORDS, SeedPolicy, make_case
from .synth_access import AccessSummary, Lin, lin_text, summarize

#: Frozen generator identity. Any change to derivation or materialisation bumps this string.
GENERATOR_VERSION = "t1773-synth-v1"
SCHEMA = "t1773-synth-domain-v1"
#: Fixture namespace (case index = NAMESPACE << 40 + ordinal). Unused by every other family.
NAMESPACE = 130
#: Provider label and evidence class name. Reported separately from default-domain evidence.
LABEL = "synth-domain"
EVIDENCE_CLASS = "synthesized-domain"

#: Cases per root. The first SWEEP_CASES are an all-valid sweep over the scalar seeds, the rest a
#: seeded mix that keeps null, garbage and boundary pointers.
CASES = 640
SWEEP_CASES = 16
#: Bump arena for synthesized regions, clear of the image (ends 0x89D000), the default scratch
#: arena (0xD00000), the fs/TLS pages and the generated stacks (0xE80000).
ARENA_LO = 0x00900000
ARENA_HI = 0x00CF0000
WINDOW_LO = 0x00010000
WINDOW_HI = 0x01000000
REGION_CAP = 0x80000
PLAUSIBLE_INDEX = 0x2100
HEAD_BYTES = 0x400
MAX_FLAG_DEPS = 3
DEFAULT_REGION = 0x40
SLACK = 0x20
UNBOUNDED_EXTENT = 0x400
P_NULL = 0.08
P_GARBAGE = 0.03
P_BOUNDARY = 0.04
P_ALIAS = 0.35
P_PRIMARY = 0.4
GENERIC_SCALARS = (
    0,
    1,
    2,
    3,
    4,
    8,
    0x10,
    0x7F,
    0x80,
    0xFF,
    0x100,
    0x7FFFFFFF,
    0x80000000,
    0xFFFFFFFF,
)
COUNT_SEEDS = (0, 1, 2, 3, 4, 8, 16, 33)
MAX_COUNT = 0x1000

_REG_INDEX = {name: index for index, name in enumerate(REG_NAMES)}


def canonical(document: Any) -> bytes:
    return json.dumps(document, sort_keys=True, separators=(",", ":")).encode()


def load_analysis_table(path: Path) -> dict[str, Any]:
    """Explicit immutable original-boundary input; never substitute current overlays."""
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or document.get("schema") != "t1779-analysis-table-v1":
        raise ValueError("unknown synth analysis-table schema")
    rows = document.get("functions")
    if not isinstance(rows, list):
        raise ValueError("invalid synth analysis-table functions")
    if any(
        not isinstance(row, list)
        or len(row) != 2
        or any(type(v) is not int for v in row)
        or not 0 <= row[0] < 1 << 32
        or not 0 < row[1] <= (1 << 32) - row[0]
        for row in rows
    ):
        raise ValueError("invalid synth analysis-table range")
    sizes = dict(rows)
    if len(sizes) != len(rows):
        raise ValueError("duplicate synth analysis-table entry")
    ranges = tuple(sorted((va, va + size) for va, size in rows))
    if hashlib.sha256(canonical(ranges)).hexdigest() != document.get("code_ranges_sha256"):
        raise ValueError("synth analysis-table hash mismatch")
    return {"code_ranges": ranges, "function_sizes": sizes}


def _signed(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - (1 << 32) if value & 0x80000000 else value


def _width_mask(width: int) -> int:
    return (1 << (8 * width)) - 1 if width < 4 else 0xFFFFFFFF


# ---------------------------------------------------------------------------------------
# Code ranges: patches must never land on original code (T1525 fetch-guard concern)
# ---------------------------------------------------------------------------------------


@lru_cache(maxsize=1)
def _default_code_ranges() -> tuple[tuple[int, int], ...]:
    try:
        from tools.codediff.boundaries import load_function_table

        table = load_function_table(Path("generated/retail/functions.csv"))
        return tuple(sorted((fn.entry_va, fn.entry_va + fn.size_bytes) for fn in table))
    except Exception:  # noqa: BLE001 - table absent: the image sections still bound the check
        return ()


def _in_code(
    code_ranges: Sequence[tuple[int, int]], starts: Sequence[int], lo: int, hi: int
) -> bool:
    index = bisect_right(starts, hi - 1) - 1
    for candidate in (index, index - 1):
        if candidate >= 0:
            start, end = code_ranges[candidate]
            if start < hi and lo < end:
                return True
    return False


# ---------------------------------------------------------------------------------------
# Role assignment
# ---------------------------------------------------------------------------------------


def _count_roots(
    summary: AccessSummary, facts_by_root: dict[str, list[dict[str, Any]]]
) -> set[str]:
    """Roots whose value is (or feeds) a `rep` iteration count: kept small, a count of 2**32-1
    makes a single `rep movsd` run for hours and would hang every case that draws it."""
    found: set[str] = set()
    stack = [
        key for key, facts in facts_by_root.items() if any(f["kind"] == "count" for f in facts)
    ]
    while stack:
        key = stack.pop()
        if key in found:
            continue
        found.add(key)
        root = summary.roots.get(key)
        if root is not None:
            stack.extend(root.sources)
    return found


def _seeds_for(facts: list[dict[str, Any]], width: int, bounded: bool = False) -> list[int]:
    mask = _width_mask(width)
    seeds: list[int] = []

    def add(value: int) -> None:
        value &= mask
        if value not in seeds:
            seeds.append(value)

    for fact in facts:
        if fact["kind"] == "eq":
            for delta in (0, -1, 1):
                add(fact["value"] + delta)
        elif fact["kind"] == "mask":
            bits = fact["value"] & mask
            add(0)
            add(bits)
            if bits:
                add(bits & -bits)
                add(mask & ~bits)
                add(mask)
        elif fact["kind"] == "zero":
            add(0)
            add(1)
        elif fact["kind"] == "count":
            for value in COUNT_SEEDS:
                add(value)
    if bounded:
        for value in COUNT_SEEDS:
            add(value)
        return [value for value in seeds if value <= MAX_COUNT]
    for value in GENERIC_SCALARS:
        add(value)
    return seeds


def _index_seeds(facts: list[dict[str, Any]], width: int) -> tuple[list[int], int]:
    """Small dense range plus every compared constant and its neighbours (even large ones:
    sentinels such as -1 or a range limit select branches a small index never reaches)."""
    bound = 8
    for fact in facts:
        if fact["kind"] == "eq" and 0 <= fact["value"] < 0x1000:
            bound = max(bound, min(fact["value"] + 2, 64))
    seeds = list(range(bound))
    mask = _width_mask(width)
    for fact in facts:
        if fact["kind"] == "eq":
            for delta in (0, -1, 1):
                value = (fact["value"] + delta) & mask
                if value not in seeds:
                    seeds.append(value)
    return seeds, bound


def _reach_values(seeds: list[int], width: int) -> tuple[int, int]:
    """(min, max) signed index values small enough to size a region for."""
    signed = [
        v - (1 << (8 * width)) if width < 4 and v >> (8 * width - 1) else _signed(v) for v in seeds
    ]
    plausible = [v for v in signed if -PLAUSIBLE_INDEX <= v <= PLAUSIBLE_INDEX] or [0]
    return min(plausible), max(plausible)


def _primary(facts: list[dict[str, Any]], width: int) -> list[int]:
    """The compared constants themselves: the values that flip a branch on this input."""
    mask = _width_mask(width)
    out: list[int] = []
    for fact in facts:
        if fact["kind"] in ("eq", "mask"):
            value = fact["value"] & mask
        elif fact["kind"] == "zero":
            value = 0
        else:
            continue
        if value not in out:
            out.append(value)
    return out


def _roles(summary: AccessSummary) -> dict[str, dict[str, Any]]:
    """Per-root role record (JSON-ready)."""
    roots: dict[str, dict[str, Any]] = {}
    for key in sorted(summary.roots):
        root = summary.roots[key]
        row: dict[str, Any] = {"key": key, "kind": root.kind, "width": root.width}
        if root.kind == "arg":
            row["index"] = root.index
        if root.kind == "reg":
            row["reg"] = root.reg
        if root.addr is not None:
            row["addr"] = {"terms": [list(t) for t in root.addr.terms], "const": root.addr.const}
        row.update(role="scalar", lo=0, hi=0, evidence=[])
        roots[key] = row

    facts_by_root: dict[str, list[dict[str, Any]]] = {key: [] for key in roots}
    for fact in sorted({(f.va, f.kind, f.root, f.value, f.other, f.width) for f in summary.facts}):
        facts_by_root.setdefault(fact[2], []).append(
            {"kind": fact[1], "value": fact[3], "other": fact[4]}
        )
    pointer_evidence: dict[str, int] = {key: 0 for key in roots}
    index_uses: dict[str, list[tuple[int, int]]] = {key: [] for key in roots}
    pointer_uses: dict[str, list[tuple[int, int, bool, list[tuple[str, int]]]]] = {
        key: [] for key in roots
    }
    accesses = sorted(
        {
            (a.va, a.rw, a.width, lin_text(a.addr), a.repeat, a.strided): a
            for a in summary.accesses
        }.items()
    )
    for _sig, access in accesses:
        addr: Lin = access.addr
        terms = [(key, _signed(coeff)) for key, coeff in addr.terms]
        if len(terms) == 1 and terms[0][1] == 1:
            pointer_evidence[terms[0][0]] += 1
    for _sig, access in accesses:
        addr = access.addr
        terms = [(key, _signed(coeff)) for key, coeff in addr.terms]
        const = _signed(addr.const)
        if not terms:
            continue
        pointer: str | None = None
        others: list[tuple[str, int]] = []
        unit = [t for t in terms if t[1] == 1 and roots[t[0]]["kind"] != "opaque"]
        if len(terms) == 1 and terms[0][1] == 1:
            if WINDOW_LO <= (const & 0xFFFFFFFF) < WINDOW_HI:
                index_uses[terms[0][0]].append((1, const & 0xFFFFFFFF))
                continue
            pointer = terms[0][0]
        elif len(terms) == 1:
            index_uses[terms[0][0]].append((terms[0][1], const & 0xFFFFFFFF))
            continue
        elif unit:
            ranked = sorted(unit, key=lambda t: (-pointer_evidence[t[0]], t[0]))
            pointer = ranked[0][0]
        if pointer is None:
            for key, coeff in terms:
                index_uses[key].append((coeff, const & 0xFFFFFFFF))
            continue
        others = [t for t in terms if t[0] != pointer]
        width = max(access.width, 1) * (access.repeat if access.repeat > 0 else 1)
        pointer_uses[pointer].append((const, width, access.strided or access.repeat == 0, others))
        for key, coeff in others:
            index_uses[key].append((coeff, 0))

    index_reach: dict[str, tuple[int, int]] = {}
    bounded = _count_roots(summary, facts_by_root)
    for key, row in roots.items():
        facts = facts_by_root.get(key, [])
        if pointer_uses[key]:
            row["role"] = "pointer"
        elif index_uses[key]:
            row["role"] = "index"
        if row["kind"] == "opaque":
            row["role"] = "opaque"
            row["seeds"] = [0]
            index_reach[key] = (0, 0)
        elif row["kind"] == "flag":
            row["role"] = "flag"
            row["seeds"] = [0, 1]
            index_reach[key] = (0, 1)
        elif row["role"] == "index":
            seeds, _bound = _index_seeds(facts, row["width"])
            row["seeds"] = seeds
            row["scales"] = sorted({s for s, _ in index_uses[key]})
            index_reach[key] = _reach_values(seeds, row["width"])
        elif row["role"] == "scalar":
            row["seeds"] = _seeds_for(facts, row["width"], key in bounded)
            row["bounded"] = key in bounded
        else:
            row["seeds"] = []
        row["facts"] = [dict(f) for f in facts]
        row["primary"] = _primary(facts, row["width"]) if row["role"] in ("scalar", "index") else []
    for key, row in roots.items():
        if row["role"] != "pointer":
            continue
        lo, hi = 0, 0
        first = True
        for off, width, open_ended, others in pointer_uses[key]:
            extra_lo = extra_hi = 0
            for other, coeff in others:
                low, high = index_reach.get(other, (0, 7))
                extra_lo += min(coeff * low, coeff * high, 0)
                extra_hi += max(coeff * low, coeff * high, 0)
            start = off + extra_lo
            end = off + extra_hi + width + (UNBOUNDED_EXTENT if open_ended else 0)
            lo = start if first else min(lo, start)
            hi = end if first else max(hi, end)
            first = False
        if first or hi <= lo:
            lo, hi = 0, DEFAULT_REGION
        row["lo"], row["hi"] = max(lo, -REGION_CAP), min(hi + SLACK, REGION_CAP)
        row["seeds"] = [0]
        row["uses"] = sorted(
            {
                (off, coeff, idx, width)
                for off, width, _open, others in pointer_uses[key]
                for idx, coeff in others
            }
        )
    return roots


def _facts_pairs(summary: AccessSummary) -> list[list[Any]]:
    """`[root, other, offset, stride]`: root + offset (+ k * stride) is compared with other."""
    pairs = {(f.root, f.other, f.value, f.stride) for f in summary.facts if f.kind == "rel"}
    return [list(pair) for pair in sorted(pairs)]


@dataclass(frozen=True)
class Domain:
    """A derived, hashed, reproducible input domain for one root."""

    va: int
    size: int
    document: dict[str, Any]
    sha256: str
    code_ranges: tuple[tuple[int, int], ...] = ()

    def case_count(self) -> int:
        return int(self.document["case_count"])

    def artifact(self) -> dict[str, Any]:
        return {**self.document, "sha256": self.sha256}

    def make_case(self, seed: int, ordinal: int, *, policy: SeedPolicy | None = None) -> Case:
        if type(ordinal) is not int or not 0 <= ordinal < self.case_count():
            raise ValueError("synth-domain: ordinal out of range")
        return _materialize(self, seed, ordinal, policy)


def _document_hash(document: dict[str, Any]) -> str:
    return hashlib.sha256(canonical(document)).hexdigest()


def derive(
    va: int,
    size: int,
    image: Any,
    *,
    code_ranges: Sequence[tuple[int, int]] | None = None,
    mutate: Any = None,
    generator_version: str = GENERATOR_VERSION,
    function_sizes: dict[int, int] | None = None,
) -> Domain:
    """Derive the domain from the original bytes only. `image` is a `GuestImage`.

    `mutate` exists solely for the analysis-mutation tests: a callable applied to the
    summary before roles are assigned (it must never be set in a production run).
    """
    code = image.code_at(va, size)
    if len(code) != size:
        raise ValueError(f"synth-domain: {va:#x}+{size} outside the guest window")
    analysis_codes = None
    if generator_version == GENERATOR_VERSION:
        summary = summarize(va, code)
    elif generator_version == "t1773-synth-v2":
        from . import synth_interproc

        if function_sizes is None:
            from tools.codediff.boundaries import load_function_table

            function_sizes = {
                fn.entry_va: fn.size_bytes
                for fn in load_function_table(Path("generated/retail/functions.csv"))
            }
        summary, analysis_codes = synth_interproc.summarize(
            va, code, read_code=image.code_at, sizes=function_sizes
        )
    else:
        raise ValueError(f"unknown synth generator version: {generator_version}")
    if mutate is not None:
        mutate(summary)
    roots = _roles(summary)
    ranges = tuple(sorted(code_ranges)) if code_ranges is not None else _default_code_ranges()
    document: dict[str, Any] = {
        "schema": SCHEMA,
        "generator_version": generator_version,
        "evidence_class": EVIDENCE_CLASS,
        "label": LABEL,
        "namespace": NAMESPACE,
        "va": f"{va:#010x}",
        "size": size,
        "code_sha256": hashlib.sha256(code).hexdigest(),
        "case_count": CASES,
        "sweep_cases": SWEEP_CASES,
        "arena": [ARENA_LO, ARENA_HI],
        "probabilities": {
            "null": P_NULL,
            "garbage": P_GARBAGE,
            "boundary": P_BOUNDARY,
            "alias": P_ALIAS,
            "primary": P_PRIMARY,
        },
        "code_ranges_sha256": hashlib.sha256(canonical(ranges)).hexdigest(),
        "string_ops": summary.string_ops,
        "roots": [roots[key] for key in sorted(roots)],
        "alias_pairs": _facts_pairs(summary),
        "summary": summary.to_dict(),
    }
    if analysis_codes is not None:
        document["analysis_code_sha256"] = analysis_codes
        document["analysis_policy"] = synth_interproc.POLICY
    return Domain(va, size, document, _document_hash(document), ranges)


# ---------------------------------------------------------------------------------------
# Materialisation
# ---------------------------------------------------------------------------------------


def _case_rng(domain: Domain, seed: int, ordinal: int) -> Random:
    material = f"{domain.document['generator_version']}:{domain.sha256}:{seed}:{ordinal}".encode()
    return Random(int.from_bytes(hashlib.sha256(material).digest(), "big"))


def _align(value: int, to: int = 16) -> int:
    return (value + to - 1) // to * to


def _unknown_dword(rng: Random) -> int:
    roll = rng.random()
    if roll < 0.40:
        return 0
    if roll < 0.60:
        return rng.randrange(1, 17)
    if roll < 0.70:
        return rng.randrange(0, 1 << 32)
    return SCRATCH_BASE + rng.randrange(0, SCRATCH_WORDS) * 4


def _dependencies(roots: list[dict[str, Any]]) -> dict[str, tuple[str, ...]]:
    """Flag roots each root's PLACE transitively depends on (sorted)."""
    by_key = {row["key"]: row for row in roots}
    memo: dict[str, tuple[str, ...]] = {}

    def deps(key: str) -> tuple[str, ...]:
        if key in memo:
            return memo[key]
        memo[key] = ()
        row = by_key[key]
        found: set[str] = set()
        if row["kind"] == "flag":
            found.add(key)
        elif row["kind"] == "cell":
            for term, _coeff in row["addr"]["terms"]:
                if term in by_key:
                    found.update(deps(term))
        memo[key] = tuple(sorted(found))[:MAX_FLAG_DEPS]
        return memo[key]

    return {row["key"]: deps(row["key"]) for row in roots}


def _depth(roots: list[dict[str, Any]]) -> dict[str, int]:
    by_key = {row["key"]: row for row in roots}
    memo: dict[str, int] = {}

    def depth(key: str) -> int:
        if key in memo:
            return memo[key]
        memo[key] = 0
        row = by_key[key]
        value = 0
        if row["kind"] == "cell":
            value = 1 + max((depth(t) for t, _ in row["addr"]["terms"] if t in by_key), default=0)
        memo[key] = value
        return value

    return {row["key"]: depth(row["key"]) for row in roots}


def _materialize(domain: Domain, seed: int, ordinal: int, policy: SeedPolicy | None) -> Case:
    base = make_case(seed, (NAMESPACE << 40) + ordinal, domain.va, domain.size, policy=policy)
    rng = _case_rng(domain, seed, ordinal)
    roots: list[dict[str, Any]] = domain.document["roots"]
    sweep = ordinal < domain.document["sweep_cases"]
    starts = [start for start, _ in domain.code_ranges]
    deps = _dependencies(roots)
    depth = _depth(roots)
    order = sorted(roots, key=lambda row: (depth[row["key"]], row["key"]))
    state = {"cursor": ARENA_LO, "boundary": False}
    # value[(key, env)] where env is the tuple of (flag, 0/1) the root's place depends on
    value: dict[tuple[str, tuple[tuple[str, int], ...]], int] = {}
    regions: list[tuple[int, int, int, str, tuple[tuple[str, int], ...]]] = []
    contents: list[tuple[int, bytes]] = []

    def envs(key: str) -> list[tuple[tuple[str, int], ...]]:
        names = deps[key]
        return [tuple(zip(names, bits, strict=True)) for bits in product((0, 1), repeat=len(names))]

    def restrict(env: dict[str, int], key: str) -> tuple[tuple[str, int], ...]:
        return tuple((name, env[name]) for name in deps[key])

    by_key = {row["key"]: row for row in roots}

    def current(term: str, env: dict[str, int]) -> int | None:
        """A root's value under one flag assignment (a flag IS its assigned bit)."""
        if by_key[term]["kind"] == "flag":
            return env.get(term)
        if any(name not in env for name in deps[term]):
            return None
        return value.get((term, restrict(env, term)))

    def allocate(row: dict[str, Any], env: tuple[tuple[str, int], ...]) -> int:
        pre = _align(max(0, -row["lo"]))
        size = _align(pre + max(row["hi"], 0) + SLACK)
        roll = rng.random()
        if sweep or roll >= P_NULL + P_GARBAGE + P_BOUNDARY:
            mode = "valid"
        elif roll < P_NULL:
            mode = "null"
        elif roll < P_NULL + P_GARBAGE:
            mode = "garbage"
        else:
            mode = "boundary"
        if mode == "null":
            return 0
        if mode == "garbage":
            return rng.randrange(0, 1 << 32)
        if mode == "boundary" and not state["boundary"] and size <= 0x8000:
            start = WINDOW_HI - size
            state["boundary"] = True
        else:
            start = _align(state["cursor"])
            if start + size > ARENA_HI:
                return 0
            state["cursor"] = start + size
        head = bytearray()
        for _ in range(min(size, HEAD_BYTES) // 4):
            head += _unknown_dword(rng).to_bytes(4, "little")
        contents.append((start, bytes(head)))
        regions.append((start, size, pre, row["key"], env))
        return start + pre

    def draw(row: dict[str, Any], env: tuple[tuple[str, int], ...]) -> int:
        role, seeds = row["role"], row["seeds"]
        mask = _width_mask(row["width"])
        if role == "opaque":
            return 0
        if role == "pointer":
            return allocate(row, env)
        if role == "flag":
            return seeds[ordinal % 2] if sweep else rng.randrange(2)
        if role == "index" and not sweep and rng.random() < P_GARBAGE:
            return rng.randrange(0, 1 << 32) & mask
        if sweep:
            return seeds[ordinal % len(seeds)]
        primary = row.get("primary")
        if primary and rng.random() < P_PRIMARY:
            return primary[rng.randrange(len(primary))]
        if role == "scalar" and not row.get("bounded") and rng.random() < 0.15:
            return rng.randrange(0, 1 << 32) & mask
        return seeds[rng.randrange(len(seeds))]

    for row in order:
        for env in envs(row["key"]):
            value[(row["key"], env)] = draw(row, env)
    for first, second, offset, stride in domain.document["alias_pairs"]:
        a, b = (first, ()), (second, ())
        if a in value and b in value and not sweep and rng.random() < P_ALIAS:
            step = stride * rng.randrange(4) if stride else 0
            value[b] = (value[a] + offset + step) & 0xFFFFFFFF

    # emit memory: region contents, then cells, then arguments (ordered patches)
    patches: list[tuple[int, bytes]] = list(base.patches)
    regs = list(base.regs)

    def allowed(address: int, width: int) -> bool:
        if address < WINDOW_LO or address + width > WINDOW_HI:
            return False
        return not (
            domain.code_ranges and _in_code(domain.code_ranges, starts, address, address + width)
        )

    for start, size, pre, key, env in regions:
        for off, coeff, index_key, width in by_key[key].get("uses", ()):
            index_value = current(index_key, dict(env))
            if index_value is None:
                continue
            at = start + pre + off + coeff * _signed(index_value)
            span = _align(width + SLACK, 4)
            if start <= at and at + span <= start + size and (at - start) >= HEAD_BYTES:
                window = bytearray()
                for _ in range(span // 4):
                    window += _unknown_dword(rng).to_bytes(4, "little")
                contents.append((at, bytes(window)))
    for start, content in sorted(contents):
        patches.append((start, content))
    cells: list[tuple[int, str, int, int]] = []
    for row in roots:
        if row["kind"] != "cell":
            continue
        for env in envs(row["key"]):
            env_map = dict(env)
            place = row["addr"]["const"]
            for term, coeff in row["addr"]["terms"]:
                place += coeff * (current(term, env_map) or 0)
            cells.append((place & 0xFFFFFFFF, row["key"], row["width"], value[(row["key"], env)]))
    for place, _key, width, cell in sorted(cells):
        if allowed(place, width):
            patches.append((place, (cell & _width_mask(width)).to_bytes(width, "little")))
    for row in roots:
        key = (row["key"], ())
        if row["kind"] == "arg":
            patches.append(
                (esp_arg(base, row["index"]), (value[key] & 0xFFFFFFFF).to_bytes(4, "little"))
            )
        elif row["kind"] == "reg" and row["reg"] != "esp":
            regs[_REG_INDEX[row["reg"]]] = value[key] & 0xFFFFFFFF
    df = 1 if domain.document["string_ops"] and not sweep and ordinal % 8 == 7 else 0
    return replace(base, regs=tuple(regs), patches=tuple(patches), df=df)


def esp_arg(base: Case, index: int) -> int:
    return base.esp + 4 + 4 * index


# ---------------------------------------------------------------------------------------
# Stream identity helpers
# ---------------------------------------------------------------------------------------


def case_digest(case: Case) -> str:
    """sha256 of one case's initial state: registers, df, ordered patches, identity."""
    material = canonical(
        {
            "seed": case.seed,
            "index": case.index,
            "va": case.va,
            "size": case.size,
            "regs": list(case.regs),
            "df": case.df,
            "patches": [[address, data.hex()] for address, data in case.patches],
        }
    )
    return hashlib.sha256(material).hexdigest()


def stream_sha256(digests: Sequence[str]) -> str:
    return hashlib.sha256("\n".join(digests).encode()).hexdigest()


def case_stream_sha256(domain: Domain, seed: int, *, policy: SeedPolicy | None = None) -> str:
    """sha256 over the per-case digests of every case of the domain for `seed`."""
    return stream_sha256(
        [
            case_digest(domain.make_case(seed, ordinal, policy=policy))
            for ordinal in range(domain.case_count())
        ]
    )


def provider_for(domain: Domain) -> Any:
    """A `providers.Provider` (namespace NAMESPACE, label LABEL, supported_vas=(domain.va,))."""
    from .providers import Provider

    def count(_va: int) -> int:
        return domain.case_count()

    def make(
        seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
    ) -> Case:
        if (va, size) != (domain.va, domain.size):
            raise ValueError("synth-domain: provider used for another root")
        return domain.make_case(seed, ordinal, policy=policy)

    return Provider(NAMESPACE, (domain.va,), count, make, LABEL)
