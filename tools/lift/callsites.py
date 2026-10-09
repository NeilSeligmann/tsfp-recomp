# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure each kernel ordinal's stack-argument count from the lifted C.

WHY THIS EXISTS. When lifted code calls a kernel import, OUR handler is the
callee, so we must perform the callee cleanup the real `ret N` would have done:
pop the return address plus N bytes of stack arguments. Pop the wrong number and
nothing crashes -- `esp` simply desyncs, the caller reads every later stack slot
off by one, and the run goes on producing a plausible, wrong trace. That is the
specific failure this project has been bitten by, so the count has to come from
somewhere better than memory.

WHERE IT COMES FROM. The user's own binary. The lifter brackets every indirect
call site it emits::

    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0x37FE1D);
    ... more argument pushes ...
    { uint32_t _icall_target = MEM32(0x475894); PUSH32(esp, 0x0037FEF4u);
      RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x0037FEEEu); }
    }

`MEM32(0x475894)` is a slot in the XBE's kernel thunk table, which identifies the
ordinal. Every ``PUSH32`` between the bracket and the call is an argument except
the last, which is the return address. So the stack-argument count is directly
readable, per call site, from the guest's own code.

WHY THE ESTIMATOR IS THE MINIMUM OVER CALL SITES, NOT UNANIMITY. Sites disagree,
and the disagreement is not noise -- it has one cause and one direction. The
lifter opens its bracket at the start of the x86 basic block, not at the start of
argument setup, so a function's own callee-saved register saves land inside it::

    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, esi);                       <- `push esi`, a register SAVE
    { ... MEM32(0x475848) ... }             <- KeRaiseIrqlToDpcLevel, 0 arguments

Nothing makes a site count FEWER pushes than there are arguments, so the bias is
one-sided and the minimum over sites is the estimator the structure of the error
justifies. Demanding unanimity instead would reject correct answers:
``MmAllocateContiguousMemory`` votes {1: 1, 2: 1}, and 1 is right.

WHY IT IS TRUSTWORTHY, AND WHERE IT IS NOT. The method is validated against
ordinals whose arity is known independently of it -- our own handlers in
``src/xbox/kernel_memory.c`` each read a fixed number of arguments, which is a
number nobody derived from call sites. On this image ``validate()`` checks nine of
them, spanning one to five arguments and including three where the sites
disagreed, and the minimum estimator reproduces all nine. A measurement that
cannot reproduce a known answer must not be used for the unknown ones, which is
why ``validate`` reports how many it actually checked: zero checked is not a pass.

It measures STACK arguments, which is exactly what `esp` cleanup needs and is
convention-agnostic -- a `__fastcall` export taking its only argument in `ecx`
measures zero stack arguments and must pop only the return address, which is
correct.

KNOWN LIMIT, AND IT IS THE ONE THAT MATTERS. An argument stored with
``MEM32(esp + N) = ...`` after a single ``esp -= K``, rather than pushed, is not
counted, and because that error is in the *opposite* direction the minimum would
be wrong rather than conservative. The nine validated ordinals show no such case,
but they do not rule it out, so an ordinal measured here is still overridden by a
hand-sourced entry where one exists, and `sites` and `unanimous` travel with
every result so a surprising arity can be weighed rather than merely trusted.

THREE WAYS A SITE REACHES AN ORDINAL, AND THE TWO THAT WERE INVISIBLE. The
original version of this module keyed on a thunk slot appearing as the target
expression of an indirect call, which is only the first of the three shapes the
compiler actually emits:

1. ``call dword ptr [slot]`` -- the slot read *is* the call target. Seen.
2. ``mov reg, dword ptr [slot]`` and later ``call reg`` -- the slot read and the
   call are different instructions. The first version counted the read as a plain
   DATA reference and the call as an untyped vtable call, so the site vanished and
   the read pushed the ordinal *towards* being classified a variable.
3. ``jmp dword ptr [slot]`` -- an import jump stub. It is a one-instruction
   function, not a call site; its callers are the sites. The first version saw the
   stub's slot read, attributed no call to it, and so classified the ordinal as a
   DATA export no matter how many functions called the stub.

Shape 3 is the expensive one. ``ExQueryNonVolatileSetting`` (ordinal 24) is
reached only through a stub, so all twelve of its sites were invisible and the
ordinal was reported as a kernel *variable* -- which would have had its thunk
patched with the address of a readable object instead of a dispatch stub.

HOW LONG A SLOT->REGISTER BINDING LIVES. Shape 2 needs the register tracked from
its load to its use, and the real code crosses basic blocks to get there:
ordinal 47's load feeds two calls, the second behind a conditional branch, and
ordinal 202's feeds two, the second past a join. So the binding is followed
forward by FALL-THROUGH and dies at the first of: a write to the register; an
intervening call, if the register is one a callee may destroy; an unconditional
transfer of control, which ends fall-through; the end of the function; or a
distance limit. Conditional branches do not end it, which is what makes the
hidden sites reachable. The cost of that choice is stated in
`track_register_calls`: a join can admit a path on which the register holds
something else, so a register-attributed site is a site that CAN reach the
ordinal rather than one that always does.

WHY A STUB-ATTRIBUTED SITE COUNTS AS A CALL BUT CASTS NO ARITY VOTE. The lifter
renders a call to a stub as a DIRECT call, and it opens an ``_icall_esp`` bracket
only for indirect ones. With no bracket there is no trustworthy push window, and
inventing one would put an unvalidated number into a table whose whole claim is
that it is validated. So a stub caller proves the ordinal is a FUNCTION and is
counted in the ranking, but contributes nothing to `counts`, and such an ordinal
reaches the host through the hand-written table instead.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from collections.abc import Iterable
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path

import capstone
from capstone import x86 as cs_x86

from tools.arity_oracle import DEFAULT_DEF_PATH, Convention, load_rows
from tools.codediff.normalise import MAX_INSN_BYTES, NormalisedText, normalise_text
from tools.kernel_ordinals import KERNEL_ORDINALS
from tools.xbe.parser import Xbe, parse_xbe

#: Opens a lifted call site and captures the pre-argument esp.
_SITE_OPEN = re.compile(r"uint32_t\s+_icall_esp\s*=\s*g_esp\s*;")

#: One pushed 32-bit value.
_PUSH = re.compile(r"PUSH32\(\s*esp\s*,")

#: The indirect call itself, with the slot it reads through.
_ICALL = re.compile(r"MEM32\(\s*(0x[0-9A-Fa-f]+)\s*\)[^\n]*?RECOMP_ICALL(?:_SAFE)?(?:_AT)?\s*\(")

#: An indirect call whose target was not loaded from a constant address.
_ICALL_ANY = re.compile(r"RECOMP_ICALL(?:_SAFE)?(?:_AT)?\s*\(")

