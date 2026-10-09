# SPDX-License-Identifier: GPL-3.0-or-later
"""ROUTE A: read the field meanings off the title's own pixel-shader assembler.

The assembler (`XGAssembleShader`, run under emulation by `tools.shaderscan.assemble`)
turns `xps.1.1` source into the 240-byte definition. A source construct with a known
meaning, assembled, shows which bits mean it. Two kinds of check come out of that:

  FACTS      "this construct sets this field of this word to this value". Each is paired
             with the documentation's name for the value (`docroute`), so one run compares
             the assembler's bits with the documents' layout.
  SEMANTICS  "this construct computes this". The construct is assembled, decoded with
             `config.decode`, evaluated with `reference.evaluate`, and compared with the
             Direct3D pixel-shader meaning of the construct, written independently below
             in plain Python. This is what pins the REFERENCE MODEL's mapping, scale, mux
             and dot-product behaviour to the binary and not to the model's own text.

Nothing derived from the executable is committed. The sources here are written by us, the
constructs are public Direct3D 8 pixel-shader syntax, and only a pass or fail leaves.
"""

from __future__ import annotations

import random
import struct
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from tools.nv2a_combiner import config as cfg
from tools.nv2a_combiner import docroute, reference
from tools.nv2a_combiner.validate import Case, make_cases
from tools.shaderscan.assemble import AssemblerEmulator, AssemblerSpec
from tools.shaderscan.image import Image

HEADER = "xps.1.1\n"
#: A final combiner that passes r0 straight out: D = r0, G = r0.a, A = B = C = 0.
PASS_FINAL = "xfc zero, zero, zero, r0, zero, zero, r0.a"

Vec4 = tuple[float, float, float, float]


class Oracle:
    """The title's assembler as a function from source text to a definition."""

    def __init__(self, image: Image, spec: AssemblerSpec) -> None:
        self._assembler = AssemblerEmulator(image, spec)

    @classmethod
    def load(cls, xbe: Path, entry: int, heap_alloc: int, heap_free: int) -> Oracle:
        return cls(Image.load(xbe), AssemblerSpec(entry, heap_alloc, heap_free))

    def assemble(self, body: str) -> bytes | None:
        """The 240-byte definition for `body`, or None when the assembler rejects it."""
        result = self._assembler.assemble((HEADER + body).encode(), 0)
        if result is None or result.kernel_call is not None or len(result.data) != 240:
            return None
        return result.data


# ---------------------------------------------------------------------------------------
# FACTS: construct -> field -> value name, compared against the documents.
# ---------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Fact:
    """`body` assembled puts `value_name` in `field_name` of `block`[`index`]."""

    body: str
    block: str
    index: int
    field_name: str
    value_name: str


def _with(final: str, *lines: str) -> str:
    return "\n".join([*lines, final])


def _operand_fact(
    line: str, block: str, letter: str, kind: str, name: str, *, index: int = 0
) -> Fact:
    return Fact(_with(PASS_FINAL, line), block, index, f"{letter}.{kind}", name)


