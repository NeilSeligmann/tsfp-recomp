/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T407: the flip queue and flip processing of the original D3D library, as device state.
 *
 *   0x003DC5E0  the PGRAPH software method handler. Its type 1 (the flip, a NOP method 0x100 whose
 *               data is `(address | interval bits) << 5 | 1`) fills the producer slot, bumps the
 *               producer index and runs the flip processor. The other twelve types are named
 *               refusals here.
 *   0x003DC200  the flip processor. Run from 0x003DC5E0 and from the vblank helper 0x003DC2E0.
 *
 * Every original behaviour here was measured under the Unicorn oracle over the retail bytes
 * (tools/vblank_helper_oracle.py, tests/test_d3d8_flip_oracle.py) and is compared dword for dword.
 * Context offsets are relative to the display object, which is device + 0x1C28.
 *
 * WHAT THE ORIGINAL DOES AT A FLIP AND WHERE IT LANDS HERE
 *   guest memory   the consumer slot (pending word cleared), the consumer index (+0x1BC), the global
 *                  0x003E6408 (the address the last flip showed), the gamma pending flag of the slot
 *                  (+0x7DC and +0x7E0). The producer adds the slot (pending, target count, address),
 *                  the producer index (+0x1CC), the last target (+0x1C8) and the threshold (+0x1C4).
 *                  All of these are ordinary guest words and are written exactly.
 *   hardware       three effects the original makes through the NV2A register window, which this
 *                  port does not have: the display start register (PCRTC 0x600800, written unless
 *                  +0x1B8 is set, as after CreateDevice), the 768 byte gamma ramp upload (PRAMDAC
 *                  0x6813C9, only for a slot whose pending flag is 1), and the flip read pointer
 *                  increment (PGRAPH 0x40071C |= 2). There is NO REGISTER FILE: each is held as a
 *                  record in d3d8_flip_hardware (the value the register would have been given, the
 *                  ramp bytes in upload order, an ordered event log) so the next stage (scanout, a
 *                  backend) reads one place, and nothing here changes what the title can observe.
 *
 * WHAT IS NOT MODELLED (named and refused where reachable, never guessed)
 *   - The flip callback at +0x18C. The original calls it with a record carrying a timestamp delta
 *     it computes from rdtsc (+0x1D4, +0x1D8). With the callback unset, that read has no effect,
 *     so the delta needs no model. A set callback is a refusal that writes nothing.
 *   - The hardware stall. FLIP_STALL (method 0x130) keeps the GPU from running the third queued
 *     flip until the first has been processed. The instantaneous GPU has no stall, so a queue
 *     whose producer slot is still pending is a refusal that writes nothing.
 *   - The Swap's own commands for the flip (WAIT_FOR_IDLE 0x110, the software NOP 0x100,
 *     FLIP_INCREMENT_WRITE 0x12C, a NOP and FLIP_STALL 0x130) stay out of the recorded stream, as
 *     the commands of 0x003D8B10's body always did. The replay would see four methods it has
 *     no use for and the digest of every stream would move for nothing (docs/d3d8-flip-queue.md).
 *   - The ISR 0x003DC0E0, which also programs the display start from 0x003E6408.
 */
#ifndef TSFP_GPU_D3D8_FLIP_H
#define TSFP_GPU_D3D8_FLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Device-relative fields (display object + 0x1C28). MEASURED, see tools/vblank_helper_oracle.py. */
#define D3D8_FLIP_DEV_PITCH 0x1C2Cu
#define D3D8_FLIP_DEV_MODE_WORD 0x1C30u     /* context + 8 */
#define D3D8_FLIP_DEV_SLOT0 0x1D9Cu         /* pending, target count, address: three dwords a slot */
#define D3D8_FLIP_DEV_SLOT1 0x1DA8u
#define D3D8_FLIP_SLOT_BYTES 12u
#define D3D8_FLIP_DEV_CALLBACK 0x1DB4u      /* context + 0x18C */
#define D3D8_FLIP_DEV_DISPLAY_MODE 0x1DDCu  /* context + 0x1B4 */
#define D3D8_FLIP_DEV_DISPLAY_START_OFF 0x1DE0u /* context + 0x1B8: nonzero skips the display start */
#define D3D8_FLIP_DEV_CONSUMER 0x1DE4u      /* context + 0x1BC */
#define D3D8_FLIP_DEV_COUNT 0x1DE8u         /* context + 0x1C0 */
#define D3D8_FLIP_DEV_THRESHOLD 0x1DECu     /* context + 0x1C4 */
#define D3D8_FLIP_DEV_LAST_TARGET 0x1DF0u   /* context + 0x1C8 */
#define D3D8_FLIP_DEV_PRODUCER 0x1DF4u      /* context + 0x1CC */
#define D3D8_FLIP_DEV_FIELD_STATUS 0x1DF8u  /* context + 0x1D0 */
#define D3D8_FLIP_DEV_GAMMA_RAMPS 0x1E04u   /* context + 0x1DC: two ramps of 0x300 bytes */
#define D3D8_FLIP_DEV_GAMMA_PENDING 0x2404u /* context + 0x7DC: one dword per ramp */
#define D3D8_FLIP_GAMMA_RAMP_BYTES 0x300u
#define D3D8_FLIP_GAMMA_CHANNEL_BYTES 0x100u

