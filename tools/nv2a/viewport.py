# SPDX-License-Identifier: GPL-3.0-or-later
"""T96: undo the title's own viewport transform on `oPos`.

MEASURED: 41 of 46 static programs write `oPos.xyz` as `MUL oPos.xyz, r12, c58` then
`MAD oPos.xyz, r12, rcc(r12.w), c59` ("full"), 3 write only `oPos.z` (output mask 0b0010, NOT y
as docs/vertex-translator.md first said) as `v1.z * c58` then `+ c59`, no divide ("z-only"), 2 read
neither constant ("none"). In the z-only programs oPos.xyw is a plain copy of `v1.xyyw`.
INFERRED: c58 is the viewport scale and c59 the offset, so for "full"
`window.xyz = clip.xyz * c58.xyz / w + c59.xyz` and `window.w = clip.w`.
Inverse (INFERRED, the same inference): "full" `clip.xyz = (window.xyz - c59.xyz) / c58.xyz * w`;
"z-only" `clip.z = (window.z - c59.z) / c58.z`, x, y, w untouched (no divide was done, so
none is undone; whether xy then need a viewport conversion is UNSETTLED, c58.xy is not used).
A zero scale component cannot be inverted: it is passed through (INFERRED guard).

T560 (opt-in, `window_to_clip`): the programs the inverse leaves alone still hand the rasterizer
WINDOW coordinates in `oPos.xy`. The "none" class (the copy composition's two instruction program
writes `(0, 0)`, `(4W, 0)`, `(0, 4H)` straight from its vertex data) and the x, y of "z-only"
(a copy of `v1`, pre-transformed vertices). Raw they land in clip space and cover the quadrant
x, y in [0, 1] (76800 of 307200 pixels at 640 x 480). The conversion is the SAME `nv2a_unscale`
on x and y with c58 and c59 (so it follows the registers 0x0A20 and 0x0AF0 when the stream
writes them and the whole-target seed of T477 otherwise), times `w` like the "full" class, z
and w untouched. INFERRED, never run on an NV2A (HQ57).
"""

from __future__ import annotations

import argparse
import ctypes
import math
import sys
from collections.abc import Sequence
from pathlib import Path

from tools.nv2a import isa

SCALE = 58
OFFSET = 59
FULL = "full"
Z_ONLY = "z-only"
NONE = "none"
KINDS = (FULL, Z_ONLY, NONE)
_MIN_NORMAL = 2.0**-126

#: GLSL helper plus the conversion, used by `translate.vertex_shader_source`.
GLSL_HELPER = """\
float nv2a_unscale(float window, float scale, float offset) {
    return scale == 0.0 ? window : (window - offset) / scale;
}
"""
GLSL_BODY = {
    FULL: (
        "    gl_Position = vec4(nv2a_unscale(o[0].x, c[58].x, c[59].x) * o[0].w, "
        "nv2a_unscale(o[0].y, c[58].y, c[59].y) * o[0].w, "
        "nv2a_unscale(o[0].z, c[58].z, c[59].z) * o[0].w, o[0].w);"
    ),
    Z_ONLY: (
        "    gl_Position = vec4(o[0].x, o[0].y, nv2a_unscale(o[0].z, c[58].z, c[59].z), o[0].w);"
    ),
}

#: T560, the x and y of the programs the inverse leaves alone (NONE, Z_ONLY), same helper. With
#: `undo_viewport` the z of Z_ONLY is unscaled too (`GLSL_BODY_WINDOW_UNDO`).
_WINDOW_XY = (
    "    gl_Position = vec4(nv2a_unscale(o[0].x, c[58].x, c[59].x) * o[0].w, "
    "nv2a_unscale(o[0].y, c[58].y, c[59].y) * o[0].w, o[0].z, o[0].w);"
)
GLSL_BODY_WINDOW = {NONE: _WINDOW_XY, Z_ONLY: _WINDOW_XY}
GLSL_BODY_WINDOW_UNDO = {
    NONE: _WINDOW_XY,
    Z_ONLY: (
        "    gl_Position = vec4(nv2a_unscale(o[0].x, c[58].x, c[59].x) * o[0].w, "
        "nv2a_unscale(o[0].y, c[58].y, c[59].y) * o[0].w, "
        "nv2a_unscale(o[0].z, c[58].z, c[59].z), o[0].w);"
    ),
}