#: Any 32-bit read of a constant guest address. Used to tell a thunk slot the
#: guest CALLS from one it merely READS.
_MEM32 = re.compile(r"MEM32\(\s*(0x[0-9A-Fa-f]+)\s*\)")

#: The eight 32-bit general-purpose registers, as the lifter names them. The lifted
#: C never assigns to a sub-register -- every write is to the full 32-bit name --
#: which is what makes textual write-tracking sound here.
_REGISTERS = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp")

_REG_ALTERNATION = "|".join(_REGISTERS)

#: `edi = MEM32(0x4758C0);` -- a thunk slot loaded into a register, which is the
#: first half of a shape-2 site.
_REG_LOAD = re.compile(rf"\b({_REG_ALTERNATION})\s*=\s*MEM32\(\s*(0x[0-9A-Fa-f]+)\s*\)\s*;")

#: Any write to a register, in every form the lifter emits: `reg =`, `reg <op>=`,
#: `reg++`, `reg--`. Matched mid-line as well as at the start, because the lifter
#: puts the string-instruction pointer bumps after a semicolon on the same line.
_REG_WRITE = re.compile(rf"\b({_REG_ALTERNATION})\s*(?:\+\+|--|(?:[-+*/|&^]|<<|>>)?=(?!=))")

#: The target expression of an indirect call, when it is a bare identifier. A
#: `MEM32(...)` target cannot match, so this selects exactly the register-target
#: form -- the second half of a shape-2 site.
_ICALL_TARGET_REG = re.compile(r"_icall_target\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;")

#: An indirect tail jump. `RECOMP_ITAIL(MEM32(0x4757EC))` in a function that does
#: nothing else is an import jump STUB; the same thing anywhere else is a tail-call
#: site, and `RECOMP_ITAIL(edi)` is a tail call through a tracked register.
_ITAIL = re.compile(r"RECOMP_ITAIL\(\s*(.+?)\s*\)\s*;")

#: A direct call to a known function, which is how a call to an import jump stub is
#: rendered: `RECOMP_ABI_CALL(0x0038486Au, sub_0038486A);`.
_ABI_CALL = re.compile(r"RECOMP_ABI_CALL\(\s*0x[0-9A-Fa-f]+u?\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")

#: The definition line of a lifted function, which bounds register tracking.
_FUNC_DEF = re.compile(r"^\s*(?:static\s+)?void\s+(sub_[0-9A-Fa-f]+)\s*\(")

#: An unconditional transfer of control, which ends fall-through. A conditional
#: `if (...) goto loc_X;` deliberately does NOT match: the binding has to survive
#: one, or ordinal 47's second site and ordinal 202's both disappear.
_UNCONDITIONAL_FLOW = re.compile(r"^\s*(?:goto\s+\w+|return\b[^;]*)\s*;\s*(?:/\*.*)?$")

#: Registers a `__stdcall`/`__cdecl` callee is free to destroy, so a binding held in
#: one of them does not survive an intervening call. The two shapes that were
#: invisible both survive one precisely because they use callee-saved registers:
#: ordinal 47's binding is in `edi` and ordinal 202's in `ebx`.
VOLATILE_REGISTERS = frozenset({"eax", "ecx", "edx"})

#: The call instruction reads the slot itself: `call dword ptr [slot]`.
SITE_BRACKET = "bracket"

#: The slot was loaded into a register and the call dispatches through it.
SITE_REGISTER = "register"

#: A direct call to a one-instruction `jmp dword ptr [slot]` import stub.
SITE_STUB = "stub"

#: Every shape, in the order a report should list them.
SITE_KINDS = (SITE_BRACKET, SITE_REGISTER, SITE_STUB)

#: Independently-known stack-argument counts, used to validate the measurement
#: rather than to supply answers. Each is the count the matching handler in
#: `src/xbox/kernel_memory.c` reads via `frame_args(frame, N, ...)`.
KNOWN_STACK_ARGS: dict[int, int] = {
    165: 1,  # MmAllocateContiguousMemory
    166: 5,  # MmAllocateContiguousMemoryEx
    169: 2,  # MmCreateKernelStack
    170: 2,  # MmDeleteKernelStack
    171: 1,  # MmFreeContiguousMemory
    173: 1,  # MmGetPhysicalAddress
    # MmLockUnlockBufferPages (the measured pass reads 1: pushes sit above an
    # intervening call, T8a)
    175: 3,
    176: 2,  # MmLockUnlockPhysicalPage
    178: 3,  # MmPersistContiguousMemory
    179: 1,  # MmQueryAddressProtect
    180: 1,  # MmQueryAllocationSize
    181: 1,  # MmQueryStatistics (T1146)
    182: 3,  # MmSetAddressProtect
    184: 5,  # NtAllocateVirtualMemory
    199: 3,  # NtFreeVirtualMemory
    217: 2,  # NtQueryVirtualMemory
}

#: Ordinals whose MEASURED value is known to diverge from the true arity for an
#: understood mechanical reason. KNOWN_STACK_ARGS keeps the TRUE count (what the
#: handler reads, pinned against kernel_memory.c by test_lift.py), while this
#: overlay records what the push-counting pass reads ON THIS IMAGE and why.
#: validate() requires the measurement to reproduce the DOCUMENTED divergent
#: reading exactly: any other value -- including suddenly reading the true
#: arity -- means the recorded mechanism no longer holds and must be re-derived
#: by hand, not waved through. The divergent row itself never reaches the host:
#: it is non-unanimous, so kernel_thunk.c's quorum gate refuses it and the hand
#: ABI row answers (T8a).
MEASURED_DIVERGENCES: dict[int, int] = {
    # T8a: at XNET 0x0043A765 and 0x0043AAE3 two of the three pushes sit above
    # an intervening `call 0x439cff` that pops nothing, and the register-
    # dispatched DSOUND site 0x0040F638 is invisible to the bracket, so the
    # minimum over sites reads 1 where MmLockUnlockBufferPages takes 3.
    175: 1,
}


@dataclass(frozen=True)
class OrdinalArity:
    """What every call site in the image says about one ordinal."""

    ordinal: int
    #: Stack-argument count -> how many call sites produced it.
    counts: dict[int, int]

    @property
    def sites(self) -> int:
        return sum(self.counts.values())

    @property
    def unanimous(self) -> bool:
        """True when every call site counted the same number of pushes."""
        return len(self.counts) == 1

    @property
    def estimate(self) -> int:
        """Stack-argument count: the minimum over call sites.

        See the module docstring for why the minimum rather than the majority or
        a unanimous vote -- callee-saved register saves inflate a site's count and
        nothing deflates it.
        """
        return min(self.counts)


@lru_cache(maxsize=1)
def _export_conventions() -> dict[int, Convention]:
    """Authoritative function/DATA kinds from the committed nxdk .def.

    Slot reads can be function pointers (AvSetDisplayMode/IoInvalidDeviceRequest
    are the T79 examples). Read-only use is not evidence of a DATA export.
    """
    source = Path(__file__).resolve().parents[2] / DEFAULT_DEF_PATH
    return {row.ordinal: row.convention for row in load_rows(source)}


