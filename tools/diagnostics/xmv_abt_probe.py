# SPDX-License-Identifier: GPL-3.0-or-later
"""T617: the title's WMV2 ABT (8x4 and 4x8) inverse transforms against FFmpeg's, bit for bit.

The title has three transform entries called from its macroblock loop: `0x448336` (plain 8x8),
`0x4489B8` (8x4: four 8 point rows then a 4 point column, writes rows 4*n..4*n+3 of the residual
block) and `0x448C2F` (4x8: eight 4 point rows from a packed 4 per row coefficient array, then an
8 point column, writes columns 4*n..4*n+3). Replays one captured codec wrapper entry on the original
instructions (bare Unicorn), records every ABT call with its coefficients and the residual it left,
and compares with two Python models:

* the TITLE model, reproduced from the instructions (must equal the original on every call),
* the FFMPEG model, `simple_idct.c` `ff_simple_idct84_add` / `ff_simple_idct48_add` (FFmpeg
  `libavcodec/simple_idct.c` and `simple_idct_template.c`, 8 bit, read from upstream into `tmp/`).

With `--movie` and `--frame` it also checks that the FFmpeg model alone explains the luma pixels
where PyAV's picture differs from the title's. Needs `unicorn` (and `av` for `--movie`) in one
interpreter. No pixel value or packet byte is printed or stored.

Exit status: 0 done, 2 an input is missing or unusable.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import struct
import sys
from pathlib import Path

from tools.diagnostics.xmv_prediction_probe import idct_col, idct_row

ENTRY_8X4, ENTRY_4X8, ENTRY_8X8, ADD_KERNEL = 0x4489B8, 0x448C2F, 0x448336, 0x449C11

# Title 4 point constants (imul operands 0x5A8, 0x764, 0x310; the 8x4 column shifts 16, so x2 = 17).
TITLE_C3, TITLE_C1, TITLE_C2 = 1448, 1892, 784
# FFmpeg idct4col_add: C_FIX(x) = x * sqrt2 * 4096 + 0.5 truncated, shift 17.
FFMPEG_C3, FFMPEG_C1, FFMPEG_C2 = 2896, 3784, 1567
# FFmpeg idct4row: R_FIX(x) = x * sqrt2 * 32768 + 0.5 truncated, shift 11.
FFMPEG_R3, FFMPEG_R1, FFMPEG_R2 = 23170, 30274, 12540
# FFmpeg simple idct 8 bit weights (W4 is 16383, not 16384).
SW1, SW2, SW3, SW4, SW5, SW6, SW7 = 22725, 21407, 19266, 16383, 12873, 8867, 4520


def four_point(
    values: list[int], c3: int, c1: int, c2: int, shift: int, round_bit: int
) -> list[int]:
    a0, a1, a2, a3 = values
    c0 = (a0 + a2) * c3 + round_bit
    d0 = (a0 - a2) * c3 + round_bit
    e1 = a1 * c1 + a3 * c2
    e3 = a1 * c2 - a3 * c1
    return [(c0 + e1) >> shift, (d0 + e3) >> shift, (d0 - e3) >> shift, (c0 - e1) >> shift]


def simple_row(row: list[int]) -> list[int]:
    """FFmpeg idctRowCondDC (8 bit, extra_shift 0): row shift 11, DC only rows are row[0] << 3."""
    if not any(row[1:]):
        value = (row[0] << 3) & 0xFFFF
        return [value - 0x10000 if value >= 0x8000 else value] * 8
    a0 = SW4 * row[0] + (1 << 10)
    a1 = a2 = a3 = a0
    a0 += SW2 * row[2] + SW4 * row[4] + SW6 * row[6]
    a1 += SW6 * row[2] - SW4 * row[4] - SW2 * row[6]
    a2 += -SW6 * row[2] - SW4 * row[4] + SW2 * row[6]
    a3 += -SW2 * row[2] + SW4 * row[4] - SW6 * row[6]
    b0 = SW1 * row[1] + SW3 * row[3] + SW5 * row[5] + SW7 * row[7]
    b1 = SW3 * row[1] - SW7 * row[3] - SW1 * row[5] - SW5 * row[7]
    b2 = SW5 * row[1] - SW1 * row[3] + SW7 * row[5] + SW3 * row[7]
    b3 = SW7 * row[1] - SW5 * row[3] + SW3 * row[5] - SW1 * row[7]
    return [(x + y) >> 11 for x, y in ((a0, b0), (a1, b1), (a2, b2), (a3, b3))] + [
        (x - y) >> 11 for x, y in ((a3, b3), (a2, b2), (a1, b1), (a0, b0))
    ]


def simple_column(col: list[int]) -> list[int]:
    """FFmpeg idctSparseColAdd without the add (COL_SHIFT 20, rounding folded into column 0)."""
    a0 = SW4 * (col[0] + (1 << 19) // SW4)
    a1 = a2 = a3 = a0
    a0 += SW2 * col[2] + SW4 * col[4] + SW6 * col[6]
    a1 += SW6 * col[2] - SW4 * col[4] - SW2 * col[6]
    a2 += -SW6 * col[2] - SW4 * col[4] + SW2 * col[6]
    a3 += -SW2 * col[2] + SW4 * col[4] - SW6 * col[6]
    b0 = SW1 * col[1] + SW3 * col[3] + SW5 * col[5] + SW7 * col[7]
    b1 = SW3 * col[1] - SW7 * col[3] - SW1 * col[5] - SW5 * col[7]
    b2 = SW5 * col[1] - SW1 * col[3] + SW7 * col[5] + SW3 * col[7]
    b3 = SW7 * col[1] - SW5 * col[3] + SW3 * col[5] - SW1 * col[7]
    return [(x + y) >> 20 for x, y in ((a0, b0), (a1, b1), (a2, b2), (a3, b3))] + [
        (x - y) >> 20 for x, y in ((a3, b3), (a2, b2), (a1, b1), (a0, b0))
    ]


def transform_8x4(
    coefficients: list[int], decoder: str, column_decoder: str | None = None
) -> list[int]:
    """Residual rows (4 rows of 8, row major) from 4 coefficient rows of 8.

    `decoder` picks the 8 point row pass, `column_decoder` (default the same) the 4 point column.
    """
    row_pass = idct_row if decoder == "title" else simple_row
    rows = [row_pass(coefficients[8 * i : 8 * i + 8]) for i in range(4)]
    constants = (
        (TITLE_C3 * 2, TITLE_C1 * 2, TITLE_C2 * 2)
        if (column_decoder or decoder) == "title"
        else (FFMPEG_C3, FFMPEG_C1, FFMPEG_C2)
    )
    columns = [
        four_point([rows[i][c] for i in range(4)], *constants, 17, 1 << 16) for c in range(8)
    ]
    return [columns[c][i] for i in range(4) for c in range(8)]


def transform_4x8(coefficients: list[int], decoder: str) -> list[int]:
    """Residual columns (8 rows of 4, row major) from 8 packed coefficient rows of 4."""
    if decoder == "title":
        rows = [
            four_point(coefficients[4 * i : 4 * i + 4], TITLE_C3, TITLE_C1, TITLE_C2, 7, 1 << 6)
            for i in range(8)
        ]
        column = idct_col
    else:
        rows = [
            four_point(
                coefficients[4 * i : 4 * i + 4], FFMPEG_R3, FFMPEG_R1, FFMPEG_R2, 11, 1 << 10
            )
            for i in range(8)
        ]
        column = simple_column
    columns = [column([rows[i][c] for i in range(8)]) for c in range(4)]
    return [columns[c][i] for i in range(8) for c in range(4)]


def place(kind: str, half: int, values: list[int]) -> dict[int, int]:
    """Residual block index (0..63) to value for one ABT call, half = the call's third argument."""
    if kind == "8x4":
        return {32 * half + i: values[i] for i in range(32)}
    return {8 * r + 4 * half + c: values[4 * r + c] for r in range(8) for c in range(4)}


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
        capture, hashlib.sha256(capture.read_bytes()).hexdigest(), MAX_CAPTURE_BYTES
    )
    uc, registers, _, _ = load_capture(
        data, parse_xbe(checked_bytes(xbe, XBE_SHA256, 8 * 1024 * 1024)), 0x202, 0
    )
    return_address, decoder = struct.unpack("<2I", uc.mem_read(registers[8], 8))
    cols, rows = struct.unpack("<2I", uc.mem_read(decoder + 0xDC, 8))
    output_base = struct.unpack("<I", uc.mem_read(decoder + 0xFC, 4))[0]
    names = {ENTRY_8X4: "8x4", ENTRY_4X8: "4x8", ENTRY_8X8: "8x8"}
    pending: list[tuple] = []
    events: list[tuple] = []

    def read(address: int, size: int = 128) -> bytes:
        return bytes(uc.mem_read(address, size))

    def code2(u: object, address: int, _size: int, _data: object) -> None:
        if address == return_address:
            u.emu_stop()
            return
        esp = u.reg_read(x86.UC_X86_REG_ESP)
        if address in names:
            ret, base, source, half = struct.unpack("<4I", read(esp, 16))
            pending.append((names[address], ret, base, source, half, read(source)))
        elif pending and address == pending[-1][1]:
            kind, _, base, _, half, coefficients = pending.pop()
            events.append(
                (
                    "call",
                    kind,
                    half,
                    struct.unpack("<64h", coefficients),
                    struct.unpack("<64h", read(base)),
                )
            )
        if address == ADD_KERNEL:
            args = struct.unpack("<8I", read(esp + 4, 32))
            events.append(("add", args, struct.unpack("<64h", read(args[6]))))

    uc.hook_add(UC_HOOK_CODE, code2)
    try:
        uc.emu_start(registers[0], 0xFFFFFFFF, timeout=60_000_000, count=30_000_000)
    except UcError as error:
        raise ValueError(f"the original replay stopped: {error}") from error
    return (
        events,
        output_base,
        cols * 16,
        rows * 16,
        bytes(uc.mem_read(output_base, cols * 16 * rows * 16)),
    )


