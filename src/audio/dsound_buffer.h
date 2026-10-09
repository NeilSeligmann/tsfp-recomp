/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_BUFFER_H
#define TSFP_AUDIO_DSOUND_BUFFER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dsound_device.h"
#include "dsound_buffer_scope.h"
/* Default-off startup-only passive36-byte buffer policy. Original vtable/ref1/
 * parent occupy first3words; six omitted CPU children/list words are zero. Public
 * handle is allocation+28. No original hardware/settings/DSP/FP or completion
 * behavior is implemented. Original tables/mappings/content must remain quiescent.
 * Stop marker and unordered original table membership are safety prerequisites,
 * not original body/table equivalence. */
bool dsound_buffer_stops_ready(void);
void dsound_buffer_set_enabled(bool enabled);
typedef bool (*dsound_buffer_irql_provider)(uint8_t *out);
typedef void (*dsound_buffer_fatal_fn)(uint32_t address,const char *reason);
void dsound_buffer_set_irql_provider(dsound_buffer_irql_provider provider);
void dsound_buffer_set_fatal(dsound_buffer_fatal_fn fatal);
/* T681: the opt-in passive completion model. With it on, the sound update SetVolume takes any value (after the
 * whole start), SetFrequency takes the title's clamp 0xBC..0x2EDEF, and Pause or SetFrequency of a buffer
 * `started` (Play was called) says yes to is refused by name. Off (default) changes nothing.
 * T733: with it on, SetBufferData, SetVolume, Pause and SetFrequency are gated on the state the model checks (the order of the
 * recorded start, a played buffer's hooks), NOT on a measured call site: the originals never read their return address, so a
 * site is not a property of the method. SetVolume takes any value after SetBufferData. Create and the three startup caches keep
 * their measured callers, and with the model off every method keeps them. */
/* T733: keyed by the buffer address AND its lease serial (a recycled address with a new lease was never played). */
typedef bool (*dsound_buffer_started_fn)(uint32_t buffer,uint64_t serial);
/* T722: Pause of a PLAYED buffer (mode 0, 1 or 2) is handed to the completion model, which answers whether it admits it. */
typedef bool (*dsound_buffer_pause_fn)(uint32_t buffer,uint64_t serial,uint32_t mode);
/* T722: SetFrequency of a PLAYED buffer is handed to the completion model (it keeps the samples left, not the ticks). */
typedef bool (*dsound_buffer_frequency_fn)(uint32_t buffer,uint64_t serial,uint32_t old_hertz,uint32_t new_hertz);
/* Real ordinary-buffer PCM volume notification; called with buffer ownership
 * verified and buffer lock held, so the hook must not reenter buffer APIs. */
typedef bool (*dsound_buffer_volume_fn)(uint32_t buffer,uint64_t serial,int32_t volume);
void dsound_buffer_set_completion(bool enabled,dsound_buffer_started_fn started);
void dsound_buffer_set_completion_frequency(dsound_buffer_frequency_fn frequency);
/* T1209: true while the completion model's voice of the buffer is still running (played, not stopped). SetLoopRegion on such a voice
 * reprograms the APU voice (measured), which the model does not do, so a missing hook counts as running. */
typedef bool (*dsound_buffer_voice_running_fn)(uint32_t buffer,uint64_t serial);
void dsound_buffer_set_completion_voice_running(dsound_buffer_voice_running_fn running);
/* T1524: SetLoopRegion of a RUNNING voice is handed to the completion model (bytes of the encoded data, already validated:
 * 36-byte aligned, nonempty, inside the data; frequency is the recorded SetFrequency). Without the hook a running voice is refused as
 * before. Called with the buffer lock held, must not reenter buffer APIs. */
typedef bool (*dsound_buffer_loop_fn)(uint32_t buffer,uint64_t serial,uint32_t loop_start,uint32_t loop_length,uint32_t frequency);
void dsound_buffer_set_completion_loop(dsound_buffer_loop_fn loop);
void dsound_buffer_set_completion_pause(dsound_buffer_pause_fn pause);
void dsound_buffer_set_completion_volume(dsound_buffer_volume_fn volume);
/* Typed policy helpers enforce scope/ownership/IRQL/global/cache order. Production
 * frame handlers additionally enforce the five measured startup return addresses.
 * Create supports only aligned slots in the two measured40-entry output pools,
 * matched to flags16/0. Output permission probing writes the same original word;
 * final publication requires quiescent mappings/content. No compound atomic
 * guest-memory claim is made. */
uint32_t dsound_buffer_create(uint32_t device,uint32_t descriptor,uint32_t output,uint32_t outer);
uint32_t dsound_buffer_cache_i3dl2(uint32_t buffer,uint32_t parameters,uint32_t apply);
uint32_t dsound_buffer_cache_min_distance(uint32_t buffer,uint32_t bits,uint32_t apply);
uint32_t dsound_buffer_cache_rolloff(uint32_t buffer,uint32_t curve,uint32_t count,uint32_t apply);
/* T1068: bounded SetMaxDistance 0x40850E host record. Only the measured title
 * caller on a fully configured spatial buffer with apply=1 is admitted. */
uint32_t dsound_buffer_cache_max_distance(uint32_t buffer,uint32_t bits,uint32_t apply);
/* T1068: bounded SetPosition 0x408556 raw xyz host record. Only its three
 * measured title return sites on a configured spatial buffer with apply=1 are
 * admitted; apply=0 enters the omitted settings/APU commit chain. */
uint32_t dsound_buffer_set_position(uint32_t buffer,uint32_t x_bits,uint32_t y_bits,
                                    uint32_t z_bits,uint32_t apply);
