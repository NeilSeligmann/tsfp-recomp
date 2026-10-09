/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T793 live render targets, CopyRects blit planner and present schedule. Rules and their evidence are in live_target.h.
 */

#include "live_target.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    live_target_desc desc;
    uint64_t generation;
    uint8_t *pixels;
} live_target_entry;

struct live_target_registry {
    live_target_entry entries[LIVE_TARGET_MAX_TARGETS];
    size_t count;
    live_target_stats stats;
};

static const char *const refusal_names[LIVE_TARGET_REFUSE_COUNT] = {
    "ok",
    "bad argument",
    "surface format outside the measured colour set",
    "surface size not decodable",
    "target registry full",
    "blit source surface not registered",
    "blit destination surface not registered",
    "header re-registered with other geometry or format",
    "2D blit of a swizzled surface (addressing not measured)",
    "blit operation other than SRCCOPY",
    "blit colour format outside 1, 4, 6, 7, 0xA",
    "blit colour format does not match the surface bytes per pixel",
    "R5G6B5 or Y8 byte blit (INFERRED, T769) not allowed",
    "blit pitch differs from the registered target pitch",
    "blit rectangle reaches past a surface",
    "clamped blit row with a non-zero x offset (unmeasured)",
    "front buffer header does not decode",
};

const char *live_target_refusal_name(live_target_refusal reason)
{
    return (unsigned)reason < LIVE_TARGET_REFUSE_COUNT ? refusal_names[reason] : "unknown refusal";
}

live_target_refusal live_target_desc_from_words(uint32_t data, uint32_t format_word, uint32_t size_word,
                                                live_target_desc *out)
{
    if (out == NULL) {
        return LIVE_TARGET_REFUSE_ARGUMENT;
    }
    const uint32_t color = (format_word >> 8) & 0xFFu;
    const bool swizzled = size_word == 0u;
    live_target_format format;
    uint32_t bytes_per_pixel;
    switch (color) {
    case 0x12u:
    case 0x06u:
        format = LIVE_TARGET_FORMAT_A8R8G8B8;
        bytes_per_pixel = 4u;
        break;
    case 0x1Eu:
    case 0x07u:
        format = LIVE_TARGET_FORMAT_X8R8G8B8;
        bytes_per_pixel = 4u;
        break;
    case 0x11u:
    case 0x05u:
        format = LIVE_TARGET_FORMAT_R5G6B5;
        bytes_per_pixel = 2u;
        break;
    default:
        return LIVE_TARGET_REFUSE_FORMAT_UNKNOWN;
    }
    /* A linear Format byte with no Size word (or the reverse) is not a header either rule describes. */
    const bool linear_code = color == 0x12u || color == 0x1Eu || color == 0x11u;
    if (linear_code == swizzled) {
        return LIVE_TARGET_REFUSE_SIZE_UNKNOWN;
    }
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    if (swizzled) {
        const uint32_t width_exponent = (format_word >> 20) & 0xFu;
        const uint32_t height_exponent = (format_word >> 24) & 0xFu;
        if (width_exponent == 0u && height_exponent == 0u) {
            return LIVE_TARGET_REFUSE_SIZE_UNKNOWN;
        }
        width = 1u << width_exponent;
        height = 1u << height_exponent;
        pitch = width * bytes_per_pixel;
    } else {
        width = (size_word & 0xFFFu) + 1u;
        height = ((size_word >> 12) & 0xFFFu) + 1u;
        pitch = ((size_word >> 24) + 1u) * 64u;
    }
    if (width > LIVE_TARGET_MAX_EDGE || height > LIVE_TARGET_MAX_EDGE || pitch < width * bytes_per_pixel) {
        return LIVE_TARGET_REFUSE_SIZE_UNKNOWN;
    }
    out->data = data;
    out->format_word = format_word;
    out->size_word = size_word;
    out->width = width;
    out->height = height;
    out->pitch = pitch;
    out->bytes_per_pixel = bytes_per_pixel;
    out->format = format;
    out->swizzled = swizzled;
    return LIVE_TARGET_OK;
}

live_target_registry *live_target_registry_create(void)
{
    return calloc(1, sizeof(live_target_registry));
}

