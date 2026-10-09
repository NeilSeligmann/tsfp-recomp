/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T832: the replay copy (gpu_pgraph_replay_copy) under GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES. A separate file so the default copy in
 * gpu_pgraph_replay.c stays byte for byte as measured and the rules here are mutated on their own (tools/mutate/sets/gpu_pgraph_copy_rules.py).
 */
#include "gpu_pgraph_replay.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static gpu_pgraph_result fail(gpu_pgraph_report *report, gpu_pgraph_result result, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static gpu_pgraph_result fail(gpu_pgraph_report *report, gpu_pgraph_result result, const char *format, ...)
{
    if (report != NULL) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(report->error, sizeof report->error, format, arguments);
        va_end(arguments);
    }
    return result;
}

/* T832, HQ58 (xemu-level): the copy under GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES, the rules of live_target_plan_blit. Kept apart from
 * the default path so that path stays byte for byte as measured. Formats 0xA, 7 (alpha 0xFF) and 6 (alpha 0), a row wider than the
 * narrower pitch cut to it with both x offsets 0, overlapping rectangles of one image copied rows ascending with one buffered row. The
 * tightly packed image rule and the flip_y row mirror are the default path's. */
gpu_pgraph_result gpu_pgraph_replay_copy_planner_rules(const gpu_pgraph_copy *copy, const gpu_pgraph_backend *backend,
                                                       const gpu_image *source, gpu_image *destination,
                                                       uint32_t *used_inferences, gpu_pgraph_report *report)
{
    const bool force_alpha = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ||
                             copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0;
    if (copy->color_format != GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8 && !force_alpha) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a blit of colour format 0x%X: with the planner rules only 0xA, 7 and 6 have a pixel meaning in the "
                    "replay's A8R8G8B8 images (the Y8 and R5G6B5 byte path is refused)",
                    (unsigned)copy->color_format);
    }
    if (source == NULL || destination == NULL || source->pixels == NULL || destination->pixels == NULL) {
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    if (copy->operation != GPU_PGRAPH_BLIT_OPERATION_SRCCOPY) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "a blit operation %u: only SRCCOPY (3) is measured",
                    (unsigned)copy->operation);
    }
    if (copy->source_pitch != source->width * 4u || copy->destination_pitch != destination->width * 4u ||
        source->stride_bytes != source->width * 4u || destination->stride_bytes != destination->width * 4u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a blit with source pitch %u and destination pitch %u over images %ux%u and %ux%u: the replay's "
                    "images are tightly packed, so only a pitch of width * 4 addresses their rows",
                    (unsigned)copy->source_pitch, (unsigned)copy->destination_pitch, (unsigned)source->width,
                    (unsigned)source->height, (unsigned)destination->width, (unsigned)destination->height);
    }
    uint32_t row_pixels = copy->width;
    const uint32_t narrow_width = source->width < destination->width ? source->width : destination->width;
    if (row_pixels > narrow_width) {
        if (copy->in_x != 0u || copy->out_x != 0u) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "a blit of %u pixels wider than the narrower pitch (%u pixels) with x offsets (%u, %u): the "
                        "clamp is measured with both offsets 0 only",
                        (unsigned)copy->width, (unsigned)narrow_width, (unsigned)copy->in_x, (unsigned)copy->out_x);
        }
        row_pixels = narrow_width;
    }
    if ((uint64_t)copy->in_x + row_pixels > source->width || (uint64_t)copy->in_y + copy->height > source->height ||
        (uint64_t)copy->out_x + row_pixels > destination->width ||
        (uint64_t)copy->out_y + copy->height > destination->height) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a blit of %ux%u from (%u, %u) of a %ux%u image to (%u, %u) of a %ux%u image reaches past an image",
                    (unsigned)copy->width, (unsigned)copy->height, (unsigned)copy->in_x, (unsigned)copy->in_y,
                    (unsigned)source->width, (unsigned)source->height, (unsigned)copy->out_x, (unsigned)copy->out_y,
                    (unsigned)destination->width, (unsigned)destination->height);
    }
    if ((backend->allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL) == 0u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "the replayed image of a surface being what the hardware holds, copied as xemu's byte copy, is "
                    "INFERRED and not allowed");
    }
    *used_inferences |= GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL;
    *used_inferences |= GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES;
    for (uint32_t row = 0u; row < copy->height; row++) {
        const uint32_t source_row = backend->flip_y ? source->height - 1u - (copy->in_y + row) : copy->in_y + row;
        const uint32_t destination_row =
            backend->flip_y ? destination->height - 1u - (copy->out_y + row) : copy->out_y + row;
        uint8_t *to = destination->pixels + (size_t)destination_row * destination->stride_bytes + (size_t)copy->out_x * 4u;
        memmove(to, source->pixels + (size_t)source_row * source->stride_bytes + (size_t)copy->in_x * 4u,
                (size_t)row_pixels * 4u);
        if (force_alpha) {
            const uint8_t alpha = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0xFFu : 0x00u;
            for (uint32_t pixel = 0u; pixel < row_pixels; pixel++) {
                to[(size_t)pixel * 4u + 3u] = alpha;
            }
        }
    }
    return GPU_PGRAPH_OK;
}
