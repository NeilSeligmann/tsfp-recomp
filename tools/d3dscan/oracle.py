# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the retail D3D8 library bytes under an x86 emulator, against a MODEL of the hardware.

WHY THIS EXISTS. D3D8.lib is statically linked into the title and IS the driver, so what the
library does to its own state is a fact about the retail image that no amount of reading
disassembly settles with confidence. `CreateDevice` alone reaches 114 functions and 7,593 static
instructions and executes about 92,000 of them, and the port in `src/gpu/` reproduces some of what
it leaves behind. This module runs the ORIGINAL bytes and reports the result, which makes every
claim of the form "the library leaves X in Y" a measurement instead of a reading, and gives the C
port something to be compared with.

WHAT IT IS NOT. It is not an Xbox emulator and it claims nothing about hardware. The kernel is a
handful of stubs, each documented below with the assumption it makes, and the NV2A is a dictionary
of 32-bit registers with exactly the behaviours the init path needs to terminate:

  - interrupt status registers (offset 0x100 within a unit) clear the bits written to them;
  - PFIFO_CACHE1_STATUS and PFIFO_CACHE0_STATUS read as "empty" (0x10);
  - bit 16 of the register at 0xFD100410 reads as clear, so a spin on it ends;
  - writing the DMA put pointer at 0xFD800040 makes the get pointer follow at 0xFD800044, and ALSO
    completes every fence the stream between the old get and the new put releases (see
    `_gpu_drain`). That is the INSTANTANEOUS GPU the C port assumes (docs/d3d8-usage.md section
    14), not a measurement of hardware: it exists so that a fence wait the library issues can end.

Nothing else is modelled, so an init path that waits on a register not listed here spins until the
instruction budget runs out, and `run` returns without error. That is how a FAILED CreateDevice
shows: its teardown waits for a GPU that does not exist. Callers decide failure from the out
pointer, not from the return value.

THE DISPLAY CAPABILITIES ARE AN INPUT. The original asks the kernel for the AV capability word
(AvSendTVEncoderOption, option 6) and filters its mode table with the answer. The default here is
0x00480104, HDTV with 480p on an NTSC-M console, which is what `tsfp_host --av-pack hdtv` answers.

    python -m tools.d3dscan.oracle IMAGE.xbe --modes
    python -m tools.d3dscan.oracle IMAGE.xbe --changes --flags 0x140
    python -m tools.d3dscan.oracle IMAGE.xbe --av-caps 0x00400101 --flags 0x100 --modes

Needs `unicorn`, a declared dependency. Read-only: it opens the image and prints.
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from tools.kernel_ordinals import KERNEL_ORDINALS
from tools.xbe import XbeSection
from tools.xbe.parser import parse_xbe

# The part of the address space the image occupies, and where everything else is put. All of it is
# below the NV2A window and the contiguous-memory window, which are the two the library touches.
IMAGE_LOW = 0x00010000
IMAGE_HIGH = 0x008B0000
FS_BASE = 0x00900000
SENTINEL = 0x00980000
SCRATCH_BASE = 0x00A00000
SCRATCH_BYTES = 0x1000
STACK_TOP = 0x00C00000
STACK_BYTES = 0x00100000
HEAP_BASE = 0x20000000
HEAP_BYTES = 0x01000000
CONTIGUOUS_BASE = 0x80000000
CONTIGUOUS_BYTES = 0x04000000
MMIO_BASE = 0xFD000000
MMIO_BYTES = 0x01000000
TRAP_BASE = 0xFE000000
TRAP_BYTES = 0x00010000
# Guest objects a differential case builds (texture headers, surfaces the title made). Between the
# stack's low end and the segment tables, so nothing else is mapped there.
ARENA_BASE = 0x00A10000
ARENA_BYTES = 0x00080000

# The D3D section's writable tail and the code before it, which is the state worth diffing.
D3D_LOW = 0x003D3380
D3D_HIGH = 0x003E6454

