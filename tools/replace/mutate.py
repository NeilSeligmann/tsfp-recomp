# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutation testing of the replacements: proof that the equivalence check can FAIL.

A differential proof that has never failed is not evidence. For every registered
function this module makes small single-site mutants of the function's OWN body in a
scratch copy of src/game, builds the harness subject with the mutant linked, runs the
harness on that one function, and records whether the mutant was

* KILLED: the function's proof entry shows `disagree + subject_faulted > 0`,
* SURVIVED: every verdict agreed, so either the mutant is equivalent (a human must say
  so) or the harness inputs cannot tell it from the original, which is the finding,
* NOT-A-MUTANT: it did not compile (the lesson of docs/xdk-dispatch.md section 8: a
  mutation that does not build looks exactly like evidence and is not), or
* WIRING: the subject did not route the call to the replacement (exit 5), or
* ERROR: anything else (a timeout, a missing proof entry). Never counted as a kill.

Nothing under src/game is ever written. Every mutant lives in
`tmp/replace/mut/<id>/{game,work,out}` and is removed afterwards unless `--keep`.

THE UNIT OF A MUTANT. `Mutant.original` and `Mutant.mutated` are the WHOLE SOURCE LINE
before and after, and `Mutant.line` is 1-based, so `apply_mutant` needs no column: it
swaps that one line, after checking the line still reads as `original`.

WHAT IS MUTATED. Only text between the braces of a registered function's body, and
never a comment, a string or character literal, a preprocessor line or the
`GAME_REPLACE` line. Operators, each applied at one site at a time:

    int+1 / int-1        integer literal N to N+1, and to N-1 when N > 0
    eq-to-ne / ne-to-eq  ==  <->  !=
    lt-to-le gt-to-ge le-to-lt ge-to-gt
    and-to-or / or-to-and  &&  <->  ||
    bitand-to-bitor / bitor-to-bitand  binary &  <->  |
    add-to-sub / sub-to-add  binary +  <->  -
    shl-to-shr / shr-to-shl  <<  <->  >>
    delete-write         a whole `guest_write*(...);` statement line becomes `;`

`extended=True` (CLI `--extended`) adds compound assignments (`|=` `&=` `+=` `-=` `<<=`
`>>=` swapped with their opposite), dropping a unary `~`, and dropping a unary `!`.

DEFINES. The brief for this tool forbids mutating `#define` lines, but most field
offsets and masks in src/game live in `#define`s and a body mutation cannot reach them.
`include_defines=True` (CLI `--include-defines`) therefore also mutates the literals of
object-like `#define`s whose name the function's body uses (transitively), attributed to
that function. It is off by default so the default behaviour is the specified one.

    python3 -m tools.replace.mutate --jobs 4 --opt-level 2
