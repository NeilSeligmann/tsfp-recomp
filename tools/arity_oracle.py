# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive kernel export arities from the nxdk `xboxkrnl.exe.def` module-definition file.

WHY THIS EXISTS, AND WHY IT IS NOT ANOTHER ESTIMATOR.

Getting an ordinal's arity wrong is this project's most expensive defect class.
`__stdcall` is callee-cleanup, so popping the wrong number of dwords eats the
caller's locals and `esp` never recovers: the run does not crash, it produces a
plausible wrong trace that cannot be falsified from the inside. The call-site
scanner in `tools/lift/callsites.py` has been measurably wrong three times
(ordinals 219, 340 and 190), always in the same direction and always for the same
reason -- it takes the MINIMUM across sites, and an argument push that falls
outside the lifter's `_icall_esp` bracket makes a site vote low.

An MSVC-decorated export name carries the argument BYTE COUNT in the decoration.
That is not a measurement of anything, so it shares no failure mode with the
scanner: a name cannot be fooled by a late bracket, by a callee-saved push inside
an argument window, or by a `jmp [slot]` import stub. Those are precisely the
three ways every other method here goes wrong.

SOURCE AND LICENCE.

  XboxDev/nxdk, `lib/xboxkrnl/xboxkrnl.exe.def`
  https://github.com/XboxDev/nxdk
  SPDX-License-Identifier: CC0-1.0
  SPDX-FileCopyrightText: 2017 Stefan Schmidt

CC0-1.0 is a public-domain dedication. `docs/provenance.md` verdicts
`XboxDev/nxdk lib/xboxkrnl/` as USE, having audited 873 commits for Microsoft DDK
fingerprints with a positive control. The file is vendored verbatim at
`tools/data/nxdk/xboxkrnl.exe.def` -- including its own SPDX header -- so the
derivation is reproducible from a fresh clone with no network and no scratch
directory. Vendored from upstream commit 14d5ee97e73347c973f1f57b68b79ec08c9e77f2;
the file itself last changed in 9e9c07f4b66b540b342e7ac000f02a4a82d7c6e5
(2022-03-20).

THIS IS A DIFFERENT KERNEL BUILD FROM OUR TARGET. TSFP is XDK 5849 and this `.def`
is nxdk's own export list. Ordinal drift across XDK versions is real -- see
`RESOLVED_ON_XDK_5849` in `tools/kernel_ordinals.py`. The oracle is therefore wired
in as a CROSS-CHECK and a last-resort fallback, never as an override of a
hand-verified row. `report` subcommand quantifies the drift risk by name.

DECORATION GRAMMAR, and the two traps in it.

    _Name@N   -> __stdcall, N ARGUMENT BYTES. The leading underscore is stripped
                 by the `.def` already, so a bare `Name@N` row is stdcall.
    @Name@N   -> __fastcall. First arg in ecx, second in edx, rest on the stack.
    Name      -> __cdecl (every such row here is variadic printf-family) or, with
                 the DATA keyword, not a function at all.

  TRAP ONE: N is BYTES, not arguments. Dividing by four is the whole job and
  forgetting to is a silent four-times error that still looks like a plausible
  arity for small counts.

  TRAP TWO: a DATA export has no arity and must never be given one. Handing a
  variable's ordinal a stack-argument count would make the thunk pop bytes nobody
  pushed.

  A THIRD, SUBTLER ONE: for __cdecl the CALLER cleans up, so the callee pops ZERO
  regardless of how many arguments were passed. The byte count and the callee pop
  are different quantities and this module keeps them apart on purpose.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path

#: Vendored oracle, relative to the repo root. Committed so a fresh clone builds.
DEFAULT_DEF_PATH = Path("tools/data/nxdk/xboxkrnl.exe.def")

#: Recorded so a silently swapped vendored file is a test failure rather than a
#: quiet change of meaning. Not a security measure, a provenance one.
DEF_SHA256 = "8aa3e48417c8a60aea8d1953bcc0674cdee95ce9a11e5e861d3107c3339d2527"

