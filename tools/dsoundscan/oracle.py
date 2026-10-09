# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the retail DirectSound library bytes under an x86 emulator, against a MODEL of the hardware.

DSOUND is statically linked into the title, so what DirectSoundCreate and its siblings do to their
objects is a fact about the retail image. This runs the ORIGINAL bytes (the whole image is mapped at
its own addresses, so the library's tables, vtables and the game's own C runtime are all real) and
reports the result, which gives the C port in `src/audio/` something to be compared with.

WHAT IT IS NOT. Not an Xbox emulator and no claim about hardware. The kernel is a handful of stubs,
each declared with the assumption it makes, and the audio hardware is a dictionary of 32-bit
registers with exactly the behaviours the init path needs to terminate (see `HardwareModel`). Any
kernel ordinal the library calls that has no stub raises `OracleError` naming it, so the set of
ordinals the init needs is a measurement. Every stub is recorded in
`Oracle.kernel_log` in call order.

    python -m tools.dsoundscan.oracle IMAGE.xbe --create

Needs `unicorn`, a declared dependency. Read-only: it opens the image and prints.
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from tools.dsoundscan.dsp_mailbox import POLL_DOWNLOAD, DspMailbox
from tools.dsoundscan.image import Image
from tools.kernel_ordinals import KERNEL_ORDINALS

IMAGE_LOW = 0x00010000
IMAGE_HIGH = 0x008B0000
FS_BASE = 0x00900000
SENTINEL = 0x00980000
SCRATCH_BASE = 0x00A00000
SCRATCH_BYTES = 0x00010000
STACK_TOP = 0x00C00000
STACK_BYTES = 0x00100000
HEAP_BASE = 0x20000000
HEAP_BYTES = 0x01000000
CONTIGUOUS_BASE = 0x80000000
CONTIGUOUS_BYTES = 0x04000000
APU_BASE = 0xFE800000
APU_BYTES = 0x00100000
AC97_BASE = 0xFEC00000
AC97_BYTES = 0x00001000
TRAP_BASE = 0xFE000000
TRAP_BYTES = 0x00010000

INSTRUCTION_BUDGET = 20_000_000

# The library's own entry points.
DIRECT_SOUND_CREATE = 0x00409635
USE_LIGHT_HRTF = 0x00406AB6
DOWNLOAD_EFFECTS_IMAGE = 0x004079DB
CREATE_STREAM = 0x0040967C
CREATE_SOUND_BUFFER = 0x004093C8
RELEASE = 0x00406A8A

# Where the library keeps its device singleton.
DEVICE_SINGLETON = 0x00412B30

# The CRT's C-initializer table. The library's own entries (pool accounting counters and the like)
# are routines in the DSOUND section, run by the title's CRT before main.
STATIC_INITIALIZER_TABLE = (0x004B82CC, 0x004B8548)

# Kernel ordinals that are __fastcall: the argument is in ecx and the return pops nothing.
FASTCALL_ORDINALS = frozenset({160, 161})

# Register values the model presets so a measured polling loop ends. Each is an ASSUMPTION about
# hardware this repository has no measurement of, named with the loop that needs it.
POWER_ON_REGISTERS: dict[int, int] = {
    # 0x0040F81C spins until (value & ~3) >= 0x20: a free-space count the model reports as 0x100.
    0xFE820010: 0x100,
}

# The AC97 global status register and its primary-codec-ready bit: 0x0041047A polls it up to 1000
# times, 20 microseconds apart. The controller's semaphore register (0xFEC00134) reads clear.
AC97_GLOBAL_STATUS = 0xFEC00130
AC97_CODEC_READY = 0x100

# Bus-master control bytes (PCM in, PCM out, S/PDIF out). Writing bit 1 resets the channel's
# registers and the library polls until it reads clear (0x00410765), so the model clears it at once.
AC97_BUS_MASTER_CONTROL = frozenset({0xFEC0010B, 0xFEC0011B, 0xFEC0017B})
AC97_RESET_REGISTERS = 0x02

# 0x0040A22C: `cmp [ebx], 0 / jne` with ebx = DSP scratch + 0x810, after the library stored
# command 3. The second mailbox loop (0x0040A28F) and the protocol live in dsp_mailbox.py.
DSP_COMMAND_SPIN = POLL_DOWNLOAD

