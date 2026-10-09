/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * D3D8 buffer resources: the 12-byte headers the title holds pointers to, and the contiguous
 * memory behind them.
 *
 * `0x003D4EE0` is the library's buffer creator. `InitD3D`'s helper (0x000192F0) calls it
 * three times with 0x80000 and keeps the three results in its own ring of dynamic vertex
 * buffers. The original takes a 12-byte zeroed block from the title's process heap for the
 * header and a contiguous allocation for the data, fills Common with 0x01000001 and Data with
 * the allocation's PHYSICAL address (its low 28 bits), and returns the header.
 *
 * TWO DIVERGENCES, ON THE RECORD.
 *   1. The header comes from a heap this host owns, not the title's. An HLE handler cannot call
 *      the title's allocator, which is lifted game code. Heap bookkeeping differs; ordinary
 *      last-reference destruction remains unsupported until ownership is recovered.
 *   2. Data is the kernel module's SYNTHETIC physical address, so the library's Lock
 *      (0x003D4F30), which returns `Data | 0x80000000` on a console, cannot be a literal port
 *      here: that value would be an unmapped address. This module records each buffer's
 *      (physical, virtual) pair so a Lock handler can return the virtual address the data
 *      really is at. `d3d8_resource_virtual_of_physical` is that lookup.
 */

#ifndef TSFP_GPU_D3D8_RESOURCE_H
#define TSFP_GPU_D3D8_RESOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "d3d8_pushbuffer.h"

/** Header size and the Common word 0x003D4F1F stores. */
#define D3D8_BUFFER_HEADER_BYTES 12u
#define D3D8_BUFFER_COMMON 0x01000001u

/**
 * 0x003D4EE0: create a buffer of `length` bytes. Returns the header's guest address, or 0 when
 * either allocation fails (the header is then released, as the original releases it).
 */
uint32_t d3d8_create_buffer(uint32_t length);

/** 0x003D4F30, stdcall(header, flags): returns the registered data guest address.
 * Bit 0x10 skips method 0x1710; bits 0xA0 skip the measured resource-fence wait.
 * Foreign Data addresses fail explicitly because the host cannot translate them. */
uint32_t d3d8_vertex_buffer_lock2(uint32_t header, uint32_t flags);

/** 0x003D58F0, stdcall(stream, header, stride): maintain guest stream bindings and
 * their resource reference/fence state. Last-reference destruction fails explicitly.
 * Returns the measured eax (stride); the original API exposes no status result. */
uint32_t d3d8_set_stream_source(uint32_t stream, uint32_t header, uint32_t stride);

/** 0x003D4FB0, stdcall(primitive, first, count), ret 12: record original DrawVertices
 * commands and the verified startup dirty-state cascade. Unsupported state and large
 * reservations stop before emission. Records guest commands; it does not rasterize. */
/* Shared exact deferred-state/stream work; read-only planning precedes emission. */
void d3d8_draw_plan_deferred(d3d8_pushbuffer_sim *sim, uint32_t base_vertex);
void d3d8_draw_flush_streams(uint32_t base_vertex);
/* 0x003DED80: the dirty cascade controller Begin and the swap composition call directly (T443). */
void d3d8_run_dirty_cascade(void);
uint32_t d3d8_draw_vertices(uint32_t primitive, uint32_t first, uint32_t count);
/* 0x003D4D70, stdcall(header, base), ret 8: relocate Data modulo 2^32.
 * Type 0x20000 retains its virtual address; other types mask to 28 bits. */
uint32_t d3d8_register_resource(uint32_t header, uint32_t base);
/* Resolve ONLY a recorded Register binding to its still-live contiguous region.
 * Validates immutable Common, Data/Format/Size and the requested allocation span. */
uint32_t d3d8_resource_resolve_registered_alias(uint32_t header, uint32_t data, uint32_t bytes);
/* Same registered alias contract, with the invoking lock entry for diagnostics. */
uint32_t d3d8_resource_resolve_registered_alias_at(uint32_t entry, uint32_t header,
                                                uint32_t data, uint32_t bytes);
typedef enum { D3D8_RESOURCE_ALIAS_ABSENT, D3D8_RESOURCE_ALIAS_RESOLVED,
               D3D8_RESOURCE_ALIAS_REFUSED } d3d8_resource_alias_result;
/* Nonfatal exact-header resolver for GPU reads; no Data-only alias search. */
d3d8_resource_alias_result d3d8_resource_try_alias(uint32_t header, uint32_t data, uint32_t bytes,
                                                  uint32_t *address, uint64_t *identity,
                                                  const char **refusal);
/* Observe actual full guest memcpy/memmove transactions, without modifying guest bytes.
 * Captures verified source headers before writes and verifies copied destinations afterwards. */
void *d3d8_resource_copy_begin(uint32_t destination, uint32_t source, uint32_t bytes);
size_t d3d8_resource_copy_end(void *transaction);
/* How many Register bindings are recorded and valid (T755). For tests. */
size_t d3d8_resource_alias_count(void);
size_t d3d8_resource_draw_register(void);

/** The guest virtual address of the contiguous allocation whose physical address is `physical`
 * (a header's Data word), or 0 when no buffer created here has it. */
uint32_t d3d8_resource_virtual_of_physical(uint32_t physical);

/** The guest virtual address of the registered texture or surface allocation whose header Data word is
 * `data`, or 0 when none is registered. Read only, for the movie frame dump (T394). */
uint32_t d3d8_resource_virtual_of_registered_data(uint32_t data);

/** How many buffers are live in the registry. For tests. */
size_t d3d8_resource_buffer_count(void);

/** Forget the registry and the header heap. For tests, after the guest allocator was reset. */
void d3d8_resource_reset(void);

#endif /* TSFP_GPU_D3D8_RESOURCE_H */
