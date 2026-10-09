/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Clear, 0x003D5EB0 (src/gpu/d3d8_frame.c). The original pushes, in order, SET_SURFACE_FORMAT when the
 * render target's format wants it, one 28-byte packet per clipped rectangle (the clear rectangle
 * 0x1D98, then the zstencil value, the colour and CLEAR_SURFACE as a count-3 run at 0x1D8C) and the
 * surface format again. Every expected word below is a literal MEASURED by running the original under
 * the oracle on the same state (tests/test_d3d8_clear_oracle.py replays the whole matrix against the
 * original and compares the ring, the D3D region and the roll-over sites byte for byte). This suite pins
 * the literals, the counters, the announcement and the refusals, because a Clear that silently wrote
 * nothing would hide the first clear a GPU path has to honour.
 */

#include "test_d3d8_support.h"

#include "d3d8_frame.h"

#define RING_BYTES 0x40000u
#define KICKOFF 0x8000u
#define SURFACES 0x00D00000u
#define RENDER_TARGET (SURFACES + 0x100u)
#define DEPTH_STENCIL (SURFACES + 0x200u)
#define RECTANGLES (SURFACES + 0x400u)
#define CONSTANTS 0x004A1000u

#define DEV_CURSOR (D3D8_DEVICE_BASE + D3D8_DEV_CURSOR)
#define DEV_LIMIT (D3D8_DEVICE_BASE + D3D8_DEV_LIMIT)

#define HEADER_RECT 0x00081D98u
#define HEADER_VALUES 0x000C1D8Cu
#define HEADER_FORMAT 0x00040208u

static unsigned roll_calls;
static uint32_t roll_end;

static void record_roll(void *context, uint32_t begin, uint32_t end)
{
    (void)context;
    (void)begin;
    roll_end = end;
    roll_calls++;
}

static void surface(uint32_t address, uint32_t format)
{
    store(address, 0x01050001u);
    store(address + 12u, (8u << 24) | (9u << 20) | (format << 8) | 1u);
    store(address + 16u, 0u);
}

/* A created-device-like state: a ring, a 640x480 viewport at scale 1.0, a render target and an optional
 * depth buffer (format 0 for none), and the library constants Clear reads (MEASURED values from the
 * retail image). Returns the cursor. */
static uint32_t seed(uint32_t colour_format, uint32_t depth_format)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_frame_reset();
    roll_calls = 0u;
    roll_end = 0u;
    map_fixed(SURFACES, 0x2000u);
    map_fixed(CONSTANTS, 0x1000u);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, RING_BYTES);
    store(D3D8_GLOBAL_KICKOFF_SIZE, KICKOFF);
    CHECK(d3d8_pushbuffer_create());
    d3d8_pushbuffer_set_consumer(record_roll, NULL);
    (void)d3d8_device_register();
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);

    static const uint8_t class_table[26] = {0, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                            2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0};
    for (uint32_t index = 0u; index < 26u; index++) {
        store_byte(0x003D625Cu + index, class_table[index]);
    }
    store_byte(GUEST_FORMAT_INFO + 0x05u, 0x91u);
    store_byte(GUEST_FORMAT_INFO + 0x06u, 0xA1u);
    store_byte(GUEST_FORMAT_INFO + 0x13u, 0x8Au);
    store(0x00475CD4u, 0x3F000000u);
    store(0x00475CACu, 0u);
    store(0x004A1BA0u, 0x4B7FFFFFu);
    store(0x004A1BA4u, 0x477FFF00u);
    store(0x004A1B90u, 0x39A08CEAu);
    store(0x004A1B94u, 0x46293E59u);
    store(0x004A1B98u, 0u);
    store(0x004A1B9Cu, 0x407FFF00u);

    surface(RENDER_TARGET, colour_format);
    store(D3D8_DEVICE_BASE + 0x1A04u, RENDER_TARGET);
    if (depth_format != 0u) {
        surface(DEPTH_STENCIL, depth_format);
    }
    store(D3D8_DEVICE_BASE + 0x1A08u, depth_format != 0u ? DEPTH_STENCIL : 0u);
    store(D3D8_DEVICE_BASE + 0xEE0u, 0u);
    store(D3D8_DEVICE_BASE + 0xEE4u, 0u);
    store(D3D8_DEVICE_BASE + 0xEE8u, 640u);
    store(D3D8_DEVICE_BASE + 0xEECu, 480u);
    store(D3D8_DEVICE_BASE + 0x95Cu, 0x3F800000u);
    store(D3D8_DEVICE_BASE + 0x960u, 0x3F800000u);
    return load(DEV_CURSOR);
}

