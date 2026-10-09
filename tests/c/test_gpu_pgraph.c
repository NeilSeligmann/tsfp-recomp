/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gpu_pgraph (T84, decoder half) and the device-free pieces of gpu_pgraph_replay: the decoder's
 * state model and draw list, its refusals, SHA-256, program resolution through the selector,
 * vertex assembly from a fake guest memory and the topology expansion. Needs no Vulkan device.
 *
 * Streams are written the way the measured emitters write them (gpu_pgraph_test_support.h). Every
 * equality is preceded by an assertion that there is something to compare.
 */
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"
#include "gpu_pgraph_test_support.h"
#include "gpu_sha256.h"
#include "gpu_vsh_draw.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(condition)) {                                                                   \
            failures++;                                                                       \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);                             \
        }                                                                                     \
    } while (0)

static uint32_t mixed_bytes_value(uint32_t index)
{
    return (index * 7u + 3u) & 255u;
}

static void test_sha256(void)
{
    printf("test_sha256\n");
    static const struct {
        uint32_t length;
        const char *hex;
    } vectors[] = {
        {0u, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {3u, "6ab0dba1f4f1dfbb37b4f9eeb092c09fca4900ad32bdcd147d8dde35d6c87c35"},
        {55u, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
        {56u, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
        {63u, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
        {64u, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
        {65u, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e"},
        {119u, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e"},
        {120u, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5"},
        {1000u, "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371"},
    };
    CHECK(sizeof vectors / sizeof vectors[0] == 10u);
    for (size_t i = 0u; i < sizeof vectors / sizeof vectors[0]; i++) {
        uint8_t data[1000];
        for (uint32_t n = 0u; n < vectors[i].length; n++) {
            data[n] = (uint8_t)mixed_bytes_value(n);
        }
        uint8_t digest[32];
        char hex[65];
        gpu_sha256(data, vectors[i].length, digest);
        gpu_sha256_hex(digest, hex);
        CHECK(strlen(hex) == 64u);
        CHECK(strcmp(hex, vectors[i].hex) == 0);
    }
}

static void test_format_decode(void)
{
    printf("test_format_decode\n");
    const gpu_pgraph_format position = gpu_pgraph_decode_format(array_format(20u, 3u, GPU_PGRAPH_TYPE_F));
    CHECK(position.type == GPU_PGRAPH_TYPE_F && position.size == 3u && position.stride == 20u);
    /* the title's measured formats (T97): v1 0x32, v2 0x40, v3 0x22, disabled marker 2 */
    const gpu_pgraph_format colour = gpu_pgraph_decode_format((28u << 8) | 0x40u);
    CHECK(colour.type == GPU_PGRAPH_TYPE_UB_D3D && colour.size == 4u && colour.stride == 28u);
    const gpu_pgraph_format disabled = gpu_pgraph_decode_format(2u);
    CHECK(disabled.size == 0u);
    const gpu_pgraph_format wide = gpu_pgraph_decode_format(0xFFFFFF22u);
    CHECK(wide.stride == 0xFFFFFFu && wide.size == 2u && wide.type == 2u);
}

/* The state a draw sees after a full library-shaped setup. */
static void build_setup(stream_builder *stream, uint32_t array_address)
{
    static const float offset[4] = {320.0f, 240.0f, 0.0f, 0.0f};
    static const float scale[4] = {320.0f, -240.0f, 16777215.0f, 0.0f};
    stream_viewport(stream, offset, scale);
    stream_pair(stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    uint32_t words[8 * 4];
    for (uint32_t i = 0u; i < 8u * 4u; i++) {
        words[i] = 0x1000u + i;
        if (i % 4u == 3u) {
            words[i] &= ~1u;
        }
    }
    words[7u * 4u + 3u] |= 1u;
    stream_program(stream, 2u, words, 8u);
    stream_pair(stream, GPU_PGRAPH_PROGRAM_START, 2u);
    float constants[5 * 4];
    for (uint32_t i = 0u; i < 5u * 4u; i++) {
        constants[i] = 0.5f + (float)i;
    }
    stream_constants(stream, 96u, constants, 5u * 4u);
    stream_array(stream, 1u, array_address, array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
    stream_pair(stream, GPU_PGRAPH_ARRAY_FORMAT + 4u * 0u, 2u); /* disabled marker */
}

static void test_state_and_draw(void)
{
    printf("test_state_and_draw\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    CHECK(pgraph != NULL);
    stream_builder stream = {0};
    build_setup(&stream, 0x00400000u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 4u, 6u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u);
    const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(pgraph, 0u);
    CHECK(draw != NULL);
    if (draw == NULL) {
        return;
    }
    CHECK(draw->primitive == GPU_PGRAPH_OP_TRIANGLES);
    CHECK(draw->index_count == 6u);
    const uint32_t *indices = gpu_pgraph_indices(pgraph) + draw->first_index;
    for (uint32_t i = 0u; i < 6u; i++) {
        CHECK(indices[i] == 4u + i);
    }
    CHECK(draw->arrays[1].address_set && draw->arrays[1].address == 0x00400000u);
    CHECK(draw->arrays[1].format == array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
    CHECK(draw->arrays[0].format_set && !draw->arrays[0].address_set);
    CHECK(draw->first_command == stream.count - 3u);
    const gpu_pgraph_state *state = gpu_pgraph_snapshot(pgraph, draw->snapshot);
    CHECK(state != NULL);
    if (state == NULL) {
        return;
    }
    CHECK(state->execution_mode_set && state->execution_mode == 6u);
    CHECK(state->program_start_set && state->program_start == 2u);
    for (uint32_t slot = 0u; slot < GPU_PGRAPH_PROGRAM_SLOTS; slot++) {
        CHECK(state->slot_written[slot] == (slot >= 2u && slot < 10u));
    }
    for (uint32_t i = 0u; i < 8u * 4u; i++) {
        const uint32_t expected = (i % 4u == 3u && i != 31u) ? (0x1000u + i) & ~1u
                                  : i == 31u                 ? (0x1000u + i) | 1u
                                                             : 0x1000u + i;
        CHECK(state->program[2u * 4u + i] == expected);
    }
    for (uint32_t row = 0u; row < GPU_PGRAPH_CONSTANT_ROWS; row++) {
        CHECK(state->constant_written[row] == (row >= 96u && row < 101u));
    }
    for (uint32_t i = 0u; i < 5u * 4u; i++) {
        CHECK(state->constants[96u * 4u + i] == 0.5f + (float)i);
    }
    CHECK(state->constants[0] == 0.0f && state->constants[101u * 4u] == 0.0f);
    CHECK(state->viewport_offset_set && state->viewport_scale_set);
    CHECK(state->viewport_offset[0] == 320.0f && state->viewport_offset[1] == 240.0f);
    CHECK(state->viewport_scale[1] == -240.0f && state->viewport_scale[2] == 16777215.0f);
    const gpu_pgraph_stats stats = gpu_pgraph_get_stats(pgraph);
    CHECK(stats.pairs == stream.count && stats.pairs_handled == stream.count);
    CHECK(stats.draws == 1u && stats.program_dwords == 32u && stats.constant_dwords == 20u);
    CHECK(gpu_pgraph_unhandled_count(pgraph) == 0u);
    CHECK(strcmp(gpu_pgraph_error(pgraph), "") == 0);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

static void test_indices(void)
{
    printf("test_indices\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    /* a 300 vertex DrawVertices is two chunks of the one non-incrementing 0x1810 run */
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLE_STRIP, 10u, 300u);
    /* DrawIndexedVertices: two 16-bit indices per dword, low half first, then one 32-bit */
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_QUADS);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT16, (7u << 16) | 3u);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT16, (0xFFFFu << 16) | 0x0102u);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, 9u);
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, 0u);
    /* a bracket with no vertices is not a draw */
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS);
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, 0u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 2u);
    const gpu_pgraph_draw *first = gpu_pgraph_draw_at(pgraph, 0u);
    const gpu_pgraph_draw *second = gpu_pgraph_draw_at(pgraph, 1u);
    CHECK(first != NULL && second != NULL);
    if (first == NULL || second == NULL) {
        return;
    }
    CHECK(first->index_count == 300u && first->primitive == GPU_PGRAPH_OP_TRIANGLE_STRIP);
    const uint32_t *all = gpu_pgraph_indices(pgraph);
    CHECK(all[first->first_index] == 10u && all[first->first_index + 255u] == 265u);
    CHECK(all[first->first_index + 256u] == 266u && all[first->first_index + 299u] == 309u);
    CHECK(second->first_index == 300u && second->index_count == 5u);
    CHECK(all[second->first_index] == 3u && all[second->first_index + 1u] == 7u);
    CHECK(all[second->first_index + 2u] == 0x0102u && all[second->first_index + 3u] == 0xFFFFu);
    CHECK(all[second->first_index + 4u] == 9u);
    CHECK(gpu_pgraph_get_stats(pgraph).empty_brackets == 1u);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

static void test_snapshots_are_copies(void)
{
    printf("test_snapshots_are_copies\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    build_setup(&stream, 0x1000u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u); /* nothing changed in between */
    static const float changed[4] = {9.0f, 8.0f, 7.0f, 6.0f};
    stream_constants(&stream, 96u, changed, 4u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 3u);
    const gpu_pgraph_draw *a = gpu_pgraph_draw_at(pgraph, 0u);
    const gpu_pgraph_draw *b = gpu_pgraph_draw_at(pgraph, 1u);
    const gpu_pgraph_draw *c = gpu_pgraph_draw_at(pgraph, 2u);
    CHECK(a != NULL && b != NULL && c != NULL);
    if (a == NULL || b == NULL || c == NULL) {
        return;
    }
    CHECK(a->snapshot == b->snapshot && b->snapshot != c->snapshot);
    CHECK(gpu_pgraph_snapshot_count(pgraph) == 2u);
    CHECK(gpu_pgraph_snapshot(pgraph, a->snapshot)->constants[96u * 4u] == 0.5f);
    CHECK(gpu_pgraph_snapshot(pgraph, c->snapshot)->constants[96u * 4u] == 9.0f);
    CHECK(gpu_pgraph_snapshot(pgraph, c->snapshot)->constants[97u * 4u] == 4.5f); /* unchanged row */
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

static void test_decode_resumes_across_kicks(void)
{
    printf("test_decode_resumes_across_kicks\n");
    gpu_pgraph *whole = gpu_pgraph_create();
    gpu_pgraph *split = gpu_pgraph_create();
    stream_builder stream = {0};
    build_setup(&stream, 0x2000u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(stream.count > 40u);
    CHECK(gpu_pgraph_decode(whole, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    /* cut in the middle of the program upload AND of the bracket */
    CHECK(gpu_pgraph_decode(split, stream.pairs, 20u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_decode(split, stream.pairs + 20u, stream.count - 22u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_decode(split, stream.pairs + stream.count - 2u, 2u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(whole) == 1u && gpu_pgraph_draw_count(split) == 1u);
    const gpu_pgraph_state *a = gpu_pgraph_snapshot(whole, 0u);
    const gpu_pgraph_state *b = gpu_pgraph_snapshot(split, 0u);
    CHECK(a != NULL && b != NULL);
    if (a != NULL && b != NULL) {
        CHECK(memcmp(a, b, sizeof *a) == 0);
    }
    stream_free(&stream);
    gpu_pgraph_destroy(whole);
    gpu_pgraph_destroy(split);
}

/* T84a: a frame boundary forgets the draw list and keeps the state the next frame draws with. */
static void test_begin_frame(void)
{
    printf("test_begin_frame\n");
    CHECK(gpu_pgraph_begin_frame(NULL) == GPU_PGRAPH_ERR_ARGUMENT);
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    build_setup(&stream, 0x2000u);
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u && gpu_pgraph_snapshot_count(pgraph) == 1u);
    const gpu_pgraph_state before = *gpu_pgraph_state_now(pgraph);
    const uint64_t pairs = gpu_pgraph_get_stats(pgraph).pairs;
    CHECK(pairs > 40u);
    CHECK(gpu_pgraph_begin_frame(pgraph) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 0u && gpu_pgraph_snapshot_count(pgraph) == 0u);
    CHECK(memcmp(&before, gpu_pgraph_state_now(pgraph), sizeof before) == 0);
    CHECK(gpu_pgraph_get_stats(pgraph).pairs == pairs); /* statistics accumulate */
    /* the second frame draws with the first frame's program and arrays, indices from 0 again */
    stream_builder again = {0};
    stream_draw_arrays(&again, GPU_PGRAPH_OP_TRIANGLES, 3u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, again.pairs, again.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u);
    const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(pgraph, 0u);
    CHECK(draw != NULL);
    if (draw != NULL) {
        CHECK(draw->first_index == 0u && draw->index_count == 3u);
        CHECK(gpu_pgraph_indices(pgraph)[0] == 3u && draw->arrays[1].address_set);
    }
    /* an open bracket cannot cross it: its indices would be lost */
    stream_builder open = {0};
    stream_pair(&open, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    CHECK(gpu_pgraph_decode(pgraph, open.pairs, open.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_begin_frame(pgraph) == GPU_PGRAPH_ERR_MALFORMED);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u); /* untouched by the refusal */
    stream_free(&open);
    stream_free(&again);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

static gpu_pgraph_result decode_pairs(gpu_pgraph *pgraph, const gpu_pgraph_command *pairs, size_t count)
{
    return gpu_pgraph_decode(pgraph, pairs, count);
}

static void test_refusals(void)
{
    printf("test_refusals\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    {
        const gpu_pgraph_command outside[] = {{GPU_PGRAPH_DRAW_ARRAYS, 2u << 24}};
        CHECK(decode_pairs(pgraph, outside, 1u) == GPU_PGRAPH_ERR_MALFORMED);
        CHECK(strstr(gpu_pgraph_error(pgraph), "0x1810") != NULL);
        CHECK(strstr(gpu_pgraph_error(pgraph), "outside a BEGIN_END") != NULL);
        CHECK(gpu_pgraph_draw_count(pgraph) == 0u);
    }
    gpu_pgraph_reset(pgraph);
    {
        const gpu_pgraph_command elements[] = {{GPU_PGRAPH_ARRAY_ELEMENT16, 1u}};
        CHECK(decode_pairs(pgraph, elements, 1u) == GPU_PGRAPH_ERR_MALFORMED);
        const gpu_pgraph_command element32[] = {{GPU_PGRAPH_ARRAY_ELEMENT32, 1u}};
        gpu_pgraph_reset(pgraph);
        CHECK(decode_pairs(pgraph, element32, 1u) == GPU_PGRAPH_ERR_MALFORMED);
    }
    gpu_pgraph_reset(pgraph);
    {
        const gpu_pgraph_command nested[] = {{GPU_PGRAPH_BEGIN_END, 5u}, {GPU_PGRAPH_BEGIN_END, 6u}};
        CHECK(decode_pairs(pgraph, nested, 2u) == GPU_PGRAPH_ERR_MALFORMED);
        CHECK(strstr(gpu_pgraph_error(pgraph), "pair 1") != NULL);
    }
    gpu_pgraph_reset(pgraph);
    {
        const gpu_pgraph_command lone_end[] = {{GPU_PGRAPH_BEGIN_END, 0u}};
        CHECK(decode_pairs(pgraph, lone_end, 1u) == GPU_PGRAPH_ERR_MALFORMED);
        const gpu_pgraph_command bad_op[] = {{GPU_PGRAPH_BEGIN_END, 11u}};
        gpu_pgraph_reset(pgraph);
        CHECK(decode_pairs(pgraph, bad_op, 1u) == GPU_PGRAPH_ERR_UNMEASURED);
    }
    /* vertex-stage state inside a bracket, one method of each group */
    static const uint32_t state_methods[] = {
        GPU_PGRAPH_VIEWPORT_OFFSET, GPU_PGRAPH_VIEWPORT_SCALE + 12u, GPU_PGRAPH_PROGRAM_DATA,
        GPU_PGRAPH_CONSTANT_DATA + 0x7Cu, GPU_PGRAPH_ARRAY_OFFSET, GPU_PGRAPH_ARRAY_FORMAT + 60u,
        GPU_PGRAPH_EXECUTION_MODE, GPU_PGRAPH_CXT_WRITE_EN, GPU_PGRAPH_PROGRAM_LOAD, GPU_PGRAPH_PROGRAM_START,
        GPU_PGRAPH_CONSTANT_LOAD,
    };
    for (size_t i = 0u; i < sizeof state_methods / sizeof state_methods[0]; i++) {
        gpu_pgraph_reset(pgraph);
        const gpu_pgraph_command inside[] = {{GPU_PGRAPH_BEGIN_END, 5u}, {state_methods[i], 0u}};
        CHECK(decode_pairs(pgraph, inside, 2u) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(gpu_pgraph_error(pgraph), "inside a BEGIN_END") != NULL);
    }
    /* the files are bounded: one dword past each is refused, the last dword fits */
    gpu_pgraph_reset(pgraph);
    {
        stream_builder stream = {0};
        stream_pair(&stream, GPU_PGRAPH_CONSTANT_LOAD, 191u);
        for (uint32_t i = 0u; i < 4u; i++) {
            stream_pair(&stream, GPU_PGRAPH_CONSTANT_DATA, float_bits_of(1.0f));
        }
        CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_state_now(pgraph)->constant_written[191]);
        const gpu_pgraph_command one_more[] = {{GPU_PGRAPH_CONSTANT_DATA, 0u}};
        CHECK(decode_pairs(pgraph, one_more, 1u) == GPU_PGRAPH_ERR_MALFORMED);
        CHECK(strstr(gpu_pgraph_error(pgraph), "192-row") != NULL);
        stream_free(&stream);
    }
    gpu_pgraph_reset(pgraph);
    {
        stream_builder stream = {0};
        stream_pair(&stream, GPU_PGRAPH_PROGRAM_LOAD, 135u);
        for (uint32_t i = 0u; i < 4u; i++) {
            stream_pair(&stream, GPU_PGRAPH_PROGRAM_DATA, i);
        }
        CHECK(decode_pairs(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_state_now(pgraph)->slot_written[135]);
        const gpu_pgraph_command one_more[] = {{GPU_PGRAPH_PROGRAM_DATA, 0u}};
        CHECK(decode_pairs(pgraph, one_more, 1u) == GPU_PGRAPH_ERR_MALFORMED);
        CHECK(strstr(gpu_pgraph_error(pgraph), "136-slot") != NULL);
        stream_free(&stream);
    }
    gpu_pgraph_reset(pgraph);
    {
        const gpu_pgraph_command load[] = {{GPU_PGRAPH_PROGRAM_LOAD, 136u}};
        CHECK(decode_pairs(pgraph, load, 1u) == GPU_PGRAPH_ERR_MALFORMED);
        const gpu_pgraph_command start[] = {{GPU_PGRAPH_PROGRAM_START, 136u}};
        gpu_pgraph_reset(pgraph);
        CHECK(decode_pairs(pgraph, start, 1u) == GPU_PGRAPH_ERR_MALFORMED);
        const gpu_pgraph_command row[] = {{GPU_PGRAPH_CONSTANT_LOAD, 192u}};
        gpu_pgraph_reset(pgraph);
        CHECK(decode_pairs(pgraph, row, 1u) == GPU_PGRAPH_ERR_MALFORMED);
    }
    gpu_pgraph_destroy(pgraph);
}

/* T391: a recorded command on a subchannel other than 0 never enters the 3D decode. The fence's software method
 * (subchannel 5, 0x310) is counted and takes its place in the pair count, everything else is refused, strict
 * or not, and the semaphore release is a handled no-op that is refused inside a bracket. */
static void test_other_subchannels(void)
{
    printf("test_other_subchannels\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    const gpu_pgraph_command fence[] = {{0x1D70u, 9u}, {0x1D90u, 0u}};
    CHECK(gpu_pgraph_decode_other_subchannel(pgraph, 5u, 0x310u, 0x12345678u) == GPU_PGRAPH_OK);
    CHECK(decode_pairs(pgraph, fence, 2u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_get_stats(pgraph).software_methods == 1u);
    CHECK(gpu_pgraph_get_stats(pgraph).semaphore_releases == 1u);
    /* One pair for the software method, so the pair index stays aligned with the recording. The packet's
     * zero colour clear value is consumed with it (the clear group is off), counted and not applied. */
    CHECK(gpu_pgraph_get_stats(pgraph).pairs == 3u && gpu_pgraph_get_stats(pgraph).pairs_handled == 3u);
    CHECK(gpu_pgraph_get_stats(pgraph).fence_clear_values == 1u);
    /* Everything else is refused with the subchannel named, and counts nothing. MUTATION: replaying it as a
     * 3D method (0x310 on subchannel 0 is SET_DITHER_ENABLE) or accepting every subchannel-5 method passes. */
    static const struct {
        uint32_t subchannel, method;
    } refused[] = {{5u, 0x314u}, {5u, 0x1D70u}, {1u, 0x310u}, {2u, 0x184u}, {7u, 0x310u}, {3u, 0x0u}};
    for (size_t i = 0u; i < sizeof refused / sizeof refused[0]; i++) {
        const uint64_t pairs = gpu_pgraph_get_stats(pgraph).pairs;
        CHECK(gpu_pgraph_decode_other_subchannel(pgraph, refused[i].subchannel, refused[i].method, 0u) ==
              GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(gpu_pgraph_get_stats(pgraph).pairs == pairs);
        char subchannel[24];
        snprintf(subchannel, sizeof subchannel, "subchannel %u", (unsigned)refused[i].subchannel);
        CHECK(strstr(gpu_pgraph_error(pgraph), subchannel) != NULL);
    }
    /* Not in strict mode only: strict makes no difference. */
    gpu_pgraph_set_strict(pgraph, true);
    CHECK(gpu_pgraph_decode_other_subchannel(pgraph, 5u, 0x310u, 0u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_decode_other_subchannel(pgraph, 4u, 0x310u, 0u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(gpu_pgraph_decode_other_subchannel(pgraph, 0u, 0x310u, 0u) == GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_pgraph_decode_other_subchannel(NULL, 5u, 0x310u, 0u) == GPU_PGRAPH_ERR_ARGUMENT);
    /* The consumption is the packet's own, not any 0x1D90 write: it needs the software method first, the
     * semaphore release first, and a value of 0. Strict mode refuses the others, group off. */
    gpu_pgraph_reset(pgraph);
    const gpu_pgraph_command bare_clear_value[] = {{0x1D90u, 0u}};
    CHECK(decode_pairs(pgraph, bare_clear_value, 1u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(gpu_pgraph_decode_other_subchannel(pgraph, 5u, 0x310u, 0u) == GPU_PGRAPH_OK);
    CHECK(decode_pairs(pgraph, bare_clear_value, 1u) == GPU_PGRAPH_ERR_UNMEASURED); /* before the release */
    gpu_pgraph_reset(pgraph);
    CHECK(gpu_pgraph_decode_other_subchannel(pgraph, 5u, 0x310u, 0u) == GPU_PGRAPH_OK);
    const gpu_pgraph_command nonzero_clear_value[] = {{0x1D70u, 5u}, {0x1D90u, 0xFFu}};
    CHECK(decode_pairs(pgraph, nonzero_clear_value, 2u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(gpu_pgraph_get_stats(pgraph).fence_clear_values == 0u);
    gpu_pgraph_reset(pgraph);
    CHECK(gpu_pgraph_decode_other_subchannel(pgraph, 5u, 0x310u, 0u) == GPU_PGRAPH_OK);
    const gpu_pgraph_command whole_tail[] = {{0x1D70u, 5u}, {0x1D90u, 0u}, {0x1D90u, 0u}, {0x1D90u, 0u}};
    CHECK(decode_pairs(pgraph, whole_tail, 3u) == GPU_PGRAPH_OK); /* the packet's three */
    CHECK(gpu_pgraph_get_stats(pgraph).fence_clear_values == 2u);
    CHECK(decode_pairs(pgraph, whole_tail + 3, 1u) == GPU_PGRAPH_ERR_UNMEASURED); /* a fourth is not */
    /* Any command that is not the packet's next one ENDS the packet: here an execution mode write follows the
     * notification, so the 0x1D70 and the 0x1D90 after it are judged on their own and the 0x1D90 is refused. */
    gpu_pgraph_reset(pgraph);
    CHECK(gpu_pgraph_decode_other_subchannel(pgraph, 5u, 0x310u, 0u) == GPU_PGRAPH_OK);
    const gpu_pgraph_command interrupted[] = {{0x1E94u, 6u}, {0x1D70u, 5u}, {0x1D90u, 0u}};
    CHECK(decode_pairs(pgraph, interrupted, 3u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(gpu_pgraph_get_stats(pgraph).fence_clear_values == 0u);
    /* With the clear group on the packet's clear values are the writes they are. */
    gpu_pgraph_set_output_groups(pgraph, GPU_PGRAPH_OUTPUT_CLEAR);
    gpu_pgraph_reset(pgraph);
    CHECK(gpu_pgraph_decode_other_subchannel(pgraph, 5u, 0x310u, 0u) == GPU_PGRAPH_OK);
    CHECK(decode_pairs(pgraph, whole_tail, 3u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_get_stats(pgraph).fence_clear_values == 0u);
    CHECK(gpu_pgraph_state_now(pgraph)->output_written[GPU_PGRAPH_OUT_CLEAR_COLOR]);
    CHECK(gpu_pgraph_state_now(pgraph)->output[GPU_PGRAPH_OUT_CLEAR_COLOR] == 0u);
    gpu_pgraph_set_output_groups(pgraph, 0u);
    gpu_pgraph_reset(pgraph);
    /* Inside a bracket neither the software method nor the semaphore release is measured. */
    const gpu_pgraph_command open_bracket[] = {{GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS}};
    CHECK(decode_pairs(pgraph, open_bracket, 1u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_decode_other_subchannel(pgraph, 5u, 0x310u, 0u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(pgraph), "BEGIN_END") != NULL);
    const gpu_pgraph_command release[] = {{0x1D70u, 1u}};
    CHECK(decode_pairs(pgraph, release, 1u) == GPU_PGRAPH_ERR_UNMEASURED);
    gpu_pgraph_destroy(pgraph);
}

static void test_unhandled_methods(void)
{
    printf("test_unhandled_methods\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    /* a texture method, a clear and the semaphore release: real, measured elsewhere, not drawn. T391: the
     * semaphore release is now a HANDLED no-op (the fence packet carries it), the other two stay unhandled. */
    const gpu_pgraph_command others[] = {
        {0x1B00u, 1u}, {0x1D94u, 0xF0u}, {0x1B00u, 2u}, {0x1D70u, 5u}, {GPU_PGRAPH_PROGRAM_START, 2u},
    };
    CHECK(decode_pairs(pgraph, others, 5u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(pgraph) == 2u);
    CHECK(gpu_pgraph_get_stats(pgraph).semaphore_releases == 1u);
    uint32_t method = 0u;
    uint64_t pairs = 0u;
    gpu_pgraph_unhandled_at(pgraph, 0u, &method, &pairs);
    CHECK(method == 0x1B00u && pairs == 2u);
    gpu_pgraph_unhandled_at(pgraph, 1u, &method, &pairs);
    CHECK(method == 0x1D94u && pairs == 1u);
    gpu_pgraph_unhandled_at(pgraph, 2u, &method, &pairs);
    CHECK(method == 0u && pairs == 0u);
    CHECK(gpu_pgraph_get_stats(pgraph).pairs == 5u && gpu_pgraph_get_stats(pgraph).pairs_handled == 2u);
    /* strict refuses the first unhandled pair and counts nothing for it */
    gpu_pgraph_reset(pgraph);
    gpu_pgraph_set_strict(pgraph, true);
    CHECK(decode_pairs(pgraph, others, 5u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(pgraph), "0x1B00") != NULL && strstr(gpu_pgraph_error(pgraph), "pair 0") != NULL);
    CHECK(gpu_pgraph_get_stats(pgraph).pairs == 0u);
    gpu_pgraph_destroy(pgraph);
}

/* T521. 0x1E98 is the second dword of the CreateDevice execution-mode packet `0x00081E94, 6, 0` (MEASURED: the title's
 * four count-2 emitters write 0 or flags & 1, the real stream carries 0). Only 0 is decoded: kept, counted, no snapshot,
 * no pixel. MUTATION: dropping the case (the pair becomes unhandled), counting it as a handled pair of another kind,
 * accepting a nonzero value, taking a snapshot for it or not refusing it inside a bracket all fail here. */
static void test_cxt_write_en(void)
{
    printf("test_cxt_write_en\n");
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_strict(pgraph, true);
    const gpu_pgraph_command packet[] = {{GPU_PGRAPH_EXECUTION_MODE, 6u}, {GPU_PGRAPH_CXT_WRITE_EN, 0u}};
    CHECK(GPU_PGRAPH_CXT_WRITE_EN == 0x1E98u);
    CHECK(decode_pairs(pgraph, packet, 2u) == GPU_PGRAPH_OK);
    const gpu_pgraph_stats stats = gpu_pgraph_get_stats(pgraph);
    CHECK(stats.pairs == 2u && stats.pairs_handled == 2u);
    CHECK(stats.cxt_write_en_pairs == 1u); /* non-empty before any equality below */
    CHECK(gpu_pgraph_unhandled_count(pgraph) == 0u);
    const gpu_pgraph_state *state = gpu_pgraph_state_now(pgraph);
    CHECK(state->cxt_write_en_set && state->cxt_write_en == 0u);
    CHECK(state->execution_mode_set && state->execution_mode == 6u);
    /* a pixel-free word: it forces no snapshot (the mode word does, 0x1E98 alone does not) */
    gpu_pgraph_reset(pgraph);
    const gpu_pgraph_command lone[] = {{GPU_PGRAPH_CXT_WRITE_EN, 0u}};
    CHECK(decode_pairs(pgraph, lone, 1u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_state_now(pgraph)->cxt_write_en_set && !gpu_pgraph_state_now(pgraph)->execution_mode_set);
    CHECK(gpu_pgraph_get_stats(pgraph).cxt_write_en_pairs == 1u);
    /* a 0x1E98 write between two draws leaves both on ONE snapshot, and the first draw is untouched by it */
    gpu_pgraph_reset(pgraph);
    {
        stream_builder stream = {0};
        build_setup(&stream, 0x1000u);
        stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
        stream_pair(&stream, GPU_PGRAPH_CXT_WRITE_EN, 0u);
        stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
        CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_draw_count(pgraph) == 2u);
        const gpu_pgraph_draw *first = gpu_pgraph_draw_at(pgraph, 0u);
        const gpu_pgraph_draw *second = gpu_pgraph_draw_at(pgraph, 1u);
        CHECK(first != NULL && second != NULL);
        if (first != NULL && second != NULL) {
            CHECK(first->snapshot == second->snapshot && gpu_pgraph_snapshot_count(pgraph) == 1u);
            CHECK(!gpu_pgraph_snapshot(pgraph, first->snapshot)->cxt_write_en_set);
        }
        CHECK(gpu_pgraph_get_stats(pgraph).cxt_write_en_pairs == 1u);
        stream_free(&stream);
    }
    /* reset clears the word and the counter */
    gpu_pgraph_reset(pgraph);
    CHECK(!gpu_pgraph_state_now(pgraph)->cxt_write_en_set && gpu_pgraph_get_stats(pgraph).cxt_write_en_pairs == 0u);
    /* 1 (a vertex program could write constants, not modelled) and every other value are refused, strict or not,
     * and counted nowhere. */
    static const uint32_t refused[] = {1u, 2u, 0x80000000u, 0xFFFFFFFFu};
    for (size_t i = 0u; i < sizeof refused / sizeof refused[0]; i++) {
        for (int strict = 0; strict < 2; strict++) {
            gpu_pgraph_reset(pgraph);
            gpu_pgraph_set_strict(pgraph, strict != 0);
            const gpu_pgraph_command bad[] = {{GPU_PGRAPH_EXECUTION_MODE, 6u}, {GPU_PGRAPH_CXT_WRITE_EN, refused[i]}};
            CHECK(decode_pairs(pgraph, bad, 2u) == GPU_PGRAPH_ERR_UNMEASURED);
            CHECK(strstr(gpu_pgraph_error(pgraph), "pair 1 method 0x1E98") != NULL);
            CHECK(gpu_pgraph_get_stats(pgraph).cxt_write_en_pairs == 0u);
            CHECK(!gpu_pgraph_state_now(pgraph)->cxt_write_en_set);
        }
    }
    /* the neighbours stay as they were: 0x1E90 (LAUNCH_TRANSFORM_PROGRAM) and 0x1E9A are not decoded */
    gpu_pgraph_reset(pgraph);
    gpu_pgraph_set_strict(pgraph, true);
    const gpu_pgraph_command neighbour[] = {{0x1E90u, 0u}};
    CHECK(decode_pairs(pgraph, neighbour, 1u) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(gpu_pgraph_error(pgraph), "0x1E90") != NULL);
    gpu_pgraph_set_strict(pgraph, false);
    gpu_pgraph_reset(pgraph);
    CHECK(decode_pairs(pgraph, neighbour, 1u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_unhandled_count(pgraph) == 1u && gpu_pgraph_get_stats(pgraph).cxt_write_en_pairs == 0u);
    gpu_pgraph_destroy(pgraph);
}

static void test_triangulate(void)
{
    printf("test_triangulate\n");
    static const uint32_t ids[] = {10, 11, 12, 13, 14, 15, 16};
    uint32_t out[64];
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_TRIANGLES, ids, 7u, out, 64u) == 6u);
    CHECK(out[0] == 10u && out[5] == 15u);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_TRIANGLES, ids, 2u, out, 64u) == 0u);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_TRIANGLE_STRIP, ids, 5u, out, 64u) == 9u);
    static const uint32_t strip[] = {10, 11, 12, 12, 11, 13, 12, 13, 14};
    CHECK(memcmp(out, strip, sizeof strip) == 0);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_TRIANGLE_FAN, ids, 5u, out, 64u) == 9u);
    static const uint32_t fan[] = {10, 11, 12, 10, 12, 13, 10, 13, 14};
    CHECK(memcmp(out, fan, sizeof fan) == 0);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_QUADS, ids, 7u, out, 64u) == 6u);
    static const uint32_t quad[] = {10, 11, 12, 10, 12, 13};
    CHECK(memcmp(out, quad, sizeof quad) == 0);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_QUADS, ids, 3u, out, 64u) == 0u);
    CHECK(gpu_pgraph_triangulate(GPU_PGRAPH_OP_TRIANGLES, ids, 6u, out, 5u) == UINT32_MAX);
    static const uint32_t refused[] = {GPU_PGRAPH_OP_END, GPU_PGRAPH_OP_POINTS, GPU_PGRAPH_OP_LINES,
                                       GPU_PGRAPH_OP_LINE_LOOP, GPU_PGRAPH_OP_LINE_STRIP,
                                       GPU_PGRAPH_OP_QUAD_STRIP, GPU_PGRAPH_OP_POLYGON, 11u};
    for (size_t i = 0u; i < sizeof refused / sizeof refused[0]; i++) {
        CHECK(gpu_pgraph_triangulate(refused[i], ids, 6u, out, 64u) == UINT32_MAX);
    }
}

/* ---- program resolution through the selector --------------------------------------------- */

static const char *const names_static[] = {"generated_ff", SYNTHETIC_PROGRAM_NAME};
static const char *const names_generated[] = {
    "generated_3ebe9b7e7c96baecc4c329dd008c862963e3d94b1a62ff2636e179b8ddc456aa"};
static const char *const names_two[] = {
    "static_d19c30fbe6fe5143bf612d338ded31ff276a99176111d0680594695319b1d1d4"};
static const struct gpu_vsh_table table_static = {0u, 0u, NULL, 0u, NULL, 2u, names_static};
static const struct gpu_vsh_table table_generated = {0u, 0u, NULL, 0u, NULL, 1u, names_generated};
static const struct gpu_vsh_table table_two = {0u, 0u, NULL, 0u, NULL, 1u, names_two};

static void bound_program(gpu_pgraph *pgraph, uint32_t slot, const uint32_t *words, uint32_t instructions,
                          bool set_start, uint32_t mode)
{
    stream_builder stream = {0};
    if (mode != 0xFFFFFFFFu) {
        stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, mode);
    }
    stream_program(&stream, slot, words, instructions);
    if (set_start) {
        stream_pair(&stream, GPU_PGRAPH_PROGRAM_START, slot);
    }
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
}

static void test_resolve_program(void)
{
    printf("test_resolve_program\n");
    gpu_pgraph_backend backend = {0};
    backend.table = &table_static;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph_program program;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);

    gpu_pgraph *pgraph = gpu_pgraph_create();
    bound_program(pgraph, 5u, synthetic_program, 1u, true, 6u);
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_OK);
    CHECK(program.module == 1u && program.is_static && program.instructions == 1u);
    const char *synthetic_name = SYNTHETIC_PROGRAM_NAME;
    CHECK(strcmp(program.digest, synthetic_name + 7) == 0);

    /* the same bytes in another slot are the same program */
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 40u, synthetic_program, 1u, true, 6u);
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_OK);
    CHECK(program.module == 1u);

    /* generated table: same digest under the other prefix */
    backend.table = &table_generated;
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_OK);
    CHECK(program.module == 0u && !program.is_static);

    /* exactness: one flipped bit of one dword is a different program, a loud miss with the digest */
    uint32_t flipped[4];
    memcpy(flipped, synthetic_program, sizeof flipped);
    flipped[1] ^= 0x100u;
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 5u, flipped, 1u, true, 6u);
    backend.table = &table_static;
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "neither the static nor the generated table") != NULL);
    CHECK(strstr(report.error, "sha256 ") != NULL);

    /* the length comes from the FINAL bit: a program followed by a stale instruction still
     * resolves to its own length, not to the end of what was uploaded */
    const uint32_t two[8] = {0xAAAA0001u, 0xBBBB0002u, 0xCCCC0003u, 0x00000000u,
                             0xDDDD0004u, 0xEEEE0005u, 0xFFFF0006u, 0x00000001u};
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 3u, two, 2u, true, 6u);
    backend.table = &table_two;
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_OK);
    CHECK(program.instructions == 2u && program.module == 0u);
    /* starting one slot later drops the first instruction: a different, unlisted program */
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 3u, two, 2u, false, 6u);
    {
        const gpu_pgraph_command start[] = {{GPU_PGRAPH_PROGRAM_START, 4u}};
        CHECK(gpu_pgraph_decode(pgraph, start, 1u) == GPU_PGRAPH_OK);
    }
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);

    /* refusals: no FINAL bit, an unwritten slot, no start, not program mode, no mode, no inference */
    backend.table = &table_static;
    gpu_pgraph_reset(pgraph);
    uint32_t no_final[4];
    memcpy(no_final, synthetic_program, sizeof no_final);
    no_final[3] &= ~1u;
    bound_program(pgraph, 5u, no_final, 1u, true, 6u);
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_ERR_MALFORMED);
    CHECK(strstr(report.error, "FINAL") != NULL || strstr(report.error, "never completely") != NULL);
    /* three dwords of a four dword instruction are not an instruction */
    gpu_pgraph_reset(pgraph);
    {
        stream_builder partial = {0};
        stream_pair(&partial, GPU_PGRAPH_EXECUTION_MODE, 6u);
        stream_pair(&partial, GPU_PGRAPH_PROGRAM_LOAD, 5u);
        stream_run(&partial, GPU_PGRAPH_PROGRAM_DATA, synthetic_program, 3u);
        stream_pair(&partial, GPU_PGRAPH_PROGRAM_START, 5u);
        CHECK(gpu_pgraph_decode(pgraph, partial.pairs, partial.count) == GPU_PGRAPH_OK);
        CHECK(!gpu_pgraph_state_now(pgraph)->slot_written[5]);
        CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
              GPU_PGRAPH_ERR_MALFORMED);
        CHECK(strstr(report.error, "never completely written") != NULL);
        stream_free(&partial);
    }
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 5u, synthetic_program, 1u, false, 6u);
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "start slot") != NULL);
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 5u, synthetic_program, 1u, true, 0u); /* mode 0: fixed function */
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "fixed-function") != NULL);
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 5u, synthetic_program, 1u, true, 0xFFFFFFFFu);
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "never set") != NULL);
    /* T441: with INFER_EXECUTION_MODE_UNWRITTEN a stream that never wrote the mode but started a program
     * resolves (and says the mode was inferred), a mode that WAS written still decides, and a stream with no
     * start slot stays refused. Not part of INFER_ALL: ALL alone keeps the refusal above. */
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN;
    CHECK((GPU_PGRAPH_INFER_ALL & GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN) == 0u);
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 5u, synthetic_program, 1u, true, 0xFFFFFFFFu);
    memset(&program, 0, sizeof program);
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_OK);
    CHECK(program.mode_inferred && program.module == 1u && program.instructions == 1u);
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 5u, synthetic_program, 1u, true, 6u);
    program.mode_inferred = true; /* stale: the resolver must clear it */
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_OK);
    CHECK(!program.mode_inferred);
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 5u, synthetic_program, 1u, true, 0u); /* a written fixed-function mode stays refused */
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "fixed-function") != NULL);
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 5u, synthetic_program, 1u, false, 0xFFFFFFFFu); /* no mode and no start */
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "never set") != NULL);
    gpu_pgraph_reset(pgraph);
    bound_program(pgraph, 5u, synthetic_program, 1u, true, 6u);
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL & ~GPU_PGRAPH_INFER_PROGRAM_HEADER;
    CHECK(gpu_pgraph_resolve_program(gpu_pgraph_state_now(pgraph), &backend, &program, &report) ==
          GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "INFERRED") != NULL);
    gpu_pgraph_destroy(pgraph);
}

