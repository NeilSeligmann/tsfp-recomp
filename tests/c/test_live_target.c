/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T793 live_target: registry, CopyRects blit planner and present schedule. Device free. The blit is cross-checked against
 * the replay's own blit (gpu_pgraph_replay_copy) wherever that accepts the shape, and the shapes it refuses (clamp, ordered
 * overlap, formats 6 and 7) are checked against the xemu-level rules of HQ58 (T736) written out by hand.
 */
#include "gpu_pgraph_replay.h"
#include "live_target.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition)                                                          \
    do {                                                                          \
        checks++;                                                                 \
        if (!(condition)) {                                                       \
            failures++;                                                           \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);                 \
        }                                                                         \
    } while (0)

#define FORMAT_A8R8G8B8 0x00011229u
#define FORMAT_R5G6B5 0x00011129u
#define FORMAT_X8R8G8B8 0x00011E29u
#define A_DATA 0x0100000u
#define B_DATA 0x0200000u
/* 16x8 linear, pitch 64: width - 1 | (height - 1) << 12 | (pitch / 64 - 1) << 24 */
#define SIZE_16X8 (15u | (7u << 12))
/* 8x8 linear, pitch 64 (a narrower row than the pitch) */
#define SIZE_8X8_P64 (7u | (7u << 12))
/* 16x8 with pitch 128 */
#define SIZE_16X8_P128 (15u | (7u << 12) | (1u << 24))

static void fill(uint8_t *pixels, size_t bytes, uint8_t seed)
{
    for (size_t i = 0; i < bytes; i++) {
        pixels[i] = (uint8_t)(seed + i * 7u + (i >> 5));
    }
}

static live_target_blit make_blit(uint32_t in_x, uint32_t in_y, uint32_t out_x, uint32_t out_y, uint32_t w, uint32_t h)
{
    live_target_blit blit = {A_DATA, B_DATA, LIVE_BLIT_FORMAT_A8R8G8B8, 64, 64, in_x, in_y, out_x, out_y, w, h,
                             LIVE_BLIT_OPERATION_SRCCOPY};
    return blit;
}

static void test_decode(void)
{
    live_target_desc desc;
    CHECK(live_target_desc_from_words(A_DATA, 0x00011229u, 0x271DF27Fu, &desc) == LIVE_TARGET_OK);
    CHECK(desc.width == 640u && desc.height == 480u && desc.pitch == 2560u && desc.bytes_per_pixel == 4u);
    CHECK(desc.format == LIVE_TARGET_FORMAT_A8R8G8B8 && !desc.swizzled);
    /* swizzled A8R8G8B8 (0x06) 256x128: width exponent 8, height exponent 7 */
    CHECK(live_target_desc_from_words(A_DATA, 0x00000600u | (8u << 20) | (7u << 24), 0u, &desc) == LIVE_TARGET_OK);
    CHECK(desc.width == 256u && desc.height == 128u && desc.pitch == 1024u && desc.swizzled);
    CHECK(live_target_desc_from_words(A_DATA, 0x00000600u, 0u, &desc) == LIVE_TARGET_REFUSE_SIZE_UNKNOWN);
    CHECK(live_target_desc_from_words(A_DATA, 0x00011229u, 0u, &desc) == LIVE_TARGET_REFUSE_SIZE_UNKNOWN);
    CHECK(live_target_desc_from_words(A_DATA, 0x00012A29u, SIZE_16X8, &desc) == LIVE_TARGET_REFUSE_FORMAT_UNKNOWN);
    CHECK(live_target_desc_from_words(A_DATA, FORMAT_R5G6B5, SIZE_16X8, &desc) == LIVE_TARGET_OK);
    CHECK(desc.bytes_per_pixel == 2u && desc.format == LIVE_TARGET_FORMAT_R5G6B5);
    CHECK(live_target_desc_from_words(A_DATA, FORMAT_X8R8G8B8, SIZE_16X8, &desc) == LIVE_TARGET_OK);
    CHECK(desc.format == LIVE_TARGET_FORMAT_X8R8G8B8);
}

static void test_registry(void)
{
    live_target_registry *registry = live_target_registry_create();
    CHECK(registry != NULL);
    CHECK(live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, SIZE_16X8, true) == LIVE_TARGET_OK);
    CHECK(live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, SIZE_16X8, false) == LIVE_TARGET_OK);
    CHECK(live_target_registry_count(registry) == 1u);
    CHECK(live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, SIZE_16X8_P128, false) == LIVE_TARGET_REFUSE_REDECLARED);
    CHECK(live_target_find(registry, A_DATA) != NULL && live_target_find(registry, B_DATA) == NULL);
    CHECK(live_target_pixels(registry, A_DATA) != NULL);
    CHECK(live_target_generation(registry, A_DATA) == 0u);
    live_target_note_written(registry, A_DATA);
    CHECK(live_target_generation(registry, A_DATA) == 1u);
    CHECK(live_target_generation(registry, B_DATA) == 0u);
    live_target_stats stats = live_target_registry_stats(registry);
    CHECK(stats.registered == 1u && stats.refusals[LIVE_TARGET_REFUSE_REDECLARED] == 1u);
    for (uint32_t i = 1u; i < LIVE_TARGET_MAX_TARGETS; i++) {
        CHECK(live_target_register(registry, 0x1000u * (i + 1u), FORMAT_A8R8G8B8, SIZE_16X8, false) == LIVE_TARGET_OK);
    }
    CHECK(live_target_registry_count(registry) == LIVE_TARGET_MAX_TARGETS);
    CHECK(live_target_register(registry, 0xAB0000u, FORMAT_A8R8G8B8, SIZE_16X8, false) == LIVE_TARGET_REFUSE_REGISTRY_FULL);
    live_target_registry_destroy(registry);
}

