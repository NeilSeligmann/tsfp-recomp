/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * D3D8 display-mode enumeration and the mode matcher (src/gpu/d3d8_display.c).
 *
 * EVERY EXPECTED NUMBER IS A LITERAL derived by hand from the original's compare chain over the
 * synthetic table in test_d3d8_support.h, and the logic those literals encode was established
 * against the original machine code: under the emulated oracle the title's real table gives 36
 * modes for the default capability word and 24 for composite, and this port agreed on all nine
 * pack-and-region combinations, mode by mode. A synthetic table is used here because the real one
 * is read from the user's executable and cannot be committed.
 *
 * WHAT BREAKS EACH CHECK is in the comment above it.
 */

#include "test_d3d8_support.h"

#include "d3d8_display.h"

#define HW_OBJECT_PAGE_OFFSET 0x400u

/* --- capabilities ------------------------------------------------------------------------ */

static void test_capabilities_are_asked_once_and_cached(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    CHECK_EQ_U32(load(D3D8_GLOBAL_AV_CAPABILITIES), 0u);

    /* The kernel's answer for the HDTV pack on an NA console, and where it lands. MUTATION:
     * the call dropped, or the result pointer not the cache word, and this reads 0. */
    CHECK_EQ_U32(d3d8_display_capabilities(), 0x00480104u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_AV_CAPABILITIES), 0x00480104u);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 1u);

    /* A cached non-zero word is NOT asked again. MUTATION: an unconditional query makes the
     * second count 2. */
    CHECK_EQ_U32(d3d8_display_capabilities(), 0x00480104u);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 1u);

    /* A cached ZERO is asked again, which the original does and which keeps a late
     * configuration visible. MUTATION: caching "was asked" instead of the value leaves 1. */
    store(D3D8_GLOBAL_AV_CAPABILITIES, 0u);
    CHECK_EQ_U32(d3d8_display_capabilities(), 0x00480104u);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 2u);
    environment_end();

    environment_begin(KERNEL_AV_PACK_COMPOSITE);
    CHECK_EQ_U32(d3d8_display_capabilities(), 0x00400101u);
    environment_end();
    environment_begin(KERNEL_AV_PACK_SVIDEO);
    CHECK_EQ_U32(d3d8_display_capabilities(), 0x00400106u);
    environment_end();
}

static void test_unconfigured_av_gives_zero_modes(void)
{
    /* No claim is made before the operator chooses, so the capability word stays 0 and the title
     * is shown no modes at all. MUTATION: a default answer in the port makes this non-zero. */
    environment_begin(KERNEL_AV_PACK_COUNT);
    CHECK_EQ_U32(d3d8_display_capabilities(), 0u);
    CHECK_EQ_U32(d3d8_adapter_mode_count(), 0u);
    environment_end();
}

/* --- the block finder -------------------------------------------------------------------- */

static void test_block_finder(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    /* Standard 1 starts at row 0 (pack 5), the first row with pack 4 is row 1. MUTATION: the
     * second loop dropped returns row 0. */
    CHECK_EQ_U32(d3d8_display_mode_block(), 0x003E2000u + 1u * 12u);
    environment_end();

    environment_begin(KERNEL_AV_PACK_COMPOSITE);
    /* Pack 1 has no block: the walk passes every pack-4 and pack-3 row and stops on the first
     * pack-0 row, row 9. MUTATION: stopping on the requested pack only walks off the block. */
    CHECK_EQ_U32(d3d8_display_mode_block(), 0x003E2000u + 9u * 12u);
    /* Pack 0 asked for explicitly lands on the same row. */
    store(D3D8_GLOBAL_AV_CAPABILITIES, 0x00400100u);
    CHECK_EQ_U32(d3d8_display_mode_block(), 0x003E2000u + 9u * 12u);

    /* A standard with no rows (4) lands on the terminator row, 184. MUTATION: an off-by-one in
     * the row bound returns row 183 or 185. */
    store(D3D8_GLOBAL_AV_CAPABILITIES, 0x00400401u);
    CHECK_EQ_U32(d3d8_display_mode_block(), 0x003E2000u + 184u * 12u);
    environment_end();
}

/* --- present flags ----------------------------------------------------------------------- */

