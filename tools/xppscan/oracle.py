# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the retail XPP library bytes under an x86 emulator, against a MODEL of the USB hardware.

This is the XPP counterpart of `tools/d3dscan/oracle.py`, built on its `Oracle` (the image is mapped
at its own addresses, kernel imports are traps, `run` calls a stdcall function). What this adds:

  * a kernel model for the ordinals the peripheral stack calls (events, DPCs, timers, interrupts,
    pool and contiguous memory, device objects), each documented with the assumption it makes;
  * an OHCI controller (`tools/xppscan/ohci.py`) at 0xFED00000 and the devices on its root hub;
  * time. A library call that waits or sleeps yields: the emulation stops, the model advances the
    clock, the controller processes its lists, interrupts and DPCs run, and the call resumes.

A MODEL IS NOT HARDWARE. Every behaviour the library depends on that this module supplies is listed
in `docs/xpp-init.md`. The point is that the library's OWN code produces every answer the port is
compared against, so a disagreement is a finding about the port or about this model, never about
what the author remembers of the XDK.
"""

from __future__ import annotations

import struct
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from tools.d3dscan.oracle import (
    FS_BASE,
    HEAP_BASE,
    SENTINEL,
    STACK_TOP,
    Oracle,
    OracleError,
)
from tools.kernel_ordinals import KERNEL_ORDINALS
from tools.xppscan import ohci as usb
from tools.xppscan.image import DEFAULT_XBE

TIMER_BASE = 0xFE802000
TIMER_REGISTER = 0xFE80200C
OHCI_BASE = 0xFED00000
OHCI_BYTES = 0x1000

# Pool memory (ExAllocatePool) and contiguous memory (MmAllocateContiguousMemory) live in the
# oracle's own windows, above anything the d3d oracle uses for its heap.
POOL_BASE = HEAP_BASE + 0x00100000
CONTIGUOUS_VIRTUAL_BASE = 0x80000000
CONTIGUOUS_FIRST_PHYSICAL = 0x02000000

# T723: the library's SetLastError (`0x37E9CF`) reads the TLS array pointer at fs:[4], then the
# slot `[0x771368]` of it, and stores the error code at slot block + 4. Without a TLS array the read
# of that slot faults at `0x37E9ED`, which is a gap of this MODEL (no thread environment), not
# behaviour of the library. The model is one zeroed error block that every slot index points at.
TLS_ARRAY = FS_BASE + 0x3000
TLS_SLOTS = 64
LAST_ERROR_BLOCK = FS_BASE + 0x3800

NESTED_STACK_STRIDE = 0x00020000
STATUS_TIMEOUT = 0x00000102
# The simulated time one wait may consume before it is declared starved: five seconds, in 100 ns.
WAIT_STARVATION_LIMIT = 5 * 10_000_000
YIELD_BUDGET = 2_000_000

# A model returns RETRY to leave the call pending: the emulation yields, time passes, and the same
# trap runs again. That is how a wait is blocked on hardware without a nested emulator.
RETRY = object()
KernelModel = Callable[["XppOracle", int], "int | None | object"]
_MODELS: dict[int, tuple[int, KernelModel]] = {}


def kernel(ordinal: int, arguments: int) -> Callable[[KernelModel], KernelModel]:
    def register(handler: KernelModel) -> KernelModel:
        _MODELS[ordinal] = (arguments, handler)
        return handler

    return register


def modelled_ordinals() -> frozenset[int]:
    return frozenset(_MODELS)


@dataclass
class KernelCall:
    ordinal: int
    name: str
    arguments: tuple[int, ...]


@dataclass
class Interrupt:
    address: int
    routine: int
    context: int
    irql: int
    connected: bool = False


@dataclass
class Timer:
    address: int
    due: int
    period: int
    dpc: int


@dataclass
class OracleStats:
    yields: int = 0
    interrupts_delivered: int = 0
    dpcs_run: int = 0
    starved_waits: int = 0
    instructions_budget_exhausted: int = 0


class _GuestMemory(usb.Memory):
    def __init__(self, oracle: XppOracle) -> None:
        self.oracle = oracle

    def physical_to_virtual(self, physical: int) -> int:
        page = physical & ~0xFFF
        virtual = self.oracle.physical_pages.get(page)
        if virtual is not None:
            return virtual | (physical & 0xFFF)
        return super().physical_to_virtual(physical)

    def read32(self, address: int) -> int:
        return self.oracle.read32(address)

    def write32(self, address: int, value: int) -> None:
        self.oracle.write32(address, value)

    def read_bytes(self, address: int, length: int) -> bytes:
        return self.oracle.read_bytes(address, length)

    def write_bytes(self, address: int, data: bytes) -> None:
        self.oracle.write_bytes(address, data)


class XppOracle(Oracle):
    """The library under emulation, a controller on 0xFED00000 and a clock."""

    def __init__(self, xbe: Path = DEFAULT_XBE) -> None:
        super().__init__(xbe)
        from unicorn import UC_HOOK_BLOCK

        self.irql = 0
        self.depth = 0
        self.now = 0
        self.calls: list[KernelCall] = []
        self.first_use: list[int] = []
        self.pool_next = POOL_BASE
        self.pool_freed: list[int] = []
        self.physical_pages: dict[int, int] = {}
        self.contiguous_next = CONTIGUOUS_FIRST_PHYSICAL
        self.interrupts: list[Interrupt] = []
        self.dpc_queue: list[tuple[int, int, int]] = []
        self.timers: dict[int, Timer] = {}
        self.stats = OracleStats()
        self.coverage: set[int] = set()
        self._yield_requested = False
        self.wait_deadlines: dict[tuple[int, int], int] = {}
        self.ohci = usb.Ohci(_GuestMemory(self))
        self.emulator.mmio_map(OHCI_BASE, OHCI_BYTES, self._ohci_read, None, self._ohci_write, None)
        self.emulator.mmio_map(TIMER_BASE, 0x1000, self._timer_read, None, self._timer_write, None)
        self.emulator.hook_add(UC_HOOK_BLOCK, self._block_hook)
        self.last_fault = (0, 0, 0)
        self.emulator.mem_write(FS_BASE + 4, struct.pack("<I", TLS_ARRAY))
        self.emulator.mem_write(TLS_ARRAY, struct.pack("<I", LAST_ERROR_BLOCK) * TLS_SLOTS)

    def last_error(self) -> int:
        """The code the library last stored with its SetLastError (0 until one is stored)."""
        return self.read32(LAST_ERROR_BLOCK + 4)

    def clear_last_error(self) -> None:
        self.write32(LAST_ERROR_BLOCK + 4, 0)

    def _unmapped(
        self, emulator: object, access: int, address: int, _size: int, _value: int, _user: object
    ) -> bool:
        from unicorn.x86_const import UC_X86_REG_EIP

        self.last_fault = (access, address, self.emulator.reg_read(UC_X86_REG_EIP))
        return False

    def _block_hook(self, _emulator: object, address: int, _size: int, _user: object) -> None:
        self.coverage.add(address)

    # ------------------------------------------------------------------ hardware

    def _timer_read(self, _emulator: object, offset: int, size: int, _user: object) -> int:
        """The 3.579545 MHz timer the library reads at 0xFE80200C to time out USB operations."""
        if offset != TIMER_REGISTER - TIMER_BASE:
            return 0
        ticks = self.now * 3579545 // 10_000_000
        return ticks & ((1 << (8 * size)) - 1)

    def _timer_write(
        self, _emulator: object, _offset: int, _size: int, _value: int, _user: object
    ) -> None:
        return None

    def _ohci_read(self, _emulator: object, offset: int, size: int, _user: object) -> int:
        value = self.ohci.read(offset & ~3)
        return (value >> ((offset & 3) * 8)) & ((1 << (8 * size)) - 1)

    def _ohci_write(
        self, _emulator: object, offset: int, size: int, value: int, _user: object
    ) -> None:
        if size != 4 or offset & 3:
            raise OracleError(f"OHCI write of {size} bytes at offset {offset:#x}")
        self.ohci.write(offset, value)

    # ------------------------------------------------------------------ the kernel

    def _trap_hook(self, _emulator: object, address: int, _size: int, _user: object) -> None:
        from unicorn.x86_const import UC_X86_REG_ESP

        ordinal = self._trap_ordinals.get(address & ~0xF)
        if ordinal is None:
            return
        esp = self.emulator.reg_read(UC_X86_REG_ESP)
        entry = _MODELS.get(ordinal)
        name = KERNEL_ORDINALS.get(ordinal, "?")
        if entry is None:
            raise OracleError(
                f"kernel ordinal {ordinal} {name} has no model (called from {self.read32(esp):#x})"
            )
        arguments, handler = entry
        values = tuple(self.read32(esp + 4 + 4 * index) for index in range(arguments))
        self.calls.append(KernelCall(ordinal, name, values))
        if ordinal not in self.first_use:
            self.first_use.append(ordinal)
        result = handler(self, esp)
        if result is RETRY:
            from unicorn.x86_const import UC_X86_REG_EIP

            self.calls.pop()
            # Writing EIP abandons the trap cell's instruction, so it never executes.
            self.emulator.reg_write(UC_X86_REG_EIP, address)
            self._yield_requested = True
        else:
            self._return_from(esp, arguments, result)  # type: ignore[arg-type]
        if self._yield_requested:
            self.emulator.emu_stop()

    def request_yield(self) -> None:
        """Called by a kernel model that has let time pass or is waiting."""
        if self.depth == 0:
            self._yield_requested = True

    # ------------------------------------------------------------------ calling guest code

    def call(
        self,
        address: int,
        arguments: tuple[int, ...] | list[int] = (),
        ecx: int | None = None,
        edx: int | None = None,
        budget: int = YIELD_BUDGET,
    ) -> int:
        """Call a stdcall function. A kernel wait or delay inside it yields to `pump`."""
        if budget <= 0:
            raise ValueError("instruction budget must be positive")
        from unicorn import UcError
        from unicorn.x86_const import (
            UC_X86_REG_EAX,
            UC_X86_REG_EBP,
            UC_X86_REG_ECX,
            UC_X86_REG_EDX,
            UC_X86_REG_EIP,
            UC_X86_REG_ESP,
        )

        esp = STACK_TOP - 0x1000 - self.depth * NESTED_STACK_STRIDE
        for value in reversed(tuple(arguments)):
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
        eip = address
        for _ in range(10_000):
            self._yield_requested = False
            try:
                self.emulator.emu_start(eip, SENTINEL, count=budget)
            except UcError as error:
                where = self.emulator.reg_read(UC_X86_REG_EIP)
                raise OracleError(
                    f"emulation failed at {where:#010x}: {error} (fault {self.last_fault})"
                ) from error
            if not self._yield_requested:
                where = self.emulator.reg_read(UC_X86_REG_EIP)
                if where != SENTINEL:
                    self.stats.instructions_budget_exhausted += 1
                    raise OracleError(
                        f"{budget} instructions without returning; stopped at {where:#010x}"
                    )
                break
            self._yield_requested = False
            eip = self.emulator.reg_read(UC_X86_REG_EIP)
            self.stats.yields += 1
            context = self.emulator.context_save()
            self.depth += 1
            try:
                self.pump()
            finally:
                self.depth -= 1
            self.emulator.context_restore(context)
        else:
            raise OracleError("a library call yielded more than 10000 times")
        return int(self.emulator.reg_read(UC_X86_REG_EAX))

    # ------------------------------------------------------------------ time, interrupts, DPCs

    def advance(self, hundred_ns: int) -> None:
        """Let simulated time pass and run everything it makes runnable."""
        self.now += hundred_ns
        self.depth += 1
        try:
            self.pump()
        finally:
            self.depth -= 1

    def advance_ms(self, milliseconds: int) -> None:
        for _ in range(milliseconds):
            self.advance(10_000)

    def settle(self) -> None:
        """Run the hardware, interrupts and DPCs until nothing is runnable."""
        self.depth += 1
        try:
            self.pump()
        finally:
            self.depth -= 1

    def pump(self) -> None:
        for _ in range(64):
            self.ohci.set_frame(self.now // 10_000)
            progressed = self._fire_timers()
            progressed |= self.ohci.process()
            progressed |= self._deliver_interrupts()
            progressed |= self._run_dpcs()
            if not progressed:
                return

    def _fire_timers(self) -> bool:
        fired = False
        for address, timer in sorted(self.timers.items(), key=lambda item: item[1].due):
            if timer.due > self.now:
                continue
            fired = True
            self.write32(address + 4, 1)
            if timer.period:
                timer.due = self.now + timer.period
            else:
                del self.timers[address]
            if timer.dpc:
                self.queue_dpc(timer.dpc, 0, 0)
        return fired

    def _deliver_interrupts(self) -> bool:
        delivered = False
        for _ in range(8):
            if not self.ohci.interrupt_pending():
                break
            routines = [item for item in self.interrupts if item.connected]
            if not routines:
                break
            for item in routines:
                saved = self.irql
                self.irql = item.irql
                self.call(item.routine, [item.address, item.context])
                self.irql = saved
                self.stats.interrupts_delivered += 1
                delivered = True
        return delivered

    def queue_dpc(self, dpc: int, argument1: int, argument2: int) -> bool:
        if any(item[0] == dpc for item in self.dpc_queue):
            return False
        self.write32(dpc + 0x14, argument1)
        self.write32(dpc + 0x18, argument2)
        self.dpc_queue.append((dpc, argument1, argument2))
        return True

    def _run_dpcs(self) -> bool:
        ran = False
        while self.dpc_queue:
            dpc, argument1, argument2 = self.dpc_queue.pop(0)
            routine = self.read32(dpc + 0x0C)
            context = self.read32(dpc + 0x10)
            saved = self.irql
            self.irql = 2
            self.call(routine, [dpc, context, argument1, argument2])
            self.irql = saved
            self.stats.dpcs_run += 1
            ran = True
        return ran

    # ------------------------------------------------------------------ allocation

    def allocate_pool(self, size: int) -> int:
        address = (self.pool_next + 15) & ~15
        self.pool_next = address + ((size + 15) & ~15)
        self.write_bytes(address, b"\x00" * size)
        return address

    def allocate_contiguous(self, size: int, alignment: int = 0x1000) -> int:
        alignment = max(alignment, 0x1000)
        physical = (self.contiguous_next + alignment - 1) & ~(alignment - 1)
        self.contiguous_next = physical + ((size + 0xFFF) & ~0xFFF)
        address = CONTIGUOUS_VIRTUAL_BASE | physical
        self.write_bytes(address, b"\x00" * size)
        return address

    # ------------------------------------------------------------------ attaching devices

    def attach(self, port: int, device: usb.UsbDevice) -> None:
        self.ohci.attach(port, device)

    def detach(self, port: int) -> None:
        self.ohci.detach(port)


# --------------------------------------------------------------------------- the kernel model


def _argument(oracle: XppOracle, esp: int, index: int) -> int:
    return oracle.read32(esp + 4 + 4 * index)


@kernel(15, 2)
def _ex_allocate_pool_with_tag(oracle: XppOracle, esp: int) -> int:
    """Zeroed pool. The real pool does not zero, so a library that reads uninitialised pool would
    read zeros here and garbage on a console: a gap this model cannot find."""
    return oracle.allocate_pool(_argument(oracle, esp, 0))


@kernel(17, 1)
def _ex_free_pool(oracle: XppOracle, esp: int) -> None:
    """ExFreePool(address): the bump allocator never reuses memory, so only record the address for
    tests (T1086). Not a pool model: no double free or bad address check."""
    oracle.pool_freed.append(_argument(oracle, esp, 0))
    return None


@kernel(44, 2)
def _hal_get_interrupt_vector(oracle: XppOracle, esp: int) -> int:
    """Vector 0x30 + level and an irql of 0x1C - level, as the console's PIC mapping goes."""
    level = _argument(oracle, esp, 0)
    oracle.write32(_argument(oracle, esp, 1), 0x1C - level)
    return 0x30 + level