# The entry points this module drives.
SET_PUSH_BUFFER_SIZE = 0x003D9210
GET_ADAPTER_MODE_COUNT = 0x003D9010
ENUM_ADAPTER_MODES = 0x003D90B0
CREATE_DEVICE = 0x003D9230

# Where the library keeps its device and the heap handle the allocation wrapper uses.
DEVICE_POINTER_SLOT = 0x003E3F58
DEVICE_BASE = 0x003E3F60
ALLOCATION_WRAPPER_HEAP_PATH = 0x0038209D

INSTRUCTION_BUDGET = 50_000_000

KernelHandler = Callable[["Oracle", int], int | None]


class OracleError(RuntimeError):
    """The emulation did something this model does not cover."""


@dataclass(frozen=True)
class Presentation:
    """The 17 dwords of D3DPRESENT_PARAMETERS, defaulting to what the title builds at 0x00023930."""

    width: int = 0x280
    height: int = 0x1E0
    back_buffer_format: int = 6
    back_buffer_count: int = 1
    multisample: int = 0x11
    swap_effect: int = 3
    window: int = 0
    windowed: int = 0
    auto_depth_stencil: int = 1
    depth_format: int = 0x2A
    flags: int = 0x140
    refresh_rate: int = 0x3C
    presentation_interval: int = 1
    buffer_surfaces: tuple[int, int, int] = (0, 0, 0)
    depth_surface: int = 0

    def words(self) -> list[int]:
        return [
            self.width,
            self.height,
            self.back_buffer_format,
            self.back_buffer_count,
            self.multisample,
            self.swap_effect,
            self.window,
            self.windowed,
            self.auto_depth_stencil,
            self.depth_format,
            self.flags,
            self.refresh_rate,
            self.presentation_interval,
            *self.buffer_surfaces,
            self.depth_surface,
        ]


@dataclass
class CreateResult:
    """What one CreateDevice left behind."""

    succeeded: bool
    return_value: int
    mode_count: int
    device: int
    # Every dword of the D3D section that differs from the image: address -> (before, after).
    changes: dict[int, tuple[int, int]] = field(default_factory=dict)
    allocations: list[tuple[int, int, int, int]] = field(default_factory=list)
    kernel_calls: list[str] = field(default_factory=list)


_KERNEL: dict[int, tuple[int, KernelHandler]] = {}


def _kernel(ordinal: int, arguments: int) -> Callable[[KernelHandler], KernelHandler]:
    def register(handler: KernelHandler) -> KernelHandler:
        _KERNEL[ordinal] = (arguments, handler)
        return handler

    return register


def _argument(oracle: Oracle, esp: int, index: int) -> int:
    return oracle.read32(esp + 4 + 4 * index)


