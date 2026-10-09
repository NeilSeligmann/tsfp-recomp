# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode and characterise NV2A vertex-program microcode. COUNTS ONLY, never content.

THE ISA IS A PUBLIC HARDWARE FACT, and the instruction-field layout below is described
by reference, not copied. It was read from two local references and cross-checked
against the binary:

  * xqemu `hw/xbox/nv2a/nv2a_vsh.c` (the field table and the opcode names). GPL, a
    local read-only reference per `docs/clean-sources-audit.md`. Its shader translation
    is downstream of Cxbx/Dxbx, so it is NOT independent of them, and that audit says
    so. Nothing was copied: the table here is rewritten as `(dword, low bit, width)`.
  * The Nouveau/envytools NV10-NV2A vertex program documentation, from memory. It could
    not be fetched in this environment, so the opcode ORDER below rests on one local
    reference plus recollection and is flagged as such in `docs/shader-inputs.md`.

WHAT THE BINARY ADDS, which is the only check that is not a second reading of the same
lineage. A program is four little-endian dwords per instruction. The measured
structure of the retail title's programs (`validate_instruction`):

  * dword 0 is zero in every instruction (the layout leaves it unused),
  * the FINAL bit (dword 3, bit 0) is set on exactly the last instruction,
  * no MAC opcode of 14 or 15 occurs, which are the two undefined values of a 4-bit field,
  * an operand a given opcode reads never has the `0` multiplexer value.

Each is a way a MISPLACED field would fail loudly, because a field read at the wrong
bit offset is uniformly random and fails each test with probability well above zero.
`null_pass_rate` quantifies that for the whole predicate.

OPERAND MULTIPLEXER. Per the references the 2-bit selector means 0 none, 1 temporary
register, 2 input attribute, 3 constant register.
"""

from __future__ import annotations

import random
import struct
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field

INSTRUCTION_DWORDS = 4
INSTRUCTION_BYTES = 16

#: field name -> (dword, low bit, width)
FIELDS: dict[str, tuple[int, int, int]] = {
    "ilu": (1, 25, 3),
    "mac": (1, 21, 4),
    "const": (1, 13, 8),
    "input": (1, 9, 4),
    "a_neg": (1, 8, 1),
    "a_temp": (2, 28, 4),
    "a_mux": (2, 26, 2),
    "b_neg": (2, 25, 1),
    "b_temp": (2, 13, 4),
    "b_mux": (2, 11, 2),
    "c_neg": (2, 10, 1),
    "c_mux": (3, 28, 2),
    "out_mac_mask": (3, 24, 4),
    "out_temp": (3, 20, 4),
    "out_ilu_mask": (3, 16, 4),
    "out_o_mask": (3, 12, 4),
    "out_to_output": (3, 11, 1),
    "out_address": (3, 3, 8),
    "out_mux": (3, 2, 1),
    "relative": (3, 1, 1),
    "final": (3, 0, 1),
}

MAC_NAMES = (
    "NOP",
    "MOV",
    "MUL",
    "ADD",
    "MAD",
    "DP3",
    "DPH",
    "DP4",
    "DST",
    "MIN",
    "MAX",
    "SLT",
    "SGE",
    "ARL",
)
ILU_NAMES = ("NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT")
MAC_ARL = 13

#: Which of the A, B, C operands each MAC opcode reads. ADD is A+C and MUL is A*B on a
#: unit that computes A*B+C, which is why ADD skips B and MUL skips C.
MAC_OPERANDS: dict[int, str] = {
    1: "A",
    2: "AB",
    3: "AC",
    4: "ABC",
    5: "AB",
    6: "AB",
    7: "AB",
    8: "AB",
    9: "AB",
    10: "AB",
    11: "AB",
    12: "AB",
    13: "A",
}

MUX_NONE, MUX_TEMP, MUX_INPUT, MUX_CONST = 0, 1, 2, 3

#: Output addresses that name a real output register, per the references. The others
#: (1, 2, 13, 14 and everything above 15) are reserved.
VALID_OUTPUT_ADDRESSES = frozenset({0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15})

#: The constant register file is 192 entries.
CONSTANT_FILE_SIZE = 192


@dataclass(frozen=True)
class Instruction:
    """One 128-bit instruction, as four little-endian dwords."""

    dwords: tuple[int, int, int, int]

    def get(self, name: str) -> int:
        dword, low, width = FIELDS[name]
        return (self.dwords[dword] >> low) & ((1 << width) - 1)

    @property
    def c_temp(self) -> int:
        """The C operand's temporary register, split across dwords 2 and 3."""
        return (self.dwords[2] & 3) << 2 | (self.dwords[3] >> 30) & 3

    @property
    def operands_read(self) -> str:
        """Which of "A", "B", "C" this instruction's opcodes read."""
        mac = self.get("mac")
        reads = MAC_OPERANDS.get(mac, "")
        if self.get("ilu"):
            reads += "C"
        return "".join(sorted(set(reads)))

    @property
    def has_operation(self) -> bool:
        """False for an all-NOP instruction, which no shipped program contains."""
        return bool(self.get("mac") or self.get("ilu"))

    def mux_of(self, operand: str) -> int:
        return self.get(f"{operand.lower()}_mux")


