# SPDX-License-Identifier: GPL-3.0-or-later
"""Cross-check implemented kernel ordinals against the confidence of their arity.

WHY THIS EXISTS. `tools/lift/callsites.py` measures each ordinal's stack-argument
count from the guest's own argument pushes, and marks a row low-confidence when the
call sites disagree. That flag was being emitted and nothing was reading it.

The cost is concrete. Our HLE handler *is* the `__stdcall` callee, so it is
responsible for popping the arguments: a wrong count does not fail loudly, it
**desyncs the guest's `esp` permanently** and the damage appears arbitrarily far
away. Three ordinals were independently found to be under-reported
(`RtlInitAnsiString` as 1 where every site pushes 2, `NtReadFile` and `NtWriteFile`
as 6 where 8 was measured), and all three carry confidence 0 — the table knew.

Worse, **four ordinals we had already implemented carry confidence 0** (129, 165,
166, 173). They happen to be right, which is luck rather than process.

So this module makes the flag impossible to ignore: it lists every implemented
ordinal whose measured arity is low-confidence and is not explicitly acknowledged.
Acknowledging one is a deliberate act that records *why* the arity is trusted
anyway, which is exactly the review step that was missing.

The minimum-across-sites rule in `callsites.py` is not wrong to be conservative --
over-counting is its only dangerous error, because a callee-saved `push esi` falls
inside the bracket -- but a late `_icall_esp` bracket can yield a spuriously LOW
count, which is how under-reporting happens.

THE CONFIDENCE FLAG IS NOT THE WHOLE STORY (T58). It only says whether the sites of
one measurement agreed with each other. Ordinal 196 `NtDeviceIoControlFile` measures
12 from ONE site, which makes it trivially "unanimous" and so "confident", yet the
export takes 10 (the CC0 oracle says so, and so does a forced stack balance at the
site). A check that reads only the flag passed it. `oracle_disagreements()` therefore
compares every implemented ordinal's hand row and measured row against the oracle
(`tools/arity_oracle.py`) and REPORTS each difference, whether or not the host would
ever use the wrong number. A difference is never resolved here in favour of either
side: it is printed with who disagrees, which number the host would really use, and
whether a reviewed `ARITY-OK` marker covers it.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from tools.arity_oracle import DEFAULT_DEF_PATH, load_rows

#: Repo root, so the CLI defaults need no hardcoded absolute path.
REPO_ROOT = Path(__file__).resolve().parents[2]

#: Fewest call sites a measured row needs before the HOST believes it. Mirrors
#: `MEASURED_ARITY_MIN_SITES` in `src/host/kernel_thunk.c`; a test pins the two.
MEASURED_ARITY_MIN_SITES = 3

#: The hand table block in `src/host/kernel_thunk.c`: `{<ord>u | ORD_<Name>,
#: THUNK_CC_<cc>, <stack args>u, <register args>u}`.
HAND_ROW = re.compile(
    r"\{\s*(ORD_[A-Za-z0-9_]+|\d+u)\s*,\s*THUNK_CC_[A-Z]+\s*,\s*(\d+)u\s*,\s*\d+u\s*\}"
)

#: One row of the generated arity table: {ordinal, args, sites, confidence}.
ARITY_ROW = re.compile(r"\{\s*(\d+)u\s*,\s*(\d+)u\s*,\s*(\d+)u\s*,\s*(\d+)\s*\}")

#: A binding row in `src/xbox/*.c`, which is the single place an implementation is
#: wired to an ordinal. Matches the `ORD_<ExportName>` convention.
ORDINAL_BINDING = re.compile(r"\{\s*ORD_([A-Za-z0-9_]+)\s*,")

#: `#define ORD_<Name> <number>u`, so a binding name resolves to its ordinal.
ORDINAL_DEFINE = re.compile(r"#define\s+ORD_([A-Za-z0-9_]+)\s+(\d+)u")

#: An explicit, deliberate acknowledgement that a low-confidence arity was reviewed.
#: Written next to the handler as `/* ARITY-OK(<ordinal>): <reason> */`. The reason is
#: required -- an acknowledgement with no argument is just a way to silence the check.
#: The reason must begin with an alphanumeric, so a bare `ARITY-OK(165): */` cannot
#: pass by capturing the comment terminator as its own justification.
ARITY_OK = re.compile(r"ARITY-OK\((\d+)\)\s*:\s*([A-Za-z0-9][^*]*)")


@dataclass(frozen=True)
class ArityRow:
    ordinal: int
    args: int
    sites: int
    confident: bool


@dataclass(frozen=True)
class Unverified:
    """An implemented ordinal whose arity is low-confidence and unacknowledged."""

    ordinal: int
    name: str
    args: int
    sites: int


def parse_arity_table(path: Path) -> dict[int, ArityRow]:
    """Read the generated `kernel_arity.inc`."""
    rows: dict[int, ArityRow] = {}
    for ordinal, args, sites, confidence in ARITY_ROW.findall(path.read_text(encoding="utf-8")):
        value = int(ordinal)
        rows[value] = ArityRow(value, int(args), int(sites), confidence != "0")
    return rows


def ordinal_defines(src_xbox: Path) -> dict[str, int]:
    """`ORD_<Name>` -> ordinal, collected across headers AND sources."""
    defines: dict[str, int] = {}
    for path in sorted(src_xbox.glob("*.[ch]")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for name, value in ORDINAL_DEFINE.findall(text):
            defines[name] = int(value)
    return defines


def implemented_ordinals(src_xbox: Path) -> dict[int, str]:
    """ordinal -> export name, for every ordinal wired to a handler.

    Defines are collected across BOTH headers and sources before resolving any
    binding, because the convention is not uniform: `kernel_memory` puts its
    `ORD_*` defines in the header while `kernel_sync`, `kernel_object` and
    `kernel_thread` put theirs in the source. A first version scanned only sources
    and silently found 10 of 20 implemented ordinals -- the same silent-undercount
    failure the badge generator had, in a check whose entire job is to stop
    something being missed.
    """
    defines = ordinal_defines(src_xbox)

    found: dict[int, str] = {}
    unresolved: list[str] = []
    for path in sorted(src_xbox.glob("*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for name in ORDINAL_BINDING.findall(text):
            if name in defines:
                found[defines[name]] = name
            else:
                unresolved.append(name)
    if unresolved:
        # Loud, because a binding whose ordinal cannot be resolved is exactly the
        # case this check must not skip over.
        raise ValueError(
            "ORD_ bindings with no matching #define: "
            + ", ".join("ORD_" + n for n in sorted(set(unresolved)))
        )
    return found


def acknowledged(src_xbox: Path) -> dict[int, str]:
    """ordinal -> stated reason, for each explicitly reviewed low-confidence arity."""
    out: dict[int, str] = {}
    for path in sorted(src_xbox.glob("*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for ordinal, reason in ARITY_OK.findall(text):
            out[int(ordinal)] = reason.strip()
    return out


def unverified(
    arities: dict[int, ArityRow], implemented: dict[int, str], acknowledgements: dict[int, str]
) -> list[Unverified]:
    """Implemented ordinals resting on a low-confidence arity, unacknowledged.

    Sorted by call-site count descending, because a wrong arity at 113 sites is worse
    than one at 2. An ordinal with no measured row at all is NOT reported here: that
    is a different problem (nothing measured it) and reporting it as low confidence
    would conflate "uncertain" with "unknown".
    """
    out = [
        Unverified(ordinal, name, arities[ordinal].args, arities[ordinal].sites)
        for ordinal, name in implemented.items()
        if ordinal in arities and not arities[ordinal].confident and ordinal not in acknowledgements
    ]
    out.sort(key=lambda u: (-u.sites, u.ordinal))
    return out


def render(items: list[Unverified]) -> str:
    """A report a human can act on, or a clear all-clear."""
    if not items:
        return "arity check: every implemented ordinal rests on a confident measurement"
    lines = [
        f"arity check: {len(items)} implemented ordinal(s) rest on a LOW-CONFIDENCE arity",
        "  Our handler is the __stdcall callee, so a wrong count desyncs the guest's",
        "  esp permanently and the damage shows up far from here.",
        "  Verify against the call sites, then record it as",
        "  /* ARITY-OK(<ordinal>): <why it is trusted> */ beside the handler.",
        "",
        f"  {'ord':>5} {'args':>5} {'sites':>6}  name",
    ]
    lines.extend(f"  {i.ordinal:>5} {i.args:>5} {i.sites:>6}  {i.name}" for i in items)
    return "\n".join(lines)


# --- the oracle cross-check ----------------------------------------------------


@dataclass(frozen=True)
class OracleDisagreement:
    """One implemented ordinal whose hand or measured arity differs from the oracle."""

    ordinal: int
    name: str
    #: "hand" (an ABI_TABLE row) or "measured" (a `kernel_arity.inc` row).
    source: str
    value: int
    oracle: int
    #: Call sites behind a measured row, 0 for a hand row.
    sites: int
    #: Whether the measured row claimed confidence (unanimous). Always True for hand.
    confident: bool
    #: True when the HOST would pop `value` dwords for this ordinal. A hand row always
    #: is. A measured row is only when no hand row shadows it and it clears the
    #: unanimity and quorum gates, so a False here means the wrong number is being
    #: kept out by something else rather than by this row being right.
    live: bool
    #: The hand value that shadows a measured row, when there is one.
    shadowed_by: int | None
    #: The reviewed `ARITY-OK` reason covering this ordinal, if any.
    acknowledged: str | None


def parse_hand_table(thunk_source: Path, defines: dict[str, int]) -> dict[int, int]:
    """Ordinal -> callee-popped dwords, from the ABI_TABLE block of kernel_thunk.c.

    Raises when the block is missing or an `ORD_` name has no define: an empty or
    partial hand table would make every shadowed measured row look live.
    """
    text = thunk_source.read_text(encoding="utf-8")
    start = text.find("static const thunk_abi ABI_TABLE[]")
    end = text.find("#define ABI_TABLE_COUNT")
    if start < 0 or end < start:
        raise ValueError(f"no ABI_TABLE block found in {thunk_source}")
    rows: dict[int, int] = {}
    for token, stack_args in HAND_ROW.findall(text[start:end]):
        if token.startswith("ORD_"):
            if token[4:] not in defines:
                raise ValueError(f"ABI_TABLE row {token} has no #define")
            ordinal = defines[token[4:]]
        else:
            ordinal = int(token[:-1])
        rows[ordinal] = int(stack_args)
    if not rows:
        raise ValueError(f"ABI_TABLE block in {thunk_source} holds no rows")
    return rows


def oracle_pops(def_path: Path) -> dict[int, int | None]:
    """Ordinal -> callee-popped dwords per the CC0 oracle, None for DATA exports."""
    return {row.ordinal: row.callee_pop_dwords for row in load_rows(def_path)}


def oracle_disagreements(
    implemented: dict[int, str],
    hand: dict[int, int],
    measured: dict[int, ArityRow],
    oracle: dict[int, int | None],
    acknowledgements: dict[int, str],
) -> list[OracleDisagreement]:
    """Every implemented ordinal whose hand or measured arity differs from the oracle.

    Both sources are checked independently, so a measured row that a correct hand row
    happens to shadow is still reported: it is the row `unverified()` passes as
    confident, and the next regeneration may drop the hand row. Ordinals the oracle
    has no pop count for (absent, or DATA) are not comparable and are not listed.
    Live disagreements sort first, then by site count.
    """
    out: list[OracleDisagreement] = []
    for ordinal, name in implemented.items():
        truth = oracle.get(ordinal)
        if truth is None:
            continue
        reason = acknowledgements.get(ordinal)
        hand_value = hand.get(ordinal)
        if hand_value is not None and hand_value != truth:
            out.append(
                OracleDisagreement(
                    ordinal=ordinal,
                    name=name,
                    source="hand",
                    value=hand_value,
                    oracle=truth,
                    sites=0,
                    confident=True,
                    live=True,
                    shadowed_by=None,
                    acknowledged=reason,
                )
            )
        row = measured.get(ordinal)
        if row is not None and row.args != truth:
            live = hand_value is None and row.confident and row.sites >= MEASURED_ARITY_MIN_SITES
            out.append(
                OracleDisagreement(
                    ordinal=ordinal,
                    name=name,
                    source="measured",
                    value=row.args,
                    oracle=truth,
                    sites=row.sites,
                    confident=row.confident,
                    live=live,
                    shadowed_by=hand_value,
                    acknowledged=reason,
                )
            )
    out.sort(key=lambda d: (not d.live, -d.sites, d.ordinal, d.source))
    return out


def parse_data_ordinals(inc_text: str) -> set[int]:
    """The ordinals the scanner calls DATA, from `MEASURED_DATA_ORDINALS[]` in the .inc."""
    marker = "MEASURED_DATA_ORDINALS[]"
    if marker not in inc_text:
        return set()
    start = inc_text.index(marker)
    end = inc_text.index("}", start)
    return {int(value) for value in re.findall(r"(\d+)u", inc_text[start:end])}


@dataclass(frozen=True)
class DataConflicts:
    """Where the scanner's DATA verdict and the oracle's disagree (T35).

    The scanner's rule is `calls == 0 and reads > 0`, which establishes "read and never
    called in this image", not "is a variable". A function whose address is taken and
    stored, like `IoInvalidDeviceRequest`, satisfies it. The oracle's `DATA` keyword says
    what the export IS, so on a conflict the oracle's verdict is the classification.
    """

    #: Scanner says DATA, oracle says function: read but never called, not a variable.
    scanner_data_oracle_function: tuple[int, ...]
    #: Oracle says DATA, scanner says function: a variable the scanner thinks is called.
    oracle_data_scanner_function: tuple[int, ...]


def data_conflicts(
    scanner_data: set[int], imported: set[int], oracle: dict[int, int | None]
) -> DataConflicts:
    """Compare DATA verdicts over the ordinals this image imports.

    `oracle[ordinal] is None` means DATA (the only way `callee_pop_dwords` is None).
    An ordinal the oracle does not list at all is not a conflict, just unknown.
    """
    wrong = sorted(o for o in scanner_data if o in oracle and oracle[o] is not None)
    missed = sorted(
        o for o in imported if o in oracle and oracle[o] is None and o not in scanner_data
    )
    return DataConflicts(tuple(wrong), tuple(missed))


def render_data_conflicts(conflicts: DataConflicts) -> str:
    if not (conflicts.scanner_data_oracle_function or conflicts.oracle_data_scanner_function):
        return "data check: the scanner's DATA verdicts and the oracle's agree"
    return (
        "data check: scanner and oracle DISAGREE about DATA. The oracle's verdict wins.\n"
        f"  scanner DATA, oracle FUNCTION (read, never called): "
        f"{list(conflicts.scanner_data_oracle_function)}\n"
        f"  oracle DATA, scanner FUNCTION: {list(conflicts.oracle_data_scanner_function)}"
    )


def render_disagreements(items: list[OracleDisagreement]) -> str:
    """A report that cannot be mistaken for an all-clear."""
    if not items:
        return "oracle check: no implemented ordinal disagrees with the CC0 arity oracle"
    live = [i for i in items if i.live]
    lines = [
        f"oracle check: {len(items)} DISAGREEMENT(S) with the CC0 arity oracle, "
        f"{len(live)} LIVE (the host would pop the wrong count)",
        "  Never resolved here. A hand row beats a measured one at runtime, and the oracle is a",
        "  different kernel build, so decide which is right and record it as ARITY-OK.",
        "",
        f"  {'ord':>5} {'source':<8} {'value':>5} {'oracle':>6} {'sites':>6}  status  name",
    ]
    for i in items:
        if i.live:
            status = "LIVE"
        elif i.shadowed_by is not None:
            status = f"shadowed by hand row {i.shadowed_by}"
        else:
            status = "refused by host gates"
        claim = "" if i.confident or i.source == "hand" else " (flagged non-unanimous)"
        if i.confident and i.source == "measured":
            claim = " (CLAIMS CONFIDENCE)"
        review = "ACKNOWLEDGED" if i.acknowledged else "UNACKNOWLEDGED"
        lines.append(
            f"  {i.ordinal:>5} {i.source:<8} {i.value:>5} {i.oracle:>6} {i.sites:>6}  "
            f"{status}{claim}, {review}  {i.name}"
        )
    return "\n".join(lines)


def unacknowledged_live(items: list[OracleDisagreement]) -> list[OracleDisagreement]:
    """The disagreements that must fail a run: the host's own number is contested."""
    return [i for i in items if i.live and i.acknowledged is None]