class Oracle:
    """The retail image mapped into an emulator, with a kernel model and a register model."""

    def __init__(
        self, xbe: Path, av_capabilities: int = 0x00480104, instant_gpu: bool = False
    ) -> None:
        from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_MODE_32, Uc

        self.av_capabilities = av_capabilities
        # Off by default: a failed CreateDevice's teardown then stops at its GPU wait, which is what
        # the `creation` check has always relied on. The differential cases turn it on.
        self.instant_gpu = instant_gpu
        self.kernel_calls: list[str] = []
        # Every kernel call the library made, with its stdcall arguments as the library pushed them
        # (the stub cannot know the count, so each entry carries the model's declared count).
        self.kernel_log: list[tuple[int, tuple[int, ...]]] = []
        # Fence values the instantaneous GPU completed, in order (method 0x1D70 data words).
        self.fences_completed: list[int] = []
        self.allocations: list[tuple[int, int, int, int]] = []
        self.registers: dict[int, int] = {0xFD003214: 0x10, 0xFD002400: 0x10}
        self._contiguous_next = 0x01000000
        self._heap_next = HEAP_BASE
        self._trap_ordinals: dict[int, int] = {}
        self._function_hooks: dict[int, tuple[int, Callable[[Oracle, int], int | None]]] = {}

        raw = xbe.read_bytes()
        image = parse_xbe(raw)
        emulator = Uc(UC_ARCH_X86, UC_MODE_32)
        self.emulator = emulator
        emulator.mem_map(IMAGE_LOW, IMAGE_HIGH - IMAGE_LOW)
        for section in image.sections:
            self._load_section(raw, section)
        for base, size in (
            (STACK_TOP - STACK_BYTES, STACK_BYTES),
            (FS_BASE, 0x10000),
            (SENTINEL, 0x1000),
            (SCRATCH_BASE, SCRATCH_BYTES),
            (CONTIGUOUS_BASE, CONTIGUOUS_BYTES),
            (HEAP_BASE, HEAP_BYTES),
            (TRAP_BASE, TRAP_BYTES),
            (ARENA_BASE, ARENA_BYTES),
        ):
            emulator.mem_map(base, size)
        emulator.mem_write(SENTINEL, b"\xf4")
        emulator.mmio_map(
            MMIO_BASE, MMIO_BYTES, self._register_read, None, self._register_write, None
        )

        # Kernel imports become traps: each thunk slot points at its own 16-byte cell, and a code
        # hook on the range emulates the call.
        for slot, ordinal in enumerate(image.kernel_import_ordinals):
            trap = TRAP_BASE + 16 * slot
            emulator.mem_write(image.kernel_thunk_addr + 4 * slot, struct.pack("<I", trap))
            self._trap_ordinals[trap] = ordinal
        emulator.hook_add(
            UC_HOOK_CODE,
            self._trap_hook,
            begin=TRAP_BASE,
            end=TRAP_BASE + 16 * len(image.kernel_import_ordinals),
        )
        emulator.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        self._install_segments()
        self.hook_function(ALLOCATION_WRAPPER_HEAP_PATH, 2, _heap_allocate)

    # ------------------------------------------------------------------ memory

    def _load_section(self, raw: bytes, section: XbeSection) -> None:
        if section.raw_size:
            body = raw[section.raw_addr : section.raw_addr + section.raw_size]
            self.emulator.mem_write(section.virtual_addr, body)

    def read32(self, address: int) -> int:
        return struct.unpack("<I", bytes(self.emulator.mem_read(address, 4)))[0]

    def write32(self, address: int, value: int) -> None:
        self.emulator.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))

    def read_bytes(self, address: int, length: int) -> bytes:
        return bytes(self.emulator.mem_read(address, length))

    def write_bytes(self, address: int, data: bytes) -> None:
        self.emulator.mem_write(address, data)

    # ------------------------------------------------------------------ the segments

    def _install_segments(self) -> None:
        """Give the guest an FS segment: the library reads KPCR.Prcb at fs:[0x20]."""
        from unicorn import UcError
        from unicorn.x86_const import (
            UC_X86_REG_CS,
            UC_X86_REG_DS,
            UC_X86_REG_ES,
            UC_X86_REG_FS,
            UC_X86_REG_GDTR,
            UC_X86_REG_SS,
        )

        def entry(base: int, limit: int, access: int, flags: int) -> bytes:
            return struct.pack(
                "<HHBBBB",
                limit & 0xFFFF,
                base & 0xFFFF,
                (base >> 16) & 0xFF,
                access,
                ((limit >> 16) & 0xF) | (flags << 4),
                (base >> 24) & 0xFF,
            )

        table = (
            entry(0, 0, 0, 0)
            + entry(0, 0xFFFFF, 0x9A, 0xC)
            + entry(0, 0xFFFFF, 0x92, 0xC)
            + entry(FS_BASE, 0xFFF, 0x92, 0xC)
            + entry(0, 0xFFFFF, 0xFA, 0xC)
        )
        table_base = FS_BASE + 0x8000
        self.emulator.mem_write(table_base, table)
        try:
            self.emulator.reg_write(UC_X86_REG_GDTR, (0, table_base, len(table) - 1, 0))
        except UcError as error:
            raise OracleError(f"cannot install a GDT: {error}") from error
        self.emulator.reg_write(UC_X86_REG_FS, 3 << 3)
        self.emulator.reg_write(UC_X86_REG_DS, 2 << 3)
        self.emulator.reg_write(UC_X86_REG_ES, 2 << 3)
        self.emulator.reg_write(UC_X86_REG_SS, 2 << 3)
        self.emulator.reg_write(UC_X86_REG_CS, 1 << 3)
        self.emulator.mem_write(FS_BASE + 0x20, struct.pack("<I", FS_BASE + 0x1000))
        self.emulator.mem_write(FS_BASE + 0x28, struct.pack("<I", FS_BASE + 0x2000))

    # ------------------------------------------------------------------ registers

    def _register_read(self, _emulator: object, offset: int, size: int, _user: object) -> int:
        address = MMIO_BASE + offset
        value = self.registers.get(address & ~3, 0)
        if address & ~3 == 0xFD100410:
            value &= ~0x10000
        return (value >> ((address & 3) * 8)) & ((1 << (8 * size)) - 1)

    def _register_write(
        self, _emulator: object, offset: int, size: int, value: int, _user: object
    ) -> None:
        address = MMIO_BASE + offset
        base = address & ~3
        current = self.registers.get(base, 0)
        shift = (address & 3) * 8
        mask = ((1 << (8 * size)) - 1) << shift
        updated = (current & ~mask) | ((value << shift) & mask)
        if base & 0xFFF == 0x100 and size == 4:
            updated = current & ~value  # write-one-to-clear interrupt status
        self.registers[base] = updated & 0xFFFFFFFF
        if base == 0xFD800040:
            if self.instant_gpu:
                self._gpu_drain(self.registers.get(0xFD800044, 0), updated & 0xFFFFFFFF)
            self.registers[0xFD800044] = updated & 0xFFFFFFFF

    def _gpu_drain(self, get: int, put: int) -> None:
        """Consume the pushbuffer from `get` to `put` instantly, completing the fences it carries.

        Only what a fence wait needs is interpreted: the walk follows the ring-wrap jumps the
        library writes and finds BACK_END_WRITE_SEMAPHORE_RELEASE (method 0x1D70), whose data is
        stored where the library polls (the dword `device+0x30` points at).
        """
        semaphore = self.read32(DEVICE_BASE + 0x30)
        position = get
        steps = 0
        while position != put and steps < 1_000_000:
            steps += 1
            word = self.read32(CONTIGUOUS_BASE | position)
            if word & 3 == 1:  # new-style jump, the ring wrap
                position = word & 0x0FFFFFFC
                continue
            if word & 0xE0000003 == 0x20000000:  # old-style jump
                position = word & 0x1FFFFFFC
                continue
            method = word & 0x1FFC
            count = (word >> 18) & 0x7FF
            increasing = not word & 0x40000000
            for index in range(count):
                data = self.read32(CONTIGUOUS_BASE | ((position + 4 + 4 * index) & 0x0FFFFFFF))
                if (method + (4 * index if increasing else 0)) == 0x1D70 and semaphore:
                    self.write32(semaphore, data)
                    self.fences_completed.append(data)
            position = (position + 4 + 4 * count) & 0x0FFFFFFF

    # ------------------------------------------------------------------ kernel and hooks

    def hook_function(
        self, address: int, arguments: int, handler: Callable[[Oracle, int], int | None]
    ) -> None:
        """Replace the stdcall function at `address` with a Python body."""
        from unicorn import UC_HOOK_CODE

        self._function_hooks[address] = (arguments, handler)
        self.emulator.hook_add(UC_HOOK_CODE, self._function_hook, begin=address, end=address)

    def _return_from(self, esp: int, arguments: int, result: int | None) -> None:
        from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP

        caller = self.read32(esp)
        self.emulator.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * arguments)
        self.emulator.reg_write(UC_X86_REG_EIP, caller)
        if result is not None:
            self.emulator.reg_write(UC_X86_REG_EAX, result & 0xFFFFFFFF)

    def _function_hook(self, _emulator: object, address: int, _size: int, _user: object) -> None:
        from unicorn.x86_const import UC_X86_REG_ESP

        arguments, handler = self._function_hooks[address]
        esp = self.emulator.reg_read(UC_X86_REG_ESP)
        self._return_from(esp, arguments, handler(self, esp))

    def _trap_hook(self, _emulator: object, address: int, _size: int, _user: object) -> None:
        from unicorn.x86_const import UC_X86_REG_ESP

        ordinal = self._trap_ordinals.get(address & ~0xF)
        if ordinal is None:
            return
        name = KERNEL_ORDINALS.get(ordinal, "?")
        entry = _KERNEL.get(ordinal)
        esp = self.emulator.reg_read(UC_X86_REG_ESP)
        if entry is None:
            raise OracleError(
                f"kernel ordinal {ordinal} {name} has no model (called from {self.read32(esp):#x})"
            )
        arguments, handler = entry
        self.kernel_calls.append(f"{name}({ordinal})")
        self.kernel_log.append((ordinal, tuple(_argument(self, esp, i) for i in range(arguments))))
        self._return_from(esp, arguments, handler(self, esp))

    def _unmapped(
        self, _emulator: object, access: int, address: int, _size: int, _value: int, _user: object
    ) -> bool:
        from unicorn.x86_const import UC_X86_REG_EIP

        eip = self.emulator.reg_read(UC_X86_REG_EIP)
        raise OracleError(f"unmapped access {access} at {address:#010x} from {eip:#010x}")

    # ------------------------------------------------------------------ running

    def run(
        self,
        address: int,
        arguments: Sequence[int] = (),
        ecx: int | None = None,
        edx: int | None = None,
        budget: int = INSTRUCTION_BUDGET,
        eax: int | None = None,
        esi: int | None = None,
    ) -> int:
        """Call the stdcall function at `address` and return eax.

        Stops without error when the budget runs out, which is how a spin on a register this model
        does not provide ends. The return value is then whatever eax held.
        """
        from unicorn import UcError
        from unicorn.x86_const import (
            UC_X86_REG_EAX,
            UC_X86_REG_EBP,
            UC_X86_REG_ECX,
            UC_X86_REG_EDX,
            UC_X86_REG_EIP,
            UC_X86_REG_ESI,
            UC_X86_REG_ESP,
        )

        esp = STACK_TOP - 0x1000
        for value in reversed(arguments):
            esp -= 4
            self.write32(esp, value)
        esp -= 4
        self.write32(esp, SENTINEL)
        self.emulator.reg_write(UC_X86_REG_ESP, esp)
        self.emulator.reg_write(UC_X86_REG_EBP, 0)
        if ecx is not None:
            self.emulator.reg_write(UC_X86_REG_ECX, ecx)
        if edx is not None:
            self.emulator.reg_write(UC_X86_REG_EDX, edx)
        if eax is not None:
            self.emulator.reg_write(UC_X86_REG_EAX, eax)
        if esi is not None:
            self.emulator.reg_write(UC_X86_REG_ESI, esi)
        try:
            self.emulator.emu_start(address, SENTINEL, count=budget)
        except UcError as error:
            eip = self.emulator.reg_read(UC_X86_REG_EIP)
            raise OracleError(f"emulation failed at {eip:#010x}: {error}") from error
        return int(self.emulator.reg_read(UC_X86_REG_EAX))