@kernel(47, 2)
def _hal_register_shutdown_notification(_oracle: XppOracle, _esp: int) -> None:
    return None


@kernel(107, 3)
def _ke_initialize_dpc(oracle: XppOracle, esp: int) -> None:
    """Writes the KDPC fields the way the kernel does: type 19, then routine and context."""
    dpc, routine, context = (_argument(oracle, esp, index) for index in range(3))
    oracle.write_bytes(dpc, struct.pack("<HBBII", 19, 0, 1, 0, 0))
    oracle.write32(dpc + 0x0C, routine)
    oracle.write32(dpc + 0x10, context)
    oracle.write32(dpc + 0x1C, 0)
    return None


@kernel(119, 3)
def _ke_insert_queue_dpc(oracle: XppOracle, esp: int) -> int:
    dpc, argument1, argument2 = (_argument(oracle, esp, index) for index in range(3))
    return int(oracle.queue_dpc(dpc, argument1, argument2))


@kernel(109, 7)
def _ke_initialize_interrupt(oracle: XppOracle, esp: int) -> None:
    address, routine, context, _vector, irql, _mode, _share = (
        _argument(oracle, esp, index) for index in range(7)
    )
    oracle.interrupts.append(Interrupt(address, routine, context, irql))
    return None


@kernel(98, 1)
def _ke_connect_interrupt(oracle: XppOracle, esp: int) -> int:
    address = _argument(oracle, esp, 0)
    for item in oracle.interrupts:
        if item.address == address:
            item.connected = True
            return 1
    raise OracleError(f"KeConnectInterrupt on an object never initialised: {address:#x}")


