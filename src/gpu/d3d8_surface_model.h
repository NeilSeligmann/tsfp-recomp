/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The guest memory surface model (T633, T596): what the replay takes a surface to hold when no replayed pass drew it.
 *
 * WHAT IS MEASURED. The host never writes a GPU result back to guest memory, so the bytes under a render target are what the CPU wrote and nothing
 * else. On the retail boots they are ARGB 0x00000000 for every texel of the back buffer (9 of 9 movie and 14 of 14 loop samples, docs/d3d8-copy-composition.md
 * "WHAT THE SURFACES HOLD"). WHAT IS INFERRED, and announced by the callers (`--gpu-replay-surface-source`): that those bytes, read when the frame is
 * replayed, are the bytes the surface held when the draw or blit ran.
 *
 * Two readers and one store, all keyed by the surface's Data word (the 28 bit address, in this host a synthetic region number that d3d8_gpu_read_guest
 * resolves):
 *   - d3d8_surface_model_probe: are the bytes under a range all zero, not all zero, or unreadable.
 *   - d3d8_surface_model_read_argb: the bytes of a pitched A8R8G8B8 surface as an RGBA image (the layout gpu_image holds: B and R swapped).
 *   - the BYTE SURFACES, a bounded store of the guest bytes a CopyRects byte path blit (Y8, R5G6B5, rows of whole pitches) wrote: the first touch
 *     reads the extent from guest memory, every blit then moves bytes inside the store, and a reader (d3d8_surface_model_read_bytes) gets the kept
 *     bytes over the guest's. The guest memory itself is never written.
 * The read function is injectable so a unit test needs no guest memory. Default: d3d8_gpu_read_guest.
 */
#ifndef TSFP_GPU_D3D8_SURFACE_MODEL_H
#define TSFP_GPU_D3D8_SURFACE_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Byte surfaces kept at once. Beyond it a new surface is REFUSED (never evicted: an evicted one would silently turn back into the guest's bytes). */
#define D3D8_SURFACE_MODEL_BYTE_SURFACES 8u
/* The largest extent one byte surface may hold, bytes. Beyond it is REFUSED by name. */
#define D3D8_SURFACE_MODEL_MAX_BYTES (32u * 1024u * 1024u)

typedef bool (*d3d8_surface_model_read_fn)(void *context, uint32_t address, void *out, size_t bytes);

typedef enum {
    D3D8_SURFACE_MODEL_OK = 0,
    D3D8_SURFACE_MODEL_UNREADABLE,  /* the guest range cannot be read */
    D3D8_SURFACE_MODEL_ALIAS,       /* the range overlaps a kept byte surface of another Data word */
    D3D8_SURFACE_MODEL_FULL,        /* all D3D8_SURFACE_MODEL_BYTE_SURFACES slots are taken */
    D3D8_SURFACE_MODEL_TOO_LARGE,   /* beyond D3D8_SURFACE_MODEL_MAX_BYTES */
    D3D8_SURFACE_MODEL_MEMORY       /* out of memory */
} d3d8_surface_model_status;

typedef enum { D3D8_SURFACE_ZERO, D3D8_SURFACE_NONZERO, D3D8_SURFACE_UNREADABLE } d3d8_surface_probe;

/** Use `read` (NULL: d3d8_gpu_read_guest) for every guest read from now on. For tests. */
void d3d8_surface_model_set_reader(d3d8_surface_model_read_fn read, void *context);

/** Forget every byte surface (a CreateDevice made new surfaces, or the replay was disabled). */
void d3d8_surface_model_reset(void);

/** Are the `bytes` bytes at `data` all zero. Read in 64 KiB pieces. */
d3d8_surface_probe d3d8_surface_model_probe(uint32_t data, uint64_t bytes);

/** The text of a status, for a refusal. */
const char *d3d8_surface_model_status_string(d3d8_surface_model_status status);

/**
 * Read `rows` rows of `width` A8R8G8B8 pixels, rows `pitch` bytes apart, from `data` into `rgba` (rows * width * 4 bytes, tightly packed): each
 * guest dword 0xAARRGGBB (bytes B, G, R, A) becomes the bytes R, G, B, A. Needs pitch >= width * 4. A byte surface of the same Data word is NOT
 * consulted: the caller refuses an image read of a surface the byte path wrote (d3d8_surface_model_has_bytes).
 */
d3d8_surface_model_status d3d8_surface_model_read_argb(uint32_t data, uint32_t width, uint32_t rows, uint32_t pitch, uint8_t *rgba);

/** Does the store hold a byte surface at `data`. */
bool d3d8_surface_model_has_bytes(uint32_t data);

/** Does [data, data + bytes) overlap a kept byte surface (any Data word, `data` itself included). */
bool d3d8_surface_model_overlaps_bytes(uint32_t data, uint64_t bytes);

/**
 * The kept byte surface at `data`, grown to at least `length` bytes (a new one reads the guest's bytes, a grown one reads the guest's bytes
 * for the new tail only). `*bytes` is valid until the next call. ALIAS when the range overlaps a surface of another Data word.
 */
d3d8_surface_model_status d3d8_surface_model_acquire(uint32_t data, size_t length, uint8_t **bytes);

/**
 * `length` bytes of the surface at `data` into `out`: the kept bytes over the guest's (a byte surface the blits wrote, else the guest's bytes
 * alone). False when the guest cannot be read. ALIAS refuses a range that partly overlaps a kept byte surface of another Data word (status out).
 */
d3d8_surface_model_status d3d8_surface_model_read_bytes(uint32_t data, uint8_t *out, size_t length);

/** Byte surfaces held, and their total bytes. */
size_t d3d8_surface_model_byte_surface_count(void);
size_t d3d8_surface_model_byte_surface_length(uint32_t data);

#endif