/* The oracle: gpu_pgraph_replay_copy on tightly packed images. */
static bool oracle_blit(const live_target_blit *blit, const uint8_t *source, uint8_t *destination, uint32_t width,
                        uint32_t height, bool same)
{
    gpu_pgraph_copy copy = {0};
    copy.color_format = blit->color_format;
    copy.source_pitch = blit->source_pitch;
    copy.destination_pitch = blit->destination_pitch;
    copy.in_x = blit->in_x;
    copy.in_y = blit->in_y;
    copy.out_x = blit->out_x;
    copy.out_y = blit->out_y;
    copy.width = blit->width;
    copy.height = blit->height;
    copy.operation = blit->operation;
    gpu_pgraph_backend backend;
    memset(&backend, 0, sizeof backend);
    backend.output_groups = GPU_PGRAPH_OUTPUT_BLIT;
    backend.allowed_inferences = GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL;
    gpu_image from = {(uint8_t *)source, width, height, width * 4u};
    gpu_image to = {destination, width, height, width * 4u};
    uint32_t used = 0u;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    return gpu_pgraph_replay_copy(&copy, &backend, &from, same ? &to : &to, &used, &report) == GPU_PGRAPH_OK;
}

static void test_oracle_agreement(void)
{
    /* 16x8 A8R8G8B8 packed images (pitch 64 = 16 * 4). Cases the oracle accepts: whole, offset, disjoint same-surface. */
    const struct {
        uint32_t in_x, in_y, out_x, out_y, w, h;
    } cases[] = {{0, 0, 0, 0, 16, 8}, {2, 1, 5, 3, 7, 4}, {0, 0, 8, 4, 8, 4}, {15, 7, 0, 0, 1, 1}, {3, 0, 3, 0, 5, 2}};
    size_t accepted = 0u;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        live_target_registry *registry = live_target_registry_create();
        live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, SIZE_16X8, true);
        live_target_register(registry, B_DATA, FORMAT_A8R8G8B8, SIZE_16X8, true);
        fill(live_target_pixels(registry, A_DATA), 64u * 8u, 0x11u);
        fill(live_target_pixels(registry, B_DATA), 64u * 8u, 0xA3u);
        uint8_t expected[64 * 8];
        memcpy(expected, live_target_pixels(registry, B_DATA), sizeof expected);
        const live_target_blit blit = make_blit(cases[i].in_x, cases[i].in_y, cases[i].out_x, cases[i].out_y, cases[i].w,
                                               cases[i].h);
        const bool oracle_ok = oracle_blit(&blit, live_target_pixels(registry, A_DATA), expected, 16u, 8u, false);
        live_target_blit_plan plan;
        bool executed = false;
        CHECK(live_target_apply_blit(registry, &blit, 0u, &plan, &executed) == LIVE_TARGET_OK);
        CHECK(executed);
        CHECK(oracle_ok);
        if (oracle_ok) {
            accepted++;
            CHECK(memcmp(expected, live_target_pixels(registry, B_DATA), sizeof expected) == 0);
        }
        CHECK(!plan.overlap && !plan.clamped && plan.copy_image_ok);
        CHECK(live_target_generation(registry, B_DATA) == 1u && live_target_generation(registry, A_DATA) == 0u);
        live_target_registry_destroy(registry);
    }
    CHECK(accepted == sizeof cases / sizeof cases[0]);
    /* The blit moved something: the destination differs from its original fill. */
    live_target_registry *registry = live_target_registry_create();
    live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, SIZE_16X8, true);
    live_target_register(registry, B_DATA, FORMAT_A8R8G8B8, SIZE_16X8, true);
    fill(live_target_pixels(registry, A_DATA), 64u * 8u, 0x11u);
    fill(live_target_pixels(registry, B_DATA), 64u * 8u, 0xA3u);
    const live_target_blit blit = make_blit(0, 0, 0, 0, 16, 8);
    CHECK(live_target_apply_blit(registry, &blit, 0u, NULL, NULL) == LIVE_TARGET_OK);
    CHECK(memcmp(live_target_pixels(registry, A_DATA), live_target_pixels(registry, B_DATA), 64u * 8u) == 0);
    live_target_registry_destroy(registry);
}

/* T832: the replay copy under GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES against the planner, byte for byte, on tightly packed images. */
typedef struct {
    uint32_t source_size, destination_size, source_width, destination_width, height;
    uint32_t format, in_x, in_y, out_x, out_y, width, rows;
    bool same;
    bool replay_default_accepts; /* the default replay (no gate) takes the shape as well */
} parity_case;

static bool replay_copy_run(const parity_case *shape, uint32_t allowed, const uint8_t *source, uint8_t *destination)
{
    gpu_pgraph_copy copy = {0};
    copy.color_format = shape->format;
    copy.source_pitch = shape->source_width * 4u;
    copy.destination_pitch = shape->destination_width * 4u;
    copy.in_x = shape->in_x;
    copy.in_y = shape->in_y;
    copy.out_x = shape->out_x;
    copy.out_y = shape->out_y;
    copy.width = shape->width;
    copy.height = shape->rows;
    copy.operation = LIVE_BLIT_OPERATION_SRCCOPY;
    gpu_pgraph_backend backend;
    memset(&backend, 0, sizeof backend);
    backend.output_groups = GPU_PGRAPH_OUTPUT_BLIT;
    backend.allowed_inferences = GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL | allowed;
    gpu_image from = {shape->same ? destination : (uint8_t *)source, shape->source_width, shape->height, shape->source_width * 4u};
    gpu_image to = {destination, shape->destination_width, shape->height, shape->destination_width * 4u};
    uint32_t used = 0u;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    return gpu_pgraph_replay_copy(&copy, &backend, &from, shape->same ? &from : &to, &used, &report) == GPU_PGRAPH_OK;
}

