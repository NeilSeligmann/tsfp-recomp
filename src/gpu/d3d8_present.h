/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Swap (0x003D8E50) and what it runs, for the configuration the title creates: swap effect 3
 * (copy). docs/d3d8-usage.md section 14 has the measurements.
 *
 * WHAT A PRESENT IS IN THIS CONFIGURATION, MEASURED BY RUNNING THE ORIGINAL. The title presents with
 * two calls per frame, Swap(2) and Swap(4), around a full-screen quad it draws itself:
 *
 *   Swap(2)  inserts a fence, waits for the one before it, adds 1 to the swap counter at
 *            device+0x2478, SAVES the render target, depth buffer, the four texture stages and the
 *            viewport (0x003D8890), then binds the FRONT buffer as the render target and the back
 *            buffer as texture 0 (0x003D8B10). The title's quad then copies back to front.
 *   Swap(4)  RESTORES what Swap(2) saved (0x003D8920), stamps the front buffer's fence word,
 *            inserts a fence and, THE FIRST TIME ONLY (device+0x1DE0 is cleared by it), sets the display
 *            mode (0x003D8450): a wait for the vertical blank, AvSetDisplayMode with the front buffer as
 *            the scanout surface, and the TV-encoder options.
 *
 * NO SURFACE HEADERS ARE ROTATED. The rotation (0x003D8D00, which swaps the Data and Lock words of the
 * surface headers) runs only for a swap effect that is not copy, and 0x003D8D00 does not execute in
 * either call (MEASURED). So the "present" is the front buffer's contents at the end of Swap(4), and it
 * is the title's own quad draw that puts the frame there.
 *
 * THE FRAME QUEUE records, at the end of every Swap(4), what was presented: the front buffer's header
 * words, its size and format decoded, the scanout address the display was last given and the virtual
 * vblank count. No pixels are produced. The queue is bounded: past its capacity the oldest entry is
 * overwritten and the overrun counted, because a backend wants the newest frames.
 *
 * NOT PORTED, each stopping with a message when its branch is reached: a triple-buffered device and the
 * user callbacks at device+0x8D4 and +0x8D8.
 *
 * WHAT FLAG 1 COMPOSES, MEASURED ON THE ORIGINAL (T393, tests/test_d3d8_copy_composition_oracle.py) AND
 * PORTED BY T443 (d3d8_copy.h, docs/d3d8-copy-composition.md). It is D3DSWAP_COPY, the default of Swap(0):
 * after the same save and front-buffer binding as flag 2, 0x003D8370 snapshots 20 render states, 11
 * stage 0 texture states, the pixel shader binding and the pixel shader state into a frame,
 * 0x003D85F0 forces the fixed-function copy state (SetPixelShader(0), so the fixed-function dirty
 * cascade 0x003DED80 runs, d3d8_combiner.h), 0x003D8990 draws ONE immediate-mode triangle list
 * (Begin 5, three texcoord0 and position SetVertexData2f pairs, End, d3d8_immediate.h) that samples
 * the back buffer into the front buffer and 0x003D8710 restores every state and the shader. The
 * port EMITS those commands exactly as the original writes them, so the recorded stream carries the
 * copy and the replay can draw it. The NET guest state difference against flag 2 is only the dirty mask
 * and the four stage control words at device+0x774. The commands of 0x003D8B10's own body stay elided.
 */

#ifndef TSFP_GPU_D3D8_PRESENT_H
#define TSFP_GPU_D3D8_PRESENT_H

#include <stddef.h>
#include <stdint.h>

#define D3D8_FRAME_QUEUE_CAPACITY 256u

typedef struct {
    uint64_t number;        /* 1 for the first present */
    uint32_t swap_counter;  /* device+0x2478 after the present */
    uint32_t interval;      /* the presentation interval, from 0x003E3EBC */
    uint64_t vblank;        /* the virtual vblank count at the present */
    uint32_t header;        /* the front buffer's header address */
    uint32_t common;        /* its six header dwords: Common, Data, Lock, Format, Size, Parent */
    uint32_t data;          /* the physical address of the pixels */
    uint32_t lock;
    uint32_t format_word;
    uint32_t size_word;
    uint32_t parent;
    uint32_t width;         /* decoded from the Size word */
    uint32_t height;
    uint32_t pitch;
    uint32_t format;        /* the hardware format number, from the Format word */
    uint32_t scanout;       /* the frame buffer of the last AvSetDisplayMode, 0 before the first */
    uint64_t stream_commands; /* commands recorded by the GPU model so far */
} d3d8_frame_record;

/** T422. Called with each record just queued at the end of Swap(4). Default NULL (nothing happens).
 * Configuration like the GPU's observers, not cleared by `d3d8_present_reset`. The frame profile
 * installs it (src/gpu/d3d8_frame_profile.c), which keeps this file free of the decoder stack. */
typedef void (*d3d8_present_observer)(const d3d8_frame_record *record);
void d3d8_present_set_observer(d3d8_present_observer observer);
/** T838: a SECOND observer slot, independent of the first (the frame profile owns the first), called right after it with the same
 * record. The M9 live renderer's `live_vk_target_present_observer` takes it, so both run. Default NULL, not cleared by
 * `d3d8_present_reset`. */
void d3d8_present_set_second_observer(d3d8_present_observer observer);
/* T1633: a third observer for the host's route probe (frame change signature), independent of the two above. */
void d3d8_present_set_route_observer(d3d8_present_observer observer);

/** 0x003D8E50: present. Returns the swap counter at device+0x2478, as the original does. */
uint32_t d3d8_swap(uint32_t flags);

/** 0x003D8B10: bind the front buffer for the copy (flags & 3) and restore (flags & 4). */
void d3d8_present_prepare(uint32_t flags);

/** 0x003D8890 and 0x003D8920: save and restore the bindings and the viewport. */
void d3d8_present_save_state(void);
void d3d8_present_restore_state(void);

/** 0x003D8E10: stamp the front buffer, insert a fence and run the mode set when it is pending. */
void d3d8_present_finish(void);

/** 0x003D8450: the mode set. Not for the title to call. */
void d3d8_present_set_mode(void);

/** The queue, oldest retained first. `count` is how many are held, `total` how many were presented. */
size_t d3d8_frame_queue_count(void);
uint64_t d3d8_frame_queue_total(void);
uint64_t d3d8_frame_queue_overruns(void);
d3d8_frame_record d3d8_frame_queue_at(size_t index);

/** Forget the queue. For tests. */
void d3d8_present_reset(void);

/** Register this file's handlers; returns how many. */
size_t d3d8_present_register(void);

#endif /* TSFP_GPU_D3D8_PRESENT_H */