FACTS: tuple[Fact, ...] = (
    # Input mappings, one construct each, the A operand of `mov r0, <operand>`.
    _operand_fact("mov r0, v0", "COLOR_ICW", "A", "mapping", "SIGNED_IDENTITY"),
    _operand_fact("mov r0, -v0", "COLOR_ICW", "A", "mapping", "SIGNED_NEGATE"),
    _operand_fact("mov r0, 1-v0", "COLOR_ICW", "A", "mapping", "UNSIGNED_INVERT"),
    _operand_fact("mov r0, v0_bx2", "COLOR_ICW", "A", "mapping", "EXPAND_NORMAL"),
    _operand_fact("mov r0, -v0_bx2", "COLOR_ICW", "A", "mapping", "EXPAND_NEGATE"),
    _operand_fact("mov r0, v0_bias", "COLOR_ICW", "A", "mapping", "HALF_BIAS_NORMAL"),
    _operand_fact("mov r0, -v0_bias", "COLOR_ICW", "A", "mapping", "HALF_BIAS_NEGATE"),
    # The interpolant of lrp is an unsigned operand.
    _operand_fact("lrp r0, v0, t0, t1", "COLOR_ICW", "A", "mapping", "UNSIGNED_IDENTITY"),
    # Sources.
    _operand_fact("mov r0, v1", "COLOR_ICW", "A", "source", "SECONDARY_COLOR"),
    _operand_fact("mov r0, fog", "COLOR_ICW", "A", "source", "FOG"),
    _operand_fact("mov r0, t3", "COLOR_ICW", "A", "source", "TEXTURE3"),
    _operand_fact("mov r0, v0.a", "COLOR_ICW", "A", "usage", "ALPHA"),
    _operand_fact("mov r0, v0", "COLOR_ICW", "A", "usage", "RGB"),
    # Output destinations: mul writes the AB product, mad the sum.
    Fact(_with(PASS_FINAL, "mul r0, v0, v1"), "COLOR_OCW", 0, "AB_DST", "SPARE0"),
    Fact(_with(PASS_FINAL, "mul r0, v0, v1"), "COLOR_OCW", 0, "SUM_DST", "ZERO"),
    Fact(_with(PASS_FINAL, "mad r0, v0, v1, c0"), "COLOR_OCW", 0, "SUM_DST", "SPARE0"),
    Fact(_with(PASS_FINAL, "mad r0, v0, v1, c0"), "COLOR_OCW", 0, "AB_DST", "ZERO"),
    # Output scale.
    Fact(_with(PASS_FINAL, "mul_x2 r0, v0, v1"), "COLOR_OCW", 0, "OP", "SHIFTLEFTBY1"),
    Fact(_with(PASS_FINAL, "mul_x4 r0, v0, v1"), "COLOR_OCW", 0, "OP", "SHIFTLEFTBY2"),
    Fact(_with(PASS_FINAL, "mul_d2 r0, v0, v1"), "COLOR_OCW", 0, "OP", "SHIFTRIGHTBY1"),
    Fact(_with(PASS_FINAL, "mul r0, v0, v1"), "COLOR_OCW", 0, "OP", "NOSHIFT"),
    # Dot product is the AB product, and writing alpha too sets blue-to-alpha.
    Fact(_with(PASS_FINAL, "dp3 r0.rgb, v0, v1"), "COLOR_OCW", 0, "AB_DOT_ENABLE", "ENABLED"),
    Fact(_with(PASS_FINAL, "dp3 r0.rgb, v0, v1"), "COLOR_OCW", 0, "BLUETOALPHA_AB", "DISABLED"),
    Fact(_with(PASS_FINAL, "dp3 r0, v0, v1"), "COLOR_OCW", 0, "BLUETOALPHA_AB", "ENABLED"),
    Fact(_with(PASS_FINAL, "mul r0, v0, v1"), "COLOR_OCW", 0, "AB_DOT_ENABLE", "DISABLED"),
    # cnd is the mux.
    Fact(_with(PASS_FINAL, "cnd r0, r0, t0, t1"), "COLOR_OCW", 0, "MUX_ENABLE", "ENABLED"),
    Fact(_with(PASS_FINAL, "cnd r0, r0, t0, t1"), "COLOR_OCW", 0, "SUM_DST", "SPARE0"),
    Fact(_with(PASS_FINAL, "mad r0, v0, v1, c0"), "COLOR_OCW", 0, "MUX_ENABLE", "DISABLED"),
    # The alpha half sits at its own block.
    Fact(_with(PASS_FINAL, "mul r0.a, v0, v1"), "ALPHA_OCW", 0, "AB_DST", "SPARE0"),
    Fact(_with(PASS_FINAL, "mul r0.a, v0, v1"), "COLOR_OCW", 0, "AB_DST", "ZERO"),
    # Control word.
    Fact(_with(PASS_FINAL, "mov r0, v0"), "COMBINER_CONTROL", 0, "ITERATION_COUNT", "1"),
    Fact(
        _with(PASS_FINAL, "mov r1, v0", "mov r0, r1"), "COMBINER_CONTROL", 0, "ITERATION_COUNT", "2"
    ),
    Fact(_with(PASS_FINAL, "mov r0, v0"), "COMBINER_CONTROL", 0, "MUX_SELECT", "MSB"),
    Fact(_with(PASS_FINAL, "mov r0, v0"), "COMBINER_CONTROL", 0, "FACTOR0", "EACH_STAGE"),
    Fact(_with(PASS_FINAL, "mov r0, v0"), "COMBINER_CONTROL", 0, "FACTOR1", "EACH_STAGE"),
    # Texture stage programs.
    Fact(
        _with(PASS_FINAL, "tex t0", "mov r0, t0"),
        "SHADER_STAGE_PROGRAM",
        0,
        "STAGE0",
        "2D_PROJECTIVE",
    ),
    Fact(
        _with(PASS_FINAL, "tex t1", "mov r0, t0"),
        "SHADER_STAGE_PROGRAM",
        0,
        "STAGE1",
        "2D_PROJECTIVE",
    ),
    Fact(
        _with(PASS_FINAL, "tex t3", "mov r0, t0"),
        "SHADER_STAGE_PROGRAM",
        0,
        "STAGE3",
        "2D_PROJECTIVE",
    ),
    Fact(
        _with(PASS_FINAL, "texcoord t0", "mov r0, t0"),
        "SHADER_STAGE_PROGRAM",
        0,
        "STAGE0",
        "PASS_THROUGH",
    ),
    Fact(
        _with(PASS_FINAL, "texkill t0", "mov r0, t0"),
        "SHADER_STAGE_PROGRAM",
        0,
        "STAGE0",
        "CLIP_PLANE",
    ),
    Fact(
        _with(PASS_FINAL, "tex t0", "texreg2gb t1, t0", "mov r0, t0"),
        "SHADER_STAGE_PROGRAM",
        0,
        "STAGE1",
        "DEPENDENT_GB",
    ),
    Fact(
        _with(PASS_FINAL, "tex t0", "texreg2ar t1, t0", "mov r0, t0"),
        "SHADER_STAGE_PROGRAM",
        0,
        "STAGE1",
        "DEPENDENT_AR",
    ),
    Fact(
        _with(PASS_FINAL, "tex t0", "texm3x2pad t1, t0", "texm3x2tex t2, t0", "mov r0, t0"),
        "SHADER_STAGE_PROGRAM",
        0,
        "STAGE1",
        "DOT_PRODUCT",
    ),
    Fact(
        _with(PASS_FINAL, "tex t0", "texm3x2pad t1, t0", "texm3x2tex t2, t0", "mov r0, t0"),
        "SHADER_STAGE_PROGRAM",
        0,
        "STAGE2",
        "DOT_ST",
    ),
    # Final combiner.
    Fact(
        "mov r0, v0\nxfc v1, r1, c0, t0, zero, zero, r0.a",
        "SPECULAR_FOG_CW0",
        0,
        "A.source",
        "SECONDARY_COLOR",
    ),
    Fact(
        "mov r0, v0\nxfc v1, r1, c0, t0, zero, zero, r0.a",
        "SPECULAR_FOG_CW0",
        0,
        "B.source",
        "SPARE1",
    ),
    Fact(
        "mov r0, v0\nxfc v1, r1, c0, t0, zero, zero, r0.a",
        "SPECULAR_FOG_CW0",
        0,
        "C.source",
        "CONSTANT_COLOR0",
    ),
    Fact(
        "mov r0, v0\nxfc v1, r1, c0, t0, zero, zero, r0.a",
        "SPECULAR_FOG_CW0",
        0,
        "D.source",
        "TEXTURE0",
    ),
    Fact(
        "mov r0, v0\nxfc zero, zero, zero, sum, zero, zero, r0.a",
        "SPECULAR_FOG_CW0",
        0,
        "D.source",
        "SPARE0_PLUS_SECONDARY_COLOR",
    ),
    Fact(
        "mov r0, v0\nxfc zero, zero, zero, prod, c0, c1, r0.a",
        "SPECULAR_FOG_CW0",
        0,
        "D.source",
        "E_TIMES_F",
    ),
    Fact(
        "mov r0, v0\nxfc zero, zero, zero, 1-r0, zero, zero, r0.a",
        "SPECULAR_FOG_CW0",
        0,
        "D.mapping",
        "UNSIGNED_INVERT",
    ),
    Fact(
        "mov r0, v0\nxfc zero, zero, zero, zero, c0, c1, r0.a",
        "SPECULAR_FOG_CW1",
        0,
        "E.source",
        "CONSTANT_COLOR0",
    ),
    Fact(
        "mov r0, v0\nxfc zero, zero, zero, zero, c0, c1, r0.a",
        "SPECULAR_FOG_CW1",
        0,
        "F.source",
        "CONSTANT_COLOR1",
    ),
    Fact(
        "mov r0, v0\nxfc zero, zero, zero, zero, zero, zero, r1.a",
        "SPECULAR_FOG_CW1",
        0,
        "G.source",
        "SPARE1",
    ),
    Fact(
        "mov r0, v0\nxfc zero, zero, zero, zero, zero, zero, r1.a",
        "SPECULAR_FOG_CW1",
        0,
        "G.usage",
        "ALPHA",
    ),
    Fact(
        "mov r0, v0\nxfc zero, zero, zero, zero, zero, zero, r1.b",
        "SPECULAR_FOG_CW1",
        0,
        "G.usage",
        "BLUE",
    ),
    Fact(
        "mov r0, v0\nxfc zero, zero, zero, r0, zero, zero, r0.a",
        "SPECULAR_FOG_CW1",
        0,
        "SPECULAR_CLAMP",
        "ENABLED",
    ),
)

