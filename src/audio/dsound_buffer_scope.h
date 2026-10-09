/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_BUFFER_SCOPE_H
#define TSFP_AUDIO_DSOUND_BUFFER_SCOPE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define DSOUND_BUFFER_DESCRIPTOR_BYTES 24u
#define DSOUND_BUFFER_FORMAT_BYTES 20u
typedef struct dsound_buffer_scope {
    uint32_t descriptor_address, format_address;
    uint32_t flags, buffer_bytes, sample_rate, average_bytes_per_second;
    uint16_t channels, block_align;
    uint8_t descriptor[DSOUND_BUFFER_DESCRIPTOR_BYTES];
    uint8_t format[DSOUND_BUFFER_FORMAT_BYTES];
} dsound_buffer_scope;
/* Measured startup scope, NOT the complete XDK's format acceptance/error rules.
 * Original creation accepts several mutated formats this guard refuses. Exact
 * 24/20 byte snapshots, size24, flags0/10, zero buffer bytes, NULL optional
 * fields, Xbox ADPCM44000 mono for both spatial and ordinary buffers. Refusal
 * preserves the caller-owned host output; no HRESULT, guest writes, allocation or registration. */
bool dsound_buffer_scope_validate(const uint8_t *descriptor, size_t descriptor_bytes,
                                  const uint8_t *format, size_t format_bytes,
                                  dsound_buffer_scope *output);
/* Bounded nonfaulting guest reads into the SAME validated snapshots returned in
 * metadata. Unproven descriptor/format alias refused. Mapping/content lifetime
 * must be quiescent during this compound snapshot; consume returned snapshots
 * rather than rereading guest arguments. Failure preserves host output. */
bool dsound_buffer_scope_snapshot(uint32_t descriptor, dsound_buffer_scope *output);
#endif
