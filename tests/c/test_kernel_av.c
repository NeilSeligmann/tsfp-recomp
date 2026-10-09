/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Av* kernel ordinals 1 AvGetSavedDataAddress, 2 AvSendTVEncoderOption, 3
 * AvSetDisplayMode and 4 AvSetSavedDataAddress (src/xbox/kernel_av.h).
 *
 * EVERY EXPECTED VALUE IS A LITERAL, never read back through a constant of the module. The
 * module's job is to hand the title a number, and a test that computed the expectation with
 * the module's own macros would pass whatever those macros said. The literals come from the
 * title's own binary: the mode table at 0x003E2000 (pack in the low byte, standard in bits
 * 8-15, capability flags above) and the certificate region word of 1 (NA).
 *
 * WHAT THE SUITE PINS, in the order it hurts to get wrong:
 *   1. THE ANSWER FOR EACH PACK AND REGION. A swapped pack code, a dropped refresh bit
 *      (the title's mode count is ZERO without bit 0x400000) or a wrong standard each makes
 *      the title see a different console, and the boot still advances.
 *   2. THE FOUR-ARGUMENT, SIX-ARGUMENT AND ONE-ARGUMENT SHAPES, from both sides. A frame
 *      with one argument too few must be refused, and one with the right count must work,
 *      because a wrong count desyncs the guest's esp forever.
 *   3. ONLY THE QUERIES WRITE. Options 9, 0xB and 0xE are set-options the title calls with
 *      a NULL result. A real result pointer on one is refused and left untouched.
 *   4. UNKNOWN OPTIONS AND UNMEASURED PARAMS ARE REFUSED, so that nothing is claimed for an
 *      option or value nobody studied. The accepted (option, param) pairs are the measured
 *      ones of the nine call sites.
 *   5. NOTHING IS CLAIMED BEFORE THE OPERATOR CHOOSES. An unconfigured option 6 query is
 *      refused and writes nothing.
 *   6. THE SAVED ADDRESS AND THE DISPLAY RECORD ARE REAL STATE, not a log line.
 *
 * DELIBERATELY FREE OF LIFTED CODE AND OF THE REAL XBE: the suite lays out its own XBE
 * header and certificate in scratch guest memory.
 */

#include "kernel_av.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_config.h"
#include "kernel_hle.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                  \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t a_ = (uint32_t)(actual);                                                \
        uint32_t e_ = (uint32_t)(expected);                                              \
        if (a_ != e_) {                                                                   \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual,  \
                   (unsigned)a_, (unsigned)e_);                                           \
            failures++;                                                                   \
        }                                                                               \
    } while (0)

/* The title ignores the NTSTATUS of option 6, but a handler that returns success for a
 * refusal would hide it from every other caller. The sign bit is what a guest `jl` reads. */
#define CHECK_FAILURE(status)                                                           \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t s_ = (uint32_t)(status);                                                \
        if ((s_ & 0x80000000u) == 0u) {                                                   \
            printf("FAIL %s:%d  %s == %#x, which a guest reads as SUCCESS\n", __FILE__,    \
                   __LINE__, #status, (unsigned)s_);                                      \
            failures++;                                                                   \
        }                                                                               \
    } while (0)

static char captured[32768];
static size_t captured_len;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(captured + captured_len, sizeof(captured) - captured_len, format,
                            args);
    va_end(args);
    if (written > 0) {
        captured_len += (size_t)written;
        if (captured_len >= sizeof(captured)) {
            captured_len = sizeof(captured) - 1u;
        }
    }
    return written;
}

static void reset_capture(void)
{
    captured[0] = '\0';
    captured_len = 0u;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

#define ORD_AV_GET_SAVED 1u
#define ORD_AV_SEND_OPTION 2u
#define ORD_AV_SET_MODE 3u
#define ORD_AV_SET_SAVED 4u
#define ORD_EX_QUERY_SETTING 24u

#define REGION_BYTES 0x2000u
#define HEADER_AT 0x0000u
#define CERT_AT 0x0200u
#define HEADER_BYTES 0x0400u
#define RESULT_AT 0x0800u
#define FRAMES_AT 0x1000u
#define FRAME_BYTES 0x100u
#define SENTINEL 0xDEADBEEFu

/* The certificate region words the title family uses: 1 NA, 2 Japan, 4 rest of world. */
#define REGION_NA 1u
#define REGION_JAPAN 2u
#define REGION_ROW 4u

static kernel_guest_ptr region;

static uint32_t read32(kernel_guest_ptr addr)
{
    uint32_t value = SENTINEL;
    if (!kernel_guest_read_u32(addr, &value)) {
        printf("FATAL could not read guest 0x%08X\n", (unsigned)addr);
        exit(EXIT_FAILURE);
    }
    return value;
}

static void write32(kernel_guest_ptr addr, uint32_t value)
{
    if (!kernel_guest_write_u32(addr, value)) {
        printf("FATAL could not write guest 0x%08X\n", (unsigned)addr);
        exit(EXIT_FAILURE);
    }
}

static void build_header(uint32_t certificate_region)
{
    write32(region + HEADER_AT, 0x48454258u); /* "XBEH" */
    write32(region + HEADER_AT + 0x108u, HEADER_BYTES);
    write32(region + HEADER_AT + 0x118u, region + CERT_AT);
    write32(region + CERT_AT + 0xA0u, certificate_region);
}

static void setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    kernel_av_reset();
    kernel_config_reset();
    CHECK_EQ_U32(kernel_av_register(), 4u);
    CHECK_EQ_U32(kernel_config_register(), 1u);

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = REGION_BYTES;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    region = guest_region_alloc(&request, &status);
    if (region == 0u) {
        printf("FATAL could not allocate the scratch region (status %#x)\n", (unsigned)status);
        exit(EXIT_FAILURE);
    }
    build_header(REGION_NA);
    kernel_hle_set_log(capture_printer);
    reset_capture();
}

