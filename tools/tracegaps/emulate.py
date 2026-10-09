# SPDX-License-Identifier: GPL-3.0-or-later
"""Run one stretch of the title's own code on literal arguments and capture the state at a stop.

WHY EXECUTE. Reading what the CreateEvent wrapper does with its `ManualReset` argument, or
which key a switch wrapper picks for a kind code, would be a second hand-written copy of
logic the image already contains. The stretch is run instead, from the XBE's own bytes, in
a Unicorn x86-32 instance with the image mapped and a stack, and the state is read where
the question is asked (the arguments of the `NtCreateEvent` call, or the key register at
the shared join of a switch wrapper).

WHAT RUNS. Only the function being asked about, entered with `arguments` pushed cdecl
style (argument 1 nearest the return address) and a return address that stops the run.
Nothing else of the title executes. A stop address is never executed: the capture is
taken when control arrives and the run ends there.

SYMBOLIC ARGUMENTS. A value the caller leaves open is passed as a distinctive sentinel
(`symbol`) so a result that equals it can be reported as "passes argument N through"
rather than as a number.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass

from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32, Uc
from unicorn.x86_const import (
    UC_X86_REG_EAX,
    UC_X86_REG_EBP,
    UC_X86_REG_EBX,
    UC_X86_REG_ECX,
    UC_X86_REG_EDI,
    UC_X86_REG_EDX,
    UC_X86_REG_EIP,
    UC_X86_REG_ESI,
    UC_X86_REG_ESP,
)

from tools.shaderscan.image import Image

PAGE = 0x1000
STACK_BASE = 0x0100_0000
STACK_SIZE = 0x0010_0000
#: A mapped page the run "returns" to. Reaching it ends the run without a capture.
SENTINEL = 0x0000_4000
#: Instruction budget. The stretches asked about are tens of instructions.
INSN_LIMIT = 20_000
#: Scratch RAM for callers that need a fake record or buffer in guest memory.
SCRATCH_BASE = 0x0200_0000
SCRATCH_SIZE = 0x0001_0000
#: First sentinel value for a symbolic argument. Distinct, unlikely as a real value.
SYMBOL_BASE = 0xC0DE0000

_REGISTERS = {
    "eax": UC_X86_REG_EAX,
    "ebx": UC_X86_REG_EBX,
    "ecx": UC_X86_REG_ECX,
    "edx": UC_X86_REG_EDX,
    "esi": UC_X86_REG_ESI,
    "edi": UC_X86_REG_EDI,
    "ebp": UC_X86_REG_EBP,
}


def symbol(number: int) -> int:
    """The sentinel standing for open argument `number` (1-based)."""
    return SYMBOL_BASE + number


@dataclass(frozen=True)
class Capture:
    """State at the stop that ended a run."""

    stop: int
    registers: Mapping[str, int]
    #: 32-bit words from `esp` upward at the stop, `words[0]` at `[esp]`
    words: tuple[int, ...]


class Runner:
    """One Unicorn instance with the XBE mapped, reusable across many runs."""

    def __init__(self, image: Image) -> None:
        self._uc = Uc(UC_ARCH_X86, UC_MODE_32)
        base = image.xbe.base_address
        size = (image.xbe.size_of_image + 0xFFFF) & ~0xFFFF
        self._uc.mem_map(base, size)
        for section in image.xbe.sections:
            body = image.raw[section.raw_addr : section.raw_addr + section.raw_size]
            if body:
                self._uc.mem_write(section.virtual_addr, body)
        self._uc.mem_map(STACK_BASE, STACK_SIZE)
        self._uc.mem_map(SENTINEL, PAGE)
        self._uc.mem_map(SCRATCH_BASE, SCRATCH_SIZE)
        self._capture: Capture | None = None
        self._stops: dict[int, int] = {}
        self._active: frozenset[int] = frozenset()
        self._words = 8

    def run(
        self,
        entry: int,
        arguments: Sequence[int],
        stops: Sequence[int],
        *,
        words: int = 8,
        registers: Mapping[str, int] | None = None,
        memory: Mapping[int, int] | None = None,
    ) -> Capture | None:
        """Call `entry(arguments...)` and return the state when control reaches a stop address.

        `registers` seeds general registers after the defaults are cleared and `memory` maps
        scratch addresses to 32-bit words (only inside `SCRATCH_BASE`). None is returned when
        the run returns or exhausts its budget without reaching a stop.
        """
        uc = self._uc
        self._capture = None
        self._words = words
        for stop in stops:
            if stop not in self._stops:
                self._stops[stop] = uc.hook_add(UC_HOOK_CODE, self._on_stop, begin=stop, end=stop)
        self._active = frozenset(stops)
        esp = STACK_BASE + STACK_SIZE - 0x2000
        frame = [SENTINEL, *[value & 0xFFFFFFFF for value in arguments]]
        for number, value in enumerate(frame):
            uc.mem_write(esp + 4 * number, value.to_bytes(4, "little"))
        uc.reg_write(UC_X86_REG_ESP, esp)
        for register in _REGISTERS.values():
            uc.reg_write(register, 0)
        for name, value in (registers or {}).items():
            uc.reg_write(_REGISTERS[name], value & 0xFFFFFFFF)
        for address, word in (memory or {}).items():
            uc.mem_write(address, (word & 0xFFFFFFFF).to_bytes(4, "little"))
        uc.reg_write(UC_X86_REG_EIP, entry)
        try:
            uc.emu_start(entry, SENTINEL, count=INSN_LIMIT)
        except Exception as error:  # a fault means the run left the modelled stretch
            raise RuntimeError(f"emulation of {entry:#x} faulted: {error}") from error
        return self._capture

    def _on_stop(self, uc: Uc, address: int, size: int, user: object) -> None:
        if address not in self._active or self._capture is not None:
            return
        esp = uc.reg_read(UC_X86_REG_ESP)
        data = bytes(uc.mem_read(esp, 4 * self._words))
        stack = tuple(int.from_bytes(data[n : n + 4], "little") for n in range(0, len(data), 4))
        registers = {name: uc.reg_read(number) for name, number in _REGISTERS.items()}
        registers["esp"] = esp
        self._capture = Capture(address, registers, stack)
        uc.emu_stop()
