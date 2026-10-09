/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_EFFECTS_METADATA_H
#define TSFP_AUDIO_DSOUND_EFFECTS_METADATA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define DSOUND_EFFECTS_IMAGE_ADDRESS 0x007F78C0u
#define DSOUND_EFFECTS_IMAGE_BYTES 18608u
#define DSOUND_EFFECTS_MAX_MAPS 9u
typedef struct dsound_effects_map {
    uint32_t code_offset, code_bytes, state_offset, state_bytes;
    uint32_t y_offset, y_bytes, workspace_offset, workspace_bytes;
} dsound_effects_map;
typedef struct dsound_effects_metadata {
    uint32_t code_offset, code_bytes, state_offset, state_bytes;
    uint32_t descriptor_offset, descriptor_bytes, iv_offset, iv_bytes;
    uint32_t map_count, workspace_bytes;
    dsound_effects_map maps[DSOUND_EFFECTS_MAX_MAPS];
} dsound_effects_metadata;
/* Pure bounded layout validation, deliberately separate from retail identity.
 * Accepts one to nine maps, disjoint nonempty code/state/workspace slices,
 * zero-size aliases and only the measured empty Y layout. Outputs image-relative
 * code/state offsets and workspace-relative offsets. Failure preserves output. */
bool dsound_effects_validate_layout(const uint8_t *image, size_t bytes,
                                   dsound_effects_metadata *output);
/* Read-only known-image parser: fixed VA/size, complete FNV-1a64 + CRC32 content
 * fingerprints and measured header/layout guards. Fingerprints detect accidental
 * changes; they are not cryptographic authentication. No guest allocation,
 * mutation, decryption, execution or DSP acknowledgement. Caller must keep guest
 * mappings quiescent during snapshot; unreadable/guard pages are refused. */
bool dsound_effects_parse_guest(uint32_t address, uint32_t bytes,
                                dsound_effects_metadata *output);
/* Same guard as parse_guest, also returns the exact validated snapshot. Both host
 * outputs remain unchanged on refusal; image must hold IMAGE_BYTES bytes. */
bool dsound_effects_snapshot_guest(uint32_t address, uint32_t bytes, uint8_t *image,
                                   dsound_effects_metadata *output);
#endif
