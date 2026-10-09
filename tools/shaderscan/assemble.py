# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the title's own `XGAssembleShader` under emulation, to see the microcode it emits.

WHY. `builders` shows the title builds a vertex-shader token stream and a pixel-shader
source string out of static fragments. The assembler then turns each into what the GPU
runs: NV2A vertex-program microcode, or a pixel-shader (register combiner) definition.
The count of distinct programs the title can run is the count of distinct OUTPUTS, so
the assembler has to be run. It is statically linked into the XBE (`XGRPH`), so it is
x86 in the image like any other function and Unicorn can execute it.

WHAT IT NEEDS, AND WHY EACH IS A STUB RATHER THAN A GUESS. The assembler is ordinary C
that expects a running process. Three things are missing in a bare emulator, each was
found by letting it fault and reading the faulting instruction (`tests` pin the
mechanism, `docs/shader-inputs.md` records the trace):

  * `fs:[0]` and `fs:[0x20]`. The compiler's exception-frame prologue reads `fs:[0]`,
    and one path reads a processor-control-block pointer at `fs:[0x20]`. A GDT with an
    FS segment based on a small zeroed block satisfies both.
  * a heap allocator. The title statically links a heap whose handle is a global that
    only the CRT start-up fills. The allocator's entry is replaced by a bump allocator
    in emulated memory and the free routine by a no-op returning success. Both
    addresses are arguments, because they belong to this build.

Everything else the assembler touches is in the image or on the stack.

HOW A CALL ENDS. `run` returns an `Outcome`, and only `RETURNED` carries an HRESULT. A
budget expiry, a fault, a kernel stop, a halt and a wrong stack cleanup all leave the
result unset, so an intermediate EAX can never be read as a return value (T202). `assemble`
keeps its older contract and returns None for the unfinished outcomes.

EVERY RUN STARTS FROM THE SAME STATE. The image, stack, thread block (`fs:[0]` chain), source
mapping, general registers, flags and the used heap are all rewritten, because a run that
stopped inside the compiler leaves frames registered, stack garbage and scribbled memory
that the next run would otherwise inherit. `flush_to_guard` places the source flush
against an unmapped page so a read past the supplied length is observable.

