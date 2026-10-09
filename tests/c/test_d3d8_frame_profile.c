/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T422: the per-present frame profile, src/gpu/d3d8_frame_profile.c.
 *
 * Five synthetic frames are written through the real pushbuffer and kicked, so the GPU model
 * records them and the profile takes the recording over its observer:
 *
 *   F1  program slot 1, draw start index 0, vertex memory A
 *   F2  program slot 4, draw start index 3, vertex memory A   only the allocator-moved fields
 *       differ from F1 (the program slot pair and the draw start index)
 *   F3  program slot 7, draw start index 6, vertex memory B   the same, and the VERTEX BYTES differ
 *   F4  as F3 with one more command (an unhandled method 0x0300)
 *   F5  exactly F4
 *   F6  as F5 with an eight vertex draw
 *
 * Each frame also writes the program load pair twice, so one frame pair has two differing commands
 * of one method and the diff must still count the method once.
 *
 * Every expectation below is derived by hand from that table, not read off the code. Synthetic
 * guest memory, not original-XBE evidence. Each group asserts something was counted before it
 * compares (an empty summary equals an empty expectation).
 */
#include "test_d3d8_support.h"

#include "d3d8_frame_profile.h"
#include "d3d8_gpu.h"
#include "gpu_combiner_words.h"
#include "gpu_standin_words.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_test_support.h"

#define ARENA 0x00D00000u
#define VERTEX_MEMORY (ARENA + 0x2000u)
#define WATCHED (ARENA + 0x4000u)

static void write_pairs(const uint32_t *words, uint32_t count)
{
    uint32_t cursor = d3d8_pushbuffer_begin();
    for (uint32_t i = 0u; i < count; i++) {
        d3d8_guest_store32(cursor + i * 4u, words[i]);
    }
    d3d8_pushbuffer_end(cursor + count * 4u);
}

/* One frame: program slot pair, format run, array offset, a four vertex draw from slot - 1. */
static void write_frame(uint32_t slot, uint32_t vertex_word, bool extra_command, uint32_t vertices)
{
    for (uint32_t i = 0u; i < 64u; i++) {
        store(VERTEX_MEMORY + i * 4u, vertex_word);
    }
    d3d8_pushbuffer_emit_pair(0x00041E9Cu, slot);
    d3d8_pushbuffer_emit_pair(0x00041E9Cu, slot);
    d3d8_pushbuffer_emit_pair(0x00041EA0u, slot);
    uint32_t formats[17];
    formats[0] = 0x00401760u;
    for (uint32_t index = 0u; index < 16u; index++) {
        formats[1u + index] = index == 1u ? (12u << 8) + 0x32u : 2u;
    }
    write_pairs(formats, 17u);
    d3d8_pushbuffer_emit_pair(0x00041724u, VERTEX_MEMORY);
    if (extra_command) {
        d3d8_pushbuffer_emit_pair(0x00040300u, 5u);
    }
    /* BEGIN_END(5), one DRAW_ARRAYS of `vertices` vertices from slot - 1, BEGIN_END(0). */
    const uint32_t words[6] = {0x000417FCu, 5u, 0x40001810u + (1u << 18), ((vertices - 1u) << 24) | (slot - 1u),
                               0x000417FCu, 0u};
    write_pairs(words, 6u);
    d3d8_gpu_kick();
}

static d3d8_frame_record record(uint64_t number, uint32_t swap_counter, uint64_t vblank)
{
    d3d8_frame_record result;
    memset(&result, 0, sizeof(result));
    result.number = number;
    result.swap_counter = swap_counter;
    result.vblank = vblank;
    return result;
}