#: One EXPORTS row. The optional leading `@` is the fastcall marker and the
#: optional trailing `@N` is the argument byte count.
_ROW_RE = re.compile(
    r"^(?P<fastcall>@)?"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"(?:@(?P<arg_bytes>\d+))?"
    r"\s+@\s*(?P<ordinal>\d+)"
    r"\s+NONAME"
    r"(?P<data>\s+DATA)?$"
)

_SKIP_DIRECTIVES = ("LIBRARY", "EXPORTS", "VERSION", "DESCRIPTION")


class Convention(StrEnum):
    """How an export takes its arguments and who cleans up."""

    STDCALL = "stdcall"
    FASTCALL = "fastcall"
    #: Variadic printf-family. Caller cleans up, so the callee pops nothing.
    CDECL = "cdecl"
    #: Not a function. Has no arity at all.
    DATA = "data"


class OracleError(Exception):
    """A `.def` row this module refuses to guess about."""


@dataclass(frozen=True)
class OracleRow:
    """One derived export. `arg_bytes` is None exactly when the decoration omits it."""

    ordinal: int
    name: str
    convention: Convention
    arg_bytes: int | None

    @property
    def is_function(self) -> bool:
        return self.convention is not Convention.DATA

    @property
    def arg_dwords(self) -> int | None:
        """Total arguments, registers included. None when the decoration cannot say."""
        if self.arg_bytes is None:
            return None
        return self.arg_bytes // 4

    @property
    def register_args(self) -> int:
        """Arguments travelling in ecx/edx. Only fastcall has any."""
        if self.convention is not Convention.FASTCALL:
            return 0
        total = self.arg_dwords
        if total is None:
            raise OracleError(f"fastcall row {self.name} has no byte count")
        return min(2, total)

    @property
    def callee_pop_dwords(self) -> int | None:
        """Dwords the callee pops on return, which is what `esp` cleanup needs.

        None means the oracle cannot say -- a DATA export, or a stdcall/fastcall row
        with no decoration. Zero for cdecl is a POSITIVE answer, not an absence of
        one: the caller cleans up, so the callee really does pop nothing.
        """
        if self.convention is Convention.DATA:
            return None
        if self.convention is Convention.CDECL:
            return 0
        total = self.arg_dwords
        if total is None:
            return None
        return total - self.register_args


def parse_def(text: str) -> list[OracleRow]:
    """Parse a module-definition file into derived rows, ordered by ordinal.

    Raises on anything unrecognised. A `.def` row this module cannot classify is a
    reason to stop, not to skip: a skipped row would silently become a missing
    oracle entry, which reads as "the oracle does not cover that ordinal".
    """
    rows: list[OracleRow] = []
    seen: dict[int, str] = {}
    in_exports = False

    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.split()[0].upper() in _SKIP_DIRECTIVES:
            in_exports = line.split()[0].upper() == "EXPORTS"
            continue
        if not in_exports:
            raise OracleError(f"line {lineno}: row before EXPORTS: {line!r}")

        match = _ROW_RE.match(line)
        if match is None:
            raise OracleError(f"line {lineno}: unrecognised EXPORTS row: {line!r}")

        ordinal = int(match["ordinal"])
        name = match["name"]
        if ordinal in seen:
            raise OracleError(f"line {lineno}: ordinal {ordinal} already used by {seen[ordinal]}")
        seen[ordinal] = name

        arg_bytes = None if match["arg_bytes"] is None else int(match["arg_bytes"])

        if match["data"]:
            if arg_bytes is not None:
                raise OracleError(f"line {lineno}: DATA export {name} carries a byte count")
            convention = Convention.DATA
        elif arg_bytes is None:
            # No decoration at all. MSVC leaves __cdecl undecorated in a .def.
            convention = Convention.CDECL
            if match["fastcall"]:
                raise OracleError(f"line {lineno}: fastcall marker with no byte count")
        elif match["fastcall"]:
            convention = Convention.FASTCALL
        else:
            convention = Convention.STDCALL

        if arg_bytes is not None and arg_bytes % 4 != 0:
            # x86 stack slots are 4 bytes. A non-multiple means the decoration was
            # misread, and misreading it is the four-times error this file warns about.
            raise OracleError(
                f"line {lineno}: {name} argument byte count {arg_bytes} is not a multiple of 4"
            )

        rows.append(OracleRow(ordinal, name, convention, arg_bytes))

    if not rows:
        raise OracleError("no EXPORTS rows found")
    return sorted(rows, key=lambda row: row.ordinal)


