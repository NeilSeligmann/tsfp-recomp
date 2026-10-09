/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_LISTENER_H
#define TSFP_AUDIO_DSOUND_LISTENER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dsound_device.h"
/* Default-off passive startup cache, independently enabled. Embedding host MUST
 * verify all DSOUND manual wrapper routes reach native dispatch before enabling.
 * Raw scalar host metadata only: no guest settings/derived3D/list/APU/DSP/FP or
 * guest critical-section operations. Mapping/content/heap lifetime quiescent. */
void dsound_listener_set_enabled(bool enabled);
typedef bool (*dsound_listener_irql_provider)(uint8_t *out);
typedef void (*dsound_listener_fatal_fn)(uint32_t address,const char *reason);
/* IRQL provider must report only; no adapter/device reentry. */
void dsound_listener_set_irql_provider(dsound_listener_irql_provider provider);
void dsound_listener_set_fatal(dsound_listener_fatal_fn fatal);
uint32_t dsound_listener_cache_doppler(uint32_t interface,uint32_t factor,uint32_t apply);
uint32_t dsound_listener_cache_position(uint32_t interface,uint32_t x,uint32_t y,uint32_t z,uint32_t apply);
uint32_t dsound_listener_cache_orientation(uint32_t interface,uint32_t fx,uint32_t fy,uint32_t fz,
                                         uint32_t tx,uint32_t ty,uint32_t tz,uint32_t apply);
/* Completed exact listener cache only, idempotent HOST request observation.
 * No guest settings/cache clear, child propagation, APU or completion effects. */
uint32_t dsound_listener_cache_commit(uint32_t interface);
/* Singleton-owned completed listener/commit only. HOST work observation, no
 * notifications/child progress, KeStall, MMIO, clock advance or completion.
 * Original VOID function has observed EAX0, not an HRESULT contract. */
uint32_t dsound_listener_cache_work(void);
size_t dsound_listener_register(void);
typedef struct dsound_listener_snapshot {
    dsound_device_identity identity;
    uint32_t cache_mask,doppler_bits,position[3],orientation[6];
    bool commit_seen;
    bool work_seen;
} dsound_listener_snapshot;
/* Read-only host observation; full parent/incarnation validation. Refusal preserves
 * output. Success before first setter returns zero cache/default scalar bits. */
bool dsound_listener_get_snapshot(uint32_t interface,dsound_listener_snapshot *output);
/* Quiescent HOST-only reset before device teardown; preserves configured policy. */
void dsound_listener_reset(void);
/* Optional route for DirectSoundDoWork from callers this module does not own (the T392 movie stream).
 * NULL (default) changes nothing. A true return means the route handled the call, false leaves the
 * measured startup caller policy to decide exactly as before. Installed at startup, quiescent. */
typedef bool (*dsound_listener_work_route_fn)(uint32_t return_address);
void dsound_listener_set_work_route(dsound_listener_work_route_fn route);
#endif