static void place(uint32_t cursor, uint32_t limit)
{
    store(DEV_CURSOR, cursor);
    store(DEV_LIMIT, limit);
}

static void clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, uint32_t depth,
                  uint32_t stencil)
{
    const uint32_t args[6] = {count, rects, flags, color, depth, stencil};
    (void)call_stdcall(0x003D5EB0u, args, 6u);
}

static void check_words(uint32_t start, const uint32_t *expected, unsigned count)
{
    CHECK(count > 0u);
    CHECK_EQ_U32(load(DEV_CURSOR), start + count * 4u);
    for (unsigned index = 0u; index < count; index++) {
        CHECK_EQ_U32(load(start + index * 4u), expected[index]);
    }
}

static void test_the_title_clear_writes_the_measured_packet(void)
{
    const uint32_t start = seed(0x12u, 0x2Eu);
    /* InitD3D's call at 0x00024032: Count 0, Flags 0xF3, Color 0, Z 1.0, Stencil 0. */
    clear(0u, RECTANGLES, 0xF3u, 0u, 0x3F800000u, 0u);
    const uint32_t expected[7] = {HEADER_RECT, 0x027F0000u, 0x01DF0000u, HEADER_VALUES,
                                  0xFFFFFF00u, 0u,          0xF3u};
    check_words(start, expected, 7u);
    CHECK(d3d8_clear_count() == 1u);
    CHECK(d3d8_clear_nonblack_count() == 0u);
    const d3d8_clear_call last = d3d8_clear_last();
    CHECK_EQ_U32(last.count, 0u);
    CHECK_EQ_U32(last.rects, RECTANGLES);
    CHECK_EQ_U32(last.flags, 0xF3u);
    CHECK_EQ_U32(last.color, 0u);
    CHECK_EQ_U32(last.depth_bits, 0x3F800000u);
    CHECK_EQ_U32(last.stencil, 0u);

    /* The announcement, once: the commands are written, the pixels are not. */
    CHECK(captured_has("0x003d5eb0 does not model"));
    capture_clear();
    clear(0u, 0u, 0xF3u, 0u, 0x3F800000u, 0u);
    CHECK(!captured_has("0x003d5eb0"));
    CHECK(d3d8_clear_count() == 2u);
    CHECK(d3d8_hle_unmodelled_count() == 2u);
    CHECK_EQ_U32(load(DEV_CURSOR), start + 56u);
    environment_end();
}

static void test_the_other_measured_shapes(void)
{
    uint32_t start = seed(0x12u, 0x2Eu);
    /* A coloured clear to 0x00FF8040: the colour is stored as given (this format keeps it). */
    clear(0u, 0u, 0xF0u, 0x00FF8040u, 0u, 0u);
    const uint32_t colour[7] = {HEADER_RECT, 0x027F0000u, 0x01DF0000u, HEADER_VALUES,
                                0u,          0x00FF8040u, 0xF0u};
    check_words(start, colour, 7u);
    CHECK(d3d8_clear_nonblack_count() == 1u);

    /* A depth clear with a rectangle: 0.5 gives 0x7FFFFF00, the rectangle is exclusive of its far
     * edge and packed as ((far << 16) - 0x10000) | near. */
    start = load(DEV_CURSOR);
    store(RECTANGLES, 10u);
    store(RECTANGLES + 4u, 20u);
    store(RECTANGLES + 8u, 300u);
    store(RECTANGLES + 12u, 200u);
    clear(1u, RECTANGLES, 0x01u, 0u, 0x3F000000u, 0u);
    const uint32_t depth[7] = {HEADER_RECT, 0x012B000Au, 0x00C70014u, HEADER_VALUES,
                               0x7FFFFF00u, 0u,          0x01u};
    check_words(start, depth, 7u);
    CHECK(d3d8_clear_nonblack_count() == 1u);

    /* A stencil clear: the stencil byte is OR-ed into the low bits of the zstencil value. */
    start = load(DEV_CURSOR);
    clear(0u, 0u, 0x02u, 0u, 0u, 0x55u);
    const uint32_t stencil[7] = {HEADER_RECT, 0x027F0000u, 0x01DF0000u, HEADER_VALUES,
                                 0x55u,       0u,          0x02u};
    check_words(start, stencil, 7u);
    CHECK(d3d8_clear_count() == 3u);
    environment_end();
}