def load_rows(def_path: Path) -> list[OracleRow]:
    return parse_def(def_path.read_text(encoding="utf-8"))


# --- emitters -----------------------------------------------------------------

_C_CONVENTION = {
    Convention.STDCALL: "KERNEL_ARITY_ORACLE_STDCALL",
    Convention.FASTCALL: "KERNEL_ARITY_ORACLE_FASTCALL",
    Convention.CDECL: "KERNEL_ARITY_ORACLE_CDECL",
    Convention.DATA: "KERNEL_ARITY_ORACLE_DATA",
}

_C_HEADER = """/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DERIVED FROM A CC0-1.0 WORK. Do not hand-edit -- regenerate instead:
 *
 *     ./.venv/bin/python -m tools.arity_oracle emit-c \\
 *         --output src/xbox/kernel_arity_oracle.c
 *
 * Source: XboxDev/nxdk, lib/xboxkrnl/xboxkrnl.exe.def
 *         https://github.com/XboxDev/nxdk
 *         SPDX-License-Identifier: CC0-1.0
 *         SPDX-FileCopyrightText: 2017 Stefan Schmidt
 *
 * The input is vendored verbatim at tools/data/nxdk/xboxkrnl.exe.def, so this table
 * is reproducible from a fresh clone. CC0-1.0 is a public-domain dedication;
 * docs/provenance.md verdicts XboxDev/nxdk lib/xboxkrnl/ as USE after auditing 873
 * commits for Microsoft DDK fingerprints with a positive control. The derivation is
 * recorded here rather than left implicit because that is what the licence terms of
 * the inputs to this project require of us.
 *
 * WHAT THE NUMBERS ARE. An MSVC-decorated export name carries the argument BYTE
 * count: `Name@N` is __stdcall, `@Name@N` is __fastcall, a bare name is __cdecl or
 * a DATA export. Dividing by four gives dwords. This is a NAME DECORATION, not a
 * measurement, so it cannot be fooled by a late `_icall_esp` bracket, by a
 * callee-saved push inside an argument window, or by a `jmp [slot]` import stub --
 * the three ways the call-site scanner has actually gone wrong on this image.
 *
 * WHAT IT IS NOT. This is nxdk's export list, not XDK 5849's. Ordinal drift across
 * XDK builds is real. Nothing here overrides a hand-verified ABI_TABLE row in
 * src/host/kernel_thunk.c; see kernel_arity_oracle.h for the ordering rule.
 */

#include "kernel_arity_oracle.h"

/*
 * Rows are ordinal-ascending and the lookup below relies on that for a binary
 * search. Ordinals are NOT dense -- the gaps are real holes in the export table.
 */
static const kernel_arity_oracle_entry ORACLE_TABLE[] = {
"""

