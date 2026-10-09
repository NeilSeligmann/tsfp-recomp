/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_frame_profile.h.
 */

#include "d3d8_frame_profile.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_frame.h"
#include "gpu_combiner.h"
#include "d3d8_gpu.h"
#include "d3d8_gpu_pgraph.h"
#include "d3d8_swap_replay.h"
#include "kernel_call.h"
#include "gpu_pgraph.h"

_Static_assert(D3D8_FRAME_PROFILE_UNHANDLED >= GPU_PGRAPH_UNHANDLED_TABLE,
               "the profile must list every distinct unhandled method the decoder holds (T441)");

#define HASH_SEED UINT64_C(0xCBF29CE484222325)
#define HASH_PRIME UINT64_C(0x100000001B3)
/* The snapshot pool one frame may hold. A frame needing more fails the decoder, loudly. */
#define VERTEX_BUDGET_BYTES (4u * 1024u * 1024u)

#define METHOD_PROGRAM_LOAD 0x1E9Cu
#define METHOD_PROGRAM_START 0x1EA0u
#define METHOD_DRAW_ARRAYS 0x1810u
#define METHOD_BEGIN_END 0x17FCu
#define METHOD_CLEAR 0x1D94u
#define METHOD_SEMAPHORE_RELEASE 0x1D70u

typedef struct {
    uint64_t value;
    bool used;
} digest_slot;

static pthread_mutex_t profile_lock = PTHREAD_MUTEX_INITIALIZER;
static bool profile_on;
static gpu_pgraph *pgraph;
static size_t decoded_next;
static uint64_t seen_reset_count;
static d3d8_frame_profile_summary summary;
static uint64_t previous_digest;
static uint64_t previous_normalised;
static uint64_t previous_content;
static d3d8_frame_profile_row previous_row;
static bool have_previous;
static digest_slot seen_full[D3D8_FRAME_PROFILE_DISTINCT];
static digest_slot seen_normalised[D3D8_FRAME_PROFILE_DISTINCT];
static digest_slot seen_vertex[D3D8_FRAME_PROFILE_DISTINCT];
static digest_slot seen_content[D3D8_FRAME_PROFILE_DISTINCT];
static uint32_t watch_addresses[D3D8_FRAME_PROFILE_WATCHES];
static size_t watch_address_count;
static d3d8_frame_profile_row ring[D3D8_FRAME_PROFILE_EDGE];
static d3d8_gpu_command kept[D3D8_FRAME_PROFILE_FRAME_COMMANDS];
static size_t kept_count;
static bool kept_valid;

static void hash_word(uint64_t *hash, uint32_t word)
{
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        *hash = (*hash ^ ((word >> shift) & 0xFFu)) * HASH_PRIME;
    }
}

/* Returns true when the digest was new. A full table counts the overflow and reports "not new". */
static bool remember(digest_slot *table, uint64_t digest)
{
    size_t index = (size_t)(digest % D3D8_FRAME_PROFILE_DISTINCT);
    for (size_t probe = 0u; probe < D3D8_FRAME_PROFILE_DISTINCT; probe++) {
        if (!table[index].used) {
            table[index].used = true;
            table[index].value = digest;
            return true;
        }
        if (table[index].value == digest) {
            return false;
        }
        index = (index + 1u) % D3D8_FRAME_PROFILE_DISTINCT;
    }
    summary.digest_overflow++;
    return false;
}

static void clear_state(void)
{
    memset(&summary, 0, sizeof(summary));
    summary.watch_count = watch_address_count;
    for (size_t index = 0u; index < watch_address_count; index++) {
        summary.watches[index].address = watch_addresses[index];
    }
    memset(seen_full, 0, sizeof(seen_full));
    memset(seen_normalised, 0, sizeof(seen_normalised));
    memset(seen_vertex, 0, sizeof(seen_vertex));
    memset(seen_content, 0, sizeof(seen_content));
    memset(ring, 0, sizeof(ring));
    memset(&previous_row, 0, sizeof(previous_row));
    previous_digest = 0u;
    previous_normalised = 0u;
    previous_content = 0u;
    have_previous = false;
    kept_count = 0u;
    kept_valid = false;
    decoded_next = 0u;
}

/* The observer runs at each kick, with the profile lock NOT held by the kicking thread, so it takes
 * the lock itself. It decodes what the recording holds that the model has not seen. */