_ENABLE = {"ENABLED": 1, "DISABLED": 0}
_USAGE = {"RGB": 0, "BLUE": 0, "ALPHA": 1}


def doc_value(field_name: str, value_name: str) -> int:
    """The number the documents give `value_name` in a field."""
    if value_name in _ENABLE:
        return _ENABLE[value_name]
    if field_name.endswith(".usage"):
        return _USAGE[value_name]
    if field_name.endswith(".mapping"):
        return {name: code for code, name in docroute.MAPPINGS.items()}[value_name]
    if field_name.endswith(".source") or field_name.endswith("_DST"):
        return {name: code for code, name in docroute.SOURCES.items()}[value_name]
    if field_name == "OP":
        return docroute.OP_VALUES[value_name]
    if field_name == "MUX_SELECT":
        return docroute.MUX_SELECT_VALUES[value_name]
    if field_name in ("FACTOR0", "FACTOR1"):
        return docroute.FACTOR_VALUES[value_name]
    if field_name.startswith("STAGE"):
        return docroute.STAGE_PROGRAMS[value_name]
    return int(value_name)


def doc_field(dwords: list[int], fact: Fact) -> int:
    """What the documents' layout reads out of the assembled words for `fact`."""
    first, _ = docroute.BLOCKS[fact.block]
    word = dwords[first + fact.index]
    name = fact.field_name
    if "." in name:
        letter, kind = name.split(".")
        if fact.block == "SPECULAR_FOG_CW1":
            return docroute.final1_operand(word, letter, kind)
        return docroute.operand(word, letter, kind)
    if name == "SPECULAR_CLAMP":
        return docroute.field(word, docroute.FINAL1_FLAGS[name], docroute.FINAL1_FLAGS[name])
    if name.startswith("STAGE"):
        stage = int(name[5:])
        bits = docroute.STAGE_PROGRAM_BITS
        return docroute.field(word, bits * stage + bits - 1, bits * stage)
    table = docroute.CONTROL_FIELDS if fact.block == "COMBINER_CONTROL" else docroute.OUTPUT_FIELDS
    high, low = table[name]
    return docroute.field(word, high, low)


