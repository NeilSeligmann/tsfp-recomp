# SPDX-License-Identifier: GPL-3.0-or-later
"""T612: where FFmpeg's WMV2 differs from the title's XMV decoder, block by block (numbers only).

Replays one captured codec wrapper entry on the original instructions (bare Unicorn, as
`replay_xmv_capture`), records every call of the prediction plus residual kernel `0x449C11` with the
residual block and the coefficient block the title's transform left beside it, and compares against
FFmpeg's decode of the same movie frame (PyAV, `export_mvs`). Reports: motion vector agreement,
whether the title rounds up (half pel average with +1 or +2) for every block, whether the residual
equals FFmpeg's WMV2 8x8 inverse transform of the coefficients (input transposed), and which block
classes hold the pixels where the two decoders differ. Needs `unicorn` and `av` in ONE interpreter
(a private venv). No pixel value or packet byte is printed or stored.

Exit status: 0 done, 2 an input is missing or unusable.
"""

from __future__ import annotations

import argparse
import collections
import json
import struct
import sys
from pathlib import Path

ADD_KERNEL = 0x449C11
REFERENCE_FIELD, OUTPUT_FIELD, COLUMNS_FIELD, ROWS_FIELD = 0xEC, 0xFC, 0xDC, 0xE0
W0, W1, W2, W3, W5, W6, W7 = 2048, 2841, 2676, 2408, 1609, 1108, 565  # FFmpeg wmv2dsp.c


def idct_row(b: list[int]) -> list[int]:
    a1, a7 = W1 * b[1] + W7 * b[7], W7 * b[1] - W1 * b[7]
    a5, a3 = W5 * b[5] + W3 * b[3], W3 * b[5] - W5 * b[3]
    a2, a6 = W2 * b[2] + W6 * b[6], W6 * b[2] - W2 * b[6]
    a0, a4 = W0 * (b[0] + b[4]), W0 * (b[0] - b[4])
    s1 = (181 * (a1 - a5 + a7 - a3) + 128) >> 8
    s2 = (181 * (a1 - a5 - a7 + a3) + 128) >> 8
    r = 1 << 7
    return [
        (a0 + a2 + a1 + a5 + r) >> 8, (a4 + a6 + s1 + r) >> 8,
        (a4 - a6 + s2 + r) >> 8, (a0 - a2 + a7 + a3 + r) >> 8,
        (a0 - a2 - a7 - a3 + r) >> 8, (a4 - a6 - s2 + r) >> 8,
        (a4 + a6 - s1 + r) >> 8, (a0 + a2 - a1 - a5 + r) >> 8,
    ]  # fmt: skip


def idct_col(b: list[int]) -> list[int]:
    a1, a7 = (W1 * b[1] + W7 * b[7] + 4) >> 3, (W7 * b[1] - W1 * b[7] + 4) >> 3
    a5, a3 = (W5 * b[5] + W3 * b[3] + 4) >> 3, (W3 * b[5] - W5 * b[3] + 4) >> 3
    a2, a6 = (W2 * b[2] + W6 * b[6] + 4) >> 3, (W6 * b[2] - W2 * b[6] + 4) >> 3
    a0, a4 = (W0 * (b[0] + b[4])) >> 3, (W0 * (b[0] - b[4])) >> 3
    s1 = (181 * (a1 - a5 + a7 - a3) + 128) >> 8
    s2 = (181 * (a1 - a5 - a7 + a3) + 128) >> 8
    r = 1 << 13
    return [
        (a0 + a2 + a1 + a5 + r) >> 14, (a4 + a6 + s1 + r) >> 14,
        (a4 - a6 + s2 + r) >> 14, (a0 - a2 + a7 + a3 + r) >> 14,
        (a0 - a2 - a7 - a3 + r) >> 14, (a4 - a6 - s2 + r) >> 14,
        (a4 + a6 - s1 + r) >> 14, (a0 + a2 - a1 - a5 + r) >> 14,
    ]  # fmt: skip


