# SPDX-License-Identifier: GPL-3.0-or-later
"""Decide which `call`-bearing functions can be tested, and how to stub their callees.

`call` is the harness's single largest instruction-based exclusion. The reason it was
excluded is sound: if the callee runs, the verdict is about the callee as much as about
the function under test. Ordinary callees use one synthetic behaviour on both sides.
One reviewed leaf stack-probe entry executes on both sides instead: removing its
allocation would break the caller frame before the caller itself could be measured.

WHAT THE STUB IS
----------------
`call_stub.h` is the single definition and this module mirrors it. An ordinary stub:

* returns `HARNESS_STUB_EAX` in eax,
* destroys ecx and edx with fixed constants (they are caller-saved scratch),
* preserves ebx, esi, edi and ebp (they are callee-saved),
* touches NO memory of its own, and
* pops its own return address plus `pop_bytes` of stdcall arguments.

The caller's own push of the return address still happens and still lands in guest
memory on both sides, because the hardware performs that store too.

WHAT MAKES THE TWO SIDES IDENTICAL
----------------------------------
Not good intentions: a closed, statically-computed table. A function is only admitted
if every call site is a direct near call to a known entry. Ordinary targets require
consistent `ret` pop amounts. The stack probe additionally requires an exact original
SHA-256 and bounds; the subject requires the reviewed lifted body and measured compiler
policy. Only the two measured original caller bodies qualify: their arithmetic flags
are overwritten before they are consumed, avoiding an unmodeled callee flag handoff.
Separate STUB and PASS tables are handed to the subject, and unknown targets
remain forbidden. The stack-probe passthrough is counted separately from synthetic
stubs and its original fingerprint is rechecked when each call is reached. Everything
that does not fit is skipped under its own counted reason:

* `indirect-call` -- the target VA is only known at run time, so the two sides could
  stub different callees, and the pop amount is not knowable at all. 509 of the 3,565
  call-bearing functions are in this class.
* `callee-convention-unknown` -- the target ends in a tail `jmp`, or does not decode
  cleanly, or has several `ret`s that disagree about how much they pop. Guessing here
  would desync esp by a few bytes and produce a confident DISAGREE about the caller
  that is really a harness defect.
* `out-of-body-branch` -- a conditional branch leaving the body. The lifter turns that
  into a tail call emitted as a plain C call, which the stub macro does NOT intercept,
  so the callee would run on the subject side while the oracle stubbed it.

WHAT A STUBBED VERDICT DOES AND DOES NOT MEAN
---------------------------------------------
It means: given a callee that returns a fixed value and touches nothing, the lifted
caller computes the same registers and writes the same bytes as the original caller.
That covers the caller's argument marshalling, its stack discipline around the call,
its use of the return value, and everything it does before and after.

It does not establish the ordinary callees: they never ran. The verified stack probe
executes its real instructions and lifted body, including page reads and frame changes.
Ordinary stubs do not exercise behaviour that depends on their real return or writes.
A branch on the return value only ever goes the way `HARNESS_STUB_EAX` sends it. A caller
reading a buffer its callee was supposed to fill sees it unfilled on both sides. And for a
function that does nothing but marshal arguments and forward the result, the verdict is
close to vacuous; `delegation_ratio` below measures that so it can be reported instead of
counted as coverage.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

import capstone

#: Mirrors call_stub.h. `tests/test_harness.py` parses that header and fails if these
#: drift, because a silent drift would make every stubbed verdict meaningless while
#: still reporting AGREE.
STUB_EAX = 0xA5B6C7D8
STUB_ECX = 0xC1C1C1C1
STUB_EDX = 0xD2D2D2D2

_RET_MNEMONICS = frozenset({"ret", "retn"})
_FAR_RET = frozenset({"retf", "lret"})
#: Longest chain of pure jmp thunks followed (T1574).
THUNK_MAX_DEPTH = 4

#: Conditional branches and `loop`. An unconditional `jmp` has its own skip reason
#: already, so it is deliberately not listed here.
_COND_BRANCH = re.compile(r"^(j(?!mp\b)[a-z]+|loop|loope|loopne|loopz|loopnz)$")

#: Instructions that only move arguments or transfer control, for `delegation_ratio`.
_PLUMBING = frozenset({"push", "pushl", "call", "calll", "ret", "retn", "leave", "nop"})


@dataclass(frozen=True)
class CallSite:
    """One `call` in the function under test, and the stub that replaces its callee."""

    site_va: int
    #: Length of the call instruction, so the oracle knows where to resume.
    insn_len: int
    target_va: int
    pop_bytes: int
    passthrough: bool = False

    @property
    def return_va(self) -> int:
        return self.site_va + self.insn_len


@dataclass(frozen=True)
class StubPlan:
    """Everything both sides need in order to stub this function's callees identically."""

    sites: tuple[CallSite, ...]
    #: Fraction of the body that is only argument plumbing and control transfer. A
    #: function near 1.0 delegates everything it does, so a stubbed verdict on it proves
    #: almost nothing and is reported rather than counted.
    delegation_ratio: float

    @property
    def table(self) -> tuple[tuple[int, int], ...]:
        """Deduplicated `(callee_va, pop_bytes)` pairs, ascending, for the wire."""
        pops: dict[int, int] = {}
        for site in self.sites:
            if not site.passthrough:
                pops[site.target_va] = site.pop_bytes
        return tuple(sorted(pops.items()))

    @property
    def passthrough_table(self) -> tuple[int, ...]:
        return tuple(sorted({site.target_va for site in self.sites if site.passthrough}))

    @property
    def sites_by_va(self) -> dict[int, CallSite]:
        return {site.site_va: site for site in self.sites}


