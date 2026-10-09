/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * D3D8 surface headers and format words (src/gpu/d3d8_surface.c).
 *
 * THE TITLE COPIES FIVE DWORDS OF EACH BACK-BUFFER HEADER, so these words reach the game. Every
 * expected value is a literal the ORIGINAL machine code produced when run under the emulated
 * oracle (tools/d3dscan/oracle.py) for the 640x480 A8R8G8B8 back buffer and D24S8 depth buffer
 * the title creates: Format 0x00011229, depth Format 0x00012E29, Size 0x271DF27F, pitch 0xA00,
 * Common 0x01050001 at birth, and the surface-format word 0x128.
 */

#include "test_d3d8_support.h"

#include "d3d8_surface.h"

static void test_format_normalisation(void)
{
    /* The nine entries of the jump table at 0x003DB414, everything else passes through.
     * MUTATION: any entry changed fails one line. */
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x02u), 0x10u);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x03u), 0x1Cu);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x05u), 0x11u);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x06u), 0x12u);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x07u), 0x1Eu);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x2Au), 0x2Eu);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x2Bu), 0x2Fu);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x2Cu), 0x30u);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x2Du), 0x31u);
    /* Neighbours and out-of-range values are untouched. */
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x01u), 0x01u);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x04u), 0x04u);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x08u), 0x08u);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x29u), 0x29u);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x2Eu), 0x2Eu);
    CHECK_EQ_U32(d3d8_surface_normalise_format(0x12u), 0x12u);
}

static void test_pitch_rules(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    /* 640 pixels of format 0x12 (32 bits per pixel, info byte 0xA2 & 0x3C = 0x20) is 2560 row
     * bytes, which is 0xA00 and is in the table. MUTATION: a shift of 2 or 4 for the bits per
     * pixel gives 0x500 or 0x1400. */
    CHECK_EQ_U32(d3d8_surface_aligned_row_bytes(640u, 0x12u), 0xA00u);
    CHECK_EQ_U32(d3d8_surface_pitch_for_width(640u, 0x12u), 0xA00u);

    /* 16 bits per pixel: 640 * 2 = 1280 = 0x500, in the table. */
    CHECK_EQ_U32(d3d8_surface_pitch_for_width(640u, 0x11u), 0x500u);

    /* Rounding to 64 first: 100 pixels of 32 bits is 400 bytes, rounded to 448, and the first
     * legal pitch that is not smaller is 0x200. MUTATION: skipping the 64-byte round gives the
     * same here, so the next case pins it. */
    CHECK_EQ_U32(d3d8_surface_aligned_row_bytes(100u, 0x12u), 448u);
    CHECK_EQ_U32(d3d8_surface_pitch_for_width(100u, 0x12u), 0x200u);

    /* 513 pixels of 32 bits is 2052 bytes, rounded to 2112 (0x840), and the first legal pitch
     * not smaller is 0xA00. MUTATION: comparing with `<` instead of `<=` is invisible here, but
     * a table scan that stops early returns 0x800. */
    CHECK_EQ_U32(d3d8_surface_aligned_row_bytes(513u, 0x12u), 2112u);
    CHECK_EQ_U32(d3d8_surface_pitch_for_width(513u, 0x12u), 0xA00u);

    /* Exactly a table entry stays. */
    CHECK_EQ_U32(d3d8_surface_pitch_for_width(128u, 0x12u), 0x200u);

    /* Larger than every entry: the rounded row bytes come back unchanged (0xE000 is the largest
     * entry, 4096 pixels of 32 bits is 16384 bytes = 0x4000, which is in the table, so use
     * 16000 pixels: 64000 bytes = 0xFA00, above 0xE000). */
    CHECK_EQ_U32(d3d8_surface_pitch_for_width(16000u, 0x12u), 0xFA00u);
    environment_end();
}

static void test_linear_words(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    uint32_t format_word = 0u;
    uint32_t size_word = 0u;

    /* The back buffer: 640x480, format 0x12, pitch 0xA00. Format word 0x00011229 is one level,
     * format byte 0x12, then the constant 0x29. Size word is (pitch/64 - 1) << 24 | (height - 1)
     * << 12 | (width - 1) = 0x27 << 24 | 0x1DF << 12 | 0x27F. Byte size 480 * 2560 = 0x12C000.
     * MUTATION: any field order or off-by-one fails one of the three. */
    CHECK_EQ_U32(d3d8_surface_linear_words(640u, 480u, 0x12u, 0xA00u, &format_word, &size_word),
                 0x0012C000u);
    CHECK_EQ_U32(format_word, 0x00011229u);
    CHECK_EQ_U32(size_word, 0x271DF27Fu);

    /* The depth buffer: format 0x2E gives 0x00012E29 and the same size word. */
    CHECK_EQ_U32(d3d8_surface_linear_words(640u, 480u, 0x2Eu, 0xA00u, &format_word, &size_word),
                 0x0012C000u);
    CHECK_EQ_U32(format_word, 0x00012E29u);
    CHECK_EQ_U32(size_word, 0x271DF27Fu);

    /* A different shape, to catch width and height swapped: 320x120, pitch 0x500 (16 bit). */
    CHECK_EQ_U32(d3d8_surface_linear_words(320u, 120u, 0x11u, 0x500u, &format_word, &size_word),
                 120u * 0x500u);
    CHECK_EQ_U32(format_word, 0x00011129u);
    CHECK_EQ_U32(size_word, ((0x500u >> 6) - 1u) << 24 | (119u << 12) | 319u);
    environment_end();
}