"""

from __future__ import annotations

import argparse
import bisect
import json
import re
import shutil
import subprocess
import sys
import threading
from collections.abc import Callable, Iterable, Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from .build import BuildError, prepare_baseline
from .cli import PARTIAL_DIR_NAME
from .scan import Registration, ScanError, scan_directory

MUTATION_SCHEMA = 1
DEFAULT_GAME_DIR = Path("src/game")
DEFAULT_OUT = Path("generated/replace/mutation.json")
DEFAULT_SCRATCH_ROOT = Path("tmp/replace/mut")
DEFAULT_BASE_DIR = Path("tmp/replace/stub-base")
DEFAULT_MUTANT_TIMEOUT_SECONDS = 900

KILLED = "killed"
SURVIVED = "survived"
NOT_A_MUTANT = "not-a-mutant"
WIRING = "wiring"
ERROR = "error"
BASELINE_OK = "baseline-ok"
BASELINE_FAILED = "baseline-failed"
BASELINE_OPERATOR = "baseline"

EXIT_BUILD_FAILURE = 2
EXIT_NOT_REPLACED = 5

# --------------------------------------------------------------------------------------
# Lexing: a copy of the source where only code survives
# --------------------------------------------------------------------------------------


def mask_code(text: str, *, keep_directives: bool = False) -> str:
    """`text` with comments and string/character literal contents blanked to spaces.

    Same length and same newlines as the input, so an offset in the mask is an offset in
    the source. Preprocessor lines (with their `\\` continuations) are blanked too unless
    `keep_directives`, which is what keeps a brace in a `#define` out of brace matching.
    """
    out = list(text)
    length = len(text)
    index = 0
    while index < length:
        char = text[index]
        pair = text[index : index + 2]
        if pair == "//":
            end = text.find("\n", index)
            end = length if end < 0 else end
            _blank(out, index, end)
            index = end
        elif pair == "/*":
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            _blank(out, index, end)
            index = end
        elif char in "\"'":
            end = _literal_end(text, index)
            _blank(out, index + 1, end - 1)
            index = end
        else:
            index += 1
    masked = "".join(out)
    if keep_directives:
        return masked
    return _blank_directives(masked)


def _blank(out: list[str], start: int, end: int) -> None:
    for position in range(start, end):
        if out[position] != "\n":
            out[position] = " "


def _literal_end(text: str, start: int) -> int:
    """Index just past the string or character literal that opens at `start`."""
    quote = text[start]
    index = start + 1
    while index < len(text):
        if text[index] == "\\":
            index += 2
            continue
        if text[index] == quote or text[index] == "\n":
            return index + 1
        index += 1
    return len(text)


def _blank_directives(masked: str) -> str:
    lines = masked.split("\n")
    in_directive = False
    for number, line in enumerate(lines):
        if in_directive or line.lstrip().startswith("#"):
            in_directive = line.rstrip().endswith("\\")
            lines[number] = " " * len(line)
    return "\n".join(lines)


def _line_starts(text: str) -> list[int]:
    starts = [0]
    for match in re.finditer("\n", text):
        starts.append(match.end())
    return starts


def _match_closer(masked: str, open_index: int) -> int:
    """Index of the bracket that closes the one at `open_index`, or -1."""
    opener = masked[open_index]
    closer = {"{": "}", "(": ")", "[": "]"}[opener]
    depth = 0
    for index in range(open_index, len(masked)):
        if masked[index] == opener:
            depth += 1
        elif masked[index] == closer:
            depth -= 1
            if depth == 0:
                return index
    return -1


FUNCTION_HEAD = re.compile(r"^[ \t]*static\b[^;{}()]*?\b([A-Za-z_]\w*)[ \t]*\(", re.MULTILINE)


def _function_spans(masked: str) -> dict[str, tuple[int, int]]:
    """Function name to the offsets of the `{` and `}` of its definition."""
    spans: dict[str, tuple[int, int]] = {}
    for head in FUNCTION_HEAD.finditer(masked):
        close_paren = _match_closer(masked, head.end() - 1)
        if close_paren < 0:
            continue
        rest = masked[close_paren + 1 :]
        stripped = rest.lstrip()
        if not stripped.startswith("{"):
            continue
        open_brace = close_paren + 1 + (len(rest) - len(stripped))
        close_brace = _match_closer(masked, open_brace)
        if close_brace >= 0:
            spans[head.group(1)] = (open_brace, close_brace)
    return spans


def function_regions(source_text: str) -> dict[str, tuple[int, int]]:
    """Function name to `(first_line, last_line)`, 1-based inclusive, of its body.

    The body runs from the line holding the opening `{` to the line holding the matching
    `}` (for a signature on one line and the brace on the next, that is the brace line).
    Functions are found by brace matching from a line that begins `static ... name(`,
    ignoring braces in comments, strings, character literals and preprocessor lines. A
    `static` declaration with no body is not a function here.
    """
    masked = mask_code(source_text)
    starts = _line_starts(source_text)
    return {
        name: (bisect.bisect_right(starts, opener), bisect.bisect_right(starts, closer))
        for name, (opener, closer) in _function_spans(masked).items()
    }


# --------------------------------------------------------------------------------------
# Operators
# --------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Mutant:
    function: str
    va: int
    source: str
    line: int
    operator: str
    original: str
    mutated: str


@dataclass(frozen=True)
class Site:
    """One place to mutate: `masked[start:end]` becomes `replacement`."""

    start: int
    end: int
    replacement: str
    operator: str


LITERAL = re.compile(r"(?<![\w.])(0[xX][0-9A-Fa-f]+|[0-9]+)([uUlL]*)(?![\w.])")
#: A swap table row is (regex, replacement, operator). Every regex is one token wide.
SPEC_SWAPS: tuple[tuple[re.Pattern[str], str, str], ...] = (
    (re.compile(r"(?<![=!<>+\-*/%&|^])==(?!=)"), "!=", "eq-to-ne"),
    (re.compile(r"!=(?!=)"), "==", "ne-to-eq"),
    (re.compile(r"(?<!<)<(?![<=])"), "<=", "lt-to-le"),
    (re.compile(r"(?<![->])>(?![>=])"), ">=", "gt-to-ge"),
    (re.compile(r"(?<![<])<=(?!=)"), "<", "le-to-lt"),
    (re.compile(r"(?<![>])>=(?!=)"), ">", "ge-to-gt"),
    (re.compile(r"&&"), "||", "and-to-or"),
    (re.compile(r"\|\|"), "&&", "or-to-and"),
    (re.compile(r"<<(?!=)"), ">>", "shl-to-shr"),
    (re.compile(r"(?<![-])>>(?!=)"), "<<", "shr-to-shl"),
)
#: Operators that are only an operator between two operands, never a unary prefix.
BINARY_SWAPS: tuple[tuple[re.Pattern[str], str, str], ...] = (
    (re.compile(r"(?<![&])&(?![&=])"), "|", "bitand-to-bitor"),
    (re.compile(r"(?<![|])\|(?![|=])"), "&", "bitor-to-bitand"),
    (re.compile(r"(?<![+])\+(?![+=])"), "-", "add-to-sub"),
    (re.compile(r"(?<![-])-(?![-=>])"), "+", "sub-to-add"),
)
EXTENDED_SWAPS: tuple[tuple[re.Pattern[str], str, str], ...] = (
    (re.compile(r"(?<![<>])\|=(?!=)"), "&=", "orassign-to-andassign"),
    (re.compile(r"(?<![<>])&=(?!=)"), "|=", "andassign-to-orassign"),
    (re.compile(r"(?<![<>])\+=(?!=)"), "-=", "addassign-to-subassign"),
    (re.compile(r"(?<![<>])-=(?!=)"), "+=", "subassign-to-addassign"),
    (re.compile(r"(?<![<>])<<=(?!=)"), ">>=", "shlassign-to-shrassign"),
    (re.compile(r"(?<![<>])>>=(?!=)"), "<<=", "shrassign-to-shlassign"),
)
UNARY_TILDE = re.compile(r"~")
UNARY_NOT = re.compile(r"!(?!=)")
GUEST_WRITE_STATEMENT = re.compile(r"^[ \t]*guest_write\w*\s*\(.*\)\s*;[ \t]*$")
TYPE_KEYWORDS = frozenset(
    {"int", "char", "short", "long", "unsigned", "signed", "float", "double", "void", "const"}
)
PREFIX_KEYWORDS = frozenset({"return", "case", "sizeof", "else", "do", "goto"})
WORD_BEFORE = re.compile(r"([A-Za-z_]\w*)\s*$")


def _is_cast(inner: str) -> bool:
    """True when the text between a pair of parentheses reads as a type name."""
    words = re.findall(r"[A-Za-z_]\w*", inner)
    if not words or re.sub(r"[A-Za-z_\w\s*]", "", inner):
        return False
    return all(word in TYPE_KEYWORDS or word.endswith("_t") for word in words)


def _is_binary_position(masked: str, operator_start: int) -> bool:
    """True when the operator at `operator_start` has an operand on its left."""
    before = masked[:operator_start].rstrip()
    if not before:
        return False
    last = before[-1]
    if last == ")":
        opener = _open_for(before, len(before) - 1)
        return not (opener >= 0 and _is_cast(before[opener + 1 : -1]))
    if last == "]":
        return True
    if last.isalnum() or last == "_":
        word = WORD_BEFORE.search(before)
        return word is None or word.group(1) not in PREFIX_KEYWORDS
    return False


def _open_for(text: str, close_index: int) -> int:
    depth = 0
    for index in range(close_index, -1, -1):
        if text[index] == ")":
            depth += 1
        elif text[index] == "(":
            depth -= 1
            if depth == 0:
                return index
    return -1


def _literal_sites(masked: str, start: int, end: int) -> Iterable[Site]:
    for match in LITERAL.finditer(masked, start, end):
        digits, suffix = match.group(1), match.group(2)
        if len(digits) > 1 and digits[0] == "0" and digits[1] not in "xX":
            continue  # octal, not worth the ambiguity
        hexadecimal = digits[:2] in ("0x", "0X")
        value = int(digits, 16 if hexadecimal else 10)
        for delta, name in ((1, "int+1"), (-1, "int-1")):
            changed = value + delta
            if changed < 0 or changed > 0xFFFFFFFF:
                continue
            if hexadecimal:
                body = digits[2:]
                lowered = any(c in "abcdef" for c in body) and not any(c in "ABCDEF" for c in body)
                text = f"{changed:0{len(body)}{'x' if lowered else 'X'}}"
                text = digits[:2] + text + suffix
            else:
                text = f"{changed}{suffix}"
            yield Site(match.start(), match.end(), text, name)


def _swap_sites(
    masked: str,
    start: int,
    end: int,
    table: Iterable[tuple[re.Pattern[str], str, str]],
    *,
    binary_only: bool,
) -> Iterable[Site]:
    for pattern, replacement, name in table:
        for match in pattern.finditer(masked, start, end):
            if binary_only and not _is_binary_position(masked, match.start()):
                continue
            yield Site(match.start(), match.end(), replacement, name)


def _unary_sites(masked: str, start: int, end: int) -> Iterable[Site]:
    for pattern, name in ((UNARY_TILDE, "drop-tilde"), (UNARY_NOT, "drop-not")):
        for match in pattern.finditer(masked, start, end):
            yield Site(match.start(), match.end(), "", name)


def _expression_sites(masked: str, start: int, end: int, *, extended: bool) -> list[Site]:
    sites = list(_literal_sites(masked, start, end))
    sites += _swap_sites(masked, start, end, SPEC_SWAPS, binary_only=False)
    sites += _swap_sites(masked, start, end, BINARY_SWAPS, binary_only=True)
    if extended:
        sites += _swap_sites(masked, start, end, EXTENDED_SWAPS, binary_only=False)
        sites += _unary_sites(masked, start, end)
    return sites


# --------------------------------------------------------------------------------------
# Generating and applying
# --------------------------------------------------------------------------------------

DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(?!\()[ \t]+(.*)$", re.MULTILINE)
IDENTIFIER = re.compile(r"[A-Za-z_]\w*")


def _define_lines(commentless: str) -> dict[str, tuple[int, int]]:
    """Object-like defines of one file: name to the `(start, end)` of its value text."""
    found: dict[str, tuple[int, int]] = {}
    for match in DEFINE.finditer(commentless):
        if match.group(2).rstrip().endswith("\\"):
            continue  # a continued define is a macro with a body, not a constant
        found[match.group(1)] = (match.start(2), match.end(2))
    return found


def _reachable_defines(
    commentless: str, defines: dict[str, tuple[int, int]], body: tuple[int, int]
) -> list[str]:
    """Defines the body names, then the defines their values name, and so on."""
    wanted: list[str] = []
    pending = list(IDENTIFIER.findall(commentless[body[0] : body[1]]))
    while pending:
        name = pending.pop()
        if name in defines and name not in wanted:
            wanted.append(name)
            start, end = defines[name]
            pending.extend(IDENTIFIER.findall(commentless[start:end]))
    return sorted(wanted, key=lambda name: defines[name][0])


def _delete_write_sites(masked: str, original: str, start: int, end: int) -> list[Site]:
    sites: list[Site] = []
    starts = _line_starts(masked)
    first = bisect.bisect_right(starts, start)
    last = bisect.bisect_right(starts, end)
    for number in range(first + 1, last):  # strictly between the brace lines
        begin = starts[number - 1]
        stop = starts[number] - 1 if number < len(starts) else len(masked)
        line = masked[begin:stop]
        if not GUEST_WRITE_STATEMENT.match(line) or line.count("(") != line.count(")"):
            continue
        indent = len(line) - len(line.lstrip())
        sites.append(Site(begin + indent, begin + len(line.rstrip()), ";", "delete-write"))
    return sites


def generate_mutants(
    source_text: str,
    source_name: str,
    registrations: Iterable[Registration],
    *,
    extended: bool = False,
    include_defines: bool = False,
) -> list[Mutant]:
    """Every single-site mutant of the registered functions' bodies in `source_text`.

    Registrations whose function is not defined in this text are ignored. A mutation
    that would leave the line unchanged is dropped, and so is a second mutation that
    produces the same line as an earlier one for the same function.
    """
    masked = mask_code(source_text)
    spans = _function_spans(masked)
    starts = _line_starts(source_text)
    lines = source_text.split("\n")
    commentless = mask_code(source_text, keep_directives=True)
    defines = _define_lines(commentless) if include_defines else {}
    mutants: list[Mutant] = []
    for registration in registrations:
        span = spans.get(registration.function)
        if span is None:
            continue
        opener, closer = span
        sites = _expression_sites(masked, opener + 1, closer, extended=extended)
        sites += _delete_write_sites(masked, source_text, opener, closer)
        if include_defines:
            for name in _reachable_defines(commentless, defines, (opener, closer)):
                start, end = defines[name]
                sites += _expression_sites(commentless, start, end, extended=extended)
        seen: set[tuple[int, str]] = set()
        for site in sorted(sites, key=lambda item: (item.start, item.operator)):
            number = bisect.bisect_right(starts, site.start)
            line_start = starts[number - 1]
            original = lines[number - 1]
            column = site.start - line_start
            mutated = original[:column] + site.replacement + original[site.end - line_start :]
            if site.operator == "delete-write":
                mutated = original[:column] + ";"
            if mutated == original or (number, mutated) in seen:
                continue
            seen.add((number, mutated))
            mutants.append(
                Mutant(
                    function=registration.function,
                    va=registration.va,
                    source=source_name,
                    line=number,
                    operator=site.operator,
                    original=original,
                    mutated=mutated,
                )
            )
    return mutants


def apply_mutant(source_text: str, mutant: Mutant) -> str:
    """`source_text` with the mutant's one line replaced. Raises if the line is not as recorded."""
    if mutant.operator == BASELINE_OPERATOR:
        return source_text
    lines = source_text.split("\n")
    if not 1 <= mutant.line <= len(lines):
        raise ValueError(f"{mutant.source}: line {mutant.line} is out of range")
    if lines[mutant.line - 1] != mutant.original:
        raise ValueError(
            f"{mutant.source}:{mutant.line} no longer reads {mutant.original!r}, "
            f"it reads {lines[mutant.line - 1]!r}"
        )
    lines[mutant.line - 1] = mutant.mutated
    return "\n".join(lines)