static void test_a_depth_only_flag_with_a_colour_word_is_not_a_colour_clear(void)
{
    (void)seed(0x12u, 0x2Eu);
    clear(0u, 0u, 0x01u, 0x00FF0000u, 0u, 0u);
    CHECK(d3d8_clear_nonblack_count() == 0u);
    clear(0u, 0u, 0xF0u, 0u, 0u, 0u);
    CHECK(d3d8_clear_nonblack_count() == 0u);
    clear(0u, 0u, 0xF0u, 0x00FF0000u, 0u, 0u);
    CHECK(d3d8_clear_nonblack_count() == 1u);
    CHECK(d3d8_clear_count() == 3u);
    environment_end();
}

static void test_the_colour_is_converted_to_the_target_format(void)
{
    /* 5-6-5 (format 0x11) and 5-5-5 (format 0x1C), MEASURED for 0x00FF8040. */
    uint32_t start = seed(0x11u, 0x2Au);
    clear(0u, 0u, 0xF0u, 0x00FF8040u, 0u, 0u);
    CHECK_EQ_U32(load(start + 20u), 0xFC08u);
    start = seed(0x1Cu, 0x2Au);
    clear(0u, 0u, 0xF0u, 0x00FF8040u, 0u, 0u);
    CHECK_EQ_U32(load(start + 20u), 0x7E08u);
    CHECK_EQ_U32(load(DEV_CURSOR), start + 28u);
    environment_end();
}

static void test_the_depth_value_follows_the_depth_format(void)
{
    /* Z 0.5 with stencil 7 in each depth format, MEASURED. */
    static const struct {
        uint32_t format;
        uint32_t value;
    } cases[] = {{0x2Au, 0x7FFFFF07u}, {0x2Bu, 0xE193E507u}, {0x2Cu, 0x7FFFu}, {0x2Du, 0xEFFFu}};
    for (unsigned index = 0u; index < 4u; index++) {
        const uint32_t start = seed(0x12u, cases[index].format);
        clear(0u, 0u, 0x01u, 0u, 0x3F000000u, 7u);
        CHECK_EQ_U32(load(start + 16u), cases[index].value);
        CHECK_EQ_U32(load(DEV_CURSOR), start + 28u);
        environment_end();
    }
}