def classify(data: bytes) -> str:
    """`full`, `z-only` or `none` from the oPos writes that read constant 58 or 59."""
    masks = 0
    for decoded in isa.decode_program(data):
        if decoded.out_to_output and decoded.out_address == 0:
            letters = "C" if decoded.out_is_ilu else isa.MAC_READS.get(decoded.mac_name, "")
            reads = any(
                decoded.source(letter).mux == isa.MUX_CONST
                and not decoded.relative
                and decoded.const in (SCALE, OFFSET)
                for letter in letters
            )
            if reads:
                masks |= decoded.out_mask
        if decoded.final:
            break
    if masks == 0:
        return NONE
    return Z_ONLY if masks == 0b0010 else FULL


def window_from_clip(
    kind: str, clip: Sequence[float], scale: Sequence[float], offset: Sequence[float]
) -> list[float]:
    """The forward model the programs implement (reference for the tests)."""
    x, y, z, w = clip
    if kind == FULL:
        return [
            x * scale[0] / w + offset[0],
            y * scale[1] / w + offset[1],
            z * scale[2] / w + offset[2],
            w,
        ]
    if kind == Z_ONLY:
        return [x, y, z * scale[2] + offset[2], w]
    return list(clip)


def clip_from_window(
    kind: str,
    window: Sequence[float],
    scale: Sequence[float],
    offset: Sequence[float],
    *,
    window_to_clip: bool = False,
) -> list[float]:
    """Python twin of the GLSL inverse. `window_to_clip` (T560) also converts x and y of the
    classes the inverse leaves alone (NONE, Z_ONLY), times `w`, z and w untouched."""

    def unscale(axis: int) -> float:
        return window[axis] if scale[axis] == 0 else (window[axis] - offset[axis]) / scale[axis]

    x, y, z, w = window
    if kind == FULL:
        return [unscale(0) * w, unscale(1) * w, unscale(2) * w, w]
    if kind == Z_ONLY:
        if window_to_clip:
            return [unscale(0) * w, unscale(1) * w, unscale(2), w]
        return [x, y, unscale(2), w]
    if window_to_clip:
        return [unscale(0) * w, unscale(1) * w, z, w]
    return list(window)


def glsl_position(kind: str, *, undo_viewport: bool, window_to_clip: bool) -> str | None:
    """The `gl_Position` line the vertex stage uses for a program of class `kind`, or None for the
    plain `gl_Position = o[0];`. The one place the two options combine."""
    if window_to_clip and kind in GLSL_BODY_WINDOW:
        return (GLSL_BODY_WINDOW_UNDO if undo_viewport else GLSL_BODY_WINDOW)[kind]
    if undo_viewport:
        return GLSL_BODY.get(kind)
    return None


def _f32(value: float) -> float:
    """Round a double to float32 and back (overflow gives inf, never an exception)."""
    return ctypes.c_float(value).value


def _read(value: float, flush_denormals: bool) -> float:
    """A source operand as the arithmetic unit sees it. With `flush_denormals` a denormal
    reads as a zero of its sign (the rule corner_classify (T100b) found on both devices)."""
    if flush_denormals and 0.0 < abs(value) < _MIN_NORMAL:
        return math.copysign(0.0, value)
    return value