static const d3d8_frame_profile_diff_method *diff_entry(const d3d8_frame_profile_summary *data,
                                                         uint32_t method)
{
    for (size_t index = 0u; index < data->diff_method_count; index++) {
        if (data->diff_methods[index].method == method) {
            return &data->diff_methods[index];
        }
    }
    return NULL;
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(ARENA, 0x10000u);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    store(0x003E3F58u, D3D8_DEVICE_BASE);
    CHECK(d3d8_gpu_create());
    CHECK(d3d8_pushbuffer_create());
    d3d8_device_store32(D3D8_DEV_FLAGS, 0x4203u);

    /* Off by default: a present is not noted and the recording is left alone. */
    CHECK(!d3d8_frame_profile_enabled());
    write_frame(1u, 0xA0A0A0A0u, false, 4u);
    const uint64_t recorded_before = d3d8_gpu_get_stats().commands_recorded;
    CHECK(recorded_before != 0u);
    const d3d8_frame_record idle = record(1u, 1u, 1u);
    d3d8_frame_profile_note(&idle);
    CHECK_EQ_U32(d3d8_frame_profile_get().frames, 0u);
    CHECK_EQ_U32(d3d8_gpu_stream_count(), recorded_before);
    CHECK_EQ_U32(d3d8_gpu_get_stats().commands_discarded, 0u);
    d3d8_gpu_stream_discard(d3d8_gpu_stream_count());

    CHECK(d3d8_frame_profile_enable(true));
    CHECK(d3d8_frame_profile_enabled());
    CHECK(d3d8_frame_profile_add_watch(WATCHED));

    const struct {
        uint32_t slot;
        uint32_t vertex_word;
        bool extra;
        uint32_t vertices;
        uint32_t swap_counter;
        uint64_t vblank;
        uint32_t watched;
    } frames[6] = {
        {1u, 0xA0A0A0A0u, false, 4u, 1u, 10u, 7u},
        {4u, 0xA0A0A0A0u, false, 4u, 2u, 11u, 7u},
        {7u, 0xB1B1B1B1u, false, 4u, 3u, 11u, 9u},
        {7u, 0xB1B1B1B1u, true, 4u, 5u, 13u, 9u},
        {7u, 0xB1B1B1B1u, true, 4u, 6u, 14u, 9u},
        {7u, 0xB1B1B1B1u, true, 8u, 7u, 15u, 9u},
    };
    for (uint32_t index = 0u; index < 6u; index++) {
        store(WATCHED, frames[index].watched);
        write_frame(frames[index].slot, frames[index].vertex_word, frames[index].extra,
                    frames[index].vertices);
        const d3d8_frame_record present =
            record(index + 1u, frames[index].swap_counter, frames[index].vblank);
        d3d8_frame_profile_note(&present);
        /* Each present releases the frame it summarised. */
        CHECK_EQ_U32(d3d8_gpu_stream_count(), 0u);
    }

    const d3d8_frame_profile_summary data = d3d8_frame_profile_get();
    CHECK(data.frames != 0u);
    CHECK_EQ_U32(data.frames, 6u);
    CHECK(!data.decoder_failed);
    if (data.decoder_failed) {
        printf("  decoder: %s\n", data.decoder_error);
    }

    /* Steps: swap counter 1,2,3,5,6,7 rose by one four times and otherwise once. */
    CHECK_EQ_U32(data.swap_step_one, 4u);
    CHECK_EQ_U32(data.swap_step_other, 1u);
    /* vblank 10,11,11,13,14,15: +1 three times, unchanged once, more than one once, never fell. */
    CHECK_EQ_U32(data.vblank_step_one, 3u);
    CHECK_EQ_U32(data.vblank_step_zero, 1u);
    CHECK_EQ_U32(data.vblank_step_more, 1u);
    CHECK_EQ_U32(data.vblank_fell, 0u);

    /* Digests. Normalised: F1 to F3 one value, F4 and F5 another, F6 a third (its draw is longer, and
     * the draw's vertex count is not masked). Vertex: A for F1 and F2, B for F3 to F5, a third for F6.
     * Content pairs the two. Full: F5 repeats F4 exactly, every other frame differs. */
    CHECK_EQ_U32(data.distinct_normalised, 3u);
    CHECK_EQ_U32(data.distinct_vertex, 3u);
    CHECK_EQ_U32(data.distinct_content, 4u);
    CHECK_EQ_U32(data.distinct_digests, 5u);
    CHECK_EQ_U32(data.digest_overflow, 0u);
    CHECK_EQ_U32(data.same_as_previous, 1u);
    CHECK_EQ_U32(data.normalised_same_as_previous, 3u);
    CHECK_EQ_U32(data.content_same_as_previous, 2u);
    CHECK_EQ_U32(data.content_changes, 3u);
    CHECK_EQ_U32(data.content_change_frames[0], 3u);
    CHECK_EQ_U32(data.content_change_frames[1], 4u);
    CHECK_EQ_U32(data.content_change_frames[2], 6u);
    CHECK_EQ_U32(data.content_last_change, 6u);
    CHECK_EQ_U32(data.content_last_new, 6u);
    CHECK(data.first[0].digest != data.first[1].digest);
    CHECK(data.first[0].normalised == data.first[1].normalised);
    CHECK(data.first[1].vertex == data.first[0].vertex);
    CHECK(data.first[2].vertex != data.first[1].vertex);
    CHECK(data.first[3].normalised != data.first[2].normalised);
    CHECK(data.first[4].digest == data.first[3].digest);
    CHECK(data.first[5].normalised != data.first[4].normalised);

    /* The one method the decoder does not interpret is the extra command, in F4, F5 and F6. */
    CHECK_EQ_U32(data.unhandled_count, 1u);
    CHECK_EQ_U32(data.unhandled_method[0], 0x0300u);
    CHECK_EQ_U32(data.unhandled_pairs[0], 3u);
    CHECK_EQ_U32(data.unhandled_overflow, 0u);

    /* One draw per frame, its vertex bytes captured. */
    CHECK_EQ_U32(data.draws_total, 6u);
    CHECK_EQ_U32(data.draws_min, 1u);
    CHECK_EQ_U32(data.draws_max, 1u);
    CHECK_EQ_U32(data.draws_uncaptured, 0u);
    CHECK_EQ_U32(data.clears_total, 0u);
    CHECK_EQ_U32(data.commands_max, data.commands_min + 1u);

    /* The diff: F1 to F2, F2 to F3, F4 to F5 (identical) and F5 to F6 are the same length, F3 to F4
     * is one command longer. F1 to F2 and F2 to F3 differ in the program load pair (twice), the start
     * pair and the draw, F5 to F6 in the draw only: 4 + 4 + 1 commands, and each method counts once per
     * frame pair. */
    CHECK_EQ_U32(data.pairs_diffed, 4u);
    CHECK_EQ_U32(data.pairs_length_differs, 1u);
    CHECK_EQ_U32(data.pairs_not_kept, 0u);
    CHECK_EQ_U32(data.commands_differing, 9u);
    CHECK_EQ_U32(data.diff_method_count, 3u);
    const d3d8_frame_profile_diff_method *load = diff_entry(&data, 0x1E9Cu);
    const d3d8_frame_profile_diff_method *start = diff_entry(&data, 0x1EA0u);
    const d3d8_frame_profile_diff_method *draw = diff_entry(&data, 0x1810u);
    CHECK(load != NULL && start != NULL && draw != NULL);
    if (load != NULL && start != NULL && draw != NULL) {
        CHECK_EQ_U32(load->frames_differing, 2u);
        CHECK_EQ_U32(start->frames_differing, 2u);
        CHECK_EQ_U32(draw->frames_differing, 3u);
        CHECK_EQ_U32(load->example_before, 1u);
        CHECK_EQ_U32(load->example_after, 4u);
        CHECK_EQ_U32(draw->example_before, 3u << 24);
        CHECK_EQ_U32(draw->example_after, (3u << 24) | 3u);
    }
    CHECK(diff_entry(&data, 0x0300u) == NULL);

    /* The watch saw the guest word go 7, 7, 9, 9, 9. */
    CHECK_EQ_U32(data.watch_count, 1u);
    CHECK_EQ_U32(data.watches[0].samples, 6u);
    CHECK_EQ_U32(data.watches[0].first, 7u);
    CHECK_EQ_U32(data.watches[0].last, 9u);
    CHECK_EQ_U32(data.watches[0].minimum, 7u);
    CHECK_EQ_U32(data.watches[0].maximum, 9u);
    CHECK_EQ_U32(data.watches[0].changes, 1u);
    /* T636: the change is logged with its present index (the third sample, index 2) and the new value. */
    CHECK_EQ_U32(data.watches[0].logged, 1u);
    CHECK_EQ_U32(data.watches[0].change_sample[0], 2u);
    CHECK_EQ_U32(data.watches[0].change_value[0], 9u);

    /* The watch list is bounded at eight. */
    for (unsigned extra = 1u; extra < D3D8_FRAME_PROFILE_WATCHES; extra++) {
        CHECK(d3d8_frame_profile_add_watch(WATCHED));
    }
    CHECK(!d3d8_frame_profile_add_watch(WATCHED));
    CHECK_EQ_U32(d3d8_frame_profile_get().watch_count, D3D8_FRAME_PROFILE_WATCHES);

    /* Every recorded command was released, none dropped. */
    const d3d8_gpu_stats gpu = d3d8_gpu_get_stats();
    CHECK(gpu.commands_recorded != 0u);
    /* The one before the profile was released by hand above, the rest by the profile. */
    CHECK_EQ_U32(gpu.commands_discarded, gpu.commands_recorded);
    CHECK_EQ_U32(gpu.commands_dropped, 0u);
    CHECK_EQ_U32(data.commands_total, gpu.commands_recorded - recorded_before);

    /* The report names what it measured. */
    FILE *file = tmpfile();
    CHECK(file != NULL);
    if (file != NULL) {
        d3d8_frame_profile_report(file);
        fflush(file);
        rewind(file);
        char text[8192];
        const size_t length = fread(text, 1u, sizeof(text) - 1u, file);
        text[length] = '\0';
        fclose(file);
        CHECK(length != 0u);
        CHECK(strstr(text, "presents 6") != NULL);
        CHECK(strstr(text, "distinct over 6 frames: full 5, normalised 3, vertex 3, content") != NULL);
        CHECK(strstr(text, "method 1E9C differed in 2 pair(s)") != NULL);
        CHECK(strstr(text, "method 1810 differed in 3 pair(s)") != NULL);
        CHECK(strstr(text, "does not interpret (a strict replay would refuse the frame): 1: 0300 x3") != NULL);
        CHECK(strstr(text, "watch [0x00D04000]") != NULL);
        CHECK(strstr(text, "watch [0x00D04000] changes (present index -> value), first 1 of 1: 2->9") != NULL);
    }

    /* T478: the combiner census tallies what gpu_combiner_plan_build says of EVERY decoded draw, without stopping
     * at the first refusal. Frames 1 to 6 wrote no combiner word, so all six draws are refused for that. */
    {
        const d3d8_frame_profile_summary census = d3d8_frame_profile_get();
        CHECK_EQ_U32(census.combiner_draws, 6u);
        CHECK_EQ_U32(census.combiner_planned, 0u);
        CHECK_EQ_U32(census.combiner_refused, 6u);
        CHECK_EQ_U32(census.combiner_outcome_count, 1u);
        CHECK(census.combiner_outcomes[0].draws == 6u && !census.combiner_outcomes[0].planned);
        CHECK(census.combiner_outcomes[0].first_frame == 1u && census.combiner_outcomes[0].first_draw == 0u);
        CHECK(strstr(census.combiner_outcomes[0].text, "gpu_pgraph_set_combiner") == NULL); /* the decoder has it on */
    }
    /* T497: the same draws with a stand-in texture at stage 0. No combiner word was written, so a texture changes
     * nothing here: six draws, six refusals, one outcome, and the census is a separate tally. */
    {
        const d3d8_frame_profile_summary census = d3d8_frame_profile_get();
        CHECK_EQ_U32(census.standin_census.draws, 6u);
        CHECK_EQ_U32(census.standin_census.planned, 0u);
        CHECK_EQ_U32(census.standin_census.refused, 6u);
        CHECK_EQ_U32(census.standin_census.outcome_count, 1u);
        CHECK(census.standin_census.outcomes[0].draws == 6u && !census.standin_census.outcomes[0].planned);
        CHECK(census.standin_census.outcomes[0].first_frame == 1u && census.standin_census.outcomes[0].first_draw == 0u);
        CHECK(strcmp(census.standin_census.outcomes[0].text, census.combiner_outcomes[0].text) == 0);
    }
    {
        /* The pass fixture in frames 20 and 21, and in frame 22 twice over: a pass draw, then (draw index 1) a
         * configuration that reads texture register t0 with no texture to read. */
        static const uint32_t pass_entries[][2] = {{34u, 0xC4200000u}, {0u, 0xD4300000u}, {45u, 0xC0u},
                                                   {26u, 0xC0u},       {53u, 0x11101u},   {8u, 0x0Cu},
                                                   {9u, 0x1C80u}};
        for (uint32_t frame = 0u; frame < 3u; frame++) {
            for (uint32_t which = 0u; which < (frame == 2u ? 2u : 1u); which++) {
                uint32_t words[GPU_PGRAPH_COMBINER_WORDS] = {0u};
                for (size_t entry = 0u; entry < sizeof pass_entries / sizeof pass_entries[0]; entry++) {
                    words[pass_entries[entry][0]] = pass_entries[entry][1];
                }
                if (frame == 2u && which == 1u) {
                    words[34] = 0x08200000u; /* A = t0: a texture read, and no texture to read */
                    words[0] = 0x18300000u;  /* and the alpha A = t0.a, the textured fixture of the stand-in tests */
                }
                stream_builder builder = {0};
                stream_pixel_shader(&builder, words);
                CHECK(builder.count > 0u);
                for (size_t pair = 0u; pair < builder.count; pair++) {
                    d3d8_pushbuffer_emit_pair(0x00040000u | builder.pairs[pair].method, builder.pairs[pair].data);
                }
                stream_free(&builder);
                write_frame(7u, 0xB1B1B1B1u, false, 4u);
            }
            const d3d8_frame_record present = record(20u + frame, 30u + frame, 30u + frame);
            d3d8_frame_profile_note(&present);
        }
        const d3d8_frame_profile_summary census = d3d8_frame_profile_get();
        CHECK(!census.decoder_failed);
        CHECK_EQ_U32(census.combiner_draws, 10u);
        CHECK_EQ_U32(census.combiner_planned, 3u);
        CHECK_EQ_U32(census.combiner_refused, 7u);
        CHECK_EQ_U32(census.combiner_outcome_count, 3u);
        size_t planned_outcomes = 0u;
        size_t texture_outcomes = 0u;
        for (size_t index = 0u; index < census.combiner_outcome_count; index++) {
            const d3d8_frame_profile_combiner_outcome *outcome = &census.combiner_outcomes[index];
            if (outcome->planned) {
                planned_outcomes++;
                CHECK(outcome->draws == 3u && outcome->first_frame == 20u && outcome->first_draw == 0u);
                CHECK(strstr(outcome->text, COMBINER_NAME_PASS) != NULL);
                CHECK(strstr(outcome->text, "1 stage(s)") != NULL);
            } else if (strstr(outcome->text, "texture register t0") != NULL) {
                texture_outcomes++;
                CHECK(outcome->draws == 1u && outcome->first_frame == 22u && outcome->first_draw == 1u);
            }
        }
        CHECK_EQ_U32(planned_outcomes, 1u);
        CHECK_EQ_U32(texture_outcomes, 1u);
        /* T497: with a stand-in texture at stage 0 the t0 draw (frame 22, draw 1) now has a plan, and the census says
         * which inferences it needed: the never-written stage program and the sampling (this synthetic stream never
         * writes word 54, the title's does). The six draws that wrote no combiner word stay refused. */
        CHECK_EQ_U32(census.standin_census.draws, 10u);
        CHECK_EQ_U32(census.standin_census.planned, 4u);
        CHECK_EQ_U32(census.standin_census.refused, 6u);
        CHECK_EQ_U32(census.standin_census.outcome_count, 3u);
        size_t standin_pass = 0u;
        size_t standin_textured = 0u;
        for (size_t index = 0u; index < census.standin_census.outcome_count; index++) {
            const d3d8_frame_profile_combiner_outcome *outcome = &census.standin_census.outcomes[index];
            if (outcome->planned && strstr(outcome->text, COMBINER_NAME_PASS) != NULL) {
                standin_pass++;
                CHECK(outcome->draws == 3u && outcome->first_frame == 20u && outcome->first_draw == 0u);
                CHECK(strstr(outcome->text, "texture stages 0x0, inferences 0x800") != NULL);
                CHECK(outcome->words[34] == 0xC4200000u && outcome->words[53] == 0x11101u); /* the pass definition */
            } else if (outcome->planned && strstr(outcome->text, COMBINER_NAME_TEXTURED) != NULL) {
                standin_textured++;
                CHECK(outcome->draws == 1u && outcome->first_frame == 22u && outcome->first_draw == 1u);
                CHECK(strstr(outcome->text, "1 stage(s), texture stages 0x1, inferences 0x1100") != NULL);
                CHECK(outcome->words[34] == 0x08200000u && outcome->words[0] == 0x18300000u); /* reads t0 */
                CHECK(outcome->words[53] == 0x11101u);
                CHECK(outcome->words[54] == 0u);
            }
        }
        CHECK_EQ_U32(standin_pass, 1u);
        CHECK_EQ_U32(standin_textured, 1u);
        FILE *census_file = tmpfile();
        CHECK(census_file != NULL);
        if (census_file != NULL) {
            d3d8_frame_profile_report(census_file);
            fflush(census_file);
            rewind(census_file);
            static char census_text[32768];
            const size_t census_length = fread(census_text, 1u, sizeof(census_text) - 1u, census_file);
            census_text[census_length] = '\0';
            fclose(census_file);
            CHECK(strstr(census_text, "combiner census (T478, every inference allowed, no test texture): 10 draw(s), "
                                      "3 with a plan, 7 refused, 3 distinct outcome(s)") != NULL);
            CHECK(strstr(census_text, "combiner plan x3, first at frame 20 draw 0: " COMBINER_NAME_PASS) != NULL);
            CHECK(strstr(census_text, "combiner REFUSED x1, first at frame 22 draw 1:") != NULL);
            CHECK(strstr(census_text, "combiner census with a STAND-IN texture at stage 0 (T497, NOT the title's "
                                      "texture, every inference allowed): 10 draw(s), 4 with a plan, 6 refused, "
                                      "3 distinct outcome(s)") != NULL);
            CHECK(strstr(census_text, "stand-in plan x1, first at frame 22 draw 1: " COMBINER_NAME_TEXTURED) != NULL);
            CHECK(strstr(census_text, "stand-in plan x3, first at frame 20 draw 0: " COMBINER_NAME_PASS) != NULL);
            /* each planned outcome's definition is printed: name, then the 57 words (word 34 is the colour input) */
            const char *definition = strstr(census_text, "  stand-in definition " COMBINER_NAME_TEXTURED " ");
            CHECK(definition != NULL);
            if (definition != NULL) {
                const char *line_end = strchr(definition, '\n');
                unsigned words_seen = 0u;
                for (const char *cursor = definition + strlen("  stand-in definition " COMBINER_NAME_TEXTURED);
                     line_end != NULL && cursor < line_end; cursor += 9) {
                    words_seen++;
                }
                CHECK_EQ_U32(words_seen, GPU_PGRAPH_COMBINER_WORDS);
                CHECK(strstr(definition, " 08200000 ") != NULL && strstr(definition, " 00011101 ") != NULL);
            }
            CHECK(strstr(census_text, "  stand-in definition " COMBINER_NAME_PASS " ") != NULL);
            CHECK(strstr(census_text, "stand-in definition combiner_") != NULL);
        }
    }

    /* T441: the unhandled list was capped at 64 and cut silently, once the real loop's distinct methods passed it.
     * A frame with 70 more distinct unhandled methods (0x0500 + 4k) must list all 71 and say none was cut. */
    {
        uint32_t words[70 * 2];
        for (uint32_t index = 0u; index < 70u; index++) {
            words[index * 2u] = (1u << 18) | (0x0500u + index * 4u);
            words[index * 2u + 1u] = index;
        }
        write_pairs(words, 140u);
        d3d8_gpu_kick();
        const d3d8_frame_record wide = record(7u, 8u, 16u);
        d3d8_frame_profile_note(&wide);
        const d3d8_frame_profile_summary many = d3d8_frame_profile_get();
        CHECK_EQ_U32(many.unhandled_count, 71u);
        CHECK_EQ_U32(many.unhandled_method[70], 0x0500u + 69u * 4u);
        CHECK(D3D8_FRAME_PROFILE_UNHANDLED >= GPU_PGRAPH_UNHANDLED_TABLE);
        FILE *wide_file = tmpfile();
        CHECK(wide_file != NULL);
        if (wide_file != NULL) {
            d3d8_frame_profile_report(wide_file);
            fflush(wide_file);
            rewind(wide_file);
            static char wide_text[32768];
            const size_t wide_length = fread(wide_text, 1u, sizeof(wide_text) - 1u, wide_file);
            wide_text[wide_length] = '\0';
            fclose(wide_file);
            CHECK(strstr(wide_text, "refuse the frame): 71: 0300 x3") != NULL);
            CHECK(strstr(wide_text, "0544 x1") != NULL);
        }
    }

    /* Disabling clears it and gives the recording back. */
    CHECK(d3d8_frame_profile_enable(false));
    CHECK(!d3d8_frame_profile_enabled());
    CHECK_EQ_U32(d3d8_frame_profile_get().frames, 0u);

    /* T391: the fence packet every present (and every refill) records is bookkeeping, not a change of scene.
     * A and B are the same frame with a fence after it: the fence value and the ring cursor differ, which the
     * diff must not count and the normalised digest must not see (the full digest keeps the fence value). C adds a
     * 3D 0x0310 pair (SET_DITHER_ENABLE) and D the same method and data on SUBCHANNEL 5: the two are different
     * commands whatever their numbers (and with the data 0 on both, so only the subchannel tells them apart), to the
     * digests and to the diff. E and F carry only a subchannel-5 0x0310
     * pair (no fence) whose DATA differs, the ring cursor of a real notification: neither digest may see it. G and H
     * are the C and D pair again with no fence at all, so nothing but the subchannel tells them apart, and the FULL
     * digest (which also holds the fence value) must still differ. */
    CHECK(d3d8_frame_profile_enable(true));
    for (uint32_t frame = 0u; frame < 8u; frame++) {
        write_frame(1u, 0xA0A0A0A0u, false, 4u);
        if (frame == 2u) {
            d3d8_pushbuffer_emit_pair(0x00040310u, 0u);
        } else if (frame == 3u) {
            d3d8_pushbuffer_emit_pair(0x0004A310u, 0u);
        } else if (frame >= 4u) {
            const uint32_t header = frame == 6u ? 0x00040310u : 0x0004A310u;
            d3d8_pushbuffer_emit_pair(header, frame == 4u ? 111u : frame == 5u ? 222u : 0u);
            d3d8_gpu_kick();
        }
        if (frame < 4u) {
            (void)d3d8_gpu_fence_insert(0u);
        }
        const d3d8_frame_record present = record(frame + 1u, frame + 1u, frame + 1u);
        d3d8_frame_profile_note(&present);
    }
    const d3d8_frame_profile_summary fenced = d3d8_frame_profile_get();
    CHECK_EQ_U32(fenced.frames, 8u);
    CHECK(!fenced.decoder_failed);
    CHECK(fenced.first[0].normalised == fenced.first[1].normalised);
    CHECK(fenced.first[0].digest != fenced.first[1].digest);
    CHECK(fenced.first[2].normalised != fenced.first[1].normalised);
    CHECK(fenced.first[3].normalised != fenced.first[2].normalised);
    CHECK(fenced.first[3].digest != fenced.first[2].digest);
    CHECK(fenced.first[4].normalised != fenced.first[3].normalised);
    CHECK(fenced.first[4].normalised == fenced.first[5].normalised);
    CHECK(fenced.first[4].digest == fenced.first[5].digest);
    CHECK(fenced.first[6].digest != fenced.first[7].digest);
    CHECK(fenced.first[6].normalised != fenced.first[7].normalised);
    CHECK(fenced.first[7].digest == fenced.first[4].digest); /* H is E with another notification data */
    CHECK_EQ_U32(fenced.distinct_normalised, 5u);
    CHECK_EQ_U32(fenced.distinct_digests, 6u);
    CHECK_EQ_U32(fenced.same_as_previous, 1u);
    CHECK_EQ_U32(fenced.normalised_same_as_previous, 2u);
    /* A to B, C to D, E to F, F to G and G to H have the same length, B to C and D to E do not. A to B and E to F
     * differ in nothing but bookkeeping data, C to D, F to G and G to H in the subchannel of one command. */
    CHECK_EQ_U32(fenced.pairs_diffed, 5u);
    CHECK_EQ_U32(fenced.pairs_length_differs, 2u);
    CHECK_EQ_U32(fenced.commands_differing, 3u);
    CHECK_EQ_U32(fenced.diff_method_count, 1u);
    CHECK(diff_entry(&fenced, 0x0310u) != NULL);
    CHECK(diff_entry(&fenced, 0x1D70u) == NULL);
    /* four fence notifications, the pair of D, the pairs of E, F and H (G's is a 3D command) */
    CHECK_EQ_U32(d3d8_gpu_get_stats().commands_other_subchannel, 8u);
    CHECK(d3d8_frame_profile_enable(false));
    environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0 || checks < 60;
}
