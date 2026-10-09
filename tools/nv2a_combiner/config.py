# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode a 60-dword pixel-shader block into a structured combiner configuration.

THE BLOCK. Dwords 0 to 56 are render-state indices 0 to 56 of
`src/gpu/d3d8_render_state_table.c`, except that definition dword 54 is the
texture-stage program: SetPixelShader emits it through method 0x1E70, whereas
SetRenderState(54, value) uses table method 0x1D90 (see docs/d3d8-usage.md section 10).
Dwords 57 to 59 are
host-side bookkeeping (which virtual `c#` constant feeds which stage) and never reach the
GPU. Every bit position below has TWO derivations, recorded in `docs/combiner-translator.md`
section 3. Route A reads them off the title's own assembler (`probe.py` assembles a source
construct and diffs the bits). Route B is the public documentation (the NV10/NV20
envytools register database, the nxdk register header, the OpenGL NV_register_combiners
specification), described by reference and written here from scratch. The two routes
agree on every layout field this module reads. The few places where they do not are
named in `UNSUPPORTED` and in the docs.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

BLOCK_DWORDS = 60
BLOCK_BYTES = BLOCK_DWORDS * 4
STATE_DWORDS = 57

# Dword index of each register block, equal to the render-state index.
IDX_ALPHA_ICW = 0
IDX_FINAL0 = 8
IDX_FINAL1 = 9
IDX_FACTOR0 = 10
IDX_FACTOR1 = 18
IDX_ALPHA_OCW = 26
IDX_COLOR_ICW = 34
IDX_CLIP_MODE = 42
IDX_FINAL_C0 = 43
IDX_FINAL_C1 = 44
IDX_COLOR_OCW = 45
IDX_CONTROL = 53
IDX_STAGE_PROGRAM = 54
IDX_DOT_MAPPING = 55
IDX_OTHER_INPUT = 56
# Host-side words after the 57 method values.
IDX_FACTOR0_MAP = 57
IDX_FACTOR1_MAP = 58
IDX_FINAL_MAP = 59

# Combiner registers, the 4-bit source and destination field.
REG_ZERO = 0
REG_C0 = 1
REG_C1 = 2
REG_FOG = 3
REG_V0 = 4
REG_V1 = 5
REG_T0 = 8
REG_SPARE0 = 12
REG_SPARE1 = 13
REG_SUM = 14
REG_PROD = 15

#: Registers a general stage may read: zero, constants, fog, the two colours, the four
#: textures and the two spares. 6 and 7 are not defined, 14 and 15 exist only in the final
#: combiner.
GENERAL_SOURCES = frozenset({0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13})
#: Registers a general stage may write. 0 discards the result.
GENERAL_DESTINATIONS = frozenset({0, 4, 5, 8, 9, 10, 11, 12, 13})
FINAL_SOURCES = GENERAL_SOURCES | {REG_SUM, REG_PROD}

# Input mapping codes, 3 bits.
MAP_UNSIGNED_IDENTITY = 0
MAP_UNSIGNED_INVERT = 1
MAP_EXPAND_NORMAL = 2
MAP_EXPAND_NEGATE = 3
MAP_HALF_BIAS_NORMAL = 4
MAP_HALF_BIAS_NEGATE = 5
MAP_SIGNED_IDENTITY = 6
MAP_SIGNED_NEGATE = 7

# Output operation codes, bits 17:15 of an output word: bit 15 is the -0.5 bias, bits
# 17:16 the scale (0: x1, 1: x2, 2: x4, 3: x1/2).
OUT_SCALE_FACTOR = (1.0, 2.0, 4.0, 0.5)