def _known_data_ordinals(observed: Iterable[int]) -> tuple[int, ...]:
    conventions = _export_conventions()
    return tuple(
        sorted(ordinal for ordinal in observed if conventions.get(ordinal) is Convention.DATA)
    )


@dataclass(frozen=True)
class SlotUse:
    """How the guest uses one kernel thunk slot.

    The distinction matters because a kernel export is either a function or a
    variable, and the thunk slot is patched differently for each. A FUNCTION slot
    must hold a synthetic VA the lifted code will dispatch on; a DATA slot must
    hold the address of a real, readable, guest-visible object, because the guest
    dereferences it. Get that backwards and the guest faults -- which is exactly
    how `LaunchDataPage` (ordinal 164) was found: every one of its 14 references
    is `eax = MEM32(slot)` and not one is a call.

    `calls` counts all three site shapes, not just the one whose call instruction
    reads the slot directly, because a stub-only ordinal has no site of that shape
    at all and was being called a variable on the strength of the stub's own read.
    """

    ordinal: int
    #: Occurrences where the slot's value reaches an indirect-call target, by any of
    #: the three shapes.
    calls: int
    #: Occurrences where the slot is read as a plain value and the value is not
    #: subsequently called.
    reads: int
    #: Site shape -> count, so a classification can be traced to the evidence that
    #: produced it. Keys are `SITE_BRACKET`, `SITE_REGISTER`, `SITE_STUB`.
    kinds: dict[str, int] = field(default_factory=dict)

    @property
    def is_data_export(self) -> bool:
        """An observed read of an export explicitly marked DATA by the oracle."""
        return self.reads > 0 and _export_conventions().get(self.ordinal) is Convention.DATA


@dataclass(frozen=True)
class ArityReport:
    """The measurement over a whole lifted image."""

    #: Ordinal -> what its call sites said.
    ordinals: dict[int, OrdinalArity]
    #: Call sites whose target was not a constant thunk slot. Informational: these
    #: are ordinary vtable and function-pointer calls, not kernel imports.
    non_thunk_sites: int
    #: Ordinal -> how its thunk slot is used.
    slot_uses: dict[int, SlotUse] = field(default_factory=dict)

    def data_export_ordinals(self) -> tuple[int, ...]:
        """Observed exports explicitly marked DATA in the committed .def."""
        return tuple(
            sorted(ordinal for ordinal, use in self.slot_uses.items() if use.is_data_export)
        )

    def measured(self) -> dict[int, int]:
        """Every ordinal reached by a call site, with its estimated arity."""
        return {ordinal: arity.estimate for ordinal, arity in self.ordinals.items()}

    def disputed(self) -> dict[int, dict[int, int]]:
        """Ordinals whose call sites disagreed, with the vote.

        Not an error list: the disagreement is explained and one-sided (see the
        module docstring). It is reported so an unexpected arity can be examined
        rather than taken on trust.
        """
        return {
            ordinal: dict(arity.counts)
            for ordinal, arity in self.ordinals.items()
            if not arity.unanimous
        }

    def validate(self, known: dict[int, int] | None = None) -> tuple[dict[int, int], int]:
        """Check the measurement against independently-known arities.

        Returns ``(mismatches, checked)`` where `mismatches` maps an ordinal to
        the measured value that disagreed with the known one. An empty mapping
        with a nonzero `checked` is the evidence that the method works on this
        image; `checked` of zero means it was never put to the test and the
        result must not be trusted.

        For the default reference, an ordinal in MEASURED_DIVERGENCES is checked
        against its DOCUMENTED divergent reading instead of the true arity: the
        true arity is exactly what the pass is known not to reproduce there, and
        any OTHER reading (the true one included) says the recorded mechanism
        changed. A caller-supplied `known` is compared verbatim.
        """
        reference = KNOWN_STACK_ARGS if known is None else known
        measured = self.measured()
        mismatches: dict[int, int] = {}
        checked = 0
        for ordinal, expected in reference.items():
            if ordinal not in measured:
                continue
            checked += 1
            if known is None:
                expected = MEASURED_DIVERGENCES.get(ordinal, expected)
            if measured[ordinal] != expected:
                mismatches[ordinal] = measured[ordinal]
        return mismatches, checked


def thunk_slot_ordinals(xbe_path: Path) -> dict[int, int]:
    """Guest address of each kernel thunk slot -> the ordinal it imports."""
    xbe = parse_xbe(xbe_path.read_bytes())
    return {
        xbe.kernel_thunk_addr + 4 * index: ordinal
        for index, ordinal in enumerate(xbe.kernel_import_ordinals)
    }


def stub_functions(chunks: Iterable[Path], slots: dict[int, int]) -> dict[str, int]:
    """Lifted function name -> ordinal, for every import jump stub.

    A stub is a function whose whole body is one indirect tail jump through a thunk
    slot: the lifted form of `jmp dword ptr [slot]`. It is NOT a call site. It is a
    one-instruction function, and the sites are the functions that call it, which is
    why this has to be resolved before anything is counted.

    The body is required to contain nothing else -- no pushes, no other call. A real
    function that happens to tail-jump into the kernel is a tail-call SITE, and
    mistaking one for a stub would wrongly promote all of *its* callers into kernel
    call sites.
    """
    found: dict[str, int] = {}
    name: str | None = None
    tail_ordinal: int | None = None
    disqualified = False

    def finish() -> None:
        if name is not None and tail_ordinal is not None and not disqualified:
            found[name] = tail_ordinal

    for chunk in chunks:
        for line in chunk.read_text(encoding="utf-8", errors="replace").splitlines():
            definition = _FUNC_DEF.match(line)
            if definition:
                finish()
                name, tail_ordinal, disqualified = definition.group(1), None, False
                continue
            if name is None:
                continue
            if _PUSH.search(line) or _ABI_CALL.search(line) or _ICALL_ANY.search(line):
                disqualified = True
            tail = _ITAIL.search(line)
            if tail is None:
                continue
            slot = _MEM32.fullmatch(tail.group(1))
            ordinal = slots.get(int(slot.group(1), 16)) if slot else None
            if ordinal is None or tail_ordinal is not None:
                # A tail jump through something other than a slot, or a second tail
                # jump: either way this is not a one-instruction import stub.
                disqualified = True
            else:
                tail_ordinal = ordinal
        finish()
        name, tail_ordinal, disqualified = None, None, False

    return found


@dataclass
class _Binding:
    """A live slot -> register binding inside one lifted function."""

    ordinal: int
    #: True once a call has dispatched through the register, which is what turns the
    #: load from a plain data READ into part of a call site.
    called: bool = False