static void teardown(void)
{
    kernel_av_reset();
    kernel_config_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    region = 0u;
}

/* Call an ordinal with `count` arguments. The stack limit is clamped to exactly the frame
 * built, so an argument past the last one FAILS to read instead of returning stack junk. */
static uint32_t call_av(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, region + FRAMES_AT, FRAME_BYTES, args, count)) {
        printf("FATAL could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    frame.stack_limit = region + FRAMES_AT + (count + 1u) * 4u;
    return kernel_hle_call(ordinal, &frame);
}

/* AvSendTVEncoderOption(RegisterBase, Option, Param, Result). */
static uint32_t send_option(uint32_t option, uint32_t param, uint32_t result_ptr)
{
    const uint32_t args[4] = {0u, option, param, result_ptr};
    return call_av(ORD_AV_SEND_OPTION, args, 4u);
}

static kernel_guest_ptr result_slot(void)
{
    write32(region + RESULT_AT, SENTINEL);
    return region + RESULT_AT;
}

/* ------------------------------------------------------------------------- */

/* Mutation: delete any binding in kernel_av_register(). A handler that is never bound is
 * a stub the guest stops at, so every ordinal must be implemented, not merely present. */
static void test_all_four_ordinals_are_implemented(void)
{
    setup();
    for (unsigned ordinal = 1u; ordinal <= 4u; ordinal++) {
        const kernel_entry *entry = kernel_hle_entry(ordinal);
        CHECK(entry != NULL);
        if (entry != NULL) {
            CHECK(entry->state == KERNEL_ENTRY_IMPLEMENTED);
        }
    }
    teardown();
}

/* Mutation: accept a name case-insensitively, by prefix, or add "scart" to the set. The set
 * is bounded on purpose: SCART has NTSC rows in the title's table but no NTSC console. */
static void test_the_pack_name_set_is_exactly_four(void)
{
    kernel_av_pack pack = KERNEL_AV_PACK_COUNT;
    CHECK(kernel_av_parse_pack("composite", &pack) && pack == KERNEL_AV_PACK_COMPOSITE);
    CHECK(kernel_av_parse_pack("svideo", &pack) && pack == KERNEL_AV_PACK_SVIDEO);
    CHECK(kernel_av_parse_pack("hdtv", &pack) && pack == KERNEL_AV_PACK_HDTV);

    pack = KERNEL_AV_PACK_COUNT;
    CHECK(!kernel_av_parse_pack("scart", &pack));
    /* VGA is withheld on evidence: no pack-5 row matches the title's pixel-aspect flag. */
    CHECK(!kernel_av_parse_pack("vga", &pack));
    CHECK(!kernel_av_parse_pack("HDTV", &pack));
    CHECK(!kernel_av_parse_pack("hdtv ", &pack));
    CHECK(!kernel_av_parse_pack("hdt", &pack));
    CHECK(!kernel_av_parse_pack("", &pack));
    CHECK(!kernel_av_parse_pack("none", &pack));
    CHECK(!kernel_av_parse_pack(NULL, &pack));
    CHECK(!kernel_av_parse_pack("hdtv", NULL));
    /* A refusal must not touch the output. */
    CHECK(pack == KERNEL_AV_PACK_COUNT);

    CHECK(strcmp(kernel_av_pack_name(KERNEL_AV_PACK_COMPOSITE), "composite") == 0);
    CHECK(strcmp(kernel_av_pack_name(KERNEL_AV_PACK_SVIDEO), "svideo") == 0);
    CHECK(strcmp(kernel_av_pack_name(KERNEL_AV_PACK_HDTV), "hdtv") == 0);
    CHECK(strcmp(kernel_av_pack_name(KERNEL_AV_PACK_COUNT), "?") == 0);
}

/* Mutation: change the default. It is derived from the title's highest requested mode
 * (480p, which only the HDTV pack yields), not chosen for convenience. */
static void test_the_default_pack_is_hdtv(void)
{
    CHECK(kernel_av_default_pack() == KERNEL_AV_PACK_HDTV);
}

/* ------------------------------------------------------------------------- */

static void configure_expect(kernel_av_pack pack, uint32_t cert_region, uint32_t capabilities,
                             uint32_t av_region, uint32_t video_flags, uint32_t smc_mode)
{
    kernel_av_reset();
    CHECK(kernel_av_configure(pack, cert_region));
    CHECK(kernel_av_is_configured());
    CHECK_EQ_U32(kernel_av_capabilities(), capabilities);
    CHECK_EQ_U32(kernel_av_av_region_setting(), av_region);
    CHECK_EQ_U32(kernel_av_video_flags_setting(), video_flags);
    CHECK_EQ_U32(kernel_av_smc_video_mode(), smc_mode);
}

/* Mutation: swap two pack codes, drop the 60 Hz bit (0x400000, without which the title's
 * mode count is zero), drop the 480p bit, or let PAL keep 480p. */
static void test_the_answer_for_every_pack_in_every_standard(void)
{
    setup();
    /* NTSC-M, the title's own region. Pack 1 composite, 6 svideo, 4 hdtv + 480p. */
    configure_expect(KERNEL_AV_PACK_COMPOSITE, REGION_NA, 0x00400101u, 0x100u, 0u, 6u);
    configure_expect(KERNEL_AV_PACK_SVIDEO, REGION_NA, 0x00400106u, 0x100u, 0u, 4u);
    configure_expect(KERNEL_AV_PACK_HDTV, REGION_NA, 0x00480104u, 0x100u, 0x00080000u, 1u);
    /* NTSC-J: the same blocks of the title's table, standard byte 2. */
    configure_expect(KERNEL_AV_PACK_COMPOSITE, REGION_JAPAN, 0x00400201u, 0x200u, 0u, 6u);
    configure_expect(KERNEL_AV_PACK_HDTV, REGION_JAPAN, 0x00480204u, 0x200u, 0x00080000u, 1u);
    /* PAL-I: 50 Hz (0x800000), and NO 480p, because XGetVideoFlags strips it for PAL. */
    configure_expect(KERNEL_AV_PACK_COMPOSITE, REGION_ROW, 0x00800301u, 0x300u, 0u, 6u);
    configure_expect(KERNEL_AV_PACK_HDTV, REGION_ROW, 0x00800304u, 0x300u, 0u, 1u);
    teardown();
}

/* Mutation: reorder the NA / Japan / rest-of-world priority, or treat a multi-bit word as
 * an error. A title released in several regions still has to run. */
static void test_region_priority_is_na_then_japan_then_rest_of_world(void)
{
    setup();
    configure_expect(KERNEL_AV_PACK_COMPOSITE, 3u, 0x00400101u, 0x100u, 0u, 6u);  /* NA|JP */
    configure_expect(KERNEL_AV_PACK_COMPOSITE, 5u, 0x00400101u, 0x100u, 0u, 6u);  /* NA|ROW */
    configure_expect(KERNEL_AV_PACK_COMPOSITE, 6u, 0x00400201u, 0x200u, 0u, 6u);  /* JP|ROW */
    configure_expect(KERNEL_AV_PACK_COMPOSITE, 7u, 0x00400101u, 0x100u, 0u, 6u);  /* all */
    /* The manufacturing bit is not a region. */
    configure_expect(KERNEL_AV_PACK_COMPOSITE, 0x80000001u, 0x00400101u, 0x100u, 0u, 6u);
    teardown();
}

/* Mutation: let configure succeed for a region word with no region bit, or change state
 * on failure. A refusal must leave the previous choice intact. */
static void test_a_region_with_no_bit_is_refused_and_changes_nothing(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_SVIDEO, REGION_NA));
    CHECK_EQ_U32(kernel_av_capabilities(), 0x00400106u);

    reset_capture();
    CHECK(!kernel_av_configure(KERNEL_AV_PACK_HDTV, 0u));
    CHECK(!kernel_av_configure(KERNEL_AV_PACK_HDTV, 0x80000000u));
    CHECK(!kernel_av_configure(KERNEL_AV_PACK_HDTV, 8u));
    CHECK(!kernel_av_configure(KERNEL_AV_PACK_COUNT, REGION_NA));
    CHECK_EQ_U32(kernel_av_capabilities(), 0x00400106u);
    CHECK(captured_contains("AV NOT configured"));
    teardown();
}