_C_TAIL = """};

#define ORACLE_TABLE_COUNT (sizeof(ORACLE_TABLE) / sizeof(ORACLE_TABLE[0]))

unsigned kernel_arity_oracle_count(void)
{
    return (unsigned)ORACLE_TABLE_COUNT;
}

const kernel_arity_oracle_entry *kernel_arity_oracle_at(unsigned index)
{
    if (index >= (unsigned)ORACLE_TABLE_COUNT) {
        return NULL;
    }
    return &ORACLE_TABLE[index];
}

const kernel_arity_oracle_entry *kernel_arity_oracle_lookup(unsigned ordinal)
{
    size_t low = 0;
    size_t high = ORACLE_TABLE_COUNT;
    while (low < high) {
        size_t mid = low + (high - low) / 2u;
        unsigned probe = ORACLE_TABLE[mid].ordinal;
        if (probe == ordinal) {
            return &ORACLE_TABLE[mid];
        }
        if (probe < ordinal) {
            low = mid + 1u;
        } else {
            high = mid;
        }
    }
    return NULL;
}

bool kernel_arity_oracle_callee_pop(unsigned ordinal, unsigned *out_dwords)
{
    const kernel_arity_oracle_entry *entry = kernel_arity_oracle_lookup(ordinal);
    if (entry == NULL) {
        return false;
    }
    /* A DATA export is not a function and has no arity. Handing one a pop count is
     * the mutation this file's test suite exists to catch: the thunk would pop bytes
     * nobody pushed and desync esp for every later call. */
    if (entry->convention == KERNEL_ARITY_ORACLE_DATA) {
        return false;
    }
    /* __cdecl is a POSITIVE zero, not a refusal: the caller cleans up, so the callee
     * pops nothing however many arguments were passed. Every cdecl row in this table
     * is a variadic printf-family export, whose true arity varies per call site and
     * is unknowable from a decoration -- which is exactly why callee cleanup cannot
     * apply to it. */
    *out_dwords = entry->stack_args;
    return true;
}
"""


def emit_c(rows: list[OracleRow]) -> str:
    """Render the committed C table."""
    body = []
    for row in rows:
        pop = row.callee_pop_dwords
        if pop is None:
            pop = 0
        body.append(
            f"    {{{row.ordinal}u, {_C_CONVENTION[row.convention]}, "
            f"{pop}u, {row.register_args}u}}, /* {row.name} */"
        )
    return _C_HEADER + "\n".join(body) + "\n" + _C_TAIL


_H_TEXT = """/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ordinal -> argument arity, derived from a CC0-1.0 module-definition file.
 * See kernel_arity_oracle.c for the source, its licence, and the decoration
 * grammar the numbers come from.
 *
 * HOW THIS IS MEANT TO BE USED, which is the whole design decision.
 *
 * It is a CROSS-CHECK and a LAST-RESORT FALLBACK, never an override. The ordering
 * in `stack_args_for()` is:
 *
 *     1. ABI_TABLE          hand-verified at this image's own call sites
 *     2. MEASURED_ARITIES   this image's call sites, behind quorum + unanimity gates
 *     3. this oracle        a name decoration from a DIFFERENT kernel build
 *     4. refuse, and stop the run
 *
 * The oracle is third DESPITE being structurally stronger evidence than a gated
 * measurement, because it is one file describing a kernel build that is not ours.
 * A hand row carries reasoning -- a forced stack balance, a literal that pins an
 * argument order -- that a decoration does not have, and if the two disagree the
 * hand row may well be the right one. So a disagreement is a FINDING, raised loudly
 * at test time by tests/test_arity_oracle.py, and never resolved silently in favour
 * of either side at runtime.
 *
 * It is third rather than absent because of what step 4 costs: an ordinal with no
 * usable count stops the run. The oracle turns some of those stops into correct
 * continuations, and a continuation whose provenance is NAMED in the diagnostic is
 * strictly better than a stop.
 */

#ifndef TSFP_KERNEL_ARITY_ORACLE_H
#define TSFP_KERNEL_ARITY_ORACLE_H

#include <stdbool.h>
#include <stddef.h>

/* How the export takes its arguments. Mirrors the `.def` decoration grammar. */
typedef enum {
    /* `Name@N`. All arguments on the stack, callee pops them. */
    KERNEL_ARITY_ORACLE_STDCALL = 0,
    /* `@Name@N`. First argument in ecx, second in edx, remainder on the stack.
     * The callee pops only the stack remainder. */
    KERNEL_ARITY_ORACLE_FASTCALL = 1,
    /* Undecorated. Variadic printf-family: the CALLER cleans up, so the callee
     * pops zero and the true argument count is not knowable from a decoration. */
    KERNEL_ARITY_ORACLE_CDECL = 2,
    /* `NONAME DATA`. A variable, not a function. HAS NO ARITY. */
    KERNEL_ARITY_ORACLE_DATA = 3,
} kernel_arity_oracle_cc;

typedef struct {
    unsigned ordinal;
    kernel_arity_oracle_cc convention;
    /* Dwords the CALLEE pops on return. Zero for cdecl because the caller cleans
     * up, and zero for DATA only because the field must hold something -- use the
     * convention to tell those two apart, never this field alone. */
    unsigned stack_args;
    /* Arguments in ecx/edx: 0, 1 or 2. Non-zero only for fastcall. */
    unsigned register_args;
} kernel_arity_oracle_entry;

/* NULL when the oracle has no row for this ordinal. */
const kernel_arity_oracle_entry *kernel_arity_oracle_lookup(unsigned ordinal);

/* Dwords a callee-cleanup thunk must pop. False when the oracle cannot say, which
 * is every DATA export and every ordinal it does not cover. */
bool kernel_arity_oracle_callee_pop(unsigned ordinal, unsigned *out_dwords);

/* Whole-table access, for the suites that sweep every row. */
unsigned kernel_arity_oracle_count(void);
const kernel_arity_oracle_entry *kernel_arity_oracle_at(unsigned index);

#endif /* TSFP_KERNEL_ARITY_ORACLE_H */
"""