def check_fact(oracle: Oracle, fact: Fact) -> tuple[bool, int | None, int]:
    """(agrees, what the assembler's bits read as, what the documents expect)."""
    expected = doc_value(fact.field_name, fact.value_name)
    block = oracle.assemble(fact.body)
    if block is None:
        return False, None, expected
    seen = doc_field(cfg.words(block), fact)
    return seen == expected, seen, expected


# ---------------------------------------------------------------------------------------
# SEMANTICS: Direct3D meaning of a construct versus decode + reference.evaluate.
# ---------------------------------------------------------------------------------------


def _scale(vector: Vec4, factor: float) -> Vec4:
    return (vector[0] * factor, vector[1] * factor, vector[2] * factor, vector[3] * factor)


def _each(function: Callable[..., float], *vectors: Vec4) -> Vec4:
    return tuple(function(*channel) for channel in zip(*vectors, strict=True))  # type: ignore[return-value]


@dataclass(frozen=True)
class Environment:
    """What an instruction can read: the colours, the virtual constants, one fog colour."""

    v0: Vec4
    v1: Vec4
    c: tuple[Vec4, ...]
    fog: Vec4


@dataclass(frozen=True)
class Construct:
    """A pixel-shader body and the value of r0 it leaves, by Direct3D's rules."""

    name: str
    body: str
    expect: Callable[[Environment], Vec4]


def _bx2(x: float) -> float:
    return 2.0 * (x - 0.5)


def _bias(x: float) -> float:
    return x - 0.5


def _all(vector: Vec4, function: Callable[[float], float]) -> Vec4:
    return (function(vector[0]), function(vector[1]), function(vector[2]), function(vector[3]))