static void test_swizzled_and_compressed_formats_are_fatal(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    uint32_t format_word = 0u;
    uint32_t size_word = 0u;
    /* Format 0x01 has the swizzled bit (info byte 0x09), 0x0C and 0x0E are the compressed
     * ones, and a pitch of 0 is the path the front buffer takes (which the device handles by
     * passing a computed pitch). MUTATION: answering them with a linear guess returns. */
    RUN_EXPECTING_FATAL((void)d3d8_surface_linear_words(64u, 64u, 0x01u, 0x100u, &format_word,
                                                       &size_word));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_surface_linear_words(64u, 64u, 0x0Cu, 0x100u, &format_word,
                                                       &size_word));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_surface_linear_words(64u, 64u, 0x0Eu, 0x100u, &format_word,
                                                       &size_word));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_surface_linear_words(64u, 64u, 0x12u, 0u, &format_word,
                                                       &size_word));
    CHECK(fatal_seen);
    environment_end();
}

static void test_header_initialisation(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t header = SCRATCH_DATA;
    for (uint32_t offset = 0u; offset < 0x18u; offset += 4u) {
        store(header + offset, 0xDEADBEEFu);
    }
    /* Common 0x01050001: reference count 1, type 5 (surface) in bits 16-18, bit 24 set. Data
     * keeps its low 28 bits. Lock and Parent are cleared. MUTATION: no mask leaves the top
     * nibble, a missed clear leaves 0xDEADBEEF. */
    d3d8_surface_init_header(header, 0x00011229u, 0x271DF27Fu, 0x81104000u);
    CHECK_EQ_U32(load(header + 0x00u), 0x01050001u);
    CHECK_EQ_U32(load(header + 0x04u), 0x01104000u);
    CHECK_EQ_U32(load(header + 0x08u), 0u);
    CHECK_EQ_U32(load(header + 0x0Cu), 0x00011229u);
    CHECK_EQ_U32(load(header + 0x10u), 0x271DF27Fu);
    CHECK_EQ_U32(load(header + 0x14u), 0u);
    environment_end();
}

static void test_header_pitch(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t header = SCRATCH_DATA;
    /* A linear surface: ((Size >> 24) + 1) * 64. 0x27 gives 0xA00. MUTATION: no +1 gives
     * 0x9C0, a shift of 5 gives 0x500. */
    store(header + 0x0Cu, 0x00011229u);
    store(header + 0x10u, 0x271DF27Fu);
    CHECK_EQ_U32(d3d8_surface_header_pitch(header), 0xA00u);

    /* A swizzled surface has no Size word: the pitch is the width (1 << exponent in Format bits
     * 20-23) times the bytes per pixel. Exponent 8 is 256 pixels, format 0x12 is 32 bits:
     * 1024. Format 0x0C is 2 bytes per pixel in the original (a doubled width): 512. Format 0x0F
     * quadruples it: 1024. */
    store(header + 0x10u, 0u);
    store(header + 0x0Cu, (8u << 20) | (0x12u << 8));
    CHECK_EQ_U32(d3d8_surface_header_pitch(header), 1024u);
    store(header + 0x0Cu, (8u << 20) | (0x0Cu << 8));
    CHECK_EQ_U32(d3d8_surface_header_pitch(header), 512u);
    store(header + 0x0Cu, (8u << 20) | (0x0Fu << 8));
    CHECK_EQ_U32(d3d8_surface_header_pitch(header), 1024u);
    environment_end();
}