#define D3D8_FLIP_GLOBAL_VALUE 0x003E6408u  /* the address the last processed flip showed */

/* The flip's method data: low five bits the type, the rest `(address | interval bits)`. */
#define D3D8_FLIP_METHOD_TYPE_FLIP 1u
#define D3D8_FLIP_METHOD_TYPES 14u
#define D3D8_FLIP_INTERVAL_IMMEDIATE_BIT 8u

typedef enum d3d8_flip_hw_kind {
    D3D8_FLIP_HW_DISPLAY_START = 1, /* value: what the original wrote to PCRTC 0x600800 */
    D3D8_FLIP_HW_GAMMA_RAMP = 2,    /* value: the ramp index, bytes in d3d8_flip_hardware.gamma */
    D3D8_FLIP_HW_PGRAPH_INCREMENT = 3 /* value: the bits ORed into PGRAPH 0x40071C (2) */
} d3d8_flip_hw_kind;

typedef struct d3d8_flip_hw_event {
    uint32_t kind;
    uint32_t value;
} d3d8_flip_hw_event;

#define D3D8_FLIP_HW_EVENTS 64u

/* What the flips programmed, since the device was created. A copy, safe from any thread. */
typedef struct d3d8_flip_hardware {
    uint64_t reset_serial;           /* host snapshot lifetime, not a guest/hardware value */
    uint64_t flips;                  /* flips processed */
    uint64_t queued;                 /* flips queued by 0x003DC5E0 */
    uint64_t display_start_writes;
    uint32_t display_start;          /* the last value programmed, valid when writes > 0 */
    uint64_t gamma_uploads;
    uint8_t gamma[D3D8_FLIP_GAMMA_RAMP_BYTES]; /* the last upload, red, green and blue interleaved */
    uint64_t pgraph_increments;
    size_t event_count;              /* recorded, at most D3D8_FLIP_HW_EVENTS, oldest first */
    uint64_t events_dropped;
    d3d8_flip_hw_event events[D3D8_FLIP_HW_EVENTS];
} d3d8_flip_hardware;

/* The one lock of the device-state effects: the vblank helper and the producer both read and write
 * count, threshold and the flip words, from different threads. Not recursive. The *_locked
 * functions need it held. A refusal is raised after releasing it. */
void d3d8_flip_lock(void);
void d3d8_flip_unlock(void);

/** 0x003DC200 over the device state. Returns the number of flips processed (the original's eax).
 * Needs the lock. Never refuses: every effect is a guest word or a d3d8_flip_hardware record. */
unsigned d3d8_flip_process_locked(void);

/**
 * Why a flip method cannot be queued now, or NULL. Nothing is written by the check. Needs the
 * lock. The reasons: a type other than the flip (0x003DC5E0's other jump table entries), a flip
 * callback set (+0x18C), a producer slot still pending (the GPU stall is not modelled).
 */
const char *d3d8_flip_queue_refusal_locked(uint32_t method_data);

/** 0x003DC5E0 type 1 followed by 0x003DC200. Needs the lock and `d3d8_flip_queue_refusal_locked`
 * returning NULL for the same data. Returns the number of flips the processor ran. */
unsigned d3d8_flip_queue_locked(uint32_t method_data);

/** The data word 0x003D8B10 writes with the flip's NOP (0x40100): the front buffer's data
 * address, the interval bits from 0x003E3EBC (0 reads as 1, bit 0 ORs 1, bit 1 ORs 2, bit 2
 * ORs 3, the sign bit ORs 8), shifted left five, type 1. Pure. */
uint32_t d3d8_flip_swap_method_data(uint32_t front_data_address, uint32_t present_interval);

/**
 * The Swap's flip (0x003D8B10 for a device with fewer than three back buffers): build the data word
 * from the front buffer and the present interval and queue it. DOES NOTHING unless BOTH the flip
 * model (--model-flips) and the device-state effects (--couple-vblank-effects) are enabled: the
 * flip is only ever completed by the vblank helper the effects run, and a default boot must not
 * queue what nothing would process. The flip model is its own opt-in because a queued flip changes
 * state the T189/T220 callback preflights pin as the measured empty state (flip index, threshold,
 * slots), so `--couple-vblank-effects` boots stay byte identical without it. Fatal, with nothing
 * written, for a refusal. Announced as not modelled where it leaves hardware out.
 */
void d3d8_flip_queue_for_swap(void);

/** Enable the flip model for Swap (default off). Needs the vblank effects to do anything. */
void d3d8_flip_configure(bool enabled);
bool d3d8_flip_enabled(void);

/** The hardware record and its reset. d3d8_flip_reset runs when the device is created (the GPU
 * reset count changes) and when tests ask. */
d3d8_flip_hardware d3d8_flip_hardware_get(void);
void d3d8_flip_reset(void);

#endif