# --------------------------------------------------------------------------- the kernel model


def _heap_allocate(oracle: Oracle, esp: int) -> int:
    """The title's process heap, behind 0x0038209D, as a bump allocator."""
    size = _argument(oracle, esp, 0)
    address = (oracle._heap_next + 15) & ~15
    oracle._heap_next = address + ((size + 15) & ~15)
    oracle.allocations.append((size, 0, 0, address))
    return address


@_kernel(166, 5)
def _mm_allocate_contiguous_ex(oracle: Oracle, esp: int) -> int:
    """Contiguous memory at virtual 0x80000000 | physical, as on a console."""
    size, _low, _high, alignment, protect = (_argument(oracle, esp, i) for i in range(5))
    alignment = alignment or 0x1000
    physical = (oracle._contiguous_next + alignment - 1) & ~(alignment - 1)
    oracle._contiguous_next = physical + ((size + 0xFFF) & ~0xFFF)
    address = CONTIGUOUS_BASE | physical
    oracle.allocations.append((size, alignment, protect, address))
    return address


@_kernel(2, 4)
def _av_send_tv_encoder_option(oracle: Oracle, esp: int) -> int:
    """Option 6 answers the capability word; 0xF and 0x10 answer 0 and 1; the rest do nothing."""
    _base, option, _param, result = (_argument(oracle, esp, i) for i in range(4))
    if option == 6:
        oracle.write32(result, oracle.av_capabilities)
    elif option == 0xF:
        oracle.write32(result, 0)
    elif option == 0x10:
        oracle.write32(result, 1)
    return 0


