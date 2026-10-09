/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_EFFECTS_BINDING_H
#define TSFP_AUDIO_DSOUND_EFFECTS_BINDING_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Passive CPU views, no DSP execution/acknowledgement. Default ON in the host since
 * T72a: the acceptance contract is fully measured, so the policy gate had become the
 * one artificial refusal at the reached call. set_enabled(false) still refuses.
 * Bind only the measured image/location to this adapter's owned SILENT device. */
void dsound_effects_binding_set_enabled(bool enabled);
/* Configure quiescent before binding. False preserves policy when support is
 * absent or an owned binding is already live. Default remains passive. */
bool dsound_effects_binding_set_gp_enabled(bool enabled);
/* INFERRED opt-in CPU-view/MMIO bridge. Original global failure and invalid-index
 * HRESULTs preserved; unknown ownership/span/MMIO domains stop by named refusal.
 * Immediate accesses require aligned DWORDs with low24-bit operands. */
uint32_t dsound_effects_binding_apply(uint32_t interface,uint32_t index,uint32_t offset,
    uint32_t source,uint32_t bytes,uint32_t flags);
typedef struct dsound_effects_binding_gp_view {
    uint32_t heap,device_heap,internal,descriptor,state,staging;
    uint32_t pending_start,pending_bytes;
} dsound_effects_binding_gp_view;
/* Observation only, never a lease/capability; failures preserve output. */
bool dsound_effects_binding_gp_view_get(uint32_t interface,dsound_effects_binding_gp_view *output);
/* Real firmware consumer; no deferred-settings registry replacement or implicit
 * PCM scheduling. Frames require caller-supplied genuine preprojection bins. */
bool dsound_effects_binding_gp_commit(uint32_t interface);
bool dsound_effects_binding_gp_frame(uint32_t interface,const uint32_t input[1024],
    uint32_t output[1024]);
typedef void (*dsound_effects_binding_fatal_fn)(uint32_t address, const char *message);
void dsound_effects_binding_set_fatal(dsound_effects_binding_fatal_fn handler);
uint32_t dsound_effects_binding_bind(uint32_t interface, uint32_t image, uint32_t bytes,
                                    uint32_t location, uint32_t output);
size_t dsound_effects_binding_register(void);
/* Quiescent reset: call before dsound_device_reset. Preserves enabled policy. */
void dsound_effects_binding_reset(void);
#endif
