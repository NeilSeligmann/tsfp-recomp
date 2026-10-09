# SPDX-License-Identifier: GPL-3.0-or-later
"""Candidate models of the two combiner questions hardware alone can settle (T102, T103).

THE POINT. Neither question can be answered from here, because there is no NV2A and no
capture. What CAN be done is to make each candidate an executable model, in the clean-room
Python evaluator and in the GLSL translation, and to measure how much they differ on the
title's own combiner blocks. A difference that is zero or tiny on the title's blocks makes
the question moot for this title. A large one says exactly what a hardware test must
distinguish. Nothing here says which candidate is right, and the shipped model is unchanged:
`reference.evaluate` and `glsl.combine_function` called without a `Model` behave as before.

PRECISION (T103). Two independent choices, both INFERRED rather than documented:
  rounding  how a value is put on the 1/255 grid: `even` (nearest, ties to even), `half_up`
            (nearest, ties up), `floor` (truncate), or `none` (float, what the translator
            ships).
  where     `write` rounds every general-stage register write (the shipped inferred model),
            `output` rounds only the final colour, as an 8-bit render target would.
The grid itself is not a free choice: NV_texture_shader documents the combiner's 9-bit
signed type as 255 <-> 1.0, 0 <-> 0.0 and -255 <-> -1.0, a 1/255 grid.

TIES. Products of grid values land exactly on grid ties all the time (`0.5 * k/255`, any
output scale 0.5). In float arithmetic such a value sits an ulp either side of the tie, so
an unguarded round picks a side by noise. The rules here treat a value within `TIE_GUARD`
of a grid step as exactly on the tie (or on the integer, for `floor`), in Python and GLSL
alike, so the two evaluators agree and the choice is the model's, not float noise.

DOT MAPPING 1 (T102). The per-channel mapping of the previous texture's texel (an 8-bit
value x = k/255) before the dot product with the texture coordinates:
  d3d           2x - 1, Direct3D `_bx2` and the OpenGL NV_texture_shader EXPAND_NORMAL_NV.
  xemu          (255x - 128) / 127, what the shipped model uses. Reads the byte as signed.
  xemu_clamped  the same, clamped to [-1, 1]: only the byte 0 changes (-1.0079 to -1).
  signed8       (255x - 128) / 128, the NV_texture_shader table for 8-bit signed fixed
                point (-128 <-> -1.0, 127 <-> 0.99218).
"""

from __future__ import annotations

import math
from dataclasses import dataclass

#: Distance from a tie (or an integer), in grid steps, inside which a value counts as exactly
#: on it. float32 error on a value in [-1, 1] times 255 is below 1e-4 steps for these chains.
TIE_GUARD = 1e-3
STEPS = 255.0

ROUNDINGS = ("none", "even", "half_up", "floor")
WHERES = ("write", "output")


def _dot_d3d(x: float) -> float:
    return 2.0 * x - 1.0


def _dot_xemu(x: float) -> float:
    return (x * 255.0 - 128.0) / 127.0


def _dot_xemu_clamped(x: float) -> float:
    return max(-1.0, _dot_xemu(x))


def _dot_signed8(x: float) -> float:
    return (x * 255.0 - 128.0) / 128.0


#: name -> (python function of the texel value, GLSL expression template over `{s}`)
DOT_MAPPINGS = {
    "d3d": (_dot_d3d, "(2.0 * ({s}) - 1.0)"),
    "xemu": (_dot_xemu, "((({s}) * 255.0 - 128.0) / 127.0)"),
    "xemu_clamped": (_dot_xemu_clamped, "max(((({s}) * 255.0 - 128.0) / 127.0), -1.0)"),
    "signed8": (_dot_signed8, "((({s}) * 255.0 - 128.0) / 128.0)"),
}


@dataclass(frozen=True)
class Model:
    name: str
    rounding: str = "none"
    where: str = "write"
    dot: str = "xemu"
    #: Texels are 8-bit in memory, so quantise what the (procedural) sampler returns.
    texels8: bool = False

    def __post_init__(self) -> None:
        if self.rounding not in ROUNDINGS:
            raise ValueError(f"rounding {self.rounding}")
        if self.where not in WHERES:
            raise ValueError(f"where {self.where}")
        if self.dot not in DOT_MAPPINGS:
            raise ValueError(f"dot mapping {self.dot}")

    def rounds_writes(self) -> bool:
        return self.rounding != "none" and self.where == "write"

    def rounds_output(self) -> bool:
        return self.rounding != "none" and self.where == "output"


def grid_round(value: float, rounding: str) -> float:
    """Put `value` on the 1/255 grid under `rounding`, with the tie guard."""
    steps = value * STEPS
    low = math.floor(steps)
    if rounding == "none":
        return value
    if rounding == "floor":
        picked = math.floor(steps + TIE_GUARD)
    elif rounding == "half_up":
        picked = math.floor(steps + 0.5 + TIE_GUARD)
    elif rounding == "even":
        if abs(steps - low - 0.5) < TIE_GUARD:
            picked = low if low % 2 == 0 else low + 1
        else:
            picked = math.floor(steps + 0.5)
    else:
        raise ValueError(f"rounding {rounding}")
    return picked / STEPS


def on_a_tie(value: float) -> bool:
    """Whether `value` is within the guard of a half step (where `rounding` choices differ)."""
    steps = value * STEPS
    return abs(steps - math.floor(steps) - 0.5) < TIE_GUARD


def glsl_functions(rounding: str) -> str:
    """The GLSL twin of `grid_round` (`nv2a_q`) and the 8-bit texel quantiser (`nv2a_texel8`)."""
    guard = repr(TIE_GUARD)
    if rounding == "floor":
        body = f"return floor(n + {guard});"
    elif rounding == "half_up":
        body = f"return floor(n + 0.5 + {guard});"
    elif rounding == "even":
        body = (
            "float low = floor(n);\n"
            f"    if (abs(n - low - 0.5) < {guard})\n"
            "        return mod(low, 2.0) == 0.0 ? low : low + 1.0;\n"
            "    return floor(n + 0.5);"
        )
    else:
        body = "return n;"
    return (
        "float nv2a_step(float n) {\n"
        f"    {body}\n"
        "}\n"
        "float nv2a_q(float x) { return nv2a_step(x * 255.0) / 255.0; }\n"
        "vec3 nv2a_q(vec3 x) { return vec3(nv2a_q(x.x), nv2a_q(x.y), nv2a_q(x.z)); }\n"
        "vec4 nv2a_q(vec4 x) { return vec4(nv2a_q(x.xyz), nv2a_q(x.w)); }\n"
        f"vec4 nv2a_texel8(vec4 x) {{ return floor(x * 255.0 + 0.5 + {guard}) / 255.0; }}\n"
    )


#: The shipped model: float arithmetic, xemu's dot mapping. What `glsl` emits with no Model.
FLOAT = Model("float")
#: The model `precision.py` measures: every register write on the grid, nearest-even.
WRITE_EVEN = Model("write_even", "even", "write")

PRECISION_CANDIDATES = (
    FLOAT,
    WRITE_EVEN,
    Model("write_half_up", "half_up", "write"),
    Model("write_floor", "floor", "write"),
    Model("output_even", "even", "output"),
    Model("output_floor", "floor", "output"),
)

DOT_CANDIDATES = tuple(Model(f"dot_{name}", dot=name, texels8=True) for name in DOT_MAPPINGS)