def parse_instructions(data: bytes) -> list[Instruction]:
    """Split `data` into instructions. A trailing partial instruction is ignored."""
    count = len(data) // INSTRUCTION_BYTES
    return [
        Instruction(struct.unpack_from("<4I", data, index * INSTRUCTION_BYTES))
        for index in range(count)
    ]


def validate_instruction(instruction: Instruction) -> list[str]:
    """Names of every structural rule `instruction` breaks. Empty means plausible."""
    broken: list[str] = []
    if instruction.dwords[0] != 0:
        broken.append("dword0_nonzero")
    if instruction.get("mac") > MAC_ARL:
        broken.append("mac_opcode_undefined")
    for operand in instruction.operands_read:
        if instruction.mux_of(operand) == MUX_NONE:
            broken.append(f"operand_{operand.lower()}_mux_none")
    return broken


def is_plausible(instruction: Instruction) -> bool:
    return not validate_instruction(instruction)


@dataclass(frozen=True)
class ProgramSpan:
    """A run of plausible instructions ending at the first FINAL bit."""

    #: Byte offset into the scanned buffer.
    offset: int
    instructions: int


def find_programs(data: bytes, stride: int = 4, minimum: int = 1) -> list[ProgramSpan]:
    """Scan `data` for vertex programs, by structure alone.

    A program is a run of plausible instructions in which the FINAL bit is clear on
    all but the last. `stride` is 4 because nothing in the container guarantees a
    16-byte-aligned start. A run that hits an implausible instruction before its
    FINAL bit is discarded and the scan resumes one stride on. A found program is
    consumed whole, so programs packed back to back are separated at their FINAL bit.

    All-zero instructions are not plausible starts: a zero-filled region would
    otherwise decode as an unbounded run of NOPs. A zero instruction has no FINAL bit,
    so such a run never terminates and is discarded, but rejecting it early keeps the
    scan linear.
    """
    spans: list[ProgramSpan] = []
    limit = len(data) - INSTRUCTION_BYTES
    offset = 0
    while offset <= limit:
        length = _program_length_at(data, offset, limit)
        if length >= minimum:
            spans.append(ProgramSpan(offset, length))
            offset += length * INSTRUCTION_BYTES
        else:
            offset += stride
    return spans


def _program_length_at(data: bytes, offset: int, limit: int) -> int:
    """Instruction count of a complete program starting at `offset`, else 0."""
    count = 0
    position = offset
    while position <= limit:
        words = struct.unpack_from("<4I", data, position)
        if words == (0, 0, 0, 0):
            return 0
        instruction = Instruction(words)
        if not is_plausible(instruction) or not instruction.has_operation:
            return 0
        count += 1
        if instruction.get("final"):
            return count
        position += INSTRUCTION_BYTES
    return 0