static void decode_pending(void)
{
    if (summary.decoder_failed || pgraph == NULL) {
        return;
    }
    const gpu_pgraph_result result = d3d8_gpu_decode_recording(pgraph, &decoded_next);
    if (result != GPU_PGRAPH_OK) {
        summary.decoder_failed = true;
        (void)snprintf(summary.decoder_error, sizeof(summary.decoder_error), "%s at pair %llu: %s",
                       gpu_pgraph_result_string(result),
                       (unsigned long long)gpu_pgraph_get_stats(pgraph).pairs,
                       gpu_pgraph_error(pgraph));
    }
}

/* A CreateDevice reset empties the recording under the decoder. Start the model over. */
static void follow_gpu_reset(void)
{
    const uint64_t now = d3d8_gpu_reset_count();
    if (now == seen_reset_count) {
        return;
    }
    seen_reset_count = now;
    decoded_next = 0u;
    kept_count = 0u;
    kept_valid = false;
    if (pgraph != NULL) {
        gpu_pgraph_reset(pgraph);
    }
}

static void on_recorded(void)
{
    pthread_mutex_lock(&profile_lock);
    if (profile_on) {
        follow_gpu_reset();
        decode_pending();
    }
    pthread_mutex_unlock(&profile_lock);
}

bool d3d8_frame_profile_add_watch(uint32_t address)
{
    pthread_mutex_lock(&profile_lock);
    const bool ok = watch_address_count < D3D8_FRAME_PROFILE_WATCHES;
    if (ok) {
        watch_addresses[watch_address_count] = address;
        summary.watches[watch_address_count].address = address;
        watch_address_count++;
        summary.watch_count = watch_address_count;
    }
    pthread_mutex_unlock(&profile_lock);
    return ok;
}

static void sample_watches(void)
{
    for (size_t index = 0u; index < summary.watch_count; index++) {
        d3d8_frame_profile_watch *watch = &summary.watches[index];
        uint32_t value = 0u;
        if (!kernel_guest_read_u32(watch->address, &value)) {
            watch->unreadable++;
            continue;
        }
        if (watch->samples == 0u) {
            watch->first = value;
            watch->minimum = value;
            watch->maximum = value;
        } else if (value != watch->last) {
            watch->changes++;
            if (watch->logged < D3D8_FRAME_PROFILE_WATCH_LOG) {
                watch->change_sample[watch->logged] = watch->samples;
                watch->change_value[watch->logged] = value;
                watch->logged++;
            }
        }
        if (value < watch->minimum) watch->minimum = value;
        if (value > watch->maximum) watch->maximum = value;
        watch->last = value;
        watch->samples++;
    }
}

bool d3d8_frame_profile_enable(bool enabled)
{
    pthread_mutex_lock(&profile_lock);
    if (profile_on) {
        d3d8_gpu_set_recorded_observer(NULL);
        d3d8_present_set_observer(NULL);
        profile_on = false;
    }
    if (pgraph != NULL) {
        gpu_pgraph_destroy(pgraph);
        pgraph = NULL;
    }
    clear_state();
    bool ok = true;
    if (enabled) {
        ok = !d3d8_swap_replay_enabled();
        if (ok) {
            pgraph = gpu_pgraph_create();
            ok = pgraph != NULL;
        }
        if (ok) {
            gpu_pgraph_set_strict(pgraph, false);
            gpu_pgraph_set_combiner(pgraph, true);
            /* T462: the Swap copy composition's triangle is immediate vertex data, decoded here so its draw reaches the
             * census and the frame digests. T578: and the 2D engine blit CopyRects emits (subchannels 2 and 3), whose first
             * packet used to refuse the decoder. Nothing else is turned on: the other output groups stay unhandled methods. */
            gpu_pgraph_set_output_groups(pgraph, GPU_PGRAPH_OUTPUT_IMMEDIATE | GPU_PGRAPH_OUTPUT_BLIT);
            gpu_pgraph_set_vertex_capture(pgraph, d3d8_gpu_read_guest, NULL, VERTEX_BUDGET_BYTES);
            seen_reset_count = d3d8_gpu_reset_count();
            profile_on = true;
            d3d8_gpu_set_recorded_observer(on_recorded);
            d3d8_present_set_observer(d3d8_frame_profile_note);
        }
    }
    pthread_mutex_unlock(&profile_lock);
    return ok;
}

bool d3d8_frame_profile_enabled(void)
{
    pthread_mutex_lock(&profile_lock);
    const bool on = profile_on;
    pthread_mutex_unlock(&profile_lock);
    return on;
}