def analyse(args: argparse.Namespace) -> dict:
    events, output_base, stride, height, output = replay(args.capture, args.xbe)
    result: collections.Counter = collections.Counter()
    predicted: set[tuple[int, int]] = set()
    group: list[tuple] = []
    for event in events:
        if event[0] == "call":
            group.append(event)
            continue
        _, kernel_args, snapshot = event
        ffmpeg = list(snapshot)
        for _, kind, half, coefficients, after in group:
            if kind == "8x8":
                continue
            transform = transform_8x4 if kind == "8x4" else transform_4x8
            cells = place(kind, half, transform(list(coefficients), "title"))
            result[f"{kind} title model equals the original"] += all(
                after[i] == v for i, v in cells.items()
            )
            result[f"{kind} calls"] += 1
            cells_ffmpeg = place(kind, half, transform(list(coefficients), "ffmpeg"))
            same = all(after[i] == v for i, v in cells_ffmpeg.items())
            result[f"{kind} FFmpeg model equals the original"] += same
            if kind == "8x4":
                for name, rows_from, column_from in (
                    ("FFmpeg row pass only", "ffmpeg", "title"),
                    ("FFmpeg column constants only", "title", "ffmpeg"),
                ):
                    mixed = place(
                        kind, half, transform_8x4(list(coefficients), rows_from, column_from)
                    )
                    result[f"8x4 {name} equals the original"] += all(
                        after[i] == v for i, v in mixed.items()
                    )
            for i, v in cells_ffmpeg.items():
                ffmpeg[i] = v
        group = []
        destination_y, destination_x = divmod(kernel_args[2] - output_base, stride)
        for i in range(64):
            if ffmpeg[i] != snapshot[i]:
                predicted.add((destination_x + i % 8, destination_y + i // 8))
    summary = {key: result[key] for key in sorted(result)}
    summary["differing_residual_pixels_predicted_by_ffmpeg_model"] = len(predicted)
    if args.movie is not None:
        from tools.diagnostics.xmv_prediction_probe import ffmpeg_frame  # noqa: PLC0415

        luma = ffmpeg_frame(args.movie, args.frame)[0]
        actual = {
            (x, y)
            for y in range(height)
            for x in range(stride)
            if output[y * stride + x] != luma[y * stride + x]
        }
        summary["differing_luma_pixels_title_vs_ffmpeg"] = len(actual)
        summary["actual_differing_pixels_the_model_predicts"] = len(actual & predicted)
        summary["actual_differing_pixels_the_model_misses"] = len(actual - predicted)
        summary["predicted_pixels_that_do_not_differ"] = len(predicted - actual)
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--capture", type=Path, required=True, help="entry_NNNNN.bin of the frame")
    parser.add_argument("--xbe", type=Path, required=True, help="the certified retail default.xbe")
    parser.add_argument("--movie", type=Path, help="the .xmv FFmpeg decodes (optional)")
    parser.add_argument("--frame", type=int, default=2, help="0 based frame of --movie")
    args = parser.parse_args()
    try:
        print(json.dumps(analyse(args), indent=1))
    except ImportError as error:
        print(
            f"refused: needs unicorn (and av for --movie) in one interpreter ({error})",
            file=sys.stderr,
        )
        return 2
    except (OSError, ValueError) as error:
        print(f"refused: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