KernelHandler = Callable[["Oracle", Sequence[int], int, int], int | None]


class OracleError(RuntimeError):
    """The emulation did something this model does not cover."""


class OracleBudgetError(OracleError):
    """The instruction budget ran out: almost always a spin on a register the model lacks."""


@dataclass
class HardwareModel:
    """The audio hardware as a table of registers plus the few behaviours init needs to finish.

    `registers` holds what the library wrote (and what the model preloads). `reads` and `writes`
    keep a bounded access log, so a spin that never ends can be diagnosed from its last accesses.
    Behaviours are added only when a measured loop demands one, each with the loop it ends.
    """

    registers: dict[int, int] = field(default_factory=dict)
    reads: list[tuple[int, int, int]] = field(default_factory=list)
    writes: list[tuple[int, int, int]] = field(default_factory=list)
    log_limit: int = 20000
    # AC97 GLOB_STA (0xFEC00130) bit 8, primary codec ready. The host default is NOT ready.
    codec_ready: bool = False


_KERNEL: dict[int, tuple[int, KernelHandler]] = {}


def kernel(ordinal: int, arguments: int) -> Callable[[KernelHandler], KernelHandler]:
    def register(handler: KernelHandler) -> KernelHandler:
        _KERNEL[ordinal] = (arguments, handler)
        return handler

    return register