CONSTRUCTS: tuple[Construct, ...] = (
    Construct("mov", "mov r0, v0", lambda e: e.v0),
    Construct("mov 1-x", "mov r0, 1-v0", lambda e: _all(e.v0, lambda x: 1.0 - x)),
    Construct("mov -x", "mov r0, -v0", lambda e: _all(e.v0, lambda x: -x)),
    Construct("mov x_bx2", "mov r0, v0_bx2", lambda e: _all(e.v0, _bx2)),
    Construct("mov -x_bx2", "mov r0, -v0_bx2", lambda e: _all(e.v0, lambda x: -_bx2(x))),
    Construct("mov x_bias", "mov r0, v0_bias", lambda e: _all(e.v0, _bias)),
    Construct("mov -x_bias", "mov r0, -v0_bias", lambda e: _all(e.v0, lambda x: -_bias(x))),
    Construct("mov x.a", "mov r0, v0.a", lambda e: (e.v0[3],) * 4),  # type: ignore[arg-type,return-value]
    Construct("mov x.b alpha only", "mov r0, v0.b", lambda e: (*e.v0[:3], e.v0[2])),
    Construct("mov c0", "mov r0, c0", lambda e: e.c[0]),
    Construct("mov c1", "mov r0, c1", lambda e: e.c[1]),
    Construct("mov fog", "mov r0, fog", lambda e: e.fog),
    Construct("add", "add r0, v0, v1", lambda e: _each(lambda a, b: a + b, e.v0, e.v1)),
    Construct("sub", "sub r0, v0, v1", lambda e: _each(lambda a, b: a - b, e.v0, e.v1)),
    Construct("mul", "mul r0, v0, v1", lambda e: _each(lambda a, b: a * b, e.v0, e.v1)),
    Construct(
        "mad", "mad r0, v0, v1, c0", lambda e: _each(lambda a, b, c: a * b + c, e.v0, e.v1, e.c[0])
    ),
    Construct(
        "lrp",
        "lrp r0, v0, v1, c0",
        lambda e: _each(lambda a, b, c: a * b + (1.0 - a) * c, e.v0, e.v1, e.c[0]),
    ),
    Construct(
        "cnd",
        "mov r0.a, v0\ncnd r0, r0, v1, c0",
        lambda e: e.v1 if e.v0[3] > 0.5 else e.c[0],
    ),
    Construct(
        "dp3 rgba",
        "dp3 r0, v0, v1",
        lambda e: (sum(a * b for a, b in zip(e.v0[:3], e.v1[:3], strict=True)),) * 4,  # type: ignore[arg-type,return-value]
    ),
    Construct(
        "dp3 rgb",
        "mov r0.a, c0\ndp3 r0.rgb, v0_bx2, v1_bx2",
        lambda e: (
            *((sum(_bx2(a) * _bx2(b) for a, b in zip(e.v0[:3], e.v1[:3], strict=True))),) * 3,
            e.c[0][3],
        ),
    ),
    Construct(
        "mul_x2", "mul_x2 r0, v0, v1", lambda e: _scale(_each(lambda a, b: a * b, e.v0, e.v1), 2.0)
    ),
    Construct(
        "mul_x4", "mul_x4 r0, v0, v1", lambda e: _scale(_each(lambda a, b: a * b, e.v0, e.v1), 4.0)
    ),
    Construct(
        "mul_d2", "mul_d2 r0, v0, v1", lambda e: _scale(_each(lambda a, b: a * b, e.v0, e.v1), 0.5)
    ),
    Construct(
        "add_x2", "add_x2 r0, v0, v1", lambda e: _scale(_each(lambda a, b: a + b, e.v0, e.v1), 2.0)
    ),
    Construct(
        "mad_x2 of bx2 operands",
        "mad_x2 r0, v0_bx2, v1, c0",
        lambda e: _scale(_each(lambda a, b, c: _bx2(a) * b + c, e.v0, e.v1, e.c[0]), 2.0),
    ),
    Construct(
        "write mask rgb then alpha",
        "mul r0.rgb, v0, v1\n+mul r0.a, c0, c1",
        lambda e: (*_each(lambda a, b: a * b, e.v0, e.v1)[:3], e.c[0][3] * e.c[1][3]),
    ),
    Construct(
        "two stages",
        "mul r1, v0, v1\nmad r0, r1, c0, v0",
        lambda e: _each(lambda a, b, c, d: a * b * c + d, e.v0, e.v1, e.c[0], e.v0),
    ),
)


def final_construct(body: str, expect: Callable[[Environment], Vec4]) -> Construct:
    return Construct("xfc " + body, "mov r0, c1\n" + body, expect)


