"""T538: which original kernel wrote each luma pixel where FFmpeg's frame differs from the original.

Replays one captured codec wrapper entry on the original instructions (bare Unicorn, as
`replay_xmv_capture`) and, for every luma pixel where the ORIGINAL output differs from FFmpeg's,
reports the kernels that wrote that pixel during the replay. FFmpeg's luma comes from a raw file
(`--reference-luma`, visible rows only) that `--emit-reference-luma` writes with PyAV (an optional
dependency of a private venv, which needs neither Unicorn nor Capstone). For the prediction plus
residual kernel `0x449C11` it also reports the interpolation arm (horizontal and vertical half
pixel flags) and the residual word added at the pixel. Numbers only: no pixel value is printed
or stored.

Exit status: 0 done, 2 an input is missing or unusable (including no PyAV for the emit mode).
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import struct
import sys
from pathlib import Path

# The kernels the lifted-kernel tests cover (docs/xmv-contracts.md 13.7) that write picture bytes.
KERNELS = (
    0x445C0C, 0x445FD3, 0x445DF7, 0x4461D2, 0x446CD0, 0x446D39, 0x447EC9, 0x448336, 0x449042,
    0x449210, 0x449618, 0x449899, 0x449C11, 0x449E10, 0x449FF5, 0x44A662, 0x44AE9F, 0x44B0AD,
    0x44B4EB, 0x44B600, 0x44B63E, 0x44B703,
)  # fmt: skip
ADD_KERNEL = 0x449C11  # prediction (4 arms) plus residual, 8x8, `ret 0x1c`


def emit_reference_luma(movie: Path, index: int, output: Path) -> tuple[int, int]:
    """Write FFmpeg's visible luma rows of one frame to `output`; returns width and height."""
    from tools.xmvscan.compare_frames import reference_planes  # noqa: PLC0415 (needs PyAV)

    try:
        frames, width, height = reference_planes(movie)
    except ImportError as error:
        raise ValueError("PyAV is not installed in this interpreter") from error
    if not 0 <= index < len(frames):
        raise ValueError(f"{movie.name} has no frame {index}")
    output.write_bytes(frames[index][0])
    return width, height


