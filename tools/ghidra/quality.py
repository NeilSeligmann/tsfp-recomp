# SPDX-License-Identifier: GPL-3.0-or-later
"""Score the health of a decompilation export.

Raw decompiler output is not uniformly trustworthy, and the untrustworthy parts
announce themselves. This module counts those markers so the pipeline's quality
is a tracked number rather than an impression, and so a change to the analysis
configuration can be judged by whether it moves them.

Markers, and what each means:

* `unaff_<REG>` — Ghidra could not resolve a register-passed argument, so the
  function reads a register the caller supposedly left untouched. The prototype
  is wrong and the body is suspect.
* `in_<REG>` — the function reads a register with no defined value on entry. Same
  root cause as above in most cases, and the most common marker by far.
* `halt_baddata` — the decompiler walked into bytes it could not treat as code.
* `UNRECOVERED_JUMPTABLE` — a switch whose targets were not recovered, so control
  flow is incomplete.
* `WARNING:` — the decompiler's own inline caveats, of ALL kinds.

Baseline measured on the TimeSplitters: Future Perfect Xbox build, 12,343
functions: 70.0% clean, 19.5% `in_*`, 8.5% warning-bearing, 5.4% `unaff_*`.
With Decompiler Parameter ID enabled: 82.79% clean, 4.9% `in_*`.

**`control_flow` WAS A MISNOMER AND MISDIRECTED PLANNING WORK.** It matched a bare
`WARNING:`, so it counted every caveat the decompiler emits, not control-flow
failures. On this build its 1,053 functions decompose as:

| warning | functions |
|---|---|
| `Globals starting with '_' overlap smaller symbols` | **735** — cosmetic |
| `__security_check_cookie` / `__SEH_prolog` injections | 150 |
| `Removing unreachable block` | 86 |
| **genuine jump-table failure** | **72** |
| `UNRECOVERED_JUMPTABLE` literal | 8 |
| other | ~40 |

So the real jump-table failure set is **75 sites in 72 functions, 7.1% of the
bucket** — not the largest remaining lever, which is what `context.md` §6k
concluded from this metric before it was decomposed.

`RE_JUMPTABLE` now isolates the genuine cases and `RE_WARNING` keeps the broad
sweep, reported separately. `RE_CONTROL_FLOW` is retained as the union so the
"clean of all markers" headline stays comparable with the 69.91% and 82.79%
figures already recorded; only the sub-category breakdown changes.
"""

from __future__ import annotations

import re
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

RE_UNAFF = re.compile(r"\bunaff_(E[A-Z]{2}|[A-Z]{2})\b")
RE_IN_REG = re.compile(r"\bin_(E[A-Z]{2}|ST\d|[A-Z]{2})\b")
#: Genuine control-flow recovery failures: the decompiler could not determine where
#: execution goes. These are the ones that actually block a static recompiler.
RE_JUMPTABLE = re.compile(
    r"halt_baddata|UNRECOVERED_JUMPTABLE"
    r"|Could not recover jumptable"
    r"|Treating indirect jump as call"
)

#: Every inline caveat, most of which are cosmetic. Kept because it is a useful
#: coarse signal, but it must NOT be read as a control-flow figure.
RE_WARNING = re.compile(r"WARNING:")

#: The union, preserved so the "clean of all markers" headline remains comparable
#: with the 69.91% / 82.79% figures already recorded. Do not narrow this without
#: re-measuring both baselines.
RE_CONTROL_FLOW = re.compile(rf"{RE_JUMPTABLE.pattern}|{RE_WARNING.pattern}")


@dataclass
class QualityReport:
    """Counts of decompiler health markers across an export."""

    total: int = 0
    with_unaff: int = 0
    with_in_reg: int = 0
    with_control_flow: int = 0
    #: Genuine control-flow failures, a strict subset of with_control_flow.
    with_jumptable: int = 0
    clean: int = 0
    registers: Counter[str] = field(default_factory=Counter)

    def percent(self, count: int) -> float:
        return 100.0 * count / self.total if self.total else 0.0

    def render(self) -> str:
        lines = [f"functions scanned: {self.total}"]
        for label, count in (
            ("unresolved register args (unaff_*)", self.with_unaff),
            ("undefined register reads (in_*)", self.with_in_reg),
            ("warning-bearing (broad)", self.with_control_flow),
            ("  of which real control-flow failures", self.with_jumptable),
            ("clean of all markers", self.clean),
        ):
            lines.append(f"  {label:36s} {count:6d}  {self.percent(count):5.1f}%")
        if self.registers:
            seen = ", ".join(f"{reg}={n}" for reg, n in self.registers.most_common())
            lines.append(f"  registers implicated: {seen}")
        return "\n".join(lines)


def score_source(text: str) -> tuple[bool, bool, bool, list[str]]:
    """Classify one decompiled function. Returns the three flags and registers.

    The third flag is the BROAD `RE_CONTROL_FLOW` union, kept that way so the
    "clean of all markers" headline stays comparable with recorded baselines. For
    genuine control-flow failures use `has_jumptable_failure` instead.
    """
    unaff = RE_UNAFF.findall(text)
    in_regs = RE_IN_REG.findall(text)
    control_flow = bool(RE_CONTROL_FLOW.search(text))
    return bool(unaff), bool(in_regs), control_flow, unaff


def has_jumptable_failure(text: str) -> bool:
    """Whether the decompiler could not determine where control flow goes.

    This is the figure that matters for a static recompiler, and it is far smaller
    than the broad warning count: measured 72 functions against 1,053 warning-bearing
    ones, because the broad sweep is dominated by a cosmetic symbol-overlap caveat.
    """
    return bool(RE_JUMPTABLE.search(text))


def score_export(root: Path) -> QualityReport:
    """Scan every `.c` file under `root` and summarise marker counts."""
    report = QualityReport()
    for path in sorted(root.rglob("*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        has_unaff, has_in, has_cf, registers = score_source(text)
        report.total += 1
        if has_unaff:
            report.with_unaff += 1
            report.registers.update(registers)
        if has_in:
            report.with_in_reg += 1
        if has_cf:
            report.with_control_flow += 1
        if has_jumptable_failure(text):
            report.with_jumptable += 1
        if not (has_unaff or has_in or has_cf):
            report.clean += 1
    return report
