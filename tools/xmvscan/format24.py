# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference model of the original XMV format 0x24 (YUY2) frame-output conversion.

The title's `0x4463AE` copies the decoder's three planar 8-bit planes into the locked Direct3D
surface with `0x445AAF` when the surface format is 0x24. T253 measured that function over the
original bytes (tests/test_xmv_format24_original.py) and this module is the independent model
it is compared with. It is pure integer arithmetic with no tables and no rounding: the two
chroma bytes of a pixel pair are copied, not filtered.

Contract (docs/xmv-contracts.md section 12):

    0x445AAF(mb_cols, mb_rows, y, u, v, width, height, dest, pitch)   stdcall, 9 dwords

`mb_cols` and `mb_rows` are the decoder's `(dimension + 15) >> 4`. The planes are the decoder's
macroblock-aligned planes: Y has stride `16 * mb_cols` and `16 * mb_rows` rows, U and V have
stride `8 * mb_cols` and `8 * mb_rows` rows. Output pixel (x, y) is two bytes, byte 0 the Y
sample and byte 1 the U sample for an even x or the V sample for an odd x, with the chroma pair
taken at (x // 2, y // 2), so each chroma row serves two output rows. Only the visible
`width` x `height` pixels are written (the macroblock padding is converted in a stack scratch
and never copied), and the bytes of a row beyond `2 * width` are left untouched.
"""

from __future__ import annotations


class Format24Error(ValueError):
    """The arguments are outside the measured contract."""


def aligned_extent(mb_cols: int, mb_rows: int) -> tuple[int, int, int, int]:
    """Return the Y stride, Y rows, chroma stride and chroma rows of the decoder planes."""
    return 16 * mb_cols, 16 * mb_rows, 8 * mb_cols, 8 * mb_rows


def check_layout(mb_cols: int, mb_rows: int, width: int, height: int, pitch: int) -> None:
    """Refuse every layout the original was not measured on."""
    if min(mb_cols, mb_rows, width, height, pitch) < 0:
        raise Format24Error("negative dimension")
    if mb_cols == 0 or mb_rows == 0:
        return
    if not 16 * mb_cols - 15 <= width <= 16 * mb_cols:
        raise Format24Error("width is not covered by exactly mb_cols macroblocks")
    if not 16 * mb_rows - 15 <= height <= 16 * mb_rows:
        raise Format24Error("height is not covered by exactly mb_rows macroblocks")
    if pitch < 2 * width:
        raise Format24Error("pitch is smaller than one output row")


def convert(
    mb_cols: int,
    mb_rows: int,
    y_plane: bytes,
    u_plane: bytes,
    v_plane: bytes,
    width: int,
    height: int,
    dest: bytearray,
    pitch: int,
) -> None:
    """Write the YUY2 image into `dest` exactly as `0x445AAF` does, rows `pitch` bytes apart."""
    check_layout(mb_cols, mb_rows, width, height, pitch)
    if mb_cols == 0 or mb_rows == 0:
        return
    luma_stride, luma_rows, chroma_stride, chroma_rows = aligned_extent(mb_cols, mb_rows)
    if len(y_plane) != luma_stride * luma_rows:
        raise Format24Error("Y plane size")
    if len(u_plane) != chroma_stride * chroma_rows or len(v_plane) != len(u_plane):
        raise Format24Error("chroma plane size")
    if len(dest) < pitch * (height - 1) + 2 * width:
        raise Format24Error("destination is smaller than the visible image")
    for row in range(height):
        luma = row * luma_stride
        chroma = (row // 2) * chroma_stride
        target = row * pitch
        for column in range(width):
            dest[target + 2 * column] = y_plane[luma + column]
            source = u_plane if column % 2 == 0 else v_plane
            dest[target + 2 * column + 1] = source[chroma + column // 2]
