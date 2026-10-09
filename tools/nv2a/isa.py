# SPDX-License-Identifier: GPL-3.0-or-later
"""NV2A vertex-program instruction decoder. Field positions only, no semantics.

The ISA is public hardware knowledge. The positions are written here as `(dword, low
bit, width)` of the in-memory 128-bit instruction. They were cross-read, not copied,
from two independent routes: the envytools "XF instruction set" Kelvin table (a 92-bit
word, bit 0 = FINAL, converted below) and the title's own assembler, whose emitted
words this table decodes back to the source operands (`tests/test_nv2a_isa.py`, and
`tools/nv2a/evidence.py`). The earlier `tools/shaderscan/vsh.py` table is a third copy
and agrees; this module does not import it so the two stay independent.

Conversion from the envytools numbering: the 92 bits live in dwords 1 to 3 with
envytools bit 0 at dword 3 bit 0, so envytools bit `n` is dword `3 - n // 32`, bit `n % 32`.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

INSTRUCTION_BYTES = 16
HEADER_VERSION = 0x2078

#: field -> (dword, low bit, width). dword 0 is unused (always zero in measured programs).
FIELDS: dict[str, tuple[int, int, int]] = {
    "final": (3, 0, 1),
    "relative": (3, 1, 1),  # constant read adds A0.x (envytools XFCTX_INDEXED)
    "out_is_ilu": (3, 2, 1),  # output register written by the scalar unit (OUT_IS_SCA)
    "out_address": (3, 3, 8),
    "out_to_output": (3, 11, 1),  # 1 output register file, 0 constant file (OUT_TARGET)
    "out_mask": (3, 12, 4),
    "ilu_mask": (3, 16, 4),  # scalar-unit write mask on the temporary
    "temp_out": (3, 20, 4),
    "mac_mask": (3, 24, 4),  # vector-unit write mask on the temporary
    "c_mux": (3, 28, 2),
    "c_temp_low": (3, 30, 2),
    "c_temp_high": (2, 0, 2),
    "c_neg": (2, 10, 1),
    "c_swizzle": (2, 2, 8),
    "b_mux": (2, 11, 2),
    "b_temp": (2, 13, 4),
    "b_swizzle": (2, 17, 8),
    "b_neg": (2, 25, 1),
    "a_mux": (2, 26, 2),
    "a_temp": (2, 28, 4),
    "a_swizzle": (1, 0, 8),
    "a_neg": (1, 8, 1),
    "input": (1, 9, 4),
    "const": (1, 13, 8),
    "mac": (1, 21, 4),
    "ilu": (1, 25, 3),
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

#: Operands each unit reads. ADD reads A and C (the unit is A*B+C), MUL reads A and B.
MAC_READS = {
    "NOP": "",
    "MOV": "A",
    "MUL": "AB",
    "ADD": "AC",
    "MAD": "ABC",
    "DP3": "AB",
    "DPH": "AB",
    "DP4": "AB",
    "DST": "AB",
    "MIN": "AB",
    "MAX": "AB",
    "SLT": "AB",
    "SGE": "AB",
    "ARL": "A",
}

MUX_NONE, MUX_TEMP, MUX_INPUT, MUX_CONST = 0, 1, 2, 3

#: Output register addresses. Names are the D3D `oXXX` registers. Address 0 is the
#: position (measured: written with xyzw component masks), 3 and 4 the colours, and so on.
OUTPUT_NAMES = {
    0: "oPos",
    3: "oD0",
    4: "oD1",
    5: "oFog",
    6: "oPts",
    7: "oB0",
    8: "oB1",
    9: "oT0",
    10: "oT1",
    11: "oT2",
    12: "oT3",
}
OUTPUT_COUNT = 16
CONSTANT_COUNT = 192
INPUT_COUNT = 16
TEMP_COUNT = 16


class DecodeError(ValueError):
    """The words are not a well-formed program."""


@dataclass(frozen=True)
class Source:
    """One operand: which register file, swizzle and negate. `swizzle[i]` is the source
    component (0 x, 1 y, 2 z, 3 w) that feeds result component i."""

    mux: int
    temp: int
    swizzle: tuple[int, int, int, int]
    negate: bool


@dataclass(frozen=True)
class Decoded:
    """One decoded instruction."""

    mac: int
    ilu: int
    const: int
    input: int
    a: Source
    b: Source
    c: Source
    mac_mask: int
    ilu_mask: int
    temp_out: int
    out_mask: int
    out_to_output: bool
    out_address: int
    out_is_ilu: bool
    relative: bool
    final: bool
    dword0: int

    @property
    def mac_name(self) -> str:
        return MAC_NAMES[self.mac] if self.mac < len(MAC_NAMES) else f"MAC{self.mac}"

    @property
    def ilu_name(self) -> str:
        return ILU_NAMES[self.ilu]

    def source(self, operand: str) -> Source:
        return {"A": self.a, "B": self.b, "C": self.c}[operand]


def _field(words: tuple[int, int, int, int], name: str) -> int:
    dword, low, width = FIELDS[name]
    return (words[dword] >> low) & ((1 << width) - 1)


def _swizzle(value: int) -> tuple[int, int, int, int]:
    """x is the HIGH pair of bits (so identity xyzw is 0b00_01_10_11, 0x1b)."""
    return (value >> 6 & 3, value >> 4 & 3, value >> 2 & 3, value & 3)


def decode_instruction(words: tuple[int, int, int, int]) -> Decoded:
    def source(prefix: str, temp: int) -> Source:
        return Source(
            _field(words, f"{prefix}_mux"),
            temp,
            _swizzle(_field(words, f"{prefix}_swizzle")),
            bool(_field(words, f"{prefix}_neg")),
        )

    c_temp = _field(words, "c_temp_high") << 2 | _field(words, "c_temp_low")
    return Decoded(
        mac=_field(words, "mac"),
        ilu=_field(words, "ilu"),
        const=_field(words, "const"),
        input=_field(words, "input"),
        a=source("a", _field(words, "a_temp")),
        b=source("b", _field(words, "b_temp")),
        c=source("c", c_temp),
        mac_mask=_field(words, "mac_mask"),
        ilu_mask=_field(words, "ilu_mask"),
        temp_out=_field(words, "temp_out"),
        out_mask=_field(words, "out_mask"),
        out_to_output=bool(_field(words, "out_to_output")),
        out_address=_field(words, "out_address"),
        out_is_ilu=bool(_field(words, "out_is_ilu")),
        relative=bool(_field(words, "relative")),
        final=bool(_field(words, "final")),
        dword0=words[0],
    )


def decode_program(data: bytes) -> list[Decoded]:
    """Decode a headed program (header dword, then instructions) or a bare one.

    A headed program is recognised by a first dword of `count << 16 | 0x2078` whose
    count matches the length. Anything else must be a whole number of instructions.
    """
    if len(data) >= 4 and len(data) % INSTRUCTION_BYTES == 4:
        (header,) = struct.unpack_from("<I", data)
        if header & 0xFFFF == HEADER_VERSION and header >> 16 == (len(data) - 4) // 16:
            data = data[4:]
    if len(data) % INSTRUCTION_BYTES:
        raise DecodeError(f"{len(data)} bytes is not a whole number of instructions")
    return [
        decode_instruction(struct.unpack_from("<4I", data, offset))
        for offset in range(0, len(data), INSTRUCTION_BYTES)
    ]


def encode_instruction(**fields: int) -> tuple[int, int, int, int]:
    """Pack named fields (see `FIELDS`) into four dwords. Used to build test inputs
    from the layout, so tests can state expected words independently."""
    words = [0, 0, 0, 0]
    for name, value in fields.items():
        dword, low, width = FIELDS[name]
        if value >> width:
            raise ValueError(f"{name}={value} does not fit {width} bits")
        words[dword] |= value << low
    return (words[0], words[1], words[2], words[3])