void live_target_registry_destroy(live_target_registry *registry)
{
    if (registry == NULL) {
        return;
    }
    for (size_t i = 0; i < registry->count; i++) {
        free(registry->entries[i].pixels);
    }
    free(registry);
}

size_t live_target_registry_count(const live_target_registry *registry)
{
    return registry == NULL ? 0u : registry->count;
}

live_target_stats live_target_registry_stats(const live_target_registry *registry)
{
    live_target_stats empty;
    memset(&empty, 0, sizeof empty);
    return registry == NULL ? empty : registry->stats;
}

static live_target_entry *find_entry(const live_target_registry *registry, uint32_t data)
{
    if (registry == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < registry->count; i++) {
        if (registry->entries[i].desc.data == data) {
            return (live_target_entry *)&registry->entries[i];
        }
    }
    return NULL;
}

static live_target_refusal refuse(live_target_registry *registry, live_target_refusal reason, bool blit)
{
    if (registry != NULL) {
        registry->stats.refusals[reason]++;
        if (blit) {
            registry->stats.blits_refused++;
        }
    }
    return reason;
}

live_target_refusal live_target_register(live_target_registry *registry, uint32_t data, uint32_t format_word,
                                         uint32_t size_word, bool keep_pixels)
{
    if (registry == NULL) {
        return LIVE_TARGET_REFUSE_ARGUMENT;
    }
    live_target_desc desc;
    const live_target_refusal decoded = live_target_desc_from_words(data, format_word, size_word, &desc);
    if (decoded != LIVE_TARGET_OK) {
        return refuse(registry, decoded, false);
    }
    live_target_entry *existing = find_entry(registry, data);
    if (existing != NULL) {
        if (existing->desc.width != desc.width || existing->desc.height != desc.height ||
            existing->desc.pitch != desc.pitch || existing->desc.format != desc.format ||
            existing->desc.swizzled != desc.swizzled) {
            return refuse(registry, LIVE_TARGET_REFUSE_REDECLARED, false);
        }
        if (keep_pixels && existing->pixels == NULL) {
            existing->pixels = calloc((size_t)desc.pitch * desc.height, 1u);
            if (existing->pixels == NULL) {
                return refuse(registry, LIVE_TARGET_REFUSE_ARGUMENT, false);
            }
        }
        return LIVE_TARGET_OK;
    }
    if (registry->count >= LIVE_TARGET_MAX_TARGETS) {
        return refuse(registry, LIVE_TARGET_REFUSE_REGISTRY_FULL, false);
    }
    live_target_entry *entry = &registry->entries[registry->count];
    memset(entry, 0, sizeof *entry);
    entry->desc = desc;
    if (keep_pixels) {
        entry->pixels = calloc((size_t)desc.pitch * desc.height, 1u);
        if (entry->pixels == NULL) {
            return refuse(registry, LIVE_TARGET_REFUSE_ARGUMENT, false);
        }
    }
    registry->count++;
    registry->stats.registered++;
    return LIVE_TARGET_OK;
}

const live_target_desc *live_target_find(const live_target_registry *registry, uint32_t data)
{
    const live_target_entry *entry = find_entry(registry, data);
    return entry == NULL ? NULL : &entry->desc;
}

uint64_t live_target_generation(const live_target_registry *registry, uint32_t data)
{
    const live_target_entry *entry = find_entry(registry, data);
    return entry == NULL ? 0u : entry->generation;
}

void live_target_note_written(live_target_registry *registry, uint32_t data)
{
    live_target_entry *entry = find_entry(registry, data);
    if (entry != NULL) {
        entry->generation++;
    }
}

