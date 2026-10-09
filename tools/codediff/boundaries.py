# SPDX-License-Identifier: GPL-3.0-or-later
"""Predict one build's missed function entries from the other build's function table.

WHAT A MATCHED RUN IS NOT. The obvious thing to do with `find_runs` output is to
read its boundaries as function boundaries, and that does not work: measured, 98.7%
of run starts sit mid-function. The reason is structural rather than a tuning
problem. A run is extended maximally, so it starts where *divergence* ends, and
divergence ends wherever the two linkers last disagreed about an operand, which is
in the middle of whatever function happened to contain the last differing
instruction. Nothing about a maximal run wants to begin at an entry point, so no
threshold on run length or `min_insns` will make its edges into entries.

WHAT A MATCHED RUN IS. Inside a run the two builds are the same code laid out at
different addresses, instruction for instruction and byte for byte, because a run
only grows while instruction lengths agree. That makes a run a byte-exact *address
mapping*: for any `0 <= delta < length`, `demo_va + delta` in one build is the same
instruction as `retail_va + delta` in the other. The mapping is the product here,
not the boundaries.

THE INFERENCE. Push a donor build's known function entries through that mapping. A
donor entry inside a run predicts an address in the target build which, if the two
builds really do share that function, is a function entry there too. Where the
target's own analysis already has a function at the predicted address, the two
agree and nothing is learned. Where it does not, the prediction is a function the
target's analysis MISSED, and that is the output: a candidate list.

The donor is deliberately interchangeable. Running demo -> retail finds what retail
analysis missed; swapping the arguments finds what the demo missed. The code here
never assumes which build is which, and `MatchedRun`'s field names (`demo_va`,
`retail_va`) are read strictly as "image a" and "image b".

MEASURED, demo as donor and retail as target, `min_insns=8`:

    12,787 runs, covering 51.30% of the demo's `.text`
     5,375 donor entries land inside a run
     5,286 of those agree with a retail function entry      98.3%
        89 do not, and every one of them is inside retail `.text`
        42 of the 89 are targeted by at least one `call rel32`

WHY 98.3% IS A RESULT AND NOT A TAUTOLOGY. A percentage with no null is not
interpretable, so `tools/codediff/cli.py` recomputes both nulls from the same inputs
on every run rather than quoting them. Exactly, on the demo/retail pair:

  * An arbitrary address inside a matched run is a retail function entry 0.2956% of
    the time (5,355 retail entries inside 1,811,268 mapped bytes). Agreement of
    98.3% is therefore about 333x that null, which is what says the mapping carries
    real structure rather than hitting entries by density.
  * An arbitrary retail `.text` address is the target of a `call rel32` 0.2273% of
    the time (8,948 distinct targets in 3,937,148 bytes). 42 of 89 candidates being
    call-targeted is 47.2%, about 208x that null, which is what says the candidates
    are entry points rather than arbitrary mid-function addresses.

AN EARLIER HAND MEASUREMENT PUT THE FIRST NULL AT 0.2%, hence a quoted 490x. That
figure does not reproduce. No natural exact definition lands near it: all retail
entries over mapped bytes is 0.2956%, excluding thunks is 0.2920%, and the plain
density over the whole of `.text` is 0.3135%. 0.2% looks like a small random sample,
where ~0.3% of a few hundred draws rounds down easily. The correct enrichment is
~333x, not ~490x. The second null reproduces to within rounding, 0.2273% against a
quoted 0.24%, which moves that enrichment from ~196x to ~208x. Both corrections are
small and neither changes any conclusion, but 490x was wrong and is not repeated.

Neither number is a proof for any individual candidate. They say the list as a whole
is overwhelmingly unlikely to be noise, and each candidate still has to be confirmed
by looking at it.

Everything here is pure except `load_functions`, which reads a CSV.
"""

from __future__ import annotations

import csv
import os
import struct
from bisect import bisect_left
from dataclasses import dataclass
from pathlib import Path

from tools.codediff.match import MatchedRun