@_kernel(107, 3)
def _ke_initialize_dpc(_oracle: Oracle, _esp: int) -> None:
    return None


@_kernel(44, 2)
def _hal_get_interrupt_vector(_oracle: Oracle, _esp: int) -> int:
    return 0x1B


@_kernel(109, 7)
def _ke_initialize_interrupt(_oracle: Oracle, _esp: int) -> None:
    return None


@_kernel(98, 1)
def _ke_connect_interrupt(_oracle: Oracle, _esp: int) -> int:
    return 1


@_kernel(47, 2)
def _hal_register_shutdown_notification(_oracle: Oracle, _esp: int) -> None:
    return None


@_kernel(46, 6)
def _hal_read_write_pci_space(oracle: Oracle, esp: int) -> int:
    """Configuration space reads as zero."""
    _bus, _slot, _register, buffer, length, write = (_argument(oracle, esp, i) for i in range(6))
    if not write:
        oracle.write_bytes(buffer, b"\x00" * length)
    return length


@_kernel(168, 2)
def _mm_claim_gpu_instance_memory(oracle: Oracle, esp: int) -> int:
    """The padding the library adds to RAMIN offsets, and a base address for the claim."""
    oracle.write32(_argument(oracle, esp, 1), 0x1000)
    return 0x03FF5000