def analyse(args: argparse.Namespace) -> dict:
    from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UcError  # noqa: PLC0415
    from unicorn import x86_const as x86  # noqa: PLC0415

    from tools.diagnostics.replay_xmv_capture import (  # noqa: PLC0415
        MAX_CAPTURE_BYTES,
        XBE_SHA256,
        checked_bytes,
        load_capture,
    )
    from tools.xbe import parse_xbe  # noqa: PLC0415

    capture = checked_bytes(
        args.capture, args.capture_sha256 or sha256(args.capture), MAX_CAPTURE_BYTES
    )
    retail = checked_bytes(args.xbe, XBE_SHA256, 8 * 1024 * 1024)
    uc, registers, _, _ = load_capture(capture, parse_xbe(retail), 0x202, 0)
    return_address, decoder = struct.unpack("<2I", uc.mem_read(registers[8], 8))
    cols, rows = struct.unpack("<2I", uc.mem_read(decoder + 0xDC, 8))
    luma = struct.unpack("<I", uc.mem_read(decoder + 0xFC, 4))[0]
    stride = cols * 16
    writers: dict[int, list[int]] = {}
    calls: collections.Counter[tuple[int, int]] = collections.Counter()
    adds: list[tuple[int, ...]] = []
    current = [0]

    def code(u: object, address: int, _size: int, _data: object) -> None:
        if address == return_address:
            u.emu_stop()
        elif address in KERNELS:
            current[0] = address
            if address == ADD_KERNEL:
                esp = u.reg_read(x86.UC_X86_REG_ESP)
                arguments = struct.unpack("<8I", bytes(u.mem_read(esp + 4, 32)))
                adds.append((*arguments, bytes(u.mem_read(arguments[6], 128))))
                calls[arguments[4], arguments[5]] += 1

    def write(
        _u: object, _access: int, address: int, size: int, _value: int, _data: object
    ) -> None:
        if luma <= address < luma + stride * rows * 16:
            for byte in range(size):
                writers.setdefault(address - luma + byte, []).append(current[0])

    uc.hook_add(UC_HOOK_CODE, code)
    uc.hook_add(UC_HOOK_MEM_WRITE, write)
    try:
        uc.emu_start(registers[0], 0xFFFFFFFF, timeout=60_000_000, count=30_000_000)
    except UcError as error:
        raise ValueError(f"the original replay stopped: {error}") from error
    reference = args.reference_luma.read_bytes()
    width, height, line = args.width, args.height, args.width
    if len(reference) != width * height:
        raise ValueError("the reference luma file is not width * height bytes")
    differing = []
    for row in range(height):
        original = bytes(uc.mem_read(luma + row * stride, width))
        for column in range(width):
            delta = original[column] - reference[row * line + column]
            if delta:
                differing.append((row, column, delta))
    pixels = []
    for row, column, delta in differing:
        offset = row * stride + column
        entry: dict = {
            "macroblock_row": row // 16,
            "macroblock_column": column // 16,
            "row_in_macroblock": row % 16,
            "column_in_macroblock": column % 16,
            "original_minus_ffmpeg": delta,
            "writers": [hex(kernel) for kernel in writers.get(offset, [])],
        }
        for (
            _source,
            _source_stride,
            destination,
            destination_stride,
            horizontal,
            vertical,
            _residual_pointer,
            _,
            residual_words,
        ) in adds:
            position = luma + offset - destination
            if (
                luma + offset >= destination
                and destination_stride == stride
                and position // stride < 8
                and position % stride < 8
            ):
                inner_row, inner_column = position // stride, position % stride
                # The residual buffer is shared and cleared after use, so the words are the
                # snapshot taken at the call (T612), not the buffer after the replay.
                at = inner_row * 16 + inner_column * 2
                word = struct.unpack_from("<h", residual_words, at)[0]
                entry.update(
                    {"horizontal_half": horizontal, "vertical_half": vertical, "residual": word}
                )
                break
        pixels.append(entry)
    return {
        "capture": args.capture.name,
        "frame": args.frame,
        "differing_luma_pixels": len(differing),
        "add_kernel_calls_per_arm_horizontal_vertical": {
            f"{horizontal},{vertical}": count
            for (horizontal, vertical), count in sorted(calls.items())
        },
        "final_writer_counts": dict(
            collections.Counter(
                entry["writers"][-1] if entry["writers"] else "none" for entry in pixels
            )
        ),
        "zero_residual_pixels": sum(1 for entry in pixels if entry.get("residual") == 0),
        "pixels": pixels,
    }


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--capture", type=Path, help="entry_NNNNN.bin")
    parser.add_argument(
        "--capture-sha256", help="expected SHA-256 of the capture (default: its own)"
    )
    parser.add_argument("--xbe", type=Path, help="the certified retail default.xbe")
    parser.add_argument("--reference-luma", type=Path, help="FFmpeg's visible luma rows (raw)")
    parser.add_argument("--width", type=int, help="visible width of the reference luma")
    parser.add_argument("--height", type=int, help="visible height of the reference luma")
    parser.add_argument("--frame", type=int, required=True, help="0 based frame of the movie")
    parser.add_argument("--movie", type=Path, help="with --emit-reference-luma, the .xmv")
    parser.add_argument("--emit-reference-luma", type=Path, metavar="OUT", help="write it and stop")
    args = parser.parse_args()
    try:
        if args.emit_reference_luma:
            if args.movie is None:
                parser.error("--emit-reference-luma needs --movie")
            width, height = emit_reference_luma(args.movie, args.frame, args.emit_reference_luma)
            print(json.dumps({"width": width, "height": height, "frame": args.frame}))
            return 0
        if not (args.reference_luma and args.width and args.height and args.capture and args.xbe):
            parser.error("give --capture, --xbe, --reference-luma, --width and --height")
        print(json.dumps(analyse(args), indent=1))
    except (OSError, ValueError) as error:
        print(f"refused: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
