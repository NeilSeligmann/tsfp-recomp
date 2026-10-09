# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare frames from the native decoder run with an independent decoder (T394).

The native side is `tsfp_host --native-xmv --trace-xmv --dump-xmv-frames DIR`, one `frame_NNNNN.yuv`
per frame holding the decoder's macroblock aligned Y, U and V planes. The independent side is PyAV
(FFmpeg's WMV2 decoder, the T233 proof). Both are the user's disc data, so frames are read from and
written to gitignored directories only and nothing here stores pixels. PyAV is an optional,
environment-local dependency (a private venv under tmp/), a missing one is reported, never skipped
silently. The report is numbers only: per frame and plane exact equality, differing byte count,
largest and mean absolute difference.

`--overlay-rgb` (T537) compares the pictures `tsfp_host --dump-overlay DIR` wrote (RGB PNGs, one per
UpdateOverlay) with FFmpeg's decode of the same movie in the RGB domain, three ways: the native
picture against FFmpeg's planes converted with the SAME matrix (the decode difference alone), that
model against FFmpeg's own swscale BT.601 conversion (the matrix difference alone) and the native
picture against swscale.
"""

import argparse
import json
import re
import struct
import sys
import zlib
from pathlib import Path
from typing import Any


def plane_stats(native: bytes, reference: bytes) -> dict[str, Any]:
    """Compare two equal-length planes byte by byte."""
    if len(native) != len(reference):
        raise ValueError(f"plane sizes differ: {len(native)} against {len(reference)}")
    differing, largest, total = 0, 0, 0
    for left, right in zip(native, reference, strict=True):
        if left != right:
            difference = abs(left - right)
            differing += 1
            total += difference
            largest = max(largest, difference)
    return {
        "bytes": len(native),
        "differing": differing,
        "max_abs": largest,
        "mean_abs": total / len(native) if native else 0.0,
    }


def split_planes(frame: bytes, cols: int, rows: int) -> tuple[bytes, bytes, bytes]:
    """Split one native frame file (macroblock aligned planes) into Y, U and V."""
    luma, chroma = 256 * cols * rows, 64 * cols * rows
    if len(frame) != luma + 2 * chroma:
        raise ValueError(f"frame file is {len(frame)} bytes, expected {luma + 2 * chroma}")
    return frame[:luma], frame[luma : luma + chroma], frame[luma + chroma :]


def crop(data: bytes, stride: int, width: int, height: int) -> bytes:
    """Visible window of a plane stored with `stride` bytes per row."""
    return b"".join(data[row * stride : row * stride + width] for row in range(height))


def reference_planes(path: Path) -> tuple[list[tuple[bytes, bytes, bytes]], int, int]:
    """Decode every video frame with PyAV as YUV420P planes with no row padding."""
    try:
        import av  # noqa: PLC0415
    except ImportError as error:
        raise SystemExit(
            "PyAV is not installed. Create a private environment, for example "
            "`uv venv tmp/venv && uv pip install --python tmp/venv/bin/python av` and run this "
            "tool with that interpreter."
        ) from error
    frames: list[tuple[bytes, bytes, bytes]] = []
    width = height = 0
    with av.open(str(path)) as container:
        for frame in container.decode(video=0):
            converted = frame.reformat(format="yuv420p")
            width, height = converted.width, converted.height
            planes = []
            for index, plane in enumerate(converted.planes):
                plane_width = width if index == 0 else (width + 1) // 2
                plane_height = height if index == 0 else (height + 1) // 2
                planes.append(crop(bytes(plane), plane.line_size, plane_width, plane_height))
            frames.append((planes[0], planes[1], planes[2]))
    return frames, width, height


# The matrix of src/gpu/d3d8_overlay_image.c (the title's own XMV library, BT.601 studio range,
# 15 bit fixed point, proven against the original bytes in tests/test_overlay_image_original.py).
LUMA_GAIN, V_TO_RED, V_TO_GREEN, U_TO_GREEN, U_TO_BLUE = 38139, 52298, -26640, -12812, 66126


def clamp_byte(value: int) -> int:
    return 0 if value < 0 else 255 if value > 255 else value


def yuv_to_rgb(y: int, u: int, v: int) -> tuple[int, int, int]:
    """One sample through the title's matrix (floor shift, no rounding offset)."""
    luma, blue, red = (y - 16) * LUMA_GAIN, u - 128, v - 128
    return (
        clamp_byte((luma + V_TO_RED * red) >> 15),
        clamp_byte((luma + V_TO_GREEN * red + U_TO_GREEN * blue) >> 15),
        clamp_byte((luma + U_TO_BLUE * blue) >> 15),
    )


def planes_to_rgb(planes: tuple[bytes, bytes, bytes], width: int, height: int) -> bytes:
    """4:2:0 planes (no row padding) to packed RGB, chroma replicated over each 2x2 block."""
    luma, blue, red = planes
    chroma_width = (width + 1) // 2
    table: dict[tuple[int, int, int], tuple[int, int, int]] = {}
    out = bytearray(3 * width * height)
    for row in range(height):
        for column in range(width):
            key = (
                luma[row * width + column],
                blue[(row // 2) * chroma_width + column // 2],
                red[(row // 2) * chroma_width + column // 2],
            )
            if key not in table:
                table[key] = yuv_to_rgb(*key)
            out[3 * (row * width + column) : 3 * (row * width + column) + 3] = bytes(table[key])
    return bytes(out)


def read_png_rgb(path: Path) -> tuple[bytes, int, int]:
    """Read an 8 bit RGB PNG with filter 0 rows (what the host writes). Anything else is refused."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")
    position, idat, width, height = 8, b"", 0, 0
    while position < len(data):
        length, kind = struct.unpack_from(">I4s", data, position)
        body = data[position + 8 : position + 8 + length]
        if zlib.crc32(kind + body) != struct.unpack_from(">I", data, position + 8 + length)[0]:
            raise ValueError(f"{path}: bad CRC in {kind!r}")
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack_from(">IIBB", body)
            if (depth, colour) != (8, 2):
                raise ValueError(f"{path}: not 8 bit RGB")
        elif kind == b"IDAT":
            idat += body
        position += 12 + length
    raw = zlib.decompress(idat)
    stride = 1 + 3 * width
    if len(raw) != stride * height or any(raw[row * stride] for row in range(height)):
        raise ValueError(f"{path}: unexpected filter or size")
    return (
        b"".join(raw[row * stride + 1 : (row + 1) * stride] for row in range(height)),
        width,
        height,
    )


def rgb_stats(left: bytes, right: bytes) -> dict[str, Any]:
    """Per pixel comparison of two packed RGB pictures: pixels that differ, per channel extremes."""
    if len(left) != len(right):
        raise ValueError(f"picture sizes differ: {len(left)} against {len(right)}")
    pixels = len(left) // 3
    differing = largest = total = 0
    for index in range(pixels):
        a, b = left[3 * index : 3 * index + 3], right[3 * index : 3 * index + 3]
        if a != b:
            differing += 1
            step = max(abs(x - y) for x, y in zip(a, b, strict=True))
            largest = max(largest, step)
            total += sum(abs(x - y) for x, y in zip(a, b, strict=True))
    return {
        "pixels": pixels,
        "differing_pixels": differing,
        "max_abs": largest,
        "mean_abs": total / (3 * pixels) if pixels else 0.0,
    }


def reference_rgb(path: Path) -> list[tuple[tuple[bytes, bytes, bytes], bytes]]:
    """FFmpeg's planes and its own swscale BT.601 RGB (the PyAV default for this size) per frame."""
    import av  # noqa: PLC0415

    frames = []
    with av.open(str(path)) as container:
        for frame in container.decode(video=0):
            converted = frame.reformat(format="yuv420p")
            planes = []
            for index, plane in enumerate(converted.planes):
                plane_width = converted.width if index == 0 else (converted.width + 1) // 2
                plane_height = converted.height if index == 0 else (converted.height + 1) // 2
                planes.append(crop(bytes(plane), plane.line_size, plane_width, plane_height))
            rgb = frame.reformat(format="rgb24")
            swscale = crop(bytes(rgb.planes[0]), rgb.planes[0].line_size, 3 * rgb.width, rgb.height)
            frames.append(((planes[0], planes[1], planes[2]), swscale))
    return frames


def compare_overlay(xmv: Path, overlay_dir: Path, offset: int, wanted: list[int]) -> dict[str, Any]:
    """The dumped overlay pictures `offset + n` against FFmpeg's frame n, for each n in `wanted`."""
    reference = reference_rgb(xmv)
    report: dict[str, Any] = {"reference_frames": len(reference), "frames": []}
    for index in wanted:
        native, width, height = read_png_rgb(overlay_dir / f"overlay_{offset + index:05d}.png")
        planes, swscale = reference[index]
        model = planes_to_rgb(planes, width, height)
        report["frames"].append(
            {
                "index": index,
                "native_vs_ffmpeg_planes_same_matrix": rgb_stats(native, model),
                "same_matrix_vs_swscale_bt601": rgb_stats(model, swscale),
                "native_vs_swscale_bt601": rgb_stats(native, swscale),
            }
        )
    return report


HASH_LINE = re.compile(r"xmv-original: frame (\d+) (\d+)x(\d+) macroblocks crc32=([0-9A-F]{8})")


def compare_hashes(xmv: Path, log: Path, offset: int) -> dict[str, Any]:
    """Match the host's per frame CRC-32 of the macroblock aligned planes against the reference.

    The reference planes are contiguous Y, U, V of a size that is a whole number of macroblocks
    (every movie on the disc), so the host's planes and the reference's are the same bytes when they
    agree.
    """
    reference, width, height = reference_planes(xmv)
    if width % 16 or height % 16:
        raise SystemExit("hash mode needs a macroblock aligned size, use the plane mode instead")
    wanted = [
        (int(m[1]), m[4])
        for line in log.read_text(errors="replace").splitlines()
        if (m := HASH_LINE.search(line))
    ][offset : offset + len(reference)]
    exact = 0
    mismatched: list[int] = []
    for index, (_, host_crc) in enumerate(wanted):
        crc = zlib.crc32(
            reference[index][2], zlib.crc32(reference[index][1], zlib.crc32(reference[index][0]))
        )
        if f"{crc:08X}" == host_crc:
            exact += 1
        else:
            mismatched.append(index)
    return {
        "reference_frames": len(reference),
        "native_frames": len(wanted),
        "width": width,
        "height": height,
        "exact_frames": exact,
        "mismatched": mismatched,
    }


def compare(xmv: Path, frames_dir: Path, offset: int = 0) -> dict[str, Any]:
    reference, width, height = reference_planes(xmv)
    native_files = sorted(frames_dir.glob("frame_*.yuv"))[offset : offset + len(reference)]
    cols, rows = (width + 15) // 16, (height + 15) // 16
    report: dict[str, Any] = {
        "reference_frames": len(reference),
        "native_frames": len(native_files),
        "width": width,
        "height": height,
        "frames": [],
    }
    for index, file in enumerate(native_files):
        luma, blue, red = split_planes(file.read_bytes(), cols, rows)
        native = (
            crop(luma, 16 * cols, width, height),
            crop(blue, 8 * cols, (width + 1) // 2, (height + 1) // 2),
            crop(red, 8 * cols, (width + 1) // 2, (height + 1) // 2),
        )
        entry = {"index": index}
        for name, left, right in zip("YUV", native, reference[index], strict=True):
            entry[name] = plane_stats(left, right)
        report["frames"].append(entry)
    exact = sum(
        1 for entry in report["frames"] if all(entry[name]["differing"] == 0 for name in "YUV")
    )
    report["exact_frames"] = exact
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("xmv", type=Path, help="the movie file (extract it into a gitignored dir)")
    parser.add_argument(
        "frames",
        type=Path,
        help="directory of frame_NNNNN.yuv from the host, or with --hash-log the host's stderr log",
    )
    parser.add_argument("--json", action="store_true", help="print the whole report as JSON")
    parser.add_argument(
        "--hash-log", action="store_true", help="compare per frame CRC-32 lines instead of files"
    )
    parser.add_argument(
        "--overlay-rgb",
        action="store_true",
        help="`frames` is the host's --dump-overlay directory, compare its RGB pictures",
    )
    parser.add_argument(
        "--frames-wanted",
        type=int,
        nargs="+",
        default=[0, 1, 2, 3, 4],
        help="with --overlay-rgb, the movie frame indexes to compare",
    )
    parser.add_argument(
        "--offset",
        type=int,
        default=0,
        help="host frames to skip (an earlier movie in the same run)",
    )
    args = parser.parse_args(argv)
    if args.overlay_rgb:
        report = compare_overlay(args.xmv, args.frames, args.offset, args.frames_wanted)
        print(json.dumps(report, indent=1))
        return 0
    if args.hash_log:
        report = compare_hashes(args.xmv, args.frames, args.offset)
        print(json.dumps(report) if args.json else report)
        return 0
    report = compare(args.xmv, args.frames, args.offset)
    if args.json:
        print(json.dumps(report, indent=1))
        return 0
    print(
        f"reference {report['reference_frames']} frames {report['width']}x{report['height']}, "
        f"native {report['native_frames']} frames, exact {report['exact_frames']}"
    )
    for entry in report["frames"][:5] + report["frames"][-2:]:
        print(entry)
    return 0


if __name__ == "__main__":
    sys.exit(main())
