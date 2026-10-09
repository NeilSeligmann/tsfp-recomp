# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn one oracle result and one subject result into a verdict.

Registers alone are not enough. A function that corrupts guest memory and then returns
a correct `eax` is the failure mode that matters most here, because it is invisible at
the call site and only surfaces much later somewhere unrelated. So the write-set is
compared byte for byte and a memory-only divergence is a full DISAGREE.

The four-way fault matrix is the other load-bearing decision:

    oracle   subject   verdict
    ------   -------   -------
    clean    clean     AGREE if registers, flags and write-set all match, else DISAGREE
    clean    fault     SUBJECT-FAULTED -- the original ran, the lifted code did not
    fault    fault     ORACLE-FAULTED  -- no reference exists, so no verdict is possible
    fault    clean     DISAGREE        -- the lifted code SWALLOWED a fault

That last row is the one that is easy to get wrong. Classifying it as ORACLE-FAULTED
would hide precisely the lifter defect `docs/lifter-evaluation.md` §8.2 names as the
worst one: an unresolved indirect call that sets `eax = 0` and sails on where the real
machine would have trapped. A harness that files that under "no verdict" is useless.
"""

from __future__ import annotations

from dataclasses import replace

from .fault_diagnostics import diagnose, fault_parity
from .model import (
    ABSENT_SUBJECT_FAULTS,
    COMPARED_FLAGS,
    FLAG_BITS,
    FP_SCALAR_MODE,
    FP_SCALAR_STATUS_MASK,
    ORACLE_NONVERDICT_FAULTS,
    REG_NAMES,
    TRACKED_FLAGS,
    Case,
    CaseResult,
    Divergence,
    ExecResult,
    Outcome,
)
from .x87 import extended_equal, extended_fits_double
from .x87_state import differences

#: How many example addresses to name per memory divergence. Enough to debug, few
#: enough that a CSV cell stays readable.
MEMORY_EXAMPLES = 3


def compare_registers(
    oracle: ExecResult, subject: ExecResult, *, ignored: frozenset[str] = frozenset()
) -> list[Divergence]:
    """One divergence per differing register, named, with both values.

    `ignored` names registers the SUBJECT does not promise to match, which only a
    hand-written replacement ever declares (see `tools/harness/replacement.py`): the
    caller-saved scratch registers of its calling convention. It is empty for every lifted
    function, which is judged on all eight.
    """
    assert oracle.regs is not None and subject.regs is not None
    out: list[Divergence] = []
    for name, subject_value, oracle_value in zip(REG_NAMES, subject.regs, oracle.regs, strict=True):
        if name in ignored:
            continue
        if subject_value != oracle_value:
            out.append(
                Divergence(
                    "register",
                    f"{name} subject={subject_value:08X} oracle={oracle_value:08X}",
                )
            )
    return out


def compare_flags(
    oracle: ExecResult, subject: ExecResult, *, mask: int = COMPARED_FLAGS
) -> list[Divergence]:
    """Differing arithmetic flags, or nothing at all when either side has no flags word.

    Returning nothing for a missing flags word is NOT a pass. It is the production
    case, and the summary says so explicitly rather than letting silence read as
    agreement.
    """
    if oracle.flags is None or subject.flags is None:
        return []
    differing = (oracle.flags ^ subject.flags) & mask
    if not differing:
        return []
    named = [
        f"{label}(subject={1 if subject.flags & bit else 0} "
        f"oracle={1 if oracle.flags & bit else 0})"
        for bit, label in FLAG_BITS
        if differing & bit
    ]
    return [Divergence("flags", " ".join(named))]


def fp_inexact(oracle: ExecResult, subject: ExecResult) -> bool:
    """Does the oracle hold an exit x87 value the subject's double stack cannot represent?

    Such a value can only be compared after narrowing to a double, which is weaker than the
    exact 80-bit comparison, so the case is reported as not exactly verified instead of AGREE.
    """
    if oracle.fp_ext is None or subject.fp_ext is None:
        return False
    return any(not extended_fits_double(*value) for value in oracle.fp_ext)


def compare_raw_x87(case: Case, oracle: ExecResult, subject: ExecResult) -> list[Divergence]:
    """Required raw state never falls back to legacy double observations."""
    if case.x87_state is None:
        return []
    if oracle.x87_state is None or subject.x87_state is None:
        raise ValueError("missing required raw x87 execution observation")
    result = []
    for field in differences(oracle.x87_state, subject.x87_state):
        if field.startswith("physical["):
            index = int(field[9:-1])
            left, right = oracle.x87_state.physical[index], subject.x87_state.physical[index]
            detail = f"{field} subject={right:020X} oracle={left:020X}"
        else:
            left, right = getattr(oracle.x87_state, field), getattr(subject.x87_state, field)
            detail = f"{field} subject={right:04X} oracle={left:04X}"
        result.append(Divergence("x87-state", detail))
    return result


def compare_vector(case: Case, oracle: ExecResult, subject: ExecResult) -> list[Divergence]:
    """An opted-in vector case requires complete state on BOTH sides."""
    if case.xmm is None:
        return []
    if (
        oracle.xmm is None
        or subject.xmm is None
        or len(oracle.xmm) != 8
        or len(subject.xmm) != 8
        or oracle.mxcsr is None
        or subject.mxcsr is None
    ):
        return [Divergence("vector-unreported", "complete XMM/MXCSR output required")]
    differences = [
        Divergence(f"xmm{i}", f"subject={right:032X} oracle={left:032X}")
        for i, (left, right) in enumerate(zip(oracle.xmm, subject.xmm, strict=True))
        if left != right
    ]
    if oracle.mxcsr != subject.mxcsr:
        differences.append(
            Divergence("mxcsr", f"subject={subject.mxcsr:04X} oracle={oracle.mxcsr:04X}")
        )
    return differences


def compare_scalar_fp(case: Case, oracle: ExecResult, subject: ExecResult) -> list[Divergence]:
    """T1620 fp-scalar-v1: raw xmm lanes exact, MXCSR exact outside the sticky status mask.

    Never converts to float. Dead scratch registers are not exempted. A side that reports no
    xmm or MXCSR refuses the verdict, exactly like the legacy comparison.
    """
    if case.xmm is None:
        return [Divergence("vector-unreported", "fp-scalar case without raw xmm state")]
    if (
        oracle.xmm is None
        or subject.xmm is None
        or len(oracle.xmm) != 8
        or len(subject.xmm) != 8
        or oracle.mxcsr is None
        or subject.mxcsr is None
    ):
        return [Divergence("vector-unreported", "complete XMM/MXCSR output required")]
    differences = [
        Divergence(f"xmm{i}", f"subject={right:032X} oracle={left:032X}")
        for i, (left, right) in enumerate(zip(oracle.xmm, subject.xmm, strict=True))
        if left != right
    ]
    if (oracle.mxcsr ^ subject.mxcsr) & ~FP_SCALAR_STATUS_MASK & 0xFFFF:
        differences.append(
            Divergence("mxcsr", f"subject={subject.mxcsr:04X} oracle={oracle.mxcsr:04X}")
        )
    return differences


def compare_fp(oracle: ExecResult, subject: ExecResult) -> list[Divergence]:
    """The exit x87 state: control word, depth, then every value (T356, T420).

    The control word is compared exactly. A value the subject can hold (an exact double in
    the oracle's 80-bit register) is compared as the exact 80-bit pair, NaNs by class. A
    value the oracle holds with more precision than a double is compared as the nearest
    double only, and `fp_inexact` lets the caller say the case was not verified exactly.
    Everything is skipped for a side that did not report it. Depth is compared first
    because a helper that leaves one extra or one missing stack slot corrupts every
    caller's x87 state even when the integer result is right.
    """
    out: list[Divergence] = []
    if oracle.fp_control is not None and subject.fp_control is not None:
        if oracle.fp_control != subject.fp_control:
            out.append(
                Divergence(
                    "x87-control",
                    f"subject={subject.fp_control:04X} oracle={oracle.fp_control:04X}",
                )
            )
    if (
        oracle.fp_status is not None
        and subject.fp_status is not None
        and oracle.fp_status_mask is not None
        and subject.fp_status_mask is not None
    ):
        mask = oracle.fp_status_mask & subject.fp_status_mask
        differing = (oracle.fp_status ^ subject.fp_status) & mask
        if differing:
            out.append(
                Divergence(
                    "x87-status",
                    f"subject={subject.fp_status:04X} oracle={oracle.fp_status:04X} "
                    f"mask={mask:04X}",
                )
            )
    if oracle.fp is None or subject.fp is None:
        return out
    if len(oracle.fp) != len(subject.fp):
        out.append(
            Divergence(
                "x87-depth", f"subject={len(subject.fp)} oracle={len(oracle.fp)} stack entries"
            )
        )
        return out
    exact = oracle.fp_ext is not None and subject.fp_ext is not None
    for position, (subject_bits, oracle_bits) in enumerate(zip(subject.fp, oracle.fp, strict=True)):
        if exact:
            assert oracle.fp_ext is not None and subject.fp_ext is not None
            wide = oracle.fp_ext[position]
            if extended_fits_double(*wide):
                if not extended_equal(subject.fp_ext[position], wide):
                    out.append(
                        Divergence(
                            "x87-extended",
                            f"st({position}) subject={subject.fp_ext[position][0]:016X}:"
                            f"{subject.fp_ext[position][1]:04X} "
                            f"oracle={wide[0]:016X}:{wide[1]:04X}",
                        )
                    )
                continue
        if subject_bits == oracle_bits:
            continue
        both_nan = all(
            (bits >> 52) & 0x7FF == 0x7FF and bits & ((1 << 52) - 1)
            for bits in (subject_bits, oracle_bits)
        )
        if not both_nan:
            out.append(
                Divergence(
                    "x87-value",
                    f"st({position}) subject={subject_bits:016X} oracle={oracle_bits:016X}",
                )
            )
    return out


def compare_writes(oracle: ExecResult, subject: ExecResult) -> list[Divergence]:
    """Compare the two write-sets, splitting the difference three ways.

    The three-way split is what makes the diagnosis actionable. A MISSING write and a
    SPURIOUS write at the same time is the signature of a right value at a wrong
    address; a missing write alone is a dropped store; a value mismatch at a shared
    address is a miscomputation. Collapsing these into one count throws that away.
    """
    only_subject = sorted(a for a in subject.writes if a not in oracle.writes)
    only_oracle = sorted(a for a in oracle.writes if a not in subject.writes)
    conflicting = sorted(
        a for a in subject.writes if a in oracle.writes and subject.writes[a] != oracle.writes[a]
    )

    out: list[Divergence] = []
    if only_oracle:
        out.append(
            Divergence(
                "memory-missing",
                f"{len(only_oracle)} byte(s) the oracle wrote and the subject did not, at "
                + _examples(only_oracle),
            )
        )
    if only_subject:
        out.append(
            Divergence(
                "memory-spurious",
                f"{len(only_subject)} byte(s) the subject wrote and the oracle did not, at "
                + _examples(only_subject),
            )
        )
    if conflicting:
        detail = ", ".join(
            f"{a:08X} subject={subject.writes[a]:02X} oracle={oracle.writes[a]:02X}"
            for a in conflicting[:MEMORY_EXAMPLES]
        )
        extra = len(conflicting) - MEMORY_EXAMPLES
        more = "" if extra <= 0 else f" (+{extra} more)"
        out.append(Divergence("memory-value", f"{len(conflicting)} byte(s) differ: {detail}{more}"))
    return out


def compare_stub_counts(oracle: ExecResult, subject: ExecResult) -> list[Divergence]:
    """Did both sides reach the same stubbed callees the same number of times?

    Only meaningful for a stubbed function, and only as a path check: the two sides are
    supposed to execute the same calls in the same order, so a different count means they
    took different routes through the caller. That can be true even when the final
    registers coincide -- a caller that skips a call whose return value it then
    overwrites would otherwise look like agreement.
    """
    if oracle.reach.stub_applied == subject.reach.stub_applied:
        return []
    return [
        Divergence(
            "stub-count",
            f"the subject reached {subject.reach.stub_applied} stubbed callee(s) and the "
            f"oracle {oracle.reach.stub_applied} -- the two sides took different paths "
            "through the caller",
        )
    ]


def compare_passthrough_counts(oracle: ExecResult, subject: ExecResult) -> list[Divergence]:
    if oracle.reach.passthrough_applied == subject.reach.passthrough_applied:
        return []
    return [
        Divergence(
            "passthrough-count",
            "verified stack-probe execution counts differ: "
            f"subject={subject.reach.passthrough_applied} "
            f"oracle={oracle.reach.passthrough_applied}",
        )
    ]


def _fault_parity_block(common: dict, oracle: ExecResult, subject: ExecResult) -> CaseResult | None:
    """T1576: a DISAGREE when two faults do not share class and guest address, else None."""
    ok, why = fault_parity(oracle.fault, oracle.fault_addr, subject.fault, subject.fault_addr)
    if ok:
        return None
    return CaseResult(
        **common, outcome=Outcome.DISAGREE, divergences=(Divergence("fault-parity", why),)
    )


def compare(
    case: Case,
    oracle: ExecResult,
    subject: ExecResult,
    *,
    flag_mask: int = COMPARED_FLAGS,
    stubbed: bool = False,
    body_insns: int = 0,
    delegation_ratio: float = 0.0,
    ignored_registers: frozenset[str] = frozenset(),
    ignored_write_ranges: tuple[tuple[int, int], ...] = (),
    require_fault_parity: bool = False,
) -> CaseResult:
    """Reach a verdict on one case. See the module docstring for the fault matrix.

    `ignored_registers` is the set a hand-written replacement declared as scratch. It
    narrows the register comparison. `ignored_write_ranges` is a list of half-open guest
    address ranges whose writes are dropped from BOTH write-sets before they are compared,
    which a replacement or explicit live-closure proof declares for returned dead stack
    frames. The fault matrix and the stub counts are judged in full.

    `stubbed` says this function's callees were replaced on both sides, which enables the
    call-count path check and is recorded on the result so the summary can report how
    much of its evidence rests on stubbed callees rather than real ones.

    `body_insns` and `delegation_ratio` come from the selection and are carried through so
    that the reachability figures can be computed per function without re-decoding.
    """
    common = {
        "seed": case.seed,
        "index": case.index,
        "va": case.va,
        "size": case.size,
        "fault_diagnostic": diagnose(oracle.fault_observation, subject.fault_observation)
        if oracle.faulted or subject.faulted
        else "",
        "oracle_fault": oracle.fault,
        "subject_fault": subject.fault,
        "stubbed": stubbed,
        "reach": oracle.reach,
        "body_insns": body_insns,
        "delegation_ratio": delegation_ratio,
        "subject_seh_ebp": subject.seh_ebp,
        "seh_ebp_drifted": _seh_ebp_drifted(subject),
    }

    raw_x87_divergences = compare_raw_x87(case, oracle, subject)

    if subject.fault in ABSENT_SUBJECT_FAULTS:
        return CaseResult(
            **common,
            outcome=Outcome.SKIPPED_UNSUPPORTED,
            note=(
                f"the subject has no implementation at this address ({subject.fault}); "
                "nothing was compared"
            ),
        )

    if oracle.fault in ORACLE_NONVERDICT_FAULTS:
        return CaseResult(
            **common,
            outcome=Outcome.ORACLE_FAULTED,
            note=(
                f"the oracle hit a harness limit ({oracle.fault}) rather than a guest "
                "trap, so no verdict is possible; the subject is not instruction-counted"
            ),
        )

    if oracle.faulted and subject.faulted and require_fault_parity:
        # T1576 guarded-jump guard. Opt-in only: the default path below is unchanged. A match
        # is still a non-verdict (never AGREES); a mismatch blocks the proof.
        blocked = _fault_parity_block(common, oracle, subject)
        if blocked is not None:
            return blocked
    if oracle.faulted and subject.faulted:
        return CaseResult(
            **common,
            outcome=Outcome.ORACLE_FAULTED,
            divergences=tuple(raw_x87_divergences),
            note=(
                f"both sides faulted ({oracle.fault} / {subject.fault}); the input was "
                "rejected by the original code too, so no verdict is possible"
            ),
        )

    if oracle.faulted and not subject.faulted:
        return CaseResult(
            **common,
            outcome=Outcome.DISAGREE,
            divergences=(
                Divergence(
                    "swallowed-fault",
                    f"the original code faulted ({oracle.fault}) but the subject returned "
                    "cleanly -- the lifted code continued past a trap the hardware took",
                ),
            ),
        )

    if subject.faulted:
        return CaseResult(
            **common,
            outcome=Outcome.SUBJECT_FAULTED,
            divergences=(
                Divergence(
                    "subject-fault",
                    f"the subject faulted ({subject.fault}) where the original executed cleanly",
                ),
            ),
        )

    if ignored_write_ranges:
        oracle = _without_writes(oracle, ignored_write_ranges)
        subject = _without_writes(subject, ignored_write_ranges)
    # The EFLAGS channel. A publish-capable subject reports WHICH bits its
    # model answered at the exit it returned through (`flags_mask`), and only
    # those bits are compared -- comparing a bit the model never claimed would
    # manufacture divergences out of a design decision, and silently comparing
    # nothing would hide the gaps. Every TRACKED flag that was NOT compared is
    # recorded as unmodelled so the summary can say so per flag.
    effective_flag_mask = flag_mask
    if subject.flags_mask is not None:
        effective_flag_mask &= subject.flags_mask
    flags_observed = oracle.flags is not None and subject.flags is not None
    if flags_observed:
        common["flags_observed"] = True
        common["flags_compared_mask"] = effective_flag_mask
        assert oracle.flags is not None and subject.flags is not None
        common["flags_disagree_mask"] = (oracle.flags ^ subject.flags) & effective_flag_mask
        common["flags_unmodeled_mask"] = TRACKED_FLAGS & ~effective_flag_mask
    divergences = (
        compare_registers(oracle, subject, ignored=ignored_registers)
        + (
            compare_scalar_fp(case, oracle, subject)
            if case.vector_mode == FP_SCALAR_MODE
            else compare_vector(case, oracle, subject)
        )
        + compare_flags(oracle, subject, mask=effective_flag_mask)
        + raw_x87_divergences
        + (compare_fp(oracle, subject) if case.x87_state is None else [])
        + compare_writes(oracle, subject)
        + (compare_stub_counts(oracle, subject) if stubbed else [])
        + compare_passthrough_counts(oracle, subject)
    )
    if divergences:
        return CaseResult(**common, outcome=Outcome.DISAGREE, divergences=tuple(divergences))
    if fp_inexact(oracle, subject):
        return CaseResult(
            **common,
            outcome=Outcome.SKIPPED_UNSUPPORTED,
            note=(
                "the oracle's exit x87 value has more precision than the subject's double "
                "stack can hold: registers, writes, control word and the narrowed value "
                "agree, but the 80-bit value was not verified exactly"
            ),
        )
    return CaseResult(
        **common,
        outcome=Outcome.AGREE,
        note=f"{len(oracle.writes)} guest byte(s) written, identically on both sides",
    )


def _without_writes(result: ExecResult, ranges: tuple[tuple[int, int], ...]) -> ExecResult:
    """`result` with every write inside any half-open `(low, high)` range removed."""
    kept = {
        address: value
        for address, value in result.writes.items()
        if not any(low <= address < high for low, high in ranges)
    }
    return replace(result, writes=kept)


def _seh_ebp_drifted(subject: ExecResult) -> bool:
    """Did the subject leave its second frame pointer disagreeing with its own `ebp`?

    NOT a divergence and deliberately not fed into any verdict. `g_seh_ebp` has no
    hardware counterpart, so there is nothing on the oracle side to compare it with, and
    manufacturing a comparison would turn a deliberate lifter design decision -- patch 10
    restores `g_ebp` and not this, because this is a handoff channel -- into a false
    DISAGREE. The driver seeds the two equal at entry, so a difference at exit is the
    function under test changing one and not the other, which is the only part of the
    staleness question a per-function harness can see at all.
    """
    if subject.seh_ebp is None or subject.regs is None:
        return False
    return subject.seh_ebp != subject.regs[REG_NAMES.index("ebp")]


def _examples(addresses: list[int]) -> str:
    shown = ", ".join(f"{a:08X}" for a in addresses[:MEMORY_EXAMPLES])
    if len(addresses) <= MEMORY_EXAMPLES:
        return shown
    return f"{shown} (+{len(addresses) - MEMORY_EXAMPLES} more)"
