/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic binding accounting, not original-XBE equivalence evidence. */
#include "test_d3d8_support.h"
#include "d3d8_bind.h"
#include "d3d8_surface.h"

/* SetRenderTarget leaves the target size, the scales, the whole target as the viewport and the depth
 * range 0 to 1 (T440, MEASURED against the original for linear and swizzled targets of several sizes in
 * tests/test_d3d8_bind_oracle.py). The device starts from a stale viewport and scale so a store that
 * is dropped shows. */
static void check_target_words(uint32_t width, uint32_t height)
{
    CHECK_EQ_U32(d3d8_device_load32(0x954u), width);
    CHECK_EQ_U32(d3d8_device_load32(0x958u), height);
    CHECK_EQ_U32(d3d8_device_load32(0x95Cu), 0x3F800000u);
    CHECK_EQ_U32(d3d8_device_load32(0x960u), 0x3F800000u);
    CHECK_EQ_U32(d3d8_device_load32(0x964u), 0x3F800000u);
    CHECK_EQ_U32(d3d8_device_load32(0xEE0u), 0u);
    CHECK_EQ_U32(d3d8_device_load32(0xEE4u), 0u);
    CHECK_EQ_U32(d3d8_device_load32(0xEE8u), width);
    CHECK_EQ_U32(d3d8_device_load32(0xEECu), height);
    CHECK_EQ_U32(d3d8_device_load32(0xEF0u), 0u);
    CHECK_EQ_U32(d3d8_device_load32(0xEF4u), 0x3F800000u);
}

static void stale_viewport(void)
{
    static const uint32_t offsets[] = {0x954u, 0x958u, 0x95Cu, 0x960u, 0x964u, 0xEE0u,
                                       0xEE4u, 0xEE8u, 0xEECu, 0xEF0u, 0xEF4u};
    for (unsigned index = 0u; index < sizeof(offsets) / sizeof(offsets[0]); index++) {
        d3d8_device_store32(offsets[index], 0x40000000u + index);
    }
}

/* The push buffer and the identity matrix the recompute's commands and viewport matrix need. */
static void begin_with_push_buffer(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    store(0x003E3F58u, D3D8_DEVICE_BASE);
    seed_recompute_constants();
    static const uint32_t identity[16] = {
        0x3F800000u, 0u, 0u, 0u, 0u, 0x3F800000u, 0u, 0u,
        0u, 0u, 0x3F800000u, 0u, 0u, 0u, 0u, 0x3F800000u,
    };
    for (uint32_t index = 0u; index < 16u; index++)
        d3d8_device_store32(0xCA0u + index * 4u, identity[index]);
}

static void test_set_render_target_resets_the_viewport(void)
{
    begin_with_push_buffer();
    const uint32_t target = SCRATCH_DATA;
    /* Linear, 256 by 128: the Size word holds width - 1 and height - 1. */
    store(target + D3D8_SURFACE_COMMON, 0x01050001u);
    store(target + D3D8_SURFACE_FORMAT, 0x00011229u);
    store(target + D3D8_SURFACE_SIZE, (127u << 12) | 255u);
    stale_viewport();
    /* From a cleared dirty mask, so the bits SetRenderTarget itself sets show (T536: the saturated
     * mask hid them, and a mutation of the 0x300 survived): unscaled leaves exactly 0x300. */
    store(0x003E3AB8u, 0u);
    RUN_EXPECTING_FATAL(d3d8_set_render_target(target, 0u));
    CHECK(!fatal_seen);
    check_target_words(256u, 128u);
    /* The stale scale (not 1.0) changes to 1.0, which 0x003D7DF3 marks 0x10F (T533, exact: the sample
     * scale changing, not the scale differing from 1.0). */
    CHECK_EQ_U32(load(0x003E3AB8u), 0x30Fu);

    /* Swizzled, 512 by 64: no Size word, the Format word's exponents 9 and 6. */
    store(target + D3D8_SURFACE_FORMAT, (6u << 24) | (9u << 20) | (0x06u << 8) | 1u);
    store(target + D3D8_SURFACE_SIZE, 0u);
    stale_viewport();
    RUN_EXPECTING_FATAL(d3d8_set_render_target(target, 0u));
    CHECK(!fatal_seen);
    check_target_words(512u, 64u);

    /* A swizzled target is never halved: 0x003D7D55 skips the sample mode when bit 0x200 of the surface format
     * (device+0x1A0C) is set (T533, from the original bytes: tests/test_d3d8_scaled_viewport_oracle.py). */
    store(0x003E3F2Cu, 2u);
    store(0x003E3AB8u, 0u);
    stale_viewport();
    RUN_EXPECTING_FATAL(d3d8_set_render_target(target, 0u));
    CHECK(!fatal_seen);
    check_target_words(512u, 64u);

    /* Offscreen supersample kind 2 halves both recomputed dimensions of a linear target and records its
     * command. */
    store(target + D3D8_SURFACE_FORMAT, 0x00011229u);
    store(target + D3D8_SURFACE_SIZE, (127u << 12) | 255u);
    store(0x003E3AB8u, 0u);
    RUN_EXPECTING_FATAL(d3d8_set_render_target(target, 0u));
    CHECK(!fatal_seen);
    CHECK_EQ_U32(d3d8_device_load32(0x954u), 128u);
    CHECK_EQ_U32(d3d8_device_load32(0x958u), 64u);
    CHECK_EQ_U32(d3d8_device_load32(0x95Cu), 0x3F000000u);
    CHECK_EQ_U32(d3d8_device_load32(0x960u), 0x3F000000u);
    CHECK_EQ_U32(d3d8_device_load32(0x964u), 0x3F000000u);
    CHECK_EQ_U32(d3d8_device_load32(0x968u), 0xBF800000u);
    CHECK_EQ_U32(load(0x003E3AB8u), 0x30Fu);
    environment_end();
}