def baseline_mutant(registration: Registration) -> Mutant:
    """A mutant that changes nothing: the control every function is run through first."""
    return Mutant(
        function=registration.function,
        va=registration.va,
        source=registration.source,
        line=0,
        operator=BASELINE_OPERATOR,
        original="",
        mutated="",
    )


def sample_mutants(mutants: Sequence[Mutant], limit: int | None) -> list[Mutant]:
    """At most `limit` mutants, evenly spread so no region is dropped wholesale."""
    if limit is None or len(mutants) <= limit:
        return list(mutants)
    return [mutants[(index * len(mutants)) // limit] for index in range(limit)]


# --------------------------------------------------------------------------------------
# Running
# --------------------------------------------------------------------------------------


@dataclass
class MutantResult:
    mutant: Mutant
    status: str
    detail: str = ""
    disagree: int = 0
    subject_faulted: int = 0
    verdicts: int = 0


def mutant_id(mutant: Mutant, ordinal: int) -> str:
    return f"{mutant.va:08X}-{'base' if mutant.operator == BASELINE_OPERATOR else f'{ordinal:03d}'}"


def classify(
    exit_code: int,
    mutant: Mutant,
    proof_path: Path,
    work_dir: Path,
    log_tail: str,
) -> MutantResult:
    """Turn one `prove` run into a status, believing the proof file and nothing else."""
    if exit_code == EXIT_BUILD_FAILURE:
        return MutantResult(mutant, NOT_A_MUTANT, _first_error(log_tail))
    if exit_code == EXIT_NOT_REPLACED:
        return MutantResult(mutant, WIRING, "exit 5: the subject did not reach the replacement")
    if not proof_path.is_file():
        return MutantResult(mutant, ERROR, f"exit {exit_code} and no proof.json: {log_tail[-300:]}")
    document = json.loads(proof_path.read_text(encoding="utf-8"))
    entry = next(
        (f for f in document["functions"] if int(f["va"], 16) == mutant.va),
        None,
    )
    if entry is None or entry.get("unjudgeable"):
        reason = "no proof entry" if entry is None else entry["unjudgeable"]
        return MutantResult(mutant, ERROR, f"exit {exit_code}: {reason}")
    # The subject must be the one this run built: a stale subject would judge the wrong code.
    if str(work_dir) not in document["subject"].get("extra_objs", ""):
        return MutantResult(mutant, ERROR, "the proof's subject was not built in this work dir")
    bad = entry["disagree"] + entry["subject_faulted"]
    result = MutantResult(
        mutant,
        KILLED if bad > 0 else SURVIVED,
        "; ".join(entry["divergence_examples"][:1]),
        disagree=entry["disagree"],
        subject_faulted=entry["subject_faulted"],
        verdicts=entry["verdicts"],
    )
    if bad == 0 and (entry["verdicts"] == 0 or not entry["replaced_confirmed"]):
        result.status = ERROR
        result.detail = "no verdicts or replacement not confirmed, so nothing was judged"
    if mutant.operator == BASELINE_OPERATOR:
        result.status = BASELINE_OK if result.status == SURVIVED else BASELINE_FAILED
    return result


COMPILER_ERROR = re.compile(r"(^|\s)(error|fatal error):\s")


def _first_error(log_tail: str) -> str:
    """The first compiler diagnostic in a build log, not the command line that precedes it."""
    lines = [line.strip() for line in log_tail.splitlines() if line.strip()]
    for line in lines:
        if COMPILER_ERROR.search(line) and not line.startswith(("error: cc ", "cc ")):
            return line[:240]
    return lines[-1][:240] if lines else "build failed"


def prove_command(
    mutant: Mutant,
    *,
    opt_level: int,
    scratch_game: Path,
    root: Path,
    base_dir: Path,
    cases_per_function: int | None,
    functions: Path | None,
    prove_args: Sequence[str] = (),
) -> list[str]:
    """The `tools.replace prove` command for one mutant (`prove_args` are appended verbatim,
    e.g. T1773 `--synth-domain`, so the same mutants can be judged under another domain)."""
    command = [
        sys.executable,
        "-m",
        "tools.replace",
        "prove",
        "--opt-level",
        str(opt_level),
        "--game-dir",
        str(scratch_game),
        "--work-dir",
        str(root / "work"),
        "--base-dir",
        str(base_dir),
        "--out-dir",
        str(root / "out"),
        "--only-va",
        f"{mutant.va:x}",
    ]
    if cases_per_function is not None:
        command += ["--cases-per-function", str(cases_per_function)]
    if functions is not None:
        command += ["--functions", str(functions)]
    return [*command, *prove_args]


def run_one(
    mutant: Mutant,
    ordinal: int,
    *,
    game_dir: Path,
    base_dir: Path,
    scratch_root: Path,
    opt_level: int,
    keep: bool,
    cases_per_function: int | None,
    timeout_seconds: int,
    functions: Path | None = None,
    prove_args: Sequence[str] = (),
) -> MutantResult:
    root = scratch_root / mutant_id(mutant, ordinal)
    if root.exists():
        shutil.rmtree(root)
    scratch_game = root / "game"
    shutil.copytree(game_dir, scratch_game)
    try:
        target = scratch_game / mutant.source
        target.write_text(
            apply_mutant(target.read_text(encoding="utf-8"), mutant), encoding="utf-8"
        )
        command = prove_command(
            mutant,
            opt_level=opt_level,
            scratch_game=scratch_game,
            root=root,
            base_dir=base_dir,
            cases_per_function=cases_per_function,
            functions=functions,
            prove_args=prove_args,
        )
        try:
            completed = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=timeout_seconds,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return MutantResult(mutant, ERROR, f"timed out after {timeout_seconds} s")
        result = classify(
            completed.returncode,
            mutant,
            # T709: `--only-va` runs write under out/partial since T698.
            root / "out" / PARTIAL_DIR_NAME / "proof.json",
            root / "work",
            completed.stderr + completed.stdout,
        )
        if keep:
            (root / "run.log").write_text(completed.stderr + completed.stdout, encoding="utf-8")
        return result
    finally:
        if not keep:
            shutil.rmtree(root, ignore_errors=True)


def run_mutants(
    mutants: Sequence[Mutant],
    *,
    game_dir: Path = DEFAULT_GAME_DIR,
    base_dir: Path = DEFAULT_BASE_DIR,
    scratch_root: Path = DEFAULT_SCRATCH_ROOT,
    jobs: int = 4,
    opt_level: int = 2,
    keep: bool = False,
    cases_per_function: int | None = None,
    timeout_seconds: int = DEFAULT_MUTANT_TIMEOUT_SECONDS,
    functions: Path | None = None,
    progress: Callable[[MutantResult], None] | None = None,
    prove_args: Sequence[str] = (),
) -> list[MutantResult]:
    """Build and judge every mutant, `jobs` at a time, results in the order given.

    Each mutant gets its own scratch copy of `game_dir`, its own work dir and its own
    out dir. Only `base_dir` (the unmodified stub-object build) is shared, read-only.
    """
    if not mutants:
        return []
    # Prepare once in the coordinator. Workers receive a verified immutable path,
    # rather than racing to repair/rebuild a shared stale cache.
    try:
        base_dir = prepare_baseline(Path("generated/lifted/gen"), base_dir)
    except BuildError as error:
        return [
            MutantResult(mutant, ERROR, f"baseline preparation failed: {error}")
            for mutant in mutants
        ]
    scratch_root.mkdir(parents=True, exist_ok=True)
    lock = threading.Lock()

    def work(item: tuple[int, Mutant]) -> MutantResult:
        ordinal, mutant = item
        result = run_one(
            mutant,
            ordinal,
            game_dir=game_dir,
            base_dir=base_dir,
            scratch_root=scratch_root,
            opt_level=opt_level,
            keep=keep,
            cases_per_function=cases_per_function,
            timeout_seconds=timeout_seconds,
            functions=functions,
            prove_args=prove_args,
        )
        if progress is not None:
            with lock:
                progress(result)
        return result

    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        return list(pool.map(work, enumerate(mutants)))


# --------------------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------------------


def summarise(
    registrations: Sequence[Registration],
    results: Sequence[MutantResult],
    baselines: dict[int, str],
) -> dict[str, object]:
    functions = []
    totals = {"mutants": 0, KILLED: 0, SURVIVED: 0, NOT_A_MUTANT: 0, WIRING: 0, ERROR: 0}
    operators: dict[str, dict[str, int]] = {}
    for registration in registrations:
        mine = [r for r in results if r.mutant.va == registration.va]
        counts = {status: sum(1 for r in mine if r.status == status) for status in totals}
        counts["mutants"] = len(mine)
        for result in mine:
            row = operators.setdefault(
                result.mutant.operator, {"mutants": 0, KILLED: 0, SURVIVED: 0}
            )
            row["mutants"] += 1
            if result.status in (KILLED, SURVIVED):
                row[result.status] += 1
        for key, value in counts.items():
            totals[key] += value
        functions.append(
            {
                "va": f"0x{registration.va:08x}",
                "name": registration.function,
                "source": registration.source,
                "baseline": baselines.get(registration.va, "not-run"),
                **counts,
                "survivors": [
                    {
                        "line": r.mutant.line,
                        "operator": r.mutant.operator,
                        "original": r.mutant.original.strip(),
                        "mutated": r.mutant.mutated.strip(),
                    }
                    for r in mine
                    if r.status == SURVIVED
                ],
                "not_a_mutant_details": [
                    {"line": r.mutant.line, "operator": r.mutant.operator, "reason": r.detail}
                    for r in mine
                    if r.status in (NOT_A_MUTANT, WIRING, ERROR)
                ],
            }
        )
    return {"totals": totals, "operators": operators, "functions": functions}


def format_table(summary: dict[str, object]) -> str:
    rows = ["function                      va          gen  killed  survived  not-a-mutant  other"]
    for function in summary["functions"]:  # type: ignore[attr-defined]
        other = function[WIRING] + function[ERROR]
        rows.append(
            f"{function['name']:<29} {function['va']}  {function['mutants']:>3}  "
            f"{function[KILLED]:>6}  {function[SURVIVED]:>8}  {function[NOT_A_MUTANT]:>12}  "
            f"{other:>5}"
        )
    totals = summary["totals"]  # type: ignore[assignment]
    rows.append(
        f"{'TOTAL':<29} {'':<10}  {totals['mutants']:>3}  {totals[KILLED]:>6}  "
        f"{totals[SURVIVED]:>8}  {totals[NOT_A_MUTANT]:>12}  {totals[WIRING] + totals[ERROR]:>5}"
    )
    return "\n".join(rows)


def write_report(path: Path, document: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--game-dir", type=Path, default=DEFAULT_GAME_DIR, metavar="DIR")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, metavar="PATH")
    parser.add_argument("--jobs", type=int, default=4, metavar="N", help="parallel mutants")
    parser.add_argument(
        "--only-va",
        action="append",
        default=[],
        type=lambda text: int(text, 16),
        metavar="HEX",
        help="restrict to this registered address (repeatable)",
    )
    parser.add_argument("--max-mutants-per-function", type=int, default=None, metavar="N")
    parser.add_argument("--keep", action="store_true", help="keep each mutant's scratch dir")
    parser.add_argument("--opt-level", type=int, choices=(0, 1, 2, 3), default=2)
    parser.add_argument("--base-dir", type=Path, default=DEFAULT_BASE_DIR, metavar="DIR")
    parser.add_argument("--scratch-root", type=Path, default=DEFAULT_SCRATCH_ROOT, metavar="DIR")
    parser.add_argument("--cases-per-function", type=int, default=None, metavar="N")
    parser.add_argument(
        "--timeout", type=int, default=DEFAULT_MUTANT_TIMEOUT_SECONDS, metavar="SECONDS"
    )
    parser.add_argument(
        "--extended", action="store_true", help="add compound-assignment and ~ ! operators"
    )
    parser.add_argument(
        "--include-defines",
        action="store_true",
        help="also mutate the literals of #defines the function body uses",
    )
    parser.add_argument(
        "--no-baseline",
        action="store_true",
        help="skip the unmutated control run of each function (it is the only evidence the "
        "mutation setup itself works)",
    )
    parser.add_argument(
        "--functions",
        type=Path,
        default=None,
        metavar="CSV",
        help="functions.csv handed to every prove run (default: the prove default)",
    )
    parser.add_argument(
        "--synth-domain",
        action="store_true",
        help="T1773: judge every mutant (and its control) with the synthesized-domain provider",
    )
    parser.add_argument("--list", action="store_true", help="print the mutants and stop")
    return parser


def collect_mutants(
    game_dir: Path,
    registrations: Sequence[Registration],
    *,
    extended: bool,
    include_defines: bool,
    limit: int | None,
) -> list[Mutant]:
    mutants: list[Mutant] = []
    for registration in registrations:
        text = (game_dir / registration.source).read_text(encoding="utf-8")
        mine = generate_mutants(
            text,
            registration.source,
            [registration],
            extended=extended,
            include_defines=include_defines,
        )
        mutants += sample_mutants(mine, limit)
    return mutants


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        registrations = scan_directory(args.game_dir)
    except ScanError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    if args.only_va:
        registrations = [r for r in registrations if r.va in args.only_va]
    if not registrations:
        print("error: no registered function selected", file=sys.stderr)
        return 2
    mutants = collect_mutants(
        args.game_dir,
        registrations,
        extended=args.extended,
        include_defines=args.include_defines,
        limit=args.max_mutants_per_function,
    )
    if args.list:
        for mutant in mutants:
            print(f"{mutant.va:08X} {mutant.source}:{mutant.line} {mutant.operator}")
            print(f"    - {mutant.original.strip()}\n    + {mutant.mutated.strip()}")
        print(f"{len(mutants)} mutant(s)")
        return 0

    def report(result: MutantResult) -> None:
        mutant = result.mutant
        print(
            f"[{result.status:<12}] {mutant.va:08X} {mutant.source}:{mutant.line} "
            f"{mutant.operator}  {mutant.mutated.strip()[:70]}",
            flush=True,
        )
        if result.status == ERROR:
            print(f"    detail: {result.detail[:300]}", flush=True)

    options = {
        "game_dir": args.game_dir,
        "base_dir": args.base_dir,
        "scratch_root": args.scratch_root,
        "jobs": args.jobs,
        "opt_level": args.opt_level,
        "keep": args.keep,
        "cases_per_function": args.cases_per_function,
        "timeout_seconds": args.timeout,
        "functions": args.functions,
        "progress": report,
        "prove_args": ("--synth-domain",) if args.synth_domain else (),
    }
    baselines: dict[int, str] = {}
    runnable = list(mutants)
    if not args.no_baseline:
        controls = run_mutants([baseline_mutant(r) for r in registrations], **options)  # type: ignore[arg-type]
        baselines = {c.mutant.va: c.status for c in controls}
        failed = {va for va, status in baselines.items() if status != BASELINE_OK}
        runnable = [m for m in mutants if m.va not in failed]
        for control in controls:
            if control.status != BASELINE_OK:
                print(
                    f"baseline FAILED for {control.mutant.function}: {control.detail}",
                    file=sys.stderr,
                )
    results = run_mutants(runnable, **options)  # type: ignore[arg-type]
    summary = summarise(registrations, results, baselines)
    document = {
        "schema": MUTATION_SCHEMA,
        "kind": "replacement-mutation",
        "opt_level": args.opt_level,
        "extended": args.extended,
        "include_defines": args.include_defines,
        "max_mutants_per_function": args.max_mutants_per_function,
        **summary,
    }
    write_report(args.out, document)
    print(format_table(summary))
    print(f"report: {args.out}")
    problems = summary["totals"][ERROR] + summary["totals"][WIRING]  # type: ignore[index]
    baseline_bad = any(status != BASELINE_OK for status in baselines.values())
    return 3 if problems or baseline_bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