@_kernel(1, 0)
def _av_get_saved_data_address(_oracle: Oracle, _esp: int) -> int:
    """No saved data page: the mode set skips its release path."""
    return 0


@_kernel(3, 6)
def _av_set_display_mode(_oracle: Oracle, _esp: int) -> int:
    """Accepted at once (step 0), as the host's AV module answers."""
    return 0


@_kernel(4, 1)
def _av_set_saved_data_address(_oracle: Oracle, _esp: int) -> None:
    return None


@_kernel(171, 1)
def _mm_free_contiguous_memory(_oracle: Oracle, _esp: int) -> None:
    """Accepted and forgotten: the bump allocator never reuses an address."""
    return None


@_kernel(145, 3)
def _ke_set_event(oracle: Oracle, esp: int) -> int:
    """Sets SignalState (the dword at +4) and returns the previous one."""
    event = _argument(oracle, esp, 0)
    previous = oracle.read32(event + 4)
    oracle.write32(event + 4, 1)
    return previous


@_kernel(159, 5)
def _ke_wait_for_single_object(oracle: Oracle, esp: int) -> int:
    """Every wait is satisfied at once. The model signals nothing by itself, so a wait on an
    event the library cleared is recorded, and the log is how the two sides are compared."""
    del oracle, esp
    return 0


# --------------------------------------------------------------------------- scenarios


