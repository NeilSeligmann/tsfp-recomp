# SPDX-License-Identifier: GPL-3.0-or-later
"""T1576 arm-event witness: which `case` arms a replacement draft SOURCE reaches per input.

Design (docs/evidence/t1576/arm-witness.md). The producers design proposed clang
`trace-pc-guard` plus a DWARF line map. Measured on clang 19 -O0 that map is unreliable
(case-entry guards carry the `switch` line and a fall-through creates no guard), so this uses
clang source-based coverage (`-fprofile-instr-generate -fcoverage-mapping`) on a separate O0
WITNESS build instead. The compiler itself emits one counted region per `case`/`default`
label, starting at the label's line and column, and a region's count includes arrivals by
fall-through. The mapping region -> label reads the label text AT the compiler's region
position; the draft author places nothing.

Soundness statement (unchanged from the design): the draft SOURCE, compiled at O0, reached
these arms on these inputs. It does not claim the shipped O3 binary took them. Fail closed:
more than one `switch` in the entry function, a missing `default`, a non-literal case value
or a label with no compiler region all refuse.

Reached semantics. A region count is "the arm's code ran" (dispatch or fall-through). The
original side is therefore "the table target VA is in the instruction coverage" for the same
input, which is also dispatch or fall-through. Labels stacked with nothing between them
(`case 1: case 2:`) share one body and form a GROUP: the original cannot tell them apart
either when their targets coincide. Hit sets are compared at label level after expanding each
side's hits to whole groups.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

CLANG = "clang"
PROFDATA = "llvm-profdata-19"
LLVM_COV = "llvm-cov-19"
WITNESS_CFLAGS = ("-O0", "-g", "-fprofile-instr-generate", "-fcoverage-mapping")

#: Included by a runner. Reset before the call, dump after it: one profile per input.
RUNTIME_HEADER = """\
extern void __llvm_profile_reset_counters(void);
extern void __llvm_profile_set_filename(const char *);
extern int __llvm_profile_write_file(void);
static void witness_begin(void) { __llvm_profile_reset_counters(); }
static void witness_end(const char *path) {
    __llvm_profile_set_filename(path);
    __llvm_profile_write_file();
}
"""

_LABEL = re.compile(r"\A(case\s+(-?(?:0[xX][0-9a-fA-F]+|\d+))\s*:|default\s*:)")


class WitnessRefusal(ValueError):
    """The draft or the build is outside what the witness can observe soundly."""


@dataclass(frozen=True)
class Label:
    kind: str  # "case" or "default"
    value: int | None
    line: int
    col: int
    count: int

    @property
    def name(self) -> str:
        return "default" if self.kind == "default" else f"case:{self.value}"


def runner_source(entry: str) -> str:
    """Synthetic `int entry(int)` runner, one profile per argv input (tests and examples)."""
    return (
        '#include <stdio.h>\n#include <stdlib.h>\n#include "witness_rt.h"\n'
        f"extern int {entry}(int);\n"
        "int main(int argc, char **argv) {\n"
        "    for (int i = 2; i < argc; i++) {\n"
        '        char path[512]; snprintf(path, sizeof path, "%s.%d.profraw", argv[1], i - 2);\n'
        "        witness_begin();\n"
        f"        int r = {entry}(atoi(argv[i]));\n"
        "        witness_end(path);\n"
        '        printf("%d\\n", r);\n'
        "    }\n    return 0;\n}\n"
    )


def build_witness(
    sources: Sequence[Path], runner: Path, out: Path, *, extra: Sequence[str] = ()
) -> Path:
    """Compile draft sources + runner as an O0 coverage-mapped binary (clang only)."""
    (runner.parent / "witness_rt.h").write_text(RUNTIME_HEADER, encoding="utf-8")
    cmd = [CLANG, *WITNESS_CFLAGS, f"-I{runner.parent}", *extra]
    cmd += [str(s) for s in sources] + [str(runner), "-o", str(out)]
    subprocess.run(cmd, check=True, capture_output=True, timeout=300)
    return out


def run_witness(binary: Path, prefix: Path, inputs: Sequence[int]) -> list[Path]:
    """Run once; returns one raw profile per input, in order."""
    subprocess.run(
        [str(binary), str(prefix), *[str(i) for i in inputs]],
        check=True,
        capture_output=True,
        timeout=600,
        env={**os.environ, "LLVM_PROFILE_FILE": os.devnull},
    )
    return [Path(f"{prefix}.{i}.profraw") for i in range(len(inputs))]


def export_all(binary: Path, profraw: Path) -> dict[str, dict]:
    """llvm-cov export of every mapped function (regions and execution count) by name."""
    data = profraw.with_suffix(".profdata")
    subprocess.run(
        [PROFDATA, "merge", "-sparse", str(profraw), "-o", str(data)],
        check=True,
        capture_output=True,
        timeout=120,
    )
    done = subprocess.run(
        [LLVM_COV, "export", "-format=text", str(binary), f"-instr-profile={data}"],
        check=True,
        capture_output=True,
        timeout=120,
    )
    return {entry["name"]: entry for entry in json.loads(done.stdout)["data"][0]["functions"]}


def find_function(functions: Mapping[str, dict], function: str) -> dict:
    """A mapped function by name. A `static` function is exported as `file.c:name`; a name
    matching more than one file is ambiguous and refused rather than guessed."""
    if function in functions:
        return functions[function]
    matches = [entry for name, entry in functions.items() if name.endswith(f":{function}")]
    if len(matches) != 1:
        raise WitnessRefusal(f"function {function} has {len(matches)} coverage mappings")
    return matches[0]


def export_function(binary: Path, profraw: Path, function: str) -> dict:
    return find_function(export_all(binary, profraw), function)


def _label_at(lines: Sequence[str], line: int, col: int) -> re.Match[str] | None:
    return _LABEL.match(lines[line - 1][col - 1 :])


def static_labels(
    source: str, function_entry: Mapping, *, filename: str | None = None
) -> list[Label]:
    """Case/default labels of the entry function, read at the compiler's region positions.

    `filename` (the draft's basename) drops regions of other files, which a macro-defined entry
    function can contain (header expansions). Their line/column are not draft positions."""
    lines = source.splitlines()
    out: list[Label] = []
    files = function_entry.get("filenames", [])
    for region in function_entry["regions"]:
        line, col, _el, _ec, count, file_id, _x, kind = region
        if kind != 0:
            continue
        if filename is not None and not str(files[file_id]).endswith(filename):
            continue
        match = _label_at(lines, line, col)
        if match is None:
            continue
        value = None if match.group(2) is None else int(match.group(2), 0)
        out.append(Label("default" if value is None else "case", value, line, col, count))
    return out


def refusals(source: str, labels: Sequence[Label], function: str) -> list[str]:
    """Static fail-closed checks on the draft (independent of any input)."""
    why: list[str] = []
    body = _function_text(source, function)
    if len(re.findall(r"\bswitch\s*\(", body)) != 1:
        why.append("entry function must contain exactly one switch")
    if not any(label.kind == "default" for label in labels):
        why.append("switch has no default label (post-switch code is not an observable arm)")
    case_values = [label.value for label in labels if label.kind == "case"]
    if len(set(case_values)) != len(case_values):
        why.append("duplicate case values")
    literal = len(re.findall(r"\bcase\s+-?(?:0[xX][0-9a-fA-F]+|\d+)\s*:", body))
    if literal != len(case_values) or len(re.findall(r"\bcase\b", body)) != literal:
        why.append("a case label is not an integer literal or has no compiler region")
    return why


def _function_text(source: str, function: str) -> str:
    match = re.search(rf"\b{re.escape(function)}\s*\([^)]*\)\s*{{", source)
    if match is None:
        # A GAME_REPLACE_EXACT body follows the registration macro that names the function.
        match = re.search(rf"\bGAME_REPLACE\w*\([^)]*\b{re.escape(function)}\s*\)\s*{{", source)
    if match is None:
        raise WitnessRefusal(f"function {function} not found in the draft source")
    depth, index = 1, match.end()
    while depth and index < len(source):
        depth += {"{": 1, "}": -1}.get(source[index], 0)
        index += 1
    return source[match.start() : index]


def groups(source: str, labels: Sequence[Label]) -> list[frozenset[str]]:
    """Labels stacked with only whitespace between them share one body."""
    lines = source.splitlines()
    ordered = sorted(labels, key=lambda label: (label.line, label.col))
    result: list[set[str]] = []
    previous: Label | None = None
    for label in ordered:
        if previous is not None:
            start = _label_at(lines, previous.line, previous.col)
            assert start is not None
            between = _text_between(lines, previous.line, previous.col + start.end(), label)
            if between.strip() == "":
                result[-1].add(label.name)
                previous = label
                continue
        result.append({label.name})
        previous = label
    return [frozenset(group) for group in result]


def _text_between(lines: Sequence[str], line: int, col: int, label: Label) -> str:
    if line == label.line:
        return lines[line - 1][col - 1 : label.col - 1]
    parts = [
        lines[line - 1][col - 1 :],
        *lines[line : label.line - 1],
        lines[label.line - 1][: label.col - 1],
    ]
    return "\n".join(parts)


def reached_labels(
    labels: Sequence[Label], label_groups: Sequence[frozenset[str]]
) -> frozenset[str]:
    """Replacement hit set: a group is reached when any member region ran."""
    counts = {label.name: label.count for label in labels}
    hit: set[str] = set()
    for group in label_groups:
        if any(counts[name] > 0 for name in group):
            hit |= group
    return frozenset(hit)


@dataclass(frozen=True)
class OriginalTable:
    """Decoded from the ORIGINAL bytes (static_tables), never from the draft."""

    slot_targets: tuple[int, ...]
    default_target: int
    #: Draft-supplied: case value of each original slot. Mutation-tested, not trusted.
    slot_case_values: tuple[int, ...]


def original_arm_hits(table: OriginalTable, covered_vas: Iterable[int]) -> frozenset[str]:
    """Original-side hit set from the emulator's instruction coverage for one input."""
    covered = set(covered_vas)
    hit: set[str] = set()
    for slot, target in enumerate(table.slot_targets):
        if target in covered:
            hit.add(f"case:{table.slot_case_values[slot]}")
    if table.default_target in covered:
        hit.add("default")
    return frozenset(hit)


def expand_original(hits: frozenset[str], table: OriginalTable) -> frozenset[str]:
    """Slots sharing a target VA are indistinguishable to the original: expand to the group."""
    by_target: dict[int, set[str]] = {}
    for slot, target in enumerate(table.slot_targets):
        by_target.setdefault(target, set()).add(f"case:{table.slot_case_values[slot]}")
    by_target.setdefault(table.default_target, set()).add("default")
    out = set(hits)
    for members in by_target.values():
        if members & hits:
            out |= members
    return frozenset(out)


def arm_parity(
    table: OriginalTable,
    covered_vas_per_case: Sequence[Iterable[int]],
    replacement_hits_per_case: Sequence[frozenset[str]],
) -> list[tuple[int, frozenset[str], frozenset[str]]]:
    """Per-case (index, original_set, replacement_set) for every case where they differ.

    An empty result is the guard's pass. Callers must also require the campaign union to cover
    every declared slot and the default (`uncovered`), or equality could be vacuous.
    """
    if len(covered_vas_per_case) != len(replacement_hits_per_case) or not covered_vas_per_case:
        raise WitnessRefusal("case streams are empty or of different length")
    bad = []
    for index, (covered, replacement) in enumerate(
        zip(covered_vas_per_case, replacement_hits_per_case, strict=True)
    ):
        original = expand_original(original_arm_hits(table, covered), table)
        if original != replacement:
            bad.append((index, original, replacement))
    return bad


def uncovered(
    table: OriginalTable, replacement_hits_per_case: Sequence[frozenset[str]]
) -> frozenset[str]:
    """Declared arms (every slot's case value and default) that no case ever reached."""
    union: set[str] = set()
    for hits in replacement_hits_per_case:
        union |= hits
    declared = {f"case:{value}" for value in table.slot_case_values} | {"default"}
    return frozenset(declared - union)


def boundary_inputs(slots: int, first_case_value: int) -> list[int]:
    """Inputs that probe the index bound from the ORIGINAL slot count: every slot, one below
    the table, and the first value past it (default), plus a far value."""
    first, last = first_case_value, first_case_value + slots - 1
    return [*range(first - 1, last + 3), first + 1000]


def function_called(binary: Path, profraw: Path, function: str) -> bool:
    """Tail-target witness: did the named function run (compiler counted)?"""
    return export_function(binary, profraw, function)["count"] > 0
