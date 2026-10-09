# SPDX-License-Identifier: GPL-3.0-or-later
"""Encode and decode Direct3D 8 `vs.1.1` binary tokens: the INPUT the title's assembler takes.

The token format is the public Direct3D 8 vertex-shader token layout (documented in the
SDK help and mirrored by Wine's d3d8 headers, which were not consulted for this file).
Written from the documented bit layout:

  version     0xFFFE0101
  instruction opcode in bits 0-15 (comments use 0xFFFE with a size in bits 16-30)
  END         0x0000FFFF
  destination bit 31 set, register number bits 0-10, type bits 28-30, relative bit 13,
              write mask bits 16-19 (x lowest), result modifier bits 20-23
  source      bit 31 set, register number bits 0-10, type bits 28-30, relative bit 13,
              swizzle bits 16-23 (x in the LOW pair), modifier bits 24-27 (1 = negate)
  types       temp 0, input 1, constant 2, address 3, rasterizer out 4, colour out 5,
              texcoord out 6. Rasterizer out numbers: position 0, fog 1, point size 2.

This is the surface used to drive the title's own assembler with source we write, so the
(source mnemonic, emitted microcode) pairs are measured from the binary. The encoder is
checked by the assembler itself accepting it and emitting the operation each mnemonic
means (`tests/test_nv2a_evidence.py`, which needs the XBE), and by hand-derived tokens in
`tests/test_nv2a_d3dtokens.py`.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

VERSION = 0xFFFE0101
END = 0x0000FFFF

#: D3DSIO opcodes of the vs.1.1 instruction set.
OPCODES = {
    "nop": 0,
    "mov": 1,
    "add": 2,
    "sub": 3,
    "mad": 4,
    "mul": 5,
    "rcp": 6,
    "rsq": 7,
    "dp3": 8,
    "dp4": 9,
    "min": 10,
    "max": 11,
    "slt": 12,
    "sge": 13,
    "exp": 14,
    "log": 15,
    "lit": 16,
    "dst": 17,
    "lrp": 18,
    "frc": 19,
    "m4x4": 20,
    "m4x3": 21,
    "m3x4": 22,
    "m3x3": 23,
    "m3x2": 24,
    "expp": 78,
    "logp": 79,
}
#: Source operands each opcode takes.
SOURCE_COUNT = {
    "nop": 0,
    "mov": 1,
    "rcp": 1,
    "rsq": 1,
    "exp": 1,
    "log": 1,
    "expp": 1,
    "logp": 1,
    "frc": 1,
    "lit": 1,
    "mad": 3,
    "lrp": 3,
}
TYPE_TEMP, TYPE_INPUT, TYPE_CONST, TYPE_ADDR, TYPE_RAST, TYPE_COLOR, TYPE_TEXCOORD = range(7)
_RAST = {"oPos": 0, "oFog": 1, "oPts": 2}
_COLOR = {"oD0": 0, "oD1": 1}
_COMPONENTS = "xyzw"

_REGISTER = re.compile(
    r"^(?P<neg>-)?(?P<name>[a-zA-Z]+)(?P<index>\d*)"
    r"(?:\[a0\.x(?:\+(?P<offset>\d+))?\])?(?:\.(?P<sw>[xyzw]{1,4}))?$"
)


@dataclass(frozen=True)
class Operand:
    kind: int
    number: int
    relative: bool
    swizzle: str
    negate: bool


def parse_operand(text: str) -> Operand:
    """`r2.yzwx`, `-v5`, `c7`, `c[a0.x+5]`, `oPos.xy`, `a0.x`."""
    text = text.strip()
    text = re.sub(r"^(-?)c\[a0\.x(?:\+(\d+))?\]", lambda m: f"{m[1]}c{m[2] or 0}[a0.x]", text)
    match = re.match(
        r"^(?P<neg>-)?(?P<name>a0|oPos|oFog|oPts|oD[01]|oT[0-3]|[rvc])(?P<index>\d*)"
        r"(?P<rel>\[a0\.x\])?(?:\.(?P<sw>[xyzw]{1,4}))?$",
        text,
    )
    if not match:
        raise ValueError(f"cannot parse operand {text!r}")
    name, index = match["name"], int(match["index"] or 0)
    if name == "r":
        kind, number = TYPE_TEMP, index
    elif name == "v":
        kind, number = TYPE_INPUT, index
    elif name == "c":
        kind, number = TYPE_CONST, index
    elif name == "a0":
        kind, number = TYPE_ADDR, 0
    elif name in _RAST:
        kind, number = TYPE_RAST, _RAST[name]
    elif name in _COLOR:
        kind, number = TYPE_COLOR, _COLOR[name]
    else:
        kind, number = TYPE_TEXCOORD, int(name[2])
    return Operand(kind, number, bool(match["rel"]), match["sw"] or "", bool(match["neg"]))


def _swizzle_bits(swizzle: str) -> int:
    if not swizzle:
        swizzle = "xyzw"
    swizzle = swizzle + swizzle[-1] * (4 - len(swizzle))
    return sum(_COMPONENTS.index(letter) << (2 * place) for place, letter in enumerate(swizzle))


def _mask_bits(swizzle: str) -> int:
    if not swizzle:
        return 0xF
    return sum(1 << _COMPONENTS.index(letter) for letter in swizzle)


def encode_destination(operand: Operand) -> int:
    return 1 << 31 | operand.kind << 28 | _mask_bits(operand.swizzle) << 16 | operand.number


def encode_source(operand: Operand) -> int:
    return (
        1 << 31
        | operand.kind << 28
        | int(operand.negate) << 24
        | _swizzle_bits(operand.swizzle) << 16
        | int(operand.relative) << 13
        | operand.number
    )


def instruction(text: str) -> list[int]:
    """Tokens for one instruction line, e.g. `mad r0.xy, r2, v5, c7`."""
    mnemonic, _, rest = text.strip().partition(" ")
    operands = [parse_operand(part) for part in re.split(r",\s*(?![^\[]*\])", rest)] if rest else []
    tokens = [OPCODES[mnemonic]]
    if operands:
        tokens.append(encode_destination(operands[0]))
        tokens.extend(encode_source(operand) for operand in operands[1:])
    return tokens


def program(lines: list[str]) -> bytes:
    """A whole `vs.1.1` token stream as little-endian bytes, ending with END."""
    tokens = [VERSION]
    for line in lines:
        tokens.extend(instruction(line))
    tokens.append(END)
    return b"".join(token.to_bytes(4, "little") for token in tokens)