/* The 0x20C word of the commands one SetRenderTarget pushed, or 0xFFFFFFFF when none. */
static uint32_t pushed_pitch_word(uint32_t from, uint32_t to)
{
    for (uint32_t cursor = from; cursor + 8u <= to; cursor += 4u) {
        if (d3d8_guest_load32(cursor) == 0x0004020Cu) {
            return d3d8_guest_load32(cursor + 4u);
        }
    }
    return 0xFFFFFFFFu;
}

/* T860: method 0x20C is the TARGET's own pitch (the Size word's top byte plus one, times 64, or a swizzled target's width times
 * its bytes) in the low half and the device's depth pitch in the high half (the original at 0x003D38D0..0x003D395D, read from
 * the retail bytes). It used to be the constant 0x0A000040 for any target that is not a display surface, which the retail intro's
 * own header aliasing a frame buffer showed (pitch 64 for a 2560 byte pitch target). */
static void test_set_render_target_pushes_the_targets_own_pitch(void)
{
    begin_with_push_buffer();
    const uint32_t target = SCRATCH_DATA;
    store(target + D3D8_SURFACE_COMMON, 0x01050001u);
    store(target + D3D8_SURFACE_FORMAT, 0x00011229u);
    /* linear 640 by 480 with the pitch field 39 (2560 bytes) */
    store(target + D3D8_SURFACE_SIZE, (39u << 24) | (479u << 12) | 639u);
    d3d8_device_store32(0x94Cu, 0x0A00u); /* the depth pitch the device holds */
    store(0x003E3AB8u, 0u);
    uint32_t from = d3d8_device_load32(D3D8_DEV_CURSOR);
    RUN_EXPECTING_FATAL(d3d8_set_render_target(target, 0u));
    CHECK(!fatal_seen);
    CHECK_EQ_U32(pushed_pitch_word(from, d3d8_device_load32(D3D8_DEV_CURSOR)), 0x0A000A00u);
    /* linear 128 wide with the pitch field 1 (128 bytes): the low half follows the field, not a constant */
    store(target + D3D8_SURFACE_SIZE, (1u << 24) | (63u << 12) | 127u);
    d3d8_device_store32(0x94Cu, 0x0140u);
    from = d3d8_device_load32(D3D8_DEV_CURSOR);
    RUN_EXPECTING_FATAL(d3d8_set_render_target(target, 0u));
    CHECK(!fatal_seen);
    CHECK_EQ_U32(pushed_pitch_word(from, d3d8_device_load32(D3D8_DEV_CURSOR)), 0x01400080u);
    environment_end();
}

/* Only the sample modes measured against the original are computed: the first back buffer up to 2
 * (GLOBAL 0x003E3F28) and an offscreen target up to 4 (GLOBAL 0x003E3F2C). The next mode up stops with
 * the named refusal (T536: a drifted mutation anchor showed nothing covered it), and the last
 * measured mode must still run. Checking the text, because other refusals (the scale word) also stop. */
static void test_set_render_target_refuses_unmeasured_sample_modes(void)
{
    begin_with_push_buffer();
    const uint32_t target = SCRATCH_DATA;
    store(target + D3D8_SURFACE_COMMON, 0x01050001u);
    store(target + D3D8_SURFACE_FORMAT, 0x00011229u);
    store(target + D3D8_SURFACE_SIZE, (127u << 12) | 255u);

    store(0x003E3F2Cu, 5u);
    RUN_EXPECTING_FATAL(d3d8_set_render_target(target, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "not measured") != NULL);
    store(0x003E3F2Cu, 4u);
    RUN_EXPECTING_FATAL(d3d8_set_render_target(target, 0u));
    CHECK(!fatal_seen);
    store(0x003E3F2Cu, 0u);

    /* The target is now the first back buffer, whose mode is the multisample flags. */
    d3d8_device_store32(0x1A14u, target);
    d3d8_device_store32(0x1A18u, target); /* the front buffer header the first back buffer's recompute sizes from */
    d3d8_device_store32(0x096Cu, 0x3F800000u);
    d3d8_device_store32(0x0970u, 0x3F800000u);
    store(0x003E3F28u, 3u);
    RUN_EXPECTING_FATAL(d3d8_set_render_target(target, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "not measured") != NULL);
    store(0x003E3F28u, 2u);
    RUN_EXPECTING_FATAL(d3d8_set_render_target(target, 0u));
    CHECK(!fatal_seen);
    environment_end();
}

int main(void)
{
    test_set_render_target_resets_the_viewport();
    test_set_render_target_pushes_the_targets_own_pitch();
    test_set_render_target_refuses_unmeasured_sample_modes();
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t texture = SCRATCH_DATA;
    store(texture + D3D8_SURFACE_COMMON, 1u);
    store(texture + D3D8_SURFACE_FORMAT, 0x00001220u);
    d3d8_device_store32(0x2Cu, 9u);
    d3d8_set_texture(2u, texture);
    CHECK_EQ_U32(d3d8_device_load32(0xF90u), texture);
    CHECK_EQ_U32(load(texture), 0x80001u);
    d3d8_set_texture(2u, 0u);
    CHECK_EQ_U32(d3d8_device_load32(0xF90u), 0u);
    CHECK_EQ_U32(load(texture), 1u);
    CHECK_EQ_U32(load(texture + D3D8_SURFACE_LOCK), 9u);
    CHECK_EQ_U32(d3d8_device_load32(0x14u), 0x80000000u);
    environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