#: Opcode of a near `call` taking a signed 32-bit displacement from the end of the
#: instruction. Same constant as `tools/gen_d3d8_surface.py`, same caveats.
OPCODE_CALL_REL32 = 0xE8

#: Encoded length of `call rel32`: one opcode byte plus a 4-byte displacement.
CALL_REL32_LENGTH = 5

#: The header `tools/ghidra/ExportFunctionBounds.java` writes, in order.
FUNCTION_CSV_COLUMNS = ("entry_va", "size_bytes", "name", "is_thunk", "body_max_va")


@dataclass(frozen=True)
class Function:
    """One row of a function-bounds export.

    `size_bytes` is an address *count*, not `body_max_va - entry_va + 1`: Ghidra
    function bodies may be non-contiguous, and the exporter emits both fields
    precisely so a consumer can tell the fragmented ones apart.
    """

    entry_va: int
    size_bytes: int
    name: str
    is_thunk: bool
    body_max_va: int


@dataclass(frozen=True)
class Candidate:
    """A predicted function entry the target build's own analysis does not have."""

    target_va: int
    """Predicted entry in the target build."""

    donor_va: int
    """Entry of the donor function this was mapped from."""

    donor_name: str
    donor_size: int

    call_sites: int
    """`call rel32` sites in the target's code targeting `target_va`. Evidence, not
    proof: see `count_call_sites` for why the count is an upper bound."""


@dataclass
class Correspondence:
    """The outcome of mapping a donor function table into a target build."""

    mapped: int
    """Donor entries that landed inside some matched run."""

    agreed: int
    """Of those, how many predicted an address the target also calls a function."""

    candidates: list[Candidate]
    """Of those, the ones the target has no function for. Ascending by `target_va`."""

    @property
    def agreement(self) -> float:
        """`agreed / mapped`, or 0.0 when nothing mapped at all."""
        if self.mapped == 0:
            return 0.0
        return self.agreed / self.mapped


def load_functions(path: Path) -> list[Function]:
    """Read a `tools/ghidra/ExportFunctionBounds.java` CSV.

    Columns: `entry_va` (hex, e.g. 0x00012000), `size_bytes` (decimal), `name`,
    `is_thunk` (true/false), `body_max_va` (hex).

    Rows are returned in file order, which the exporter emits ascending by entry.
    A row that does not parse raises `ValueError` naming the file, the line and the
    offending field. It is never skipped: a function table silently missing rows
    would understate `mapped` and quietly deflate every statistic computed from it.
    """
    functions: list[Function] = []
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if header is None:
            raise ValueError(f"{path}: file is empty, expected a CSV header row")
        if tuple(field.strip() for field in header) != FUNCTION_CSV_COLUMNS:
            raise ValueError(
                f"{path}:1: unexpected header {header!r}, expected {list(FUNCTION_CSV_COLUMNS)}"
            )
        for line, row in enumerate(reader, start=2):
            functions.append(_parse_row(row, path, line))
    return functions


#: Tracked corrections to the generated Ghidra table. `generated/` is gitignored and comes
#: from `tools/ghidra/ExportFunctionBounds.java`, which cannot be rerun here, so a wrong
#: size or a missing entry is fixed in this file and layered over the export on load.
DEFAULT_OVERRIDES = Path(__file__).resolve().parents[1] / "data" / "function_overrides.csv"
OVERRIDE_CSV_COLUMNS = ("entry_va", "size_bytes", "reason")


def load_function_overrides(path: Path = DEFAULT_OVERRIDES) -> dict[int, int]:
    """Read `entry_va -> size_bytes` corrections. A malformed row raises `ValueError`."""
    overrides: dict[int, int] = {}
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if header is None or tuple(field.strip() for field in header) != OVERRIDE_CSV_COLUMNS:
            raise ValueError(f"{path}:1: expected header {list(OVERRIDE_CSV_COLUMNS)}")
        for line, row in enumerate(reader, start=2):
            if len(row) != len(OVERRIDE_CSV_COLUMNS) or not row[2].strip():
                raise ValueError(f"{path}:{line}: expected entry_va,size_bytes,reason: {row!r}")
            entry = _parse_hex(row[0], "entry_va", path, line)
            if entry in overrides:
                raise ValueError(f"{path}:{line}: duplicate override for {entry:#010x}")
            overrides[entry] = _parse_decimal(row[1], "size_bytes", path, line)
    return overrides