def wmv2_idct(block: list[int]) -> list[int]:
    """FFmpeg's 8x8 WMV2 inverse transform of 64 row major coefficients."""
    rows = [idct_row(block[8 * row : 8 * row + 8]) for row in range(8)]
    columns = [idct_col([rows[row][column] for row in range(8)]) for column in range(8)]
    return [columns[column][row] for row in range(8) for column in range(8)]


def transpose(block: list[int]) -> list[int]:
    return [block[8 * column + row] for row in range(8) for column in range(8)]


def predict(
    reference: bytes,
    stride: int,
    rows: int,
    x: int,
    y: int,
    half_h: int,
    half_v: int,
    no_rounding: int,
) -> int:
    def pixel(px: int, py: int) -> int:
        return reference[min(max(py, 0), rows - 1) * stride + min(max(px, 0), stride - 1)]

    a, b, c, d = pixel(x, y), pixel(x + 1, y), pixel(x, y + 1), pixel(x + 1, y + 1)
    if half_h and half_v:
        return (a + b + c + d + 2 - no_rounding) >> 2
    if half_h:
        return (a + b + 1 - no_rounding) >> 1
    if half_v:
        return (a + c + 1 - no_rounding) >> 1
    return a


def ffmpeg_frame(movie: Path, index: int):  # noqa: ANN201
    import av  # noqa: PLC0415

    with av.open(str(movie)) as container:
        stream = container.streams.video[0]
        stream.codec_context.options = {"flags2": "+export_mvs"}
        for number, frame in enumerate(container.decode(video=0)):
            if number == index:
                plane = frame.planes[0]
                data, line = bytes(plane), plane.line_size
                luma = b"".join(
                    data[r * line : r * line + frame.width] for r in range(frame.height)
                )
                vectors = {}
                for vector in frame.side_data.get("MOTION_VECTORS") or []:
                    key = (vector.dst_x - vector.w // 2, vector.dst_y - vector.h // 2, vector.w)
                    vectors.setdefault(key, []).append((vector.motion_x, vector.motion_y))
                return luma, frame.width, frame.height, vectors, frame.pict_type
    raise ValueError(f"{movie.name} has no frame {index}")


def replay(capture: Path, xbe: Path):  # noqa: ANN201
    from unicorn import UC_HOOK_CODE, UcError  # noqa: PLC0415
    from unicorn import x86_const as x86  # noqa: PLC0415

    from tools.diagnostics.replay_xmv_capture import (  # noqa: PLC0415
        MAX_CAPTURE_BYTES,
        XBE_SHA256,
        checked_bytes,
        load_capture,
    )
    from tools.xbe import parse_xbe  # noqa: PLC0415

    data = checked_bytes(
        capture, __import__("hashlib").sha256(capture.read_bytes()).hexdigest(), MAX_CAPTURE_BYTES
    )
    retail = checked_bytes(xbe, XBE_SHA256, 8 * 1024 * 1024)
    uc, registers, _, _ = load_capture(data, parse_xbe(retail), 0x202, 0)
    return_address, decoder = struct.unpack("<2I", uc.mem_read(registers[8], 8))
    cols, rows = struct.unpack("<2I", uc.mem_read(decoder + COLUMNS_FIELD, 8))
    reference_base, output_base = (
        struct.unpack("<I", uc.mem_read(decoder + REFERENCE_FIELD, 4))[0],
        struct.unpack("<I", uc.mem_read(decoder + OUTPUT_FIELD, 4))[0],
    )
    stride, height = cols * 16, rows * 16
    reference = bytes(uc.mem_read(reference_base, stride * height))
    calls = []

    def code(u: object, address: int, _size: int, _data: object) -> None:
        if address == return_address:
            u.emu_stop()
        elif address == ADD_KERNEL:
            esp = u.reg_read(x86.UC_X86_REG_ESP)
            args = struct.unpack("<8I", bytes(u.mem_read(esp + 4, 32)))
            residual = struct.unpack("<64h", bytes(u.mem_read(args[6], 128)))
            coefficients = struct.unpack("<64h", bytes(u.mem_read(args[6] + 0x80, 128)))
            calls.append((args, residual, coefficients))

    uc.hook_add(UC_HOOK_CODE, code)
    try:
        uc.emu_start(registers[0], 0xFFFFFFFF, timeout=60_000_000, count=30_000_000)
    except UcError as error:
        raise ValueError(f"the original replay stopped: {error}") from error
    output = bytes(uc.mem_read(output_base, stride * height))
    return calls, reference, reference_base, output, output_base, stride, height


def analyse(args: argparse.Namespace) -> dict:
    calls, reference, reference_base, output, output_base, stride, height = replay(
        args.capture, args.xbe
    )
    luma, width, visible_height, vectors, picture_type = ffmpeg_frame(args.movie, args.frame)
    if (width, visible_height) != (stride, height):
        raise ValueError("this probe needs a frame whose visible size is macroblock aligned")
    blocks = {}
    for arguments, residual, coefficients in calls:
        source, _, destination, _, half_h, half_v, _, _ = arguments
        dy, dx = divmod(destination - output_base, stride)
        sy, sx = divmod(source - reference_base, stride)
        blocks[dx, dy] = (
            2 * (sx - dx) + half_h,
            2 * (sy - dy) + half_v,
            half_h,
            half_v,
            residual,
            coefficients,
            sx,
            sy,
        )
    interior = {
        k: b for k, b in blocks.items() if 0 <= b[6] < stride - 9 and 0 <= b[7] < height - 9
    }
    motion = collections.Counter()
    rounding = collections.Counter()
    transform = {}
    for key, (mx, my, hh, hv, residual, coefficients, sx, sy) in interior.items():
        candidates = vectors.get((key[0] // 16 * 16, key[1] // 16 * 16, 16)) or vectors.get(
            (*key, 8)
        )
        motion[
            "no FFmpeg vector"
            if not candidates
            else "match"
            if (mx, my) in candidates
            else "mismatch"
        ] += 1
        for no_rounding in (0, 1):
            fits = True
            for r in range(8):
                for c in range(8):
                    base = predict(reference, stride, height, sx + c, sy + r, hh, hv, no_rounding)
                    value = min(max(base + residual[r * 8 + c], 0), 255)
                    fits = fits and value == output[(key[1] + r) * stride + key[0] + c]
            rounding[f"no_rounding={no_rounding} fits original output"] += fits
        if not any(residual):
            transform[key] = "prediction only"
        elif tuple(wmv2_idct(transpose(list(coefficients)))) == tuple(residual):
            transform[key] = "8x8 equals FFmpeg wmv2 idct"
        else:
            transform[key] = "other transform"
    differing = collections.Counter()
    differing_blocks = set()
    unlocated = 0
    for y in range(height):
        for x in range(stride):
            if output[y * stride + x] != luma[y * stride + x]:
                key = (x // 8 * 8, y // 8 * 8)
                if key in transform:
                    differing[transform[key]] += 1
                    differing_blocks.add(key)
                else:
                    unlocated += 1
    classes = collections.Counter(transform.values())
    return {
        "capture": args.capture.name,
        "frame": args.frame,
        "ffmpeg_picture_type": int(picture_type),
        "add_kernel_calls": len(calls),
        "interior_blocks": len(interior),
        "edge_blocks_not_compared": len(blocks) - len(interior),
        "motion_vectors_vs_ffmpeg": dict(motion),
        "rounding_models_matching_original_blocks": dict(rounding),
        "blocks_by_residual_class": dict(classes),
        "differing_luma_pixels_by_class": dict(differing),
        "differing_blocks_by_class": dict(
            collections.Counter(transform[k] for k in differing_blocks)
        ),
        "differing_luma_pixels_in_edge_blocks": unlocated,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--capture", type=Path, required=True, help="entry_NNNNN.bin of the frame")
    parser.add_argument("--xbe", type=Path, required=True, help="the certified retail default.xbe")
    parser.add_argument("--movie", type=Path, required=True, help="the .xmv FFmpeg decodes")
    parser.add_argument("--frame", type=int, required=True, help="0 based frame of the movie")
    args = parser.parse_args()
    try:
        print(json.dumps(analyse(args), indent=1))
    except ImportError as error:
        print(f"refused: needs unicorn and av in one interpreter ({error})", file=sys.stderr)
        return 2
    except (OSError, ValueError) as error:
        print(f"refused: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
