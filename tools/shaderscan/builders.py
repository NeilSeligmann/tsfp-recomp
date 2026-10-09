# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the title's own shader-source builders under emulation and count what they emit.

WHY EMULATION. The title does not hand `XGAssembleShader` a static string. Two game
functions build the source in a scratch buffer by concatenating fragments, choosing
each fragment from bits of one 32-bit key. Whether the set of programs is closed is
then the question "how many distinct buffers can those two functions produce", and the
most reliable way to answer it is to run them on every key that matters and look. Reading
the branches by hand would be a second, unverifiable implementation of the same logic.

WHAT IS EXECUTED. Only the builder function itself, from the XBE's own bytes, in a
Unicorn x86-32 instance with the image mapped. The assembler entry point is replaced by
a stub that returns success and a hook that copies out the arguments it was called
with. Nothing else of the title runs.

WHICH KEY BITS MATTER. `tested_bits` reads them from the builder's instructions
(`test`, `and`, `cmp` against the key or a copy of it), so the enumeration is not
derived from a hand-written list. A static bit set can be incomplete, so `probe_dead_bits`
checks it the other way: random keys are run with and without the bits OUTSIDE the set,
and any difference is reported. Each missed bit that controls output is caught by a
random key with probability 1/2, so with N samples a missed bit survives with
probability 2**-N.

