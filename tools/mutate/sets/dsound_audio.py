# ruff: noqa: E501  (mutation anchors are verbatim C source)
"""Reachability mutations for T759's ADPCM decoder and virtual-clock mixer."""

_ADPCM = "src/audio/dsound_adpcm.c"
_MIXER = "src/audio/dsound_mixer.c"
_COMPLETION = "src/audio/dsound_completion.c"
_ADPCM_TEST = ["test_dsound_adpcm"]
_MIXER_TEST = ["test_dsound_mixer"]
_INTEGRATION_TEST = ["test_dsound_audio_integration"]


def _m(file: str, name: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"dsound-audio-{name}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(targets),
        "why": why,
    }


MUTATIONS: list[dict] = [
    _m(
        _ADPCM,
        "header-index-bound",
        "if (index < 0 || index > 88) return false;",
        "if (index < 0 || index > 89) return false;",
        "an out-of-range Xbox ADPCM step index is refused before decoding.",
        _ADPCM_TEST,
    ),
    _m(
        _ADPCM,
        "nibble-order",
        "decode_nibble(&predictor[channel], &step_index[channel], packed & 0x0fu);",
        "decode_nibble(&predictor[channel], &step_index[channel], (uint8_t)(packed >> 4u));",
        "the low nibble is the first sample in each byte.",
        _ADPCM_TEST,
    ),
    _m(
        _ADPCM,
        "frame-capacity",
        "blocks * DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK > pcm_frame_capacity)",
        "blocks * DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK >= pcm_frame_capacity)",
        "exactly sufficient PCM capacity accepts a complete block.",
        _ADPCM_TEST,
    ),
    _m(
        _MIXER,
        "packet-sequential-start",
        "const dsound_frequency_phase start = source_q32(tail) > source_q32(source_now) ? tail : source_now;",
        "const dsound_frequency_phase start = source_q32(tail) < source_q32(source_now) ? tail : source_now;",
        "packets submitted before a stream tail drains play consecutively.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "late-packet-refusal",
        "if (mixer->started && (source_q32(start) < source_q32(rendered_source) ||",
        "if (mixer->started && (source_q32(start) > source_q32(rendered_source) ||",
        "late arrivals cannot be retroactively inserted into rendered PCM.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "saturation-positive",
        "if (mix_left > INT16_MAX) mix_left = INT16_MAX;",
        "if (mix_left > INT16_MAX) mix_left = INT16_MIN;",
        "overlapping streams saturate the mixed sample instead of wrapping.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "silence-gap",
        "        int32_t mix_left = 0;",
        "        int32_t mix_left = 1;",
        "timeline gaps are represented by exact zero-valued stereo frames.",
        _MIXER_TEST,
    ),
    _m(
        _COMPLETION,
        "source-rate-from-format",
        "audio_rate=le32(format+4u);",
        "audio_rate=44100u;",
        "the packet timeline uses the stream's measured current format rate.",
        _INTEGRATION_TEST,
    ),
    _m(
        _MIXER,
        "volume-not-applied",
        "if (stream->attenuated) {",
        "if (false) {",
        "T818: a SetVolume below 0 scales the stream's PCM at render time.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "volume-floor-not-mute",
        "if (millibels == DSOUND_MIXER_VOLUME_MIN) { *gain_q16 = 0u; return true; }",
        "",
        "T818: -10000 is silence, not the 10^-5 gain the formula gives.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "volume-zero-not-unity",
        "if (millibels == 0) { *gain_q16 = 65536u; return true; }",
        "if (millibels == 0) { *gain_q16 = 65535u; return true; }",
        "T818: volume 0 is exactly unity gain.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "volume-above-zero-accepted",
        "millibels > 0 || millibels < DSOUND_MIXER_VOLUME_MIN) return false;",
        "millibels > 1 || millibels < DSOUND_MIXER_VOLUME_MIN) return false;",
        "T818: a volume above 0 (DSBVOLUME_MAX) is refused.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "volume-below-floor-accepted",
        "millibels > 0 || millibels < DSOUND_MIXER_VOLUME_MIN) return false;",
        "millibels > 0 || millibels < DSOUND_MIXER_VOLUME_MIN - 1) return false;",
        "T818: a volume below -10000 is refused.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "volume-divisor",
        "(double)millibels * 2.302585092994046 / 2000.0 / 256.0",
        "(double)millibels * 2.302585092994046 / 1000.0 / 256.0",
        "T818: gain = 10^(mb/2000), the amplitude decibel the title builds with 2000*log10.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "resample-no-join-neighbour",
        "if (later ? packet_start_q32(other) == packet_end_q32(packet) : packet_end_q32(other) == packet_start_q32(packet)) return other;",
        "if (later ? packet_start_q32(other) == packet_end_q32(packet) + 1u : packet_end_q32(other) == packet_start_q32(packet) + 1u) return other;",
        "T818: the filter reads through to the back to back packet, the join is continuous.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "resample-no-history",
        "if (packet->used && source_q32(source_now) >= packet_end_q32(packet) +\n            ((unsigned __int128)mul_div_ceil(RESAMPLE_HALF_TAPS, mixer->ticks_per_second,\n                                           packet->source_rate_hz) << 32u)) {",
        "if (packet->used && source_q32(source_now) >= packet_end_q32(packet)) {",
        "T818: a finished packet stays long enough to be the filter history of the next one.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "resample-linear-only",
        "if (playback_rate_q32 < ((uint64_t)mixer->output_rate_hz<<32u)) {",
        "if (false && playback_rate_q32 < ((uint64_t)mixer->output_rate_hz<<32u)) {",
        "T818: an upsampling stream goes through the band limited filter, not linear interpolation.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "resample-kaiser-beta",
        "#define RESAMPLE_KAISER_BETA 8.0",
        "#define RESAMPLE_KAISER_BETA 2.0",
        "T818: the window keeps the stop band below -60 dB.",
        _MIXER_TEST,
    ),
    _m(
        _MIXER,
        "resample-phase-not-normalised",
        "resample_table[phase][tap] /= sum;",
        "resample_table[phase][tap] /= sum * 1.05;",
        "T818: every phase sums to 1 so a constant and the tone level stay exact.",
        _MIXER_TEST,
    ),
]
