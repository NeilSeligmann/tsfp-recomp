/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_adpcm.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#define BLOCK DSOUND_XBOX_ADPCM_BLOCK_BYTES
#define FRAMES DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK

static unsigned checks;
static unsigned failures;
#define CHECK(condition)                                                                  \
    do {                                                                                  \
        checks++;                                                                         \
        if (!(condition)) {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);           \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)
#define CHECK_EQ_U32(actual, expected) CHECK((uint32_t)(actual) == (uint32_t)(expected))

static void set_header(uint8_t *block, unsigned channel, int16_t predictor, int16_t index)
{
    const size_t offset = channel * 4u;
    const uint16_t sample_word = (uint16_t)predictor;
    const uint16_t index_word = (uint16_t)index;
    block[offset] = (uint8_t)sample_word;
    block[offset + 1u] = (uint8_t)(sample_word >> 8u);
    block[offset + 2u] = (uint8_t)index_word;
    block[offset + 3u] = (uint8_t)(index_word >> 8u);
}

static void fill_channel_nibbles(uint8_t *block, unsigned channel, uint8_t packed)
{
    for (unsigned group = 0u; group < 8u; group++) {
        const size_t cursor = 8u + group * 8u + channel * 4u;
        memset(block + cursor, packed, 4u);
    }
}

static void test_stereo_layout_and_declared_sample_count(void)
{
    uint8_t block[BLOCK] = {0};
    int16_t pcm[FRAMES * 2u];
    size_t frames = 999u;
    set_header(block, 0u, 0, 0);
    set_header(block, 1u, -1000, 0);
    fill_channel_nibbles(block, 0u, 0x11u);
    fill_channel_nibbles(block, 1u, 0x22u);
    CHECK(dsound_adpcm_xbox_decode_stereo(block, sizeof(block), pcm, FRAMES, &frames));
    CHECK_EQ_U32((uint32_t)frames, FRAMES);
    for (size_t frame = 0u; frame < FRAMES; frame++) {
        CHECK(pcm[frame * 2u] == (int16_t)frame);
        CHECK(pcm[frame * 2u + 1u] == (int16_t)(-1000 + (int32_t)frame * 3));
    }

    /* The final nibble pads the 64-frame Xbox block and is intentionally discarded. */
    block[BLOCK - 1u] = 0xF2u;
    int16_t changed[FRAMES * 2u];
    CHECK(dsound_adpcm_xbox_decode_stereo(block, sizeof(block), changed, FRAMES, &frames));
    CHECK(memcmp(changed, pcm, sizeof(pcm)) == 0);
}

static void test_sign_saturation_and_step_clamp(void)
{
    uint8_t block[BLOCK] = {0};
    int16_t pcm[FRAMES * 2u] = {0};
    size_t frames = 0u;
    set_header(block, 0u, 32760, 88);
    set_header(block, 1u, -32760, 88);
    block[8] = 0xF7u; /* left +7 then -8; right receives the same nibbles */
    block[12] = 0x8Fu;
    CHECK(dsound_adpcm_xbox_decode_stereo(block, sizeof(block), pcm, FRAMES, &frames));
    CHECK(pcm[0] == 32760 && pcm[1] == -32760);
    CHECK(pcm[2] == INT16_MAX && pcm[3] == INT16_MIN);
    CHECK(pcm[5] == INT16_MIN);
}

static void test_multiple_blocks_restart_headers(void)
{
    uint8_t blocks[2u * BLOCK] = {0};
    int16_t pcm[2u * FRAMES * 2u];
    size_t frames = 0u;
    set_header(blocks, 0u, 123, 0);
    set_header(blocks, 1u, -456, 0);
    set_header(blocks + BLOCK, 0u, 789, 0);
    set_header(blocks + BLOCK, 1u, -987, 0);
    CHECK(dsound_adpcm_xbox_decode_stereo(blocks, sizeof(blocks), pcm, 2u * FRAMES, &frames));
    CHECK_EQ_U32((uint32_t)frames, 2u * FRAMES);
    CHECK(pcm[0] == 123 && pcm[1] == -456);
    CHECK(pcm[2u * FRAMES] == 789 && pcm[2u * FRAMES + 1u] == -987);
}

static void test_rejects_invalid_shapes_without_writing_output(void)
{
    uint8_t block[BLOCK] = {0};
    int16_t pcm[FRAMES * 2u];
    size_t frames = 55u;
    memset(pcm, 0x5a, sizeof(pcm));
    set_header(block, 0u, 0, 89);
    set_header(block, 1u, 0, 0);
    CHECK(!dsound_adpcm_xbox_decode_stereo(block, sizeof(block), pcm, FRAMES, &frames));
    CHECK_EQ_U32((uint32_t)frames, 55u);
    for (size_t i = 0u; i < sizeof(pcm) / sizeof(pcm[0]); i++)
        CHECK((uint16_t)pcm[i] == 0x5a5au);

    set_header(block, 0u, 0, 0);
    set_header(block, 1u, 0, -1);
    CHECK(!dsound_adpcm_xbox_decode_stereo(block, sizeof(block), pcm, FRAMES, &frames));
    CHECK(!dsound_adpcm_xbox_decode_stereo(block, sizeof(block) - 1u, pcm, FRAMES, &frames));
    CHECK(!dsound_adpcm_xbox_decode_stereo(block, sizeof(block), pcm, FRAMES - 1u, &frames));
    CHECK(!dsound_adpcm_xbox_decode_stereo(NULL, sizeof(block), pcm, FRAMES, &frames));
    CHECK(!dsound_adpcm_xbox_decode_stereo(block, sizeof(block), NULL, FRAMES, &frames));
    CHECK(!dsound_adpcm_xbox_decode_stereo(block, sizeof(block), pcm, FRAMES, NULL));
    CHECK(dsound_adpcm_xbox_decode_stereo(NULL, 0u, NULL, 0u, &frames));
    CHECK_EQ_U32((uint32_t)frames, 0u);
}

int main(void)
{
    test_stereo_layout_and_declared_sample_count();
    test_sign_saturation_and_step_clamp();
    test_multiple_blocks_restart_headers();
    test_rejects_invalid_shapes_without_writing_output();
    if (failures != 0u) {
        fprintf(stderr, "dsound_adpcm: %u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("dsound_adpcm: %u checks passed\n", checks);
    return 0;
}