# Shader stage program codes, 5 bits per texture stage.
TEX_NONE = 0
TEX_2D_PROJECTIVE = 1
TEX_3D_PROJECTIVE = 2
TEX_CUBE_MAP = 3
TEX_PASS_THROUGH = 4
TEX_CLIP_PLANE = 5
TEX_DOT_ST = 9
TEX_DEPENDENT_AR = 15
TEX_DEPENDENT_GB = 16
TEX_DOT_PRODUCT = 17
SUPPORTED_TEXTURE_MODES = frozenset(
    {
        TEX_NONE,
        TEX_2D_PROJECTIVE,
        TEX_3D_PROJECTIVE,
        TEX_CUBE_MAP,
        TEX_PASS_THROUGH,
        TEX_CLIP_PLANE,
        TEX_DOT_ST,
        TEX_DEPENDENT_AR,
        TEX_DEPENDENT_GB,
        TEX_DOT_PRODUCT,
    }
)
#: Dot-product input mappings the translator models: 0 (rgb as is), 1 (signed, D3D), and
#: (T1490, xemu psh.c sign2 and sign3) 2 and 3.
SUPPORTED_DOT_MAPPINGS = frozenset({0, 1, 2, 3})
TEXTURE_MODE_NAMES = {
    0: "none",
    1: "2d_projective",
    2: "3d_projective",
    3: "cube_map",
    4: "pass_through",
    5: "clip_plane",
    6: "bump_env_map",
    7: "bump_env_map_luminance",
    8: "brdf",
    9: "dot_st",
    10: "dot_zw",
    11: "dot_reflect_diffuse",
    12: "dot_reflect_specular",
    13: "dot_str_3d",
    14: "dot_str_cube",
    15: "dependent_ar",
    16: "dependent_gb",
    17: "dot_product",
    18: "dot_reflect_specular_const",
}

# Combiner control word.
CONTROL_COUNT_MASK = 0xFF
CONTROL_MUX_MSB_BIT = 1 << 8
CONTROL_FACTOR0_EACH_BIT = 1 << 12
CONTROL_FACTOR1_EACH_BIT = 1 << 16

# Output word flag bits.
OUT_CD_DOT = 1 << 12
OUT_AB_DOT = 1 << 13
OUT_MUX = 1 << 14
OUT_BLUE_TO_ALPHA_CD = 1 << 18
OUT_BLUE_TO_ALPHA_AB = 1 << 19

# Final combiner word 1 flags.
FINAL_CLAMP_SUM = 1 << 7
FINAL_COMPLEMENT_V1 = 1 << 6
FINAL_COMPLEMENT_R0 = 1 << 5

#: Causes a block cannot be translated, keyed by the short name the corpus report uses.
UNSUPPORTED = {
    "stage_count": "combiner stage count outside 1 to 8",
    "texture_mode": "texture stage program needs dependent or bump texturing",
    "invalid_output_op": "output operation code 5 or 7 (x4 or x1/2 with bias, invalid by spec)",
    "bad_source": "a general stage reads register 6, 7, 14 or 15",
    "bad_destination": "a general stage writes a register that is not a colour or spare",
    "alpha_dot": "dot product or blue-to-alpha flag set on an alpha half",
    "bad_final_source": "the final combiner reads register 6 or 7",
    "clip_mode": "a clip-plane stage has a compare mode other than the default (kill when < 0)",
    "dot_mapping": "a dot-product stage uses an input mapping outside 0 to 3 (hilo or hemisphere)",
    "stage_order": "a dependent or dot-product stage reads a texture that is not an earlier stage",
}


_READS_EARLIER_STAGE = frozenset({TEX_DOT_ST, TEX_DOT_PRODUCT, TEX_DEPENDENT_AR, TEX_DEPENDENT_GB})


def input_stage(other_input: int, stage: int) -> int:
    """Which texture stage `stage` takes its input from. Stage 1 always reads stage 0."""
    if stage < 2:
        return 0
    return (other_input >> (16 + 4 * (stage - 2))) & 0xF