/* Mutation: stop announcing. "Nothing hardware-claiming may be silent" is the policy. */
static void test_configuring_announces_the_fabrication(void)
{
    setup();
    reset_capture();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_NA));
    CHECK(captured_contains("FABRICATED"));
    CHECK(captured_contains("no AV encoder"));
    CHECK(captured_contains("\"hdtv\""));
    CHECK(captured_contains("NTSC-M"));
    CHECK(captured_contains("0x00480104"));
    teardown();
}

/* ------------------------------------------------------------------------- */

/* Mutation: read the certificate at the wrong header offset or the region at the wrong
 * certificate offset. Region 2 and 4 sit in the neighbouring words so a one-slot slip is
 * visible as a different standard. */
static void test_the_region_is_read_from_the_xbe_certificate(void)
{
    setup();
    build_header(REGION_JAPAN);
    write32(region + CERT_AT + 0x9Cu, REGION_NA);  /* allowed media neighbour */
    write32(region + CERT_AT + 0xA4u, REGION_ROW); /* game ratings neighbour */
    CHECK(kernel_av_configure_from_image(KERNEL_AV_PACK_COMPOSITE, region));
    CHECK_EQ_U32(kernel_av_capabilities(), 0x00400201u);
    teardown();
}

/* Mutation: skip the magic test, or return true after a failed read. */
static void test_an_unreadable_or_foreign_header_is_refused(void)
{
    setup();
    write32(region + HEADER_AT, 0x12345678u);
    CHECK(!kernel_av_configure_from_image(KERNEL_AV_PACK_HDTV, region));
    CHECK(!kernel_av_is_configured());

    /* A certificate address below the image, past the header, or too close to the header's
     * end for the region word, each of which would be an in-range unmapped read. */
    const uint32_t bad_certificates[5] = {0u, 0x00000200u, region + HEADER_BYTES,
                                          region + HEADER_BYTES - 0xA0u, region + 0x600u};
    for (unsigned i = 0u; i < 5u; i++) {
        build_header(REGION_NA);
        /* A valid NA region word wherever the address is mapped, so that only the range
         * check can be what refuses it. */
        if (bad_certificates[i] >= region) {
            write32(bad_certificates[i] + 0xA0u, REGION_NA);
        }
        write32(region + HEADER_AT + 0x118u, bad_certificates[i]);
        CHECK(!kernel_av_configure_from_image(KERNEL_AV_PACK_HDTV, region));
        CHECK(!kernel_av_is_configured());
    }
    /* Exactly enough room for the region word is accepted. */
    build_header(REGION_NA);
    write32(region + HEADER_AT + 0x118u, region + HEADER_BYTES - 0xA4u);
    write32(region + HEADER_BYTES - 4u, REGION_JAPAN);
    CHECK(kernel_av_configure_from_image(KERNEL_AV_PACK_COMPOSITE, region));
    CHECK_EQ_U32(kernel_av_capabilities(), 0x00400201u);
    kernel_av_reset();

    CHECK(!kernel_av_configure_from_image(KERNEL_AV_PACK_HDTV, 0u));
    CHECK(!kernel_av_is_configured());
    teardown();
}