def emit_h() -> str:
    return _H_TEXT


def emit_json(rows: list[OracleRow]) -> str:
    payload = [
        {
            "ordinal": row.ordinal,
            "name": row.name,
            "convention": row.convention.value,
            "arg_bytes": row.arg_bytes,
            "arg_dwords": row.arg_dwords,
            "register_args": row.register_args,
            "callee_pop_dwords": row.callee_pop_dwords,
        }
        for row in rows
    ]
    return json.dumps(payload, indent=2) + "\n"


# --- cross-checks -------------------------------------------------------------

# CDECL is matched too (T34): a convention the pattern omits would make its ABI_TABLE
# rows invisible to the cross-check -- silently, since a skipped row fails nothing.
# tests/test_arity_oracle.py pins that the parse actually yields a CDECL row.
_ABI_ROW_RE = re.compile(
    r"\{\s*(?:(?P<ordinal>\d+)u|ORD_(?P<symbol>[A-Za-z_][A-Za-z0-9_]*))\s*,"
    r"\s*THUNK_CC_(?P<cc>STDCALL|FASTCALL|CDECL)\s*,"
    r"\s*(?P<stack>\d+)u\s*,\s*(?P<regs>\d+)u\s*\}"
)


@dataclass(frozen=True)
class HandRow:
    ordinal: int
    convention: Convention
    stack_args: int
    register_args: int


def parse_abi_table(text: str, ordinal_by_name: dict[str, int]) -> list[HandRow]:
    """Pull `ABI_TABLE` rows out of `src/host/kernel_thunk.c`.

    Parsing C from Python is ugly, and it is still the right tool: `ABI_TABLE` is
    `static`, so no C test can see it, and the alternative -- asking the owner of
    that file to export it -- buys a weaker check for a larger diff.
    """
    start = text.index("static const thunk_abi ABI_TABLE[] = {")
    end = text.index("\n};", start)
    rows: list[HandRow] = []
    for match in _ABI_ROW_RE.finditer(text[start:end]):
        if match["ordinal"] is not None:
            ordinal = int(match["ordinal"])
        else:
            symbol = match["symbol"]
            if symbol not in ordinal_by_name:
                raise OracleError(f"ABI_TABLE names unknown ordinal symbol {symbol}")
            ordinal = ordinal_by_name[symbol]
        rows.append(
            HandRow(
                ordinal=ordinal,
                convention=Convention[match["cc"]],
                stack_args=int(match["stack"]),
                register_args=int(match["regs"]),
            )
        )
    if not rows:
        raise OracleError("ABI_TABLE parsed to zero rows")
    return rows