class UnsupportedConfig(ValueError):
    """The block decodes but cannot be translated. `cause` is a key of `UNSUPPORTED`."""

    def __init__(self, cause: str, detail: str = "") -> None:
        super().__init__(f"{cause}: {UNSUPPORTED[cause]}{' (' + detail + ')' if detail else ''}")
        self.cause = cause


@dataclass(frozen=True)
class Input:
    """One A, B, C or D operand: a register, a component selector and a mapping."""

    register: int
    #: True selects alpha (RGB half: alpha replicated, alpha half: alpha not blue).
    alpha: bool
    mapping: int


@dataclass(frozen=True)
class Output:
    """The output word of one half stage."""

    cd_dst: int
    ab_dst: int
    sum_dst: int
    cd_dot: bool
    ab_dot: bool
    mux: bool
    bias: bool
    scale_code: int
    blue_to_alpha_cd: bool
    blue_to_alpha_ab: bool

    @property
    def scale(self) -> float:
        return OUT_SCALE_FACTOR[self.scale_code]

    @property
    def operation(self) -> int:
        return (self.scale_code << 1) | int(self.bias)


@dataclass(frozen=True)
class Half:
    inputs: tuple[Input, Input, Input, Input]
    output: Output


@dataclass(frozen=True)
class Stage:
    rgb: Half
    alpha: Half
    #: Indices into the factor arrays this stage reads as constant colours 0 and 1.
    factor0: int
    factor1: int


@dataclass(frozen=True)
class Final:
    a: Input
    b: Input
    c: Input
    d: Input
    e: Input
    f: Input
    g: Input
    clamp_sum: bool
    complement_v1: bool
    complement_r0: bool


@dataclass(frozen=True)
class Config:
    stages: tuple[Stage, ...]
    final: Final
    mux_msb: bool
    texture_modes: tuple[int, int, int, int]
    #: Raw words the GLSL does not interpret, kept so a caller can see them.
    dot_mapping: int
    other_input: int


def words(block: bytes) -> list[int]:
    """Unpack a definition into its 60 little-endian dwords."""
    if len(block) != BLOCK_BYTES:
        raise ValueError(f"a definition is {BLOCK_BYTES} bytes, got {len(block)}")
    return list(struct.unpack(f"<{BLOCK_DWORDS}I", block))


def _inputs(word: int) -> tuple[Input, Input, Input, Input]:
    """A is the top byte, D the bottom. Each byte is mapping(7:5) alpha(4) register(3:0)."""
    operands = []
    for shift in (24, 16, 8, 0):
        byte = (word >> shift) & 0xFF
        operands.append(Input(byte & 0xF, bool(byte & 0x10), byte >> 5))
    return (operands[0], operands[1], operands[2], operands[3])


def _output(word: int) -> Output:
    operation = (word >> 15) & 7
    return Output(
        cd_dst=word & 0xF,
        ab_dst=(word >> 4) & 0xF,
        sum_dst=(word >> 8) & 0xF,
        cd_dot=bool(word & OUT_CD_DOT),
        ab_dot=bool(word & OUT_AB_DOT),
        mux=bool(word & OUT_MUX),
        bias=bool(operation & 1),
        scale_code=operation >> 1,
        blue_to_alpha_cd=bool(word & OUT_BLUE_TO_ALPHA_CD),
        blue_to_alpha_ab=bool(word & OUT_BLUE_TO_ALPHA_AB),
    )


def _check_half(half: Half, *, is_alpha: bool) -> None:
    for operand in half.inputs:
        if operand.register not in GENERAL_SOURCES:
            raise UnsupportedConfig("bad_source", f"register {operand.register}")
    out = half.output
    for destination in (out.cd_dst, out.ab_dst, out.sum_dst):
        if destination not in GENERAL_DESTINATIONS:
            raise UnsupportedConfig("bad_destination", f"register {destination}")
    # Scale code 0 or 1 takes either bias. Codes 2 (x4) and 3 (x1/2) take none.
    if out.scale_code >= 2 and out.bias:
        raise UnsupportedConfig("invalid_output_op", f"operation {out.operation}")
    if is_alpha and (out.ab_dot or out.cd_dot or out.blue_to_alpha_ab or out.blue_to_alpha_cd):
        raise UnsupportedConfig("alpha_dot")