/* ------------------------------------------------------------------------- */

/* Mutation: answer an unconfigured option 6 with a default. No hardware claim may be made
 * before the operator (or the host's default) has chosen. */
static void test_option_6_before_configure_is_refused_and_writes_nothing(void)
{
    setup();
    kernel_guest_ptr slot = result_slot();
    CHECK_FAILURE(send_option(6u, 0u, slot));
    CHECK_EQ_U32(read32(slot), SENTINEL);
    CHECK_EQ_U32(kernel_av_refused_count(), 1u);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 0u);
    CHECK(captured_contains("REFUSED"));
    teardown();
}

/* Mutation: write the answer to the wrong argument slot (the title passes the result
 * pointer LAST, after a Param of 0), or write nothing. */
static void test_option_6_writes_the_capability_word_through_the_result_pointer(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_NA));
    reset_capture();
    kernel_guest_ptr slot = result_slot();
    CHECK_EQ_U32(send_option(6u, 0u, slot), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(slot), 0x00480104u);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 1u);
    /* The per-call line must say it, independently of the configure announcement. */
    CHECK(captured_contains("FABRICATED"));

    /* The same call under another pack answers differently, so the value is not constant. */
    CHECK(kernel_av_configure(KERNEL_AV_PACK_COMPOSITE, REGION_NA));
    slot = result_slot();
    CHECK_EQ_U32(send_option(6u, 0u, slot), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(slot), 0x00400101u);
    teardown();
}

