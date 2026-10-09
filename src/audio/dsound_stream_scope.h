/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_STREAM_SCOPE_H
#define TSFP_AUDIO_DSOUND_STREAM_SCOPE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define DSOUND_STREAM_DESCRIPTOR_BYTES 24u
#define DSOUND_STREAM_FORMAT_BYTES 20u
typedef struct dsound_stream_scope {
    uint32_t descriptor_address, format_address;
    uint32_t flags, max_packets, sample_rate, average_bytes_per_second;
    uint16_t channels, block_align;
    uint8_t descriptor[DSOUND_STREAM_DESCRIPTOR_BYTES];
    uint8_t format[DSOUND_STREAM_FORMAT_BYTES];
} dsound_stream_scope;
/* Measured startup scope, NOT the complete XDK's format acceptance/error rules.
 * Original creation accepts several mutated formats this guard refuses. Exact
 * 24/20 byte snapshots, flags0/10, three packets, no callback/context/mixbins,
 * Xbox ADPCM44100 mono-spatial or stereo-nonspatial only. Refusal preserves the
 * caller-owned host output; no HRESULT, guest writes, allocation or registration. */
bool dsound_stream_scope_validate(const uint8_t *descriptor, size_t descriptor_bytes,
                                  const uint8_t *format, size_t format_bytes,
                                  dsound_stream_scope *output);
/* Bounded nonfaulting guest reads into the SAME validated snapshots returned in
 * metadata. Unproven descriptor/format alias refused. Mapping/content lifetime
 * must be quiescent during this compound snapshot; consume returned snapshots
 * rather than rereading guest arguments. Failure preserves host output. */
bool dsound_stream_scope_snapshot(uint32_t descriptor, dsound_stream_scope *output);
#endif