static void note_diff_method(uint32_t method, uint32_t before, uint32_t after)
{
    for (size_t index = 0u; index < summary.diff_method_count; index++) {
        if (summary.diff_methods[index].method == method) {
            summary.diff_methods[index].frames_differing++;
            return;
        }
    }
    if (summary.diff_method_count >= D3D8_FRAME_PROFILE_DIFF_METHODS) {
        summary.diff_methods_overflow++;
        return;
    }
    d3d8_frame_profile_diff_method *entry = &summary.diff_methods[summary.diff_method_count++];
    entry->method = method;
    entry->frames_differing = 1u;
    entry->example_before = before;
    entry->example_after = after;
}

/* Compare this frame's commands with the previous frame's. Each differing method is counted once per
 * frame pair, however many of its commands differ. */
static void diff_against_kept(size_t count)
{
    if (!kept_valid || kept_count != count) {
        if (kept_valid) {
            summary.pairs_length_differs++;
        }
        return;
    }
    summary.pairs_diffed++;
    uint32_t seen_methods[D3D8_FRAME_PROFILE_DIFF_METHODS];
    size_t seen_count = 0u;
    for (size_t index = 0u; index < count; index++) {
        const d3d8_gpu_command now = d3d8_gpu_stream_at(index);
        /* T391: the fence packet's data (see d3d8_frame_profile_note) is not a difference between frames. */
        const bool bookkeeping = now.subchannel != D3D8_GPU_SUBCHANNEL_3D || now.method == METHOD_SEMAPHORE_RELEASE;
        if (now.method == kept[index].method && now.subchannel == kept[index].subchannel &&
            (bookkeeping || now.data == kept[index].data)) {
            continue;
        }
        summary.commands_differing++;
        bool listed = false;
        for (size_t scan = 0u; scan < seen_count; scan++) {
            listed = listed || seen_methods[scan] == now.method;
        }
        if (listed) {
            continue;
        }
        if (seen_count < D3D8_FRAME_PROFILE_DIFF_METHODS) {
            seen_methods[seen_count++] = now.method;
        }
        note_diff_method(now.method, kept[index].data, now.data);
    }
}

/* Record one outcome in a census (T497): the totals, then the table of distinct outcomes (planned and refused, by text). */
static void census_tally(d3d8_frame_profile_census *census, bool planned, const char *label, uint64_t frame_number,
                         size_t draw_index, const gpu_pgraph_state *state)
{
    census->draws++;
    if (planned) {
        census->planned++;
    } else {
        census->refused++;
    }
    for (size_t index = 0u; index < census->outcome_count; index++) {
        d3d8_frame_profile_combiner_outcome *outcome = &census->outcomes[index];
        if (outcome->planned == planned && strcmp(label, outcome->text) == 0) {
            outcome->draws++;
            return;
        }
    }
    if (census->outcome_count == D3D8_FRAME_PROFILE_COMBINER_OUTCOMES) {
        census->overflow++;
        return;
    }
    d3d8_frame_profile_combiner_outcome *outcome = &census->outcomes[census->outcome_count++];
    outcome->planned = planned;
    outcome->draws = 1u;
    outcome->first_frame = frame_number;
    outcome->first_draw = (uint32_t)draw_index;
    (void)snprintf(outcome->text, sizeof(outcome->text), "%s", label);
    if (planned) {
        memcpy(outcome->words, state->combiner, sizeof(outcome->words));
    }
}

/* T497: the plan of this draw with ONE stand-in texel at stage 0, every inference allowed. The pixel is never read here. */
static void census_standin(uint64_t frame, size_t draw, const gpu_pgraph_state *state)
{
    static const uint8_t texel[4] = {0u, 0u, 0u, 0u};
    const gpu_combiner_texture standin[GPU_COMBINER_TEXTURE_STAGES] = {{texel, 1u, 1u, false, false, NULL, false, false}};
    gpu_combiner_plan plan;
    char error[160];
    error[0] = '\0';
    const gpu_pgraph_result planned =
        gpu_combiner_plan_build(state, GPU_COMBINER_INFER_ALL, standin, &plan, error, sizeof(error));
    char text[sizeof summary.standin_census.outcomes[0].text];
    if (planned == GPU_PGRAPH_OK) {
        (void)snprintf(text, sizeof(text), "%s: %u stage(s), texture stages 0x%X, inferences 0x%X", plan.name,
                       (unsigned)plan.stage_count, (unsigned)plan.texture_stages, (unsigned)plan.used_inferences);
    } else {
        (void)snprintf(text, sizeof(text), "%s", error);
    }
    census_tally(&summary.standin_census, planned == GPU_PGRAPH_OK, text, frame, draw, state);
}

/* T478: what the combiner would do with this draw. Every inference allowed and no test texture, so a refusal
 * is one no inference lifts (a texture read, fog, an untranslatable configuration) or a missing word. */