static void test_depth_clamps_and_float_zero(void)
{
    /* Other depth values, MEASURED against the original: the integer formats clamp to their range
     * and the float formats keep zero as zero (the original skips the multiply). */
    static const struct {
        uint32_t format;
        uint32_t depth;
        uint32_t value;
    } cases[] = {{0x2Cu, 0x40000000u, 0xFFFFu},     {0x2Cu, 0xBF800000u, 0u},
                 {0x2Au, 0xBF800000u, 0u},          {0x2Au, 0x40000000u, 0xFFFFFF00u},
                 {0x2Bu, 0u, 0u},                   {0x2Du, 0u, 0u},
                 {0x2Bu, 0x3F800000u, 0xE293E500u}, {0x2Du, 0x3F800000u, 0xFFFFu}};
    for (unsigned index = 0u; index < sizeof(cases) / sizeof(cases[0]); index++) {
        const uint32_t start = seed(0x12u, cases[index].format);
        clear(0u, 0u, 0x01u, 0u, cases[index].depth, 0u);
        CHECK_EQ_U32(load(start + 16u), cases[index].value);
        environment_end();
    }
    /* A colour whose green has the low bit of the 6-bit field set tells 5-6-5 from 5-5-5. */
    uint32_t start = seed(0x11u, 0x2Au);
    clear(0u, 0u, 0xF0u, 0x00123456u, 0u, 0u);
    CHECK_EQ_U32(load(start + 20u), 0x11AAu);
    start = seed(0x1Cu, 0x2Au);
    clear(0u, 0u, 0xF0u, 0x00123456u, 0u, 0u);
    CHECK_EQ_U32(load(start + 20u), 0x08CAu);
    environment_end();
}

static void test_a_format_that_wants_it_is_rewritten_and_restored(void)
{
    /* Swizzled A8R8G8B8 with a D24S8 depth buffer: SET_SURFACE_FORMAT with bit 9 off and bit 8 on,
     * the packet, then the original word. 44 bytes. */
    const uint32_t start = seed(0x06u, 0x2Au);
    clear(0u, 0u, 0xF1u, 0x00FF8040u, 0x3F000000u, 0u);
    const uint32_t expected[11] = {HEADER_FORMAT, 0x08090128u, HEADER_RECT,   0x027F0000u,
                                   0x01DF0000u,   HEADER_VALUES, 0x7FFFFF00u, 0x00FF8040u,
                                   0xF1u,         HEADER_FORMAT, 0x08090228u};
    check_words(start, expected, 11u);
    environment_end();
}

static void test_a_clear_with_nothing_left_after_a_missing_depth_buffer_writes_nothing(void)
{
    uint32_t start = seed(0x12u, 0u);
    /* No depth buffer: the depth and stencil bits are dropped, and nothing remaining returns. */
    clear(0u, 0u, 0x03u, 0u, 0x3F800000u, 0u);
    CHECK_EQ_U32(load(DEV_CURSOR), start);
    /* With a colour bit left, the flags in the packet lose bits 0 and 1. */
    clear(0u, 0u, 0xF3u, 0x1234u, 0x3F800000u, 0u);
    const uint32_t expected[7] = {HEADER_RECT, 0x027F0000u, 0x01DF0000u, HEADER_VALUES,
                                  0u,          0x1234u,     0xF0u};
    check_words(start, expected, 7u);
    environment_end();

    /* The early return does not put a rewritten format back (0x003D5F90 jumps past it). */
    start = seed(0x06u, 0u);
    clear(0u, 0u, 0x03u, 0u, 0x3F800000u, 0u);
    CHECK_EQ_U32(load(start), HEADER_FORMAT);
    CHECK_EQ_U32(load(DEV_CURSOR), start + 8u);
    environment_end();
}