class Oracle:
    """The retail image mapped into an emulator, with a kernel model and a register model."""

    def __init__(self, xbe: Path) -> None:
        from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_MODE_32, Uc

        self.image = Image(xbe)
        self.kernel_log: list[tuple[int, str, tuple[int, ...]]] = []
        self.hardware = HardwareModel()
        self.hardware.registers.update(POWER_ON_REGISTERS)
        self.allocations: dict[int, tuple[int, str]] = {}
        self.free_log: list[int] = []
        self.irql = 0
        self.clock = 0
        self._heap_next = HEAP_BASE
        self._contiguous_next = 0x01000000
        self._trap_ordinals: dict[int, int] = {}
        self.stalled_microseconds = 0
        self.fault: tuple[int, int, int] | None = None
        # EEPROM settings by ValueIndex, as ExQueryNonVolatileSetting answers them. Unset is zero.
        self.settings: dict[int, int] = {}
        self.dsp_mailbox: DspMailbox | None = None

        emulator = Uc(UC_ARCH_X86, UC_MODE_32)
        self.emulator = emulator
        emulator.mem_map(IMAGE_LOW, IMAGE_HIGH - IMAGE_LOW)
        emulator.mem_write(
            self.image.xbe.base_address, self.image.raw[: self.image.xbe.size_of_headers]
        )
        for section in self.image.sections:
            if section.raw_size:
                body = self.image.raw[section.raw_addr : section.raw_addr + section.raw_size]
                emulator.mem_write(section.virtual_addr, body)
        for base, size in (
            (STACK_TOP - STACK_BYTES, STACK_BYTES),
            (FS_BASE, 0x10000),
            (SENTINEL, 0x1000),
            (SCRATCH_BASE, SCRATCH_BYTES),
            (CONTIGUOUS_BASE, CONTIGUOUS_BYTES),
            (HEAP_BASE, HEAP_BYTES),
            (TRAP_BASE, TRAP_BYTES),
        ):
            emulator.mem_map(base, size)
        emulator.mem_write(SENTINEL, b"\xf4")
        emulator.mmio_map(APU_BASE, APU_BYTES, self._read, None, self._write, None)
        emulator.mmio_map(AC97_BASE, AC97_BYTES, self._read_ac97, None, self._write_ac97, None)

        ordinals = self.image.xbe.kernel_import_ordinals
        for slot, ordinal in enumerate(ordinals):
            trap = TRAP_BASE + 16 * slot
            emulator.mem_write(self.image.xbe.kernel_thunk_addr + 4 * slot, struct.pack("<I", trap))
            self._trap_ordinals[trap] = ordinal
        emulator.hook_add(
            UC_HOOK_CODE, self._trap_hook, begin=TRAP_BASE, end=TRAP_BASE + 16 * len(ordinals)
        )
        emulator.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        self._install_segments()
        self.install_dsp_acknowledgement()

    # ------------------------------------------------------------------ hooks

    def hook_pc(self, address: int, handler: Callable[[Oracle], None]) -> None:
        """Run `handler` each time the guest is about to execute `address`."""
        from unicorn import UC_HOOK_CODE

        def fire(_emulator: object, _address: int, _size: int, _user: object) -> None:
            handler(self)

        self.emulator.hook_add(UC_HOOK_CODE, fire, begin=address, end=address)

    def register(self, name: str) -> int:
        from unicorn import x86_const

        return int(self.emulator.reg_read(getattr(x86_const, f"UC_X86_REG_{name.upper()}")))

    def install_dsp_acknowledgement(self) -> None:
        """Install the validating DSP mailbox model (tools/dsoundscan/dsp_mailbox.py).

        The library posts a command at scratch + 0x810 and polls until it reads zero (0x0040A22C
        after command 3, 0x0040A28F before command 2). There is no DSP, so at each loop head the
        model validates and clears a pending command 2 or 3 and refuses any other value.
        """
        self.dsp_mailbox = DspMailbox()
        self.dsp_mailbox.install(self)

    # ------------------------------------------------------------------ memory

    def read32(self, address: int) -> int:
        return int(struct.unpack("<I", bytes(self.emulator.mem_read(address, 4)))[0])

    def write32(self, address: int, value: int) -> None:
        self.emulator.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))

    def read_bytes(self, address: int, length: int) -> bytes:
        return bytes(self.emulator.mem_read(address, length))

    def write_bytes(self, address: int, data: bytes) -> None:
        self.emulator.mem_write(address, data)

    # ------------------------------------------------------------------ segments

    def _install_segments(self) -> None:
        """An FS segment: the library reads the IRQL byte at fs:[0x24]."""
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
        self.set_irql(0)

    def set_irql(self, level: int) -> None:
        self.irql = level
        self.emulator.mem_write(FS_BASE + 0x24, bytes([level & 0xFF]))

    # ------------------------------------------------------------------ hardware

    def _read(self, _emulator: object, offset: int, size: int, _user: object) -> int:
        address = APU_BASE + offset
        value = self.hardware.registers.get(address & ~3, 0)
        if len(self.hardware.reads) < self.hardware.log_limit:
            self.hardware.reads.append((address, value, self.eip()))
        return (value >> ((address & 3) * 8)) & ((1 << (8 * size)) - 1)

    def _write(self, _emulator: object, offset: int, size: int, value: int, _user: object) -> None:
        address = APU_BASE + offset
        base = address & ~3
        shift = (address & 3) * 8
        mask = ((1 << (8 * size)) - 1) << shift
        current = self.hardware.registers.get(base, 0)
        self.hardware.registers[base] = (current & ~mask) | ((value << shift) & mask)
        if len(self.hardware.writes) < self.hardware.log_limit:
            self.hardware.writes.append((address, value, self.eip()))

    def _read_ac97(self, _emulator: object, offset: int, size: int, _user: object) -> int:
        address = AC97_BASE + offset
        value = self.hardware.registers.get(address & ~3, 0)
        if address & ~3 == AC97_GLOBAL_STATUS:
            value = (value & ~AC97_CODEC_READY) | (
                AC97_CODEC_READY if self.hardware.codec_ready else 0
            )
        if len(self.hardware.reads) < self.hardware.log_limit:
            self.hardware.reads.append((address, value, self.eip()))
        return (value >> ((address & 3) * 8)) & ((1 << (8 * size)) - 1)

    def _write_ac97(
        self, _emulator: object, offset: int, size: int, value: int, _user: object
    ) -> None:
        address = AC97_BASE + offset
        base = address & ~3
        shift = (address & 3) * 8
        mask = ((1 << (8 * size)) - 1) << shift
        current = self.hardware.registers.get(base, 0)
        updated = (current & ~mask) | ((value << shift) & mask)
        if size == 1 and address in AC97_BUS_MASTER_CONTROL:
            updated &= ~(AC97_RESET_REGISTERS << shift)  # the reset-registers bit self-clears
        self.hardware.registers[base] = updated
        if len(self.hardware.writes) < self.hardware.log_limit:
            self.hardware.writes.append((address, value, self.eip()))

    def eip(self) -> int:
        from unicorn.x86_const import UC_X86_REG_EIP

        return int(self.emulator.reg_read(UC_X86_REG_EIP))

    # ------------------------------------------------------------------ kernel

    def _return_from(self, esp: int, popped: int, result: int | None) -> None:
        from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP

        caller = self.read32(esp)
        self.emulator.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * popped)
        self.emulator.reg_write(UC_X86_REG_EIP, caller)
        if result is not None:
            self.emulator.reg_write(UC_X86_REG_EAX, result & 0xFFFFFFFF)

    def _trap_hook(self, _emulator: object, address: int, _size: int, _user: object) -> None:
        from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESP

        ordinal = self._trap_ordinals.get(address & ~0xF)
        if ordinal is None:
            return
        name = KERNEL_ORDINALS.get(ordinal, "?")
        esp = self.emulator.reg_read(UC_X86_REG_ESP)
        entry = _KERNEL.get(ordinal)
        if entry is None:
            raise OracleError(
                f"kernel ordinal {ordinal} {name} has no model (called from {self.read32(esp):#x})"
            )
        count, handler = entry
        ecx = int(self.emulator.reg_read(UC_X86_REG_ECX))
        edx = int(self.emulator.reg_read(UC_X86_REG_EDX))
        arguments = tuple(self.read32(esp + 4 + 4 * index) for index in range(count))
        if ordinal in FASTCALL_ORDINALS:
            arguments = (ecx, edx)
        self.kernel_log.append((ordinal, name, arguments))
        popped = 0 if ordinal in FASTCALL_ORDINALS else count
        self._return_from(esp, popped, handler(self, arguments, ecx, edx))

    def _unmapped(
        self, _emulator: object, access: int, address: int, _size: int, _value: int, _user: object
    ) -> bool:
        # A Python exception inside a unicorn callback is swallowed, so keep the fault for `run`.
        self.fault = (access, address, self.eip())
        return False

    # ------------------------------------------------------------------ running

    def run(
        self,
        address: int,
        arguments: Sequence[int] = (),
        ecx: int | None = None,
        budget: int = INSTRUCTION_BUDGET,
    ) -> int:
        """Call the stdcall function at `address` and return eax.

        Raises OracleBudgetError if the budget runs out: a spin the model does not end. The
        caller's stack arguments are left in place above the frame, so a function that does not pop
        them (cdecl) can be detected from `last_esp`.
        """
        from unicorn import UcError
        from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_ESP

        esp = STACK_TOP - 0x1000
        for value in reversed(arguments):
            esp -= 4
            self.write32(esp, value)
        esp -= 4
        self.write32(esp, SENTINEL)
        frame = esp
        self.emulator.reg_write(UC_X86_REG_ESP, esp)
        self.emulator.reg_write(UC_X86_REG_EBP, 0)
        if ecx is not None:
            self.emulator.reg_write(UC_X86_REG_ECX, ecx)
        self.instructions_run = 0
        try:
            self.emulator.emu_start(address, SENTINEL, count=budget)
        except UcError as error:
            where = ""
            if self.fault is not None:
                access, address, eip = self.fault
                where = f" (unmapped access {access} at {address:#010x} from {eip:#010x})"
            raise OracleError(f"emulation failed at {self.eip():#010x}: {error}{where}") from error
        if self.eip() != SENTINEL:
            raise OracleBudgetError(
                f"{budget} instructions without returning; stopped at {self.eip():#010x}"
            )
        self.last_esp = int(self.emulator.reg_read(UC_X86_REG_ESP))
        self.last_popped = self.last_esp - frame
        return int(self.emulator.reg_read(UC_X86_REG_EAX))

    last_esp: int = 0
    last_popped: int = 0
    instructions_run: int = 0

    def static_initializers(self) -> list[int]:
        """Return CRT initializer routines inside DSOUND, in table order."""
        low, high = self.image.dsound_range
        start, end = STATIC_INITIALIZER_TABLE
        found = (self.image.read_u32(address) for address in range(start, end, 4))
        return [entry for entry in found if low <= entry < high]

    def initialize_library(self) -> list[int]:
        """Run the library's CRT initializers, as the title's start-up does before main."""
        entries = self.static_initializers()
        for entry in entries:
            self.run(entry)
        return entries