static void census_draw(uint64_t frame, size_t draw, const gpu_pgraph_draw *info)
{
    static const gpu_combiner_texture no_textures[GPU_COMBINER_TEXTURE_STAGES] = {{NULL, 0u, 0u, false, false, NULL, false, false}};
    const gpu_pgraph_state *state = gpu_pgraph_snapshot(pgraph, info->snapshot);
    if (state == NULL) {
        return;
    }
    census_standin(frame, draw, state);
    gpu_combiner_plan plan;
    char error[160];
    error[0] = '\0';
    const gpu_pgraph_result planned = gpu_combiner_plan_build(state, GPU_COMBINER_INFER_ALL, no_textures, &plan,
                                                              error, sizeof(error));
    char text[sizeof summary.combiner_outcomes[0].text];
    if (planned == GPU_PGRAPH_OK) {
        (void)snprintf(text, sizeof(text), "%s: %u stage(s), texture stages 0x%X, inferences 0x%X", plan.name,
                       (unsigned)plan.stage_count, (unsigned)plan.texture_stages, (unsigned)plan.used_inferences);
    } else {
        (void)snprintf(text, sizeof(text), "%s", error);
    }
    summary.combiner_draws++;
    if (planned == GPU_PGRAPH_OK) {
        summary.combiner_planned++;
    } else {
        summary.combiner_refused++;
    }
    for (size_t index = 0u; index < summary.combiner_outcome_count; index++) {
        d3d8_frame_profile_combiner_outcome *outcome = &summary.combiner_outcomes[index];
        if (outcome->planned == (planned == GPU_PGRAPH_OK) && strcmp(outcome->text, text) == 0) {
            outcome->draws++;
            return;
        }
    }
    if (summary.combiner_outcome_count == D3D8_FRAME_PROFILE_COMBINER_OUTCOMES) {
        summary.combiner_overflow++;
        return;
    }
    d3d8_frame_profile_combiner_outcome *outcome = &summary.combiner_outcomes[summary.combiner_outcome_count++];
    outcome->planned = planned == GPU_PGRAPH_OK;
    outcome->draws = 1u;
    outcome->first_frame = frame;
    outcome->first_draw = (uint32_t)draw;
    memcpy(outcome->text, text, sizeof(outcome->text));
}