/* ---- vertex assembly --------------------------------------------------------------------- */

typedef struct {
    float position[3];
    uint8_t colour[4]; /* B, G, R, A in memory: the D3DCOLOR 0xAARRGGBB */
} vertex20;

/* T847: a program in no table goes to the maker, once, and the lookup is retried. */
typedef struct {
    unsigned calls;
    bool answer;
    bool register_module;
    size_t bytes;
    uint8_t program[4u + 16u];
    char name[96];
    const char *names[2];
    struct gpu_vsh_table table;
} program_maker_probe;

static bool program_probe_make(void *context, bool fragment, const char *name, const uint8_t *bytes, size_t byte_count, char *error,
                               size_t error_bytes)
{
    program_maker_probe *probe = context;
    probe->calls++;
    CHECK(!fragment);
    probe->bytes = byte_count;
    if (byte_count == sizeof probe->program) {
        memcpy(probe->program, bytes, byte_count);
    }
    snprintf(probe->name, sizeof probe->name, "%s", name);
    if (!probe->answer) {
        snprintf(error, error_bytes, "no translator here");
        return false;
    }
    if (probe->register_module) {
        probe->names[probe->table.module_count++] = probe->name;
    }
    return true;
}

static void test_resolve_program_makes_a_missing_module(void)
{
    printf("test_resolve_program_makes_a_missing_module\n");
    program_maker_probe probe;
    memset(&probe, 0, sizeof probe);
    probe.table.module_names = probe.names;
    gpu_pgraph_backend backend = {0};
    backend.table = &probe.table;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph_program program;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    gpu_pgraph *pgraph = gpu_pgraph_create();
    bound_program(pgraph, 5u, synthetic_program, 1u, true, 6u);
    const gpu_pgraph_state *state = gpu_pgraph_state_now(pgraph);

    /* no maker: the old refusal, the maker is not involved */
    CHECK(gpu_pgraph_resolve_program(state, &backend, &program, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "neither the static nor the generated table") != NULL && strstr(report.error, "translating") == NULL);
    CHECK(probe.calls == 0u);

    backend.make_module = program_probe_make;
    backend.make_module_context = &probe;
    probe.answer = false;
    CHECK(gpu_pgraph_resolve_program(state, &backend, &program, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(probe.calls == 1u);
    CHECK(strstr(report.error, "neither the static nor the generated table, and translating it failed: no translator here") != NULL);

    probe.answer = true; /* says yes but registers nothing: the retry misses */
    CHECK(gpu_pgraph_resolve_program(state, &backend, &program, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(probe.calls == 2u);

    probe.register_module = true;
    CHECK(gpu_pgraph_resolve_program(state, &backend, &program, &report) == GPU_PGRAPH_OK);
    CHECK(probe.calls == 3u && program.module == 0u && !program.is_static && program.instructions == 1u);
    /* the maker got the generated_ name of the program's own digest and the bytes that digest was taken over */
    char expected[96];
    snprintf(expected, sizeof expected, "generated_%s", program.digest);
    CHECK(strcmp(probe.name, expected) == 0);
    CHECK(probe.bytes == 20u && probe.program[0] == 0x78u && probe.program[1] == 0x20u && probe.program[2] == 1u &&
          probe.program[3] == 0u);
    for (uint32_t i = 0u; i < 4u; i++) {
        const uint32_t word = synthetic_program[i];
        CHECK(probe.program[4u + i * 4u] == (uint8_t)word && probe.program[7u + i * 4u] == (uint8_t)(word >> 24));
    }
    /* now in the table: found by name, the maker is not asked again */
    CHECK(gpu_pgraph_resolve_program(state, &backend, &program, &report) == GPU_PGRAPH_OK);
    CHECK(probe.calls == 3u);

    /* a static hit never reaches the maker either */
    program_maker_probe other;
    memset(&other, 0, sizeof other);
    backend.table = &table_static;
    backend.make_module_context = &other;
    CHECK(gpu_pgraph_resolve_program(state, &backend, &program, &report) == GPU_PGRAPH_OK && program.is_static);
    CHECK(other.calls == 0u);
    gpu_pgraph_destroy(pgraph);
}

static void test_assemble(void)
{
    printf("test_assemble\n");
    static const vertex20 vertices[4] = {
        {{1.0f, 2.0f, 3.0f}, {0x10u, 0x20u, 0x30u, 0xFFu}},
        {{4.0f, 5.0f, 6.0f}, {0x00u, 0x00u, 0xFFu, 0x80u}},
        {{7.0f, 8.0f, 9.0f}, {0xFFu, 0x00u, 0x00u, 0x00u}},
        {{-1.0f, -2.0f, -3.0f}, {0x01u, 0x02u, 0x03u, 0x04u}},
    };
    uint8_t memory[sizeof vertices];
    memcpy(memory, vertices, sizeof vertices);
    fake_guest guest = {0x00500000u, memory, sizeof memory};
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read;
    backend.context = &guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;

    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    static const float offset[4] = {320.0f, 240.0f, 0.0f, 0.0f};
    static const float scale[4] = {320.0f, -240.0f, 255.0f, 0.0f};
    stream_viewport(&stream, offset, scale);
    stream_array(&stream, 1u, guest.base + offsetof(vertex20, position), array_format(sizeof(vertex20), 3u, GPU_PGRAPH_TYPE_F));
    stream_array(&stream, 2u, guest.base + offsetof(vertex20, colour), array_format(sizeof(vertex20), 4u, GPU_PGRAPH_TYPE_UB_D3D));
    static const float c5[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    stream_constants(&stream, 5u, c5, 4u);
    /* the draw reads vertices 2, 0, 1 as a triangle list: index order, not memory order */
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT16, (0u << 16) | 2u);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, 1u);
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, 0u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u);

    gpu_pgraph_assembled assembled;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.vertex_count == 3u && assembled.attributes != NULL);
    if (assembled.attributes != NULL && assembled.vertex_count == 3u) {
        static const uint32_t order[3] = {2u, 0u, 1u};
        for (uint32_t v = 0u; v < 3u; v++) {
            const float *slots = assembled.attributes + v * GPU_VSH_ATTRIBUTE_FLOATS;
            const vertex20 *source = &vertices[order[v]];
            /* slot 1: the three floats, w defaulted to 1 */
            CHECK(slots[4] == source->position[0] && slots[5] == source->position[1] &&
                  slots[6] == source->position[2] && slots[7] == 1.0f);
            /* slot 2: (R, G, B, A) / 255 from bytes B, G, R, A */
            CHECK(slots[8] == (float)source->colour[2] / 255.0f &&
                  slots[9] == (float)source->colour[1] / 255.0f &&
                  slots[10] == (float)source->colour[0] / 255.0f &&
                  slots[11] == (float)source->colour[3] / 255.0f);
            /* slot 0 is not enabled: (0, 0, 0, 1) */
            CHECK(slots[0] == 0.0f && slots[3] == 1.0f);
        }
        CHECK(assembled.attributes[8] == 0.0f && assembled.attributes[10] == 1.0f); /* vertex 2 is B=255 */
    }
    CHECK(assembled.constants[5u * 4u] == 5.0f && assembled.constants[5u * 4u + 3u] == 8.0f);
    /* the viewport registers reach constants 58 (scale) and 59 (offset), INFERRED */
    CHECK(assembled.constants[58u * 4u + 1u] == -240.0f && assembled.constants[59u * 4u] == 320.0f);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_VIEWPORT_CONSTANTS) != 0u);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_D3DCOLOR_ORDER) != 0u);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_COMPONENT_DEFAULTS) != 0u);
    gpu_pgraph_assembled_free(&assembled);

    /* every inference is refused on its own when it is the only one withheld */
    static const struct {
        uint32_t bit;
        const char *text;
    } needs[] = {
        {GPU_PGRAPH_INFER_VIEWPORT_CONSTANTS, "viewport registers"},
        {GPU_PGRAPH_INFER_D3DCOLOR_ORDER, "D3DCOLOR"},
        {GPU_PGRAPH_INFER_COMPONENT_DEFAULTS, "(0, 0, 0, 1)"},
    };
    for (size_t i = 0u; i < sizeof needs / sizeof needs[0]; i++) {
        gpu_pgraph_backend strict = backend;
        strict.allowed_inferences = GPU_PGRAPH_INFER_ALL & ~needs[i].bit;
        CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &strict, &assembled, &report) ==
              GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(strstr(report.error, needs[i].text) != NULL && strstr(report.error, "not allowed") != NULL);
        CHECK(assembled.attributes == NULL);
    }
    /* a read that fails is refused, naming the slot */
    {
        fake_guest short_guest = {guest.base, memory, 30u};
        gpu_pgraph_backend cut = backend;
        cut.context = &short_guest;
        CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &cut, &assembled, &report) == GPU_PGRAPH_ERR_MALFORMED);
        CHECK(strstr(report.error, "guest read") != NULL);
    }
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* types without a measured conversion, a missing address, a size above 4: refused */
    static const struct {
        uint32_t format;
        bool address;
        const char *text;
    } bad[] = {
        {ARRAY_FORMAT(8u, 3u, GPU_PGRAPH_TYPE_S32K), true, "S32K"}, /* only size 2 is converted (T84d) */
        {ARRAY_FORMAT(8u, 3u, GPU_PGRAPH_TYPE_CMP), true, "CMP"},
        {ARRAY_FORMAT(8u, 4u, GPU_PGRAPH_TYPE_S1), true, "S1"},
        {ARRAY_FORMAT(8u, 4u, GPU_PGRAPH_TYPE_UB_OGL), true, "UB_OGL"},
        {ARRAY_FORMAT(8u, 3u, GPU_PGRAPH_TYPE_UB_D3D), true, "UB_D3D"},
        {ARRAY_FORMAT(8u, 5u, GPU_PGRAPH_TYPE_F), true, "no measured conversion"},
        {ARRAY_FORMAT(12u, 3u, GPU_PGRAPH_TYPE_F), false, "no address"},
    };
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i++) {
        gpu_pgraph *odd = gpu_pgraph_create();
        stream_builder one = {0};
        if (bad[i].address) {
            stream_array(&one, 3u, guest.base, bad[i].format);
        } else {
            stream_pair(&one, GPU_PGRAPH_ARRAY_FORMAT + 12u, bad[i].format);
        }
        stream_draw_arrays(&one, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
        CHECK(gpu_pgraph_decode(odd, one.pairs, one.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_assemble_draw(odd, 0u, &backend, &assembled, &report) != GPU_PGRAPH_OK);
        CHECK(strstr(report.error, bad[i].text) != NULL);
        stream_free(&one);
        gpu_pgraph_destroy(odd);
    }
}

static void test_stride_zero_and_stride(void)
{
    printf("test_stride_zero_and_stride\n");
    float memory[8];
    for (uint32_t i = 0u; i < 8u; i++) {
        memory[i] = (float)(i + 1u);
    }
    fake_guest guest = {0x1000u, (uint8_t *)memory, sizeof memory};
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read;
    backend.context = &guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    stream_array(&stream, 1u, 0x1000u, array_format(8u, 2u, GPU_PGRAPH_TYPE_F));  /* walks */
    stream_array(&stream, 2u, 0x1010u, array_format(0u, 4u, GPU_PGRAPH_TYPE_F));  /* constant */
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 1u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    gpu_pgraph_assembled assembled;
    gpu_pgraph_report report;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.vertex_count == 3u);
    if (assembled.vertex_count == 3u) {
        for (uint32_t v = 0u; v < 3u; v++) {
            const float *slots = assembled.attributes + v * GPU_VSH_ATTRIBUTE_FLOATS;
            /* vertex index 1 + v at stride 8 bytes: floats 2 + 2v and 3 + 2v */
            CHECK(slots[4] == (float)(3u + 2u * v) && slots[5] == (float)(4u + 2u * v));
            CHECK(slots[6] == 0.0f && slots[7] == 1.0f);
            CHECK(slots[8] == 5.0f && slots[11] == 8.0f); /* stride 0: the same four floats */
        }
    }
    gpu_pgraph_assembled_free(&assembled);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* T477: when no viewport words/rows were recorded, target dimensions can seed the
     * measured D3D whole-target mapping only behind its separately named inference. */
    pgraph = gpu_pgraph_create();
    memset(&stream, 0, sizeof stream);
    stream_array(&stream, 1u, 0x1000u, array_format(4u, 1u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    backend.viewport_from_target = true;
    backend.viewport_width = 640u;
    backend.viewport_height = 480u;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL |
                                 GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET) != 0u);
    CHECK(assembled.constants[58u * 4u] == 320.0f && assembled.constants[58u * 4u + 1u] == -240.0f);
    CHECK(assembled.constants[59u * 4u] == 320.0f && assembled.constants[59u * 4u + 1u] == 240.0f);
    gpu_pgraph_assembled_free(&assembled);

    /* T560: modules that convert window coordinates with c58/c59 are drawn only with a viewport scale in c58 */
    backend.window_clip_modules = true;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.constants[58u * 4u] == 320.0f); /* from-target still seeds, so the draw is allowed */
    gpu_pgraph_assembled_free(&assembled);
    backend.viewport_from_target = false;
    memset(&report, 0, sizeof report);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "window-to-clip") != NULL && strstr(report.error, "refused") != NULL);
    backend.window_clip_modules = false;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.constants[58u * 4u] == 0.0f && assembled.constants[58u * 4u + 1u] == 0.0f);
    gpu_pgraph_assembled_free(&assembled);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* T560. MEASURED in the movie loop: the stream writes 0x0A20 = (0.53125, 0.53125, 0, 0) and no 0x0AF0 for the first
 * frames. The register feed alone leaves c58 zero and a lone offset in c59, which the window-to-clip modules cannot
 * convert with: they take the whole-target pair instead, announced, and refuse a draw that has no scale at all. */