#: Low half of the header dword that precedes a program in memory. The high half is the
#: instruction count. 0x2078 is the plain vertex shader, 0x7378 and 0x7778 the state and
#: read/write variants, per the xqemu header. Measured here: only 0x2078 occurs.
HEADER_VERSIONS = (0x2078, 0x7378, 0x7778)


@dataclass(frozen=True)
class HeadedProgram:
    """A program found through its header dword, which names the instruction count."""

    #: Byte offset of the header dword in the scanned buffer.
    offset: int
    version: int
    instructions: int


def find_headed_programs(data: bytes) -> list[HeadedProgram]:
    """Every 4-aligned header dword followed by exactly its declared plausible program.

    The header is `count << 16 | version`. A candidate must have `count` instructions
    that are each plausible, with the FINAL bit set on the last and on no other. That
    is four independent conditions per instruction, so a chance pass is the rate
    `null_pass_rate` reports raised to the program length.

    Candidates come from `bytes.find` on the version's two low bytes, which keeps a
    multi-gigabyte disc scan tractable. The find is byte-granular, so a hit at a
    non-4-aligned offset is skipped and the search resumes one byte on.
    """
    found: list[HeadedProgram] = []
    for version in HEADER_VERSIONS:
        needle = struct.pack("<H", version)
        position = data.find(needle)
        while position >= 0:
            if position % 4 == 0 and position + 4 <= len(data):
                (count,) = struct.unpack_from("<H", data, position + 2)
                end = position + 4 + count * INSTRUCTION_BYTES
                if count and end <= len(data):
                    body = parse_instructions(data[position + 4 : end])
                    if all(i.has_operation and is_plausible(i) for i in body) and (
                        _final_only_last(body)
                    ):
                        found.append(HeadedProgram(position, version, count))
            position = data.find(needle, position + 1)
    return sorted(found, key=lambda program: program.offset)


def _final_only_last(body: Sequence[Instruction]) -> bool:
    return all(bool(i.get("final")) == (n == len(body) - 1) for n, i in enumerate(body))


@dataclass
class Characterisation:
    """Counts describing a set of programs. Holds no instruction bytes."""

    programs: int = 0
    instructions: int = 0
    lengths: Counter[int] = field(default_factory=Counter)
    mac_ops: Counter[str] = field(default_factory=Counter)
    ilu_ops: Counter[str] = field(default_factory=Counter)
    #: Instructions that carry BOTH a MAC and an ILU operation.
    dual_issue: int = 0
    arl_instructions: int = 0
    programs_with_arl: int = 0
    #: Instructions whose constant operand is read relative to the address register.
    relative_reads: int = 0
    programs_with_relative: int = 0
    constant_reads: int = 0
    programs_reading_constants: int = 0
    #: Programs by how many DISTINCT constant registers they name.
    distinct_constants: Counter[int] = field(default_factory=Counter)
    highest_constant: int = -1
    input_registers: Counter[int] = field(default_factory=Counter)
    output_addresses: Counter[int] = field(default_factory=Counter)
    reserved_output_writes: int = 0
    constant_file_writes: int = 0
    defects: Counter[str] = field(default_factory=Counter)
    final_bit_misplaced: int = 0

    def add_program(self, instructions: Sequence[Instruction]) -> None:
        self.programs += 1
        self.instructions += len(instructions)
        self.lengths[len(instructions)] += 1
        constants: set[int] = set()
        has_arl = has_relative = reads_constants = False
        for position, instruction in enumerate(instructions):
            mac = instruction.get("mac")
            ilu = instruction.get("ilu")
            if mac:
                self.mac_ops[MAC_NAMES[mac] if mac <= MAC_ARL else f"UNDEF{mac}"] += 1
            if ilu:
                self.ilu_ops[ILU_NAMES[ilu]] += 1
            if mac and ilu:
                self.dual_issue += 1
            if mac == MAC_ARL:
                self.arl_instructions += 1
                has_arl = True
            for name in validate_instruction(instruction):
                self.defects[name] += 1
            is_last = position == len(instructions) - 1
            if bool(instruction.get("final")) != is_last:
                self.final_bit_misplaced += 1
            if MUX_CONST in (instruction.mux_of(o) for o in instruction.operands_read):
                index = instruction.get("const")
                constants.add(index)
                self.constant_reads += 1
                reads_constants = True
                self.highest_constant = max(self.highest_constant, index)
                if instruction.get("relative"):
                    self.relative_reads += 1
                    has_relative = True
            if MUX_INPUT in (instruction.mux_of(o) for o in instruction.operands_read):
                self.input_registers[instruction.get("input")] += 1
            self._count_outputs(instruction)
        self.programs_with_arl += has_arl
        self.programs_with_relative += has_relative
        self.programs_reading_constants += reads_constants
        self.distinct_constants[len(constants)] += 1

    def _count_outputs(self, instruction: Instruction) -> None:
        if not instruction.get("out_o_mask"):
            return
        if not instruction.get("out_to_output"):
            self.constant_file_writes += 1
            return
        address = instruction.get("out_address")
        self.output_addresses[address] += 1
        if address not in VALID_OUTPUT_ADDRESSES:
            self.reserved_output_writes += 1