class _LiftedScan:
    """The lifted-C measurement, accumulated one line at a time.

    Explicit state rather than closures over a loop variable: the lifted image is
    millions of lines, so anything allocated per line shows up in the runtime.
    """

    def __init__(self, slots: dict[int, int], stubs: dict[str, int]) -> None:
        self.slots = slots
        self.stubs = stubs
        self.tally: dict[int, dict[int, int]] = defaultdict(lambda: defaultdict(int))
        self.calls: dict[int, int] = defaultdict(int)
        self.kinds: dict[int, dict[str, int]] = defaultdict(lambda: defaultdict(int))
        self.reads: dict[int, int] = defaultdict(int)
        self.non_thunk = 0
        self.pushes: int | None = None
        self.bindings: dict[str, _Binding] = {}
        self.function: str | None = None

    def _retire(self, register: str) -> None:
        """Drop a binding, booking the load as a plain READ if nothing called it."""
        binding = self.bindings.pop(register, None)
        if binding is not None and not binding.called:
            self.reads[binding.ordinal] += 1

    def _retire_all(self) -> None:
        for register in list(self.bindings):
            self._retire(register)

    def _record(self, ordinal: int, kind: str) -> None:
        self.calls[ordinal] += 1
        self.kinds[ordinal][kind] += 1

    def _vote(self, ordinal: int) -> None:
        """Cast this site's stack-argument vote, if it is inside a push window."""
        if self.pushes is not None and self.pushes >= 1:
            # Every push but the last is an argument; the last is the return address
            # the macro requires the caller to have pushed.
            self.tally[ordinal][self.pushes - 1] += 1

    def finish(self) -> None:
        """End the current translation unit. A binding cannot outlive one."""
        self._retire_all()
        self.pushes = None
        self.function = None

    def line(self, text: str) -> None:
        definition = _FUNC_DEF.match(text)
        if definition:
            self._retire_all()
            self.function = definition.group(1)
            self.pushes = None
            return

        if "_icall_esp" in text and _SITE_OPEN.search(text):
            self.pushes = 0
        if self.pushes is not None and "PUSH32" in text:
            self.pushes += len(_PUSH.findall(text))

        has_mem32 = "MEM32(0x" in text
        # Thunk-slot occurrences on this line, minus the ones a call or a register
        # load consumes. Whatever is left over is a plain data read.
        unconsumed: list[int] = (
            [
                address
                for address in (int(m.group(1), 16) for m in _MEM32.finditer(text))
                if address in self.slots
            ]
            if has_mem32
            else []
        )

        icall = _ICALL_ANY.search(text) if "RECOMP_ICALL" in text else None
        # Shape 1: the indirect call reads the slot itself.
        bracket = _ICALL.search(text) if icall and has_mem32 else None
        # Shape 2: the indirect call dispatches through a register we have bound.
        target = _ICALL_TARGET_REG.search(text) if icall else None
        bound = self.bindings.get(target.group(1)) if target else None
        tail = _ITAIL.search(text) if "RECOMP_ITAIL" in text else None
        tail_expr = tail.group(1) if tail else None
        tail_slot = _MEM32.fullmatch(tail_expr) if tail_expr else None
        abi = "RECOMP_ABI_CALL" in text

        if bracket is not None:
            address = int(bracket.group(1), 16)
            _discard(unconsumed, address)
            ordinal = self.slots.get(address)
            if ordinal is None:
                self.non_thunk += 1
            else:
                self._record(ordinal, SITE_BRACKET)
                self._vote(ordinal)
        elif bound is not None:
            self._record(bound.ordinal, SITE_REGISTER)
            bound.called = True
            self._vote(bound.ordinal)
        elif icall is not None and self.pushes is not None:
            # An indirect call inside a bracket whose target is neither a slot nor a
            # register holding one: an ordinary vtable or function-pointer call,
            # never a kernel import.
            self.non_thunk += 1

        # A tail jump pushes no return address, so it is a site but casts no vote.
        if tail_slot is not None:
            address = int(tail_slot.group(1), 16)
            _discard(unconsumed, address)
            ordinal = self.slots.get(address)
            if ordinal is not None and self.function not in self.stubs:
                # A real function tail-jumping into the kernel is a site. Inside a
                # stub the same jump IS the stub: neither a site nor a data read, and
                # booking it as a read is what classified ordinal 24 as a variable.
                self._record(ordinal, SITE_BRACKET)
        elif tail_expr is not None:
            binding = self.bindings.get(tail_expr)
            if binding is not None:
                self._record(binding.ordinal, SITE_REGISTER)
                binding.called = True

        # Shape 3: a direct call to an import jump stub. Each caller is a site; the
        # stub's own slot read belongs to nobody.
        if abi:
            for callee in _ABI_CALL.findall(text):
                ordinal = self.stubs.get(callee)
                if ordinal is not None:
                    self._record(ordinal, SITE_STUB)

        if icall is not None or tail is not None or abi:
            self.pushes = None
            for volatile in VOLATILE_REGISTERS:
                self._retire(volatile)

        # Register writes before new loads, so `edi = MEM32(slot)` retires whatever
        # edi held and then binds the new slot rather than the other way round.
        if self.bindings:
            for register in _REG_WRITE.findall(text):
                self._retire(register)

        if has_mem32:
            for register, address in _REG_LOAD.findall(text):
                ordinal = self.slots.get(int(address, 16))
                if ordinal is None:
                    continue
                # The load's own slot read is not a data read yet: whether it is one
                # depends on what happens to the register, settled when the binding
                # retires.
                _discard(unconsumed, int(address, 16))
                self.bindings[register] = _Binding(ordinal)

        if self.bindings and _UNCONDITIONAL_FLOW.match(text):
            self._retire_all()

        for address in unconsumed:
            self.reads[self.slots[address]] += 1


def _discard(values: list[int], value: int) -> None:
    """Remove one occurrence of `value`, if present."""
    if value in values:
        values.remove(value)


def measure(gen_dir: Path, slots: dict[int, int]) -> ArityReport:
    """Scan every lifted chunk and tally stack arguments per ordinal.

    Attributes all three site shapes (see the module docstring). Only the two the
    lifter brackets -- the direct slot read and the register-indirect call --
    contribute a stack-argument vote; a stub caller is counted as a call so the
    ordinal is classified a FUNCTION, but casts no vote, because the lifter opens no
    `_icall_esp` bracket around a direct call and there is therefore no push window
    to measure.
    """
    chunks = sorted(gen_dir.glob("recomp_[0-9]*.c"))
    stubs = stub_functions(chunks, slots)
    scan = _LiftedScan(slots, stubs)
    for chunk in chunks:
        for line in chunk.read_text(encoding="utf-8", errors="replace").splitlines():
            scan.line(line)
        scan.finish()

    tally, calls, kinds, reads, non_thunk = (
        scan.tally,
        scan.calls,
        scan.kinds,
        scan.reads,
        scan.non_thunk,
    )
    return ArityReport(
        ordinals={
            ordinal: OrdinalArity(ordinal=ordinal, counts=dict(counts))
            for ordinal, counts in sorted(tally.items())
        },
        non_thunk_sites=non_thunk,
        slot_uses={
            ordinal: SlotUse(
                ordinal=ordinal,
                calls=calls.get(ordinal, 0),
                reads=reads.get(ordinal, 0),
                kinds=dict(kinds.get(ordinal, {})),
            )
            for ordinal in sorted(set(calls) | set(reads))
        },
    )


