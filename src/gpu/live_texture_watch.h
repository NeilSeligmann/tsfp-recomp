/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_LIVE_TEXTURE_WATCH_H
#define TSFP_GPU_LIVE_TEXTURE_WATCH_H
/* T792: the guest write watch of the live texture cache. Guest code stores through plain host pointers (lifted code, no
 * store interception exists), so the observable events are the D3D8 LockRect calls: the title gets a pointer and writes the
 * texels after it. d3d8_lock.c notes the locked Data range here, the renderer thread drains the notes into
 * live_texture_note_write before it looks a texture up. Thread safe (a guest thread notes, the render thread drains). A note
 * overflow invalidates every texture. NOT observed: writes through a pointer kept after UnlockRect, kernel or file reads
 * into texture memory, and a guest write to a texture it never locked. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "live_texture.h"

#define LIVE_TEXTURE_WATCH_PENDING 64u

/* Record one written range (Data word domain, as live_texture_binding.data). Cheap, never fails. */
void live_texture_watch_note(uint32_t address, uint32_t bytes);
/* Apply every pending note to `cache`, return how many cached textures became stale. */
size_t live_texture_watch_drain(live_texture_cache *cache);
/* Counters for the census: notes since start, drained notes, overflows. */
void live_texture_watch_counts(uint64_t *noted, uint64_t *overflows);
/* Pending note `index` (oldest first) without draining, false past the end. For tests and the census. */
bool live_texture_watch_pending(size_t index, uint32_t *address, uint32_t *bytes);
void live_texture_watch_reset(void);
#endif