static void test_replay_planner_parity(void)
{
    const uint32_t s16 = SIZE_16X8;
    const uint32_t s32 = 31u | (7u << 12) | (1u << 24); /* 32x8, pitch 128 */
    const parity_case shapes[] = {
        /* both accept (default replay too) */
        {s16, s16, 16, 16, 8, 0x0A, 0, 0, 0, 0, 16, 8, false, true},
        {s16, s16, 16, 16, 8, 0x0A, 2, 1, 5, 3, 7, 4, false, true},
        {s16, s16, 16, 16, 8, 0x0A, 0, 0, 8, 4, 8, 4, true, true},
        /* only the planner accepted before: ordered overlap right (buffered row), down (smear), up */
        {s16, s16, 16, 16, 8, 0x0A, 0, 0, 3, 0, 8, 2, true, false},
        {s16, s16, 16, 16, 8, 0x0A, 0, 0, 0, 3, 16, 4, true, false},
        {s16, s16, 16, 16, 8, 0x0A, 0, 3, 0, 0, 16, 4, true, false},
        /* the clamp to the narrower pitch, either side narrower, offsets 0 */
        {s32, s16, 32, 16, 8, 0x0A, 0, 0, 0, 0, 32, 3, false, false},
        {s16, s32, 16, 32, 8, 0x0A, 0, 2, 0, 1, 32, 3, false, false},
        /* formats 7 and 6 force the alpha byte, also with a clamp and an overlap */
        {s16, s16, 16, 16, 8, 0x07, 0, 0, 0, 0, 16, 8, false, false},
        {s16, s16, 16, 16, 8, 0x06, 1, 1, 2, 2, 9, 5, false, false},
        {s32, s16, 32, 16, 8, 0x07, 0, 0, 0, 0, 32, 8, false, false},
        {s16, s16, 16, 16, 8, 0x06, 0, 0, 3, 0, 8, 2, true, false},
    };
    /* the rules are opt-in: INFER_OUTPUT_ALL and INFER_ALL do not carry them, so the default refusals stand */
    CHECK((GPU_PGRAPH_INFER_OUTPUT_ALL & GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES) == 0u);
    CHECK((GPU_PGRAPH_INFER_ALL & GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES) == 0u);
    size_t compared = 0u;
    size_t planner_only = 0u;
    for (size_t i = 0u; i < sizeof shapes / sizeof shapes[0]; i++) {
        const parity_case *shape = &shapes[i];
        live_target_registry *registry = live_target_registry_create();
        CHECK(live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, shape->source_size, true) == LIVE_TARGET_OK);
        CHECK(live_target_register(registry, B_DATA, FORMAT_A8R8G8B8, shape->destination_size, true) == LIVE_TARGET_OK);
        const size_t source_bytes = (size_t)shape->source_width * 4u * shape->height;
        const size_t destination_bytes = (size_t)shape->destination_width * 4u * shape->height;
        fill(live_target_pixels(registry, A_DATA), source_bytes, 0x11u);
        fill(live_target_pixels(registry, B_DATA), destination_bytes, 0xA3u);
        uint8_t *oracle_source = malloc(source_bytes);
        uint8_t *oracle_destination = malloc(destination_bytes);
        uint8_t *default_destination = malloc(destination_bytes);
        memcpy(oracle_source, live_target_pixels(registry, A_DATA), source_bytes);
        memcpy(oracle_destination, live_target_pixels(registry, B_DATA), destination_bytes);
        memcpy(default_destination, oracle_destination, destination_bytes);
        live_target_blit blit = {shape->same ? B_DATA : A_DATA, B_DATA, shape->format, shape->source_width * 4u,
                                 shape->destination_width * 4u, shape->in_x, shape->in_y, shape->out_x, shape->out_y,
                                 shape->width, shape->rows, LIVE_BLIT_OPERATION_SRCCOPY};
        /* the same-surface oracle copies inside one image: the replay gets the destination image twice */
        const uint8_t *replay_source = shape->same ? oracle_destination : oracle_source;
        CHECK(replay_copy_run(shape, GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES, replay_source, oracle_destination));
        CHECK(live_target_apply_blit(registry, &blit, 0u, NULL, NULL) == LIVE_TARGET_OK);
        CHECK(memcmp(oracle_destination, live_target_pixels(registry, B_DATA), destination_bytes) == 0);
        CHECK(memcmp(oracle_destination, default_destination, destination_bytes) != 0); /* something moved */
        compared++;
        const bool default_accepts = replay_copy_run(shape, 0u, replay_source, default_destination);
        CHECK(default_accepts == shape->replay_default_accepts);
        if (!shape->replay_default_accepts) {
            planner_only++;
        }
        free(oracle_source);
        free(oracle_destination);
        free(default_destination);
        live_target_registry_destroy(registry);
    }
    CHECK(compared == sizeof shapes / sizeof shapes[0] && planner_only == 9u);
    /* an offset with a clamp, an out of bounds rectangle and the byte formats stay refused by both */
    const parity_case refused[] = {
        {s32, s16, 32, 16, 8, 0x0A, 1, 0, 0, 0, 32, 3, false, false},
        {s16, s16, 16, 16, 8, 0x0A, 4, 0, 0, 0, 16, 3, false, false},
        {s16, s16, 16, 16, 8, 0x0A, 0, 6, 0, 0, 8, 3, false, false},
        {s16, s16, 16, 16, 8, 0x0A, 0, 0, 4, 0, 16, 3, false, false},
        {s16, s16, 16, 16, 8, 0x0A, 0, 0, 0, 6, 8, 3, false, false},
        {s16, s32, 16, 32, 8, 0x0A, 0, 0, 1, 0, 32, 3, false, false},
        {s16, s16, 16, 16, 8, 0x04, 0, 0, 0, 0, 8, 3, false, false},
        {s16, s16, 16, 16, 8, 0x01, 0, 0, 0, 0, 8, 3, false, false},
    };
    for (size_t i = 0u; i < sizeof refused / sizeof refused[0]; i++) {
        const parity_case *shape = &refused[i];
        live_target_registry *registry = live_target_registry_create();
        live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, shape->source_size, true);
        live_target_register(registry, B_DATA, FORMAT_A8R8G8B8, shape->destination_size, true);
        uint8_t source[128 * 8];
        uint8_t destination[128 * 8];
        memset(source, 0x44, sizeof source);
        memset(destination, 0x99, sizeof destination);
        const live_target_blit blit = {A_DATA, B_DATA, shape->format, shape->source_width * 4u, shape->destination_width * 4u,
                                       shape->in_x, shape->in_y, shape->out_x, shape->out_y, shape->width, shape->rows,
                                       LIVE_BLIT_OPERATION_SRCCOPY};
        live_target_blit_plan plan;
        CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) != LIVE_TARGET_OK);
        CHECK(!replay_copy_run(shape, GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES, source, destination));
        live_target_registry_destroy(registry);
    }

    /* flip_y: both images hold their rows reversed and the rectangle rows follow, so the result is the planner's, flipped back */
    const size_t flip_cases[] = {1u, 4u, 6u, 8u}; /* offset rectangle, smearing overlap, clamp, alpha format */
    size_t flipped_compared = 0u;
    for (size_t k = 0u; k < sizeof flip_cases / sizeof flip_cases[0]; k++) {
        const parity_case *shape = &shapes[flip_cases[k]];
        const size_t source_bytes = (size_t)shape->source_width * 4u * shape->height;
        const size_t destination_bytes = (size_t)shape->destination_width * 4u * shape->height;
        uint8_t *plain_source = malloc(source_bytes);
        uint8_t *plain_destination = malloc(destination_bytes);
        uint8_t *flipped_source = malloc(source_bytes);
        uint8_t *flipped_destination = malloc(destination_bytes);
        fill(plain_source, source_bytes, 0x11u);
        fill(plain_destination, destination_bytes, 0xA3u);
        for (uint32_t row = 0u; row < shape->height; row++) {
            memcpy(flipped_source + (size_t)row * shape->source_width * 4u,
                   plain_source + (size_t)(shape->height - 1u - row) * shape->source_width * 4u, (size_t)shape->source_width * 4u);
            memcpy(flipped_destination + (size_t)row * shape->destination_width * 4u,
                   plain_destination + (size_t)(shape->height - 1u - row) * shape->destination_width * 4u,
                   (size_t)shape->destination_width * 4u);
        }
        gpu_pgraph_copy copy = {0};
        copy.color_format = shape->format;
        copy.source_pitch = shape->source_width * 4u;
        copy.destination_pitch = shape->destination_width * 4u;
        copy.in_x = shape->in_x;
        copy.in_y = shape->in_y;
        copy.out_x = shape->out_x;
        copy.out_y = shape->out_y;
        copy.width = shape->width;
        copy.height = shape->rows;
        copy.operation = LIVE_BLIT_OPERATION_SRCCOPY;
        gpu_pgraph_backend backend;
        memset(&backend, 0, sizeof backend);
        backend.output_groups = GPU_PGRAPH_OUTPUT_BLIT;
        backend.flip_y = true;
        backend.allowed_inferences = GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL | GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES;
        gpu_image from = {shape->same ? flipped_destination : flipped_source, shape->source_width, shape->height,
                          shape->source_width * 4u};
        gpu_image to = {flipped_destination, shape->destination_width, shape->height, shape->destination_width * 4u};
        uint32_t used = 0u;
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        CHECK(gpu_pgraph_replay_copy(&copy, &backend, &from, shape->same ? &from : &to, &used, &report) == GPU_PGRAPH_OK);
        CHECK(used == (GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL | GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES));
        live_target_registry *registry = live_target_registry_create();
        live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, shape->source_size, true);
        live_target_register(registry, B_DATA, FORMAT_A8R8G8B8, shape->destination_size, true);
        memcpy(live_target_pixels(registry, A_DATA), plain_source, source_bytes);
        memcpy(live_target_pixels(registry, B_DATA), plain_destination, destination_bytes);
        const live_target_blit blit = {shape->same ? B_DATA : A_DATA, B_DATA, shape->format, shape->source_width * 4u,
                                       shape->destination_width * 4u, shape->in_x, shape->in_y, shape->out_x, shape->out_y,
                                       shape->width, shape->rows, LIVE_BLIT_OPERATION_SRCCOPY};
        CHECK(live_target_apply_blit(registry, &blit, 0u, NULL, NULL) == LIVE_TARGET_OK);
        const uint8_t *expected = live_target_pixels(registry, B_DATA);
        CHECK(memcmp(plain_destination, expected, destination_bytes) != 0); /* the blit moved something */
        for (uint32_t row = 0u; row < shape->height; row++) {
            CHECK(memcmp(flipped_destination + (size_t)row * shape->destination_width * 4u,
                         expected + (size_t)(shape->height - 1u - row) * shape->destination_width * 4u,
                         (size_t)shape->destination_width * 4u) == 0);
        }
        flipped_compared++;
        live_target_registry_destroy(registry);
        free(plain_source);
        free(plain_destination);
        free(flipped_source);
        free(flipped_destination);
    }
    CHECK(flipped_compared == sizeof flip_cases / sizeof flip_cases[0]);
    /* the blit model inference is still needed, and a refusal moves nothing */
    {
        const parity_case *shape = &shapes[0];
        uint8_t source[64 * 8];
        uint8_t destination[64 * 8];
        uint8_t untouched[64 * 8];
        fill(source, sizeof source, 0x11u);
        fill(destination, sizeof destination, 0xA3u);
        memcpy(untouched, destination, sizeof untouched);
        gpu_pgraph_copy copy = {0};
        copy.color_format = shape->format;
        copy.source_pitch = 64u;
        copy.destination_pitch = 64u;
        copy.width = 16u;
        copy.height = 8u;
        copy.operation = LIVE_BLIT_OPERATION_SRCCOPY;
        gpu_pgraph_backend backend;
        memset(&backend, 0, sizeof backend);
        backend.output_groups = GPU_PGRAPH_OUTPUT_BLIT;
        backend.allowed_inferences = GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES;
        gpu_image from = {source, 16u, 8u, 64u};
        gpu_image to = {destination, 16u, 8u, 64u};
        uint32_t used = 0u;
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        CHECK(gpu_pgraph_replay_copy(&copy, &backend, &from, &to, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(used == 0u && memcmp(destination, untouched, sizeof destination) == 0);
        copy.operation = 1u;
        backend.allowed_inferences |= GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL;
        CHECK(gpu_pgraph_replay_copy(&copy, &backend, &from, &to, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
        copy.operation = LIVE_BLIT_OPERATION_SRCCOPY;
        copy.source_pitch = 128u; /* not the tightly packed width * 4 */
        CHECK(gpu_pgraph_replay_copy(&copy, &backend, &from, &to, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
        copy.source_pitch = 64u;
        copy.destination_pitch = 128u;
        CHECK(gpu_pgraph_replay_copy(&copy, &backend, &from, &to, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
        copy.destination_pitch = 64u;
        from.stride_bytes = 128u;
        CHECK(gpu_pgraph_replay_copy(&copy, &backend, &from, &to, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
        from.stride_bytes = 64u;
        to.stride_bytes = 128u;
        CHECK(gpu_pgraph_replay_copy(&copy, &backend, &from, &to, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
        to.stride_bytes = 64u;
        CHECK(memcmp(destination, untouched, sizeof destination) == 0);
        CHECK(gpu_pgraph_replay_copy(&copy, &backend, &from, &to, &used, &report) == GPU_PGRAPH_OK);
        CHECK(memcmp(destination, untouched, sizeof destination) != 0 && used != 0u);
    }
}

static void test_overlap_order(void)
{
    /* HQ58 outcome 3 (xemu): destination 3 pixels right is exact (buffered row), 3 ROWS below smears with period 3. */
    live_target_registry *registry = live_target_registry_create();
    live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, SIZE_16X8, true);
    uint8_t *pixels = live_target_pixels(registry, A_DATA);
    uint8_t original[64 * 8];
    fill(pixels, sizeof original, 0x21u);
    memcpy(original, pixels, sizeof original);
    live_target_blit blit = make_blit(0, 0, 3, 0, 8, 2);
    blit.destination_data = A_DATA;
    live_target_blit_plan plan;
    CHECK(live_target_apply_blit(registry, &blit, 0u, &plan, NULL) == LIVE_TARGET_OK);
    CHECK(plan.overlap && plan.same_surface && !plan.copy_image_ok);
    CHECK(memcmp(pixels + 3 * 4, original, 8 * 4) == 0);          /* row 0 shifted right, buffered */
    CHECK(memcmp(pixels + 64 + 3 * 4, original + 64, 8 * 4) == 0); /* row 1 too */
    /* rows: copy rows 0..3 (4 rows) down by 3 */
    memcpy(pixels, original, sizeof original);
    blit = make_blit(0, 0, 0, 3, 16, 4);
    blit.destination_data = A_DATA;
    CHECK(live_target_apply_blit(registry, &blit, 0u, &plan, NULL) == LIVE_TARGET_OK);
    CHECK(plan.overlap);
    /* ascending rows: row 3 = old 0, 4 = old 1, 5 = old 2, 6 = row 3 as just written = old 0 (period 3) */
    CHECK(memcmp(pixels + 3 * 64, original, 64) == 0);
    CHECK(memcmp(pixels + 6 * 64, original, 64) == 0);
    CHECK(memcmp(pixels + 6 * 64, original + 6 * 64, 64) != 0); /* a buffered copy would have kept old row 3 */
    CHECK(memcmp(pixels + 4 * 64, original + 64, 64) == 0);
    /* 3 rows above is exact */
    memcpy(pixels, original, sizeof original);
    blit = make_blit(0, 3, 0, 0, 16, 4);
    blit.destination_data = A_DATA;
    CHECK(live_target_apply_blit(registry, &blit, 0u, &plan, NULL) == LIVE_TARGET_OK);
    CHECK(plan.overlap);
    CHECK(memcmp(pixels, original + 3 * 64, 4 * 64) == 0);
    /* same surface, disjoint rectangles: no overlap, the oracle's own shape */
    blit = make_blit(0, 0, 8, 4, 8, 4);
    blit.destination_data = A_DATA;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_OK);
    CHECK(!plan.overlap && plan.same_surface && plan.copy_image_ok);
    /* same columns, disjoint rows */
    blit = make_blit(0, 0, 0, 4, 16, 4);
    blit.destination_data = A_DATA;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_OK && !plan.overlap);
    /* same rows, disjoint columns */
    blit = make_blit(0, 0, 8, 0, 8, 8);
    blit.destination_data = A_DATA;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_OK && !plan.overlap);
    live_target_registry_destroy(registry);
}

static void test_clamp_and_empty(void)
{
    /* source 16x8 pitch 128 (64 px wide row bytes 64), destination 8x8 pitch 64: width 16 px = 64 bytes, pitch 64: fits.
     * Use a destination 8x8 with pitch 64 and a 16-wide request from a 16x8 pitch 128 source: 64 bytes, not clamped.
     * A 32-wide request is 128 bytes > the narrower pitch 64: clamped to 64 bytes, the rest untouched. */
    live_target_registry *registry = live_target_registry_create();
    CHECK(live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, SIZE_16X8_P128, true) == LIVE_TARGET_OK);
    CHECK(live_target_register(registry, B_DATA, FORMAT_A8R8G8B8, SIZE_16X8, true) == LIVE_TARGET_OK);
    fill(live_target_pixels(registry, A_DATA), 128u * 8u, 0x31u);
    fill(live_target_pixels(registry, B_DATA), 64u * 8u, 0xC5u);
    uint8_t before[64 * 8];
    memcpy(before, live_target_pixels(registry, B_DATA), sizeof before);
    live_target_blit blit = {A_DATA, B_DATA, LIVE_BLIT_FORMAT_A8R8G8B8, 128, 64, 0, 0, 0, 0, 32, 2, 3};
    live_target_blit_plan plan;
    CHECK(live_target_apply_blit(registry, &blit, 0u, &plan, NULL) == LIVE_TARGET_OK);
    CHECK(plan.clamped && plan.row_bytes == 64u && plan.rows == 2u);
    CHECK(memcmp(live_target_pixels(registry, B_DATA), live_target_pixels(registry, A_DATA), 64u) == 0);
    CHECK(memcmp(live_target_pixels(registry, B_DATA) + 64, live_target_pixels(registry, A_DATA) + 128, 64u) == 0);
    CHECK(memcmp(live_target_pixels(registry, B_DATA) + 128, before + 128, sizeof before - 128u) == 0);
    CHECK(memcmp(live_target_pixels(registry, B_DATA), before, 64u) != 0);
    /* a clamped row with an x offset is unmeasured */
    blit.in_x = 1;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_OFFSET_CLAMP);
    blit.in_x = 0;
    blit.out_x = 1;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_OFFSET_CLAMP);
    /* zero width and zero height: no write, counted, no generation bump */
    blit = (live_target_blit){A_DATA, B_DATA, LIVE_BLIT_FORMAT_A8R8G8B8, 128, 64, 0, 0, 0, 0, 0, 4, 3};
    const uint64_t generation = live_target_generation(registry, B_DATA);
    memcpy(before, live_target_pixels(registry, B_DATA), sizeof before);
    CHECK(live_target_apply_blit(registry, &blit, 0u, &plan, NULL) == LIVE_TARGET_OK && plan.empty);
    blit.width = 4;
    blit.height = 0;
    CHECK(live_target_apply_blit(registry, &blit, 0u, &plan, NULL) == LIVE_TARGET_OK && plan.empty);
    CHECK(memcmp(before, live_target_pixels(registry, B_DATA), sizeof before) == 0);
    CHECK(live_target_generation(registry, B_DATA) == generation);
    live_target_stats stats = live_target_registry_stats(registry);
    CHECK(stats.blits_empty == 2u && stats.blits_clamped == 1u && stats.blits_applied == 1u);
    live_target_registry_destroy(registry);
}

static void test_alpha_formats(void)
{
    live_target_registry *registry = live_target_registry_create();
    live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, SIZE_16X8, true);
    live_target_register(registry, B_DATA, FORMAT_X8R8G8B8, SIZE_16X8, true);
    uint8_t *source = live_target_pixels(registry, A_DATA);
    for (size_t i = 0; i < 64u * 8u; i++) {
        source[i] = (i % 4u) == 3u ? 0x55u : (uint8_t)(i | 1u);
    }
    live_target_blit blit = make_blit(0, 0, 0, 0, 16, 8);
    live_target_blit_plan plan;
    /* 0xA keeps alpha */
    CHECK(live_target_apply_blit(registry, &blit, 0u, &plan, NULL) == LIVE_TARGET_OK);
    CHECK(plan.alpha == LIVE_ALPHA_KEEP && !plan.copy_image_ok); /* destination is X8R8G8B8: not a plain image copy */
    CHECK(live_target_pixels(registry, B_DATA)[3] == 0x55u);
    blit.color_format = LIVE_BLIT_FORMAT_X8R8G8B8_ALPHAFF;
    CHECK(live_target_apply_blit(registry, &blit, 0u, &plan, NULL) == LIVE_TARGET_OK);
    CHECK(plan.alpha == LIVE_ALPHA_FORCE_FF && !plan.copy_image_ok);
    CHECK(live_target_pixels(registry, B_DATA)[3] == 0xFFu && live_target_pixels(registry, B_DATA)[63] == 0xFFu);
    CHECK(live_target_pixels(registry, B_DATA)[0] == source[0] && live_target_pixels(registry, B_DATA)[2] == source[2]);
    blit.color_format = LIVE_BLIT_FORMAT_X8R8G8B8_ALPHA0;
    CHECK(live_target_apply_blit(registry, &blit, 0u, &plan, NULL) == LIVE_TARGET_OK);
    CHECK(plan.alpha == LIVE_ALPHA_FORCE_00);
    CHECK(live_target_pixels(registry, B_DATA)[3] == 0u && live_target_pixels(registry, B_DATA)[64u * 8u - 1u] == 0u);
    CHECK(live_target_pixels(registry, B_DATA)[1] == source[1]);
    live_target_registry_destroy(registry);
}

static void test_refusals(void)
{
    live_target_registry *registry = live_target_registry_create();
    live_target_register(registry, A_DATA, FORMAT_A8R8G8B8, SIZE_16X8, true);
    live_target_register(registry, B_DATA, FORMAT_A8R8G8B8, SIZE_16X8, true);
    live_target_register(registry, 0x300000u, FORMAT_R5G6B5, SIZE_16X8, true);
    live_target_register(registry, 0x400000u, 0x00000600u | (4u << 20) | (3u << 24), 0u, true);
    live_target_blit_plan plan;
    live_target_blit blit = make_blit(0, 0, 0, 0, 4, 4);
    blit.operation = 1;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_OPERATION);
    blit = make_blit(0, 0, 0, 0, 4, 4);
    blit.source_data = 0x999000u;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_NO_SOURCE);
    blit = make_blit(0, 0, 0, 0, 4, 4);
    blit.destination_data = 0x999000u;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_NO_DESTINATION);
    blit = make_blit(0, 0, 0, 0, 4, 4);
    blit.destination_data = 0x400000u;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_SWIZZLED);
    blit = make_blit(0, 0, 0, 0, 4, 4);
    blit.color_format = 0x2u;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_BLIT_FORMAT);
    blit = make_blit(0, 0, 0, 0, 4, 4);
    blit.color_format = LIVE_BLIT_FORMAT_R5G6B5; /* surfaces are 4 bytes per pixel */
    CHECK(live_target_plan_blit(registry, &blit, LIVE_TARGET_INFER_BYTE_FORMATS, &plan) ==
          LIVE_TARGET_REFUSE_BLIT_FORMAT_MISMATCH);
    blit = make_blit(0, 0, 0, 0, 4, 4);
    blit.source_pitch = 128;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_PITCH);
    blit = make_blit(0, 0, 0, 0, 4, 4);
    blit.destination_pitch = 128;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_PITCH);
    blit = make_blit(0, 5, 0, 0, 4, 4); /* source rows 5..8 of 8 */
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_OUT_OF_BOUNDS);
    blit = make_blit(0, 0, 0, 5, 4, 4);
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_OUT_OF_BOUNDS);
    blit = make_blit(13, 0, 0, 0, 4, 4); /* columns 13..16 of 16 */
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_OUT_OF_BOUNDS);
    blit = make_blit(0, 0, 13, 0, 4, 4);
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_OUT_OF_BOUNDS);
    /* the last legal rectangle is accepted (boundary) */
    blit = make_blit(12, 4, 12, 4, 4, 4);
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_OK);
    /* R5G6B5 byte path: INFERRED, opt in only */
    live_target_register(registry, 0x500000u, FORMAT_R5G6B5, SIZE_16X8, true);
    blit = make_blit(0, 0, 0, 0, 4, 4);
    blit.source_data = 0x300000u;
    blit.destination_data = 0x500000u;
    blit.color_format = LIVE_BLIT_FORMAT_R5G6B5;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_INFERRED_BYTE_FORMAT);
    CHECK(live_target_plan_blit(registry, &blit, LIVE_TARGET_INFER_BYTE_FORMATS, &plan) == LIVE_TARGET_OK);
    CHECK(plan.inferred && plan.bytes_per_pixel == 2u && plan.row_bytes == 8u);
    /* Y8 is a byte copy over any surface */
    blit.color_format = LIVE_BLIT_FORMAT_Y8;
    blit.width = 10;
    CHECK(live_target_plan_blit(registry, &blit, 0u, &plan) == LIVE_TARGET_REFUSE_INFERRED_BYTE_FORMAT);
    CHECK(live_target_plan_blit(registry, &blit, LIVE_TARGET_INFER_BYTE_FORMATS, &plan) == LIVE_TARGET_OK);
    CHECK(plan.row_bytes == 10u && plan.inferred);
    /* census: every refusal above was counted by reason */
    live_target_stats stats = live_target_registry_stats(registry);
    CHECK(stats.refusals[LIVE_TARGET_REFUSE_PITCH] == 2u);
    CHECK(stats.refusals[LIVE_TARGET_REFUSE_OUT_OF_BOUNDS] == 4u);
    CHECK(stats.refusals[LIVE_TARGET_REFUSE_INFERRED_BYTE_FORMAT] == 2u);
    CHECK(stats.refusals[LIVE_TARGET_REFUSE_OPERATION] == 1u && stats.refusals[LIVE_TARGET_REFUSE_SWIZZLED] == 1u);
    CHECK(stats.blits_refused == 14u);
    CHECK(strlen(live_target_refusal_name(LIVE_TARGET_REFUSE_PITCH)) > 0u);
    CHECK(strcmp(live_target_refusal_name(LIVE_TARGET_REFUSE_COUNT), "unknown refusal") == 0);
    live_target_registry_destroy(registry);
}