/* T597: IDirectSoundBuffer_SetBufferData 0x408C0D, (this, data, length) stdcall RET 0xC. HOST record of
 * pointer and length only, S_OK, no playback, no guest write (the original writes only omitted
 * settings/hardware objects, takes the critical section, locks pages, programs APU 0xFE820804/808).
 * Admitted for the measured startup buffers (flags 0x10 with the three spatial caches, or flags 0
 * with none), non-null data, non-zero length up to 64 MiB, readable guest memory, no alias with
 * owned state. Anything else is refused by name before any record (the original self-allocates
 * for a null pointer and clears for 0/0). A later call replaces the record (nothing plays). */
uint32_t dsound_buffer_set_data(uint32_t buffer,uint32_t data,uint32_t length);
/* T601: IDirectSoundBuffer_SetVolume 0x407A64, (this, volume) stdcall RET 8. HOST record of the volume
 * only, S_OK, no playback, no guest write (the original stores volume minus its +0x20 word in the omitted
 * settings object and programs the voice's APU volume registers, details in docs/audio-input-recovery.md).
 * Admitted for the measured startup buffers (flags 0x10 with the three spatial caches, or flags 0 with none)
 * AFTER a recorded SetBufferData and only for -10000 (0xFFFFD8F0, the title's sound start value, caller 0x28348).
 * T605: also -3204 (0xFFFFF37C, the sound update sub_00028610, caller 0x28643) on a flags 0 buffer whose start
 * (SetBufferData, SetVolume, Pause, SetFrequency) is recorded, each caller with its own volume. Anything
 * else is refused by name before any record. A repeat replaces the record (nothing plays). */
uint32_t dsound_buffer_set_volume(uint32_t buffer,int32_t volume);
/* T601: IDirectSoundBuffer_Pause 0x407ABC, (this, mode) stdcall RET 8, the title's resume of a sound it has
 * just set up (caller 0x2751E, mode 0). The original on a buffer nothing ever started (voice state bits
 * 0x101, low two bits not 3) changes no guest or APU word and returns the mode, so Pause(0) is S_OK with
 * no effect. HOST count only (`pause_sets`). Same startup scopes and order as SetVolume (after a recorded
 * SetBufferData and SetVolume). Any other mode (the original returns the mode itself as the HRESULT) or a
 * started buffer is refused by name. */
uint32_t dsound_buffer_pause(uint32_t buffer,uint32_t mode);
/* T601: IDirectSoundBuffer_SetFrequency 0x4084F2, (this, hertz) stdcall RET 8, called by the title after
 * Pause(0) (caller 0x27547, the sound's rate clamped to 0xBC..0x2EDEF). HOST record of the frequency only,
 * S_OK, no playback, no guest write (the original stores the derived pitch at +0x18 of the omitted settings
 * object and writes the voice's APU pitch register 0xFE82037C). Admitted only for the measured 22042 (0x561A)
 * of the first sound, after a recorded SetBufferData, SetVolume and Pause(0). Anything else is refused by
 * name. A repeat replaces the record. */
uint32_t dsound_buffer_set_frequency(uint32_t buffer,uint32_t hertz);
/* T1068: original SetLoopRegion407AD8/stdcall3 RET12. With explicit passive
 * completion, record a nonempty block-aligned region on an ordinary owned
 * buffer before any Play. Count0 means data_length-start. Explicit nonwrapping
 * out-of-bounds counts return original88780032; wrap/empty/unaligned cases and
 * changes after Play are named policy refusals. No guest settings/APU write.
 * Play uses this region for its INFERRED cursor; Stop drains the full data tail. */
uint32_t dsound_buffer_set_loop_region(uint32_t buffer,uint32_t start,uint32_t length);
/* Registers CreateSoundBuffer and the three setters, plus SetBufferData, SetVolume, Pause and SetFrequency
 * ONLY while the policy is enabled at registration time, so a flags-off boot keeps its exact registry.
 * SetLoopRegion additionally requires the explicit completion model. */
size_t dsound_buffer_register(void);
typedef struct dsound_buffer_snapshot {
    dsound_buffer_scope scope;
    dsound_device_lease lease;
    uint32_t buffer_address,header_address,buffer_heap,header[9];
    uint32_t publication_address,i3dl2_address;
    uint32_t cache_mask,i3dl2[9],min_distance_bits,curve_address,curve_count;
    uint32_t data_address,data_length,data_sets;
    uint32_t loop_start,loop_length,loop_sets;
    int32_t volume;
    uint32_t volume_sets,pause_sets,frequency,frequency_sets;
    uint32_t max_distance_bits,max_distance_sets;
    uint32_t min_distance_sets,rolloff_sets;
    uint32_t position_bits[3],position_sets;
} dsound_buffer_snapshot;
/* Read-only HOST observation of the passive policy, not original guest internals.
 * Validates full owned device/buffer and generation. Failure preserves output. */
bool dsound_buffer_get_snapshot(uint32_t buffer,dsound_buffer_snapshot *output);
/* Quiescent per-object shutdown/test cleanup BEFORE streams/effects/device reset. Refuses
 * changed/tampered/stale objects and preserves foreign replacements. Successful
 * lease detachment precedes checked child cleanup outside device lock; cleanup is
 * not atomic with ref release. Earlier objects may be cleaned before later refusal.
 * Failed unpublished allocation cleanup stays on an unleased retry list and
 * never releases a parent reference. Child heaps are exclusive to this adapter. External allocations/frees or
 * byte-identical block replacement within one live heap are outside the ownership
 * contract (heap generation does not identify each allocation incarnation).
 * External mapping/content/free/reset must remain quiescent; configured policy is preserved. */
bool dsound_buffer_reset_checked(void);
void dsound_buffer_reset(void);
#endif