def main(argv: list[str] | None = None) -> int:
    """Print both reports. Exit 1 on an unacknowledged live disagreement."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--src-xbox", type=Path, default=REPO_ROOT / "src" / "xbox")
    parser.add_argument("--thunk-source", type=Path, default=REPO_ROOT / "src/host/kernel_thunk.c")
    parser.add_argument(
        "--arity-inc", type=Path, default=REPO_ROOT / "generated/lifted/gen/kernel_arity.inc"
    )
    parser.add_argument("--oracle-def", type=Path, default=REPO_ROOT / DEFAULT_DEF_PATH)
    args = parser.parse_args(argv)

    implemented = implemented_ordinals(args.src_xbox)
    acks = acknowledged(args.src_xbox)
    if args.arity_inc.is_file():
        measured = parse_arity_table(args.arity_inc)
        print(render(unverified(measured, implemented, acks)))
    else:
        measured = {}
        print(f"arity check: SKIPPED the measured rows, {args.arity_inc} does not exist")
    hand = parse_hand_table(args.thunk_source, ordinal_defines(args.src_xbox))
    items = oracle_disagreements(implemented, hand, measured, oracle_pops(args.oracle_def), acks)
    print(render_disagreements(items))
    if args.arity_inc.is_file():
        scanner_data = parse_data_ordinals(args.arity_inc.read_text(encoding="utf-8"))
        # The .inc lists only ordinals the guest references, so that is the universe.
        referenced = set(measured) | scanner_data
        conflicts = data_conflicts(scanner_data, referenced, oracle_pops(args.oracle_def))
        print(render_data_conflicts(conflicts))
    return 1 if unacknowledged_live(items) else 0


if __name__ == "__main__":
    sys.exit(main())