@kernel(113, 2)
def _ke_initialize_timer_ex(oracle: XppOracle, esp: int) -> None:
    timer, kind = _argument(oracle, esp, 0), _argument(oracle, esp, 1)
    oracle.write_bytes(timer, struct.pack("<BBBBIII", 8 + kind, 0, 10, 0, 0, 0, 0))
    oracle.write_bytes(timer + 0x10, b"\x00" * 0x18)
    return None


def _arm_timer(oracle: XppOracle, timer: int, low: int, high: int, period: int, dpc: int) -> int:
    was_set = timer in oracle.timers
    due_signed = struct.unpack("<q", struct.pack("<II", low, high))[0]
    due = oracle.now + (-due_signed if due_signed < 0 else max(due_signed - oracle.now, 0))
    oracle.write32(timer + 4, 0)
    oracle.write32(timer + 0x20, dpc)
    oracle.timers[timer] = Timer(timer, due, period * 10_000, dpc)
    return int(was_set)


@kernel(149, 4)
def _ke_set_timer(oracle: XppOracle, esp: int) -> int:
    timer, low, high, dpc = (_argument(oracle, esp, index) for index in range(4))
    return _arm_timer(oracle, timer, low, high, 0, dpc)


@kernel(150, 5)
def _ke_set_timer_ex(oracle: XppOracle, esp: int) -> int:
    timer, low, high, period, dpc = (_argument(oracle, esp, index) for index in range(5))
    return _arm_timer(oracle, timer, low, high, period, dpc)


