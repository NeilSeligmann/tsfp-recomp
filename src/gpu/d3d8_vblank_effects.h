/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T372: the device-state half of the original vblank helper 0x003DC2E0, coupled to completed
 * waits. DEFAULT OFF (--couple-vblank-effects). See docs/vblank-delivery.md "T372 results" for
 * the oracle measurements behind every line here (tools/vblank_helper_oracle.py).
 *
 * The helper runs once per vertical blank, from the DPC. The wait model (d3d8_gpu_wait_vblank)
 * already stands in for its KeSetEvent and for one clock period. With this module enabled each
 * COMPLETED wait also runs the helper's guest-memory effects that are derivable:
 *
 *   timestamps  +0x1E00 = low 32 bits of the vblank's virtual time (the clock floor after the
 *               frame, never a wall time), +0x1DFC = the 32-bit difference from the previous
 *               one, written only when a previous one exists (0x003DC0D0 is rdtsc, 0x003DC2E0
 *               stores both).
 *   count       +0x1DE8 += 1.
 *   threshold   +0x1DEC += 1 when the new count equals it and no flip was processed.
 *   flip processing 0x003DC200 (T407, src/gpu/d3d8_flip.c): the consumer slot, the consumer index,
 *               the global 0x003E6408 and the gamma pending flag are guest words and are written
 *               exactly. The display start, the gamma ramp upload and the PGRAPH increment the
 *               original makes through the register window are held as records in
 *               d3d8_flip_hardware (no register file). A processed flip gives flags 1.
 *
 * REFUSED, as a named stop and with nothing written:
 *   field status +0x1DF8. The helper reads port 0x80C0 into it when the display mode word
 *               +0x1DDC has bit 0x1000000 clear. That is hardware input. The only reader is
 *               GetDisplayFieldStatus (0x003D4350), which looks at it only when mode bits
 *               0x1200000 are set, so the value is unobservable for the measured mode 0x02480104
 *               and a mode with 0x200000 set and 0x1000000 clear is refused.
 * NOT MODELLED (no guest-visible effect, no hardware state to hold): the PCRTC interrupt
 * acknowledge write [reg+0x600100] and the save/restore of [reg+0x6013D4]. Not part of this
 * module: the callback call (T189/T220 deliver it at frame entry, T369 will generalize).
 */
#ifndef TSFP_GPU_D3D8_VBLANK_EFFECTS_H
#define TSFP_GPU_D3D8_VBLANK_EFFECTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Device-relative fields (context + 0x1C28), MEASURED under the oracle. */
#define D3D8_VBLANK_DEV_CALLBACK 0x1DB8u
#define D3D8_VBLANK_DEV_MODE 0x1DDCu
#define D3D8_VBLANK_DEV_FLIP_INDEX 0x1DE4u
#define D3D8_VBLANK_DEV_COUNT 0x1DE8u
#define D3D8_VBLANK_DEV_THRESHOLD 0x1DECu
#define D3D8_VBLANK_DEV_FIELD_STATUS 0x1DF8u
#define D3D8_VBLANK_DEV_TIMESTAMP_DELTA 0x1DFCu
#define D3D8_VBLANK_DEV_TIMESTAMP_LAST 0x1E00u
#define D3D8_VBLANK_DEV_FLIP_SLOT0 0x1D9Cu
#define D3D8_VBLANK_DEV_FLIP_SLOT1 0x1DA8u

/* What the original passes its callback by pointer, in order: the helper's own record. */
typedef struct d3d8_vblank_record {
    uint32_t count;
    uint32_t flip_index;
    uint32_t flags; /* 0 none, 1 a flip was processed, 2 the threshold was hit */
} d3d8_vblank_record;

/* T513: the flip related device words a helper run STARTED from, in the order of the fields below,
 * so a trace can seed the original helper under the oracle with the same state (the measurement
 * of the flip model against 0x003DC2E0, tests/test_vblank_owner_waits_flips.py). */
typedef struct d3d8_vblank_input {
    uint32_t callback, mode, display_start_off, consumer, count, threshold, last_target, producer;
    uint32_t slot[6]; /* pending, target, address, twice */
    uint32_t gamma_pending[2];
} d3d8_vblank_input;

/* Quiescent configuration. Enabling resets the applied counter. */
void d3d8_vblank_effects_configure(bool enabled);
bool d3d8_vblank_effects_enabled(void);
/* Count of helper runs applied since configuration, safe from any thread. */
uint64_t d3d8_vblank_effects_applied(void);
/* T513: the record the latest helper run built (and would have passed its callback), all zero before
 * the first run after configuration. The two-event and owner-wait callbacks under the flip model
 * deliver exactly this record, so a callback is never given flags the helper did not compute. */
d3d8_vblank_record d3d8_vblank_effects_last_record(void);
/* T513: the device words the latest helper run started from, all zero before the first. */
d3d8_vblank_input d3d8_vblank_effects_last_input(void);
/* T513: the schedule trace suffix of one completed wait, EMPTY unless the flip model and these effects
 * are both on (so every default trace stays byte identical): ` flip-in=<16 words of
 * d3d8_vblank_input in field order> flip-out=<count,consumer index,threshold,flags>` where the out
 * words are the record the helper built and the threshold it left. Seeds the original helper under
 * the oracle with the same words (tests/test_vblank_owner_waits_flips.py). */
void d3d8_vblank_effects_trace(char *out, size_t size);

/* One helper run over the device state. `timestamp_low` is the low 32 bits of the vblank's
 * virtual time. Fatal (nothing written) for a refused state. Independent of the enabled flag,
 * which gates only the caller (d3d8_gpu_wait_vblank). Returns the callback record. */
d3d8_vblank_record d3d8_vblank_effects_apply(uint32_t timestamp_low);

#endif
