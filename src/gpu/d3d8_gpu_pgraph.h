/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The seam between the instantaneous GPU's recording (d3d8_gpu.h) and the command interpreter
 * (gpu_pgraph.h, T84): the recording is decoded into a model, and the guest memory the vertex
 * arrays point into is read through the D3D8 module's guest access.
 */

#ifndef TSFP_GPU_D3D8_GPU_PGRAPH_H
#define TSFP_GPU_D3D8_GPU_PGRAPH_H

#include "gpu_pgraph.h"
#include "d3d8_gpu_memory.h"
#include "live_texture.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Decode the recorded pairs from index `*next` to the end of the recording into `pgraph` and
 * advance `*next` past the ones consumed. The recording only grows between d3d8_gpu_reset calls
 * (past the capacity pairs are dropped, never overwritten), so the index is stable. On a refusal
 * `*next` points AT the refused pair and gpu_pgraph_error says why. Reset `*next` to 0 and the
 * model together with d3d8_gpu_reset.
 */
gpu_pgraph_result d3d8_gpu_decode_recording(gpu_pgraph *pgraph, size_t *next);

/**
 * The same, stopping at recorded pair `end` (exclusive, clamped to the recording) so a caller can
 * decode a frame in pieces (T84a1: split at a render target switch). `*next` may already be past `end`,
 * in which case nothing is decoded.
 */
gpu_pgraph_result d3d8_gpu_decode_recording_until(gpu_pgraph *pgraph, size_t *next, size_t end);

/* Binding-aware checked VA resolution and a direct VA reader for the texture bridge. */
bool d3d8_gpu_resolve_texture(void *context, const live_texture_binding *binding, uint32_t bytes,
                              uint32_t *address, uint64_t *identity, const char **refusal);

#endif