@kernel(97, 1)
def _ke_cancel_timer(oracle: XppOracle, esp: int) -> int:
    return int(oracle.timers.pop(_argument(oracle, esp, 0), None) is not None)


@kernel(128, 1)
def _ke_query_system_time(oracle: XppOracle, esp: int) -> None:
    oracle.write_bytes(_argument(oracle, esp, 0), struct.pack("<Q", oracle.now))
    return None


@kernel(129, 0)
def _ke_raise_irql_to_dpc_level(oracle: XppOracle, _esp: int) -> int:
    """Returns the old irql in al. Raising never preempts anything in this model."""
    previous = oracle.irql
    oracle.irql = max(oracle.irql, 2)
    return previous


@kernel(161, 0)
def _kf_lower_irql(oracle: XppOracle, _esp: int) -> None:
    from unicorn.x86_const import UC_X86_REG_ECX

    oracle.irql = int(oracle.emulator.reg_read(UC_X86_REG_ECX)) & 0xFF
    return None


@kernel(151, 1)
def _ke_stall_execution_processor(oracle: XppOracle, esp: int) -> None:
    oracle.now += _argument(oracle, esp, 0) * 10
    return None


@kernel(99, 3)
def _ke_delay_execution_thread(oracle: XppOracle, esp: int) -> int:
    """Sleeps. The interval is read as a signed 64-bit count of 100 ns, negative being relative."""
    interval = oracle.read_bytes(_argument(oracle, esp, 2), 8)
    amount = struct.unpack("<q", interval)[0]
    oracle.now += -amount if amount < 0 else 0
    oracle.request_yield()
    return 0