static live_present_event present(uint64_t number, uint64_t vblank, uint32_t interval, uint32_t data)
{
    live_present_event event = {.number = number, .interval = interval, .vblank = vblank, .data = data,
                                .format_word = FORMAT_A8R8G8B8, .size_word = SIZE_16X8};
    return event;
}

static void test_present(void)
{
    live_target_registry *registry = live_target_registry_create();
    live_present_schedule schedule;
    live_present_frame frame;
    live_present_init(&schedule, LIVE_PRESENT_DEFAULT_LATENCY);
    /* nothing yet: black */
    live_present_vblank(&schedule, 1, false, &frame);
    CHECK(frame.layer_count == 0u && !frame.front_new && !frame.front_held);
    /* a present at vblank 5 shows at 6 (one vblank late), not 5 */
    live_present_event event = present(1, 5, 1, A_DATA);
    CHECK(live_present_submit(&schedule, registry, &event) == LIVE_TARGET_OK);
    CHECK(live_target_find(registry, A_DATA) != NULL);
    live_present_vblank(&schedule, 5, false, &frame);
    CHECK(frame.layer_count == 0u);
    live_present_vblank(&schedule, 6, false, &frame);
    CHECK(frame.front_new && frame.front_data == A_DATA && frame.front_number == 1u && frame.layer_count == 1u);
    CHECK(frame.layers[0] == LIVE_LAYER_FRONT);
    /* nothing new: held */
    live_present_vblank(&schedule, 7, false, &frame);
    CHECK(frame.front_held && !frame.front_new && frame.front_data == A_DATA);
    /* two presents before one vblank: the newest wins, the older is superseded */
    event = present(2, 7, 1, A_DATA);
    live_present_submit(&schedule, registry, &event);
    event = present(3, 7, 1, B_DATA);
    live_present_submit(&schedule, registry, &event);
    live_present_vblank(&schedule, 8, false, &frame);
    CHECK(frame.front_new && frame.front_data == B_DATA && frame.front_number == 3u);
    CHECK(schedule.superseded == 1u && schedule.count == 0u);
    /* interval 2: a present eligible one vblank after the last shown waits for it */
    event = present(4, 8, 2, A_DATA);
    live_present_submit(&schedule, registry, &event);
    live_present_vblank(&schedule, 9, false, &frame);
    CHECK(!frame.front_new && frame.front_held && frame.front_data == B_DATA && schedule.count == 1u);
    live_present_vblank(&schedule, 10, false, &frame);
    CHECK(frame.front_new && frame.front_data == A_DATA && schedule.count == 0u);
    /* interval 0 counts as 1 */
    event = present(5, 10, 0, B_DATA);
    live_present_submit(&schedule, registry, &event);
    live_present_vblank(&schedule, 11, false, &frame);
    CHECK(frame.front_new && frame.front_data == B_DATA);
    /* movie overlay composes above the front on the same swapchain, and alone before any front */
    live_present_vblank(&schedule, 12, true, &frame);
    CHECK(frame.layer_count == 2u && frame.layers[0] == LIVE_LAYER_FRONT && frame.layers[1] == LIVE_LAYER_OVERLAY);
    live_present_schedule fresh;
    live_present_init(&fresh, 1);
    live_present_vblank(&fresh, 3, true, &frame);
    CHECK(frame.layer_count == 1u && frame.layers[0] == LIVE_LAYER_OVERLAY);
    CHECK(fresh.overlay_frames == 1u && schedule.overlay_frames == 1u);
    /* a front header that does not decode is refused and counted, nothing queued */
    event = present(6, 12, 1, 0x700000u);
    event.format_word = 0x00012A29u;
    const uint64_t submitted = schedule.submitted;
    CHECK(live_present_submit(&schedule, registry, &event) == LIVE_TARGET_REFUSE_PRESENT_UNREGISTERABLE);
    CHECK(schedule.submitted == submitted);
    CHECK(live_target_registry_stats(registry).refusals[LIVE_TARGET_REFUSE_PRESENT_UNREGISTERABLE] == 1u);
    /* queue overflow drops the oldest, counted */
    live_present_schedule full;
    live_present_init(&full, 1);
    for (uint64_t i = 0u; i < LIVE_PRESENT_QUEUE + 2u; i++) {
        event = present(100 + i, 20, 1, A_DATA);
        CHECK(live_present_submit(&full, registry, &event) == LIVE_TARGET_OK);
    }
    CHECK(full.count == LIVE_PRESENT_QUEUE && full.dropped_full == 2u);
    live_present_vblank(&full, 21, false, &frame);
    CHECK(frame.front_number == 100u + LIVE_PRESENT_QUEUE + 1u);

    /* Playback mode holds Swap fronts until their modelled timestamps reach the audio clock. */
    live_present_schedule timed;
    live_present_init(&timed, LIVE_PRESENT_DEFAULT_LATENCY);
    event = present(200u, 30u, 1u, A_DATA);
    event.media_time_ns = 1000000000u;
    CHECK(live_present_submit_media(&timed, registry, &event) == LIVE_TARGET_OK);
    live_present_media_vblank(&timed, 31u, true, false, 0u, &frame);
    CHECK(!frame.front_new && frame.layer_count == 1u && frame.layers[0] == LIVE_LAYER_OVERLAY);
    live_present_media_vblank(&timed, 31u, false, true, 999999999u, &frame);
    CHECK(!frame.front_new && frame.layer_count == 0u && timed.shown == 0u && timed.count == 1u);
    live_present_media_vblank(&timed, 31u, false, true, 1000000000u, &frame);
    CHECK(frame.front_new && frame.front_number == 200u && frame.front_data == A_DATA);
    /* A delayed audio clock never lets the next Swap title frame cover the still-playing movie. */
    event = present(201u, 31u, 1u, B_DATA);
    event.media_time_ns = 2000000000u;
    CHECK(live_present_submit_media(&timed, registry, &event) == LIVE_TARGET_OK);
    live_present_media_vblank(&timed, 32u, false, true, 1999999999u, &frame);
    CHECK(frame.front_held && frame.front_number == 200u && frame.front_data == A_DATA);
    live_present_media_vblank(&timed, 32u, false, true, 2000000000u, &frame);
    CHECK(frame.front_new && frame.front_number == 201u && frame.front_data == B_DATA);
    live_target_registry_destroy(registry);
}

