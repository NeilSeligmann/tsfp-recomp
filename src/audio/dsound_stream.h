/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_STREAM_H
#define TSFP_AUDIO_DSOUND_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dsound_device.h"
#include "kernel_call.h"
#include "dsound_stream_scope.h"
#include "dsound_stream_routing.h"
/* Default-off passive public stream policy, independent of codec/effects policy.
 * Embedding host MUST verify all T161 entry and T163 wrapper stop boundaries
 * before enabling: the original vtables are unsafe without those protections.
 * Header words0..3 are primary/secondary vtables, ref1, and parent singleton
 * internal address (no +4 parent adjustment). Words4..9 are policy zeros.
 * No settings/hardware/list/packet objects, DSP/FP/critical-section operations,
 * audio activity or packet completion are implemented. Original tables and all
 * guest mappings/content referenced by a transaction must remain quiescent.
 * Runtime table targets must belong to the verified stop-only address set;
 * this safety membership test is not ordered-table/original-body equivalence. */
void dsound_stream_set_enabled(bool enabled);
typedef bool (*dsound_stream_irql_provider)(uint8_t *out);
typedef void (*dsound_stream_fatal_fn)(uint32_t address,const char *reason);
/* T681: the opt-in passive completion model. With it on, SetVolume of a started stream takes any value and a started stream accepts Pause mode 0 (resume). `note` hears every admitted
 * Pause (stream address, mode) under the stream lock and must not re-enter this module. Off (default) changes nothing. */
typedef void (*dsound_stream_pause_note_fn)(uint32_t stream,uint32_t mode);
void dsound_stream_set_completion(bool enabled,dsound_stream_pause_note_fn note);
/* Optional real PCM routing transaction, installed quiescent before register.
 * Called with stream ownership lock held; must not reenter this module.
 * NULL leaves the routing trio absent and preserves prior volume policy. */
typedef bool (*dsound_stream_routing_note_fn)(uint32_t stream, uint64_t serial,
                                              const dsound_stream_routing *routing);
void dsound_stream_set_routing_note(dsound_stream_routing_note_fn note);
uint32_t dsound_stream_set_headroom(uint32_t stream, uint32_t headroom);
uint32_t dsound_stream_set_mix_bins(uint32_t stream, uint32_t mix_bins);
uint32_t dsound_stream_set_mix_bin_volumes(uint32_t stream, uint32_t mix_bins);
/* T1245: optional real frequency transaction and guest control-word provider,
 * installed quiescent. Consumer runs under device -> stream ownership locks. */
typedef bool (*dsound_stream_frequency_note_fn)(uint32_t stream,uint64_t serial,
    uint32_t old_hz,uint32_t new_hz);
typedef bool (*dsound_stream_control_word_fn)(uint16_t *word);
void dsound_stream_set_frequency_note(dsound_stream_frequency_note_fn note,
    dsound_stream_control_word_fn control_word);
/* SetFormat aborts pending packets and resets effective pitch. */
typedef bool (*dsound_stream_format_note_fn)(uint32_t stream,uint64_t serial,
    uint32_t old_hz,uint32_t new_hz,uint32_t format_address);
void dsound_stream_set_format_note(dsound_stream_format_note_fn note);
uint32_t dsound_stream_set_frequency(uint32_t stream,uint32_t hertz,uint16_t control_word);
/* Process-lifetime accepted real frequency transactions; refused/global failure calls do not count. */
uint64_t dsound_stream_frequency_set_count(void);
void dsound_stream_set_irql_provider(dsound_stream_irql_provider provider);
void dsound_stream_set_fatal(dsound_stream_fatal_fn fatal);
/* Optional extension for streams this module does not own (the T392 movie stream). NULL (default)
 * changes nothing. `route` runs first for every public stream row with the guest return address, and a
 * true return is the answer (result set). False leaves the startup policy to decide exactly as before.
 * `reset` runs before the startup streams are reset and its false makes the checked reset report a
 * refusal. Installed at startup, quiescent. Neither may re-enter this module's configuration. */
typedef bool (*dsound_stream_public_route_fn)(uint32_t entry,const kernel_call_frame *frame,
                                              uint32_t return_address,uint32_t *result);
typedef bool (*dsound_stream_reset_route_fn)(void);
void dsound_stream_set_extension(dsound_stream_public_route_fn route,dsound_stream_reset_route_fn reset);
/* Typed policy helpers enforce scope/ownership/IRQL/global/cache order. Production
 * frame handlers additionally enforce the measured startup return addresses. */
uint32_t dsound_stream_create(uint32_t descriptor,uint32_t output);
uint32_t dsound_stream_cache_i3dl2(uint32_t stream,uint32_t parameters,uint32_t apply);
uint32_t dsound_stream_cache_min_distance(uint32_t stream,uint32_t bits,uint32_t apply);
/* T1068: bounded 0x4085D9 startup/update raw host record; apply must be 1. */
uint32_t dsound_stream_cache_max_distance(uint32_t stream,uint32_t bits,uint32_t apply);
/* T1182: IDirectSoundStream_SetPosition 0x408609 (stdcall 5: stream, x, y, z, apply), the apply=1 update of a fully configured spatial
 * stream (flags 0x10, cache mask 7). The original (0x4083D7, then 0x407EA5 shared with the buffer SetPosition) stores the three raw float
 * words in the omitted nested settings object (+8, +0xC, +0x10), marks it dirty (+2 bit 0x01) and, with apply=1, skips the commit helper
 * 0x406E90 and every APU write. HOST record only (`position_bits`, `position_sets`), no spatial playback. Callers: the returns 0x29ACA
 * (setup, position 0 0 0.05), 0x29E2B and 0x29FAD (updates). apply=0 enters the unmodeled commit and is refused. */