def apply_function_overrides(
    functions: list[Function], overrides: dict[int, int]
) -> list[Function]:
    """Functions with each override's size applied, or appended as a new entry, sorted by VA."""
    merged = {function.entry_va: function for function in functions}
    for entry, size in overrides.items():
        known = merged.get(entry)
        name = known.name if known else f"FUN_{entry:08x}"
        merged[entry] = Function(entry, size, name, False, entry + size - 1)
    return [merged[entry] for entry in sorted(merged)]


#: Verified additions (T1266): code reached only through data tables, absent from the Ghidra
#: export. Produced by `tools/verify_function_additions.py`, which records the evidence.
DEFAULT_ADDITIONS = Path(__file__).resolve().parents[1] / "data" / "function_additions.csv"
ADDITION_CSV_COLUMNS = ("entry_va", "size_bytes", "evidence")


def load_function_additions(path: Path = DEFAULT_ADDITIONS) -> dict[int, int]:
    """Read `entry_va -> size_bytes` additions. Malformed rows and duplicates raise."""
    additions: dict[int, int] = {}
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if header is None or tuple(field.strip() for field in header) != ADDITION_CSV_COLUMNS:
            raise ValueError(f"{path}:1: expected header {list(ADDITION_CSV_COLUMNS)}")
        for line, row in enumerate(reader, start=2):
            if len(row) != len(ADDITION_CSV_COLUMNS) or not row[2].strip():
                raise ValueError(f"{path}:{line}: expected entry_va,size_bytes,evidence: {row!r}")
            entry = _parse_hex(row[0], "entry_va", path, line)
            if entry in additions:
                raise ValueError(f"{path}:{line}: duplicate addition for {entry:#010x}")
            size = _parse_decimal(row[1], "size_bytes", path, line)
            if size <= 0:
                raise ValueError(f"{path}:{line}: size_bytes must be positive: {size}")
            additions[entry] = size
    return additions


def apply_function_additions(
    functions: list[Function], additions: dict[int, int]
) -> list[Function]:
    """`functions` plus each addition as a new `FUN_<va>` entry, sorted by VA.

    Raises `ValueError` when an addition duplicates a listed entry, or when its byte range
    overlaps a listed function's span (entry through the larger of its size and
    `body_max_va`) or another addition. Silent acceptance would double count bytes.
    """
    spans = sorted(
        (f.entry_va, max(f.entry_va + f.size_bytes - 1, f.body_max_va)) for f in functions
    )
    known = {f.entry_va for f in functions}
    claimed: list[tuple[int, int, int]] = []
    for entry in sorted(additions):
        size = additions[entry]
        last = entry + size - 1
        if entry in known:
            raise ValueError(f"function addition {entry:#010x} duplicates a listed function")
        for lo, hi in spans:
            if lo <= last and entry <= hi:
                raise ValueError(
                    f"function addition {entry:#010x}+{size} overlaps listed function "
                    f"{lo:#010x}..{hi:#010x}"
                )
        if claimed and claimed[-1][1] >= entry:
            raise ValueError(
                f"function addition {entry:#010x} overlaps addition {claimed[-1][0]:#010x}"
            )
        claimed.append((entry, last, size))
    added = [
        Function(entry, size, f"FUN_{entry:08x}", False, entry + size - 1)
        for entry, size in additions.items()
    ]
    return sorted([*functions, *added], key=lambda f: f.entry_va)