static void test_present_flags(void)
{
    /* Each pair is (row flags, mode flags) from 0x003DBD65. MUTATION: any bit mapped to the
     * wrong value fails exactly one line. */
    CHECK_EQ_U32(d3d8_display_present_flags(0x00000000u), 0x040u);
    CHECK_EQ_U32(d3d8_display_present_flags(0x00010000u), 0x050u);
    CHECK_EQ_U32(d3d8_display_present_flags(0x00200000u), 0x020u);
    CHECK_EQ_U32(d3d8_display_present_flags(0x01000000u), 0x0A0u);
    /* Interlaced wins over field: the second test is the `else`. */
    CHECK_EQ_U32(d3d8_display_present_flags(0x01200000u), 0x020u);
    CHECK_EQ_U32(d3d8_display_present_flags(0x02000000u), 0x140u);
    CHECK_EQ_U32(d3d8_display_present_flags(0x03210000u), 0x130u);
    CHECK_EQ_U32(d3d8_display_present_flags(0x02480104u), 0x140u);
}

/* --- counting ---------------------------------------------------------------------------- */

static void test_mode_count(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    /* HDTV with 480p, caps 0x00480104. Rows 1 to 7 are the pack-4 block:
     *   1 accepted (480p row, 60 Hz, HD bit shared)
     *   2 refused (720p row, the capability word has 480p only)
     *   3 refused (widescreen row, capability word is not widescreen)
     *   4 refused (50 Hz class, the word carries 60 Hz)
     *   5, 6, 7 accepted.
     * Four accepted rows, four formats each. MUTATION: dropping the HD test accepts row 2, the
     * widescreen test row 3, the refresh test row 4, and the count moves off 16. */
    CHECK_EQ_U32(d3d8_adapter_mode_count(), 16u);
    environment_end();

    environment_begin(KERNEL_AV_PACK_COMPOSITE);
    /* Pack 1 uses the fallback block: rows 9 and 10 are both 60 Hz and both accepted. Two rows,
     * four modes each. The pack is not 4, so the HD test is off. */
    CHECK_EQ_U32(d3d8_adapter_mode_count(), 8u);
    environment_end();

    environment_begin(KERNEL_AV_PACK_HDTV);
    /* A capability word without the 60 Hz class bit refuses every row: the title is shown no
     * modes. MUTATION: ignoring the refresh class makes this non-zero. */
    store(D3D8_GLOBAL_AV_CAPABILITIES, 0x00080104u);
    CHECK_EQ_U32(d3d8_adapter_mode_count(), 0u);
    environment_end();
}

/* --- enumeration ------------------------------------------------------------------------- */

static void read_mode(uint32_t at, uint32_t out[5])
{
    for (unsigned index = 0u; index < 5u; index++) {
        out[index] = load(at + index * 4u);
    }
}

