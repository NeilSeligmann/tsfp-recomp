/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_DSOUND_EFFECTS_DSP_H
#define TSFP_DSOUND_EFFECTS_DSP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct dsound_effects_dsp dsound_effects_dsp;
/* Private, caller-serialized GP core using pinned xemu instructions and DMA.
 * Only the attested retail firmware/image is accepted. No guest registration,
 * fabricated acknowledgment, APU voice scheduling or full-VM timing contract. */
dsound_effects_dsp *dsound_effects_dsp_create(const uint8_t *firmware, size_t firmware_bytes,
                                             const uint8_t *image, size_t image_bytes);
/* Original Download returns when command3 is consumed, before a whole GP frame.
 * This constructor stops at that real firmware boundary. */
dsound_effects_dsp *dsound_effects_dsp_create_at_download(const uint8_t *firmware,
    size_t firmware_bytes,const uint8_t *image,size_t image_bytes);
/* Separate original first RAM copy from the subsequent MMIO copy; preserves
 * source rereads/alias chronology in the guest adapter. The aligned live API
 * admits only the pinned xemu four-byte/24-bit operand domain. */
bool dsound_effects_dsp_stage(dsound_effects_dsp *dsp,unsigned map,size_t offset,
    const uint8_t *source,size_t bytes);
bool dsound_effects_dsp_live_aligned(dsound_effects_dsp *dsp,unsigned map,size_t offset,
    const uint8_t *source,size_t bytes);
bool dsound_effects_dsp_defer(dsound_effects_dsp *dsp,unsigned map,size_t offset,size_t bytes);
/* Diagnostic snapshot; caller retains serialization. Original stage offsets. */
bool dsound_effects_dsp_pending(const dsound_effects_dsp *dsp,uint32_t *start,uint32_t *bytes);
void dsound_effects_dsp_destroy(dsound_effects_dsp *dsp);
/* Stage every byte first; immediate writes also reach GP XRAM. Deferred writes
 * remain staged until a real command2 consumer runs during commit. Host sources
 * are quiescent readable spans, separate from the context's private storage. */
bool dsound_effects_dsp_write(dsound_effects_dsp *dsp, unsigned map, size_t offset,
                              const uint8_t *source, size_t bytes, bool deferred);
bool dsound_effects_dsp_commit(dsound_effects_dsp *dsp);
/* One actual 32-sample GP frame, bin-major signed24 words in low24 bits.
 * Output is copied only on successful firmware halt. GP timing is not modeled. */
bool dsound_effects_dsp_frame(dsound_effects_dsp *dsp, const uint32_t input[1024],
                              uint32_t output[1024]);
bool dsound_effects_dsp_read(dsound_effects_dsp *dsp, unsigned map, size_t offset,
                             uint8_t *output, size_t bytes);
const char *dsound_effects_dsp_error(const dsound_effects_dsp *dsp);
#endif