/* Mutation: dereference a NULL result pointer, or report success without writing. */
static void test_a_query_with_no_result_pointer_is_refused(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_NA));
    CHECK_FAILURE(send_option(6u, 0u, 0u));
    CHECK_FAILURE(send_option(0xFu, 0u, 0u));
    CHECK_FAILURE(send_option(0x10u, 0u, 0u));
    CHECK_EQ_U32(kernel_av_refused_count(), 3u);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 0u);
    teardown();
}

/* Mutation: change the field or encoder answer. 0x10 is compared with 2 by the title, so
 * the answer must NOT be 2, which would select a chip-specific workaround. */
static void test_field_and_encoder_queries_write_their_answers(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_NA));
    kernel_guest_ptr slot = result_slot();
    CHECK_EQ_U32(send_option(0xFu, 0u, slot), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(slot), 0u);
    slot = result_slot();
    CHECK_EQ_U32(send_option(0x10u, 0u, slot), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(slot), 1u);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 2u);
    teardown();
}

/* Every (option, param, result) the nine call sites send, MEASURED from default.xbe by
 * disassembly of each push sequence (docs/av-policy.md section 2). Each row is accepted. The
 * three queries get a result slot and write it, the five set-calls pass NULL and are ignored.
 * Mutation: refuse any measured row, or count a query as ignored (or the reverse). */
static void test_every_measured_call_site_is_accepted(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_NA));
    static const struct {
        uint32_t option;
        uint32_t param;
    } queries[3] = {{6u, 0u}, {0xFu, 0u}, {0x10u, 0u}};
    for (unsigned i = 0u; i < 3u; i++) {
        kernel_guest_ptr slot = result_slot();
        CHECK_EQ_U32(send_option(queries[i].option, queries[i].param, slot), STATUS_SUCCESS);
        CHECK(read32(slot) != SENTINEL);
    }
    static const struct {
        uint32_t option;
        uint32_t param;
    } sets[5] = {{9u, 0u}, {9u, 1u}, {0xBu, 0u}, {0xBu, 5u}, {0xEu, 0u}};
    for (unsigned i = 0u; i < 5u; i++) {
        CHECK_EQ_U32(send_option(sets[i].option, sets[i].param, 0u), STATUS_SUCCESS);
    }
    CHECK_EQ_U32(kernel_av_fabricated_count(), 3u);
    CHECK_EQ_U32(kernel_av_ignored_option_count(), 5u);
    CHECK_EQ_U32(kernel_av_refused_count(), 0u);
    CHECK(captured_contains("IGNORED"));
    teardown();
}

/* The title passes a NULL result to options 9, 0xB and 0xE at every site. A caller that hands
 * one a real pointer expects an answer these options never give, so it is refused and the
 * pointer is left alone, rather than accepted with the slot silently stale.
 * Mutation: accept a non-NULL result for a set-option, or write through it. */
static void test_a_set_option_with_a_result_pointer_is_refused(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_NA));
    static const uint32_t set_options[3] = {9u, 0xBu, 0xEu};
    for (unsigned i = 0u; i < 3u; i++) {
        kernel_guest_ptr slot = result_slot();
        CHECK_FAILURE(send_option(set_options[i], 0u, slot));
        CHECK_EQ_U32(read32(slot), SENTINEL);
    }
    CHECK_EQ_U32(kernel_av_refused_count(), 3u);
    CHECK_EQ_U32(kernel_av_ignored_option_count(), 0u);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 0u);
    CHECK(captured_contains("REFUSED"));
    teardown();
}

/* A param the title never sends is refused, whatever the option. Each row sits directly beside
 * a measured value, so widening a range by one is caught: 9 takes 0 and 1 only, 0xB takes 0 and
 * 5 only (SetFlickerFilter has one caller, with 5, plus the literal 0), 0xE takes 0 only
 * (SetSoftDisplayFilter has one caller, with 0), and the queries take 0 only.
 * Mutation: drop or widen any param check, or check it for some options only. */
static void test_a_param_the_title_never_sends_is_refused(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_NA));
    static const struct {
        uint32_t option;
        uint32_t param;
        bool query;
    } refused[] = {
        {6u, 1u, true},       {0xFu, 1u, true},     {0x10u, 1u, true},
        {0x10u, 0xFFFFFFFFu, true},
        {9u, 2u, false},      {9u, 0xFFFFFFFFu, false},
        {0xBu, 1u, false},    {0xBu, 4u, false},    {0xBu, 6u, false},
        {0xBu, 0xFFFFFFFFu, false},
        {0xEu, 1u, false},    {0xEu, 2u, false},    {0xEu, 0xFFFFFFFFu, false},
    };
    const unsigned count = (unsigned)(sizeof(refused) / sizeof(refused[0]));
    CHECK(count > 0u);
    for (unsigned i = 0u; i < count; i++) {
        kernel_guest_ptr slot = refused[i].query ? result_slot() : 0u;
        if (refused[i].query) {
            CHECK_FAILURE(send_option(refused[i].option, refused[i].param, slot));
            CHECK_EQ_U32(read32(slot), SENTINEL);
        } else {
            CHECK_FAILURE(send_option(refused[i].option, refused[i].param, 0u));
        }
    }
    CHECK_EQ_U32(kernel_av_refused_count(), count);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 0u);
    CHECK_EQ_U32(kernel_av_ignored_option_count(), 0u);
    /* The refusal names the option and the param in decimal, loudly. */
    CHECK(captured_contains("option 0xB, param 4) REFUSED"));
    teardown();
}

