# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only structural facts; no decoder or guest API semantics."""

from dataclasses import dataclass


@dataclass(frozen=True)
class AudioTrack:
    compression: int
    channels: int
    sample_rate: int
    bits_per_sample: int
    flags: int


@dataclass(frozen=True)
class Packet:
    offset: int
    size: int
    next_size: int
    raw_video_size: int
    raw_frame_count: int
    has_extradata: bool
    video_header_second_word: int
    raw_audio_sizes: tuple[int, ...]


@dataclass(frozen=True)
class Container:
    version: int
    width: int
    height: int
    raw_duration: int
    prefix_next_size: int
    first_size: int
    max_packet_size: int
    reserved: int
    header_size: int
    audio: tuple[AudioTrack, ...]
    packets: tuple[Packet, ...]
    raw_frame_count: int
    duration_note: str = (
        "Raw header duration is not reconciled with packet counts or decoded timestamps. "
        "No playback duration, EOF guest result, codec acceptance or decode is established."
    )