@kernel(165, 1)
def _mm_allocate_contiguous_memory(oracle: XppOracle, esp: int) -> int:
    return oracle.allocate_contiguous(_argument(oracle, esp, 0))


@kernel(173, 1)
def _mm_get_physical_address(oracle: XppOracle, esp: int) -> int:
    """Contiguous memory at 0x80000000 | p has physical p. Everything else is identity mapped, which
    the controller memory adapter undoes. Record translated pages because the inherited allocator
    also uses low physical pages outside OHCI's default inverse range. Not a console page table."""
    address = _argument(oracle, esp, 0)
    if address >= CONTIGUOUS_VIRTUAL_BASE and address < CONTIGUOUS_VIRTUAL_BASE + 0x08000000:
        physical = address & 0x0FFFFFFF
        oracle.physical_pages[physical & ~0xFFF] = address & ~0xFFF
        return physical
    return address


@kernel(175, 3)
def _mm_lock_unlock_buffer_pages(_oracle: XppOracle, _esp: int) -> None:
    return None


@kernel(176, 2)
def _mm_lock_unlock_physical_page(_oracle: XppOracle, _esp: int) -> None:
    """MmLockUnlockPhysicalPage(address, unlock): the oracle's memory is never paged, so locking
    is a no-op. Called by the voice iso completion at 0x00474240 (T1086)."""
    return None