def clip_from_window_f32(
    kind: str,
    window: Sequence[float],
    scale: Sequence[float],
    offset: Sequence[float],
    *,
    flush_denormals: bool = False,
    reciprocal_divide: bool = False,
) -> list[float]:
    """Float32 twin of the GLSL inverse (T100c), the same operations in the GLSL order, each
    result rounded to float32 before the next (`clip_from_window` runs in float64 and so
    overflows, loses NaN/inf and denormals differently).

    The GLSL is `scale == 0.0 ? window : (window - offset) / scale`, then `* w`. A float32
    add, subtract, multiply or divide of two float32 operands is correctly rounded by
    rounding the double result once (double has more than 2 * 24 + 2 bits), so this is the
    IEEE float32 result bit for bit. NaN payloads are not modelled (the quiet NaN). A device
    divide is allowed a few ULP by Vulkan, which the comparison tolerance absorbs.
    `flush_denormals` is the variant in which every source operand of every operation reads
    a denormal as zero (so `scale == 0.0` is also true for a denormal scale).
    `reciprocal_divide` is the variant in which `a / b` is `a * (1 / b)` with the reciprocal
    rounded to float32 and a denormal reciprocal flushed to zero (MEASURED on RADV, T100c:
    `inf / FLT_MAX` is NaN there, see docs/vertex-translator.md 7.6.2)."""

    def read(value: float) -> float:
        return _read(value, flush_denormals)

    def unscale(axis: int) -> float:
        value, divisor = read(window[axis]), read(scale[axis])
        if divisor == 0.0:
            return value
        difference = _f32(value - read(offset[axis]))
        if reciprocal_divide:
            reciprocal = _f32(1.0 / divisor)
            if 0.0 < abs(reciprocal) < _MIN_NORMAL:
                reciprocal = math.copysign(0.0, reciprocal)
            return _f32(read(difference) * reciprocal)
        return _f32(read(difference) / divisor)

    x, y, z, w = window
    if kind == FULL:
        return [_f32(read(unscale(axis)) * read(w)) for axis in range(3)] + [w]
    if kind == Z_ONLY:
        return [x, y, unscale(2), w]
    return list(window)


TWIN_FLOAT64 = "float64"
TWIN_FLOAT32 = "float32"
TWIN_FLOAT32_FTZ = "float32-ftz"
TWIN_FLOAT32_RCP = "float32-ftz-rcp"
TWINS = (TWIN_FLOAT64, TWIN_FLOAT32, TWIN_FLOAT32_FTZ, TWIN_FLOAT32_RCP)


def twin_position(
    twin: str,
    kind: str,
    window: Sequence[float],
    scale: Sequence[float],
    offset: Sequence[float],
) -> list[float]:
    """The expected `gl_Position` (float32 values) for the window position under a named twin
    (T100c, T266): the float64 twin of T96 or one of the three float32 variants."""
    if twin == TWIN_FLOAT64:
        return [_f32(value) for value in clip_from_window(kind, window, scale, offset)]
    if twin in (TWIN_FLOAT32, TWIN_FLOAT32_FTZ, TWIN_FLOAT32_RCP):
        return clip_from_window_f32(
            kind,
            window,
            scale,
            offset,
            flush_denormals=twin != TWIN_FLOAT32,
            reciprocal_divide=twin == TWIN_FLOAT32_RCP,
        )
    raise ValueError(f"unknown twin {twin!r}, expected one of {TWINS}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.nv2a.viewport",
        description="Classify vertex programs by viewport shape.",
    )
    parser.add_argument("files", type=Path, nargs="+", help="microcode .bin files")
    args = parser.parse_args(argv)
    for path in args.files:
        sys.stdout.write(f"{path.name} {classify(path.read_bytes())}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


def live_raster_position() -> str:
    """Pinned xemu programmable raster map; live_raster=(width,height,clip_max,fixed_point_size).

    Source-derived INFERRED, positive Vulkan viewport; no c58/c59 inverse.
    """
    return """    float raster_w = (o[0].w > 0.0 || floatBitsToUint(o[0].w) == 0u)
        ? clamp(o[0].w, uintBitsToFloat(0x1F800000u), uintBitsToFloat(0x5F800000u))
        : clamp(o[0].w, uintBitsToFloat(0xDF800000u), uintBitsToFloat(0x9F800000u));
    vec2 screen_xy = trunc(o[0].xy * 16.0) / 16.0;
    gl_Position = vec4(((2.0 * screen_xy - live_raster.xy) / live_raster.xy) * raster_w,
                       o[0].z / live_raster.z * raster_w, raster_w);"""
