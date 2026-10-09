# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded version-4 XMV metadata inspector.

Layout reference: FFmpeg 7.1 libavformat/xmv.c (official ffmpeg.org release).
The audio/video size adjustment follows that demuxer's documented accounting,
not a claim that all padding, compression flags or timestamps are understood.
"""

import struct

from tools.errors import BadMagicError, ParseError, check_range
from tools.xmvscan.model import AudioTrack, Container, Packet


def parse(data: bytes, *, max_packets: int = 100000, max_audio_tracks: int = 32) -> Container:
    """Validate a complete forward packet chain ending at a zero-next-size header.

    Resource ceilings are inspector policy, not original-library format limits.
    Payload bytes are never decoded; zero-frame and audio alias metadata stay raw.
    """
    if max_packets < 1 or max_audio_tracks < 0:
        raise ValueError("invalid inspector resource ceiling")
    check_range(data, 0, 36, "XMV fixed header")
    prefix_next, first_size, maximum, magic, version, width, height, duration = struct.unpack_from(
        "<8I", data
    )
    if magic != 0x58626F78:
        raise BadMagicError("XMV header requires xobX magic")
    if version != 4:
        raise ParseError(f"unsupported XMV version {version}; inspector supports version4 only")
    if not 0 < width <= 65535 or not 0 < height <= 65535:
        raise ParseError("invalid or unsupported image dimensions")
    tracks, reserved = struct.unpack_from("<HH", data, 32)
    if tracks > max_audio_tracks:
        raise ParseError("audio track ceiling exceeded")
    header_size = 36 + tracks * 12
    check_range(data, 0, header_size, "XMV audio table")
    audio = []
    for index in range(tracks):
        compression, channels, rate, bits, flags = struct.unpack_from(
            "<HHIHH", data, 36 + index * 12
        )
        if not 0 < channels <= 32 or rate == 0 or not 0 < bits <= 32:
            raise ParseError(f"invalid or unsupported audio fields in track {index}")
        audio.append(AudioTrack(compression, channels, rate, bits, flags))
    packet_header = 12 + tracks * 4
    if maximum < packet_header or first_size > maximum or first_size < header_size + packet_header:
        raise ParseError("invalid first packet boundary or maximum packet size")
    offset, size = header_size, first_size - header_size
    packets = []
    while True:
        if len(packets) >= max_packets:
            raise ParseError("packet ceiling exceeded")
        if size < packet_header or size > maximum:
            raise ParseError("invalid packet size")
        check_range(data, offset, size, "XMV packet")
        next_size, video_word, second_word = struct.unpack_from("<3I", data, offset)
        audio_words = struct.unpack_from(f"<{tracks}I", data, offset + 12)
        raw_audio = tuple(word & 0x7FFFFF for word in audio_words)
        raw_video = video_word & 0x7FFFFF
        # FFmpeg subtracts four bytes per track from the video span. A zero audio
        # size after track0 inherits its predecessor for extent accounting only.
        if raw_video < tracks * 4:
            raise ParseError("video size underflows audio header adjustment")
        if video_word & 0x80000000 and raw_video - tracks * 4 < 4:
            raise ParseError("video extradata flag lacks a four-byte payload")
        effective_audio = []
        for index, amount in enumerate(raw_audio):
            effective_audio.append(effective_audio[-1] if amount == 0 and index else amount)
        if raw_video - tracks * 4 + sum(effective_audio) > size - packet_header:
            raise ParseError("audio/video extents exceed packet")
        packets.append(
            Packet(
                offset,
                size,
                next_size,
                raw_video,
                (video_word >> 23) & 255,
                bool(video_word & 0x80000000),
                second_word,
                raw_audio,
            )
        )
        end = offset + size
        if next_size == 0:
            if end != len(data):
                raise ParseError("terminal packet does not end at file boundary")
            break
        if next_size > maximum or next_size < packet_header:
            raise ParseError("invalid linked next packet size")
        offset, size = end, next_size  # strictly positive advance prevents cycles
    return Container(
        version,
        width,
        height,
        duration,
        prefix_next,
        first_size,
        maximum,
        reserved,
        header_size,
        tuple(audio),
        tuple(packets),
        sum(packet.raw_frame_count for packet in packets),
    )