BANNER = """\
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GENERATED by tools/lift/callsites.py. Do not edit: regenerate instead.
 *
 * Stack-argument counts for the kernel ordinals this title imports, MEASURED
 * from the user's own executable by counting the argument pushes the guest makes
 * at every call site that reaches each ordinal.
 *
 * The count is the MINIMUM over those sites, because the only error the method
 * makes is over-counting: a function's own callee-saved register saves fall
 * inside the lifter's call-site bracket and look like arguments. The estimator is
 * validated against arities known independently of it before this file is written
 * at all -- see tools/lift/callsites.py.
 *
 * MEASURED-FROM: lifted-c. The sites counted here are the ones present in the LIFTED C
 * (generated/lifted/gen/recomp_*.c), so code the lifter did not translate is absent and
 * the counts are NOT comparable with ordinal_callsites.json, which scans the XBE binary
 * directly and counts every decoded call site. Neither is the other's estimate.
 *
 * This is derived from the user's binary and is therefore never committed.
 */
"""


#: Matches one emitted MEASURED_ARITIES row: {ordinal, stack_args, sites, unanimous}.
_ARITY_ROW = re.compile(r"^\s*\{(\d+)u,\s*(\d+)u,\s*(\d+)u,\s*[01]\},\s*$", re.MULTILINE)


def read_previous_rows(path: Path) -> dict[int, tuple[int, int]]:
    """Parse ordinal -> (stack_args, sites) out of an existing `kernel_arity.inc`.

    Reads the pre-regeneration ON-DISK copy, not git: the file is generated and
    gitignored, so the disk is the only place the previous measurement exists.
    """
    if not path.is_file():
        return {}
    return {
        int(ordinal): (int(stack_args), int(sites))
        for ordinal, stack_args, sites in _ARITY_ROW.findall(path.read_text(encoding="utf-8"))
    }


def diff_against_previous(
    previous: dict[int, tuple[int, int]], report: ArityReport, measured: dict[int, int]
) -> list[str]:
    """Describe rows that vanished or changed arity since the previous emission.

    A re-lift against a manual set has already shrunk this table silently (116 -> 115
    rows, ordinal 91 gone, ordinal 200 across the quorum) because the estimator counts
    pushes in bodies the manual set deletes. Returns one line per regression so the
    caller can shout about them; an empty list means nothing was lost.
    """
    lines = []
    for ordinal in sorted(previous):
        old_args, old_sites = previous[ordinal]
        if ordinal not in measured:
            lines.append(
                f"ordinal {ordinal}: REMOVED (was {old_args} stack args, {old_sites} sites)"
            )
            continue
        new_args = measured[ordinal]
        new_sites = report.ordinals[ordinal].sites
        if new_args != old_args:
            lines.append(f"ordinal {ordinal}: stack_args {old_args} -> {new_args}")
        elif new_sites < old_sites:
            lines.append(f"ordinal {ordinal}: sites {old_sites} -> {new_sites} (lost voters)")
    return lines


def emit_c(report: ArityReport, path: Path) -> int:
    """Write the measured arities as a C table. Returns how many were written.

    Refuses to write anything if the measurement cannot reproduce the
    independently-known arities, because a table that fails its own check is
    worse than no table: the host stops cleanly without one and desyncs `esp`
    silently with a wrong one.
    """
    mismatches, checked = report.validate()
    if checked == 0:
        raise ValueError(
            "the arity measurement was never checked against a known answer, so it "
            "must not be used; expected some of KNOWN_STACK_ARGS to be called"
        )
    if mismatches:
        raise ValueError(
            "the arity measurement disagrees with independently-known arities "
            f"{mismatches}; refusing to emit a table that fails its own validation"
        )

    measured = report.measured()
    # Compare against the pre-regeneration on-disk table BEFORE overwriting it. A
    # manual-set re-lift has shrunk this table before and nothing noticed, so a lost
    # or re-measured row is shouted on stderr rather than discovered in a desync.
    regressions = diff_against_previous(read_previous_rows(path), report, measured)
    if regressions:
        print(
            f"WARNING: kernel_arity.inc regression against the previous {path}:",
            *(f"  {line}" for line in regressions),
            "  The previous table is being overwritten. If a manual set deleted the",
            "  bodies these sites lived in, the new counts rest on less evidence.",
            sep="\n",
            file=sys.stderr,
        )
    lines = [
        BANNER,
        "",
        f"/* Validated: reproduced {checked} independently-known arities, 0 mismatches. */",
        "",
        "typedef struct {",
        "    unsigned ordinal;",
        "    unsigned stack_args;",
        "    /* How many call sites were measured, and whether they all agreed. Low",
        "     * site counts and disagreement both weaken the evidence without",
        "     * invalidating it; they are carried so the host can say so. */",
        "    unsigned sites;",
        "    unsigned char unanimous;",
        "} measured_arity;",
        "",
        "static const measured_arity MEASURED_ARITIES[] = {",
    ]
    for ordinal, stack_args in sorted(measured.items()):
        arity = report.ordinals[ordinal]
        lines.append(
            f"    {{{ordinal}u, {stack_args}u, {arity.sites}u, {1 if arity.unanimous else 0}}},"
        )
    lines.append("};")
    lines.append("")
    lines.append(
        "#define MEASURED_ARITY_COUNT (sizeof(MEASURED_ARITIES) / sizeof(MEASURED_ARITIES[0]))"
    )
    lines.append("")
    conventions = _export_conventions()
    unknown = sorted(
        ordinal
        for ordinal, use in report.slot_uses.items()
        if (use.calls or use.reads) and ordinal not in conventions
    )
    called_data = sorted(
        ordinal
        for ordinal, use in report.slot_uses.items()
        if use.calls and conventions.get(ordinal) is Convention.DATA
    )
    if unknown or called_data:
        raise ValueError(
            f"kernel export classification unresolved: unknown ordinals {unknown}; "
            f"DATA exports attributed as calls {called_data}; refusing to emit"
        )
    lines.append("/* Observed kernel DATA exports, classified by the committed nxdk")
    lines.append(" * xboxkrnl.exe.def NONAME DATA markers, not by absence of calls.")
    lines.append(" * Read-only function pointers remain functions. Unknown kinds refuse")
    lines.append(" * emission instead of silently becoming data or function exports. */")
    data_exports = report.data_export_ordinals()
    if data_exports:
        body = ", ".join(f"{ordinal}u" for ordinal in data_exports)
        lines.append(f"static const unsigned MEASURED_DATA_ORDINALS[] = {{{body}}};")
    else:
        lines.append("static const unsigned MEASURED_DATA_ORDINALS[1] = {0u};")
    lines.append(f"#define MEASURED_DATA_ORDINAL_COUNT {len(data_exports)}u")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")
    return len(measured)