@dataclass(frozen=True)
class Disagreement:
    ordinal: int
    name: str
    field: str
    hand: object
    oracle: object


def crosscheck_abi_table(
    hand_rows: list[HandRow], rows: list[OracleRow]
) -> tuple[list[Disagreement], list[int]]:
    """Compare hand rows against the oracle. Returns (disagreements, uncovered)."""
    by_ordinal = {row.ordinal: row for row in rows}
    disagreements: list[Disagreement] = []
    uncovered: list[int] = []
    for hand in hand_rows:
        oracle = by_ordinal.get(hand.ordinal)
        if oracle is None:
            uncovered.append(hand.ordinal)
            continue
        if oracle.convention is not hand.convention:
            disagreements.append(
                Disagreement(
                    hand.ordinal,
                    oracle.name,
                    "convention",
                    hand.convention.value,
                    oracle.convention.value,
                )
            )
            continue
        pop = oracle.callee_pop_dwords
        if pop != hand.stack_args:
            disagreements.append(
                Disagreement(hand.ordinal, oracle.name, "stack_args", hand.stack_args, pop)
            )
        if oracle.register_args != hand.register_args:
            disagreements.append(
                Disagreement(
                    hand.ordinal,
                    oracle.name,
                    "register_args",
                    hand.register_args,
                    oracle.register_args,
                )
            )
    return disagreements, uncovered


_MEASURED_ROW_RE = re.compile(r"\{\s*(\d+)u\s*,\s*(\d+)u\s*,\s*(\d+)u\s*,\s*(\d+)\s*\}")


@dataclass(frozen=True)
class MeasuredRow:
    ordinal: int
    stack_args: int
    sites: int
    unanimous: bool

    def accepted(self, min_sites: int) -> bool:
        """Mirror of the gates in `stack_args_for()`."""
        return self.unanimous and self.sites >= min_sites


def parse_measured(text: str) -> tuple[list[MeasuredRow], list[int]]:
    """Pull rows and DATA ordinals out of a generated `kernel_arity.inc`."""
    rows: list[MeasuredRow] = []
    marker = "MEASURED_ARITIES[]"
    if marker in text:
        start = text.index(marker)
        # Not "\n};" -- the DATA array below is emitted on a single line, so the
        # brace close has no newline before it and anchoring on one silently fails.
        end = text.index("};", start)
        for match in _MEASURED_ROW_RE.finditer(text[start:end]):
            rows.append(MeasuredRow(int(match[1]), int(match[2]), int(match[3]), match[4] != "0"))
    data: list[int] = []
    data_marker = "MEASURED_DATA_ORDINALS[]"
    if data_marker in text:
        start = text.index(data_marker)
        end = text.index("};", start)
        data = [int(value) for value in re.findall(r"(\d+)u", text[start:end])]
    return rows, data


def _name_drift(rows: list[OracleRow], ordinal_names: dict[int, str]) -> list[str]:
    """Ordinals where the oracle and `tools/kernel_ordinals.py` name different things.

    This is the DRIFT CHECK. Agreement on a name is not proof the ordinal means the
    same thing on XDK 5849, because both lists could share an ancestor -- but a
    DISAGREEMENT is positive evidence of drift and must be surfaced.
    """
    drift = []
    for row in rows:
        known = ordinal_names.get(row.ordinal)
        if known is not None and known != row.name:
            drift.append(f"{row.ordinal}: ours={known} oracle={row.name}")
    return drift


