# SPDX-License-Identifier: GPL-3.0-or-later
"""Directed overlap verification for the lifted `_memmove`, judged byte for byte.

`selection.py` classifies `_memmove` (guest VA 0x003C9800) as `partial-decode`: its
embedded jump tables defeat the linear disassembly, so the random sweep can never claim
a verdict on it and it lands in the results as SKIPPED-UNSUPPORTED. That skip is
correct for the RANDOM harness, but it left the one libc routine whose overlap
semantics the title actually relies on verified only by a bespoke probe living in
`tmp/` (`tmp/memmove-probe/memmove_probe_srcoff3.py`), which a container restart or a
cleanup pass deletes.

This module folds that probe in as a first-class, repeatable check that runs on every
normal (non-replacement) harness invocation:

* **Directed cases, not random ones.** Overlap is a property random pointer-shaped
  inputs essentially never produce: `dst` must land INSIDE `[src, src+n)`. Every case
  here is a fully specified cdecl `memmove(dst, src, n)` call with hand-laid arenas.
* **Layouts.** Disjoint buffers at all four destination alignments (the head-alignment
  jump table dispatches on `dst & 3`), forward overlap (`dst < src`), and backward
  overlap (`dst > src`, dst inside the source range -- the case that leaves the
  672-byte body through a tail jump and that plain differential testing skips).
* **Sizes.** Zero length, single byte, sub-word and odd sizes, word multiples (the
  zero-tail-remainder arm), the 7-dword arm, and page-boundary-straddling sizes
  (0xFFF/0x1000/0x1001 from an unaligned source crosses a 4 KiB page).
* **Two independent judgements per case.** First the ordinary differential verdict
  (`compare`: registers plus the exact write-set) against the Unicorn oracle running
  the original bytes. Second, the subject's final destination bytes are compared
  byte for byte against host `memmove` semantics computed in Python (the source bytes
  as they stood BEFORE the call). The second check is what catches a case where both
  sides are wrong in the same way, and it is asserted on the destination component
  itself, never on an aggregate.

The selection skip is deliberately left in place: the random sweep still cannot judge
`_memmove`, and its `partial-decode` row still says so. What changes is that the
overlap verification now runs alongside it and reports itself BY NAME in the summary.
A run where the probe cannot execute prints a named SKIPPED line rather than nothing,
because a skip that does not name itself reads as a pass.
"""

from __future__ import annotations

import struct
import sys
from collections.abc import Callable
from dataclasses import dataclass, replace

from .compare import compare
from .model import Case, CaseResult, ExecResult, Outcome
from .seeding import GUEST_HI, GUEST_LO, SENTINEL

#: The MSVC CRT `memcpy`/`memmove` alignment dispatch in this title. The functions CSV
#: names it `_memmove`; the probe refuses to run when this VA is not a listed function
#: entry, so a different functions file cannot silently probe the wrong bytes.
MEMMOVE_VA = 0x003C9800

#: Probe case indices live far above the random (dense), edge (1 << 40) and feedback
#: (1 << 42) streams, so a `(seed, index)` pair can never name two different cases.
PROBE_INDEX_BASE = 1 << 44

#: Scratch arenas, inside the guest window and away from image code. 0x00D00000 is the
#: arena the production seeding already aims registers at; both arenas are fully
#: re-patched per case, so whatever the image holds there is irrelevant by construction.
SRC_ARENA = 0x00D00000
DST_ARENA = 0x00D20000
#: Large enough for the biggest size plus the source offset plus the widest delta.
ARENA_LEN = 0x1800
#: Source sits at an odd offset so unaligned-source arms run and a page boundary is
#: crossed by the 4 KiB-ish sizes.
SRC_OFFSET = 0x103
DST_OFFSET = 0x100

#: Entry esp. The cdecl frame sits at and above it, callee-saved pushes go below it.
STACK_ENTRY = 0x00E80000
STACK_PATCH_LO = STACK_ENTRY - 0x80
STACK_PATCH_LEN = 0xC0
#: The stack region is pre-filled so a spurious write below the frame changes a byte
#: and therefore shows up in the write-set.
STACK_FILLER = 0xAA