static void test_enumeration(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t out = SCRATCH_DATA;
    uint32_t mode[5];

    /* Mode 0: row 1, variant 0. Width and height from the size word, 60 Hz from the class bit,
     * present flags 0x140 (progressive and the 10:11 aspect), format 0x1E. MUTATION: swapping
     * width and height, the 60/50 choice, or the flags helper fails here. */
    for (unsigned index = 0u; index < 5u; index++) {
        store(out + index * 4u, 0xDEADBEEFu);
    }
    CHECK_EQ_U32(d3d8_adapter_enum_mode(0u, out), 0u);
    read_mode(out, mode);
    CHECK_EQ_U32(mode[0], 640u);
    CHECK_EQ_U32(mode[1], 480u);
    CHECK_EQ_U32(mode[2], 60u);
    CHECK_EQ_U32(mode[3], 0x140u);
    CHECK_EQ_U32(mode[4], 0x1Eu);

    /* The four variants of one row differ only in the format. MUTATION: a reordered jump table
     * or `& 3` replaced by `% 3` breaks one. */
    static const uint32_t formats[4] = {0x1E, 0x11, 0x1C, 0x12};
    for (uint32_t variant = 0u; variant < 4u; variant++) {
        CHECK_EQ_U32(d3d8_adapter_enum_mode(variant, out), 0u);
        CHECK_EQ_U32(load(out + 16u), formats[variant]);
        CHECK_EQ_U32(load(out), 640u);
    }

    /* Mode 4 is the second accepted row (row 5): 720x480, progressive only (0x40). MUTATION: a
     * shift of 3 for the row ordinal lands on a refused row. */
    CHECK_EQ_U32(d3d8_adapter_enum_mode(4u, out), 0u);
    read_mode(out, mode);
    CHECK_EQ_U32(mode[0], 720u);
    CHECK_EQ_U32(mode[1], 480u);
    CHECK_EQ_U32(mode[3], 0x40u);
    CHECK_EQ_U32(mode[4], 0x1Eu);

    /* Mode 8 is the third accepted row (6), mode 12 the fourth (7), both 320x120. */
    CHECK_EQ_U32(d3d8_adapter_enum_mode(8u, out), 0u);
    CHECK_EQ_U32(load(out), 320u);
    CHECK_EQ_U32(load(out + 4u), 120u);
    CHECK_EQ_U32(d3d8_adapter_enum_mode(15u, out), 0u);
    CHECK_EQ_U32(load(out + 16u), 0x12u);

    /* One past the end (index 16 is row ordinal 4) is D3DERR_INVALIDCALL and writes NOTHING. */
    for (unsigned index = 0u; index < 5u; index++) {
        store(out + index * 4u, 0xDEADBEEFu);
    }
    CHECK_EQ_U32(d3d8_adapter_enum_mode(16u, out), 0x8876086Cu);
    for (unsigned index = 0u; index < 5u; index++) {
        CHECK_EQ_U32(load(out + index * 4u), 0xDEADBEEFu);
    }
    environment_end();

    /* The composite pack has two accepted rows. Row 9 is 640x480 at 60 Hz, not interlaced so 0x40,
     * and row 10 is 720x480 interlaced so 0x20. Mode 8 is past them. */
    environment_begin(KERNEL_AV_PACK_COMPOSITE);
    const uint32_t composite_out = SCRATCH_DATA;
    CHECK_EQ_U32(d3d8_adapter_enum_mode(0u, composite_out), 0u);
    CHECK_EQ_U32(load(composite_out), 640u);
    CHECK_EQ_U32(load(composite_out + 8u), 60u);
    CHECK_EQ_U32(load(composite_out + 12u), 0x40u);
    CHECK_EQ_U32(d3d8_adapter_enum_mode(4u, composite_out), 0u);
    CHECK_EQ_U32(load(composite_out), 720u);
    CHECK_EQ_U32(load(composite_out + 12u), 0x20u);
    CHECK_EQ_U32(d3d8_adapter_enum_mode(8u, composite_out), 0x8876086Cu);
    environment_end();
}

/* --- the matcher ------------------------------------------------------------------------- */

static d3d8_mode_request title_request(uint32_t flags)
{
    const d3d8_mode_request request = {
        .width = 640u,
        .height = 480u,
        .refresh = 60u,
        .flags = flags,
        .format = 6u,
        .interval = 1u,
        .pitch = 0xA00u,
    };
    return request;
}

static void test_match_success_stores_the_row(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t hw = SCRATCH_DATA;
    const d3d8_mode_request request = title_request(0x140u);

    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0u);
    /* The normalised format (6 becomes 0x12), the pitch, the row's MODE WORD, the row's flags and
     * the two marker words. MUTATION: any field moved or dropped fails one line. */
    CHECK_EQ_U32(load(hw + 0x0Cu), 0x12u);
    CHECK_EQ_U32(load(hw + 0x04u), 0xA00u);
    CHECK_EQ_U32(load(hw + 0x08u), 0x88110F01u);
    CHECK_EQ_U32(load(hw + 0x1B4u), 0x02480104u);
    CHECK_EQ_U32(load(hw + 0x1B8u), 1u);
    CHECK_EQ_U32(load(hw + 0x7DCu), 1u);
    environment_end();
}

static void test_match_slot_index(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t hw = SCRATCH_DATA;
    const d3d8_mode_request request = title_request(0x140u);
    store(hw + 0x7E4u, 2u);
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0u);
    /* The marker goes in the slot the display object names, not slot 0. */
    CHECK_EQ_U32(load(hw + 0x7DCu + 2u * 4u), 1u);
    CHECK_EQ_U32(load(hw + 0x7DCu), 0u);
    environment_end();
}

