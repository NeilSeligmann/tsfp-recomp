# SPDX-License-Identifier: GPL-3.0-or-later
"""The data the harness moves between its two sides, and the verdicts it reaches.

Deliberately free of both Unicorn and the real binary so the comparison logic can be
unit-tested, and mutation-tested, against synthetic fixtures alone. `oracle.py` and
`subject.py` are the only modules that touch the outside world; both of them reduce
their side to an `ExecResult`, and everything downstream works on that.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import StrEnum

from .fault_diagnostics import FaultObservation
from .x87_state import X87State

# The eight integer registers, in the order the wire protocol uses. There is no EIP
# because the subject's control flow is native C, and no segment registers because the
# guest is flat-mapped.
REG_NAMES: tuple[str, ...] = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
REG_COUNT = len(REG_NAMES)

FLAG_CF = 0x0001
FLAG_PF = 0x0004
FLAG_AF = 0x0010
FLAG_ZF = 0x0040
FLAG_SF = 0x0080
FLAG_DF = 0x0400
FLAG_OF = 0x0800

FLAG_BITS: tuple[tuple[int, str], ...] = (
    (FLAG_CF, "CF"),
    (FLAG_PF, "PF"),
    (FLAG_AF, "AF"),
    (FLAG_ZF, "ZF"),
    (FLAG_SF, "SF"),
    (FLAG_DF, "DF"),
    (FLAG_OF, "OF"),
)

# Compared only when BOTH sides supply a flags word. The production subject supplies
# None: the lifter does not store flags at all, it reconstructs each condition
# symbolically at the consumer, so there is no flags word to read back. Flag errors are
# therefore caught in real runs only through their EFFECT on a register or a memory
# write. The channel exists because the comparison has to be correct for any subject
# that can report flags, and because the mutation suite exercises it directly.
COMPARED_FLAGS = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF

# The bits a --publish-eflags lift can ever CLAIM to model: the five probe-able
# arithmetic flags plus DF, which the lifter models unconditionally as g_df.
# AF is deliberately absent -- nothing in the lifter models AF -- so with this
# as the comparison mask AF is never compared, and the reporting below counts
# it as unmodelled on every flags-observed case rather than skipping it
# silently. See `tools/harness/compare.compare` and the EFLAGS summary block.
LIFTER_MODELED_FLAGS = FLAG_CF | FLAG_PF | FLAG_ZF | FLAG_SF | FLAG_DF | FLAG_OF

#: Every flag the EFLAGS channel accounts for, modelled or not. The unmodelled
#: remainder of this set is REPORTED per case, never silently dropped.
TRACKED_FLAGS = LIFTER_MODELED_FLAGS | FLAG_AF


class Outcome(StrEnum):
    """What the harness concluded about one case.

    Every generated case lands in exactly one of these and every one is reported. A
    case that quietly vanishes is how a harness ends up flattering itself, so there is
    deliberately no "ignored" value.
    """

    AGREE = "AGREE"
    DISAGREE = "DISAGREE"
    #: The original code faulted, so there is no reference to compare against. Not a
    #: pass and not a failure: the input was rejected by the hardware itself.
    ORACLE_FAULTED = "ORACLE-FAULTED"
    #: The original code ran cleanly and the lifted code did not. A real divergence,
    #: kept separate from DISAGREE only because the diagnosis is of a different shape.
    SUBJECT_FAULTED = "SUBJECT-FAULTED"
    #: Never executed, because the function contains instruction classes this harness
    #: does not claim to verify (x87, SSE, privileged). Counted and reported.
    SKIPPED_UNSUPPORTED = "SKIPPED-UNSUPPORTED"


#: Outcomes that mean the lifted code is wrong.
DIVERGENT_OUTCOMES: frozenset[Outcome] = frozenset({Outcome.DISAGREE, Outcome.SUBJECT_FAULTED})

#: Outcomes where a comparison actually happened. Every figure the harness reports is
#: divided by a count of THESE, never by the number of cases run: 41% of cases reached no
#: verdict in the standing run, so a percentage over cases would overstate the evidence by
#: nearly a factor of two.
VERDICT_OUTCOMES: frozenset[Outcome] = frozenset(
    {Outcome.AGREE, Outcome.DISAGREE, Outcome.SUBJECT_FAULTED}
)

#: Oracle "faults" that are THE HARNESS giving up, not the guest taking a trap. A
#: budget exhaustion says nothing about the guest: Unicorn counts instructions and the
#: subject does not, so a loop needing more than the budget is simply truncated on the
#: oracle side while the compiled C runs it to completion. Treating that as the subject
#: swallowing a fault produces confident, wrong DISAGREE reports -- which is worse than
#: no harness at all, because it trains readers to ignore the output.
ORACLE_BUDGET_FAULTS: frozenset[str] = frozenset({"COUNT-LIMIT"})

#: T1620 fp-scalar-v1: the opt-in scalar-float vector proof mode (docs/t-vector-scalar-design.md).
FP_SCALAR_MODE = "fp-scalar-v1"
#: MXCSR control word of the mode: round to nearest, FTZ=0, DAZ=0, all exceptions masked.
FP_SCALAR_MXCSR = 0x1F80
#: Sticky status bits IE DE ZE OE UE PE. Unicorn never sets them (MEASURED, docs/evidence/t1620),
#: so the mode compares MXCSR exactly outside this mask. Justified by tools/mxcsr_scan.py.
FP_SCALAR_STATUS_MASK = 0x3F

#: T1620: the fp-scalar-v1 NaN-pair tripwire stops the oracle when an add/sub/mul/div/comiss
#: has two NaN operands with different bit patterns (Unicorn and silicon disagree on which NaN
#: is returned). The case is a counted non-verdict, never an agree.
NAN_PAIR_FAULT = "NAN-PAIR"
ORACLE_NONVERDICT_FAULTS: frozenset[str] = ORACLE_BUDGET_FAULTS | frozenset({NAN_PAIR_FAULT})

#: Subject faults that mean there is no implementation to judge, as opposed to an
#: implementation that misbehaved. The lifter simply emitted nothing for this address,
#: so calling it a divergence would blame the lifted code for not existing -- and
#: calling it an agreement would be far worse. It is a skip, and it is counted.
ABSENT_SUBJECT_FAULTS: frozenset[str] = frozenset({"NOFUNC"})


@dataclass(frozen=True)
class Case:
    """One fully-specified test case, regenerable from `(seed, index)` alone.

    `seed` and `index` are carried through to the results CSV precisely so a reported
    divergence can be reproduced without the run that found it. See
    `tools/harness/seeding.py`.
    """

    seed: int
    index: int
    va: int
    size: int
    regs: tuple[int, ...]
    df: int
    #: Guest memory written before the call: the stack frame and a scratch arena. These
    #: are part of the INITIAL state, not of the write-set, so both sides must apply
    #: them before snapshotting.
    patches: tuple[tuple[int, bytes], ...] = ()
    #: x87 stack at entry, top first, as doubles. Empty for a function that never touches
    #: the FPU. Both sides load it, so it is initial state like the registers (T356).
    fp_stack: tuple[float, ...] = ()
    #: x87 control word at entry (default 0x037F: round to nearest, extended precision).
    fp_control: int = 0x037F
    #: Opt-in complete raw XMM register file (XMM0..7), never float-converted.
    xmm: tuple[int, ...] | None = None
    mxcsr: int = 0x1F80
    x87_state: X87State | None = None
    #: T1620: "" is the legacy vector mode, "fp-scalar-v1" selects the scalar-float comparison
    #: (masked sticky MXCSR status). Never serialised into legacy case streams or receipts.
    vector_mode: str = ""

    def __post_init__(self) -> None:
        if self.x87_state is not None and not isinstance(self.x87_state, X87State):
            raise ValueError("raw x87 case requires X87State")
        if self.x87_state is not None and (self.fp_stack or self.fp_control != 0x037F):
            raise ValueError("raw x87 case conflicts with legacy FP input")
        if self.xmm is not None and (
            len(self.xmm) != 8 or any(not 0 <= value < 1 << 128 for value in self.xmm)
        ):
            raise ValueError("vector case needs eight unsigned 128-bit XMM registers")
        if not 0 <= self.mxcsr <= 0xFFFF:
            raise ValueError("MXCSR reserved high bits must be zero")
        if len(self.regs) != REG_COUNT:
            raise ValueError(f"case {self.index}: expected {REG_COUNT} registers")

    @property
    def esp(self) -> int:
        return self.regs[REG_NAMES.index("esp")]


@dataclass(frozen=True)
class Reachability:
    """How much of the function this case actually executed, measured on the oracle.

    An AGREE on a function whose every case returns after three instructions is almost
    no evidence, but it counts exactly as much towards the AGREE total as an AGREE on a
    function that was driven through all of its branches. Random registers do not make a
    body reachable: two mutations once survived this harness because the mutated line sat
    behind a branch gated on a guest global that loads as 0, leaving the code dead on all
    32 cases. So coverage is measured and reported rather than assumed, and the summary
    says how many AGREEs rest on bodies that were barely entered.

    `covered_vas` holds only addresses INSIDE the function body: a case that tail-calls
    out of the body should not be able to inflate its own coverage with another
    function's instructions.
    """

    insns: int = 0
    covered_vas: frozenset[int] = frozenset()
    #: Stubbed callees actually reached. Zero on a call-bearing function means this case
    #: never got as far as its call site.
    stub_applied: int = 0
    #: Real execution of an exact-byte and lifted-body verified stack-probe callee.
    passthrough_applied: int = 0
    #: `(address, size, value)` for every guest memory load, in order, when the oracle was
    #: built with `record_loads`. What the function READ is what a hand-written replacement
    #: must be checked against, and what `feedback.py` mutates to reach guarded branches.
    loads: tuple[tuple[int, int, int], ...] = ()

    @property
    def covered(self) -> int:
        return len(self.covered_vas)


@dataclass(frozen=True)
class ExecResult:
    """One side's outcome: either a final state, or a fault.

    `writes` is a byte-granular write-SET, not a write-LOG: it maps a guest address to
    the byte finally found there, and only for addresses whose value actually changed.
    A store of the value already present is therefore invisible, identically on both
    sides, which is what makes the two sets comparable at all.
    """

    regs: tuple[int, ...] | None = None
    writes: dict[int, int] = field(default_factory=dict)
    flags: int | None = None
    #: Which bits of `flags` the SUBJECT claims its model answered at the exit it
    #: returned through (`g_harness_eflags_mask`). None for the oracle, whose flags
    #: are the hardware's and carry no claim, and for a subject not built from a
    #: --publish-eflags tree. 0 for a publish-capable subject whose executed exit
    #: published nothing (a stubbed tail call, or driver reset never overwritten),
    #: which is reported as UNPUBLISHED rather than compared.
    flags_mask: int | None = None
    fault: str | None = None
    #: T1576: raw fault address from a driver run with HARNESS_FAULT_ADDR (identity-mapped guest).
    #: Never compared and never serialised, so existing receipts are unchanged.
    fault_addr: int | None = None
    fault_observation: FaultObservation | None = None
    #: Oracle-side telemetry. Deliberately NOT part of the comparison: it describes how
    #: much of the function ran, not what it computed, so `compare` never reads it.
    reach: Reachability = field(default_factory=Reachability)
    #: `g_seh_ebp` as the subject left it. The lifter publishes TWO frame pointers and
    #: lifter patch 10 deliberately restores only `g_ebp`, because this one is a handoff
    #: channel to the next callee. It has no hardware counterpart, so the oracle supplies
    #: None and `compare` NEVER reads it: there is nothing to compare it against, and
    #: inventing a comparison would manufacture divergences out of a design decision.
    #: What it does support is one internal-consistency question, reported in the summary.
    seh_ebp: int | None = None
    #: Whether the subject said the dispatch table returned a registered hand-written
    #: replacement for this address (`REPL 1`). None for a subject that has no replacements
    #: linked, which is every subject but the one `--replacement` runs against.
    replaced: bool | None = None
    #: x87 stack at exit, top first, each entry the IEEE double bit pattern. None when
    #: the side did not report it (a subject without x87 support). The depth is its length.
    fp: tuple[int, ...] | None = None
    #: x87 control word at exit (T420). None when the side did not report it.
    fp_control: int | None = None
    #: Architectural x87 status word plus the bits this execution model claims.
    fp_status: int | None = None
    fp_status_mask: int | None = None
    #: x87 stack at exit, top first, as exact 80-bit `(mantissa, sign_exponent)` pairs. The
    #: oracle reports the register contents, the subject the exact widening of its doubles.
    fp_ext: tuple[tuple[int, int], ...] | None = None
    xmm: tuple[int, ...] | None = None
    mxcsr: int | None = None
    x87_state: X87State | None = None

    def __post_init__(self) -> None:
        if self.fault_observation is not None and (
            type(self.fault_observation) is not FaultObservation
            or self.fault != self.fault_observation.label
        ):
            raise ValueError("fault observation must match actual fault label")
        if self.x87_state is not None and not isinstance(self.x87_state, X87State):
            raise ValueError("raw x87 result requires X87State")
        if self.x87_state is not None and any(
            value is not None
            for value in (
                self.fp,
                self.fp_ext,
                self.fp_control,
                self.fp_status,
                self.fp_status_mask,
            )
        ):
            raise ValueError("raw x87 result conflicts with legacy FP channels")
        if self.fault is None and (self.regs is None or len(self.regs) != REG_COUNT):
            raise ValueError("a clean ExecResult needs all eight registers")

    @property
    def faulted(self) -> bool:
        return self.fault is not None


@dataclass(frozen=True)
class Divergence:
    """One concrete way the two sides differed, phrased so it can be acted on."""

    kind: str
    detail: str

    def __str__(self) -> str:
        return f"{self.kind}: {self.detail}"


@dataclass(frozen=True)
class CaseResult:
    """The verdict on one case, plus everything needed to reproduce or explain it."""

    seed: int
    index: int
    va: int
    size: int
    outcome: Outcome
    divergences: tuple[Divergence, ...] = ()
    oracle_fault: str | None = None
    subject_fault: str | None = None
    note: str = ""
    fault_diagnostic: str = ""
    #: This function's callees were replaced by the synthetic stub on both sides, so the
    #: verdict is about the caller only. Recorded per case so the summary can separate
    #: stubbed evidence from unstubbed evidence instead of pooling them.
    stubbed: bool = False
    reach: Reachability = field(default_factory=Reachability)
    #: Instructions in the function body, from the selection's decode.
    body_insns: int = 0
    delegation_ratio: float = 0.0
    #: `g_seh_ebp` as the subject left it, and whether it ended up disagreeing with the
    #: subject's own final `ebp`. An OBSERVATION, not a verdict: there is no oracle
    #: counterpart, so `outcome` is never influenced by it. It exists because the second
    #: published frame pointer has 21,716 writes against `g_ebp`'s 16,253 and no run of
    #: this harness could previously say whether it was ever left stale.
    subject_seh_ebp: int | None = None
    seh_ebp_drifted: bool = False
    #: EFLAGS channel accounting, nonzero/True only when BOTH sides supplied a
    #: flags word (a --compare-eflags run against a --publish-eflags subject).
    #: `flags_compared_mask` is the bits actually compared on this case,
    #: `flags_disagree_mask` the compared bits that differed, and
    #: `flags_unmodeled_mask` the TRACKED_FLAGS bits that were NOT compared --
    #: AF always, plus whatever the executed exit's publish mask did not claim.
    #: Reported per flag in the summary, never silently skipped.
    flags_observed: bool = False
    flags_compared_mask: int = 0
    flags_disagree_mask: int = 0
    flags_unmodeled_mask: int = 0

    @property
    def flags_unpublished(self) -> bool:
        """The subject could publish flags but this case's exit published none."""
        return self.flags_observed and self.flags_compared_mask == 0

    @property
    def coverage(self) -> float:
        """Fraction of the body this ONE case executed. 0.0 when the body size is unknown."""
        if not self.body_insns:
            return 0.0
        return self.reach.covered / self.body_insns

    @property
    def diagnosis(self) -> str:
        """A single line a human can act on, or the note when there is nothing to say."""
        if self.divergences:
            return "; ".join(str(d) for d in self.divergences)
        return self.note

    @property
    def kinds(self) -> str:
        return ",".join(dict.fromkeys(d.kind for d in self.divergences))


@dataclass(frozen=True)
class SkippedFunction:
    """A function no case was generated for, and exactly why.

    Carried all the way into the results CSV. An unreported skip is indistinguishable
    from a pass when someone reads the totals later.
    """

    va: int
    size: int
    reason: str