#: Register seeds. ebx is 0xFF so a historical spurious `or [esi+0x5F], bl` (removed by
#: lifter patch 12) would always flip a visible bit if it ever came back; the source
#: pattern stays below 0x80 for the same reason.
REGISTER_SEEDS = (
    0x11111111,  # eax
    0x22222222,  # ecx
    0x33333333,  # edx
    0x000000FF,  # ebx
    STACK_ENTRY,  # esp
    0x00E7FF00,  # ebp
    0x44444444,  # esi
    0x55555555,  # edi
)

#: Deliberate sizes: zero length, single byte, every small alignment remainder, the
#: word-multiple and 7-dword arms, odd tails, and three sizes that straddle a 4 KiB
#: page boundary when copied from the unaligned source offset.
PROBE_SIZES: tuple[int, ...] = (
    0,
    1,
    2,
    3,
    4,
    7,
    8,
    13,
    16,
    28,
    31,
    32,
    64,
    255,
    256,
    0x0FFF,
    0x1000,
    0x1001,
)

#: `(mode, delta)` layouts. `none` keeps the buffers disjoint and sweeps the
#: destination alignment 0..3 so every head arm of the alignment table runs. `fwd`
#: puts dst below src by `delta` (forward overlap). `bwd` puts dst ABOVE src by
#: `delta`, i.e. dst inside `[src, src+n)` whenever `delta < n`: the
#: backward-overlapping case this probe exists for.
PROBE_LAYOUTS: tuple[tuple[str, int], ...] = (
    ("none", 0),
    ("none", 1),
    ("none", 2),
    ("none", 3),
    ("fwd", 1),
    ("fwd", 2),
    ("fwd", 4),
    ("bwd", 1),
    ("bwd", 2),
    ("bwd", 4),
)


def src_pattern(length: int) -> bytes:
    """Non-zero source bytes, all below 0x80, never equal to a destination byte."""
    return bytes(((i * 7 + 0x11) & 0x7F) for i in range(length))


def dst_pattern(length: int) -> bytes:
    """Destination filler with the high bit always set, so no byte matches the source."""
    return bytes((((i * 13 + 0x25) & 0x7F) | 0x80) for i in range(length))


@dataclass(frozen=True)
class ProbeCase:
    """One fully specified memmove call plus everything needed to judge it."""

    label: str
    n: int
    mode: str
    delta: int
    src: int
    dst: int
    case: Case

    def baseline(self) -> dict[int, int]:
        """addr -> byte of the patched initial state, for reconstructing final memory."""
        state: dict[int, int] = {}
        for base, blob in self.case.patches:
            for i, byte in enumerate(blob):
                state[base + i] = byte
        return state

    def expected_destination(self, baseline: dict[int, int]) -> bytes:
        """Host memmove semantics: the source bytes as they stood BEFORE the call."""
        return bytes(baseline[self.src + i] for i in range(self.n))

    def final_destination(self, result: ExecResult, baseline: dict[int, int]) -> bytes:
        """The destination range after the run: the write-set overlaid on the baseline."""
        return bytes(result.writes.get(self.dst + i, baseline[self.dst + i]) for i in range(self.n))


@dataclass(frozen=True)
class ProbeCaseVerdict:
    """The two judgements on one probe case, kept separate on purpose."""

    label: str
    outcome: Outcome
    #: Subject's final destination bytes == host memmove model. None when the subject
    #: produced no final state to read (it faulted, or was never run).
    dest_matches_model: bool | None
    diagnosis: str

    @property
    def failed(self) -> bool:
        """A directed case that did not BOTH agree and match the model has failed.

        ORACLE-FAULTED and NOT-RUN are failures here, unlike in the random sweep:
        every probe input is hand-laid to be executable, so a case that reaches no
        verdict means the probe itself is broken and must not read as a pass.
        """
        if self.outcome is not Outcome.AGREE:
            return True
        return self.dest_matches_model is not True