static void test_match_failures_store_nothing(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t hw = SCRATCH_DATA;

    /* Widescreen requested (0x110) against a non-widescreen capability word: the 640x480 row
     * disagrees on the widescreen bit and the others on size. MUTATION: a widescreen test that
     * is `!=` instead of the xor of the two bits matches the 640x480 row. */
    d3d8_mode_request request = title_request(0x110u);
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);
    CHECK_EQ_U32(load(hw + 0x08u), 0u);
    CHECK_EQ_U32(load(hw + 0x1B8u), 0u);

    /* Right width, wrong height, and the other way round: both must match, not either. MUTATION:
     * dropping the height or the width comparison serves 640x240 or 641x480 from the 640x480
     * row. */
    request = title_request(0x140u);
    request.height = 240u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);
    request = title_request(0x140u);
    request.width = 641u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);

    /* The 720p row is refused outright under this word (480p only), so a 1280x720 request has
     * no row. MUTATION: dropping the HD test finds row 2. */
    request = title_request(0x100u);
    request.width = 1280u;
    request.height = 720u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);

    /* The 640x480 row has no aspect... it has, so a request without the 10:11 bit (flags 0x040,
     * bit 8 clear) disagrees on it. MUTATION: ignoring bit 25 of the row matches. */
    request = title_request(0x040u);
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);

    /* Interlaced requested (0x20): row 1 is not interlaced and has no field bit, so it is
     * refused. MUTATION: the scan test inverted accepts it. */
    request = title_request(0x120u);
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);

    /* A 50 Hz request needs the 50 Hz class and the word carries only 60: nothing matches. */
    request = title_request(0x140u);
    request.refresh = 50u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);
    environment_end();
}

static void test_first_agreeing_row_decides(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t hw = SCRATCH_DATA;
    /* Rows 6 and 7 are both 320x120 480p 60 Hz. Row 6 has a ZERO mode word, row 7 a usable one.
     * The original stops at the first row that agrees and fails on its zero mode word instead of
     * reading on to row 7. MUTATION: continuing the search returns success with row 7's word. */
    d3d8_mode_request request = title_request(0x040u);
    request.width = 320u;
    request.height = 120u;
    request.flags = 0x040u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);
    CHECK_EQ_U32(load(hw + 0x08u), 0u);
    environment_end();
}

static void test_pack_zero_accepts_the_first_row(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t hw = SCRATCH_DATA;
    /* A capability word whose pack is 0 takes the first row of its block without matching a
     * thing, mode word 0 included. 1920x1080 is in no row. MUTATION: searching anyway fails. */
    store(D3D8_GLOBAL_AV_CAPABILITIES, 0x00400100u);
    d3d8_mode_request request = title_request(0x100u);
    request.width = 1920u;
    request.height = 1080u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0u);
    CHECK_EQ_U32(load(hw + 0x08u), 0u);
    CHECK_EQ_U32(load(hw + 0x1B4u), 0x00400100u);
    environment_end();
}

static void test_scan_type_must_agree(void)
{
    environment_begin(KERNEL_AV_PACK_COMPOSITE);
    const uint32_t hw = SCRATCH_DATA;
    /* Row 10 is 720x480 and interlaced. A progressive request (0x40) refuses it, an interlaced
     * one (0x20) takes it, and a request that names neither takes it too. MUTATION: dropping the
     * interlaced-row refusal accepts the progressive request, dropping the interlaced acceptance
     * refuses the second. */
    d3d8_mode_request request = title_request(0x040u);
    request.width = 720u;
    request.height = 480u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);

    request.flags = 0x020u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0u);
    CHECK_EQ_U32(load(hw + 0x08u), 0x88770F08u);
    CHECK_EQ_U32(load(hw + 0x1B4u), 0x00600100u);

    request.flags = 0x000u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0u);

    /* A field-flagged request (0x80) drops the interlaced bit of the scan type, so 0xA0 asks for
     * nothing about scan: the same row serves it, but its field bit (24) must agree and the row
     * has none, so the request is refused. */
    request.flags = 0x0A0u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);
    environment_end();
}

static void test_capability_word_must_carry_the_matched_class(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t hw = SCRATCH_DATA;
    /* A 50 Hz request picks the 50 Hz class, and only row 4 carries it (800x576). The geometry
     * and flags agree, but the capability word has no 50 Hz class, so the call FAILS rather than
     * serving a row the console cannot show. MUTATION: dropping the admission test succeeds. */
    d3d8_mode_request request = title_request(0x000u);
    request.width = 800u;
    request.height = 576u;
    request.refresh = 50u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);
    CHECK_EQ_U32(load(hw + 0x1B8u), 0u);
    environment_end();
}