def load_function_table(
    path: Path,
    overrides_path: Path | None = DEFAULT_OVERRIDES,
    additions_path: Path | None = DEFAULT_ADDITIONS,
) -> list[Function]:
    """THE function table: the Ghidra export, size/entry overrides, then verified additions.

    Coverage, the harness and every tool that needs the measured function set read it
    through this one loader so they all see the same denominator. Pass `None` for a
    layer to skip it (the raw export is `load_functions`).
    """
    functions = load_functions(path)
    if overrides_path is not None and overrides_path.is_file():
        functions = apply_function_overrides(functions, load_function_overrides(overrides_path))
    if additions_path is not None and additions_path.is_file():
        functions = apply_function_additions(functions, load_function_additions(additions_path))
    return functions


def _export_root(path: Path) -> Path | None:
    """`<root>` when `path` is `<root>/generated/<build>/functions.csv`, else `None`."""
    parents = Path(os.path.abspath(path)).parents  # not resolve(): keep worktree symlinks
    if path.name == "functions.csv" and len(parents) > 2 and parents[1].name == "generated":
        return parents[2]
    return None


def load_function_table_for_export(path: Path) -> list[Function]:
    """`load_function_table` with the tracked layers found from the export's location.

    For `<root>/generated/<build>/functions.csv` the layers are `<root>/tools/data/...`. Any
    other location yields the raw export, so a synthetic table in a test is never altered.
    """
    root = _export_root(path)
    if root is None:
        return load_functions(path)
    data = root / "tools" / "data"
    return load_function_table(
        path, data / "function_overrides.csv", data / "function_additions.csv"
    )


def function_table_rows(path: Path) -> list[dict[str, str]]:
    """The shared table as `functions.csv`-shaped string rows (for `csv.DictReader` users).

    Outside the `<root>/generated/<build>/functions.csv` layout the file is read verbatim, so
    synthetic tables with extra columns keep working.
    """
    if _export_root(path) is None:
        with path.open(newline="", encoding="utf-8") as handle:
            return list(csv.DictReader(handle))
    return [
        {
            "entry_va": f"0x{f.entry_va:08x}",
            "size_bytes": str(f.size_bytes),
            "name": f.name,
            "is_thunk": "true" if f.is_thunk else "false",
            "body_max_va": f"0x{f.body_max_va:08x}",
        }
        for f in load_function_table_for_export(path)
    ]


#: (bytes-back, opcode) for each instruction a function entry may follow, where the
#: distance is that instruction's total length. MSVC pads between functions with
#: `int3` or `nop`, and otherwise the preceding function ends in a return or a tail
#: jump.
#:
#: THE DISTANCE MATTERS, and getting it wrong is easy. A first version of this tested
#: only the single preceding byte against all six opcodes, which is correct for the
#: 1-byte instructions and WRONG for the rest: `ret imm16` is `C2 imm16`, so its last
#: byte is the immediate's high byte (normally 0x00), and `jmp rel8`/`jmp rel32` end
#: in a displacement. That version rejected 0x003867fd, which is preceded by a
#: perfectly good `ret 4` (`c2 04 00`), while matching stray displacement bytes that
#: happened to equal 0xC2/0xE9/0xEB.
TERMINATORS: tuple[tuple[int, int], ...] = (
    (1, 0xCC),  # int3
    (1, 0xC3),  # ret
    (1, 0x90),  # nop
    (3, 0xC2),  # ret imm16
    (2, 0xEB),  # jmp rel8
    (5, 0xE9),  # jmp rel32
)


def preceded_by_terminator(text: bytes, base_va: int, va: int) -> bool:
    """Whether a terminating instruction ends exactly where `va` begins.

    This is the single best cheap discriminator between a real function entry and a
    label in the middle of one. MEASURED on retail TSFP: 1,000 known Ghidra entries
    pass at a far higher rate than 1,000 arbitrary `.text` addresses -- see
    `docs/cross-build-diff.md` for the figures.

    Backward x86 decoding is not generally possible, so this does not attempt it: it
    checks each candidate terminator at its own length behind `va`. That can still be
    fooled by data whose bytes coincide, which is why the null rate is non-zero rather
    than zero, but it is sound in the direction that matters -- it does not reject
    real entries for being preceded by a multi-byte return.

    An address with no room for a predecessor fails rather than raising.
    """
    for distance, opcode in TERMINATORS:
        offset = va - base_va - distance
        if 0 <= offset < len(text) and text[offset] == opcode:
            return True
    return False