# --------------------------------------------------------------------------------
# The image scan.
#
# The arity measurement above reads the LIFTED C, because that is where the push
# windows are. The call-site RANKING reads the binary directly, for two reasons:
# the lift covers only what the lifter recognised as a function, and the ranking's
# whole job is to say what the guest asks for, which must not inherit the lifter's
# idea of what code is. Both scans attribute the same three shapes, so a
# disagreement between them is informative rather than confusing.
# --------------------------------------------------------------------------------

#: XBE section header flag: this section is executable.
SECTION_EXECUTABLE = 0x4

#: How far a slot->register binding is followed forward from its load. The longest
#: real binding on the retail image spans 0xAD bytes (ordinal 202's, 0x4145AF to
#: 0x41465C), so this is generous; it is bounded at all so a register that is never
#: written again cannot drag the scan across half a section.
REGISTER_TRACK_BYTES = 1024

#: Mnemonics after which execution does not fall through to the next instruction, so
#: a binding followed by fall-through cannot survive them. A `call` is deliberately
#: absent: it returns.
FLOW_ENDS = frozenset({"ret", "retf", "retfq", "iret", "iretd", "jmp", "ljmp", "hlt", "ud2"})

#: What may precede an import jump stub. A stub is a whole one-instruction function,
#: so nothing falls into it: the bytes before it end a function or are padding.
#:
#: This is the test that separates a STUB from a TAIL CALL, and the difference is not
#: cosmetic -- a stub is not a site and its callers are, whereas a tail call IS a
#: site and has no callers to redistribute to. Six of this image's twenty-two
#: `jmp dword ptr [slot]` instructions are tail calls sitting after a `pop`/`mov`
#: epilogue inside a larger function, and calling them stubs loses three of
#: `KfLowerIrql`'s sites outright. Two independent checks agree with this one on all
#: twenty-two: every instruction it calls a stub is the target of a direct `call` and
#: is flagged `is_thunk` by the function-boundary pass, and no tail call is either.
STUB_PRECEDERS = FLOW_ENDS | frozenset({"int3"})

#: Every name capstone can give for a part of each 32-bit register, so a write to
#: `al` or `si` is recognised as destroying the binding held in `eax` or `esi`.
_REGISTER_FAMILY: dict[str, str] = {
    name: parent
    for parent, names in {
        "eax": ("eax", "ax", "al", "ah"),
        "ebx": ("ebx", "bx", "bl", "bh"),
        "ecx": ("ecx", "cx", "cl", "ch"),
        "edx": ("edx", "dx", "dl", "dh"),
        "esi": ("esi", "si", "sil"),
        "edi": ("edi", "di", "dil"),
        "ebp": ("ebp", "bp", "bpl"),
        "esp": ("esp", "sp", "spl"),
    }.items()
    for name in names
}


@dataclass(frozen=True)
class CallSite:
    """One place in the image from which control reaches a kernel ordinal."""

    va: int
    section: str
    ordinal: int
    #: One of `SITE_KINDS`.
    kind: str
    #: For `SITE_STUB`, the stub's VA; for `SITE_REGISTER`, the VA of the load that
    #: put the slot into the register. Zero for `SITE_BRACKET`, which needs no
    #: second instruction. Carried so every attribution can be checked by hand.
    via: int = 0


@dataclass(frozen=True)
class ImageScan:
    """Every kernel call site in the image, by ordinal, section and shape."""

    sites: tuple[CallSite, ...]
    #: VA of each import jump stub -> the ordinal it jumps to. A stub is not a site.
    stubs: dict[int, int]
    #: Ordinal -> references to its slot that are reads, not calls.
    reads: dict[int, int]
    #: Section name -> how many times the linear decode had to resynchronise. Zero
    #: everywhere would mean the decode never met a byte that is not an instruction,
    #: which on real `.text` means the sweep stopped early instead.
    resyncs: dict[str, int]
    #: Every ordinal the XBE imports, called or not.
    imported: tuple[int, ...]

    def counts(self) -> dict[int, int]:
        """Ordinal -> number of call sites, descending by count then ordinal."""
        tally: dict[int, int] = defaultdict(int)
        for site in self.sites:
            tally[site.ordinal] += 1
        return dict(sorted(tally.items(), key=lambda item: (-item[1], item[0])))

    def per_section(self) -> dict[str, int]:
        tally: dict[str, int] = defaultdict(int)
        for site in self.sites:
            tally[site.section] += 1
        return dict(sorted(tally.items(), key=lambda item: -item[1]))

    def per_kind(self) -> dict[str, int]:
        tally = {kind: 0 for kind in SITE_KINDS}
        for site in self.sites:
            tally[site.kind] += 1
        return tally

    def kinds_for(self, ordinal: int) -> dict[str, int]:
        tally = {kind: 0 for kind in SITE_KINDS}
        for site in self.sites:
            if site.ordinal == ordinal:
                tally[site.kind] += 1
        return tally

    def data_export_ordinals(self) -> tuple[int, ...]:
        """Observed reads whose authoritative export kind is DATA."""
        return _known_data_ordinals(ordinal for ordinal, count in self.reads.items() if count)

    def unclassified_export_ordinals(self) -> tuple[int, ...]:
        """Observed ordinals absent from the oracle; never guessed as DATA."""
        observed = set(self.reads) | {site.ordinal for site in self.sites}
        return tuple(sorted(observed - _export_conventions().keys()))

    def uncalled_ordinals(self) -> tuple[int, ...]:
        """Imported ordinals no site reaches, whether read as data or untouched."""
        called = {site.ordinal for site in self.sites}
        return tuple(sorted(set(self.imported) - called))

    def coverage_depth(self, fraction: float = 0.8) -> int:
        """How many of the top ordinals it takes to cover `fraction` of all sites."""
        total = len(self.sites)
        if total == 0:
            return 0
        running = 0
        for depth, count in enumerate(self.counts().values(), start=1):
            running += count
            if running >= fraction * total:
                return depth
        return len(self.counts())


def _detail_decoder() -> capstone.Cs:
    """A detail-enabled 32-bit x86 decoder. Built per call: no shared state."""
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    return decoder


def _slot_operand(insn: capstone.CsInsn, slots: dict[int, int]) -> int | None:
    """The thunk slot this instruction addresses absolutely, if any.

    Only `[disp32]` with no base, index or segment counts. `[eax + 0x475780]` is an
    indexed access that merely happens to be based at the table and says nothing
    about one slot, so attributing it to the ordinal at offset zero would be wrong.
    """
    for operand in insn.operands:
        if operand.type != cs_x86.X86_OP_MEM:
            continue
        memory = operand.value.mem
        if memory.base or memory.index or memory.segment:
            continue
        if memory.disp in slots:
            return memory.disp
    return None