WHAT IS NEVER PRINTED. The emitted source is hashed and sized, never shown. Only counts
leave this module.
"""

from __future__ import annotations

import hashlib
import random
import re
from collections import Counter
from collections.abc import Iterable, Iterator
from dataclasses import dataclass, field

from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_MODE_32, Uc, UcError
from unicorn.x86_const import (
    UC_X86_REG_EAX,
    UC_X86_REG_EBX,
    UC_X86_REG_ECX,
    UC_X86_REG_EDX,
    UC_X86_REG_EIP,
    UC_X86_REG_ESP,
)

from tools.shaderscan.callargs import FAMILY_OF
from tools.shaderscan.image import Image

PAGE = 0x1000
STACK_BASE = 0x0100_0000
STACK_SIZE = 0x0010_0000
#: A mapped page the builder "returns" to. Emulation stops when control reaches it.
SENTINEL = 0x0000_4000

#: `xor eax, eax ; ret 0x2c`. The assembler takes 11 stack arguments, so this is the
#: callee-cleanup return that leaves the stack as the real one would.
STUB_FMT = b"\x33\xc0\xc2%c\x00"

#: Instruction budget for one builder run. The builders are about 150 straight-line
#: instructions plus one `rep` fill, so hitting this means the run went somewhere wrong.
INSN_LIMIT = 200_000


@dataclass(frozen=True)
class BuilderSpec:
    """How to call one builder and read what it hands the assembler."""

    name: str
    entry: int
    #: Register carrying the key into the builder (a title-private convention).
    key_register: str
    assembler: int
    #: 1-based assembler argument holding the source pointer, the byte length and flags.
    source_arg: int = 2
    length_arg: int = 3
    flags_arg: int = 4
    argument_count: int = 11


@dataclass(frozen=True)
class Emission:
    """What one builder run handed the assembler.

    `content` is kept so the assembler can be run on it, and is excluded from the repr
    and from equality so that it cannot reach a log or a failing assertion by accident.
    """

    digest: str
    length: int
    flags: int
    content: bytes = field(default=b"", repr=False, compare=False)


@dataclass
class Enumeration:
    """The result of running a builder over a set of keys."""

    keys_run: int = 0
    failures: int = 0
    no_call: int = 0
    by_digest: Counter[str] = field(default_factory=Counter)
    lengths: dict[str, int] = field(default_factory=dict)
    flag_values: Counter[int] = field(default_factory=Counter)
    #: digest -> (source bytes, assembler flags), only when `keep_sources` was set
    sources: dict[str, tuple[bytes, int]] = field(default_factory=dict, repr=False)
    #: key -> emission digest for every key that emitted (always recorded)
    key_digest: dict[int, str] = field(default_factory=dict, repr=False)

    @property
    def distinct(self) -> int:
        return len(self.by_digest)


class BuilderEmulator:
    """One Unicorn instance with the XBE mapped, reusable across many keys."""

    def __init__(self, image: Image, spec: BuilderSpec) -> None:
        self.spec = spec
        self._uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self._emission: Emission | None = None
        base = image.xbe.base_address
        size = (image.xbe.size_of_image + 0xFFFF) & ~0xFFFF
        self._uc.mem_map(base, size)
        for section in image.xbe.sections:
            body = image.raw[section.raw_addr : section.raw_addr + section.raw_size]
            if body:
                self._uc.mem_write(section.virtual_addr, body)
        self._uc.mem_map(STACK_BASE, STACK_SIZE)
        self._uc.mem_map(SENTINEL, PAGE)
        self._uc.mem_write(spec.assembler, STUB_FMT % (spec.argument_count * 4))
        self._uc.hook_add(
            UC_HOOK_CODE, self._on_assembler, begin=spec.assembler, end=spec.assembler
        )
        self._key_reg = {
            "ebx": UC_X86_REG_EBX,
            "edx": UC_X86_REG_EDX,
            "ecx": UC_X86_REG_ECX,
            "eax": UC_X86_REG_EAX,
        }[spec.key_register]

    def _stack_arg(self, number: int) -> int:
        esp = self._uc.reg_read(UC_X86_REG_ESP)
        return int.from_bytes(self._uc.mem_read(esp + 4 * number, 4), "little")

    def _on_assembler(self, uc: Uc, address: int, size: int, user: object) -> None:
        source = self._stack_arg(self.spec.source_arg)
        length = self._stack_arg(self.spec.length_arg)
        flags = self._stack_arg(self.spec.flags_arg)
        content = bytes(uc.mem_read(source, length))
        self._emission = Emission(hashlib.sha256(content).hexdigest(), length, flags, content)

    def static_reads(self, keys: Iterable[int], low: int, high: int) -> set[int]:
        """Every byte address in `[low, high]` that the builder reads while running `keys`.

        With `[low, high]` spanning the image's initialised data, this is the set of
        static fragment bytes the builder can ever copy, which says how much fixed text
        the whole generator is made of, independent of how the fragments are chosen.
        """
        touched: set[int] = set()

        def on_read(uc: Uc, access: int, address: int, size: int, value: int, user: object) -> None:
            touched.update(range(address, address + size))

        # Translated blocks are cached without the new hook's instrumentation, so they have
        # to be flushed for it to see reads, and again afterwards so later runs stay fast.
        handle = self._uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=low, end=high)
        self._uc.ctl_flush_tb()
        try:
            for key in keys:
                self.run(key)
        finally:
            self._uc.hook_del(handle)
            self._uc.ctl_flush_tb()
        return touched

    def run(self, key: int) -> Emission | None:
        """Run the builder for `key`. None when it never reached the assembler."""
        self._emission = None
        uc = self._uc
        esp = STACK_BASE + STACK_SIZE - 0x1000
        uc.mem_write(esp, SENTINEL.to_bytes(4, "little"))
        uc.reg_write(UC_X86_REG_ESP, esp)
        for register in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX):
            uc.reg_write(register, 0)
        uc.reg_write(self._key_reg, key & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_EIP, self.spec.entry)
        uc.emu_start(self.spec.entry, SENTINEL, count=INSN_LIMIT)
        return self._emission


def enumerate_keys(
    emulator: BuilderEmulator, keys: Iterable[int], *, keep_sources: bool = False
) -> Enumeration:
    """Run every key and tally the distinct emissions."""
    result = Enumeration()
    for key in keys:
        result.keys_run += 1
        try:
            emission = emulator.run(key)
        except UcError:
            result.failures += 1
            continue
        if emission is None:
            result.no_call += 1
            continue
        result.by_digest[emission.digest] += 1
        result.key_digest[key] = emission.digest
        result.lengths[emission.digest] = emission.length
        result.flag_values[emission.flags] += 1
        if keep_sources:
            result.sources.setdefault(emission.digest, (emission.content, emission.flags))
    return result


def subsets(mask: int) -> Iterator[int]:
    """Every submask of `mask`, including 0 and `mask` itself."""
    sub = mask
    while True:
        yield sub
        if sub == 0:
            return
        sub = (sub - 1) & mask


# ---------------------------------------------------------------------------
# Which key bits does the builder read?
# ---------------------------------------------------------------------------

_SUB_REGISTER = re.compile(r"^(?:e?([abcd])x|([abcd])([lh]))$")


def _bit_window(name: str) -> tuple[str, int, int] | None:
    """`(family, low bit, width)` of the part of a 32-bit register a name addresses."""
    family = FAMILY_OF.get(name)
    if family is None:
        return None
    if name.endswith("h") and len(name) == 2:
        return family, 8, 8
    if name.endswith("l") and len(name) == 2:
        return family, 0, 8
    if len(name) == 2:
        return family, 0, 16
    return family, 0, 32


def _next_jump(instructions: list[tuple[str, str]], index: int) -> str:
    """Mnemonic of the first conditional jump after `index`, looking a few instructions on.

    The compiler schedules unrelated loads between a `test r, r` and its jump, so the
    jump is not always the next instruction.
    """
    for mnemonic, _ in instructions[index + 1 : index + 8]:
        if mnemonic.startswith("j"):
            return mnemonic
    return ""


def tested_bits(instructions: Iterable[tuple[str, str]], key_register: str) -> int:
    """Bits of the key that the builder's own code reads, from `(mnemonic, operands)`.

    Recognised, in program order: `test`/`and`/`cmp` of the key register or any of its
    sub-registers against an immediate (the immediate's bits), `test r, r` followed by
    a sign jump (the top bit of that part), `mov copy, key` (the copy inherits the key's
    bits), and `and copy, imm` (the copy then carries only the immediate's bits, which
    are recorded as tested). A write to a copy by anything else drops it.

    This is a static over-approximation of what the code branches on. It can miss a
    read it does not recognise, which is what `probe_dead_bits` exists to catch.
    """
    instructions = list(instructions)
    key_family = FAMILY_OF[key_register]
    live: dict[str, int] = {key_family: 0xFFFFFFFF}
    tested = 0
    for index, (mnemonic, operands) in enumerate(instructions):
        parts = [part.strip() for part in operands.split(",")]
        if mnemonic == "mov" and len(parts) == 2:
            dest = _bit_window(parts[0])
            source = _bit_window(parts[1])
            if dest and dest[0] != key_family:
                if source and source[0] in live and dest[1:] == (0, 32):
                    live[dest[0]] = live[source[0]]
                else:
                    live.pop(dest[0], None)
            continue
        if mnemonic in ("test", "and", "cmp") and len(parts) == 2:
            window = _bit_window(parts[0])
            if window is None or window[0] not in live:
                continue
            family, low, width = window
            span = ((1 << width) - 1) << low
            if parts[1] in FAMILY_OF and _bit_window(parts[1]) == window:
                sign = _next_jump(instructions, index) in ("js", "jns")
                tested |= (1 << (low + width - 1) if sign else span) & live[family]
                continue
            try:
                immediate = int(parts[1], 0)
            except ValueError:
                continue
            bits = (immediate << low) & span & live[family]
            if mnemonic == "cmp":
                bits = span & live[family] if family == key_family else 0
            tested |= bits
            if mnemonic == "and" and family != key_family:
                live[family] = immediate & live[family]
            continue
        if mnemonic in ("add", "sub", "or", "xor", "shl", "shr", "lea", "movzx", "inc", "dec"):
            window = _bit_window(parts[0]) if parts else None
            if window and window[0] != key_family:
                live.pop(window[0], None)
    return tested


def probe_dead_bits(
    emulator: BuilderEmulator,
    candidate_mask: int,
    tested: int,
    samples: int,
    seed: int,
) -> int:
    """Count random keys where bits OUTSIDE `tested` change the output.

    Returns the number of samples where emitting for `key` differs from emitting for
    `key & tested`. A non-zero count means `tested` missed a bit. Keys are drawn
    uniformly from `candidate_mask`.
    """
    rng = random.Random(seed)
    disagreements = 0
    for _ in range(samples):
        key = rng.getrandbits(32) & candidate_mask
        full = emulator.run(key)
        reduced = emulator.run(key & tested)
        if (full and full.digest) != (reduced and reduced.digest):
            disagreements += 1
    return disagreements


def contiguous_regions(addresses: Iterable[int]) -> list[tuple[int, int]]:
    """Merge byte addresses into `(start, length)` runs. Adjacent fragments merge, so the
    run count is a LOWER bound on the number of distinct fragments."""
    ordered = sorted(set(addresses))
    runs: list[tuple[int, int]] = []
    for address in ordered:
        if runs and address == runs[-1][0] + runs[-1][1]:
            runs[-1] = (runs[-1][0], runs[-1][1] + 1)
        else:
            runs.append((address, 1))
    return runs


def sample_keys(mask: int, count: int, seed: int) -> list[int]:
    """`count` seeded random submasks of `mask`, plus every single-bit key and 0 and `mask`."""
    rng = random.Random(seed)
    bits = [1 << position for position in range(32) if mask & (1 << position)]
    keys = {0, mask, *bits}
    keys.update(rng.getrandbits(32) & mask for _ in range(count))
    return sorted(keys)