def filter_plausible(
    candidates: list[Candidate], text: bytes, base_va: int
) -> tuple[list[Candidate], list[Candidate]]:
    """Split candidates into (plausible, rejected) by `preceded_by_terminator`.

    WHY THIS GATE EXISTS, and why applying unfiltered candidates is actively harmful.
    Measured by creating all 106 candidates in Ghidra: the 70 landing in bytes Ghidra
    never disassembled pass this test 91.4% of the time, statistically
    indistinguishable from confirmed entries, and recover genuinely missing code. The
    36 landing *inside* an existing function pass only 25.0%, and roughly 27 of those
    are mid-function labels whose creation **destroys a correct boundary** -- ten
    reduced a correct function to a 1-4 byte stub, the worst turning a 603-byte
    function into 1 byte.

    The cautionary case is `0x0003d441`, which is `FUN_0003d440 + 1`: the byte before
    it is `0x53`, `push ebx`. Creating it decapitates the prologue, and yet it
    decompiles into 136 lines of entirely convincing level-loading code, because
    losing one `push` barely perturbs the decompiler. **Plausible decompiled output is
    not evidence of a correct boundary; the preceding byte is.**

    Returns both halves rather than silently dropping the rejects, so a caller can
    report what it discarded instead of quietly shrinking its own input.
    """
    plausible: list[Candidate] = []
    rejected: list[Candidate] = []
    for candidate in candidates:
        target = (
            plausible if preceded_by_terminator(text, base_va, candidate.target_va) else rejected
        )
        target.append(candidate)
    return plausible, rejected


def count_call_sites(text: bytes, base_va: int) -> dict[int, int]:
    """VA -> number of `call rel32` (opcode 0xE8) sites targeting it.

    `base_va` is the virtual address of `text[0]`. Only targets reachable by the
    arithmetic are reported, with no filtering by section or image bounds, so the
    caller decides what counts as a plausible destination.

    THE COUNTS OVER-COUNT, exactly as `tools/gen_d3d8_surface.py` warns about the
    same scan. A naive 0xE8 sweep has false positives, because that byte occurs
    inside other instructions and inside data, and a 4-byte window after it always
    decodes as *some* displacement. Nothing here decodes instruction lengths, so
    there is no way to tell a real call from a coincidence. Treat every figure as an
    upper bound: a non-zero count is evidence that an address is an entry point,
    never proof. What makes it useful anyway is the ranking, since scattered false
    positives land on scattered addresses and real entries accumulate sites.

    A matched 0xE8 advances the scan by the whole 5-byte instruction rather than one
    byte, so a displacement's own bytes cannot be re-read as a second opcode. That
    choice is not load-bearing: scanning retail `.text` a byte at a time instead finds
    8,967 distinct targets against 8,948, and leaves "42 of 89 candidates are
    call-targeted" unchanged. Skipping is kept because it is what
    `tools/gen_d3d8_surface.py` already does and because it cannot double-count.
    """
    counts: dict[int, int] = {}
    offset = 0
    limit = len(text) - CALL_REL32_LENGTH
    while offset <= limit:
        if text[offset] != OPCODE_CALL_REL32:
            offset += 1
            continue
        (displacement,) = struct.unpack_from("<i", text, offset + 1)
        target_va = base_va + offset + CALL_REL32_LENGTH + displacement
        counts[target_va] = counts.get(target_va, 0) + 1
        offset += CALL_REL32_LENGTH
    return counts