static void test_references(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t surface = SCRATCH_DATA;
    const uint32_t parent = SCRATCH_DATA + 0x40u;

    /* A plain add-ref counts in the low 16 bits and returns the count. */
    store(surface, 0x01050001u);
    store(surface + 0x14u, 0u);
    CHECK_EQ_U32(d3d8_resource_add_ref(surface), 2u);
    CHECK_EQ_U32(load(surface), 0x01050002u);

    /* A surface whose own count is zero and which has a parent adds one to the parent FIRST.
     * MUTATION: dropping the recursion leaves the parent at 1. */
    store(parent, 0x01000001u);
    store(surface, 0x01050000u);
    store(surface + 0x14u, parent);
    CHECK_EQ_U32(d3d8_resource_add_ref(surface), 1u);
    CHECK_EQ_U32(load(parent), 0x01000002u);
    CHECK_EQ_U32(load(surface), 0x01050001u);

    /* A render-target reference adds 0x80000, and the parent's too when none exists yet. */
    store(surface, 0x01050002u);
    store(surface + 0x14u, 0u);
    d3d8_resource_add_render_target_ref(surface);
    CHECK_EQ_U32(load(surface), 0x010D0002u);
    d3d8_resource_add_render_target_ref(surface);
    CHECK_EQ_U32(load(surface), 0x01150002u);

    store(parent, 0x01000001u);
    store(surface, 0x01050002u);
    store(surface + 0x14u, parent);
    d3d8_resource_add_render_target_ref(surface);
    CHECK_EQ_U32(load(parent), 0x01080001u);
    CHECK_EQ_U32(load(surface), 0x010D0002u);
    /* Not the first render-target reference: the parent is left alone. */
    d3d8_resource_add_render_target_ref(surface);
    CHECK_EQ_U32(load(parent), 0x01080001u);
    environment_end();
}

static void test_surface_format_word(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t colour = SCRATCH_DATA;
    const uint32_t depth = SCRATCH_DATA + 0x40u;

    /* 0x12 (A8R8G8B8, linear) with a D24S8 depth buffer: base 8, + 0x100 for linear, + 0x20
     * for 24-bit depth = 0x128, the value the original stored at device+0x1A0C. MUTATION: each
     * term moved fails here. */
    store(colour + 0x0Cu, 0x00011229u);
    store(depth + 0x0Cu, 0x00012E29u);
    CHECK_EQ_U32(d3d8_surface_format_word(colour, depth), 0x128u);

    /* A 16-bit depth format (0x30) adds 0x10. */
    store(depth + 0x0Cu, 0x00013029u);
    CHECK_EQ_U32(d3d8_surface_format_word(colour, depth), 0x118u);

    /* No depth buffer: 0x20 for a 32-bit colour format, 0x10 for 16. */
    CHECK_EQ_U32(d3d8_surface_format_word(colour, 0u), 0x128u);

    /* Colour format 0x11 (class 4) has base 3 plus 0x100, then 0x10 for the 16-bit target. */
    store(colour + 0x0Cu, 0x00011129u);
    CHECK_EQ_U32(d3d8_surface_format_word(colour, 0u), 0x113u);

    /* A format with no entry is fatal. */
    store(colour + 0x0Cu, 0x00014029u);
    RUN_EXPECTING_FATAL((void)d3d8_surface_format_word(colour, 0u));
    CHECK(fatal_seen);
    environment_end();
}

static void test_max_depth(void)
{
    /* The four floats of 0x003DB620 by format, and the same four for 0x2A-0x2D.
     * 16777215.0, 3.4e30-ish, 65280.0 and 511.9375. */
    CHECK_EQ_U32(d3d8_surface_max_depth_bits(0x2Eu), 0x4B7FFFFFu);
    CHECK_EQ_U32(d3d8_surface_max_depth_bits(0x2Fu), 0x7149F2CAu);
    CHECK_EQ_U32(d3d8_surface_max_depth_bits(0x30u), 0x477FFF00u);
    CHECK_EQ_U32(d3d8_surface_max_depth_bits(0x31u), 0x43FFF800u);
    CHECK_EQ_U32(d3d8_surface_max_depth_bits(0x2Au), 0x4B7FFFFFu);
    CHECK_EQ_U32(d3d8_surface_max_depth_bits(0x2Bu), 0x7149F2CAu);
    CHECK_EQ_U32(d3d8_surface_max_depth_bits(0x2Cu), 0x477FFF00u);
    CHECK_EQ_U32(d3d8_surface_max_depth_bits(0x2Du), 0x43FFF800u);
    environment_begin(KERNEL_AV_PACK_HDTV);
    RUN_EXPECTING_FATAL((void)d3d8_surface_max_depth_bits(0x12u));
    CHECK(fatal_seen);
    environment_end();
}

int main(void)
{
    test_format_normalisation();
    test_pitch_rules();
    test_linear_words();
    test_swizzled_and_compressed_formats_are_fatal();
    test_header_initialisation();
    test_header_pitch();
    test_references();
    test_surface_format_word();
    test_max_depth();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