# --- CLI ----------------------------------------------------------------------


def _load_imported(path: Path | None) -> set[int] | None:
    if path is None:
        return None
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".json":
        payload = json.loads(text)
        if isinstance(payload, dict):
            payload = payload.get("ordinals", sorted(int(key) for key in payload))
        return {int(value) for value in payload}
    return {int(line) for line in text.split() if line.strip().isdigit()}


def _report(args: argparse.Namespace, rows: list[OracleRow]) -> int:
    from tools.kernel_ordinals import KERNEL_ORDINALS

    out = sys.stdout
    functions = [row for row in rows if row.is_function]
    data_rows = [row for row in rows if not row.is_function]
    out.write(f"oracle rows: {len(rows)}\n")
    out.write(f"  functions: {len(functions)}\n")
    by_cc: dict[str, int] = {}
    for row in rows:
        by_cc[row.convention.value] = by_cc.get(row.convention.value, 0) + 1
    for name in sorted(by_cc):
        out.write(f"    {name}: {by_cc[name]}\n")
    out.write(f"  data exports: {len(data_rows)}\n")

    drift = _name_drift(rows, KERNEL_ORDINALS)
    out.write(f"\nname drift vs tools/kernel_ordinals.py: {len(drift)}\n")
    for line in drift:
        out.write(f"  DRIFT {line}\n")
    ours = set(KERNEL_ORDINALS)
    theirs = {row.ordinal for row in rows}
    out.write(f"  ordinals only in ours: {sorted(ours - theirs)}\n")
    out.write(f"  ordinals only in oracle: {sorted(theirs - ours)}\n")

    hand_rows: list[HandRow] = []
    if args.thunk is not None:
        symbol_ordinals = {name: ordinal for ordinal, name in KERNEL_ORDINALS.items()}
        hand_rows = parse_abi_table(args.thunk.read_text(encoding="utf-8"), symbol_ordinals)
        bad, uncovered = crosscheck_abi_table(hand_rows, rows)
        out.write(f"\nABI_TABLE rows: {len(hand_rows)}\n")
        out.write(f"  agree with oracle: {len(hand_rows) - len(bad) - len(uncovered)}\n")
        out.write(f"  DISAGREE: {len(bad)}\n")
        for item in bad:
            out.write(
                f"    FINDING ordinal {item.ordinal} ({item.name}) {item.field}: "
                f"hand={item.hand} oracle={item.oracle}\n"
            )
        out.write(f"  not covered by oracle: {sorted(uncovered)}\n")

    imported = _load_imported(args.imported)
    if imported is None:
        return 1 if (args.strict and drift) else 0

    out.write(f"\nimported ordinals: {len(imported)}\n")
    covered = {row.ordinal for row in rows} & imported
    out.write(f"  covered by oracle: {len(covered)}\n")
    out.write(f"  NOT covered: {sorted(imported - covered)}\n")

    if args.measured is None:
        return 1 if (args.strict and drift) else 0

    measured, measured_data = parse_measured(args.measured.read_text(encoding="utf-8"))
    by_ordinal = {row.ordinal: row for row in rows}
    hand_ordinals = {row.ordinal for row in hand_rows}
    accepted_measured = {
        row.ordinal
        for row in measured
        if row.accepted(args.min_sites) and row.ordinal not in hand_ordinals
    }
    answered = hand_ordinals | accepted_measured

    refused = sorted(imported - answered)
    unblocked = []
    for ordinal in refused:
        oracle = by_ordinal.get(ordinal)
        if oracle is None:
            continue
        if oracle.callee_pop_dwords is not None:
            unblocked.append(ordinal)

    out.write(f"\nstack_args_for() answers today: {len(answered & imported)}\n")
    out.write(f"  refused or missing: {len(refused)}\n")
    out.write(f"  of those, oracle can answer: {len(unblocked)}\n")
    out.write(f"  still refused after the oracle: {len(refused) - len(unblocked)}\n")

    conflicts = []
    for row in measured:
        oracle = by_ordinal.get(row.ordinal)
        if oracle is None or oracle.callee_pop_dwords is None:
            continue
        if row.stack_args != oracle.callee_pop_dwords:
            conflicts.append(
                f"{row.ordinal} ({oracle.name}): measured={row.stack_args} "
                f"({row.sites} sites, unanimous={row.unanimous}) "
                f"oracle={oracle.callee_pop_dwords}"
            )
    out.write(f"\nmeasured-vs-oracle disagreements: {len(conflicts)}\n")
    for line in conflicts:
        out.write(f"  {line}\n")

    # THE SAFETY CLAIM, and the reason the oracle can go in as a third source without
    # changing a single answer the host gives today. A disagreement only matters if the
    # measured row was going to be USED: one that the quorum or unanimity gate already
    # refuses cannot be made worse. If this number is ever non-zero, adding the oracle
    # as a fallback stops being purely additive and the conflict has to be settled by
    # hand before anything is wired up.
    live = []
    for row in measured:
        if row.ordinal in hand_ordinals or not row.accepted(args.min_sites):
            continue
        oracle = by_ordinal.get(row.ordinal)
        if oracle is None or oracle.callee_pop_dwords is None:
            continue
        if row.stack_args != oracle.callee_pop_dwords:
            live.append(
                f"{row.ordinal} ({oracle.name}): measured={row.stack_args} "
                f"({row.sites} sites) oracle={oracle.callee_pop_dwords}"
            )
    out.write(f"  of those, ACCEPTED today (would change an answer): {len(live)}\n")
    for line in live:
        out.write(f"    FINDING {line}\n")

    data_conflicts = sorted(
        ordinal
        for ordinal in measured_data
        if (entry := by_ordinal.get(ordinal)) is not None and entry.is_function
    )
    out.write(f"\nscanner called these DATA but the oracle says function: {data_conflicts}\n")
    return 1 if (args.strict and drift) else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="tools.arity_oracle",
        description=(
            "Derive kernel export arities from the vendored CC0 nxdk "
            "xboxkrnl.exe.def and cross-check them against the hand and "
            "measured arity tables."
        ),
    )
    parser.add_argument(
        "--def-file",
        type=Path,
        default=DEFAULT_DEF_PATH,
        help="module-definition file to derive from (default: %(default)s)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    emit = sub.add_parser("emit-c", help="write the committed C table")
    emit.add_argument("--output", type=Path, help="write here instead of stdout")
    emit.add_argument("--header", type=Path, help="also write the companion .h to this path")

    sub.add_parser("emit-json", help="write the derived rows as JSON")

    report = sub.add_parser("report", help="coverage and cross-check report")
    report.add_argument(
        "--thunk",
        type=Path,
        default=Path("src/host/kernel_thunk.c"),
        help="file holding ABI_TABLE (default: %(default)s)",
    )
    report.add_argument(
        "--measured", type=Path, help="generated kernel_arity.inc to compare against"
    )
    report.add_argument(
        "--imported",
        type=Path,
        help="newline- or JSON-listed ordinals this title imports",
    )
    report.add_argument(
        "--min-sites",
        type=int,
        default=3,
        help="MEASURED_ARITY_MIN_SITES quorum to mirror (default: %(default)s)",
    )
    report.add_argument(
        "--strict", action="store_true", help="exit non-zero if any name drift is found"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    rows = load_rows(args.def_file)

    if args.command == "emit-c":
        text = emit_c(rows)
        if args.output is None:
            sys.stdout.write(text)
        else:
            args.output.write_text(text, encoding="utf-8")
        if args.header is not None:
            args.header.write_text(emit_h(), encoding="utf-8")
        return 0
    if args.command == "emit-json":
        sys.stdout.write(emit_json(rows))
        return 0
    return _report(args, rows)


if __name__ == "__main__":
    raise SystemExit(main())