uint8_t *live_target_pixels(live_target_registry *registry, uint32_t data)
{
    live_target_entry *entry = find_entry(registry, data);
    return entry == NULL ? NULL : entry->pixels;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

live_target_refusal live_target_plan_blit(live_target_registry *registry, const live_target_blit *blit,
                                          uint32_t allowed, live_target_blit_plan *plan)
{
    if (registry == NULL || blit == NULL || plan == NULL) {
        return LIVE_TARGET_REFUSE_ARGUMENT;
    }
    memset(plan, 0, sizeof *plan);
    registry->stats.blits_planned++;
    if (blit->operation != LIVE_BLIT_OPERATION_SRCCOPY) {
        return refuse(registry, LIVE_TARGET_REFUSE_OPERATION, true);
    }
    const live_target_entry *source = find_entry(registry, blit->source_data);
    if (source == NULL) {
        return refuse(registry, LIVE_TARGET_REFUSE_NO_SOURCE, true);
    }
    const live_target_entry *destination = find_entry(registry, blit->destination_data);
    if (destination == NULL) {
        return refuse(registry, LIVE_TARGET_REFUSE_NO_DESTINATION, true);
    }
    if (source->desc.swizzled || destination->desc.swizzled) {
        return refuse(registry, LIVE_TARGET_REFUSE_SWIZZLED, true);
    }
    uint32_t bytes_per_pixel;
    live_alpha_mode alpha = LIVE_ALPHA_KEEP;
    bool inferred = false;
    switch (blit->color_format) {
    case LIVE_BLIT_FORMAT_A8R8G8B8:
        bytes_per_pixel = 4u;
        break;
    case LIVE_BLIT_FORMAT_X8R8G8B8_ALPHAFF:
        bytes_per_pixel = 4u;
        alpha = LIVE_ALPHA_FORCE_FF;
        break;
    case LIVE_BLIT_FORMAT_X8R8G8B8_ALPHA0:
        bytes_per_pixel = 4u;
        alpha = LIVE_ALPHA_FORCE_00;
        break;
    case LIVE_BLIT_FORMAT_R5G6B5:
        bytes_per_pixel = 2u;
        inferred = true;
        break;
    case LIVE_BLIT_FORMAT_Y8:
        bytes_per_pixel = 1u;
        inferred = true;
        break;
    default:
        return refuse(registry, LIVE_TARGET_REFUSE_BLIT_FORMAT, true);
    }
    if (blit->color_format != LIVE_BLIT_FORMAT_Y8 && (source->desc.bytes_per_pixel != bytes_per_pixel ||
                                                      destination->desc.bytes_per_pixel != bytes_per_pixel)) {
        return refuse(registry, LIVE_TARGET_REFUSE_BLIT_FORMAT_MISMATCH, true);
    }
    if (blit->width == 0u || blit->height == 0u) {
        plan->empty = true;
        plan->destination_data = blit->destination_data;
        registry->stats.blits_empty++;
        return LIVE_TARGET_OK;
    }
    if (inferred && (allowed & LIVE_TARGET_INFER_BYTE_FORMATS) == 0u) {
        return refuse(registry, LIVE_TARGET_REFUSE_INFERRED_BYTE_FORMAT, true);
    }
    if (blit->source_pitch != source->desc.pitch || blit->destination_pitch != destination->desc.pitch) {
        return refuse(registry, LIVE_TARGET_REFUSE_PITCH, true);
    }
    const uint32_t narrow_pitch = min_u32(blit->source_pitch, blit->destination_pitch);
    const uint64_t wanted_bytes = (uint64_t)blit->width * bytes_per_pixel;
    uint32_t row_bytes = (uint32_t)wanted_bytes;
    bool clamped = false;
    if (wanted_bytes > narrow_pitch) {
        if (blit->in_x != 0u || blit->out_x != 0u) {
            return refuse(registry, LIVE_TARGET_REFUSE_OFFSET_CLAMP, true);
        }
        row_bytes = narrow_pitch;
        clamped = true;
    }
    const uint64_t source_row_bytes = (uint64_t)source->desc.width * source->desc.bytes_per_pixel;
    const uint64_t destination_row_bytes = (uint64_t)destination->desc.width * destination->desc.bytes_per_pixel;
    if ((uint64_t)blit->in_y + blit->height > source->desc.height ||
        (uint64_t)blit->out_y + blit->height > destination->desc.height ||
        ((uint64_t)blit->in_x * bytes_per_pixel + row_bytes) > source_row_bytes ||
        ((uint64_t)blit->out_x * bytes_per_pixel + row_bytes) > destination_row_bytes) {
        return refuse(registry, LIVE_TARGET_REFUSE_OUT_OF_BOUNDS, true);
    }
    plan->rows = blit->height;
    plan->row_bytes = row_bytes;
    plan->source_offset = blit->in_y * blit->source_pitch + blit->in_x * bytes_per_pixel;
    plan->destination_offset = blit->out_y * blit->destination_pitch + blit->out_x * bytes_per_pixel;
    plan->source_pitch = blit->source_pitch;
    plan->destination_pitch = blit->destination_pitch;
    plan->bytes_per_pixel = bytes_per_pixel;
    plan->clamped = clamped;
    plan->same_surface = blit->source_data == blit->destination_data;
    plan->alpha = alpha;
    plan->inferred = inferred;
    plan->destination_data = blit->destination_data;
    if (plan->same_surface) {
        const uint64_t source_start = (uint64_t)blit->in_x * bytes_per_pixel;
        const uint64_t destination_start = (uint64_t)blit->out_x * bytes_per_pixel;
        plan->overlap = source_start < destination_start + row_bytes && destination_start < source_start + row_bytes &&
                        blit->in_y < blit->out_y + blit->height && blit->out_y < blit->in_y + blit->height;
    }
    plan->copy_image_ok = !plan->overlap && alpha == LIVE_ALPHA_KEEP && blit->color_format == LIVE_BLIT_FORMAT_A8R8G8B8 &&
                          source->desc.format == LIVE_TARGET_FORMAT_A8R8G8B8 &&
                          destination->desc.format == LIVE_TARGET_FORMAT_A8R8G8B8;
    if (clamped) {
        registry->stats.blits_clamped++;
    }
    if (plan->overlap) {
        registry->stats.blits_overlapped++;
    }
    return LIVE_TARGET_OK;
}

void live_target_execute_plan(const live_target_blit_plan *plan, const uint8_t *source, uint8_t *destination)
{
    if (plan == NULL || plan->empty || source == NULL || destination == NULL) {
        return;
    }
    for (uint32_t row = 0u; row < plan->rows; row++) {
        uint8_t *to = destination + plan->destination_offset + (size_t)row * plan->destination_pitch;
        const uint8_t *from = source + plan->source_offset + (size_t)row * plan->source_pitch;
        memmove(to, from, plan->row_bytes);
        if (plan->alpha != LIVE_ALPHA_KEEP) {
            const uint8_t value = plan->alpha == LIVE_ALPHA_FORCE_FF ? 0xFFu : 0x00u;
            for (uint32_t byte = 3u; byte < plan->row_bytes; byte += 4u) {
                to[byte] = value;
            }
        }
    }
}

live_target_refusal live_target_apply_blit(live_target_registry *registry, const live_target_blit *blit, uint32_t allowed,
                                     live_target_blit_plan *plan, bool *executed)
{
    live_target_blit_plan local;
    live_target_blit_plan *use = plan != NULL ? plan : &local;
    if (executed != NULL) {
        *executed = false;
    }
    const live_target_refusal result = live_target_plan_blit(registry, blit, allowed, use);
    if (result != LIVE_TARGET_OK || use->empty) {
        return result;
    }
    live_target_entry *source = find_entry(registry, blit->source_data);
    live_target_entry *destination = find_entry(registry, blit->destination_data);
    destination->generation++;
    if (source->pixels != NULL && destination->pixels != NULL) {
        live_target_execute_plan(use, source->pixels, destination->pixels);
        registry->stats.blits_applied++;
        if (executed != NULL) {
            *executed = true;
        }
    }
    return result;
}

/* --- present schedule --- */

void live_present_init(live_present_schedule *schedule, uint32_t latency)
{
    if (schedule == NULL) {
        return;
    }
    memset(schedule, 0, sizeof *schedule);
    schedule->latency = latency;
}

live_target_refusal live_present_submit(live_present_schedule *schedule, live_target_registry *registry,
                                        const live_present_event *event)
{
    if (schedule == NULL || registry == NULL || event == NULL) {
        return LIVE_TARGET_REFUSE_ARGUMENT;
    }
    const live_target_refusal registered =
        live_target_register(registry, event->data, event->format_word, event->size_word, false);
    if (registered != LIVE_TARGET_OK) {
        return refuse(registry, LIVE_TARGET_REFUSE_PRESENT_UNREGISTERABLE, false);
    }
    if (schedule->count == LIVE_PRESENT_QUEUE) {
        memmove(&schedule->queue[0], &schedule->queue[1], (LIVE_PRESENT_QUEUE - 1u) * sizeof schedule->queue[0]);
        schedule->count--;
        schedule->dropped_full++;
    }
    schedule->queue[schedule->count++] = *event;
    schedule->submitted++;
    return LIVE_TARGET_OK;
}

live_target_refusal live_present_submit_media(live_present_schedule *schedule, live_target_registry *registry,
                                              const live_present_event *event)
{
    if (event == NULL) {
        return LIVE_TARGET_REFUSE_ARGUMENT;
    }
    live_present_event timed = *event;
    timed.media_timed = true;
    return live_present_submit(schedule, registry, &timed);
}

void live_present_vblank(live_present_schedule *schedule, uint64_t vblank, bool overlay_active,
                         live_present_frame *frame)
{
    if (schedule == NULL || frame == NULL) {
        return;
    }
    memset(frame, 0, sizeof *frame);
    int chosen = -1;
    for (uint32_t i = 0u; i < schedule->count; i++) {
        if (schedule->queue[i].vblank + schedule->latency <= vblank) {
            chosen = (int)i;
        }
    }
    if (chosen >= 0) {
        const live_present_event *event = &schedule->queue[chosen];
        const uint32_t interval = event->interval == 0u ? 1u : event->interval;
        if (!schedule->shown_any || vblank >= schedule->last_shown_vblank + interval) {
            schedule->superseded += (uint64_t)chosen;
            schedule->last_data = event->data;
            schedule->last_number = event->number;
            schedule->last_shown_vblank = vblank;
            schedule->shown_any = true;
            schedule->shown++;
            frame->front_new = true;
            const uint32_t remaining = schedule->count - (uint32_t)chosen - 1u;
            memmove(&schedule->queue[0], &schedule->queue[chosen + 1], remaining * sizeof schedule->queue[0]);
            schedule->count = remaining;
        }
    }
    if (schedule->shown_any) {
        frame->front_data = schedule->last_data;
        frame->front_number = schedule->last_number;
        frame->layers[frame->layer_count++] = LIVE_LAYER_FRONT;
        if (!frame->front_new) {
            frame->front_held = true;
            schedule->held++;
        }
    }
    if (overlay_active) {
        frame->layers[frame->layer_count++] = LIVE_LAYER_OVERLAY;
        schedule->overlay_frames++;
    }
}

void live_present_media_vblank(live_present_schedule *schedule, uint64_t vblank, bool overlay_active,
                               bool media_clock_valid, uint64_t media_time_ns, live_present_frame *frame)
{
    if (schedule == NULL || frame == NULL) {
        return;
    }
    memset(frame, 0, sizeof *frame);
    int chosen = -1;
    if (media_clock_valid) {
        for (uint32_t i = 0u; i < schedule->count; i++) {
            const live_present_event *event = &schedule->queue[i];
            if (event->media_timed && event->media_time_ns <= media_time_ns &&
                event->vblank + schedule->latency <= vblank) {
                chosen = (int)i;
            }
        }
    }
    if (chosen >= 0) {
        const live_present_event *event = &schedule->queue[chosen];
        const uint32_t interval = event->interval == 0u ? 1u : event->interval;
        if (!schedule->shown_any || vblank >= schedule->last_shown_vblank + interval) {
            schedule->superseded += (uint64_t)chosen;
            schedule->last_data = event->data;
            schedule->last_number = event->number;
            schedule->last_shown_vblank = vblank;
            schedule->shown_any = true;
            schedule->shown++;
            frame->front_new = true;
            const uint32_t remaining = schedule->count - (uint32_t)chosen - 1u;
            memmove(&schedule->queue[0], &schedule->queue[chosen + 1], remaining * sizeof schedule->queue[0]);
            schedule->count = remaining;
        }
    }
    if (schedule->shown_any) {
        frame->front_data = schedule->last_data;
        frame->front_number = schedule->last_number;
        frame->layers[frame->layer_count++] = LIVE_LAYER_FRONT;
        if (!frame->front_new) {
            frame->front_held = true;
            schedule->held++;
        }
    }
    if (overlay_active) {
        frame->layers[frame->layer_count++] = LIVE_LAYER_OVERLAY;
        schedule->overlay_frames++;
    }
}