def characterise(programs: Sequence[Sequence[Instruction]]) -> Characterisation:
    result = Characterisation()
    for program in programs:
        result.add_program(program)
    return result


def _random_word(rng: random.Random, *, zero_first: bool) -> Instruction:
    first = 0 if zero_first else rng.getrandbits(32)
    return Instruction((first, rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32)))


def null_pass_rate(trials: int, seed: int, *, zero_first: bool = False) -> float:
    """Fraction of uniformly random 128-bit words that pass `is_plausible`.

    The null model for the structural rules: what a misplaced field, or random data,
    would score. Seeded, so it reproduces. Dword 0 must be zero, which random data
    fails with probability 1 - 2**-32 and which dominates the answer, so
    `zero_first` forces it to zero and isolates the opcode and multiplexer rules.
    """
    rng = random.Random(seed)
    passed = sum(is_plausible(_random_word(rng, zero_first=zero_first)) for _ in range(trials))
    return passed / trials


@dataclass(frozen=True)
class ArlCoincidence:
    """How well the ARL opcode and the relative-addressing bit agree across programs.

    Two fields read from different dwords that were each located by the same layout
    reading. If either were misplaced, relative constant reads would be scattered over
    the programs without regard to whether they contain an address-register load, and
    this reports how far from that the data is.
    """

    relative_reads: int
    relative_reads_in_arl_programs: int
    instructions_in_arl_programs: int
    instructions: int

    @property
    def null_probability(self) -> float:
        """P(every relative read lands in an ARL program) if reads were placed uniformly
        over all instructions. Each read is an independent draw, which is an approximation
        because the reads cluster by program, so this is a rough scale and not a p-value."""
        if not self.instructions:
            return 1.0
        share = self.instructions_in_arl_programs / self.instructions
        return share**self.relative_reads


def arl_coincidence(programs: Sequence[Sequence[Instruction]]) -> ArlCoincidence:
    relative = in_arl = arl_instructions = total = 0
    for program in programs:
        reads = sum(
            1
            for instruction in program
            if instruction.get("relative")
            and MUX_CONST in (instruction.mux_of(o) for o in instruction.operands_read)
        )
        has_arl = any(instruction.get("mac") == MAC_ARL for instruction in program)
        relative += reads
        total += len(program)
        if has_arl:
            in_arl += reads
            arl_instructions += len(program)
    return ArlCoincidence(relative, in_arl, arl_instructions, total)


def shuffled_dword_scan(data: bytes, seed: int, minimum: int = 1) -> list[ProgramSpan]:
    """`find_programs` over `data` with its dwords randomly permuted.

    The control for the scan: the same dword VALUES, so the same zero density and the
    same value distribution, with every structural relationship between neighbours
    destroyed. A scan that still finds programs here is finding coincidence.
    """
    count = len(data) // 4
    words = list(struct.unpack_from(f"<{count}I", data))
    random.Random(seed).shuffle(words)
    return find_programs(struct.pack(f"<{count}I", *words), minimum=minimum)