static void test_rectangles_are_clipped_and_scaled(void)
{
    uint32_t start = seed(0x12u, 0x2Au);
    /* A rectangle past the viewport on every side is clipped to it, then scaled by the viewport scale
     * (1.25 and 0.75, a bias of 0.5 truncated). MEASURED against the original. */
    store(D3D8_DEVICE_BASE + 0xEE0u, 23u);
    store(D3D8_DEVICE_BASE + 0xEE4u, 47u);
    store(D3D8_DEVICE_BASE + 0xEE8u, 400u);
    store(D3D8_DEVICE_BASE + 0xEECu, 300u);
    store(D3D8_DEVICE_BASE + 0x95Cu, 0x3FA00000u);
    store(D3D8_DEVICE_BASE + 0x960u, 0x3F400000u);
    store(RECTANGLES, (uint32_t)-50);
    store(RECTANGLES + 4u, (uint32_t)-50);
    store(RECTANGLES + 8u, 700u);
    store(RECTANGLES + 12u, 500u);
    clear(1u, RECTANGLES, 0xF0u, 0u, 0u, 0u);
    const uint32_t scaled[7] = {HEADER_RECT, 0x0210001Du, 0x01030023u, HEADER_VALUES,
                                0u,          0u,          0xF0u};
    check_words(start, scaled, 7u);
    environment_end();

    /* An empty or inverted rectangle writes no packet, and the others still do. */
    start = seed(0x12u, 0x2Au);
    store(RECTANGLES, 100u);
    store(RECTANGLES + 4u, 100u);
    store(RECTANGLES + 8u, 100u);
    store(RECTANGLES + 12u, 200u);
    store(RECTANGLES + 16u, 300u);
    store(RECTANGLES + 20u, 300u);
    store(RECTANGLES + 24u, 100u);
    store(RECTANGLES + 28u, 100u);
    store(RECTANGLES + 32u, 0u);
    store(RECTANGLES + 36u, 0u);
    store(RECTANGLES + 40u, 8u);
    store(RECTANGLES + 44u, 8u);
    clear(3u, RECTANGLES, 0xF0u, 0u, 0u, 0u);
    CHECK_EQ_U32(load(DEV_CURSOR), start + 28u);
    CHECK_EQ_U32(load(start + 4u), 0x00070000u);
    environment_end();

    /* A rectangle with no height (top equal to bottom) writes nothing either. */
    start = seed(0x12u, 0x2Au);
    store(RECTANGLES, 10u);
    store(RECTANGLES + 4u, 100u);
    store(RECTANGLES + 8u, 50u);
    store(RECTANGLES + 12u, 100u);
    clear(1u, RECTANGLES, 0xF0u, 0u, 0u, 0u);
    CHECK_EQ_U32(load(DEV_CURSOR), start);
    environment_end();
}

static void test_every_site_has_its_own_reservation(void)
{
    /* Two rectangles: 28 bytes below the limit the first packet fits and the second site rolls over. */
    uint32_t start = seed(0x12u, 0x2Au);
    const uint32_t base = load(D3D8_DEVICE_BASE + D3D8_DEV_PB_BASE);
    const uint32_t limit = load(DEV_LIMIT);
    store(RECTANGLES, 0u);
    store(RECTANGLES + 4u, 0u);
    store(RECTANGLES + 8u, 100u);
    store(RECTANGLES + 12u, 100u);
    store(RECTANGLES + 16u, 100u);
    store(RECTANGLES + 20u, 100u);
    store(RECTANGLES + 24u, 200u);
    store(RECTANGLES + 28u, 200u);
    place(limit - 28u, limit);
    clear(2u, RECTANGLES, 0xF0u, 0u, 0u, 0u);
    CHECK_EQ_U32(roll_calls, 1u);
    CHECK_EQ_U32(roll_end, limit);
    CHECK_EQ_U32(load(limit - 28u), HEADER_RECT);
    CHECK_EQ_U32(load(limit - 28u + 24u), 0xF0u);
    CHECK_EQ_U32(load(limit), HEADER_RECT);
    CHECK_EQ_U32(load(limit + 4u), 0x00C70064u);
    CHECK_EQ_U32(load(DEV_CURSOR), limit + 28u);
    (void)start;
    (void)base;

    /* Four bytes of room are enough, because the packet may use the 0x204 slack. */
    start = seed(0x12u, 0x2Au);
    const uint32_t before = load(DEV_LIMIT);
    place(before - 4u, before);
    clear(0u, 0u, 0xF0u, 0u, 0u, 0u);
    CHECK_EQ_U32(roll_calls, 0u);
    CHECK_EQ_U32(load(DEV_CURSOR), before - 4u + 28u);

    /* At the limit exactly the first site rolls. */
    start = seed(0x12u, 0x2Au);
    place(load(DEV_LIMIT), load(DEV_LIMIT));
    clear(0u, 0u, 0xF0u, 0u, 0u, 0u);
    CHECK_EQ_U32(roll_calls, 1u);
    environment_end();
}