def callee_pop_bytes(code: bytes, va: int, size: int, decoder: capstone.Cs) -> int | None:
    """How many argument bytes this callee pops, or None if that cannot be read off it.

    None is returned rather than a guess for every ambiguous shape. A guess that is
    wrong by four bytes desyncs esp and produces a DISAGREE that looks like a lifter
    defect in the CALLER, which is the most expensive kind of false positive this
    harness can emit.
    """
    if not code or len(code) != size:
        return None
    instructions = list(decoder.disasm(code, va))
    if not instructions:
        return None
    if sum(i.size for i in instructions) != size:
        # Embedded data or wrong bounds: the terminator cannot be trusted.
        return None

    pops: set[int] = set()
    for insn in instructions:
        mnemonic = insn.mnemonic.lower()
        if mnemonic in _FAR_RET:
            return None
        if mnemonic not in _RET_MNEMONICS:
            continue
        operand = insn.op_str.strip()
        if not operand:
            pops.add(0)
            continue
        try:
            pops.add(int(operand, 0))
        except ValueError:
            return None

    if len(pops) != 1:
        # No `ret` at all (tail call or fallthrough), or several that disagree.
        return None
    return pops.pop()


class CalleeConventions:
    """Caches `callee_pop_bytes` per callee VA across the whole selection pass.

    Direct call targets repeat heavily -- a few hot callees account for a large share of
    all call sites -- so without the cache the same callee is decoded thousands of times.
    """

    def __init__(
        self,
        sizes: dict[int, int],
        read_code: object,
        decoder: capstone.Cs | None = None,
    ) -> None:
        self._sizes = sizes
        reader = read_code
        assert callable(reader)
        self._read = reader
        self._decoder = decoder if decoder is not None else _decoder()
        self._cache: dict[int, int | None] = {}

    def stackprobe_proven(self, target_va: int) -> bool:
        from .stackprobe import STACKPROBE_SIZE, original_stackprobe_proven

        size = self._sizes.get(target_va)
        return size == STACKPROBE_SIZE and original_stackprobe_proven(
            target_va, self._read(target_va, size)
        )

    def pop_bytes(self, target_va: int) -> int | None:
        """Pop amount for `target_va`, or None when it is not a resolvable function."""
        if target_va in self._cache:
            return self._cache[target_va]
        size = self._sizes.get(target_va)
        if size is None:
            # Not a known function entry: a mid-function target, a thunk outside the
            # selection, or data. Not stubbable.
            self._cache[target_va] = None
            return None
        answer = callee_pop_bytes(self._read(target_va, size), target_va, size, self._decoder)
        if answer is None:
            answer = self._thunk_pop_bytes(target_va)
        self._cache[target_va] = answer
        return answer

    def thunk_target(self, va: int) -> int | None:
        """Target of a pure single direct near `jmp` function body, else None (T1574).

        Only one direct near `jmp` (E9 rel32 / EB rel8) as the whole function, landing on
        an exact known function entry (a self jmp is caught by the visited set) and never
        the stackprobe. A lone jmp leaves esp unchanged, so the thunk pops what its target
        pops.
        """
        from .stackprobe import STACKPROBE_VA

        size = self._sizes.get(va)
        if size is None or size > 5:
            return None
        code = self._read(va, size)
        if len(code) != size:
            return None
        instructions = list(self._decoder.disasm(code, va))
        if len(instructions) != 1 or instructions[0].size != size:
            return None
        insn = instructions[0]
        if insn.mnemonic.lower() != "jmp":
            return None
        target = _branch_target(insn)
        if target is None or target == STACKPROBE_VA:
            return None
        if target not in self._sizes:
            return None
        return target

    def _thunk_pop_bytes(self, va: int) -> int | None:
        """Pop amount through a chain of pure thunks, bounded and cycle-safe."""
        seen = {va}
        current = va
        for _ in range(THUNK_MAX_DEPTH):
            target = self.thunk_target(current)
            if target is None or target in seen:
                return None
            seen.add(target)
            size = self._sizes[target]
            answer = callee_pop_bytes(self._read(target, size), target, size, self._decoder)
            if answer is not None:
                return answer
            current = target
        return None