# --------------------------------------------------------------------------- the kernel model


def _pool_allocate(oracle: Oracle, size: int, tag: str) -> int:
    address = (oracle._heap_next + 15) & ~15
    oracle._heap_next = address + ((max(size, 1) + 15) & ~15)
    oracle.allocations[address] = (size, tag)
    return address


@kernel(15, 2)
def _ex_allocate_pool_with_tag(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> int:
    """ExAllocatePoolWithTag: a bump allocator in the pool window, zero-filled by the emulator."""
    tag = struct.pack("<I", args[1]).decode("latin-1")
    return _pool_allocate(oracle, args[0], tag)


@kernel(17, 1)
def _ex_free_pool(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> None:
    oracle.free_log.append(args[0])
    oracle.allocations.pop(args[0], None)
    return None


@kernel(23, 1)
def _ex_query_pool_block_size(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> int:
    size, _tag = oracle.allocations[args[0]]
    return (size + 15) & ~15


@kernel(166, 5)
def _mm_allocate_contiguous_ex(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> int:
    """Contiguous memory at virtual 0x80000000 | physical, as on a console."""
    size, _low, _high, alignment, _protect = args
    alignment = alignment or 0x1000
    physical = (oracle._contiguous_next + alignment - 1) & ~(alignment - 1)
    oracle._contiguous_next = physical + ((size + 0xFFF) & ~0xFFF)
    address = CONTIGUOUS_BASE | physical
    oracle.allocations[address] = (size, "contiguous")
    return address


@kernel(171, 1)
def _mm_free_contiguous(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> None:
    oracle.free_log.append(args[0])
    oracle.allocations.pop(args[0], None)
    return None


@kernel(180, 1)
def _mm_query_allocation_size(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> int:
    size, _tag = oracle.allocations[args[0]]
    return (size + 0xFFF) & ~0xFFF


@kernel(173, 1)
def _mm_get_physical_address(_oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> int:
    """Contiguous memory is identity-mapped under 0x80000000; anything else reads as itself."""
    return args[0] & 0x7FFFFFFF if args[0] >= CONTIGUOUS_BASE else args[0]


@kernel(175, 3)
def _mm_lock_unlock_buffer_pages(
    _oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int
) -> None:
    return None


@kernel(160, 1)
def _kf_raise_irql(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> int:
    previous = oracle.irql
    oracle.set_irql(args[0] & 0xFF)
    return previous


@kernel(161, 1)
def _kf_lower_irql(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> None:
    oracle.set_irql(args[0] & 0xFF)
    return None


@kernel(151, 1)
def _ke_stall_execution_processor(
    oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int
) -> None:
    oracle.stalled_microseconds += args[0]
    oracle.clock += args[0] * 10
    return None


@kernel(128, 1)
def _ke_query_system_time(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> None:
    """The system time advances by 1 ms per query, so a timeout loop ends."""
    oracle.clock += 10_000
    oracle.write_bytes(args[0], struct.pack("<Q", oracle.clock))
    return None


@kernel(277, 1)
def _rtl_enter_critical_section(_oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int) -> int:
    return 0


@kernel(294, 1)
def _rtl_leave_critical_section(_oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int) -> int:
    return 0


@kernel(107, 3)
def _ke_initialize_dpc(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> None:
    """KDPC is 0x1C bytes: the real kernel fills Type, Number, routine and context."""
    dpc, routine, context = args
    oracle.write_bytes(dpc, struct.pack("<HBBIIIII", 19, 0, 0, 0, 0, routine, context, 0)[:0x1C])
    return None


@kernel(113, 2)
def _ke_initialize_timer_ex(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> None:
    timer, timer_type = args
    oracle.write_bytes(timer, struct.pack("<II", 8 + timer_type, 0))
    return None


@kernel(119, 3)
def _ke_insert_queue_dpc(_oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int) -> int:
    return 1


@kernel(137, 1)
def _ke_remove_queue_dpc(_oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int) -> int:
    return 0


@kernel(44, 2)
def _hal_get_interrupt_vector(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> int:
    level, irql_pointer = args
    oracle.write_bytes(irql_pointer, bytes([level & 0xFF]))
    return 0x30 + level


@kernel(109, 7)
def _ke_initialize_interrupt(_oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int) -> None:
    return None


@kernel(98, 1)
def _ke_connect_interrupt(_oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int) -> int:
    return 1


@kernel(100, 1)
def _ke_disconnect_interrupt(_oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int) -> int:
    return 1


@kernel(47, 2)
def _hal_register_shutdown_notification(
    _oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int
) -> None:
    return None


@kernel(142, 1)
def _ke_save_floating_point_state(
    _oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int
) -> int:
    return 0


@kernel(139, 1)
def _ke_restore_floating_point_state(
    _oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int
) -> int:
    return 0


@kernel(24, 5)
def _ex_query_non_volatile_setting(
    oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int
) -> int:
    """A dword setting: Type 4, the stored value (zero if none), the requested length ignored."""
    index, type_pointer, value_pointer, length, result_pointer = args
    if type_pointer:
        oracle.write32(type_pointer, 4)
    oracle.write_bytes(value_pointer, oracle.settings.get(index, 0).to_bytes(4, "little")[:length])
    if result_pointer:
        oracle.write32(result_pointer, min(length, 4))
    return 0


@kernel(289, 2)
def _rtl_init_ansi_string(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> None:
    """ANSI_STRING { Length, MaximumLength, Buffer }, the length being strlen without the NUL."""
    destination, source = args
    length = 0
    if source:
        while oracle.read_bytes(source + length, 1) != b"\x00":
            length += 1
    oracle.write_bytes(
        destination, struct.pack("<HHI", length, length + 1 if source else 0, source)
    )
    return None


@kernel(279, 3)
def _rtl_equal_string(oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> int:
    first, second, fold = args

    def text(pointer: int) -> bytes:
        length, _maximum, buffer = struct.unpack("<HHI", oracle.read_bytes(pointer, 8))
        return oracle.read_bytes(buffer, length)

    left, right = text(first), text(second)
    return int((left.lower() == right.lower()) if fold else (left == right))


@kernel(301, 1)
def _rtl_nt_status_to_dos_error(_oracle: Oracle, args: Sequence[int], _ecx: int, _edx: int) -> int:
    return args[0] & 0xFFFF


@kernel(327, 1)
def _xe_load_section(_oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int) -> int:
    """Every section is already mapped at its own address, so loading one succeeds at once."""
    return 0


@kernel(328, 1)
def _xe_unload_section(_oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int) -> int:
    return 0


@kernel(225, 3)
def _nt_set_event(_oracle: Oracle, _args: Sequence[int], _ecx: int, _edx: int) -> int:
    return 0


# --------------------------------------------------------------------------- command line


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.dsoundscan.oracle",
        description="Run retail DirectSoundCreate under an x86 emulator and report its behaviour.",
    )
    parser.add_argument("xbe", type=Path, help="path to the retail XBE")
    parser.add_argument("--create", action="store_true", help="run DirectSoundCreate")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    oracle = Oracle(args.xbe)
    if args.create:
        oracle.initialize_library()
        out = SCRATCH_BASE + 0x100
        oracle.write32(out, 0xDEADBEEF)
        try:
            result = oracle.run(DIRECT_SOUND_CREATE, [0, out, 0])
        except OracleError as error:
            print(f"oracle stopped: {error}")
            for ordinal, name, arguments in oracle.kernel_log:
                print(f"  kernel {ordinal} {name} {[hex(a) for a in arguments]}")
            for address, value, eip in oracle.hardware.reads[-6:]:
                print(f"  last read {address:#010x} -> {value:#x} from {eip:#010x}")
            for address, value, eip in oracle.hardware.writes[-12:]:
                print(f"  last write {address:#010x} <- {value:#x} from {eip:#010x}")
            return 1
        print(f"DirectSoundCreate -> {result:#010x}, *ppDS = {oracle.read32(out):#010x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