/* T910: interactive title fronts follow the paced guest vblank, while movie media timing stays
 * independent. Exercise actual schedules with the observed producer/audio clock lead; the legacy
 * evidence-media queue's bounded starvation is documented, not silently changed by interactive play. */
static void test_interactive_front_clock(uint64_t lead_frames)
{
    live_target_registry *registry = live_target_registry_create();
    live_present_schedule interactive, evidence;
    live_present_init(&interactive, LIVE_PRESENT_DEFAULT_LATENCY);
    live_present_init(&evidence, LIVE_PRESENT_DEFAULT_LATENCY);
    live_present_frame front, movie_front;
    for (uint64_t number = 1u; number <= 600u; number++) {
        live_present_event event = present(number, number, 1u, A_DATA);
        event.media_time_ns = number * 16666667u;
        CHECK(live_present_submit(&interactive, registry, &event) == LIVE_TARGET_OK);
        CHECK(live_present_submit_media(&evidence, registry, &event) == LIVE_TARGET_OK);
        const uint64_t media = number > lead_frames ? (number - lead_frames) * 16666667u : 0u;
        live_present_vblank(&interactive, number + 1u, true, &front);
        live_present_media_vblank(&evidence, number + 1u, true, true, media, &movie_front);
        CHECK(front.front_new && front.front_number == number);
        CHECK(front.layer_count == 2u && front.layers[0] == LIVE_LAYER_FRONT &&
              front.layers[1] == LIVE_LAYER_OVERLAY);
        CHECK(interactive.count == 0u && interactive.dropped_full == 0u);
        CHECK(!movie_front.front_new && movie_front.layer_count == 1u &&
              movie_front.layers[0] == LIVE_LAYER_OVERLAY);
        CHECK(evidence.count <= LIVE_PRESENT_QUEUE);
    }
    CHECK(interactive.shown == 600u && evidence.shown == 0u);
    CHECK(evidence.dropped_full == 600u - LIVE_PRESENT_QUEUE);
    /* A rebuffered/frozen movie clock leaves the audio-timed queue untouched. The title front can
     * keep presenting; overlay removal has no effect on the front's timestamp or vblank identity. */
    live_present_media_vblank(&evidence, 601u, true, false, 0u, &movie_front);
    CHECK(!movie_front.front_new && evidence.count == LIVE_PRESENT_QUEUE);
    live_present_vblank(&interactive, 602u, false, &front);
    CHECK(front.front_held && front.front_number == 600u && front.layer_count == 1u);
    CHECK(front.layers[0] == LIVE_LAYER_FRONT);
    /* When the media clock reaches the last event, ordinary evidence eligibility is preserved. */
    live_present_media_vblank(&evidence, 602u, false, true, 600ull * 16666667u, &movie_front);
    CHECK(movie_front.front_new && movie_front.front_number == 600u);
    CHECK(evidence.count == 0u && movie_front.layer_count == 1u);
    live_target_registry_destroy(registry);
}

int main(void)
{
    test_decode();
    test_registry();
    test_oracle_agreement();
    test_replay_planner_parity();
    test_overlap_order();
    test_clamp_and_empty();
    test_alpha_formats();
    test_refusals();
    test_present();
    test_interactive_front_clock(24u); /* 400ms refill lead */
    test_interactive_front_clock(240u); /* four-second audio ring */
    printf("live_target: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