@dataclass(frozen=True)
class ProbeReport:
    """What the probe did, rendered by name in the harness summary."""

    va: int
    #: Why the probe did not run, or None when it ran.
    skipped: str | None = None
    size: int = 0
    verdicts: tuple[ProbeCaseVerdict, ...] = ()

    @property
    def failures(self) -> tuple[ProbeCaseVerdict, ...]:
        return tuple(v for v in self.verdicts if v.failed)

    @property
    def model_checked(self) -> int:
        return sum(1 for v in self.verdicts if v.dest_matches_model is not None)

    @property
    def model_matched(self) -> int:
        return sum(1 for v in self.verdicts if v.dest_matches_model is True)

    @property
    def agreed(self) -> int:
        return sum(1 for v in self.verdicts if v.outcome is Outcome.AGREE)

    def render(self) -> str:
        """The named block for the run summary. A skip names itself out loud."""
        title = "MEMMOVE OVERLAP PROBE (directed overlap cases, judged byte for byte)"
        if self.skipped is not None:
            return "\n".join(
                [
                    "",
                    f"{title}",
                    f"  SKIPPED: {self.skipped}",
                    "  The overlap verification DID NOT RUN. This is not a pass.",
                ]
            )
        total = len(self.verdicts)
        bwd = sum(1 for v in self.verdicts if v.label.startswith("bwd"))
        fwd = sum(1 for v in self.verdicts if v.label.startswith("fwd"))
        lines = [
            "",
            title,
            f"  target                              : {self.va:#010x} ({self.size} bytes),"
            f" still skipped by the random sweep's selection",
            f"  directed cases                      : {total:,}"
            f" ({bwd} backward-overlap, {fwd} forward-overlap,"
            f" {total - bwd - fwd} disjoint)",
            f"  agree with oracle (regs + write-set): {self.agreed:,} of {total:,}",
            f"  destination == host memmove model   : {self.model_matched:,}"
            f" of {self.model_checked:,} checked",
        ]
        if self.failures:
            lines.append(f"  RESULT: FAIL ({len(self.failures)} case(s))")
            for verdict in self.failures[:10]:
                lines.append(f"    [{verdict.outcome}] {verdict.label}: {verdict.diagnosis}")
            remaining = len(self.failures) - 10
            if remaining > 0:
                lines.append(f"    ... and {remaining} more; see the CSV")
        else:
            lines.append("  RESULT: PASS")
        return "\n".join(lines)


def build_probe_cases(
    va: int,
    size: int,
    seed: int,
    *,
    sizes: tuple[int, ...] = PROBE_SIZES,
    layouts: tuple[tuple[str, int], ...] = PROBE_LAYOUTS,
) -> list[ProbeCase]:
    """Lay down the arenas and a cdecl frame for every (size, layout) combination."""
    probes: list[ProbeCase] = []
    src = SRC_ARENA + SRC_OFFSET
    src_blob = src_pattern(ARENA_LEN)
    dst_blob = dst_pattern(ARENA_LEN)
    for n in sizes:
        for mode, delta in layouts:
            if mode == "none":
                dst = DST_ARENA + DST_OFFSET + delta
                label = f"disjoint align={dst & 3} n={n}"
            elif mode == "fwd":
                dst = src - delta
                label = f"fwd d={delta} n={n}"
            elif mode == "bwd":
                dst = src + delta
                label = f"bwd d={delta} n={n}"
            else:
                raise ValueError(f"unknown overlap mode {mode!r}")

            for base, length in (
                (SRC_ARENA, ARENA_LEN),
                (DST_ARENA, ARENA_LEN),
                (STACK_PATCH_LO, STACK_PATCH_LEN),
            ):
                if base < GUEST_LO or base + length > GUEST_HI:
                    raise ValueError(f"arena {base:#010x}+{length:#x} leaves the guest window")
            arenas = ((SRC_ARENA, SRC_ARENA + ARENA_LEN), (DST_ARENA, DST_ARENA + ARENA_LEN))
            for start in (src, dst):
                if not any(lo <= start and start + n <= hi for lo, hi in arenas):
                    raise ValueError(f"case {label} overruns its arena")

            # Frame: [esp]=return address, [esp+4]=dst, [esp+8]=src, [esp+12]=n.
            stack = bytearray([STACK_FILLER] * STACK_PATCH_LEN)
            struct.pack_into("<IIII", stack, STACK_ENTRY - STACK_PATCH_LO, SENTINEL, dst, src, n)

            case = Case(
                seed=seed,
                index=PROBE_INDEX_BASE + len(probes),
                va=va,
                size=size,
                regs=REGISTER_SEEDS,
                df=0,
                patches=(
                    (SRC_ARENA, src_blob),
                    (DST_ARENA, dst_blob),
                    (STACK_PATCH_LO, bytes(stack)),
                ),
            )
            probes.append(
                ProbeCase(label=label, n=n, mode=mode, delta=delta, src=src, dst=dst, case=case)
            )
    return probes