def _decoder() -> capstone.Cs:
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    return decoder


def _direct_target(insn: capstone.CsInsn) -> int | None:
    """The absolute target of a direct near call, or None if it is indirect or far.

    Operand text is deliberately not parsed: capstone's operand model distinguishes an
    immediate from a register or a memory reference without any string guessing.
    """
    if insn.mnemonic.lower() not in {"call", "calll"}:
        return None
    try:
        operands = insn.operands
    except capstone.CsError:  # pragma: no cover - detail is always on here
        return None
    if len(operands) != 1:
        return None
    operand = operands[0]
    if operand.type != capstone.x86.X86_OP_IMM:
        return None
    return int(operand.imm) & 0xFFFFFFFF


def plan_calls(
    va: int,
    size: int,
    instructions: list[capstone.CsInsn],
    conventions: CalleeConventions,
    *,
    allow_tail_branches: bool = False,
) -> StubPlan | str:
    """A `StubPlan` for this body, or the skip reason naming why it cannot be stubbed."""
    sites: list[CallSite] = []
    plumbing = 0
    body_end = va + size

    for insn in instructions:
        mnemonic = insn.mnemonic.lower()
        if mnemonic in _PLUMBING or mnemonic.startswith("push"):
            plumbing += 1

        if _COND_BRANCH.match(mnemonic):
            target = _branch_target(insn)
            if allow_tail_branches and target is not None:
                # Replacement judging: the oracle runs the original bytes through the
                # tail target to its `ret`, and the replacement covers the whole path.
                continue
            if target is None or not (va <= target < body_end):
                # The lifter emits an out-of-body branch as a tail call -- a plain C
                # call that the stub macro cannot intercept -- so the callee would run
                # on the subject side while the oracle stubbed it.
                return "out-of-body-branch"
            continue

        if not mnemonic.startswith("call"):
            continue

        if mnemonic in {"lcall", "callf"}:
            return "indirect-call"
        target = _direct_target(insn)
        if target is None:
            return "indirect-call"
        from .stackprobe import STACKPROBE_VA, original_stackprobe_caller_proven

        passthrough = target == STACKPROBE_VA
        if passthrough and not conventions.stackprobe_proven(target):
            return "callee-stackprobe-unproven"
        if passthrough and not original_stackprobe_caller_proven(
            va, b"".join(bytes(instruction.bytes) for instruction in instructions)
        ):
            return "stackprobe-caller-unproven"
        pop = conventions.pop_bytes(target)
        if pop is None:
            return "callee-convention-unknown"
        sites.append(
            CallSite(
                site_va=insn.address,
                insn_len=insn.size,
                target_va=target,
                pop_bytes=pop,
                passthrough=passthrough,
            )
        )

    ratio = plumbing / len(instructions) if instructions else 0.0
    return StubPlan(sites=tuple(sites), delegation_ratio=ratio)


def _branch_target(insn: capstone.CsInsn) -> int | None:
    try:
        operands = insn.operands
    except capstone.CsError:  # pragma: no cover - detail is always on here
        return None
    if len(operands) != 1 or operands[0].type != capstone.x86.X86_OP_IMM:
        return None
    return int(operands[0].imm) & 0xFFFFFFFF
