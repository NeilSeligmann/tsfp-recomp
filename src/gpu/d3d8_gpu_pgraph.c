/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_gpu_pgraph.h"

#include "d3d8_gpu.h"
#include "d3d8_resource.h"
#include "guest_frame_trace.h"
#include "guest_mem.h"
#include "kernel_call.h"

#define BATCH 256u

gpu_pgraph_result d3d8_gpu_decode_recording(gpu_pgraph *pgraph, size_t *next)
{
    return d3d8_gpu_decode_recording_until(pgraph, next, d3d8_gpu_stream_count());
}

static gpu_pgraph_result decode_recording_until(gpu_pgraph *pgraph, size_t *next, size_t end);

/* T1289: the guest frame trace times the push buffer decode (guest thread, at the Swap). */
gpu_pgraph_result d3d8_gpu_decode_recording_until(gpu_pgraph *pgraph, size_t *next, size_t end)
{
    gft_enter(GFT_DECODE, 0u);
    const gpu_pgraph_result result = decode_recording_until(pgraph, next, end);
    gft_leave();
    return result;
}

static gpu_pgraph_result decode_recording_until(gpu_pgraph *pgraph, size_t *next, size_t end)
{
    if (pgraph == NULL || next == NULL) {
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    const size_t held = d3d8_gpu_stream_count();
    const size_t total = end < held ? end : held;
    gpu_pgraph_command batch[BATCH];
    while (*next < total) {
        size_t count = 0u;
        while (count < BATCH && *next + count < total) {
            const d3d8_gpu_command command = d3d8_gpu_stream_at(*next + count);
            if (command.subchannel != D3D8_GPU_SUBCHANNEL_3D) {
                break;
            }
            batch[count].method = command.method;
            batch[count].data = command.data;
            count++;
        }
        if (count != 0u) {
            const uint64_t before = gpu_pgraph_get_stats(pgraph).pairs;
            const gpu_pgraph_result result = gpu_pgraph_decode(pgraph, batch, count);
            if (result != GPU_PGRAPH_OK) {
                *next += (size_t)(gpu_pgraph_get_stats(pgraph).pairs - before);
                return result;
            }
            *next += count;
            continue;
        }
        /* T391: a command on another subchannel, in its place in the order. */
        const d3d8_gpu_command other = d3d8_gpu_stream_at(*next);
        const gpu_pgraph_result result =
            gpu_pgraph_decode_other_subchannel(pgraph, other.subchannel, other.method, other.data);
        if (result != GPU_PGRAPH_OK) {
            return result;
        }
        *next += 1u;
    }
    return GPU_PGRAPH_OK;
}

bool d3d8_gpu_resolve_texture(void *context, const live_texture_binding *binding, uint32_t bytes,
                              uint32_t *address, uint64_t *identity, const char **refusal)
{
    (void)context;
    *identity = 0u;
    if (bytes == 0u || (uint64_t)binding->data + bytes > UINT32_MAX) {
        *refusal = "texture address span wraps";
        return false;
    }
    if (binding->header != 0u) {
        uint32_t words[5];
        if (!kernel_guest_read_bytes(binding->header, words, sizeof words) ||
            words[1] != binding->data || words[3] != binding->format || words[4] != binding->size_word) {
            *refusal = "texture binding header changed or is unreadable";
            return false;
        }
        const d3d8_resource_alias_result result = d3d8_resource_try_alias(
            binding->header, binding->data, bytes, address, identity, refusal);
        if (result == D3D8_RESOURCE_ALIAS_RESOLVED) return true;
        if (result == D3D8_RESOURCE_ALIAS_REFUSED) return false;
    }
    kernel_guest_ptr first = 0u, last = 0u;
    if (guest_virtual_from_physical(binding->data, &first)) {
        if (!guest_virtual_from_physical(binding->data + bytes - 1u, &last) ||
            (uint64_t)first + bytes - 1u != last) {
            *refusal = "texture physical span crosses allocation";
            return false;
        }
        *address = first;
    } else {
        *address = binding->data;
    }
    *identity = guest_allocation_generation(*address);
    return true;
}