def decode(block: bytes) -> Config:
    """Structured form of a 240-byte definition. Raises `UnsupportedConfig`."""
    dwords = words(block)
    control = dwords[IDX_CONTROL]
    count = control & CONTROL_COUNT_MASK
    if not 1 <= count <= 8:
        raise UnsupportedConfig("stage_count", f"count {count}")
    stages = []
    for index in range(count):
        rgb = Half(_inputs(dwords[IDX_COLOR_ICW + index]), _output(dwords[IDX_COLOR_OCW + index]))
        alpha = Half(_inputs(dwords[IDX_ALPHA_ICW + index]), _output(dwords[IDX_ALPHA_OCW + index]))
        _check_half(rgb, is_alpha=False)
        _check_half(alpha, is_alpha=True)
        stages.append(
            Stage(
                rgb,
                alpha,
                factor0=index if control & CONTROL_FACTOR0_EACH_BIT else 0,
                factor1=index if control & CONTROL_FACTOR1_EACH_BIT else 0,
            )
        )
    first = _inputs(dwords[IDX_FINAL0])
    second = _inputs(dwords[IDX_FINAL1])
    for operand in (*first, *second[:3]):
        if operand.register not in FINAL_SOURCES:
            raise UnsupportedConfig("bad_final_source", f"register {operand.register}")
    if REG_PROD in (second[0].register, second[1].register):
        raise UnsupportedConfig("bad_final_source", "E or F reads the E times F product")
    final = Final(
        *first,
        e=second[0],
        f=second[1],
        g=second[2],
        clamp_sum=bool(dwords[IDX_FINAL1] & FINAL_CLAMP_SUM),
        complement_v1=bool(dwords[IDX_FINAL1] & FINAL_COMPLEMENT_V1),
        complement_r0=bool(dwords[IDX_FINAL1] & FINAL_COMPLEMENT_R0),
    )
    program = dwords[IDX_STAGE_PROGRAM]
    modes = tuple((program >> (5 * stage)) & 0x1F for stage in range(4))
    if TEX_CLIP_PLANE in modes and dwords[IDX_CLIP_MODE]:
        raise UnsupportedConfig("clip_mode", f"word {dwords[IDX_CLIP_MODE]:#x}")
    for stage_index, mode in enumerate(modes):
        if mode not in SUPPORTED_TEXTURE_MODES:
            raise UnsupportedConfig("texture_mode", TEXTURE_MODE_NAMES.get(mode, str(mode)))
        if mode in (TEX_DOT_ST, TEX_DOT_PRODUCT):
            if stage_index == 0:
                raise UnsupportedConfig("stage_order", f"stage 0 mode {mode}")
            mapper = (dwords[IDX_DOT_MAPPING] >> (4 * (stage_index - 1))) & 0xF
            if mapper not in SUPPORTED_DOT_MAPPINGS:
                raise UnsupportedConfig("dot_mapping", f"stage {stage_index} mapping {mapper}")
        if mode in (TEX_DEPENDENT_AR, TEX_DEPENDENT_GB) and stage_index == 0:
            raise UnsupportedConfig("stage_order", f"stage 0 mode {mode}")
        if mode in _READS_EARLIER_STAGE and input_stage(dwords[IDX_OTHER_INPUT], stage_index) >= (
            stage_index
        ):
            raise UnsupportedConfig("stage_order", f"stage {stage_index} reads a later texture")
    return Config(
        stages=tuple(stages),
        final=final,
        mux_msb=bool(control & CONTROL_MUX_MSB_BIT),
        texture_modes=(modes[0], modes[1], modes[2], modes[3]),
        dot_mapping=dwords[IDX_DOT_MAPPING],
        other_input=dwords[IDX_OTHER_INPUT],
    )