/* Mutation: widen the switch so a neighbouring option is accepted. Each refused option is
 * one the title does not send, and several sit directly beside one it does. */
static void test_every_option_the_title_does_not_send_is_refused(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_NA));
    static const uint32_t refused[] = {0u,   1u,   2u,   3u,   4u,   5u,   7u,  8u,
                                       0xAu, 0xCu, 0xDu, 0x11u, 0x12u, 0x100u, 0xFFFFFFFFu};
    unsigned expected_refusals = 0u;
    for (size_t i = 0u; i < sizeof(refused) / sizeof(refused[0]); i++) {
        kernel_guest_ptr slot = result_slot();
        CHECK_FAILURE(send_option(refused[i], 0u, slot));
        CHECK_EQ_U32(read32(slot), SENTINEL);
        /* And with the NULL result the title passes to its set-options, so an option widened
         * into the set-calls is not refused merely for the pointer it was handed. */
        CHECK_FAILURE(send_option(refused[i], 0u, 0u));
        expected_refusals += 2u;
    }
    CHECK_EQ_U32(kernel_av_refused_count(), expected_refusals);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 0u);
    CHECK_EQ_U32(kernel_av_ignored_option_count(), 0u);
    teardown();
}

/* Mutation: fetch fewer arguments. The arity is pinned from BOTH sides so a count of 3 or
 * 5 is caught, and the stack limit is exactly the frame, so a read past it fails. */
static void test_option_call_needs_exactly_four_arguments(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_NA));
    kernel_guest_ptr slot = result_slot();
    const uint32_t args[4] = {0u, 6u, 0u, slot};
    CHECK_EQ_U32(call_av(ORD_AV_SEND_OPTION, args, 4u), STATUS_SUCCESS);
    CHECK_EQ_U32(read32(slot), 0x00480104u);

    slot = result_slot();
    const uint32_t short_args[3] = {0u, 6u, 0u};
    CHECK_FAILURE(call_av(ORD_AV_SEND_OPTION, short_args, 3u));
    CHECK_EQ_U32(read32(slot), SENTINEL);
    CHECK_FAILURE(call_av(ORD_AV_SEND_OPTION, short_args, 0u));
    CHECK_FAILURE(kernel_hle_call(ORD_AV_SEND_OPTION, NULL));
    teardown();
}

/* ------------------------------------------------------------------------- */