def create_device(
    oracle: Oracle,
    presentation: Presentation | None = None,
    push_buffer: tuple[int, int] = (0x100000, 0x10000),
) -> CreateResult:
    """SetPushBufferSize, GetAdapterModeCount and CreateDevice, as InitD3D calls them."""
    presentation = presentation or Presentation()
    parameters = SCRATCH_BASE
    out_pointer = SCRATCH_BASE + 0xFF0
    oracle.write_bytes(parameters, struct.pack("<17I", *presentation.words()))
    oracle.write32(out_pointer, 0xDEADBEEF)
    before = oracle.read_bytes(D3D_LOW, D3D_HIGH - D3D_LOW)

    oracle.run(SET_PUSH_BUFFER_SIZE, list(push_buffer))
    count = oracle.run(GET_ADAPTER_MODE_COUNT, [0])
    value = oracle.run(CREATE_DEVICE, [0, 1, 0, 0, parameters, out_pointer])
    device = oracle.read32(out_pointer)
    after = oracle.read_bytes(D3D_LOW, D3D_HIGH - D3D_LOW)

    changes: dict[int, tuple[int, int]] = {}
    for offset in range(0, len(before), 4):
        old = struct.unpack_from("<I", before, offset)[0]
        new = struct.unpack_from("<I", after, offset)[0]
        if old != new:
            changes[D3D_LOW + offset] = (old, new)
    return CreateResult(
        succeeded=device == DEVICE_BASE,
        return_value=value,
        mode_count=count,
        device=device,
        changes=changes,
        allocations=list(oracle.allocations),
        kernel_calls=list(oracle.kernel_calls),
    )


def list_modes(oracle: Oracle) -> list[tuple[int, int, int, int, int] | None]:
    """Every mode GetAdapterModeCount reports, then one past the end (None).

    Each is (width, height, refresh, flags, format).
    """
    buffer = SCRATCH_BASE + 0x800
    count = oracle.run(GET_ADAPTER_MODE_COUNT, [0])
    modes: list[tuple[int, int, int, int, int] | None] = []
    for index in range(count + 1):
        status = oracle.run(ENUM_ADAPTER_MODES, [0, index, buffer])
        if status != 0:
            modes.append(None)
            continue
        width, height, refresh, flags, form = struct.unpack("<5I", oracle.read_bytes(buffer, 20))
        modes.append((width, height, refresh, flags, form))
    return modes


# --------------------------------------------------------------------------- command line


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.d3dscan.oracle",
        description="Run the retail D3D8 init path under an x86 emulator and report what it does.",
    )
    parser.add_argument("xbe", type=Path, help="path to the retail XBE")
    parser.add_argument(
        "--av-caps",
        type=lambda text: int(text, 0),
        default=0x00480104,
        help="the AV capability word the kernel answers (default 0x00480104, HDTV NA)",
    )
    parser.add_argument(
        "--flags", type=lambda text: int(text, 0), default=0x140, help="presentation flags"
    )
    parser.add_argument("--refresh", type=int, default=60, help="requested refresh rate")
    parser.add_argument("--modes", action="store_true", help="list the modes it enumerates")
    parser.add_argument(
        "--changes", action="store_true", help="list every D3D section dword CreateDevice changed"
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.modes:
        modes = list_modes(Oracle(args.xbe, args.av_caps))
        print(f"{len(modes) - 1} modes, capability word {args.av_caps:#010x}")
        for index, mode in enumerate(modes):
            print(f"  {index:3d}  {mode if mode is not None else 'end of list (0x8876086C)'}")
    presentation = Presentation(flags=args.flags, refresh_rate=args.refresh)
    result = create_device(Oracle(args.xbe, args.av_caps), presentation)
    verdict = "succeeded" if result.succeeded else "FAILED (teardown reached the register model)"
    print(f"CreateDevice {verdict}; {result.mode_count} modes; device {result.device:#010x}")
    print(f"kernel calls: {', '.join(result.kernel_calls)}")
    for size, alignment, protect, address in result.allocations:
        print(
            f"allocation: size {size:#x} alignment {alignment:#x} protect {protect:#x}"
            f" -> {address:#010x}"
        )
    if args.changes:
        print(f"{len(result.changes)} dwords changed:")
        for address, (old, new) in sorted(result.changes.items()):
            print(f"  {address:#010x}  {old:08x} -> {new:08x}")
    return 0 if result.succeeded else 1


if __name__ == "__main__":
    sys.exit(main())
