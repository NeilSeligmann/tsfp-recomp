/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_LIVE_TEXTURE_INPUTS_H
#define TSFP_GPU_LIVE_TEXTURE_INPUTS_H
/* T1246: the guest-memory inputs of one frame's texture lookups, captured on the guest thread at the Swap so the live renderer can
 * draw the frame on the presenter thread while the guest keeps running. For every binding a draw of the frame can sample the
 * store holds what live_texture_lookup_resolved would have asked the resolver and the reader (resolved address, backing identity,
 * or the refusal, plus the source bytes). The lookups of the presenter job then use `live_texture_inputs_resolver` and
 * `live_texture_inputs_reader` instead of guest memory, so they see the memory as it stood at the Swap, exactly like the serial
 * path that ran while the guest was blocked. A lookup the store cannot answer is a MISS (counted, refused by name), never a
 * fallback to live guest memory. */
#include "live_texture.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct live_texture_inputs live_texture_inputs;

live_texture_inputs *live_texture_inputs_create(size_t budget_bytes);
void live_texture_inputs_destroy(live_texture_inputs *inputs);
/* Forget the previous frame's entries (the byte arena is reused). */
void live_texture_inputs_reset(live_texture_inputs *inputs);
/* True when the lookup of `binding` resolves to (or aliases) a registered render target of `cache`, so it reads no guest bytes. */
bool live_texture_inputs_names_target(const live_texture_cache *cache, const live_texture_binding *binding, uint32_t source_bytes);
/* Capture one binding with the guest side resolver and reader (the same ones the lookup would use). Idempotent per binding.
 * Nothing is read for a binding the plan refuses or that names a registered render target of `cache` (the lookup reads no
 * guest bytes for those). False when the store is full (budget or entry limit): the frame must then not be pipelined. */
bool live_texture_inputs_capture(live_texture_inputs *inputs, const live_texture_cache *cache, const live_texture_binding *binding,
                                 live_texture_resolver resolver, void *resolver_context, live_texture_reader reader,
                                 void *reader_context);
/* The presenter side, same signatures as live_texture_resolver and live_texture_reader. `context` is the store. */
bool live_texture_inputs_resolver(void *context, const live_texture_binding *binding, uint32_t bytes, uint32_t *address,
                                  uint64_t *identity, const char **refusal);
bool live_texture_inputs_reader(void *context, uint32_t address, void *out, size_t bytes);
/* Lookups the store could not answer since create (a pipelined frame that diverged from the serial one), and entries held now. */
uint64_t live_texture_inputs_misses(const live_texture_inputs *inputs);
size_t live_texture_inputs_entries(const live_texture_inputs *inputs);
size_t live_texture_inputs_bytes(const live_texture_inputs *inputs);

#endif