@kernel(289, 2)
def _rtl_init_ansi_string(oracle: XppOracle, esp: int) -> None:
    destination, source = _argument(oracle, esp, 0), _argument(oracle, esp, 1)
    length = 0
    if source:
        while oracle.read_bytes(source + length, 1) != b"\x00":
            length += 1
    oracle.write_bytes(
        destination, struct.pack("<HHI", length, length + 1 if source else 0, source)
    )
    return None


@kernel(65, 6)
def _io_create_device(oracle: XppOracle, esp: int) -> int:
    """A device object of 0x40 bytes then the extension, the extension pointer at +0x18 (the one
    field the library reads), the driver object at +0x0C."""
    driver, extension_size, _name, _type, _exclusive, out = (
        _argument(oracle, esp, index) for index in range(6)
    )
    device = oracle.allocate_pool(0x40 + extension_size)
    oracle.write32(device + 0x0C, driver)
    oracle.write32(device + 0x18, device + 0x40)
    oracle.write32(out, device)
    return 0


@kernel(145, 3)
def _ke_set_event(oracle: XppOracle, esp: int) -> int:
    event = _argument(oracle, esp, 0)
    previous = oracle.read32(event + 4)
    oracle.write32(event + 4, 1)
    return previous


@kernel(159, 5)
def _ke_wait_for_single_object(oracle: XppOracle, esp: int) -> int:
    """Wait on an event or timer. If it is not signalled, let time pass in 1 ms
    steps, running the controller, interrupts and DPCs, until it is or until five simulated seconds
    went by, which is recorded as a starved wait and answered STATUS_TIMEOUT."""
    obj = _argument(oracle, esp, 0)
    key = (esp, obj)
    if not oracle.read32(obj + 4):
        deadline = oracle.wait_deadlines.setdefault(key, oracle.now + WAIT_STARVATION_LIMIT)
        if oracle.now >= deadline:
            del oracle.wait_deadlines[key]
            oracle.stats.starved_waits += 1
            return STATUS_TIMEOUT
        oracle.now += 10_000
        return RETRY
    oracle.wait_deadlines.pop(key, None)
    if oracle.read_bytes(obj, 1)[0] in (1, 9):  # synchronisation event or timer: auto-reset
        oracle.write32(obj + 4, 0)
    return 0


@kernel(277, 1)
def _rtl_enter_critical_section(_oracle: XppOracle, _esp: int) -> int:
    return 0


@kernel(294, 1)
def _rtl_leave_critical_section(_oracle: XppOracle, _esp: int) -> int:
    return 0


@kernel(301, 1)
def _rtl_nt_status_to_dos_error(oracle: XppOracle, esp: int) -> int:
    return _argument(oracle, esp, 0)