def judge_probe_case(
    probe: ProbeCase,
    oracle_result: ExecResult,
    subject_result: ExecResult,
) -> tuple[CaseResult, ProbeCaseVerdict]:
    """Both judgements for one case: the differential verdict and the model check.

    The model check reads the SUBJECT's final destination bytes. Judging the oracle's
    destination instead, or the source range, would let a wrong lifted copy hide
    behind a correct original; the mutation test in `tests/test_harness_memmove_probe.py`
    exists to catch exactly that substitution.
    """
    result = compare(probe.case, oracle_result, subject_result)
    dest_matches: bool | None = None
    diagnosis = result.diagnosis
    if not subject_result.faulted:
        baseline = probe.baseline()
        got = probe.final_destination(subject_result, baseline)
        expected = probe.expected_destination(baseline)
        dest_matches = got == expected
        if not dest_matches:
            first_bad = next(
                i for i, (g, e) in enumerate(zip(got, expected, strict=True)) if g != e
            )
            mismatch = (
                f"dest[{first_bad}] at {probe.dst + first_bad:#010x} is "
                f"{got[first_bad]:#04x}, host memmove model says {expected[first_bad]:#04x}"
            )
            diagnosis = f"{diagnosis}; {mismatch}" if diagnosis else mismatch

    note = f"memmove-probe {probe.label}"
    if diagnosis and diagnosis != result.diagnosis:
        note = f"{note}: DEST-MISMATCH {diagnosis}"
    if not result.divergences:
        result = replace(result, note=note)
    verdict = ProbeCaseVerdict(
        label=probe.label,
        outcome=result.outcome,
        dest_matches_model=dest_matches,
        diagnosis=diagnosis or note,
    )
    return result, verdict


def run_memmove_probe(
    functions: list[tuple[int, int]],
    runner: Callable[[Case], tuple[ExecResult, ExecResult]],
    *,
    seed: int,
    va: int = MEMMOVE_VA,
    write_case: Callable[[CaseResult], None] | None = None,
    skip_reason: str | None = None,
    sizes: tuple[int, ...] = PROBE_SIZES,
    layouts: tuple[tuple[str, int], ...] = PROBE_LAYOUTS,
) -> ProbeReport:
    """Run the directed overlap cases through the normal oracle/subject pair.

    `runner` executes one `Case` on both sides (the CLI passes `run_case` bound to its
    oracle and subject), so the probe reuses the production execution path rather than
    reimplementing it. `write_case` lets every probe case land in the same results CSV
    as the sweep, labelled `memmove-probe ...` in the diagnosis column.

    `skip_reason` is how a caller that cannot run the probe (a replacement run, an
    `--only-va` aimed elsewhere, an exhausted time budget) still produces a NAMED skip
    instead of silence.
    """
    if skip_reason is not None:
        return ProbeReport(va=va, skipped=skip_reason)
    size = next((s for v, s in functions if v == va), None)
    if size is None:
        return ProbeReport(
            va=va, skipped=f"{va:#010x} is not a function entry in the functions CSV"
        )

    verdicts: list[ProbeCaseVerdict] = []
    for probe in build_probe_cases(va, size, seed, sizes=sizes, layouts=layouts):
        oracle_result, subject_result = runner(probe.case)
        result, verdict = judge_probe_case(probe, oracle_result, subject_result)
        if write_case is not None:
            write_case(result)
        if verdict.failed:
            print(
                f"MEMMOVE-PROBE FAIL [{verdict.outcome}] {verdict.label}: {verdict.diagnosis}",
                file=sys.stderr,
            )
        verdicts.append(verdict)
    return ProbeReport(va=va, size=size, verdicts=tuple(verdicts))


__all__ = [
    "MEMMOVE_VA",
    "PROBE_INDEX_BASE",
    "PROBE_LAYOUTS",
    "PROBE_SIZES",
    "ProbeCase",
    "ProbeCaseVerdict",
    "ProbeReport",
    "build_probe_cases",
    "judge_probe_case",
    "run_memmove_probe",
]