static void test_window_clip_takes_the_target_pair_when_the_registers_are_half_written(void)
{
    printf("test_window_clip_takes_the_target_pair_when_the_registers_are_half_written\n");
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read;
    uint8_t memory[16] = {0};
    fake_guest guest = {0x100u, memory, sizeof memory};
    backend.context = &guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET;
    backend.viewport_width = 640u;
    backend.viewport_height = 480u;
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    for (uint32_t i = 0u; i < 2u; i++) {
        stream_pair(&stream, GPU_PGRAPH_VIEWPORT_OFFSET + 4u * i, float_bits_of(0.53125f));
    }
    stream_array(&stream, 1u, 0x100u, array_format(4u, 1u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    gpu_pgraph_assembled assembled;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    /* no flag: the lone offset reaches c59 and c58 stays zero, exactly as before (T96 register feed) */
    backend.viewport_from_target = true;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.constants[58u * 4u] == 0.0f && assembled.constants[59u * 4u] == 0.53125f);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET) == 0u);
    gpu_pgraph_assembled_free(&assembled);
    /* the flag: the pair comes from the target and the lone offset is replaced */
    backend.window_clip_modules = true;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.constants[58u * 4u] == 320.0f && assembled.constants[58u * 4u + 1u] == -240.0f);
    CHECK(assembled.constants[59u * 4u] == 320.0f && assembled.constants[59u * 4u + 1u] == 240.0f);
    CHECK(assembled.constants[58u * 4u + 2u] == 1.0f && assembled.constants[58u * 4u + 3u] == 0.0f);
    CHECK(assembled.constants[59u * 4u + 2u] == 0.0f && assembled.constants[59u * 4u + 3u] == 0.0f);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET) != 0u);
    gpu_pgraph_assembled_free(&assembled);
    /* the inference is announced, so withholding it refuses with its name */
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "window-to-clip") != NULL);
    /* without the target option there is no scale at all: refused, not drawn raw */
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET;
    backend.viewport_from_target = false;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "no viewport scale") != NULL);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* a measured scale (here a row 58 write, as the movie loop's later frames carry) is never replaced */
    pgraph = gpu_pgraph_create();
    memset(&stream, 0, sizeof stream);
    static const float rows[8] = {320.0f, -240.0f, 1.0f, 1.0f, 320.53125f, 240.53125f, 0.0f, 0.0f};
    stream_constants(&stream, 58u, rows, 8u);
    stream_array(&stream, 1u, 0x100u, array_format(4u, 1u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    backend.viewport_from_target = true;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.constants[59u * 4u] == 320.53125f && (assembled.used_inferences & GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET) == 0u);
    gpu_pgraph_assembled_free(&assembled);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* a full register pair is a viewport: the registers feed c58 and c59, the target never replaces them */
    pgraph = gpu_pgraph_create();
    memset(&stream, 0, sizeof stream);
    static const float register_offset[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    static const float register_scale[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    stream_viewport(&stream, register_offset, register_scale);
    stream_array(&stream, 1u, 0x100u, array_format(4u, 1u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.constants[58u * 4u] == 5.0f && assembled.constants[59u * 4u] == 1.0f);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET) == 0u);
    gpu_pgraph_assembled_free(&assembled);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* only row 59 written, plus the lone 0x0A20: the scale comes from the target, the written row 59 is kept */
    pgraph = gpu_pgraph_create();
    memset(&stream, 0, sizeof stream);
    static const float row59[4] = {9.0f, 8.0f, 7.0f, 6.0f};
    stream_pair(&stream, GPU_PGRAPH_VIEWPORT_OFFSET, float_bits_of(0.53125f));
    stream_constants(&stream, 59u, row59, 4u);
    stream_array(&stream, 1u, 0x100u, array_format(4u, 1u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.constants[58u * 4u] == 320.0f && assembled.constants[59u * 4u] == 9.0f);
    gpu_pgraph_assembled_free(&assembled);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* a scale with one zero component is still a scale (the module's guard passes that axis through): not refused */
    pgraph = gpu_pgraph_create();
    memset(&stream, 0, sizeof stream);
    static const float half_zero[4] = {5.0f, 0.0f, 1.0f, 1.0f};
    stream_constants(&stream, 58u, half_zero, 4u);
    stream_array(&stream, 1u, 0x100u, array_format(4u, 1u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    backend.viewport_from_target = false;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    gpu_pgraph_assembled_free(&assembled);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
    pgraph = gpu_pgraph_create();
    memset(&stream, 0, sizeof stream);
    static const float other_half_zero[4] = {0.0f, 5.0f, 1.0f, 1.0f};
    stream_constants(&stream, 58u, other_half_zero, 4u);
    stream_array(&stream, 1u, 0x100u, array_format(4u, 1u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    gpu_pgraph_assembled_free(&assembled);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

static void test_stream_written_viewport_constants_win(void)
{
    printf("test_stream_written_viewport_constants_win\n");
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read;
    uint8_t memory[16] = {0};
    fake_guest guest = {0x100u, memory, sizeof memory};
    backend.context = &guest;
    /* only the measured path is allowed: the viewport inference is withheld, rows written directly */
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL & ~GPU_PGRAPH_INFER_VIEWPORT_CONSTANTS;
    gpu_pgraph *pgraph = gpu_pgraph_create();
    stream_builder stream = {0};
    static const float offset[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    static const float scale[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    stream_viewport(&stream, offset, scale);
    static const float direct[8] = {50.0f, 60.0f, 70.0f, 80.0f, 10.0f, 20.0f, 30.0f, 40.0f};
    stream_constants(&stream, 58u, direct, 8u);
    stream_array(&stream, 1u, 0x100u, array_format(4u, 1u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    gpu_pgraph_assembled assembled;
    gpu_pgraph_report report;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK((assembled.used_inferences & GPU_PGRAPH_INFER_VIEWPORT_CONSTANTS) == 0u);
    CHECK(assembled.constants[58u * 4u] == 50.0f && assembled.constants[59u * 4u] == 10.0f);
    gpu_pgraph_assembled_free(&assembled);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* only the scale row written directly: the offset register still needs the inference */
    pgraph = gpu_pgraph_create();
    stream_viewport(&stream, offset, scale);
    stream_constants(&stream, 58u, direct, 4u);
    stream_array(&stream, 1u, 0x100u, array_format(4u, 1u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "viewport registers") != NULL);
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(assembled.constants[58u * 4u] == 50.0f);  /* the measured write wins over the register */
    CHECK(assembled.constants[59u * 4u] == 1.0f);   /* offset row from the register */
    gpu_pgraph_assembled_free(&assembled);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* --- T262: the vertex snapshot ---------------------------------------------------------------- */

#define SNAP_BASE 0x4000u

typedef struct {
    fake_guest guest;
    unsigned reads;
    size_t bytes;
} counting_guest;

static bool counting_read(void *context, uint32_t address, void *out, size_t bytes)
{
    counting_guest *counting = context;
    counting->reads++;
    counting->bytes += bytes;
    return fake_guest_read(&counting->guest, address, out, bytes);
}

static void put_floats(uint8_t *memory, uint32_t byte_offset, float first, uint32_t count)
{
    for (uint32_t i = 0u; i < count; i++) {
        const float value = first + (float)i;
        memcpy(memory + byte_offset + i * 4u, &value, sizeof value);
    }
}

static gpu_pgraph_backend snapshot_backend(fake_guest *guest)
{
    gpu_pgraph_backend backend = {0};
    backend.read_guest = fake_guest_read; /* the LIVE memory: what a replay at the present would see */
    backend.context = guest;
    backend.allowed_inferences = GPU_PGRAPH_INFER_ALL;
    return backend;
}

static float attribute_of(const gpu_pgraph_assembled *assembled, uint32_t vertex, uint32_t slot,
                          uint32_t lane)
{
    return assembled->attributes[(size_t)vertex * GPU_VSH_ATTRIBUTE_FLOATS + slot * 4u + lane];
}

/* The bug: a buffer rewritten between two draws. Each draw must replay its OWN bytes. */
static void test_snapshot_survives_an_overwrite(void)
{
    printf("test_snapshot_survives_an_overwrite\n");
    uint8_t memory[256] = {0};
    fake_guest guest = {SNAP_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = snapshot_backend(&guest);
    stream_builder first = {0};
    stream_array(&first, 1u, SNAP_BASE, array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&first, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    stream_builder second = {0};
    stream_draw_arrays(&second, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u); /* same arrays, still bound */

    for (int captured = 0; captured < 2; captured++) {
        gpu_pgraph *pgraph = gpu_pgraph_create();
        CHECK(!gpu_pgraph_vertex_capture_enabled(pgraph));
        if (captured != 0) {
            gpu_pgraph_set_vertex_capture(pgraph, fake_guest_read, &guest, 1024u);
            CHECK(gpu_pgraph_vertex_capture_enabled(pgraph));
        }
        put_floats(memory, 0u, 1.0f, 9u); /* triangle A */
        CHECK(gpu_pgraph_decode(pgraph, first.pairs, first.count) == GPU_PGRAPH_OK);
        put_floats(memory, 0u, 11.0f, 9u); /* the title rewrites the buffer for the next draw */
        CHECK(gpu_pgraph_decode(pgraph, second.pairs, second.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_draw_count(pgraph) == 2u);
        gpu_pgraph_assembled one;
        gpu_pgraph_assembled two;
        gpu_pgraph_report report;
        CHECK(gpu_pgraph_assemble_draw(pgraph, 0u, &backend, &one, &report) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_assemble_draw(pgraph, 1u, &backend, &two, &report) == GPU_PGRAPH_OK);
        CHECK(one.vertex_count == 3u && two.vertex_count == 3u && one.attributes != NULL &&
              two.attributes != NULL);
        if (one.attributes != NULL && two.attributes != NULL) {
            /* draw 2 always sees the rewritten bytes */
            CHECK(attribute_of(&two, 0u, 1u, 0u) == 11.0f && attribute_of(&two, 2u, 1u, 2u) == 19.0f);
            if (captured != 0) {
                CHECK(attribute_of(&one, 0u, 1u, 0u) == 1.0f && attribute_of(&one, 1u, 1u, 1u) == 5.0f);
                CHECK(attribute_of(&one, 2u, 1u, 2u) == 9.0f);
                CHECK(gpu_pgraph_vertex_bytes_held(pgraph) == 72u);
                CHECK(gpu_pgraph_get_stats(pgraph).vertex_draws_captured == 2u);
                CHECK(gpu_pgraph_get_stats(pgraph).vertex_bytes_captured == 72u);
            } else {
                /* the control: with no snapshot draw 1 reads what is there at the replay (the bug) */
                CHECK(attribute_of(&one, 0u, 1u, 0u) == 11.0f && attribute_of(&one, 2u, 1u, 2u) == 19.0f);
                CHECK(gpu_pgraph_vertex_bytes_held(pgraph) == 0u);
                CHECK(gpu_pgraph_get_stats(pgraph).vertex_draws_captured == 0u);
                CHECK(gpu_pgraph_draw_vertex_bytes(pgraph, 0u, 1u, NULL, NULL) == NULL);
            }
        }
        gpu_pgraph_assembled_free(&one);
        gpu_pgraph_assembled_free(&two);
        gpu_pgraph_destroy(pgraph);
    }
    stream_free(&first);
    stream_free(&second);
}

/* The moment is the decoder's BEGIN_END(0), not the BEGIN, not the arrays. */
static void test_snapshot_moment_is_the_end_of_the_bracket(void)
{
    printf("test_snapshot_moment_is_the_end_of_the_bracket\n");
    uint8_t memory[256] = {0};
    fake_guest guest = {SNAP_BASE, memory, sizeof memory};
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(pgraph, fake_guest_read, &guest, 1024u);
    stream_builder head = {0};
    stream_array(&head, 1u, SNAP_BASE, array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
    stream_pair(&head, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    stream_pair(&head, GPU_PGRAPH_DRAW_ARRAYS, (2u << 24) | 0u);
    put_floats(memory, 0u, 1.0f, 9u);
    CHECK(gpu_pgraph_decode(pgraph, head.pairs, head.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_in_bracket(pgraph) && gpu_pgraph_draw_count(pgraph) == 0u);
    CHECK(gpu_pgraph_get_stats(pgraph).vertex_bytes_captured == 0u); /* nothing read mid-bracket */
    put_floats(memory, 0u, 21.0f, 9u);
    stream_builder tail = {0};
    stream_pair(&tail, GPU_PGRAPH_BEGIN_END, 0u);
    CHECK(gpu_pgraph_decode(pgraph, tail.pairs, tail.count) == GPU_PGRAPH_OK);
    CHECK(!gpu_pgraph_in_bracket(pgraph) && gpu_pgraph_draw_count(pgraph) == 1u);
    uint32_t address = 0u;
    uint32_t length = 0u;
    const uint8_t *bytes = gpu_pgraph_draw_vertex_bytes(pgraph, 0u, 1u, &address, &length);
    CHECK(bytes != NULL && address == SNAP_BASE && length == 36u);
    if (bytes != NULL) {
        float first;
        memcpy(&first, bytes, sizeof first);
        CHECK(first == 21.0f); /* the bytes at the END, which is the stated moment */
    }
    stream_free(&head);
    stream_free(&tail);
    gpu_pgraph_destroy(pgraph);
}

/* Only the vertices the replay fetches are copied: lowest to highest, one element wide at the end. */
static void test_snapshot_range_is_exact(void)
{
    printf("test_snapshot_range_is_exact\n");
    uint8_t memory[256] = {0};
    fake_guest guest = {SNAP_BASE, memory, sizeof memory};
    put_floats(memory, 0u, 100.0f, 60u);
    counting_guest counting = {guest, 0u, 0u};
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(pgraph, counting_read, &counting, 4096u);
    stream_builder stream = {0};
    stream_array(&stream, 1u, SNAP_BASE, array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, 2u);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, 6u);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, 4u);
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, 0u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    uint32_t address = 0u;
    uint32_t length = 0u;
    const uint8_t *bytes = gpu_pgraph_draw_vertex_bytes(pgraph, 0u, 1u, &address, &length);
    CHECK(bytes != NULL && address == SNAP_BASE + 24u && length == 12u * (6u - 2u) + 12u);
    CHECK(bytes != NULL && memcmp(bytes, memory + 24u, 60u) == 0);
    CHECK(counting.reads == 1u && counting.bytes == 60u && gpu_pgraph_vertex_bytes_held(pgraph) == 60u);
    CHECK(gpu_pgraph_draw_vertex_bytes(pgraph, 0u, 2u, NULL, NULL) == NULL); /* slot 2 has no array */
    CHECK(gpu_pgraph_draw_vertex_bytes(pgraph, 1u, 1u, NULL, NULL) == NULL); /* no such draw */
    CHECK(gpu_pgraph_draw_vertex_bytes(pgraph, 0u, GPU_PGRAPH_ATTRIBUTES, NULL, NULL) == NULL);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* a dropped remainder (the 4th index of TRIANGLES) is not fetched, so it is not read: its index
     * points far outside the guest memory, and reading it would refuse the draw */
    pgraph = gpu_pgraph_create();
    counting.reads = 0u;
    counting.bytes = 0u;
    gpu_pgraph_set_vertex_capture(pgraph, counting_read, &counting, 4096u);
    stream_array(&stream, 1u, SNAP_BASE, array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_TRIANGLES);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, 1u);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, 2u);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, 3u);
    stream_pair(&stream, GPU_PGRAPH_ARRAY_ELEMENT32, 0x100000u);
    stream_pair(&stream, GPU_PGRAPH_BEGIN_END, 0u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u && gpu_pgraph_draw_at(pgraph, 0u)->index_count == 4u);
    CHECK(counting.reads == 1u && counting.bytes == 36u);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* Slots that overlap are read once and each still sees its own bytes (compared with the live read). */
static void test_snapshot_merges_overlapping_slots(void)
{
    printf("test_snapshot_merges_overlapping_slots\n");
    uint8_t memory[512] = {0};
    fake_guest guest = {SNAP_BASE, memory, sizeof memory};
    put_floats(memory, 0u, 1.0f, 120u);
    gpu_pgraph_backend backend = snapshot_backend(&guest);

    const struct {
        const char *name;
        uint32_t offsets[3]; /* of slots 1, 2, 3 from the base */
        uint32_t sizes[3];   /* components, 0 = slot unused */
        uint32_t stride;
        uint32_t reads;
        size_t held;
    } cases[] = {
        /* interleaved: position F3 at +0 and an F4 at +12, stride 28, 3 vertices: [0,68) and
         * [12,84) are one span of 84 */
        {"interleaved", {0u, 12u, 0u}, {3u, 4u, 0u}, 28u, 1u, 84u},
        /* two separate buffers: two reads */
        {"disjoint", {0u, 0x100u, 0u}, {3u, 3u, 0u}, 12u, 2u, 72u},
        /* a chain that only merges transitively: [0,36) [30,66) [60,96) */
        {"chained", {0u, 30u, 60u}, {3u, 3u, 3u}, 12u, 1u, 96u},
        /* the same range twice */
        {"identical", {0u, 0u, 0u}, {3u, 3u, 0u}, 12u, 1u, 36u},
    };
    for (size_t index = 0u; index < sizeof cases / sizeof cases[0]; index++) {
        counting_guest counting = {guest, 0u, 0u};
        gpu_pgraph *captured = gpu_pgraph_create();
        gpu_pgraph *live = gpu_pgraph_create();
        gpu_pgraph_set_vertex_capture(captured, counting_read, &counting, 4096u);
        stream_builder stream = {0};
        for (uint32_t slot = 0u; slot < 3u; slot++) {
            if (cases[index].sizes[slot] != 0u) {
                stream_array(&stream, slot + 1u, SNAP_BASE + cases[index].offsets[slot],
                             array_format(cases[index].stride, cases[index].sizes[slot],
                                          GPU_PGRAPH_TYPE_F));
            }
        }
        stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
        CHECK(gpu_pgraph_decode(captured, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_decode(live, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_draw_count(captured) == 1u);
        CHECK(counting.reads == cases[index].reads);
        CHECK(gpu_pgraph_vertex_bytes_held(captured) == cases[index].held);
        gpu_pgraph_assembled left;
        gpu_pgraph_assembled right;
        gpu_pgraph_report report;
        CHECK(gpu_pgraph_assemble_draw(captured, 0u, &backend, &left, &report) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_assemble_draw(live, 0u, &backend, &right, &report) == GPU_PGRAPH_OK);
        CHECK(left.vertex_count == 3u && right.vertex_count == 3u);
        CHECK(left.attributes != NULL && right.attributes != NULL);
        if (left.attributes != NULL && right.attributes != NULL) {
            /* every slot of every vertex equals the live read, and is not all defaults */
            CHECK(memcmp(left.attributes, right.attributes,
                         (size_t)3u * GPU_VSH_ATTRIBUTE_FLOATS * sizeof(float)) == 0);
            CHECK(attribute_of(&left, 2u, 1u, 0u) != 0.0f);
        }
        gpu_pgraph_assembled_free(&left);
        gpu_pgraph_assembled_free(&right);
        stream_free(&stream);
        gpu_pgraph_destroy(captured);
        gpu_pgraph_destroy(live);
    }
}

/* One draw spanning all four vertices of an interleaved buffer: the merged span is read once. */
static void test_snapshot_interleaved_span(void)
{
    printf("test_snapshot_interleaved_span\n");
    uint8_t memory[512] = {0};
    fake_guest guest = {SNAP_BASE, memory, sizeof memory};
    put_floats(memory, 0u, 1.0f, 120u);
    counting_guest counting = {guest, 0u, 0u};
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(pgraph, counting_read, &counting, 4096u);
    stream_builder stream = {0};
    stream_array(&stream, 1u, SNAP_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(&stream, 2u, SNAP_BASE + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_QUADS, 0u, 4u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(counting.reads == 1u && counting.bytes == 112u && gpu_pgraph_vertex_bytes_held(pgraph) == 112u);
    uint32_t address = 0u;
    uint32_t length = 0u;
    const uint8_t *position = gpu_pgraph_draw_vertex_bytes(pgraph, 0u, 1u, &address, &length);
    CHECK(position != NULL && address == SNAP_BASE && length == 28u * 3u + 12u);
    const uint8_t *colour = gpu_pgraph_draw_vertex_bytes(pgraph, 0u, 2u, &address, &length);
    CHECK(colour != NULL && address == SNAP_BASE + 12u && length == 28u * 3u + 16u);
    CHECK(position != NULL && colour != NULL && colour == position + 12u);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

/* What the replay converts is copied, what it refuses is never read. */
static void test_snapshot_types_and_primitives(void)
{
    printf("test_snapshot_types_and_primitives\n");
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_F, 1u) == 4u);
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_F, 4u) == 16u);
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_F, 0u) == 0u);
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_F, 5u) == 0u);
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_UB_D3D, 4u) == 4u);
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_UB_D3D, 3u) == 0u);
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_S32K, 2u) == 4u);
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_S32K, 3u) == 0u);
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_CMP, 1u) == 4u); /* T1204 */
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_CMP, 2u) == 0u);
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_S1, 2u) == 0u);
    CHECK(gpu_pgraph_element_bytes(GPU_PGRAPH_TYPE_UB_OGL, 4u) == 0u);

    uint8_t memory[512] = {0};
    fake_guest guest = {SNAP_BASE, memory, sizeof memory};
    put_floats(memory, 0u, 1.0f, 120u);
    counting_guest counting = {guest, 0u, 0u};
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(pgraph, counting_read, &counting, 4096u);
    stream_builder stream = {0};
    stream_array(&stream, 0u, SNAP_BASE, array_format(16u, 2u, GPU_PGRAPH_TYPE_S32K));
    stream_array(&stream, 3u, SNAP_BASE + 4u, array_format(16u, 4u, GPU_PGRAPH_TYPE_UB_D3D));
    stream_array(&stream, 5u, SNAP_BASE + 0x100u, array_format(4u, 1u, GPU_PGRAPH_TYPE_CMP));
    stream_array(&stream, 6u, SNAP_BASE + 0x180u, array_format(4u, 2u, GPU_PGRAPH_TYPE_S1));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_vertex_bytes(pgraph, 0u, 0u, NULL, NULL) != NULL);
    CHECK(gpu_pgraph_draw_vertex_bytes(pgraph, 0u, 3u, NULL, NULL) != NULL);
    CHECK(gpu_pgraph_draw_vertex_bytes(pgraph, 0u, 5u, NULL, NULL) != NULL); /* CMP size 1: captured (T1204) */
    CHECK(gpu_pgraph_draw_vertex_bytes(pgraph, 0u, 6u, NULL, NULL) == NULL);
    CHECK(counting.reads == 2u); /* S32K at +0 (4 bytes) and UB_D3D at +4 overlap: one span, CMP at +0x100 its own */
    CHECK(counting.bytes == 16u * 2u + 8u + 4u * 2u + 4u);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    /* a primitive the replay refuses (QUAD_STRIP, POLYGON, LINE_LOOP) is not read at all */
    for (uint32_t primitive = 0u; primitive < 3u; primitive++) {
        static const uint32_t refused[3] = {GPU_PGRAPH_OP_QUAD_STRIP, GPU_PGRAPH_OP_POLYGON,
                                            GPU_PGRAPH_OP_LINE_LOOP};
        pgraph = gpu_pgraph_create();
        counting.reads = 0u;
        gpu_pgraph_set_vertex_capture(pgraph, counting_read, &counting, 4096u);
        stream_array(&stream, 1u, SNAP_BASE, array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
        stream_draw_arrays(&stream, refused[primitive], 0u, 4u);
        CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_draw_count(pgraph) == 1u && counting.reads == 0u);
        CHECK(!gpu_pgraph_draw_at(pgraph, 0u)->vertices_captured);
        stream_free(&stream);
        gpu_pgraph_destroy(pgraph);
    }
    /* the supported ones are */
    static const uint32_t supported[] = {GPU_PGRAPH_OP_POINTS, GPU_PGRAPH_OP_LINES,
                                         GPU_PGRAPH_OP_LINE_STRIP, GPU_PGRAPH_OP_TRIANGLES,
                                         GPU_PGRAPH_OP_TRIANGLE_STRIP, GPU_PGRAPH_OP_TRIANGLE_FAN,
                                         GPU_PGRAPH_OP_QUADS};
    for (size_t i = 0u; i < sizeof supported / sizeof supported[0]; i++) {
        pgraph = gpu_pgraph_create();
        counting.reads = 0u;
        gpu_pgraph_set_vertex_capture(pgraph, counting_read, &counting, 4096u);
        stream_array(&stream, 1u, SNAP_BASE, array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
        stream_draw_arrays(&stream, supported[i], 0u, 4u);
        CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_draw_count(pgraph) == 1u && counting.reads == 1u);
        CHECK(gpu_pgraph_draw_at(pgraph, 0u)->vertices_captured);
        stream_free(&stream);
        gpu_pgraph_destroy(pgraph);
    }
}

