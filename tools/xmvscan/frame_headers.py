# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded raw XMV frame-header metadata; no decompression or timing semantics."""

import argparse
import json
import struct
from dataclasses import asdict, dataclass
from pathlib import Path

from tools.errors import ParseError, TruncatedError
from tools.xmvscan import parse


@dataclass(frozen=True)
class FrameHeader:
    packet_index: int
    frame_index: int
    offset: int
    raw_word: int
    payload_size: int
    raw_delta: int
    cumulative_uint32: int


@dataclass(frozen=True)
class FrameReport:
    frames: tuple[FrameHeader, ...]
    packet_padding_sizes: tuple[int, ...]
    cumulative_uint32: int
    raw_container_duration: int
    note: str = (
        "Structural raw values only: no milliseconds, decoder, playback clock, "
        "EOF or guest acceptance equivalence. Zero frame counts stay zero; "
        "unconsumed video bytes are reported as padding without interpreting them."
    )


def inspect_frames(data: bytes, *, max_frames: int = 1000000) -> FrameReport:
    """Read every declared frame within its validated packet video span.

    Payload bytes are never returned. Count and resource guards are inspector
    policy. Cumulative values use the original extraction's uint32 arithmetic.
    """
    if max_frames < 0:
        raise ValueError("negative frame ceiling")
    container = parse(data)
    frames = []
    padding = []
    cumulative = 0
    for packet_index, packet in enumerate(container.packets):
        cursor = packet.offset + 12 + len(container.audio) * 4
        end = cursor + packet.raw_video_size - len(container.audio) * 4
        if packet.has_extradata:
            cursor += 4  # parser already requires the four-byte extradata extent
        for frame_index in range(packet.raw_frame_count):
            if len(frames) >= max_frames:
                raise ParseError("frame ceiling exceeded")
            if cursor + 4 > end:
                raise TruncatedError("declared frame header exceeds video span")
            raw = struct.unpack_from("<I", data, cursor)[0]
            payload_size = (raw & 0x1FFFF) * 4 + 4
            if cursor + 4 + payload_size > end:
                raise TruncatedError("declared frame payload exceeds video span")
            delta = raw >> 17
            cumulative = (cumulative + delta) & 0xFFFFFFFF
            frames.append(
                FrameHeader(packet_index, frame_index, cursor, raw, payload_size, delta, cumulative)
            )
            cursor += 4 + payload_size
        padding.append(end - cursor)
    return FrameReport(tuple(frames), tuple(padding), cumulative, container.raw_duration)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    args = parser.parse_args(argv)
    try:
        result = inspect_frames(args.input.read_bytes())
    except (OSError, ParseError) as error:
        parser.exit(2, f"XMV frame headers: {error}\n")
    print(json.dumps(asdict(result), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