uint32_t dsound_stream_set_position(uint32_t stream,uint32_t x,uint32_t y,uint32_t z,uint32_t apply);
uint32_t dsound_stream_cache_rolloff(uint32_t stream,uint32_t curve,uint32_t count,uint32_t apply);
/* Only flags0/cache_mask0 or spatial flags16/cache_mask7, mode1 are admitted.
 * Partial spatial setup remains refused. This idempotent HOST request
 * cache does not publish original voice flags, hardware pause or completion. */
uint32_t dsound_stream_cache_pause(uint32_t stream,uint32_t mode);
/* Only empty flags0/cache0 or spatial flags16/cache7 streams after recorded
 * Pause1, time0/flags1 are admitted.
 * This is HOST request metadata, never flush completion or deferred scheduling.
 * Future packet admission MUST revisit this empty-stream scope first. */
uint32_t dsound_stream_cache_flush_ex(uint32_t stream,uint32_t time_low,
                                     uint32_t time_high,uint32_t flags);
/* Owned indirect method 0x40733B, outside the adopted public DSOUND surface.
 * Embedding router must enforce caller 0x29B72 or 0x29A5A (T602)/stdcall1 RET4 and its compiled
 * stop-marker readiness. Generated direct calls remain unconditional stops.
 * Only flags0/cache0 or spatial flags16/cache7, recorded Pause1 then empty
 * FlushEx(time0,flags1); idempotent HOST
 * request metadata only. Packet admission must revisit this empty scope. */
uint32_t dsound_stream_cache_discontinuity(uint32_t stream);
/* Spatial flags16/cache7 or fresh stereo flags0/cache0/max3/44100/block72/
 * avg49612, volume -10000 only. Requires fresh state or stereo with the exact
 * completed Pause1/FlushEx(time0,flags1)/Discontinuity sequence; partial refused.
 * HOST request cache; original settings attenuation and APU programming omitted. */
uint32_t dsound_stream_cache_volume(uint32_t stream,int32_t volume);
/* T602: IDirectSoundStream_SetFormat 0x408C2D, only a stereo XADPCM format (T1159: any rate 8000 to 48000 Hz,
 * tag 0x69, 2 channels, avg rate*72/64, block 72, 4 bits, cbSize 2, 64 samples; 32000 and 22042 measured) on a stereo startup
 * stream after its recorded volume -10000, Pause1, FlushEx and Discontinuity. HOST record
 * (`format_sets`, `format`) only: the original copies the format into the omitted settings object,
 * sets the pitch word, rewrites voice state and programs APU voice registers, none of it modelled,
 * no guest word written, nothing plays. A later call replaces the record. Production caller 0x299FA. */
uint32_t dsound_stream_cache_set_format(uint32_t stream,uint32_t format);
/* Owned indirect STD2 method4073D3; embedding router must guard caller29CEA
 * and compiled stops. Exact spatial or fresh stereo silence startup output1,
 * after recorded volume -10000 and before Pause/Flush/Discontinuity; not general status.
 * Only output DWORD changes; same-byte permission probe is itself a guest write.
 * Output must be disjoint from durable owned state; mappings stay quiescent.
 * Consumed descriptor/format/I3DL2 inputs are historical snapshots and may be reused.
 * No packet admission exists; adopting packets MUST revisit this scope. */
uint32_t dsound_stream_get_startup_status(uint32_t stream,uint32_t output);
/* True when every original stream vtable entry (the SECONDARY table the stream header points
 * at) is one of the compiled stop targets. The movie stream (T392) checks it before creating. */
bool dsound_stream_original_tables_stopped(void);
size_t dsound_stream_register(void);
typedef struct dsound_stream_snapshot {
    dsound_stream_scope scope;
    dsound_device_lease lease;
    uint32_t stream_address,stream_heap,header[10];
    uint32_t publication_address,i3dl2_address;
    uint32_t cache_mask,i3dl2[9],min_distance_bits;
    bool max_distance_seen;
    uint32_t max_distance_bits,curve_address,curve_count;
    bool pause_seen;
    uint32_t pause_mode;
    bool flush_seen;
    uint32_t flush_time_low,flush_time_high,flush_flags;
    bool discontinuity_seen;
    bool volume_seen;
    int32_t volume;
    uint32_t format_sets;
    uint32_t source_rate_hz,frequency_sets;
    int32_t frequency_pitch;
    uint8_t format[DSOUND_STREAM_FORMAT_BYTES];
    dsound_stream_routing routing;
    uint32_t position_bits[3],position_sets;
} dsound_stream_snapshot;
/* Read-only HOST observation of the passive policy, not original guest internals.
 * Validates full owned device/stream and generation. Failure preserves output. */
bool dsound_stream_get_snapshot(uint32_t stream,dsound_stream_snapshot *output);
/* Quiescent per-object shutdown/test cleanup BEFORE effects/device reset. Refuses
 * changed/tampered/stale objects and preserves foreign replacements. Successful
 * lease detachment precedes checked child cleanup outside device lock; cleanup is
 * not atomic with ref release. Earlier objects may be cleaned before later refusal.
 * Child heaps are exclusive to this adapter. External allocations/frees or
 * byte-identical block replacement within one live heap are outside the ownership
 * contract (heap generation does not identify each allocation incarnation).
 * External mapping/content/free/reset must remain quiescent; configured policy is preserved. */
bool dsound_stream_reset_checked(void);
void dsound_stream_reset(void);
#endif