/* Mutation: return the stored value from the wrong variable, or never store. */
static void test_the_saved_data_address_is_real_state(void)
{
    setup();
    CHECK_EQ_U32(call_av(ORD_AV_GET_SAVED, NULL, 0u), 0u);
    CHECK_EQ_U32(kernel_av_saved_data_address(), 0u);

    const uint32_t set_nonzero[1] = {0x04F00000u};
    CHECK_EQ_U32(call_av(ORD_AV_SET_SAVED, set_nonzero, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(call_av(ORD_AV_GET_SAVED, NULL, 0u), 0x04F00000u);
    CHECK_EQ_U32(kernel_av_saved_data_address(), 0x04F00000u);

    /* The only call the title ever makes: it clears the address it was handed. */
    const uint32_t clear[1] = {0u};
    CHECK_EQ_U32(call_av(ORD_AV_SET_SAVED, clear, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(call_av(ORD_AV_GET_SAVED, NULL, 0u), 0u);
    CHECK_EQ_U32(kernel_av_saved_data_address(), 0u);

    /* One argument required, from both sides. */
    CHECK_FAILURE(call_av(ORD_AV_SET_SAVED, NULL, 0u));
    CHECK_FAILURE(kernel_hle_call(ORD_AV_SET_SAVED, NULL));

    CHECK_EQ_U32(call_av(ORD_AV_SET_SAVED, set_nonzero, 1u), STATUS_SUCCESS);
    kernel_av_reset();
    CHECK_EQ_U32(kernel_av_saved_data_address(), 0u);
    teardown();
}

/* Mutation: record the arguments in the wrong order. Every slot carries a distinct value
 * so any swap is visible, and the values are the shape the title passes (a 480p mode word
 * from the table, a 640 by 4 byte pitch, a 32-bit format). */
static void test_set_display_mode_records_every_argument_in_order(void)
{
    setup();
    kernel_av_display shown;
    memset(&shown, 0, sizeof(shown));
    CHECK(!kernel_av_display_get(&shown));

    const uint32_t first[6] = {0xFD000000u, 0u, 0x88110F01u, 0x1Eu, 0xA00u, 0x03C00000u};
    CHECK_EQ_U32(call_av(ORD_AV_SET_MODE, first, 6u), 0u);
    CHECK(kernel_av_display_get(&shown));
    CHECK_EQ_U32(shown.register_base, 0xFD000000u);
    CHECK_EQ_U32(shown.mode, 0x88110F01u);
    CHECK_EQ_U32(shown.format, 0x1Eu);
    CHECK_EQ_U32(shown.pitch, 0xA00u);
    CHECK_EQ_U32(shown.frame_buffer, 0x03C00000u);
    CHECK_EQ_U32(shown.calls, 1u);

    /* The title calls it twice for one mode (a 32-bit pre-clear, then the real format), and
     * the LAST call is the scanout surface. */
    const uint32_t second[6] = {0xFD000000u, 0u, 0x88110F01u, 0x11u, 0x500u, 0x03C00000u};
    CHECK_EQ_U32(call_av(ORD_AV_SET_MODE, second, 6u), 0u);
    CHECK(kernel_av_display_get(&shown));
    CHECK_EQ_U32(shown.format, 0x11u);
    CHECK_EQ_U32(shown.pitch, 0x500u);
    CHECK_EQ_U32(shown.calls, 2u);
    CHECK(captured_contains("RECORDED ONLY"));
    teardown();
}

/* Mutation: return a nonzero step. The title loops while the result is nonzero, waiting on
 * a vertical-blank event between steps, so a nonzero return is a hang. */
static void test_set_display_mode_finishes_in_one_step(void)
{
    setup();
    const uint32_t args[6] = {0xFD000000u, 0u, 0x88070701u, 0x1Eu, 0xA00u, 0x03C00000u};
    CHECK_EQ_U32(call_av(ORD_AV_SET_MODE, args, 6u), 0u);
    const uint32_t later_step[6] = {0xFD000000u, 5u, 0x88070701u, 0x1Eu, 0xA00u, 0x03C00000u};
    CHECK_EQ_U32(call_av(ORD_AV_SET_MODE, later_step, 6u), 0u);
    teardown();
}

/* Mutation: fetch five arguments. Pinned from both sides with the stack limit exactly the
 * frame: six must work and five must be refused WITHOUT recording a half-read request. */
static void test_set_display_mode_needs_exactly_six_arguments(void)
{
    setup();
    const uint32_t five[5] = {0xFD000000u, 0u, 0x88070701u, 0x1Eu, 0xA00u};
    CHECK_FAILURE(call_av(ORD_AV_SET_MODE, five, 5u));
    kernel_av_display shown;
    CHECK(!kernel_av_display_get(&shown));
    CHECK_EQ_U32(kernel_av_refused_count(), 1u);
    CHECK_FAILURE(kernel_hle_call(ORD_AV_SET_MODE, NULL));
    teardown();
}

/* ------------------------------------------------------------------------- */

static uint32_t query_setting(uint32_t index, uint32_t *value_out)
{
    kernel_guest_ptr type_slot = region + RESULT_AT + 0x10u;
    kernel_guest_ptr value_slot = region + RESULT_AT + 0x20u;
    write32(type_slot, SENTINEL);
    write32(value_slot, SENTINEL);
    const uint32_t args[5] = {index, type_slot, value_slot, 4u, 0u};
    const uint32_t status = call_av(ORD_EX_QUERY_SETTING, args, 5u);
    *value_out = read32(value_slot);
    return status;
}

/* Mutation: install the settings under swapped indices, or not at all. XGetVideoStandard
 * reads index 0x103 and XGetVideoFlags reads index 8, and the title's three routes to the
 * same claim must agree. */
static void test_the_title_reads_the_same_claim_through_the_config_ordinal(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_NA));
    reset_capture();
    CHECK_EQ_U32(kernel_av_install_settings(), 2u);
    CHECK(captured_contains("FABRICATED"));
    CHECK(captured_contains("0x00080000"));

    uint32_t value = 0u;
    CHECK_EQ_U32(query_setting(0x103u, &value), STATUS_SUCCESS);
    CHECK_EQ_U32(value, 0x100u);
    /* XGetVideoStandard returns byte 1 of the value: 1 is NTSC-M. */
    CHECK_EQ_U32((value >> 8) & 0xFFu, 1u);

    CHECK_EQ_U32(query_setting(8u, &value), STATUS_SUCCESS);
    CHECK_EQ_U32(value, 0x00080000u);
    /* XGetVideoFlags returns (value >> 16) & 0x5F: bit 8 is 480p. */
    CHECK_EQ_U32((value >> 16) & 0x5Fu, 8u);
    teardown();
}

/* Mutation: install settings for an unconfigured module. */
static void test_settings_are_not_installed_before_configure(void)
{
    setup();
    reset_capture();
    CHECK_EQ_U32(kernel_av_install_settings(), 0u);
    CHECK(captured_contains("NOT installed"));
    CHECK_EQ_U32(kernel_config_setting_count(), 0u);
    teardown();
}

/* Mutation: keep 480p for PAL, or lose it for the composite pack the other way round. */
static void test_a_pack_without_480p_installs_no_video_flags(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_COMPOSITE, REGION_NA));
    CHECK_EQ_U32(kernel_av_install_settings(), 2u);
    uint32_t value = SENTINEL;
    CHECK_EQ_U32(query_setting(8u, &value), STATUS_SUCCESS);
    CHECK_EQ_U32(value, 0u);

    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_ROW));
    CHECK_EQ_U32(kernel_av_install_settings(), 2u);
    CHECK_EQ_U32(query_setting(0x103u, &value), STATUS_SUCCESS);
    CHECK_EQ_U32(value, 0x300u);
    CHECK_EQ_U32(query_setting(8u, &value), STATUS_SUCCESS);
    CHECK_EQ_U32(value, 0u);
    teardown();
}

