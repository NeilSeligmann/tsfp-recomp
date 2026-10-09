/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_D3D8_LOCK_H
#define TSFP_GPU_D3D8_LOCK_H
#include <stddef.h>
#include <stdint.h>
/* Original 0x003D4AC0, stdcall(surface, locked_rect, rect, flags), ret16.
 * Pitch/EAX preserve the original; Bits resolves an explicitly registered live
 * contiguous allocation. Pending waits refuse before writes.
 * Flags 0x40 (uncached alias, the XMV converter 0x4463AE passes it) publishes the same
 * Pitch and Bits as flags 0. The original only swaps Bits high bits 0x80000000 for
 * 0xF0000000 and every other output and the wait gate are identical (T393 oracle). The
 * native guest memory is coherent with one mapping per allocation, so the alias is the
 * same bytes and only write-combining stays unmodelled (the title never fences it).
 * Flag 0x20 skips the wait as before. Other flag bits are ignored, as in the original
 * which tests only 0x20 and 0x40. */
uint32_t d3d8_surface_lock_rect(uint32_t surface, uint32_t locked_rect, uint32_t rect, uint32_t flags);
/* Original 0x003D4E60, stdcall(texture, level, locked_rect, rect, flags), ret20.
 * Bounded idle registered mip0 / single-level 2D format6 / Size0 / NULLrect /
 * flags0 or20 view (flag 0x40 is refused here, only the surface lock models it). Bits is the raw swizzled backing, never linearized.
 * Registered identity/extent are checked; the caller must keep backing payload
 * mappings readable/writable. Payload OS permissions are not probed.
 * Inputs and mappings must remain quiescent. Output preflight performs an
 * identical-byte write before final Pitch/Bits publication; unsupported aliases,
 * layouts and wait/uncached paths stop without publishing a new result. */
uint32_t d3d8_texture_lock_rect(uint32_t texture, uint32_t level, uint32_t locked_rect,
                              uint32_t rect, uint32_t flags);
size_t d3d8_lock_register(void);
#endif