def correspond(
    runs: list[MatchedRun],
    donor: list[Function],
    target_entries: set[int],
    *,
    call_sites: dict[int, int] | None = None,
) -> Correspondence:
    """Map donor function entries through `runs` and report what the target lacks.

    `runs` is `find_runs(donor_text, target_text)` output, so `demo_va` is an address
    in the donor image and `retail_va` the corresponding address in the target.
    A donor entry at `donor_va` inside the half-open run `[demo_va, demo_va + length)`
    predicts `retail_va + (donor_va - demo_va)` in the target. The run's end bound is
    exclusive, matching `run_boundaries`: an entry exactly one past the last byte of a
    run is outside it, and the mapping there would be unsupported by any compared byte.

    `target_entries` is the set of entry VAs the target's own analysis already found.
    A prediction in that set counts towards `agreed`; a prediction outside it becomes
    a `Candidate`.

    `call_sites` is optional `count_call_sites` output for the *target* image. When
    omitted every candidate reports `call_sites=0`, which is the absence of evidence
    rather than evidence of absence.

    DUPLICATES ARE COLLAPSED. Two runs with different deltas can predict the same
    target address from two different donor functions. Each `target_va` appears at
    most once in `candidates`, keeping the prediction from the donor function with the
    lower `donor_va`, so the output does not depend on the order `runs` arrives in.
    `mapped` and `agreed` still count donor *entries*, so `len(candidates)` can be
    smaller than `mapped - agreed` when collapsing happens.

    Linear-ish in the inputs. Donor entries are sorted once and located per run with
    `bisect`, never rescanned, because the real inputs are ~11,000 functions against
    ~12,800 runs and the nested scan would be ~140 million comparisons.
    """
    ordered = sorted(donor, key=lambda function: function.entry_va)
    entry_vas = [function.entry_va for function in ordered]

    mapped = 0
    agreed = 0
    best: dict[int, Candidate] = {}

    for run in runs:
        lo = run.demo_va
        hi = run.demo_va + run.length
        index = bisect_left(entry_vas, lo)
        while index < len(entry_vas) and entry_vas[index] < hi:
            function = ordered[index]
            index += 1
            target_va = run.retail_va + (function.entry_va - run.demo_va)
            mapped += 1
            if target_va in target_entries:
                agreed += 1
                continue
            incumbent = best.get(target_va)
            if incumbent is not None and incumbent.donor_va <= function.entry_va:
                continue
            best[target_va] = Candidate(
                target_va=target_va,
                donor_va=function.entry_va,
                donor_name=function.name,
                donor_size=function.size_bytes,
                call_sites=0 if call_sites is None else call_sites.get(target_va, 0),
            )

    return Correspondence(
        mapped=mapped,
        agreed=agreed,
        candidates=[best[key] for key in sorted(best)],
    )


def _parse_row(row: list[str], path: Path, line: int) -> Function:
    """One CSV data row as a `Function`, or `ValueError` naming what went wrong."""
    if len(row) != len(FUNCTION_CSV_COLUMNS):
        raise ValueError(
            f"{path}:{line}: expected {len(FUNCTION_CSV_COLUMNS)} fields, got {len(row)}: {row!r}"
        )
    entry_va, size_bytes, name, is_thunk, body_max_va = row
    return Function(
        entry_va=_parse_hex(entry_va, "entry_va", path, line),
        size_bytes=_parse_decimal(size_bytes, "size_bytes", path, line),
        name=name,
        is_thunk=_parse_bool(is_thunk, "is_thunk", path, line),
        body_max_va=_parse_hex(body_max_va, "body_max_va", path, line),
    )


def _parse_hex(value: str, column: str, path: Path, line: int) -> int:
    text = value.strip()
    try:
        return int(text, 16)
    except ValueError:
        raise ValueError(f"{path}:{line}: {column} is not hex: {value!r}") from None


def _parse_decimal(value: str, column: str, path: Path, line: int) -> int:
    text = value.strip()
    try:
        return int(text, 10)
    except ValueError:
        raise ValueError(f"{path}:{line}: {column} is not a decimal integer: {value!r}") from None


def _parse_bool(value: str, column: str, path: Path, line: int) -> bool:
    text = value.strip().lower()
    if text == "true":
        return True
    if text == "false":
        return False
    raise ValueError(f"{path}:{line}: {column} is not true/false: {value!r}")