def _called_register(insn: capstone.CsInsn) -> str | None:
    """The register an indirect `call reg` / `jmp reg` dispatches through."""
    if insn.mnemonic not in ("call", "jmp"):
        return None
    operands = insn.operands
    if len(operands) != 1 or operands[0].type != cs_x86.X86_OP_REG:
        return None
    return _REGISTER_FAMILY.get(insn.reg_name(operands[0].value.reg))


def _loaded_register(insn: capstone.CsInsn, slots: dict[int, int]) -> tuple[str, int] | None:
    """`mov reg32, dword ptr [slot]` -> (register, slot address)."""
    if insn.mnemonic != "mov":
        return None
    operands = insn.operands
    if len(operands) != 2:
        return None
    if operands[0].type != cs_x86.X86_OP_REG or operands[1].type != cs_x86.X86_OP_MEM:
        return None
    if operands[0].size != 4:
        return None
    slot = _slot_operand(insn, slots)
    if slot is None:
        return None
    register = _REGISTER_FAMILY.get(insn.reg_name(operands[0].value.reg))
    return (register, slot) if register else None


def _relative_target(data: bytes, offset: int, length: int, va: int) -> tuple[int, str] | None:
    """Target and mnemonic of a direct relative `call`/`jmp`, read from the bytes.

    Decoded arithmetically rather than through capstone because every call and jump
    in the image passes through here and only three encodings matter: `E8 rel32`,
    `E9 rel32` and `EB rel8`, none of which takes a prefix.
    """
    opcode = data[offset]
    if length == 5 and opcode in (0xE8, 0xE9):
        delta = int.from_bytes(data[offset + 1 : offset + 5], "little", signed=True)
        return (va + 5 + delta) & 0xFFFFFFFF, "call" if opcode == 0xE8 else "jmp"
    if length == 2 and opcode == 0xEB:
        delta = int.from_bytes(data[offset + 1 : offset + 2], "little", signed=True)
        return (va + 2 + delta) & 0xFFFFFFFF, "jmp"
    return None


def track_register_calls(
    data: bytes,
    offset: int,
    base_va: int,
    register: str,
    *,
    limit: int = REGISTER_TRACK_BYTES,
) -> tuple[int, ...]:
    """VAs of the indirect calls that dispatch through a slot just loaded into
    `register`. `offset` must be the first byte of the loading instruction.

    The walk follows FALL-THROUGH from the load and stops at the first of: a write
    to the register (including through a sub-register name); a call, if the register
    is one a callee may destroy; an instruction after which control does not fall
    through; an undecodable byte; or `limit` bytes. A conditional branch does NOT
    stop it, because both of ordinal 47's register sites and both of ordinal 202's
    sit behind one and a rule that stopped there would find none of them.

    THE COST OF THAT. Continuing past a branch means continuing past a join, so a
    path on which the register holds something else can reach the call. A
    register-attributed site is therefore a site that CAN reach the ordinal, not one
    that provably always does -- which is the right standard for a static ranking of
    what the guest asks for, and the wrong one for a claim about a single execution.

    Decoding is local and linear rather than via `normalise_text` because operand
    detail is needed and the starting point is already a boundary that
    `normalise_text` established, so there is nothing left to resynchronise onto.
    """
    decoder = _detail_decoder()
    volatile = register in VOLATILE_REGISTERS
    end = min(len(data), offset + limit)
    found: list[int] = []
    cursor = offset
    first = True

    while cursor < end:
        window = data[cursor : min(cursor + MAX_INSN_BYTES, end)]
        insn = next(decoder.disasm(window, base_va + cursor, count=1), None)
        if insn is None:
            break
        cursor += insn.size
        if first:
            first = False
            continue

        if _called_register(insn) == register:
            found.append(insn.address)
            if insn.mnemonic == "jmp":
                # A tail jump through the register: a site, and the end of the walk.
                break
        if insn.mnemonic in FLOW_ENDS:
            break
        if volatile and insn.mnemonic == "call":
            break
        _, written = insn.regs_access()
        if any(_REGISTER_FAMILY.get(insn.reg_name(reg)) == register for reg in written):
            break

    return tuple(found)


def executable_sections(xbe: Xbe, raw: bytes) -> list[tuple[str, int, bytes]]:
    """(name, base VA, bytes) for every section the XBE marks executable.

    All of them, not just `.text`. More than half of this title's kernel call sites
    live in the statically-linked library sections -- XNET, XPP, DSOUND, D3D,
    XONLINE -- and a scan of `.text` alone reports a third of the real total.
    """
    out: list[tuple[str, int, bytes]] = []
    for section in xbe.sections:
        if not section.flags & SECTION_EXECUTABLE:
            continue
        body = raw[section.raw_addr : section.raw_addr + section.raw_size]
        if body:
            out.append((section.name, section.virtual_addr, body))
    return out


def scan_image(xbe_path: Path) -> ImageScan:
    """Find every call site that reaches a kernel ordinal, by all three shapes.

    Linear sweeps are driven by `tools.codediff.normalise.normalise_text`, which
    resynchronises after a byte that is not an instruction. That is not a detail:
    capstone's `disasm` stops at the first undecodable byte and silently returns a
    short iterator, and an earlier version of this scan reported ZERO call sites
    because it decoded only a prefix of `.text`. This image needs 132
    resynchronisations in `.text` alone, and `ImageScan.resyncs` reports them so a
    sweep that quietly stopped early cannot look like a clean one.
    """
    raw = xbe_path.read_bytes()
    xbe = parse_xbe(raw)
    return scan_sections(
        executable_sections(xbe, raw),
        {
            xbe.kernel_thunk_addr + 4 * index: ordinal
            for index, ordinal in enumerate(xbe.kernel_import_ordinals)
        },
        image_lo=xbe.base_address,
        image_hi=xbe.base_address + xbe.size_of_image,
        imported=xbe.kernel_import_ordinals,
    )


def _decode_section(
    name: str, base_va: int, data: bytes, image_lo: int, image_hi: int
) -> NormalisedText:
    """Decode a whole section, RESYNCHRONISING past bytes that are not instructions.

    Delegates to `tools.codediff.normalise`, which already does this correctly.
    Calling capstone's `disasm` on the section in one shot instead is the single
    easiest way to break this module and the hardest to notice: it stops at the first
    undecodable byte and silently returns a short iterator, which is exactly how an
    earlier version of this scan reported ZERO kernel call sites across a `.text`
    that needs 132 resynchronisations.
    """
    return normalise_text(data, base_va, image_lo, image_hi, section_name=name)