def _complement(vector: Vec4) -> Vec4:
    return _all(vector, lambda x: 1.0 - x)


FINAL_CONSTRUCTS: tuple[Construct, ...] = (
    final_construct(
        "xfc v0, v1, c0, fog, zero, zero, r0.a",
        lambda e: tuple(  # type: ignore[arg-type,return-value]
            [min(1.0, e.fog[i] + (1 - e.v0[i]) * e.c[0][i] + e.v0[i] * e.v1[i]) for i in range(3)]
            + [e.c[1][3]]
        ),
    ),
    final_construct(
        "xfc 1-v0, v1, c0, fog, zero, zero, r0.a",
        lambda e: tuple(  # type: ignore[arg-type,return-value]
            [
                min(1.0, e.fog[i] + (1 - (1 - e.v0[i])) * e.c[0][i] + (1 - e.v0[i]) * e.v1[i])
                for i in range(3)
            ]
            + [e.c[1][3]]
        ),
    ),
    final_construct(
        "xfc zero, zero, zero, sum, zero, zero, v1.a",
        lambda e: (*(min(1.0, e.c[1][i] + e.v1[i]) for i in range(3)), e.v1[3]),  # type: ignore[arg-type,return-value]
    ),
    final_construct(
        "xfc zero, zero, zero, prod, v0, v1, v0.a",
        lambda e: (*(e.v0[i] * e.v1[i] for i in range(3)), e.v0[3]),  # type: ignore[arg-type,return-value]
    ),
    final_construct(
        "xfc sum, v1, zero, zero, zero, zero, v1.a",
        lambda e: (*(min(1.0, e.c[1][i] + e.v1[i]) * e.v1[i] for i in range(3)), e.v1[3]),  # type: ignore[arg-type,return-value]
    ),
    final_construct(
        "xfc 1-sum, v1, zero, zero, zero, zero, v1.a",
        lambda e: (  # type: ignore[arg-type,return-value]
            *((1 - min(1.0, e.c[1][i] + e.v1[i])) * e.v1[i] for i in range(3)),
            e.v1[3],
        ),
    ),
    final_construct(
        "xfc r0.a, v0, v1, zero, zero, zero, 1-r0.a",
        lambda e: (  # type: ignore[arg-type,return-value]
            *((1 - e.c[1][3]) * e.v1[i] + e.c[1][3] * e.v0[i] for i in range(3)),
            1 - e.c[1][3],
        ),
    ),
)


def _fill(block: bytes, env: Environment) -> reference.Fragment:
    """A fragment whose factor registers carry the virtual constants as the block maps them."""
    mapping = cfg.constant_map(block)
    zero: Vec4 = (0.0, 0.0, 0.0, 0.0)
    factor0 = tuple(env.c[i] if i is not None else zero for i in mapping.factor0)
    factor1 = tuple(env.c[i] if i is not None else zero for i in mapping.factor1)
    final = [env.c[i] if i is not None else zero for i in mapping.final]
    return reference.Fragment(
        v0=env.v0,
        v1=env.v1,
        fog_color=env.fog[:3],  # type: ignore[arg-type]
        fog_factor=env.fog[3],
        factor0=factor0,
        factor1=factor1,
        final_c0=final[0],
        final_c1=final[1],
    )


def random_environment(rng: random.Random) -> Environment:
    def vec() -> Vec4:
        return tuple(rng.randrange(256) / 255.0 for _ in range(4))  # type: ignore[return-value]

    return Environment(vec(), vec(), tuple(vec() for _ in range(8)), vec())


def check_construct(
    oracle: Oracle, construct: Construct, *, trials: int = 60, seed: int = 1
) -> tuple[bool, float]:
    """(assembled, worst error) of decode + reference.evaluate against the D3D meaning."""
    block = oracle.assemble(
        construct.body + "\n" + PASS_FINAL if "xfc" not in construct.body else construct.body
    )
    if block is None:
        return False, float("inf")
    config = cfg.decode(block)
    rng = random.Random(seed)
    worst = 0.0
    for _ in range(trials):
        env = random_environment(rng)
        got = reference.evaluate(config, _fill(block, env))
        want = construct.expect(env)
        worst = max(
            worst, *(abs(g - min(1.0, max(0.0, w))) for g, w in zip(got, want, strict=True))
        )
    return True, worst


def pack_floats(values: list[float]) -> bytes:
    return struct.pack(f"<{len(values)}f", *values)


__all__ = ["Case", "make_cases"]