static void test_unmeasured_shapes_stop_by_name(void)
{
    uint32_t start = seed(0x12u, 0x2Au);
    store(D3D8_DEVICE_BASE + 0x1A04u, 0u);
    RUN_EXPECTING_FATAL(clear(0u, 0u, 0xF0u, 0u, 0u, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "no render target") != NULL);
    CHECK_EQ_U32(fatal_address, 0x003D5EB0u);
    environment_end();

    /* A depth format outside the eight the jump table has: the original jumps into padding. */
    start = seed(0x12u, 0x2Au);
    store_byte(DEPTH_STENCIL + 13u, 0x35u);
    RUN_EXPECTING_FATAL(clear(0u, 0u, 0x01u, 0u, 0x3F800000u, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "depth buffer with format byte 0x35") != NULL);
    CHECK_EQ_U32(load(DEV_CURSOR), start);
    environment_end();

    /* The jump table has eight entries, 0x2A to 0x31. The ninth (0x32) is padding and is refused,
     * the eighth is not. */
    start = seed(0x12u, 0x31u);
    clear(0u, 0u, 0x01u, 0u, 0x3F000000u, 0u);
    CHECK_EQ_U32(load(DEV_CURSOR), start + 28u);
    environment_end();
    start = seed(0x12u, 0x32u);
    RUN_EXPECTING_FATAL(clear(0u, 0u, 0x01u, 0u, 0x3F000000u, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "format byte 0x32") != NULL);
    CHECK_EQ_U32(load(DEV_CURSOR), start);
    environment_end();

    /* The plan holds 256 rectangles: 256 empty ones are fine, 257 are refused. */
    start = seed(0x12u, 0x2Au);
    clear(256u, RECTANGLES, 0xF0u, 0u, 0u, 0u);
    CHECK_EQ_U32(load(DEV_CURSOR), start);
    CHECK(d3d8_clear_count() == 1u);
    environment_end();

    /* Rectangles: a null array, more than the plan holds, and a colour class the table lacks. */
    start = seed(0x12u, 0x2Au);
    RUN_EXPECTING_FATAL(clear(1u, 0u, 0xF0u, 0u, 0u, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "unreadable") != NULL);
    RUN_EXPECTING_FATAL(clear(257u, RECTANGLES, 0xF0u, 0u, 0u, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "257 rectangles is unmeasured") != NULL);
    store_byte(0x003D625Cu + 15u, 7u);
    RUN_EXPECTING_FATAL(clear(0u, 0u, 0xF0u, 0u, 0u, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "colour class 7") != NULL);
    CHECK_EQ_U32(load(DEV_CURSOR), start);
    environment_end();

    /* A command span that lands on state Clear reads is refused before the first write. */
    start = seed(0x12u, 0x2Au);
    place(0x003D5000u, 0x003D6000u);
    RUN_EXPECTING_FATAL(clear(0u, 0u, 0xF0u, 0u, 0u, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "aliases state it reads") != NULL);
    CHECK_EQ_U32(load(DEV_CURSOR), 0x003D5000u);
    environment_end();

    /* The recovered device only. */
    start = seed(0x12u, 0x2Au);
    store(D3D8_DEVICE_POINTER_SLOT, 0x00123456u);
    RUN_EXPECTING_FATAL(clear(0u, 0u, 0xF0u, 0u, 0u, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "recovered device") != NULL);
    (void)start;
    environment_end();
}

int main(void)
{
    test_the_title_clear_writes_the_measured_packet();
    test_the_other_measured_shapes();
    test_a_depth_only_flag_with_a_colour_word_is_not_a_colour_clear();
    test_the_colour_is_converted_to_the_target_format();
    test_the_depth_value_follows_the_depth_format();
    test_depth_clamps_and_float_zero();
    test_a_format_that_wants_it_is_rewritten_and_restored();
    test_a_clear_with_nothing_left_after_a_missing_depth_buffer_writes_nothing();
    test_rectangles_are_clipped_and_scaled();
    test_every_site_has_its_own_reservation();
    test_unmeasured_shapes_stop_by_name();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