void d3d8_frame_profile_note(const d3d8_frame_record *record)
{
    pthread_mutex_lock(&profile_lock);
    if (!profile_on) {
        pthread_mutex_unlock(&profile_lock);
        return;
    }
    follow_gpu_reset();
    decode_pending();
    sample_watches();
    const size_t count = d3d8_gpu_stream_count();

    uint64_t full = HASH_SEED;
    uint64_t normalised = HASH_SEED;
    uint64_t draws = 0u;
    uint64_t clears = 0u;
    for (size_t index = 0u; index < count; index++) {
        const d3d8_gpu_command command = d3d8_gpu_stream_at(index);
        uint32_t masked = command.data;
        if (command.method == METHOD_PROGRAM_LOAD || command.method == METHOD_PROGRAM_START) {
            masked = 0u;
        } else if (command.method == METHOD_DRAW_ARRAYS) {
            masked = command.data & 0xFF000000u;
        }
        /* T391: the fence packet's data is bookkeeping that changes every frame by construction: the subchannel-5
         * notification holds the ring cursor (a host address, so it differs between two boots) and the fence, and
         * SEMAPHORE_RELEASE the fence value. The notification's data is left out of both digests, the fence value
         * out of the normalised one. A command on another subchannel mixes the subchannel in, so it never equals
         * a 3D command of the same method, and 3D commands hash exactly as before. */
        uint32_t full_data = command.data;
        if (command.subchannel != D3D8_GPU_SUBCHANNEL_3D) {
            full_data = 0u;
            masked = 0u;
        } else if (command.method == METHOD_SEMAPHORE_RELEASE) {
            masked = 0u;
        }
        hash_word(&full, command.method);
        hash_word(&full, full_data);
        hash_word(&normalised, command.method);
        hash_word(&normalised, masked);
        if (command.subchannel != D3D8_GPU_SUBCHANNEL_3D) {
            hash_word(&full, command.subchannel);
            hash_word(&normalised, command.subchannel);
            continue;
        }
        if (command.method == METHOD_BEGIN_END && command.data != 0u) {
            draws++;
        }
        if (command.method == METHOD_CLEAR) {
            clears++;
        }
    }

    uint64_t vertex = HASH_SEED;
    if (!summary.decoder_failed) {
        const size_t decoded_draws = gpu_pgraph_draw_count(pgraph);
        for (size_t draw = 0u; draw < decoded_draws; draw++) {
            const gpu_pgraph_draw *info = gpu_pgraph_draw_at(pgraph, draw);
            census_draw(record->number, draw, info);
            hash_word(&vertex, info->primitive);
            hash_word(&vertex, info->index_count);
            if (!info->vertices_captured) {
                summary.draws_uncaptured++;
                continue;
            }
            for (uint32_t slot = 0u; slot < GPU_PGRAPH_ATTRIBUTES; slot++) {
                uint32_t address = 0u;
                uint32_t length = 0u;
                const uint8_t *bytes = gpu_pgraph_draw_vertex_bytes(pgraph, draw, slot, &address, &length);
                hash_word(&vertex, slot);
                hash_word(&vertex, length);
                for (uint32_t offset = 0u; bytes != NULL && offset < length; offset++) {
                    vertex = (vertex ^ bytes[offset]) * HASH_PRIME;
                }
            }
        }
    }
    uint64_t content = HASH_SEED;
    hash_word(&content, (uint32_t)normalised);
    hash_word(&content, (uint32_t)(normalised >> 32));
    hash_word(&content, (uint32_t)vertex);
    hash_word(&content, (uint32_t)(vertex >> 32));

    const d3d8_gpu_stats gpu = d3d8_gpu_get_stats();
    const d3d8_frame_profile_row row = {
        .number = record->number,
        .swap_counter = record->swap_counter,
        .vblank = record->vblank,
        .digest = full,
        .normalised = normalised,
        .vertex = vertex,
        .commands = count,
        .draws = draws,
        .clears = clears,
        .fences_inserted = gpu.fences_inserted,
        .kicks = gpu.kicks,
    };
    if (have_previous) {
        if (row.swap_counter == previous_row.swap_counter + 1u) {
            summary.swap_step_one++;
        } else {
            summary.swap_step_other++;
        }
        if (row.vblank == previous_row.vblank + 1u) {
            summary.vblank_step_one++;
        } else if (row.vblank == previous_row.vblank) {
            summary.vblank_step_zero++;
        } else if (row.vblank > previous_row.vblank) {
            summary.vblank_step_more++;
        } else {
            summary.vblank_fell++;
        }
        if (full == previous_digest) {
            summary.same_as_previous++;
        }
        if (normalised == previous_normalised) {
            summary.normalised_same_as_previous++;
        }
        if (content == previous_content) {
            summary.content_same_as_previous++;
        } else {
            summary.content_last_change = row.number;
            if (summary.content_changes < D3D8_FRAME_PROFILE_CHANGES) {
                summary.content_change_frames[summary.content_changes] = row.number;
            }
            summary.content_changes++;
            const uint64_t bucket = (row.number - 1u) / D3D8_FRAME_PROFILE_BUCKET_FRAMES;
            if (bucket < D3D8_FRAME_PROFILE_BUCKETS) {
                summary.content_changes_per_bucket[bucket]++;
            }
        }
    }
    if (remember(seen_full, full)) summary.distinct_digests++;
    if (remember(seen_normalised, normalised)) summary.distinct_normalised++;
    if (remember(seen_vertex, vertex)) summary.distinct_vertex++;
    if (remember(seen_content, content)) {
        summary.distinct_content++;
        summary.content_last_new = row.number;
    }
    if (summary.frames == 0u) {
        summary.commands_min = row.commands;
        summary.commands_max = row.commands;
        summary.draws_min = row.draws;
        summary.draws_max = row.draws;
    }
    if (row.commands < summary.commands_min) summary.commands_min = row.commands;
    if (row.commands > summary.commands_max) summary.commands_max = row.commands;
    if (row.draws < summary.draws_min) summary.draws_min = row.draws;
    if (row.draws > summary.draws_max) summary.draws_max = row.draws;
    summary.commands_total += row.commands;
    summary.draws_total += row.draws;
    summary.clears_total += row.clears;
    if (summary.first_row_count < D3D8_FRAME_PROFILE_EDGE) {
        summary.first[summary.first_row_count++] = row;
    }
    ring[summary.frames % D3D8_FRAME_PROFILE_EDGE] = row;
    summary.frames++;

    /* Keep this frame's commands for the next diff, then release them from the bounded recording. */
    diff_against_kept(count);
    if (count <= D3D8_FRAME_PROFILE_FRAME_COMMANDS) {
        for (size_t index = 0u; index < count; index++) {
            kept[index] = d3d8_gpu_stream_at(index);
        }
        kept_count = count;
        kept_valid = true;
    } else {
        kept_valid = false;
        summary.pairs_not_kept++;
    }
    previous_digest = full;
    previous_normalised = normalised;
    previous_content = content;
    previous_row = row;
    have_previous = true;
    if (!summary.decoder_failed && gpu_pgraph_begin_frame(pgraph) != GPU_PGRAPH_OK) {
        summary.decoder_failed = true;
        (void)snprintf(summary.decoder_error, sizeof(summary.decoder_error),
                       "begin_frame refused: %s", gpu_pgraph_error(pgraph));
    }
    d3d8_gpu_stream_discard(count);
    decoded_next = decoded_next > count ? decoded_next - count : 0u;
    pthread_mutex_unlock(&profile_lock);
}