NOTHING IS PRINTED. Outputs are hashed and characterised by counts. A vertex program
is checked against `vsh.is_plausible` as a by-product, which is an independent test of
the instruction decoder: these programs were produced by the title's own code from token
streams, not read out of `.data`.
"""

from __future__ import annotations

import enum
import hashlib
import multiprocessing
import os
import struct
from collections.abc import Sequence
from dataclasses import dataclass, replace

from unicorn import (
    UC_ARCH_X86,
    UC_HOOK_CODE,
    UC_HOOK_MEM_UNMAPPED,
    UC_MODE_32,
    Uc,
    UcError,
    unicorn_const,
)
from unicorn.x86_const import (
    UC_X86_REG_CS,
    UC_X86_REG_DS,
    UC_X86_REG_EAX,
    UC_X86_REG_EBP,
    UC_X86_REG_EBX,
    UC_X86_REG_ECX,
    UC_X86_REG_EDI,
    UC_X86_REG_EDX,
    UC_X86_REG_EFLAGS,
    UC_X86_REG_EIP,
    UC_X86_REG_ES,
    UC_X86_REG_ESI,
    UC_X86_REG_ESP,
    UC_X86_REG_FS,
    UC_X86_REG_GDTR,
    UC_X86_REG_SS,
)

from tools.shaderscan.image import Image
from tools.shaderscan.seh import Dispatcher, SehEvent, SehStop

STACK_BASE = 0x0100_0000
STACK_SIZE = 0x0010_0000
HEAP_BASE = 0x0200_0000
HEAP_SIZE = 0x0100_0000
SOURCE_BASE = 0x0300_0000
SOURCE_SIZE = 0x0001_0000
OUTPUT_SLOT = SOURCE_BASE + 0x8000
#: One zeroed pointer slot per extra output argument (T385), at `EXTRA_SLOT_BASE + 4 * index`.
EXTRA_SLOT_BASE = SOURCE_BASE + 0x8100
TIB_BASE = 0x0500_0000
GDT_BASE = 0x0600_0000
SENTINEL = 0x0000_4000
#: Where a guest exception handler returns to when `run(dispatch_exceptions=True)` calls it.
#: Same mapped page as `SENTINEL`, and the emulator stops there to continue the dispatch.
DISPATCH_RETURN = SENTINEL + 0x10
#: Emulation segments a dispatching run may use, one per kernel stop or handler return.
MAX_SEH_SEGMENTS = 512
#: Source placement used by `flush_to_guard`: the last byte of the source is the last
#: mapped byte, so one read past the source end is an unmapped-memory fault.
GUARD_BASE = 0x0700_0000
GUARD_SIZE = 0x0001_0000
#: Where the bump pointer lives, just below the heap's end.
BUMP_SLOT = HEAP_BASE + HEAP_SIZE - 4
# The word immediately before the bump pointer counts allocation requests only when the
# bounded/fail-at test allocator is installed.
ALLOC_COUNT_SLOT = BUMP_SLOT - 4
#: Bytes past the last block that are checked, and cleared again after a run, because an
#: original that overruns its last block writes there (T485). Each run leaves them zero.
OVERRUN_TAIL = 0x400

#: Instruction budget for the one-off static initialiser, which is a few stores.
STATIC_INIT_BUDGET = 10_000

#: Instruction budget for one assembly. The measured need is a small fraction of this.
INSN_LIMIT = 20_000_000

#: `fs:[0x20]` points here and the block is zeroed, so a read through it yields 0.
PRCB_OFFSET = 0x400

#: XGBuffer object layout, measured from `XGBuffer_GetBufferPointer` (reads `[obj + 4]`)
#: and from the object the assembler returns: refcount, data pointer, byte size.
BUFFER_POINTER_OFFSET = 4
BUFFER_SIZE_OFFSET = 8


@dataclass(frozen=True)
class AssemblerSpec:
    """Addresses in the title that the emulation needs. All belong to one build."""

    entry: int
    heap_alloc: int
    heap_free: int
    #: Stack arguments of the assembler, all dwords.
    argument_count: int = 11
    #: A no-argument routine of the title's C runtime that fills static tables the startup
    #: code would have filled, run once at construction and baked into the pristine image
    #: (T385). The title's `_cfltcvt_init` (`0x3C843D`) points the floating point table at
    #: its real routines. Without it a source with a float literal (`def c0, 1.0, ...`)
    #: stops with the CRT error R6002 "floating point not loaded" through `int 0x2d`.
    static_init: int | None = None


#: Kernel imports are called through thunks that hold `0x80000000 | ordinal`, and nothing
#: is mapped there, so a call into the kernel shows up as an instruction fetch at that value.
KERNEL_THUNK_FLOOR = 0x8000_0000
KERNEL_THUNK_CEILING = 0x8001_0000
#: `RtlRaiseException`, the one kernel call the assembler makes (measured, see
#: `docs/shader-inputs.md`): it is how a source that does not assemble reports failure.
ORDINAL_RAISE_EXCEPTION = 302


class Outcome(enum.Enum):
    """How one emulated call ended. Only RETURNED carries an original HRESULT.

    The others are limits of the observation. None of them is a returned error: the
    original produced no return value, so nothing may be compared against one.
    """

    #: Reached the return sentinel with the expected stdcall stack. EAX is the HRESULT.
    RETURNED = "returned"
    #: Stopped at a kernel import thunk. Exception dispatch is not emulated, so a call to
    #: `RtlRaiseException` is the original's own reject path with no observed return.
    KERNEL_CALL = "kernel_call"
    #: Unicorn stopped because the instruction budget ran out, still inside the compiler.
    BUDGET_EXHAUSTED = "budget_exhausted"
    #: A CPU or memory fault other than a kernel call (unmapped access, invalid opcode).
    FAULT = "fault"
    #: Reached the return sentinel with the wrong stack pointer (wrong RET imm16).
    BAD_CLEANUP = "bad_cleanup"
    #: Only with `dispatch_exceptions`: an exception no handler caught, or one the dispatch
    #: model cannot continue (`Run.seh_stop` says which). Nothing was returned.
    UNHANDLED = "unhandled"
    #: The emulator never started: the source did not fit the harness buffer.
    REFUSED = "refused"
    #: Unicorn returned normally at neither the sentinel nor an exhausted budget (HLT).
    HALTED = "halted"


@dataclass(frozen=True)
class FaultInfo:
    """A fault Unicorn reported. `address` is the accessed address for memory faults."""

    error: str
    eip: int
    address: int | None = None
    access: str | None = None
    #: Which harness region the address is next to, see `_region_of`.
    region: str | None = None


@dataclass(frozen=True)
class RaisedException:
    """What `RtlRaiseException` was handed when the assembler stopped at it.

    Read from the stack and the guest exception chain at the stop, never emulated.
    `handlers` are the `fs:[0]` registration handlers, innermost first. Empty fields mean
    the record could not be read.
    """

    code: int | None = None
    flags: int | None = None
    address: int | None = None
    parameters: tuple[int, ...] = ()
    handlers: tuple[int, ...] = ()


@dataclass(frozen=True)
class Assembled:
    """What one assembly produced. The bytes are held for analysis, never printed.

    `kernel_call` is the ordinal of the kernel import the assembler called and was
    stopped at, or None when it returned normally. Exception dispatch is not emulated, so
    a call to `ORDINAL_RAISE_EXCEPTION` means "the assembler rejected this source".
    """

    status: int
    data: bytes
    kernel_call: int | None = None
    #: `(argument index, buffer bytes)` for each extra output argument that `run` was asked
    #: to supply (`output_args`), read whatever the HRESULT. None when the assembler left that
    #: slot NULL, b"" when it set a buffer that holds nothing readable.
    extras: tuple[tuple[int, bytes | None], ...] = ()

    @property
    def digest(self) -> str:
        return hashlib.sha256(self.data).hexdigest()


@dataclass(frozen=True)
class Run:
    """One emulated call with its outcome kept apart from any value.

    `result` is set for RETURNED, and for KERNEL_CALL (status 0, no data, the ordinal in
    `result.kernel_call`). It is None for every other outcome, so a caller cannot read an
    intermediate EAX or a half-published buffer by accident. `instructions` is only
    filled when the run was asked to count them.
    """

    outcome: Outcome
    result: Assembled | None = None
    fault: FaultInfo | None = None
    raised: RaisedException | None = None
    instructions: int | None = None
    #: The exception dispatch steps taken, only when the run asked for dispatch (T446).
    seh: tuple[SehEvent, ...] = ()
    #: Why dispatch ended the run, for `Outcome.UNHANDLED`.
    seh_stop: str | None = None
    #: EIP and ESP when emulation stopped, for diagnosis only.
    eip: int = 0
    esp: int = 0
    #: Non-zero bytes a RETURNED run left past the requested size of its heap blocks (T485),
    #: only measured when `run` was asked to `track_allocations` on a zero-fill heap, else 0.
    overrun_bytes: int = 0

    @property
    def returned(self) -> bool:
        return self.outcome is Outcome.RETURNED


def _descriptor(base: int, limit: int, access: int, flags: int) -> bytes:
    return struct.pack(
        "<HHBBBB",
        limit & 0xFFFF,
        base & 0xFFFF,
        (base >> 16) & 0xFF,
        access,
        ((limit >> 16) & 0xF) | (flags << 4),
        (base >> 24) & 0xFF,
    )


def _bump_allocator(
    fill: int | None = None, *, heap_limit: int | None = None, fail_at: int | None = None
) -> bytes:
    """`RtlAllocateHeap(heap, flags, size)` as a bump allocator, stdcall, 3 arguments.

        mov eax, [BUMP] ; mov ecx, [esp+0xc] ; add ecx, 0x1f ; and ecx, -16
        add [BUMP], ecx ; ret 0xc

    The heap is a fresh mapping, so it is already zero and the zero-memory flag is met.

    With `fill` (T387) a block whose caller did NOT pass HEAP_ZERO_MEMORY (flag 8) is first
    filled with that byte, as a used title heap would hand back stale memory. A result that
    changes with the fill depends on uninitialised heap, see `verify.heap_dependent`.

    `heap_limit` caps the total bump allocation and returns NULL if the next block would
    cross that cap. `fail_at` instead returns NULL on one one-based allocation ordinal, so
    a measured native allocation failure can be repeated at the same request.
    """
    if heap_limit is not None and not 0 <= heap_limit < HEAP_SIZE - 0x100:
        raise ValueError(f"heap_limit must be in [0, {HEAP_SIZE - 0x101}]")
    if fail_at is not None and fail_at < 1:
        raise ValueError("fail_at is a one-based allocation ordinal")
    if heap_limit is not None and fail_at is not None:
        raise ValueError("choose a heap_limit or one fail_at ordinal, not both")
    slot = struct.pack("<I", BUMP_SLOT)
    head = bytearray(b"\xa1" + slot + b"\x8b\x4c\x24\x0c\x83\xc1\x1f\x83\xe1\xf0")
    fail_branches: list[int] = []
    if heap_limit is not None or fail_at is not None:
        count_slot = struct.pack("<I", ALLOC_COUNT_SLOT)
        head.extend(b"\xff\x05" + count_slot)  # inc dword ptr [count]
        if fail_at is not None:
            head.extend(b"\x81\x3d" + count_slot + struct.pack("<I", fail_at))
            fail_branches.append(len(head))
            head.extend(b"\x0f\x84\0\0\0\0")  # je failure
    if heap_limit is not None:
        head.extend(b"\x8d\x14\x08\x81\xfa" + struct.pack("<I", HEAP_BASE + heap_limit))
        fail_branches.append(len(head))
        head.extend(b"\x0f\x87\0\0\0\0")  # ja failure
        head.extend(b"\x89\x15" + slot)  # mov [BUMP], edx
    else:
        head.extend(b"\x01\x0d" + slot)  # add [BUMP], ecx
    stuff = b""
    if fill is not None:
        # test byte [esp+8], 8 ; jnz skip ; push edi ; mov edi, eax ; push eax ;
        # mov al, fill ; rep stosb ; pop eax ; pop edi
        body = b"\x57\x89\xc7\x50\xb0" + bytes([fill & 0xFF]) + b"\xf3\xaa\x58\x5f"
        stuff = b"\xf6\x44\x24\x08\x08\x75" + bytes([len(body)]) + body
    head.extend(stuff)
    head.extend(b"\xc2\x0c\x00")
    if fail_branches:
        failure = len(head)
        head.extend(b"\x31\xc0\xc2\x0c\x00")  # return NULL
        for branch in fail_branches:
            displacement = failure - (branch + 6)
            head[branch + 2 : branch + 6] = struct.pack("<i", displacement)
    return bytes(head)


def _block_size(request: int) -> int:
    """The bytes the bump allocator advances for a request: `(size + 0x1f) & -16`."""
    return (request + 0x1F) & ~0xF


#: `xor eax, eax ; inc eax ; ret 0xc` -- `RtlFreeHeap` reporting success.
_FREE_STUB = b"\x31\xc0\x40\xc2\x0c\x00"


class AssemblerEmulator:
    """One Unicorn instance able to run the assembler repeatedly."""

    def __init__(
        self,
        image: Image,
        spec: AssemblerSpec,
        *,
        heap_fill: int | None = None,
        heap_limit: int | None = None,
        heap_fail_at: int | None = None,
    ) -> None:
        self.spec = spec
        self.heap_fill = heap_fill
        self.heap_limit = heap_limit
        self.heap_fail_at = heap_fail_at
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        size = (image.xbe.size_of_image + 0xFFFF) & ~0xFFFF
        uc.mem_map(image.xbe.base_address, size)
        # The mapped image, section by section, kept so every assembly can start from it.
        # The assembler keeps state in static data, and a second run on the state a first
        # one left behind faults, so each run rewrites the whole image.
        pristine = bytearray(size)
        for section in image.xbe.sections:
            body = image.raw[section.raw_addr : section.raw_addr + section.raw_size]
            offset = section.virtual_addr - image.xbe.base_address
            pristine[offset : offset + len(body)] = body
        for address, stub in (
            (
                spec.heap_alloc,
                _bump_allocator(heap_fill, heap_limit=heap_limit, fail_at=heap_fail_at),
            ),
            (spec.heap_free, _FREE_STUB),
        ):
            offset = address - image.xbe.base_address
            pristine[offset : offset + len(stub)] = stub
        self._pristine = bytes(pristine)
        self._base = image.xbe.base_address
        for base, length in (
            (STACK_BASE, STACK_SIZE),
            (HEAP_BASE, HEAP_SIZE),
            (SOURCE_BASE, SOURCE_SIZE),
            (TIB_BASE, 0x2000),
            (GDT_BASE, 0x1000),
            (SENTINEL & ~0xFFF, 0x1000),
            (GUARD_BASE, GUARD_SIZE),
        ):
            uc.mem_map(base, length)
        self._install_segments(uc)
        self._uc = uc
        self._heap_used = 0
        self.heap_allocations = 0
        self._unclean = False
        self._last_fault: FaultInfo | None = None
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)
        # Only reached when a dispatching run called a guest handler, see `seh.Dispatcher`.
        uc.hook_add(
            UC_HOOK_CODE,
            lambda stopped, _a, _s, _u: stopped.emu_stop(),
            begin=DISPATCH_RETURN,
            end=DISPATCH_RETURN,
        )
        self._seh = Dispatcher(uc, TIB_BASE, STACK_BASE, STACK_BASE + STACK_SIZE, DISPATCH_RETURN)
        if spec.static_init is not None:
            self._bake_static_init(spec.static_init)

    def _bake_static_init(self, address: int) -> None:
        """Run a no-argument runtime routine once and keep what it wrote to the image."""
        uc = self._uc
        uc.mem_write(self._base, self._pristine)
        esp = STACK_BASE + STACK_SIZE - 0x2000
        uc.mem_write(esp, struct.pack("<I", SENTINEL))
        for register in _CLEARED_REGISTERS:
            uc.reg_write(register, 0)
        uc.reg_write(UC_X86_REG_EFLAGS, 2)
        uc.reg_write(UC_X86_REG_ESP, esp)
        try:
            uc.emu_start(address, SENTINEL, count=STATIC_INIT_BUDGET)
        except UcError as error:
            raise RuntimeError(f"static init {address:#x} faulted: {error}") from error
        if uc.reg_read(UC_X86_REG_EIP) != SENTINEL or uc.reg_read(UC_X86_REG_ESP) != esp + 4:
            raise RuntimeError(f"static init {address:#x} did not return cleanly")
        self._pristine = bytes(uc.mem_read(self._base, len(self._pristine)))
        uc.mem_write(STACK_BASE, bytes(STACK_SIZE))

    #: Unicorn access types for the unmapped-memory hook (UC_MEM_*_UNMAPPED).
    _ACCESS = {
        unicorn_const.UC_MEM_READ_UNMAPPED: "read",
        unicorn_const.UC_MEM_WRITE_UNMAPPED: "write",
        unicorn_const.UC_MEM_FETCH_UNMAPPED: "fetch",
    }

    def _on_unmapped(
        self, uc: Uc, access: int, address: int, size: int, value: int, user: object
    ) -> bool:
        """Remember the address of the access that is about to fault. Does not map it."""
        self._last_fault = FaultInfo(
            "unmapped", uc.reg_read(UC_X86_REG_EIP), address, self._ACCESS.get(access)
        )
        return False

    @staticmethod
    def _reset_tib(uc: Uc) -> None:
        """Clear the thread block, so `fs:[0]` is the end of the exception chain again.

        A run that stopped inside the compiler leaves its frames registered there.
        """
        uc.mem_write(TIB_BASE, bytes(0x2000))
        uc.mem_write(TIB_BASE, struct.pack("<III", 0xFFFFFFFF, STACK_BASE + STACK_SIZE, STACK_BASE))
        uc.mem_write(TIB_BASE + 0x20, struct.pack("<I", TIB_BASE + PRCB_OFFSET))

    @classmethod
    def _install_segments(cls, uc: Uc) -> None:
        cls._reset_tib(uc)
        table = (
            _descriptor(0, 0, 0, 0)
            + _descriptor(0, 0xFFFFF, 0x9B, 0xC)
            + _descriptor(0, 0xFFFFF, 0x93, 0xC)
            + _descriptor(TIB_BASE, 0xFFF, 0x93, 0x4)
        )
        uc.mem_write(GDT_BASE, table)
        uc.reg_write(UC_X86_REG_GDTR, (0, GDT_BASE, len(table) - 1, 0))
        for register, selector in (
            (UC_X86_REG_CS, 0x08),
            (UC_X86_REG_DS, 0x10),
            (UC_X86_REG_ES, 0x10),
            (UC_X86_REG_SS, 0x10),
            (UC_X86_REG_FS, 0x18),
        ):
            uc.reg_write(register, selector)

    def _read_u32(self, address: int) -> int:
        return int.from_bytes(self._uc.mem_read(address, 4), "little")

    def assemble(self, source: bytes, flags: int) -> Assembled | None:
        """Assemble `source`. None for a fault other than a kernel call, an
        exhausted instruction budget, or an incomplete stdcall return.

        Use `run` when the reason for None matters."""
        return self.run(source, flags).result

    def _raised_exception(self, esp: int) -> RaisedException:
        """Read the `RtlRaiseException` argument and the guest handler chain at the stop."""
        record = None
        try:
            pointer = self._read_u32(esp + 4)
            head = self._uc.mem_read(pointer, 20)
            code, flags, _, address, count = struct.unpack("<5I", bytes(head))
            count = min(count, 15)
            params = tuple(self._read_u32(pointer + 20 + 4 * index) for index in range(count))
            record = (code, flags, address, params)
        except UcError:
            pass
        handlers: list[int] = []
        try:
            node = self._read_u32(TIB_BASE)
            while node != 0xFFFFFFFF and len(handlers) < 32:
                handlers.append(self._read_u32(node + 4))
                node = self._read_u32(node)
        except UcError:
            pass
        if record is None:
            return RaisedException(handlers=tuple(handlers))
        return RaisedException(record[0], record[1], record[2], record[3], tuple(handlers))

    def run(
        self,
        source: bytes,
        flags: int,
        *,
        flush_to_guard: bool = False,
        count_instructions: bool = False,
        budget: int | None = None,
        output_args: Sequence[int] = (),
        dispatch_exceptions: bool = False,
        track_allocations: bool = False,
    ) -> Run:
        """Run the assembler once and say how the call ended.

        `output_args` lists stack argument indexes (0 is the first argument after the return
        address) that get a pointer to a zeroed slot instead of NULL. The XGBuffer each one
        receives is read back into `Assembled.extras` whether or not the HRESULT is zero.

        `flush_to_guard` places the source so its last byte is the last mapped byte of
        the region, making any read past the source end an unmapped fault. By default the
        source sits in a zero-filled mapping, where an overread reads zeros. Comparing
        the two reveals reads past the end of the input.
        `count_instructions` fills `Run.instructions` through a per-instruction hook, which
        is slow. `budget` overrides the module instruction budget for this call.
        `dispatch_exceptions` models `RtlRaiseException` and `RtlUnwind` (`seh.Dispatcher`),
        so a guest `catch` runs and the call can return. Off by default, because the
        production host has no dispatch and a comparison must not assume one (T446). The
        budget then applies to each emulation segment between dispatch steps.
        `track_allocations` records every block the bump allocator hands out and, on a
        zero-fill heap, counts the non-zero bytes the run left past each requested size and
        past the last block (`Run.overrun_bytes`, T485). The bump allocator rounds each
        request up with at least 16 spare bytes and never reuses a block, so a write past
        the end of a block is harmless here where the title's real heap would put a block
        header there. A write into a LATER block cannot be told from a legitimate one.
        """
        self._seh.reset()
        self.heap_allocations = 0
        outcome = self._run(
            source,
            flags,
            flush_to_guard,
            count_instructions,
            budget,
            tuple(output_args),
            dispatch_exceptions,
            track_allocations,
        )
        self._unclean = outcome.outcome is not Outcome.RETURNED
        return replace(outcome, seh=tuple(self._seh.events))

    def _drive(self, limit: int, dispatch: bool) -> SehStop | None:
        """Emulate until the sentinel, continuing through exception dispatch when asked.

        A UcError the dispatcher does not own is re-raised for `_run` to classify."""
        uc = self._uc
        start = self.spec.entry
        for _ in range(MAX_SEH_SEGMENTS if dispatch else 1):
            try:
                uc.emu_start(start, SENTINEL, count=limit)
            except UcError:
                stopped_at = uc.reg_read(UC_X86_REG_EIP)
                if not dispatch or not KERNEL_THUNK_FLOOR <= stopped_at < KERNEL_THUNK_CEILING:
                    raise
                step = self._seh.on_kernel_call(stopped_at - KERNEL_THUNK_FLOOR)
                if step is None:
                    raise
            else:
                if not dispatch or uc.reg_read(UC_X86_REG_EIP) != DISPATCH_RETURN:
                    return None
                step = self._seh.on_return(uc.reg_read(UC_X86_REG_ESP), uc.reg_read(UC_X86_REG_EAX))
            if isinstance(step, SehStop):
                return step
            start = step
        return SehStop("exception dispatch used too many steps")

    def _run(
        self,
        source: bytes,
        flags: int,
        flush_to_guard: bool,
        count_instructions: bool,
        budget: int | None,
        output_args: tuple[int, ...] = (),
        dispatch: bool = False,
        track_allocations: bool = False,
    ) -> Run:
        uc = self._uc
        if len(source) > SOURCE_SIZE // 2:
            return Run(Outcome.REFUSED)
        limit = INSN_LIMIT if budget is None else budget
        if limit <= 0:
            # Unicorn treats a count of 0 as unlimited, which would hang on a runaway.
            raise ValueError("instruction budget must be positive")
        uc.mem_write(self._base, self._pristine)
        # Every run starts from the same memory. A run that did not return can have
        # scribbled anywhere, including over the bump pointer, so it clears all of the heap.
        uc.mem_write(STACK_BASE, bytes(STACK_SIZE))
        self._reset_tib(uc)
        uc.mem_write(SOURCE_BASE, bytes(SOURCE_SIZE))
        uc.mem_write(GUARD_BASE, bytes(GUARD_SIZE))
        used = HEAP_SIZE if self._unclean else min(self._heap_used, HEAP_SIZE)
        if used:
            uc.mem_write(HEAP_BASE, bytes(used))
        self._unclean = False
        uc.mem_write(BUMP_SLOT, struct.pack("<I", HEAP_BASE))
        if self.heap_limit is not None or self.heap_fail_at is not None:
            uc.mem_write(ALLOC_COUNT_SLOT, b"\0" * 4)
        if flush_to_guard:
            source_address = GUARD_BASE + GUARD_SIZE - len(source)
        else:
            source_address = SOURCE_BASE
        uc.mem_write(source_address, source)
        uc.mem_write(OUTPUT_SLOT, b"\0" * 4)
        arguments = [0] * self.spec.argument_count
        arguments[1], arguments[2], arguments[3], arguments[5] = (
            source_address,
            len(source),
            flags,
            OUTPUT_SLOT,
        )
        for index in output_args:
            if not 0 <= index < len(arguments) or index in (1, 2, 3, 5):
                raise ValueError(f"argument {index} cannot be an extra output")
            arguments[index] = EXTRA_SLOT_BASE + 4 * index
        esp = STACK_BASE + STACK_SIZE - 0x2000
        uc.mem_write(
            esp, struct.pack("<I", SENTINEL) + struct.pack(f"<{len(arguments)}I", *arguments)
        )
        for register in _CLEARED_REGISTERS:
            uc.reg_write(register, 0)
        uc.reg_write(UC_X86_REG_EFLAGS, 2)
        uc.reg_write(UC_X86_REG_ESP, esp)
        self._last_fault = None
        counter = [0]
        hook = None
        if count_instructions:

            def count(_uc: Uc, _address: int, _size: int, _user: object) -> None:
                counter[0] += 1

            hook = uc.hook_add(UC_HOOK_CODE, count)
            # Blocks translated before the hook existed would skip it and undercount.
            uc.ctl_flush_tb()
        blocks: list[tuple[int, int]] = []
        alloc_hook = None
        if track_allocations:
            # At the allocator's entry the next block starts at the bump pointer and the request
            # is the third stack argument, whatever else the stub does (fill, limit, fail_at).
            def record(hooked: Uc, _address: int, _size: int, _user: object) -> None:
                esp_now = hooked.reg_read(UC_X86_REG_ESP)
                blocks.append(
                    (
                        self._read_u32(BUMP_SLOT),
                        int.from_bytes(hooked.mem_read(esp_now + 0xC, 4), "little"),
                    )
                )

            alloc_hook = uc.hook_add(
                UC_HOOK_CODE, record, begin=self.spec.heap_alloc, end=self.spec.heap_alloc
            )
            uc.ctl_flush_tb()
        try:
            stop = self._drive(limit, dispatch)
        except UcError as error:
            stopped_at = uc.reg_read(UC_X86_REG_EIP)
            stopped_esp = uc.reg_read(UC_X86_REG_ESP)
            counted = counter[0] if count_instructions else None
            if KERNEL_THUNK_FLOOR <= stopped_at < KERNEL_THUNK_CEILING:
                ordinal = stopped_at - KERNEL_THUNK_FLOOR
                raised = (
                    self._raised_exception(stopped_esp)
                    if ordinal == ORDINAL_RAISE_EXCEPTION
                    else None
                )
                return Run(
                    Outcome.KERNEL_CALL,
                    Assembled(0, b"", ordinal),
                    raised=raised,
                    instructions=counted,
                    eip=stopped_at,
                    esp=stopped_esp,
                )
            seen = self._last_fault or FaultInfo(_error_name(error.errno), stopped_at)
            fault = FaultInfo(
                _error_name(error.errno),
                stopped_at,
                seen.address,
                seen.access,
                None if seen.address is None else _region_of(seen.address),
            )
            return Run(
                Outcome.FAULT, fault=fault, instructions=counted, eip=stopped_at, esp=stopped_esp
            )
        finally:
            if self.heap_limit is not None or self.heap_fail_at is not None:
                self.heap_allocations = self._read_u32(ALLOC_COUNT_SLOT)
            if hook is not None:
                uc.hook_del(hook)
                uc.ctl_flush_tb()
            if alloc_hook is not None:
                uc.hook_del(alloc_hook)
                uc.ctl_flush_tb()
        # The tail is cleared too: an original that overruns its last block writes past the bump
        # pointer, and the next run must still start on a zero heap (T485).
        bump = self._read_u32(BUMP_SLOT)
        self._heap_used = bump - HEAP_BASE + OVERRUN_TAIL if bump >= HEAP_BASE else 0
        counted = counter[0] if count_instructions else None
        stopped_at = uc.reg_read(UC_X86_REG_EIP)
        stopped_esp = uc.reg_read(UC_X86_REG_ESP)
        if stop is not None:
            return Run(
                Outcome.UNHANDLED,
                seh_stop=stop.reason,
                instructions=counted,
                eip=stopped_at,
                esp=stopped_esp,
            )
        where = {"instructions": counted, "eip": stopped_at, "esp": stopped_esp}
        # Unicorn also returns normally when its instruction budget expires.
        # EAX/output may already look plausible while execution is still inside
        # the compiler. Require the actual stdcall return and its stack cleanup.
        if stopped_at != SENTINEL:
            if bytes(uc.mem_read(stopped_at - 1, 1)) == b"\xf4":
                return Run(Outcome.HALTED, **where)
            return Run(Outcome.BUDGET_EXHAUSTED, **where)
        expected_esp = esp + 4 * (self.spec.argument_count + 1)
        if stopped_esp != expected_esp:
            return Run(Outcome.BAD_CLEANUP, **where)
        status = uc.reg_read(UC_X86_REG_EAX)
        extras = tuple(
            (index, self._read_buffer(EXTRA_SLOT_BASE + 4 * index)) for index in output_args
        )
        data = self._read_buffer(OUTPUT_SLOT) if status == 0 else None
        # Only a finished run says anything: a runaway or a stop scribbles anywhere.
        overrun = self._overrun_bytes(blocks) if track_allocations else 0
        return Run(
            Outcome.RETURNED,
            Assembled(status, data or b"", extras=extras),
            overrun_bytes=overrun,
            **where,
        )

    def _overrun_bytes(self, blocks: Sequence[tuple[int, int]]) -> int:
        """Non-zero bytes past each block's requested size, and past the last block.

        The plain zero-fill bump heap only: `heap_fill` fills the whole rounded block, spare
        bytes included, and a limit or `fail_at` allocator can return NULL for a request, so
        none of those can say anything and they report 0.
        """
        if (
            self.heap_fill is not None
            or self.heap_limit is not None
            or self.heap_fail_at is not None
        ):
            return 0
        found = 0
        end = HEAP_BASE
        for pointer, request in blocks:
            block_end = pointer + _block_size(request)
            found += self._nonzero(pointer + request, block_end)
            end = max(end, block_end)
        return found + self._nonzero(end, min(end + OVERRUN_TAIL, HEAP_BASE + HEAP_SIZE - 4))

    def _nonzero(self, start: int, stop: int) -> int:
        if stop <= start:
            return 0
        return sum(1 for byte in self._uc.mem_read(start, stop - start) if byte)

    def _read_buffer(self, slot: int) -> bytes | None:
        """The bytes of the XGBuffer a slot points at: None for a NULL slot, b"" if unreadable.

        An output argument that the assembler used for something that is not an XGBuffer
        points at memory that cannot be read as one, which reads as b"".
        """
        buffer = self._read_u32(slot)
        if buffer == 0:
            return None
        try:
            pointer = self._read_u32(buffer + BUFFER_POINTER_OFFSET)
            length = self._read_u32(buffer + BUFFER_SIZE_OFFSET)
            if pointer == 0 or length > SOURCE_SIZE:
                return b""
            return bytes(self._uc.mem_read(pointer, length))
        except UcError:
            return b""


#: Registers cleared before every run so one run cannot leak its state into the next.
_CLEARED_REGISTERS = (
    UC_X86_REG_EAX,
    UC_X86_REG_EBX,
    UC_X86_REG_ECX,
    UC_X86_REG_EDX,
    UC_X86_REG_ESI,
    UC_X86_REG_EDI,
    UC_X86_REG_EBP,
)

#: Regions the harness maps, for naming where a stray access landed.
_REGIONS = (
    ("stack", STACK_BASE, STACK_SIZE),
    ("heap", HEAP_BASE, HEAP_SIZE),
    ("source", SOURCE_BASE, SOURCE_SIZE),
    ("source_guard", GUARD_BASE, GUARD_SIZE),
)


def _region_of(address: int) -> str:
    """Name an unmapped address by the nearest harness region that ends below it (1 MiB).

    `past_source_guard` is the signature of a read beyond the end of the input when the
    run used `flush_to_guard`. `past_source` is an access off the end of the source and
    output scratch mapping, which is where an overrun of the heap, which sits directly
    below it, ends up.
    """
    if address < 0x10000:
        return "null_page"
    below = [
        (base + size, name)
        for name, base, size in _REGIONS
        if base + size <= address < base + size + 0x100000
    ]
    if below:
        return f"past_{max(below)[1]}"
    for name, base, _ in _REGIONS:
        if base - 0x100000 <= address < base:
            return f"before_{name}"
    return "elsewhere"


def _error_name(errno: int) -> str:
    """`UC_ERR_READ_UNMAPPED` -> `read_unmapped`."""
    for name, value in vars(unicorn_const).items():
        if name.startswith("UC_ERR_") and value == errno:
            return name[len("UC_ERR_") :].lower()
    return f"errno_{errno}"


# ---------------------------------------------------------------------------
# sharded corpus runs

#: Worker state, set in the parent before the fork so children inherit it without pickling
#: the 6 MiB image per task. Each child builds its own emulator on first use.
_SHARD_IMAGE: Image | None = None
_SHARD_SPEC: AssemblerSpec | None = None
_SHARD_ITEMS: Sequence[tuple[bytes, int]] = ()
_SHARD_EMULATOR: AssemblerEmulator | None = None

#: Chunks per worker. Many small contiguous chunks balance the long budget-exhausting runs.
_CHUNKS_PER_JOB = 16


def default_jobs() -> int:
    return max(1, min(os.cpu_count() or 1, 8))


def _shard_chunk(bounds: tuple[int, int]) -> list[Assembled | None]:
    global _SHARD_EMULATOR
    if _SHARD_EMULATOR is None:
        assert _SHARD_IMAGE is not None and _SHARD_SPEC is not None
        _SHARD_EMULATOR = AssemblerEmulator(_SHARD_IMAGE, _SHARD_SPEC)
    start, stop = bounds
    return [_SHARD_EMULATOR.assemble(*_SHARD_ITEMS[index]) for index in range(start, stop)]


def assemble_many(
    image: Image,
    spec: AssemblerSpec,
    items: Sequence[tuple[bytes, int]],
    jobs: int = 1,
    *,
    serial: AssemblerEmulator | None = None,
) -> list[Assembled | None]:
    """`AssemblerEmulator.assemble` over `items`, in input order, across `jobs` processes.

    The items are cut into contiguous chunks, run by independent emulators and
    concatenated in chunk order, so the result equals the serial run (each run starts from
    the pristine image, which the run-order tests pin). `jobs` of 1 runs in-process on
    `serial` (or a new emulator)."""
    global _SHARD_IMAGE, _SHARD_SPEC, _SHARD_ITEMS
    if jobs < 1:
        raise ValueError("jobs must be at least 1")
    if jobs == 1 or len(items) < 2:
        emulator = serial or AssemblerEmulator(image, spec)
        return [emulator.assemble(source, flags) for source, flags in items]
    count = min(len(items), jobs * _CHUNKS_PER_JOB)
    edges = [len(items) * index // count for index in range(count + 1)]
    # Pairwise windows: edges has one more entry than there are chunks, so strict=False is intended.
    bounds = list(zip(edges, edges[1:], strict=False))
    _SHARD_IMAGE, _SHARD_SPEC, _SHARD_ITEMS = image, spec, items
    try:
        with multiprocessing.get_context("fork").Pool(jobs) as pool:
            chunks = pool.map(_shard_chunk, bounds, chunksize=1)
    finally:
        _SHARD_IMAGE, _SHARD_SPEC, _SHARD_ITEMS = None, None, ()
    return [result for chunk in chunks for result in chunk]