def encode_input(operand: Input) -> int:
    return (operand.mapping << 5) | (int(operand.alpha) << 4) | operand.register


def encode_inputs(operands: tuple[Input, Input, Input, Input]) -> int:
    word = 0
    for operand in operands:
        word = (word << 8) | encode_input(operand)
    return word


def encode_output(out: Output) -> int:
    return (
        out.cd_dst
        | (out.ab_dst << 4)
        | (out.sum_dst << 8)
        | (int(out.cd_dot) * OUT_CD_DOT)
        | (int(out.ab_dot) * OUT_AB_DOT)
        | (int(out.mux) * OUT_MUX)
        | (out.operation << 15)
        | (int(out.blue_to_alpha_cd) * OUT_BLUE_TO_ALPHA_CD)
        | (int(out.blue_to_alpha_ab) * OUT_BLUE_TO_ALPHA_AB)
    )


def encode(config: Config) -> bytes:
    """Inverse of `decode` for the fields it keeps. Used to build test blocks."""
    dwords = [0] * BLOCK_DWORDS
    each0 = any(stage.factor0 for stage in config.stages)
    each1 = any(stage.factor1 for stage in config.stages)
    control = len(config.stages)
    control |= CONTROL_MUX_MSB_BIT if config.mux_msb else 0
    control |= CONTROL_FACTOR0_EACH_BIT if each0 else 0
    control |= CONTROL_FACTOR1_EACH_BIT if each1 else 0
    dwords[IDX_CONTROL] = control
    for index, stage in enumerate(config.stages):
        dwords[IDX_COLOR_ICW + index] = encode_inputs(stage.rgb.inputs)
        dwords[IDX_COLOR_OCW + index] = encode_output(stage.rgb.output)
        dwords[IDX_ALPHA_ICW + index] = encode_inputs(stage.alpha.inputs)
        dwords[IDX_ALPHA_OCW + index] = encode_output(stage.alpha.output)
    final = config.final
    dwords[IDX_FINAL0] = encode_inputs((final.a, final.b, final.c, final.d))
    dwords[IDX_FINAL1] = (
        encode_inputs((final.e, final.f, final.g, Input(0, False, 0)))
        | (FINAL_CLAMP_SUM if final.clamp_sum else 0)
        | (FINAL_COMPLEMENT_V1 if final.complement_v1 else 0)
        | (FINAL_COMPLEMENT_R0 if final.complement_r0 else 0)
    )
    program = 0
    for stage, mode in enumerate(config.texture_modes):
        program |= mode << (5 * stage)
    dwords[IDX_STAGE_PROGRAM] = program
    dwords[IDX_DOT_MAPPING] = config.dot_mapping
    dwords[IDX_OTHER_INPUT] = config.other_input
    return struct.pack(f"<{BLOCK_DWORDS}I", *dwords)


@dataclass(frozen=True)
class ConstantMap:
    """Which virtual constant (`c0` to `c7` of the shader source) feeds each hardware slot.

    `None` means the slot is unused. The GPU never sees these words: they tell the host
    which factor registers to rewrite when the title sets a pixel-shader constant.
    """

    factor0: tuple[int | None, ...]
    factor1: tuple[int | None, ...]
    final: tuple[int | None, int | None]


def _nibbles(word: int, count: int) -> tuple[int | None, ...]:
    values = [(word >> (4 * index)) & 0xF for index in range(count)]
    return tuple(None if value == 0xF else value for value in values)


def constant_map(block: bytes) -> ConstantMap:
    dwords = words(block)
    final = _nibbles(dwords[IDX_FINAL_MAP], 2)
    return ConstantMap(
        factor0=_nibbles(dwords[IDX_FACTOR0_MAP], 8),
        factor1=_nibbles(dwords[IDX_FACTOR1_MAP], 8),
        final=(final[0], final[1]),
    )