d3d8_frame_profile_summary d3d8_frame_profile_get(void)
{
    pthread_mutex_lock(&profile_lock);
    d3d8_frame_profile_summary copy = summary;
    const uint64_t held = summary.frames < D3D8_FRAME_PROFILE_EDGE ? summary.frames
                                                                    : D3D8_FRAME_PROFILE_EDGE;
    copy.unhandled_count = 0u;
    if (pgraph != NULL) {
        const size_t distinct = gpu_pgraph_unhandled_count(pgraph);
        for (size_t index = 0u; index < distinct && index < D3D8_FRAME_PROFILE_UNHANDLED; index++) {
            gpu_pgraph_unhandled_at(pgraph, index, &copy.unhandled_method[index],
                                    &copy.unhandled_pairs[index]);
            copy.unhandled_count++;
        }
        copy.unhandled_overflow = gpu_pgraph_unhandled_overflow(pgraph);
    }
    copy.last_row_count = held;
    for (uint64_t index = 0u; index < held; index++) {
        copy.last[index] = ring[(summary.frames - held + index) % D3D8_FRAME_PROFILE_EDGE];
    }
    pthread_mutex_unlock(&profile_lock);
    return copy;
}

static void print_row(FILE *out, const d3d8_frame_profile_row *row)
{
    fprintf(out, "  frame %6llu  swap counter %u  vblank %llu  full %016llx  norm %016llx  vertex %016llx  "
                 "commands %llu  draws %llu  clears %llu  fences %llu  kicks %llu\n",
            (unsigned long long)row->number, (unsigned)row->swap_counter,
            (unsigned long long)row->vblank, (unsigned long long)row->digest,
            (unsigned long long)row->normalised, (unsigned long long)row->vertex,
            (unsigned long long)row->commands, (unsigned long long)row->draws,
            (unsigned long long)row->clears, (unsigned long long)row->fences_inserted,
            (unsigned long long)row->kicks);
}