static void test_hd_rows_are_exempt_from_the_widescreen_test(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t hw = SCRATCH_DATA;
    /* Standard 2 (NTSC-J) has one row, row 11: 1280x720, 720p, widescreen, 60 Hz. The capability
     * word 0x00420204 is HDTV with 720p and 60 Hz but NOT widescreen. A widescreen row is normally
     * refused by such a word, but a 720p or 1080i row under the HDTV pack is exempt from that test
     * (0x003D9057, 0x003DC05A). MUTATION: dropping the exemption refuses the row in the counter,
     * the enumerator and the matcher. */
    store(D3D8_GLOBAL_AV_CAPABILITIES, 0x00420204u);
    CHECK_EQ_U32(d3d8_display_mode_block(), 0x003E2000u + 11u * 12u);
    CHECK_EQ_U32(d3d8_adapter_mode_count(), 4u);

    const uint32_t out = hw + 0x100u;
    CHECK_EQ_U32(d3d8_adapter_enum_mode(0u, out), 0u);
    CHECK_EQ_U32(load(out), 1280u);
    CHECK_EQ_U32(load(out + 4u), 720u);
    CHECK_EQ_U32(load(out + 8u), 60u);
    CHECK_EQ_U32(load(out + 12u), 0x50u);

    /* Widescreen requested (0x10): the row matches, and the exemption lets the matcher accept it
     * against a capability word with no widescreen bit. */
    d3d8_mode_request request = title_request(0x010u);
    request.width = 1280u;
    request.height = 720u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0u);
    CHECK_EQ_U32(load(hw + 0x08u), 0x88990F0Au);
    environment_end();
}

static void test_widescreen_equality_and_admission_are_separate_tests(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t hw = SCRATCH_DATA;

    /* EQUALITY: a widescreen-capable word (0x00490104) and a widescreen request for 640x480. The
     * only 640x480 row is not widescreen, so the request is refused on the bit comparison alone.
     * MUTATION: dropping the comparison serves the request from that row. */
    store(D3D8_GLOBAL_AV_CAPABILITIES, 0x00490104u);
    d3d8_mode_request request = title_request(0x110u);
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);

    /* ADMISSION: a non-widescreen word (0x00480104) and a widescreen request for 720x480, which
     * row 3 (widescreen) matches on every bit. The row is found and then refused because the word
     * cannot show widescreen. MUTATION: dropping the admission serves it. */
    store(D3D8_GLOBAL_AV_CAPABILITIES, 0x00480104u);
    request = title_request(0x010u);
    request.width = 720u;
    request.height = 480u;
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0x80004005u);
    CHECK_EQ_U32(load(hw + 0x1B8u), 0u);

    /* The same request against the widescreen word is served by row 3. */
    store(D3D8_GLOBAL_AV_CAPABILITIES, 0x00490104u);
    CHECK_EQ_U32(d3d8_display_match_mode(hw, &request), 0u);
    CHECK_EQ_U32(load(hw + 0x08u), 0x88220F03u);
    environment_end();
}

static void test_timing_override_is_fatal(void)
{
    environment_begin(KERNEL_AV_PACK_COMPOSITE);
    const uint32_t hw = SCRATCH_DATA;
    /* Flag 0x200 with a 50 Hz request on a 60 Hz word reprograms CRTC timing in the original,
     * which is not modelled. MUTATION: silently continuing returns without the fatal. */
    d3d8_mode_request request = title_request(0x300u);
    request.refresh = 50u;
    RUN_EXPECTING_FATAL((void)d3d8_display_match_mode(hw, &request));
    CHECK(fatal_seen);
    CHECK(fatal_address == 0x003DBF61u);
    environment_end();
}

int main(void)
{
    test_capabilities_are_asked_once_and_cached();
    test_unconfigured_av_gives_zero_modes();
    test_block_finder();
    test_present_flags();
    test_mode_count();
    test_enumeration();
    test_match_success_stores_the_row();
    test_match_slot_index();
    test_match_failures_store_nothing();
    test_first_agreeing_row_decides();
    test_pack_zero_accepts_the_first_row();
    test_scan_type_must_agree();
    test_capability_word_must_carry_the_matched_class();
    test_hd_rows_are_exempt_from_the_widescreen_test();
    test_widescreen_equality_and_admission_are_separate_tests();
    test_timing_override_is_fatal();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