/* The budget bounds the frame's pool: a draw past it is REFUSED loudly, never truncated. */
static void test_snapshot_budget(void)
{
    printf("test_snapshot_budget\n");
    uint8_t memory[512] = {0};
    fake_guest guest = {SNAP_BASE, memory, sizeof memory};
    put_floats(memory, 0u, 1.0f, 120u);
    stream_builder array = {0};
    stream_array(&array, 1u, SNAP_BASE, array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
    stream_builder draw = {0};
    stream_draw_arrays(&draw, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u); /* 36 bytes */

    /* exactly at the budget is fine, one byte under is refused */
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(pgraph, fake_guest_read, &guest, 36u);
    CHECK(gpu_pgraph_decode(pgraph, array.pairs, array.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_decode(pgraph, draw.pairs, draw.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 1u && gpu_pgraph_vertex_bytes_held(pgraph) == 36u);
    gpu_pgraph_destroy(pgraph);
    pgraph = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(pgraph, fake_guest_read, &guest, 35u);
    CHECK(gpu_pgraph_decode(pgraph, array.pairs, array.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_decode(pgraph, draw.pairs, draw.count) == GPU_PGRAPH_ERR_FULL);
    CHECK(strstr(gpu_pgraph_error(pgraph), "budget of 35 bytes") != NULL);
    CHECK(strstr(gpu_pgraph_error(pgraph), "needs 36 more bytes") != NULL);
    CHECK(gpu_pgraph_draw_count(pgraph) == 0u && gpu_pgraph_vertex_bytes_held(pgraph) == 0u);
    CHECK(gpu_pgraph_get_stats(pgraph).vertex_budget_refusals == 1u);
    gpu_pgraph_destroy(pgraph);

    /* the pool accumulates over a frame: two draws fit 100 bytes, the third does not */
    pgraph = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(pgraph, fake_guest_read, &guest, 100u);
    CHECK(gpu_pgraph_decode(pgraph, array.pairs, array.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_decode(pgraph, draw.pairs, draw.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_decode(pgraph, draw.pairs, draw.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(pgraph) == 2u && gpu_pgraph_vertex_bytes_held(pgraph) == 72u);
    CHECK(gpu_pgraph_decode(pgraph, draw.pairs, draw.count) == GPU_PGRAPH_ERR_FULL);
    CHECK(strstr(gpu_pgraph_error(pgraph), "72 are held") != NULL);
    CHECK(strstr(gpu_pgraph_error(pgraph), "draw 2") != NULL);
    CHECK(gpu_pgraph_draw_count(pgraph) == 2u); /* the refused draw is not in the list */
    gpu_pgraph_destroy(pgraph);

    /* the budget is per frame: begin_frame empties the pool, so the same draws fit again */
    pgraph = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(pgraph, fake_guest_read, &guest, 100u);
    CHECK(gpu_pgraph_decode(pgraph, array.pairs, array.count) == GPU_PGRAPH_OK);
    for (int frame = 0; frame < 3; frame++) {
        CHECK(gpu_pgraph_decode(pgraph, draw.pairs, draw.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_decode(pgraph, draw.pairs, draw.count) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_draw_count(pgraph) == 2u && gpu_pgraph_vertex_bytes_held(pgraph) == 72u);
        CHECK(gpu_pgraph_begin_frame(pgraph) == GPU_PGRAPH_OK);
        CHECK(gpu_pgraph_vertex_bytes_held(pgraph) == 0u);
    }
    CHECK(gpu_pgraph_get_stats(pgraph).vertex_bytes_captured == 6u * 36u); /* cumulative */
    /* reset empties the pool and keeps the setting, like strictness */
    CHECK(gpu_pgraph_decode(pgraph, draw.pairs, draw.count) == GPU_PGRAPH_OK);
    gpu_pgraph_reset(pgraph);
    CHECK(gpu_pgraph_vertex_capture_enabled(pgraph) && gpu_pgraph_vertex_bytes_held(pgraph) == 0u);
    /* off switches: budget 0, or no reader */
    gpu_pgraph_set_vertex_capture(pgraph, fake_guest_read, &guest, 0u);
    CHECK(!gpu_pgraph_vertex_capture_enabled(pgraph));
    gpu_pgraph_set_vertex_capture(pgraph, NULL, &guest, 100u);
    CHECK(!gpu_pgraph_vertex_capture_enabled(pgraph));
    gpu_pgraph_destroy(pgraph);

    /* a huge range is refused by the budget BEFORE any byte is read or allocated */
    counting_guest counting = {guest, 0u, 0u};
    pgraph = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(pgraph, counting_read, &counting, 1024u * 1024u);
    stream_builder huge = {0};
    stream_array(&huge, 1u, SNAP_BASE, array_format(0xFFFFFFu, 3u, GPU_PGRAPH_TYPE_F));
    stream_pair(&huge, GPU_PGRAPH_BEGIN_END, GPU_PGRAPH_OP_POINTS);
    stream_pair(&huge, GPU_PGRAPH_ARRAY_ELEMENT32, 0u);
    stream_pair(&huge, GPU_PGRAPH_ARRAY_ELEMENT32, 20u);
    stream_pair(&huge, GPU_PGRAPH_BEGIN_END, 0u);
    const gpu_pgraph_result huge_result = gpu_pgraph_decode(pgraph, huge.pairs, huge.count);
    CHECK(huge_result == GPU_PGRAPH_ERR_FULL);
    CHECK(counting.reads == 0u && strstr(gpu_pgraph_error(pgraph), "budget") != NULL);
    gpu_pgraph_destroy(pgraph);
    stream_free(&huge);

    stream_free(&array);
    stream_free(&draw);
}

/* A read that fails, and an array past the 32-bit address space, are refused at the draw. */
static void test_snapshot_read_failures(void)
{
    printf("test_snapshot_read_failures\n");
    uint8_t memory[64] = {0};
    fake_guest guest = {SNAP_BASE, memory, sizeof memory};
    stream_builder stream = {0};
    gpu_pgraph *pgraph = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(pgraph, fake_guest_read, &guest, 4096u);
    stream_array(&stream, 1u, SNAP_BASE + 40u, array_format(12u, 3u, GPU_PGRAPH_TYPE_F)); /* 36 > 24 left */
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_ERR_MALFORMED);
    CHECK(strstr(gpu_pgraph_error(pgraph), "could not read 36 guest bytes at 0x00004028") != NULL);
    CHECK(gpu_pgraph_draw_count(pgraph) == 0u);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);

    pgraph = gpu_pgraph_create();
    gpu_pgraph_set_vertex_capture(pgraph, fake_guest_read, &guest, 4096u);
    stream_array(&stream, 1u, 0xFFFFFFF0u, array_format(12u, 3u, GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(pgraph, stream.pairs, stream.count) == GPU_PGRAPH_ERR_MALFORMED);
    CHECK(strstr(gpu_pgraph_error(pgraph), "32-bit guest address space") != NULL);
    stream_free(&stream);
    gpu_pgraph_destroy(pgraph);
}

int main(void)
{
    test_sha256();
    test_format_decode();
    test_state_and_draw();
    test_indices();
    test_snapshots_are_copies();
    test_decode_resumes_across_kicks();
    test_begin_frame();
    test_refusals();
    test_unhandled_methods();
    test_other_subchannels();
    test_cxt_write_en();
    test_triangulate();
    test_resolve_program();
    test_resolve_program_makes_a_missing_module();
    test_assemble();
    test_stride_zero_and_stride();
    test_stream_written_viewport_constants_win();
    test_window_clip_takes_the_target_pair_when_the_registers_are_half_written();
    test_snapshot_survives_an_overwrite();
    test_snapshot_moment_is_the_end_of_the_bracket();
    test_snapshot_range_is_exact();
    test_snapshot_merges_overlapping_slots();
    test_snapshot_interleaved_span();
    test_snapshot_types_and_primitives();
    test_snapshot_budget();
    test_snapshot_read_failures();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 && checks > 300 ? 0 : 1;
}