void d3d8_frame_profile_report(FILE *out)
{
    const d3d8_frame_profile_summary data = d3d8_frame_profile_get();
    const d3d8_gpu_stats gpu = d3d8_gpu_get_stats();
    fprintf(out, "\n--- frame profile (T422, every present, command and vertex digests) ---\n");
    fprintf(out, "presents %llu (the end of each Swap(4)); GPU model: kicks %llu, fences inserted "
                 "%llu, fence waits %llu (%llu blocked), vblanks %llu, commands recorded %llu "
                 "(dropped past the bound %llu, released by the profile %llu), %llu library emission(s) elided "
                 "by the ports\n",
            (unsigned long long)data.frames, (unsigned long long)gpu.kicks,
            (unsigned long long)gpu.fences_inserted, (unsigned long long)gpu.fence_waits,
            (unsigned long long)gpu.fence_waits_blocked, (unsigned long long)gpu.vblanks,
            (unsigned long long)gpu.commands_recorded, (unsigned long long)gpu.commands_dropped,
            (unsigned long long)gpu.commands_discarded,
            (unsigned long long)gpu.emissions_elided);
    fprintf(out, "Clear: %llu call(s), %llu with a non-zero colour, %llu CLEAR_SURFACE command(s) in the "
                 "recorded stream (%llu counted by the consumer, one per clipped rectangle, "
                 "docs/d3d8-usage.md 13.6)\n",
            (unsigned long long)d3d8_clear_count(), (unsigned long long)d3d8_clear_nonblack_count(),
            (unsigned long long)data.clears_total, (unsigned long long)gpu.clear_surfaces);
    fprintf(out, "Subchannels (T391): %llu recorded command(s) on a subchannel other than 0, the fence packets' "
                 "software method 0x310 on subchannel 5, counted and never replayed\n",
            (unsigned long long)gpu.commands_other_subchannel);
    pthread_mutex_lock(&profile_lock);
    const gpu_pgraph_stats model = pgraph != NULL ? gpu_pgraph_get_stats(pgraph) : (gpu_pgraph_stats){0};
    const bool model_held = pgraph != NULL;
    pthread_mutex_unlock(&profile_lock);
    if (model_held) {
        fprintf(out, "2D engine blits (T578): %llu image blit(s) decoded (the CopyRects packets on subchannels 2 and 3), "
                     "%llu of them empty\n",
                (unsigned long long)model.copies, (unsigned long long)model.copies_empty);
    }
    if (data.decoder_failed) {
        fprintf(out, "DECODER REFUSED, vertex digests are not meaningful after it: %s\n",
                data.decoder_error);
    }
    if (data.frames == 0u) {
        fprintf(out, "no present reached\n");
        return;
    }
    fprintf(out, "swap counter: +1 %llu, other %llu\n", (unsigned long long)data.swap_step_one,
            (unsigned long long)data.swap_step_other);
    fprintf(out, "vblank: +1 %llu, unchanged %llu, +more %llu, fell %llu\n",
            (unsigned long long)data.vblank_step_one, (unsigned long long)data.vblank_step_zero,
            (unsigned long long)data.vblank_step_more, (unsigned long long)data.vblank_fell);
    fprintf(out, "distinct over %llu frames: full %llu, normalised %llu, vertex %llu, content "
                 "(normalised and vertex) %llu, %llu not remembered\n",
            (unsigned long long)data.frames, (unsigned long long)data.distinct_digests,
            (unsigned long long)data.distinct_normalised, (unsigned long long)data.distinct_vertex,
            (unsigned long long)data.distinct_content, (unsigned long long)data.digest_overflow);
    fprintf(out, "equal to the previous frame: full %llu, normalised %llu, content %llu; the content "
                 "last changed at frame %llu\n",
            (unsigned long long)data.same_as_previous,
            (unsigned long long)data.normalised_same_as_previous,
            (unsigned long long)data.content_same_as_previous,
            (unsigned long long)data.content_last_change);
    fprintf(out, "frames whose content differed from the previous frame (%llu in all):",
            (unsigned long long)data.content_changes);
    for (uint64_t index = 0u; index < data.content_changes && index < D3D8_FRAME_PROFILE_CHANGES;
         index++) {
        fprintf(out, " %llu", (unsigned long long)data.content_change_frames[index]);
    }
    fprintf(out, "\n");
    fprintf(out, "the last frame to show a content digest not seen before: %llu (a run past it only "
                 "repeats frames already seen)\n", (unsigned long long)data.content_last_new);
    fprintf(out, "content changes per %u frames:", (unsigned)D3D8_FRAME_PROFILE_BUCKET_FRAMES);
    for (uint64_t bucket = 0u; bucket < D3D8_FRAME_PROFILE_BUCKETS &&
                               bucket * D3D8_FRAME_PROFILE_BUCKET_FRAMES < data.frames; bucket++) {
        fprintf(out, " %llu", (unsigned long long)data.content_changes_per_bucket[bucket]);
    }
    fprintf(out, "\n");
    fprintf(out, "commands per frame: min %llu max %llu total %llu; draws per frame: min %llu max "
                 "%llu total %llu (%llu uncaptured); clears total %llu\n",
            (unsigned long long)data.commands_min, (unsigned long long)data.commands_max,
            (unsigned long long)data.commands_total, (unsigned long long)data.draws_min,
            (unsigned long long)data.draws_max, (unsigned long long)data.draws_total,
            (unsigned long long)data.draws_uncaptured, (unsigned long long)data.clears_total);
    fprintf(out, "combiner census (T478, every inference allowed, no test texture): %llu draw(s), %llu with a plan, %llu "
                 "refused, %zu distinct outcome(s)%s\n",
            (unsigned long long)data.combiner_draws, (unsigned long long)data.combiner_planned,
            (unsigned long long)data.combiner_refused, data.combiner_outcome_count,
            data.combiner_overflow != 0u ? " (more beyond the table)" : "");
    for (size_t index = 0u; index < data.combiner_outcome_count; index++) {
        const d3d8_frame_profile_combiner_outcome *outcome = &data.combiner_outcomes[index];
        fprintf(out, "  combiner %s x%llu, first at frame %llu draw %u: %s\n", outcome->planned ? "plan" : "REFUSED",
                (unsigned long long)outcome->draws, (unsigned long long)outcome->first_frame,
                (unsigned)outcome->first_draw, outcome->text);
    }
    fprintf(out, "combiner census with a STAND-IN texture at stage 0 (T497, NOT the title's texture, every inference "
                 "allowed): %llu draw(s), %llu with a plan, %llu refused, %zu distinct outcome(s)%s\n",
            (unsigned long long)data.standin_census.draws, (unsigned long long)data.standin_census.planned,
            (unsigned long long)data.standin_census.refused, data.standin_census.outcome_count,
            data.standin_census.overflow != 0u ? " (more beyond the table)" : "");
    for (size_t index = 0u; index < data.standin_census.outcome_count; index++) {
        const d3d8_frame_profile_combiner_outcome *outcome = &data.standin_census.outcomes[index];
        fprintf(out, "  stand-in %s x%llu, first at frame %llu draw %u: %s\n", outcome->planned ? "plan" : "REFUSED",
                (unsigned long long)outcome->draws, (unsigned long long)outcome->first_frame,
                (unsigned)outcome->first_draw, outcome->text);
    }
    /* the definitions behind the planned outcomes, so a tool can make the combiner modules the loop needs (T497) */
    for (size_t index = 0u; index < data.standin_census.outcome_count; index++) {
        const d3d8_frame_profile_combiner_outcome *outcome = &data.standin_census.outcomes[index];
        if (!outcome->planned) {
            continue;
        }
        char name[GPU_COMBINER_NAME_BYTES];
        (void)snprintf(name, sizeof(name), "%.*s", (int)strcspn(outcome->text, ":"), outcome->text);
        fprintf(out, "  stand-in definition %s", name);
        for (size_t word = 0u; word < GPU_PGRAPH_COMBINER_WORDS; word++) {
            fprintf(out, " %08X", (unsigned)outcome->words[word]);
        }
        fprintf(out, "\n");
    }
    fprintf(out, "frame to frame diff: %llu pairs compared command by command, %llu differed in "
                 "length, %llu not kept, %llu differing commands in all\n",
            (unsigned long long)data.pairs_diffed, (unsigned long long)data.pairs_length_differs,
            (unsigned long long)data.pairs_not_kept, (unsigned long long)data.commands_differing);
    for (size_t index = 0u; index < data.diff_method_count; index++) {
        const d3d8_frame_profile_diff_method *entry = &data.diff_methods[index];
        fprintf(out, "  method %04X differed in %llu pair(s), first seen %08X -> %08X\n",
                (unsigned)entry->method, (unsigned long long)entry->frames_differing,
                (unsigned)entry->example_before, (unsigned)entry->example_after);
    }
    if (data.diff_methods_overflow != 0u) {
        fprintf(out, "  %llu differing method(s) beyond the table\n",
                (unsigned long long)data.diff_methods_overflow);
    }
    fprintf(out, "methods the decoder does not interpret (a strict replay would refuse the frame): %zu",
            data.unhandled_count);
    for (size_t index = 0u; index < data.unhandled_count; index++) {
        fprintf(out, "%s %04X x%llu", index == 0u ? ":" : ",", (unsigned)data.unhandled_method[index],
                (unsigned long long)data.unhandled_pairs[index]);
    }
    if (data.unhandled_overflow != 0u) {
        fprintf(out, ", %llu more pair(s) beyond the table", (unsigned long long)data.unhandled_overflow);
    }
    fprintf(out, "\n");
    for (size_t index = 0u; index < data.watch_count; index++) {
        const d3d8_frame_profile_watch *watch = &data.watches[index];
        fprintf(out, "watch [0x%08X]: %llu sample(s), first %u last %u min %u max %u, changed at %llu "
                     "present(s)%s\n",
                (unsigned)watch->address, (unsigned long long)watch->samples,
                (unsigned)watch->first, (unsigned)watch->last, (unsigned)watch->minimum,
                (unsigned)watch->maximum, (unsigned long long)watch->changes,
                watch->unreadable != 0u ? ", SOME READS FAILED" : "");
        if (watch->logged != 0u) {
            fprintf(out, "watch [0x%08X] changes (present index -> value), first %zu of %llu:", (unsigned)watch->address,
                    watch->logged, (unsigned long long)watch->changes);
            for (size_t change = 0u; change < watch->logged; change++) {
                fprintf(out, " %llu->%u", (unsigned long long)watch->change_sample[change],
                        (unsigned)watch->change_value[change]);
            }
            fprintf(out, "\n");
        }
    }
    fprintf(out, "first %llu frame(s):\n", (unsigned long long)data.first_row_count);
    for (uint64_t index = 0u; index < data.first_row_count; index++) {
        print_row(out, &data.first[index]);
    }
    fprintf(out, "last %llu frame(s):\n", (unsigned long long)data.last_row_count);
    for (uint64_t index = 0u; index < data.last_row_count; index++) {
        print_row(out, &data.last[index]);
    }
}
