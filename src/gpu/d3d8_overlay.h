/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The overlay entry points of the statically linked D3D8 library (T393 part B), ported from the
 * original machine code. The title's XMV movie function 0x30280 calls all three.
 *
 *   0x003D9990  stdcall ret 4, EnableOverlay. The title passes 1 at the start and 0 at the end of
 *               a movie and the original executes the same code for both, it never reads the
 *               argument (MEASURED). Returns the register window base in eax.
 *   0x003D9810  stdcall ret 20, UpdateOverlay(surface, source rect*, destination rect*, color key
 *               enable, color key). Returns the register 0x8140 value it stored in eax.
 *   0x003D97F0  bare return, GetOverlayUpdateStatus, no arguments. Returns the comparison
 *               device[+0x2410] != device[+0x1DE8] as 0 or 1. The generated ABI table has no row
 *               for it (one call site and a bare ret, `python -m tools.xdk_abi --verbose` says refused).
 *
 * WHAT THESE FUNCTIONS TOUCH. The device from the global 0x3E3F58 and a register window whose
 * base is the dword at device+0x934 (0xFD000000, a literal CreateDevice stores). The original
 * writes dwords at fixed offsets from that base. This host maps no such window and the port never
 * dereferences it. Every register access goes to a bounded native shadow instead (offsets 0x8100
 * to 0x8B00), written in the order and with the values the original writes. The original touches
 * exactly these registers, no others: 0x8100, 0x8140, 0x8700, 0x8704, 0x8900, 0x8908, 0x8910,
 * 0x8918, 0x8920, 0x8928, 0x8930, 0x8938, 0x8940, 0x8948, 0x8950, 0x8958 and 0x8B00.
 *
 * NO SCANOUT AND NO LIVE PRESENTATION ARE PRODUCED OR CLAIMED. The shadow and the last
 * UpdateOverlay descriptor exist so a presenter can read what the title asked for. Labels
 * in the descriptor name the arithmetic that produced each value. They are not hardware names
 * because the binary carries none. The one picture output is the opt-in image dump (T537,
 * d3d8_overlay_set_dump), a pure observer that writes what each UpdateOverlay asked to show into
 * a directory and changes nothing the title or this port can see.
 *
 * THE ONE HARDWARE-DEPENDENT BEHAVIOUR. UpdateOverlay stores 1 to register 0x8700 and only the
 * display hardware clears it. EnableOverlay spins while that register is non-zero after writing 1
 * to register 0x8704. With 0x8700 clear (no buffer pending, the title's first call) it runs
 * faithfully with no spin. With it set this port REFUSES through d3d8_hle_fatal and writes
 * nothing, because ending the wait needs either a scanout model or an announced policy for when a
 * started buffer clears, and faking the clear would make the trace fiction. A future scanout model
 * retires a buffer with d3d8_overlay_hardware_write(0x8700, 0).
 *
 * ALSO REFUSED, before any write, each an input the original would act on and this port cannot
 * model: a register window base other than 0xFD000000, a null or unreadable rectangle pointer (the
 * original dereferences both and would fault) and a surface header that cannot be read.
 */

#ifndef TSFP_GPU_D3D8_OVERLAY_H
#define TSFP_GPU_D3D8_OVERLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D3D8_OVERLAY_ENABLE 0x003D9990u
#define D3D8_OVERLAY_UPDATE 0x003D9810u
#define D3D8_OVERLAY_STATUS 0x003D97F0u

/* The shadowed register range, first and last dword offset. */
#define D3D8_OVERLAY_FIRST_REGISTER 0x8100u
#define D3D8_OVERLAY_LAST_REGISTER 0x8B00u
#define D3D8_OVERLAY_WRITE_LOG_CAPACITY 64u

/* One register write, in program order. */
typedef struct {
    uint32_t offset;
    uint32_t value;
} d3d8_overlay_write;

/* What the last UpdateOverlay asked for. Valid when `updates` is non-zero. */
typedef struct {
    uint64_t updates;            /* UpdateOverlay calls that ran */
    uint64_t enables;            /* EnableOverlay calls that ran */
    uint64_t write_count;        /* register writes the title made, all calls */
    uint32_t surface;            /* the surface header address, also stored at device+0x1C1C */
    uint32_t counter;            /* the field counter copied to device+0x2410 */
    uint32_t source[4];          /* left, top, right, bottom as the title gave them */
    uint32_t destination[4];     /* left, top, right, bottom as the title gave them */
    uint32_t color_key_enable;   /* the argument as given */
    uint32_t color_key;
    uint32_t surface_data;       /* the Data word of the header */
    uint32_t surface_pitch;      /* from 0x003D3640 */
    uint32_t source_start;       /* register 0x8920, the start offset rounded down to 64 */
    uint32_t source_phase;       /* register 0x8930, (start offset & 63) << 3 */
    uint32_t source_size_phase;  /* register 0x8928, (height << 16 | width) + (phase >> 4) */
    uint32_t horizontal_step;    /* register 0x8938, 0x100000 when the destination is 1 wide */
    uint32_t vertical_step;      /* register 0x8940, 0x100000 when the destination is 1 high */
    uint32_t destination_origin; /* register 0x8948, top << 16 | left */
    uint32_t destination_size;   /* register 0x8950, height << 16 | width */
    uint32_t control;            /* register 0x8958, pitch | 0x10000 | key enable 0x100000 */
    uint32_t result;             /* eax the original returns, the register 0x8140 value stored */
} d3d8_overlay_descriptor;

/**
 * Opt in (default off, T540): a completed modeled vblank consumes a started buffer by clearing
 * register 0x8700 as a hardware side effect. This is an inferred one-vblank latency, not measured
 * NV2A scanout timing. If EnableOverlay is reached while a buffer is still pending, it refuses
 * before writes because the synchronous hardware wait is not modeled. The policy survives reset.
 */
void d3d8_overlay_set_consume_policy(bool enabled);

/** Retire a pending buffer at one completed modeled vblank when the policy is enabled. */
void d3d8_overlay_consume_vblank(void);

/** Resolve a surface header Data word to the guest virtual address of its allocation, 0 if none. */
typedef uint32_t (*d3d8_overlay_data_resolver)(uint32_t data);

/**
 * Opt in (default off, T537): at every UpdateOverlay write the picture it asks for into
 * `directory` (it must exist). Per update `overlay_NNNNN.png` (the source rectangle converted to RGB
 * and scaled to the destination rectangle by the steps the title wrote, see d3d8_overlay_image.h
 * for the colour matrix, UNMEASURED for the overlay hardware), `overlay_NNNNN.yuv422p` (the raw Y,
 * U and V planes of the source rectangle, no matrix applied) and one line in `overlay_index.txt`.
 * `maximum` stops the files after that many updates (0 is no limit). `resolver` maps the header's
 * Data word to guest memory. A NULL directory switches it off. A PURE OBSERVER: it only reads
 * guest memory, never refuses, never touches the shadow, the descriptor or any counter the title
 * can reach. A picture it cannot write is reported to stderr and counted, never silent. The
 * buffers are allocated here, before the guest runs. False when they cannot be allocated. The
 * setting survives d3d8_overlay_reset, the counters do not.
 */
bool d3d8_overlay_set_dump(const char *directory, uint32_t maximum,
                           d3d8_overlay_data_resolver resolver);

/**
 * Opt in (default off, T831, XEMU-LEVEL, d3d8_overlay_image.h): the dump and the present hook build the picture with
 * the xemu matrix and bilinear scale instead of the XMV library matrix and nearest scale. The dump file is then the
 * xemu box, one pixel wider and taller than the destination rectangle, and the present hook still gets the declared
 * destination size (the top left of the box). The raw planes are unchanged. Survives d3d8_overlay_reset.
 */
void d3d8_overlay_set_xemu_image(bool enabled);
bool d3d8_overlay_xemu_image(void);

/**
 * Opt in (default off, T831, XEMU-LEVEL, d3d8_overlay_key.h): every UpdateOverlay also latches its picture, origin,
 * control word and key as the layer that d3d8_overlay_key_displayed composes over the swap replay's frame. Builds
 * pictures like the dump does, so `resolver` maps the header's Data word (used when no dump or present hook
 * supplies one). False when the buffers cannot be allocated. A pure observer of guest memory. False enabled switches
 * it off.
 */
bool d3d8_overlay_set_key_composition(bool enabled, d3d8_overlay_data_resolver resolver);

/** One UpdateOverlay picture for the present hook (T760). `rgb` is width * height * 3 bytes, the
 * destination rectangle after the dump's conversion and scaling (matrix UNMEASURED, see above),
 * valid only during the call. `rgb` is NULL and width and height are 0 when the picture could not be
 * built (the reason went to stderr and into the dump_failed count): the update still counts. */
typedef struct {
    uint64_t number; /* update index, 0 based, equals the dump file number */
    uint32_t width;
    uint32_t height;
    const uint8_t *rgb;
} d3d8_overlay_picture;
typedef void (*d3d8_overlay_picture_hook)(const d3d8_overlay_picture *picture, void *context);
typedef void (*d3d8_overlay_vblank_hook)(void *context);

/**
 * T760, default off: a PURE OBSERVER like the dump. `picture_hook` is called exactly once per
 * UpdateOverlay (also for the update the dump limit has stopped writing). `vblank_hook` is called at
 * every modeled vblank the GPU model completes (d3d8_overlay_consume_vblank), before the consume
 * policy runs. A sink that latches the picture on the first and presents on the second shows one
 * picture per vblank, the overlay one vblank late (INFERRED, T540). Either may be NULL, both NULL
 * switches it off. `resolver` maps the Data word as for the dump, the dump's own resolver is used while a dump directory is set. Allocates the picture buffers
 * if the dump has not. False when it cannot. Survives d3d8_overlay_reset.
 */
bool d3d8_overlay_set_present_hook(d3d8_overlay_picture_hook picture_hook,
                                   d3d8_overlay_vblank_hook vblank_hook, void *context,
                                   d3d8_overlay_data_resolver resolver);

/** Updates whose picture was written, and updates that were asked for but could not be written. */
uint64_t d3d8_overlay_dump_written(void);
uint64_t d3d8_overlay_dump_failed(void);

/** Buffers the policy retired since the last reset. */
uint64_t d3d8_overlay_consumed_count(void);

/** Clear the shadow, the descriptor, the counters and the write log. */
void d3d8_overlay_reset(void);

/** A copy of the last UpdateOverlay descriptor with the running counters. */
d3d8_overlay_descriptor d3d8_overlay_state(void);

/** Read a shadowed register. False for an offset outside the range or not 4-aligned. */
bool d3d8_overlay_register_value(uint32_t offset, uint32_t *value);

/**
 * The write log, oldest first, at most D3D8_OVERLAY_WRITE_LOG_CAPACITY entries (the newest when
 * more were written). Returns how many were copied to `out`, up to `capacity`.
 */
size_t d3d8_overlay_write_log(d3d8_overlay_write *out, size_t capacity);

/**
 * The hardware side of a register, for a scanout model and for tests. It is not a title write, so
 * it is neither counted nor logged. Returns false for an offset outside the range.
 */
bool d3d8_overlay_hardware_write(uint32_t offset, uint32_t value);

/** The ported entry points. Each carries the original address in its definition. */
uint32_t d3d8_overlay_enable(void);
uint32_t d3d8_overlay_update(uint32_t surface, uint32_t source, uint32_t destination,
                             uint32_t color_key_enable, uint32_t color_key);
uint32_t d3d8_overlay_update_status(void);

/** Register the three handlers. Resets the shadow. Returns how many the surface carried. */
size_t d3d8_overlay_register(void);

#endif /* TSFP_GPU_D3D8_OVERLAY_H */