/* Mutation: stop clearing the counters or the state on reset. */
static void test_reset_forgets_everything(void)
{
    setup();
    CHECK(kernel_av_configure(KERNEL_AV_PACK_HDTV, REGION_NA));
    kernel_guest_ptr slot = result_slot();
    CHECK_EQ_U32(send_option(6u, 0u, slot), STATUS_SUCCESS);
    CHECK_FAILURE(send_option(0x7Fu, 0u, slot));
    CHECK_EQ_U32(send_option(9u, 1u, 0u), STATUS_SUCCESS);
    const uint32_t args[6] = {0xFD000000u, 0u, 0x88070701u, 0x1Eu, 0xA00u, 0x03C00000u};
    CHECK_EQ_U32(call_av(ORD_AV_SET_MODE, args, 6u), 0u);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 1u);
    CHECK_EQ_U32(kernel_av_refused_count(), 1u);
    CHECK_EQ_U32(kernel_av_ignored_option_count(), 1u);

    kernel_av_reset();
    CHECK(!kernel_av_is_configured());
    CHECK_EQ_U32(kernel_av_capabilities(), 0u);
    CHECK_EQ_U32(kernel_av_av_region_setting(), 0u);
    CHECK_EQ_U32(kernel_av_video_flags_setting(), 0u);
    CHECK_EQ_U32(kernel_av_smc_video_mode(), 0u);
    CHECK_EQ_U32(kernel_av_fabricated_count(), 0u);
    CHECK_EQ_U32(kernel_av_refused_count(), 0u);
    CHECK_EQ_U32(kernel_av_ignored_option_count(), 0u);
    kernel_av_display shown;
    CHECK(!kernel_av_display_get(&shown));
    teardown();
}

int main(void)
{
    printf("kernel Av HLE tests\n");

    test_all_four_ordinals_are_implemented();
    test_the_pack_name_set_is_exactly_four();
    test_the_default_pack_is_hdtv();

    test_the_answer_for_every_pack_in_every_standard();
    test_region_priority_is_na_then_japan_then_rest_of_world();
    test_a_region_with_no_bit_is_refused_and_changes_nothing();
    test_configuring_announces_the_fabrication();
    test_the_region_is_read_from_the_xbe_certificate();
    test_an_unreadable_or_foreign_header_is_refused();

    test_option_6_before_configure_is_refused_and_writes_nothing();
    test_option_6_writes_the_capability_word_through_the_result_pointer();
    test_a_query_with_no_result_pointer_is_refused();
    test_field_and_encoder_queries_write_their_answers();
    test_every_measured_call_site_is_accepted();
    test_a_set_option_with_a_result_pointer_is_refused();
    test_a_param_the_title_never_sends_is_refused();
    test_every_option_the_title_does_not_send_is_refused();
    test_option_call_needs_exactly_four_arguments();

    test_the_saved_data_address_is_real_state();
    test_set_display_mode_records_every_argument_in_order();
    test_set_display_mode_finishes_in_one_step();
    test_set_display_mode_needs_exactly_six_arguments();

    test_the_title_reads_the_same_claim_through_the_config_ordinal();
    test_settings_are_not_installed_before_configure();
    test_a_pack_without_480p_installs_no_video_flags();
    test_reset_forgets_everything();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
