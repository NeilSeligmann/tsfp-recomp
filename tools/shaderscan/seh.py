# SPDX-License-Identifier: GPL-3.0-or-later
"""x86 structured exception dispatch for the analysis emulator (T446).

WHY. The title's compiler rejects some sources with a C++ throw: `__CxxThrowException` fills
an `EXCEPTION_RECORD` and calls the kernel's `RtlRaiseException`. Whether the original then
catches it and returns an error HRESULT depends on the guest's own `fs:[0]` handler chain, the
statically linked `__CxxFrameHandler` and `RtlUnwind`. The harness used to stop at that kernel
call (`Outcome.KERNEL_CALL`), so the original's return was never observed. This module models
only the two kernel services, in Python, and runs every guest handler as guest code.

WHAT IS MODELLED. The NT x86 semantics, from the documented Windows contract and not from the
Xbox kernel binary (which is not in the image, INFERRED to match):

  * `RtlRaiseException(record)`: capture a CONTEXT that resumes after the call, then walk the
    registration chain at `fs:[0]`, calling `handler(record, frame, context, dispatcher)`
    (cdecl) for each frame. `ExceptionContinueExecution` (0) resumes at the raise site (refused
    for a non-continuable record), `ExceptionContinueSearch` (1) goes to the next frame.
    Reaching the chain end is `unhandled`.
  * `RtlUnwind(target, target_ip, record, retval)` (stdcall, 4 arguments): flag the record as
    unwinding, call each frame's handler up to `target` with the unwinding flag, unlink every
    frame it passes, then resume at `target_ip` (or the caller) with `eax = retval`, the
    callee saved registers of the call and the arguments popped.

A catch is the handler jumping into the guest function's continuation, so the dispatcher's own
return is never reached. The abandoned walk stays below newer ones and is never consulted.

NOT MODELLED. Nested and collided exceptions (dispositions 2 and 3), kernel stack bound
checks, debugger and second chance handling, and floating point or debug register context.
A handler that returns one of those dispositions ends the run as `unhandled` with its reason.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from unicorn import Uc
from unicorn.x86_const import (
    UC_X86_REG_EAX,
    UC_X86_REG_EBP,
    UC_X86_REG_EBX,
    UC_X86_REG_ECX,
    UC_X86_REG_EDI,
    UC_X86_REG_EDX,
    UC_X86_REG_EFLAGS,
    UC_X86_REG_ESI,
    UC_X86_REG_ESP,
)

ORDINAL_RAISE_EXCEPTION = 302
ORDINAL_RTL_UNWIND = 312

EXCEPTION_NONCONTINUABLE = 0x1
EXCEPTION_UNWINDING = 0x2
EXCEPTION_EXIT_UNWIND = 0x4
STATUS_UNWIND = 0xC0000027

DISPOSITION_CONTINUE_EXECUTION = 0
DISPOSITION_CONTINUE_SEARCH = 1

END_OF_CHAIN = 0xFFFFFFFF
#: x86 CONTEXT, the offsets the handlers could read. ContextFlags is CONTROL | INTEGER | SEGMENTS.
CONTEXT_SIZE = 0xCC
CONTEXT_FLAGS = 0x10007
#: Scratch below the stack pointer of the stop: a CONTEXT, a dispatcher context, the handler
#: arguments. The distance also keeps clear of what the interrupted frame could still read.
WORK_GAP = 0x400
#: Frames a single walk may visit, a guard against a corrupted or looping chain.
MAX_FRAMES = 64

_REGISTERS = (
    ("eax", UC_X86_REG_EAX),
    ("ebx", UC_X86_REG_EBX),
    ("ecx", UC_X86_REG_ECX),
    ("edx", UC_X86_REG_EDX),
    ("esi", UC_X86_REG_ESI),
    ("edi", UC_X86_REG_EDI),
    ("ebp", UC_X86_REG_EBP),
    ("eflags", UC_X86_REG_EFLAGS),
)


@dataclass(frozen=True)
class SehEvent:
    """One step of dispatch, kept so a run can say which handlers ran and what they answered."""

    #: `raise`, `unwind`, `handler` and `unwind_handler` (a guest handler was called),
    #: `disposition` (it returned, `disposition` holds EAX).
    kind: str
    #: Exception code of the record being dispatched or unwound.
    code: int
    frame: int = 0
    handler: int = 0
    #: What the handler returned, None for steps that call nothing.
    disposition: int | None = None


@dataclass(frozen=True)
class SehStop:
    """The model cannot continue. `reason` names why, it is never an HRESULT."""

    reason: str


@dataclass
class _Walk:
    """One raise or unwind in progress."""

    unwinding: bool
    record: int
    context: int
    #: Registers and resume point if the walk finishes. For a raise this is the raise site.
    registers: dict[str, int]
    esp_after: int
    eip_after: int
    #: The frame being called and the one after it.
    frame: int
    following: int = 0
    #: Stack pointer the handler runs with, the matching return leaves esp one dword above.
    call_esp: int = 0
    target: int = 0
    visited: int = 0
    last_frame: int = 0


@dataclass
class Dispatcher:
    """SEH dispatch over one Unicorn instance. Reset it at the start of every run."""

    uc: Uc
    tib: int
    stack_low: int
    stack_high: int
    #: Address handlers return to. The emulator stops there and calls `on_return`.
    return_address: int
    events: list[SehEvent] = field(default_factory=list)
    _walks: list[_Walk] = field(default_factory=list)

    def reset(self) -> None:
        self.events = []
        self._walks = []

    # -- guest memory helpers -------------------------------------------------------------

    def _u32(self, address: int) -> int:
        return int.from_bytes(self.uc.mem_read(address, 4), "little")

    def _put(self, address: int, *values: int) -> None:
        self.uc.mem_write(address, struct.pack(f"<{len(values)}I", *values))

    def _snapshot(self) -> dict[str, int]:
        return {name: self.uc.reg_read(register) for name, register in _REGISTERS}

    def _restore(self, registers: dict[str, int]) -> None:
        for name, register in _REGISTERS:
            self.uc.reg_write(register, registers[name])

    def _frame_ok(self, walk: _Walk, frame: int) -> bool:
        return (
            self.stack_low <= frame < self.stack_high - 8
            and frame % 4 == 0
            and frame > walk.last_frame
        )

    def _write_context(self, base: int, registers: dict[str, int], esp: int, eip: int) -> None:
        """The NT x86 CONTEXT fields a handler may read, as of the resume point."""
        self.uc.mem_write(base, bytes(CONTEXT_SIZE))
        self._put(base, CONTEXT_FLAGS)
        for offset, value in (
            (0x9C, registers["edi"]),
            (0xA0, registers["esi"]),
            (0xA4, registers["ebx"]),
            (0xA8, registers["edx"]),
            (0xAC, registers["ecx"]),
            (0xB0, registers["eax"]),
            (0xB4, registers["ebp"]),
            (0xB8, eip),
            (0xC0, registers["eflags"]),
            (0xC4, esp),
        ):
            self._put(base + offset, value)

    # -- entry points -------------------------------------------------------------------

    def on_kernel_call(self, ordinal: int) -> int | SehStop | None:
        """Handle a stop at kernel import `ordinal`. The resume address, a stop, or None when
        the ordinal is not an exception service."""
        if ordinal == ORDINAL_RAISE_EXCEPTION:
            return self._raise()
        if ordinal == ORDINAL_RTL_UNWIND:
            return self._unwind()
        return None

    def on_return(self, esp: int, eax: int) -> int | SehStop:
        """A handler returned to its sentinel. The live walk is the newest one, a walk a catch
        abandoned sits below it and is never consulted again."""
        if not self._walks:
            return SehStop("handler returned to no pending dispatch")
        walk = self._walks[-1]
        if walk.call_esp + 4 != esp:
            self._walks.pop()
            return SehStop(f"handler returned with an unbalanced stack ({esp:#x})")
        self.events.append(
            SehEvent(
                "disposition", self._u32(walk.record), walk.frame, self._u32(walk.frame + 4), eax
            )
        )
        if walk.unwinding:
            return self._unwind_step(walk, unlink=True)
        if eax == DISPOSITION_CONTINUE_EXECUTION:
            flags = self._u32(walk.record + 4)
            if flags & EXCEPTION_NONCONTINUABLE:
                return SehStop("handler continued a non-continuable exception")
            self._walks.pop()
            self._restore(walk.registers)
            self.uc.reg_write(UC_X86_REG_ESP, walk.esp_after)
            return walk.eip_after
        if eax == DISPOSITION_CONTINUE_SEARCH:
            walk.frame = walk.following
            return self._raise_step(walk)
        return SehStop(f"handler returned disposition {eax} (nested exceptions are not modelled)")

    # -- RtlRaiseException --------------------------------------------------------------

    def _raise(self) -> int | SehStop:
        esp = self.uc.reg_read(UC_X86_REG_ESP)
        record = self._u32(esp + 4)
        registers = self._snapshot()
        base = ((esp - WORK_GAP - CONTEXT_SIZE) & ~0xF) - 0x40
        self._write_context(base, registers, esp + 8, self._u32(esp))
        walk = _Walk(
            unwinding=False,
            record=record,
            context=base,
            registers=registers,
            esp_after=esp + 8,
            eip_after=self._u32(esp),
            frame=self._u32(self.tib),
        )
        walk.call_esp = base - 0x20
        self._walks.append(walk)
        self.events.append(SehEvent("raise", self._u32(record)))
        return self._raise_step(walk)

    def _raise_step(self, walk: _Walk) -> int | SehStop:
        if walk.frame == END_OF_CHAIN:
            self._walks.pop()
            return SehStop("unhandled: the registration chain ended")
        if walk.visited >= MAX_FRAMES or not self._frame_ok(walk, walk.frame):
            self._walks.pop()
            return SehStop(f"corrupt registration chain at {walk.frame:#x}")
        walk.visited += 1
        walk.last_frame = walk.frame
        walk.following = self._u32(walk.frame)
        return self._call_handler(walk)

    def _call_handler(self, walk: _Walk) -> int:
        """Set up `handler(record, frame, context, dispatcher)` as a cdecl call."""
        handler = self._u32(walk.frame + 4)
        kind = "unwind_handler" if walk.unwinding else "handler"
        self.events.append(SehEvent(kind, self._u32(walk.record), walk.frame, handler))
        dispatcher_context = walk.call_esp - 0x10
        self.uc.mem_write(dispatcher_context, bytes(8))
        self._put(
            walk.call_esp,
            self.return_address,
            walk.record,
            walk.frame,
            walk.context,
            dispatcher_context,
        )
        self.uc.reg_write(UC_X86_REG_ESP, walk.call_esp)
        self.uc.reg_write(UC_X86_REG_EAX, 0)
        return handler

    # -- RtlUnwind ----------------------------------------------------------------------

    def _unwind(self) -> int | SehStop:
        esp = self.uc.reg_read(UC_X86_REG_ESP)
        caller = self._u32(esp)
        target, target_ip, record, retval = (self._u32(esp + 4 * index) for index in (1, 2, 3, 4))
        registers = self._snapshot()
        registers["eax"] = retval
        base = ((esp - WORK_GAP - CONTEXT_SIZE) & ~0xF) - 0x40
        if record == 0:
            record = base - 0x80
            self.uc.mem_write(record, bytes(0x50))
            self._put(record, STATUS_UNWIND)
        flags = self._u32(record + 4) | EXCEPTION_UNWINDING
        if target == 0:
            flags |= EXCEPTION_EXIT_UNWIND
        self._put(record + 4, flags)
        self._write_context(base, registers, esp + 20, target_ip or caller)
        walk = _Walk(
            unwinding=True,
            record=record,
            context=base,
            registers=registers,
            esp_after=esp + 20,
            eip_after=target_ip or caller,
            frame=self._u32(self.tib),
            target=target,
        )
        walk.call_esp = base - 0x20
        self._walks.append(walk)
        self.events.append(SehEvent("unwind", self._u32(record), target))
        return self._unwind_step(walk, unlink=False)

    def _unwind_step(self, walk: _Walk, *, unlink: bool) -> int | SehStop:
        if unlink:
            # The frame whose handler just ran is gone, as is everything the guest chained below.
            walk.frame = walk.following
            self._put(self.tib, walk.frame)
        if walk.frame == walk.target:
            self._walks.pop()
            self._restore(walk.registers)
            self.uc.reg_write(UC_X86_REG_ESP, walk.esp_after)
            return walk.eip_after
        if walk.frame == END_OF_CHAIN:
            self._walks.pop()
            return SehStop("RtlUnwind target frame is not on the registration chain")
        if walk.visited >= MAX_FRAMES or not self._frame_ok(walk, walk.frame):
            self._walks.pop()
            return SehStop(f"corrupt registration chain at {walk.frame:#x}")
        walk.visited += 1
        walk.last_frame = walk.frame
        walk.following = self._u32(walk.frame)
        return self._call_handler(walk)