def scan_sections(
    sections: Iterable[tuple[str, int, bytes]],
    slots: dict[int, int],
    *,
    image_lo: int,
    image_hi: int,
    imported: Iterable[int] = (),
) -> ImageScan:
    """Scan already-extracted (name, base VA, bytes) sections for kernel call sites.

    Split out from `scan_image` so the attribution rules can be exercised against
    hand-written byte sequences: every claim this module makes about a shape is a
    claim about a handful of instructions, and a test that has to build a whole XBE
    to check one of them is a test nobody will write.
    """
    # A slot address can only appear in an instruction at a position whose upper two
    # bytes match one of the table's. Testing that first keeps the detailed decode
    # off the overwhelming majority of instructions.
    high_halves = {address.to_bytes(4, "little")[2:] for address in slots}

    decoder = _detail_decoder()
    sites: list[CallSite] = []
    stubs: dict[int, int] = {}
    reads: dict[int, int] = defaultdict(int)
    resyncs: dict[str, int] = {}
    #: (target, caller VA, section, mnemonic) for every direct relative call and
    #: jump, held until every stub in the image is known -- the stub ordinal 24 goes
    #: through lives in `.text` and is called from XONLINE and XNET as well.
    relative: list[tuple[int, int, str, str]] = []

    for name, base_va, data in sections:
        normalised = _decode_section(name, base_va, data, image_lo, image_hi)
        resyncs[name] = len(normalised.undecodable)
        prior_mnemonic: str | None = None
        prior_end = -1
        for insn in normalised.insns:
            preceded_by_fallthrough = (
                prior_mnemonic is not None
                and prior_end == insn.offset
                and prior_mnemonic not in STUB_PRECEDERS
            )
            prior_mnemonic, prior_end = insn.mnemonic, insn.offset + insn.length

            start, length = insn.offset, insn.length
            direct = _relative_target(data, start, length, insn.va)
            if direct is not None:
                relative.append((direct[0], insn.va, name, direct[1]))
                continue
            window = data[start : start + length]
            if not any(window[i + 2 : i + 4] in high_halves for i in range(len(window) - 3)):
                continue
            decoded = next(decoder.disasm(window, insn.va, count=1), None)
            if decoded is None:
                continue
            slot = _slot_operand(decoded, slots)
            if slot is None:
                continue
            ordinal = slots[slot]
            if decoded.mnemonic == "call":
                sites.append(CallSite(insn.va, name, ordinal, SITE_BRACKET))
                continue
            if decoded.mnemonic == "jmp":
                if preceded_by_fallthrough:
                    # A TAIL CALL into the kernel from inside a larger function. The
                    # jump reads the slot itself, so it is a site of the same shape
                    # as a `call dword ptr [slot]`, and it has no callers to spread.
                    sites.append(CallSite(insn.va, name, ordinal, SITE_BRACKET))
                else:
                    # An import jump stub: a one-instruction function, not a site.
                    # Its callers are the sites, attributed once they are all known.
                    stubs[insn.va] = ordinal
                continue
            load = _loaded_register(decoded, slots)
            if load is None:
                reads[ordinal] += 1
                continue
            register, _ = load
            tracked = track_register_calls(data, start, base_va, register)
            for va in tracked:
                sites.append(CallSite(va, name, ordinal, SITE_REGISTER, via=insn.va))
            if not tracked:
                # The value went somewhere other than a call: a genuine data read.
                reads[ordinal] += 1

    for target, va, section, mnemonic in relative:
        ordinal = stubs.get(target)
        if ordinal is None or va in stubs:
            continue
        # A `jmp` to a stub is a tail call and reaches the ordinal just as a `call`
        # does; both are counted, and `mnemonic` is not otherwise needed.
        del mnemonic
        sites.append(CallSite(va, section, ordinal, SITE_STUB, via=target))

    return ImageScan(
        sites=tuple(sorted(sites, key=lambda site: (site.va, site.ordinal, site.kind))),
        stubs=dict(sorted(stubs.items())),
        reads=dict(sorted(reads.items())),
        resyncs=resyncs,
        imported=tuple(sorted(set(imported))),
    )


def ranked_queue(scan: ImageScan, implemented: Iterable[int] = ()) -> list[dict[str, object]]:
    """The work queue: every called ordinal, most-called first."""
    done = set(implemented)
    return [
        {
            "ordinal": ordinal,
            "sites": count,
            "name": KERNEL_ORDINALS.get(ordinal, f"ordinal_{ordinal}"),
            "implemented": ordinal in done,
            "kinds": scan.kinds_for(ordinal),
            "measured_from": "xbe-binary",
        }
        for ordinal, count in scan.counts().items()
    ]


def render_scan(scan: ImageScan) -> str:
    """A human-readable summary of an image scan."""
    counts = scan.counts()
    lines = [
        f"kernel call sites  {len(scan.sites)} over {len(counts)} of "
        f"{len(scan.imported)} imported ordinals",
        "  by shape          "
        + ", ".join(f"{kind} {count}" for kind, count in scan.per_kind().items()),
        f"  import stubs      {len(scan.stubs)}",
        "  by section        "
        + ", ".join(f"{name} {count}" for name, count in scan.per_section().items()),
        "  resyncs           "
        + ", ".join(f"{name} {count}" for name, count in scan.resyncs.items()),
        f"  80% covered by    the top {scan.coverage_depth()} ordinals",
        f"  data exports      {len(scan.data_export_ordinals())}: "
        + ", ".join(str(ordinal) for ordinal in scan.data_export_ordinals()),
        f"  never reached     {len(scan.uncalled_ordinals())}",
        f"  unknown export kinds {list(scan.unclassified_export_ordinals())}",
        "",
        f"  {'ord':>5} {'sites':>6}  {'bracket/register/stub':<22} name",
    ]
    for ordinal, count in counts.items():
        kinds = scan.kinds_for(ordinal)
        shape = "/".join(str(kinds[kind]) for kind in SITE_KINDS)
        lines.append(f"  {ordinal:>5} {count:>6}  {shape:<22} {KERNEL_ORDINALS.get(ordinal, '?')}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    """Scan an XBE and report, and optionally write, the ranked ordinal queue."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--xbe", required=True, type=Path, help="the XBE to scan")
    parser.add_argument(
        "--json", type=Path, default=None, help="write the ranked queue here as JSON"
    )
    parser.add_argument(
        "--implemented",
        default="",
        help="comma-separated ordinals already implemented, flagged in the queue",
    )
    parser.add_argument(
        "--top", type=int, default=0, help="print only the top N ordinals (0 = all)"
    )
    args = parser.parse_args(argv)

    scan = scan_image(args.xbe)
    implemented = [int(part) for part in args.implemented.split(",") if part.strip()]
    report = render_scan(scan)
    if args.top:
        head = report.split("\n")
        body_start = next(i for i, line in enumerate(head) if line.strip().startswith("ord"))
        report = "\n".join(head[: body_start + 1 + args.top])
    print(report)

    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps(ranked_queue(scan, implemented), indent=1) + "\n", encoding="utf-8"
        )
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
