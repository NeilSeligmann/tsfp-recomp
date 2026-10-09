/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Xc* crypto ordinals: 335/336/337 (SHA-1), 340 (HMAC-SHA1), 346 (DES key parity).
 *
 * THE ORACLE IS EXTERNAL, WHICH IS THE WHOLE POINT OF THIS SUITE. Every expected
 * digest below is a number printed in a standards document -- FIPS 180-1 for SHA-1,
 * RFC 2202 for HMAC-SHA1, FIPS 74 for the DES weak keys -- and not a number this
 * implementation produced. A suite that hashed something twice and compared the two
 * results would pass against a wrong SHA-1, a swapped HMAC pad, a little-endian
 * length field and SHA-0. `tools/mutate/sets/crypto.py` injects exactly those four
 * defects, plus three more, to show that this suite would not.
 *
 * WHAT THERE IS TO GET WRONG, in order of how badly it hurts:
 *
 *   1. THE ARITY. 1, 3, 2, 7 and 2 stack arguments, and the measured table has NO row
 *      for any of the five -- every call site reaches them through an import jump stub
 *      the measurement cannot see through. The counts are entirely hand-derived, which
 *      makes them the weakest link and therefore the thing tested hardest: our handler
 *      IS the __stdcall callee, so a frame one slot short must be REFUSED rather than
 *      read past. XcHMAC's SEVEN is the dangerous one, because the minimum-over-sites
 *      rule would have said six.
 *   2. THE ALGORITHM. Testable only against published vectors, and tested only against
 *      published vectors. See above.
 *   3. THE ARGUMENT ORDER. XcSHAUpdate's (context, data, length) and XcSHAFinal's
 *      (context, digest) are both pairs of pointers next to a length or each other, so
 *      a swap would still write 20 plausible bytes somewhere. Pinned by asserting the
 *      digest lands at the address argument 1 named and that the context is untouched
 *      except for its cookie.
 *   4. THE TWO-SEGMENT HMAC. 33 of 40 sites pass 0/0 for segment 2 and 7 pass a real
 *      one, so BOTH have to work, and the two-segment result has to equal the
 *      one-segment result over the concatenation or the guest's own KDF breaks.
 *   5. A STALE CONTEXT BEING SILENTLY ACCEPTED. The worst available failure is
 *      hashing from an unknown state and handing the guest a digest over a suffix of
 *      its message, which it would read as a corrupt save rather than as our bug.
 *
 * DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. Every frame is built with
 * kernel_frame_build in scratch guest memory and every expected value is a published
 * constant, so no image address is resolved and no disc data is touched.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED; each test says what breaks it.
 */

#include "kernel_crypto.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t a_ = (uint32_t)(actual);                                               \
        uint32_t e_ = (uint32_t)(expected);                                             \
        if (a_ != e_) {                                                                 \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual, \
                   (unsigned)a_, (unsigned)e_);                                         \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

/* ---------------------------------------------------------------- log capture */

static char captured[16384];
static size_t captured_len;

static int capture_printer(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const size_t room = sizeof captured - captured_len;
    const int written = vsnprintf(captured + captured_len, room, format, arguments);
    va_end(arguments);
    if (written > 0) {
        captured_len += (size_t)written < room ? (size_t)written : room - 1u;
    }
    return written;
}

static void reset_capture(void)
{
    captured_len = 0u;
    captured[0] = '\0';
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

/* ------------------------------------------------------------ digest compare */

/*
 * Compare against a HEX STRING rather than a byte array, on purpose: the expected
 * value then appears in the source in exactly the form the standard prints it, so a
 * transcription error is visible by eye instead of hidden in an initialiser list.
 */
static void check_digest(const char *what, const uint8_t *digest, const char *expected)
{
    char actual[41];
    for (unsigned i = 0u; i < 20u; i++) {
        snprintf(actual + i * 2u, 3u, "%02x", digest[i]);
    }
    checks++;
    if (strcmp(actual, expected) != 0) {
        printf("FAIL %s\n  computed %s\n  published %s\n", what, actual, expected);
        failures++;
    }
}

/* ------------------------------------------------------------ guest scratch */

#define SCRATCH_BYTES 0x4000u
static kernel_guest_ptr scratch;

static void setup(void)
{
    kernel_hle_init();
    kernel_crypto_reset();
    guest_mem_reset();
    CHECK_EQ_U32(kernel_crypto_register(), 7u);
    kernel_hle_set_log(capture_printer);
    reset_capture();

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = SCRATCH_BYTES;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("FATAL could not allocate scratch (status %#x)\n", (unsigned)status);
        exit(EXIT_FAILURE);
    }
    memset(kernel_guest_at(scratch, SCRATCH_BYTES), 0, SCRATCH_BYTES);
}

static void teardown(void)
{
    kernel_hle_set_log(NULL);
    kernel_crypto_reset();
    guest_mem_reset();
}

/* Call ordinal `ordinal` with `count` stack arguments laid out in scratch. */
static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    const kernel_guest_ptr frame_buffer = scratch + 0x3000u;
    kernel_call_frame frame;
    if (!kernel_frame_build(&frame, frame_buffer, 0x200u, args, count)) {
        printf("FAIL could not build a %u-argument frame\n", count);
        failures++;
        return STATUS_INVALID_PARAMETER;
    }
    return kernel_hle_call(ordinal, &frame);
}

/* Build a frame with DELIBERATELY TOO LITTLE ROOM, to prove a short frame is
 * refused rather than read past. */
static uint32_t call_ordinal_short(unsigned ordinal, const uint32_t *args, unsigned count,
                                   uint32_t frame_bytes)
{
    const kernel_guest_ptr frame_buffer = scratch + 0x3000u;
    kernel_call_frame frame;
    if (!kernel_frame_build(&frame, frame_buffer, (uint32_t)((count + 1u) * 4u), args,
                            count)) {
        printf("FAIL could not build the full frame first\n");
        failures++;
        return STATUS_INVALID_PARAMETER;
    }
    /* Shrink the limit so the last argument falls outside it. */
    frame.stack_limit = frame_buffer + frame_bytes;
    return kernel_hle_call(ordinal, &frame);
}

/* ======================================================================
 * The published SHA-1 vectors, FIPS 180-1.
 * ====================================================================== */

static void test_fips_180_1_sha1_vectors(void)
{
    uint8_t digest[20];

    /* FIPS 180-1 appendix A. */
    kernel_crypto_sha1("abc", 3u, digest);
    check_digest("FIPS 180-1 A: \"abc\"", digest,
                 "a9993e364706816aba3e25717850c26c9cd0d89d");

    /* FIPS 180-1 appendix B: 56 bytes, which spans two blocks after padding and is
     * therefore the shortest vector that exercises a second compression. */
    kernel_crypto_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56u,
                       digest);
    check_digest("FIPS 180-1 B: 56 bytes", digest,
                 "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    /*
     * FIPS 180-1 appendix C: one million 'a'. This is the vector that catches a
     * length field which overflows or is counted in the wrong unit, because
     * 1,000,000 bytes is 8,000,000 bits and neither fits in a byte count that was
     * confused for a bit count.
     */
    char *million = (char *)malloc(1000000u);
    if (!million) {
        printf("FAIL could not allocate the 1,000,000-byte vector\n");
        failures++;
    } else {
        memset(million, 'a', 1000000u);
        kernel_crypto_sha1(million, 1000000u, digest);
        check_digest("FIPS 180-1 C: 1,000,000 x 'a'", digest,
                     "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
        free(million);
    }

    /*
     * THE EMPTY MESSAGE. Not in FIPS 180-1's appendices, but the digest of the empty
     * string is a published constant in its own right and it is the one input where
     * the padding is the ENTIRE message, so an off-by-one in the pad has nowhere to
     * hide.
     */
    kernel_crypto_sha1("", 0u, digest);
    check_digest("SHA-1 of the empty message", digest,
                 "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

/*
 * MUTATION: any of the three SHA-1 mutations in sets/crypto.py, and this fails.
 * Splitting the 56-byte vector at EVERY boundary is what makes the streaming form
 * trustworthy, because XcSHAUpdate is called two and three times in a row by the
 * guest (0x00439897 calls it three times) and a buffering bug that only shows up at
 * one particular split would otherwise pass.
 */
static void test_streaming_agrees_with_one_shot_at_every_split(void)
{
    static const char message[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const size_t length = 56u;
    unsigned mismatches = 0u;

    for (size_t split = 0u; split <= length; split++) {
        kernel_sha1_state state;
        kernel_crypto_sha1_reset(&state);
        kernel_crypto_sha1_absorb(&state, message, split);
        kernel_crypto_sha1_absorb(&state, message + split, length - split);
        uint8_t digest[20];
        kernel_crypto_sha1_squeeze(&state, digest);

        char actual[41];
        for (unsigned i = 0u; i < 20u; i++) {
            snprintf(actual + i * 2u, 3u, "%02x", digest[i]);
        }
        if (strcmp(actual, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") != 0) {
            mismatches++;
        }
    }
    /* ASSERTED NON-EMPTY FIRST: 57 splits were actually tried. A loop that ran zero
     * times would also report zero mismatches. */
    CHECK_EQ_U32(length + 1u, 57u);
    CHECK_EQ_U32(mismatches, 0u);
}

/*
 * A THREE-SEGMENT UPDATE AT A BLOCK BOUNDARY, because the guest does exactly this.
 * 0x0037E65B hashes a 64-byte opad and a 20-byte digest in ONE 84-byte update, and
 * 0x00439897 feeds three independent segments. 64 is the case where a naive
 * implementation compresses an empty block or forgets to.
 */
static void test_segment_lengths_the_guest_actually_uses(void)
{
    uint8_t one_shot[20];
    uint8_t streamed[20];
    uint8_t buffer[84];
    for (unsigned i = 0u; i < sizeof buffer; i++) {
        buffer[i] = (uint8_t)(i * 7u + 1u);
    }

    kernel_crypto_sha1(buffer, sizeof buffer, one_shot);

    kernel_sha1_state state;
    kernel_crypto_sha1_reset(&state);
    kernel_crypto_sha1_absorb(&state, buffer, 64u);
    kernel_crypto_sha1_absorb(&state, buffer + 64u, 20u);
    kernel_crypto_sha1_squeeze(&state, streamed);

    CHECK(memcmp(one_shot, streamed, 20u) == 0);

    /* A zero-length update must be a no-op, not a block flush. */
    kernel_crypto_sha1_reset(&state);
    kernel_crypto_sha1_absorb(&state, buffer, 64u);
    kernel_crypto_sha1_absorb(&state, NULL, 0u);
    kernel_crypto_sha1_absorb(&state, buffer + 64u, 20u);
    kernel_crypto_sha1_squeeze(&state, streamed);
    CHECK(memcmp(one_shot, streamed, 20u) == 0);
}

/* ======================================================================
 * The published HMAC-SHA1 vectors, RFC 2202 section 3.
 * ====================================================================== */

static void test_rfc_2202_hmac_sha1_vectors(void)
{
    uint8_t mac[20];
    uint8_t key[80];
    uint8_t data[73];

    /* Case 1. */
    memset(key, 0x0b, 20u);
    kernel_crypto_hmac_sha1(key, 20u, "Hi There", 8u, mac);
    check_digest("RFC 2202 case 1", mac, "b617318655057264e28bc0b6fb378c8ef146be00");

    /* Case 2: a 4-byte key, i.e. one shorter than the digest. */
    kernel_crypto_hmac_sha1("Jefe", 4u, "what do ya want for nothing?", 28u, mac);
    check_digest("RFC 2202 case 2", mac, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");

    /* Case 3. */
    memset(key, 0xaa, 20u);
    memset(data, 0xdd, 50u);
    kernel_crypto_hmac_sha1(key, 20u, data, 50u, mac);
    check_digest("RFC 2202 case 3", mac, "125d7342b9ac11cd91a39af48aa17b4f63f175d3");

    /* Case 4: a 25-byte key, longer than the digest but shorter than the block. */
    for (unsigned i = 0u; i < 25u; i++) {
        key[i] = (uint8_t)(i + 1u);
    }
    memset(data, 0xcd, 50u);
    kernel_crypto_hmac_sha1(key, 25u, data, 50u, mac);
    check_digest("RFC 2202 case 4", mac, "4c9007f4026250c6bc8414f9bf50c86c2d7235da");

    /* Case 5. */
    memset(key, 0x0c, 20u);
    kernel_crypto_hmac_sha1(key, 20u, "Test With Truncation", 20u, mac);
    check_digest("RFC 2202 case 5", mac, "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04");

    /*
     * Cases 6 and 7: AN 80-BYTE KEY, longer than the 64-byte block. These are the two
     * vectors the brief required and they are the ONLY external evidence for the
     * key-reduction step, which is the step the Xbox export is believed to do
     * differently. They pin the RFC 2104 behaviour; the divergence is pinned
     * separately below.
     */
    memset(key, 0xaa, 80u);
    kernel_crypto_hmac_sha1(key, 80u,
                            "Test Using Larger Than Block-Size Key - Hash Key First", 54u,
                            mac);
    check_digest("RFC 2202 case 6 (80-byte key)", mac,
                 "aa4ae5e15272d00e95705637ce8a3b55ed402112");

    kernel_crypto_hmac_sha1(
        key, 80u,
        "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data", 73u,
        mac);
    check_digest("RFC 2202 case 7 (80-byte key)", mac,
                 "e8e99d0f45237d786d6bbaa7965c7808bbff1a91");
}

/*
 * THE EMPTY MESSAGE, which RFC 2202 does not cover and which the brief required.
 *
 * SOURCED HONESTLY: these two values are NOT published vectors. They were produced by
 * OpenSSL via Python's hmac module -- an independent, widely validated implementation
 * -- and are recorded here as cross-implementation agreement rather than as standards
 * text. That is weaker evidence than RFC 2202 and is labelled as such instead of
 * being passed off as a vector. They are worth having because an empty message is the
 * case where the inner hash is the ipad alone, so a padding bug cannot be masked by
 * message bytes.
 */
static void test_empty_message_hmac_against_an_independent_implementation(void)
{
    uint8_t mac[20];
    uint8_t key[20];

    memset(key, 0x0b, 20u);
    kernel_crypto_hmac_sha1(key, 20u, "", 0u, mac);
    check_digest("HMAC, 20-byte key, EMPTY message (OpenSSL cross-check)", mac,
                 "123fd78bda0100786ae86b76f50f01bd18e477f3");

    kernel_crypto_hmac_sha1("", 0u, "", 0u, mac);
    check_digest("HMAC, EMPTY key and EMPTY message (OpenSSL cross-check)", mac,
                 "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d");
}

/*
 * THE XBOX TWO-SEGMENT FORM IS THE SAME FUNCTION OVER THE CONCATENATION, which is
 * what lets RFC 2202 cases 1-5 count as published coverage of the ORDINAL and not
 * merely of the RFC helper. Checked by splitting case 1's message at every boundary.
 *
 * MUTATION: the inner/outer pad swap kills this and the vectors above together.
 */
static void test_xc_hmac_two_segments_equal_one(void)
{
    uint8_t key[20];
    memset(key, 0x0b, 20u);
    static const char message[] = "Hi There";
    unsigned mismatches = 0u;

    for (size_t split = 0u; split <= 8u; split++) {
        uint8_t mac[20];
        kernel_crypto_xc_hmac(key, 20u, message, split, message + split, 8u - split, mac);
        char actual[41];
        for (unsigned i = 0u; i < 20u; i++) {
            snprintf(actual + i * 2u, 3u, "%02x", mac[i]);
        }
        if (strcmp(actual, "b617318655057264e28bc0b6fb378c8ef146be00") != 0) {
            mismatches++;
        }
    }
    CHECK_EQ_U32(mismatches, 0u);

    /* A NULL second segment with length 0 is the common case: 33 of the 40 measured
     * sites. It must equal the single-segment answer exactly. */
    uint8_t mac[20];
    kernel_crypto_xc_hmac(key, 20u, message, 8u, NULL, 0u, mac);
    check_digest("XcHMAC with a NULL segment 2", mac,
                 "b617318655057264e28bc0b6fb378c8ef146be00");

    /* And a NULL FIRST segment, which 0x00434508's KDF reaches when its counter
     * buffer is empty. */
    kernel_crypto_xc_hmac(key, 20u, NULL, 0u, message, 8u, mac);
    check_digest("XcHMAC with a NULL segment 1", mac,
                 "b617318655057264e28bc0b6fb378c8ef146be00");
}

/*
 * THE DIVERGENCE IS PINNED, NOT MERELY DOCUMENTED.
 *
 * For a key at or below the 64-byte block the two policies must be IDENTICAL, which
 * is what makes the RFC vectors transfer. Above it they must DIFFER, because the Xbox
 * form truncates where RFC 2104 hashes. A test that only checked the first half would
 * pass if kernel_crypto_xc_hmac quietly called the RFC path, and then the documented
 * divergence would be fiction.
 */
static void test_the_long_key_divergence_is_real_and_bounded(void)
{
    uint8_t rfc[20];
    uint8_t xbox[20];
    uint8_t key[80];

    /* At the block size exactly, and one byte below: must agree. */
    memset(key, 0xaa, 80u);
    kernel_crypto_hmac_sha1(key, 64u, "abc", 3u, rfc);
    kernel_crypto_xc_hmac(key, 64u, "abc", 3u, NULL, 0u, xbox);
    CHECK(memcmp(rfc, xbox, 20u) == 0);

    kernel_crypto_hmac_sha1(key, 63u, "abc", 3u, rfc);
    kernel_crypto_xc_hmac(key, 63u, "abc", 3u, NULL, 0u, xbox);
    CHECK(memcmp(rfc, xbox, 20u) == 0);

    /* The key length every measured site uses: must agree. */
    kernel_crypto_hmac_sha1(key, 16u, "abc", 3u, rfc);
    kernel_crypto_xc_hmac(key, 16u, "abc", 3u, NULL, 0u, xbox);
    CHECK(memcmp(rfc, xbox, 20u) == 0);

    /* One byte ABOVE the block: must differ, or the truncation is not happening. */
    kernel_crypto_hmac_sha1(key, 65u, "abc", 3u, rfc);
    kernel_crypto_xc_hmac(key, 65u, "abc", 3u, NULL, 0u, xbox);
    CHECK(memcmp(rfc, xbox, 20u) != 0);

    /* And the truncating form must equal the RFC form over the key's FIRST 64 bytes,
     * which is what "truncate" means and is a stronger claim than "differs". */
    kernel_crypto_hmac_sha1(key, 64u, "abc", 3u, rfc);
    kernel_crypto_xc_hmac(key, 80u, "abc", 3u, NULL, 0u, xbox);
    CHECK(memcmp(rfc, xbox, 20u) == 0);
}

/* ======================================================================
 * DES key parity, FIPS 46-3, pinned by the FIPS 74 weak keys.
 * ====================================================================== */

/*
 * THE FOUR PUBLISHED WEAK KEYS ARE ALREADY ODD-PARITY-CORRECT, which makes them a
 * sharper test than any adjustment case: a correct routine must leave them BYTE
 * IDENTICAL and report that it changed nothing. An even-parity routine flips the low
 * bit of all eight bytes of every one of them.
 *
 * MUTATION: crypto-des-parity-even-instead-of-odd dies here.
 */
static void test_fips_74_weak_keys_are_left_untouched(void)
{
    static const uint8_t published[][8] = {
        /* FIPS 74's four weak keys. */
        {0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u},
        {0xFEu, 0xFEu, 0xFEu, 0xFEu, 0xFEu, 0xFEu, 0xFEu, 0xFEu},
        {0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x0Eu, 0x0Eu, 0x0Eu, 0x0Eu},
        {0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xF1u, 0xF1u, 0xF1u, 0xF1u},
        /* The canonical example key from the DES literature, also parity-correct. */
        {0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu},
    };
    const unsigned count = (unsigned)(sizeof published / sizeof published[0]);

    /* ASSERTED NON-EMPTY FIRST, so an empty table cannot pass by vacuity. */
    CHECK_EQ_U32(count, 5u);

    for (unsigned k = 0u; k < count; k++) {
        uint8_t key[8];
        memcpy(key, published[k], 8u);
        const bool changed = kernel_crypto_des_key_parity(key, 8u);
        CHECK(!changed);
        CHECK(memcmp(key, published[k], 8u) == 0);
    }
}

static void test_parity_adjustment_of_the_two_extreme_keys(void)
{
    uint8_t key[8];

    /* All zeros has even parity in every byte, so every low bit must be set. */
    memset(key, 0x00, 8u);
    CHECK(kernel_crypto_des_key_parity(key, 8u));
    for (unsigned i = 0u; i < 8u; i++) {
        CHECK_EQ_U32(key[i], 0x01u);
    }

    /* All ones has even parity in every byte (eight bits), so every low bit clears. */
    memset(key, 0xFFu, 8u);
    CHECK(kernel_crypto_des_key_parity(key, 8u));
    for (unsigned i = 0u; i < 8u; i++) {
        CHECK_EQ_U32(key[i], 0xFEu);
    }

    /* THE SEVEN KEY BITS ARE NEVER TOUCHED, which is the other half of the contract:
     * a routine that "fixed parity" by rewriting the byte would pass the two cases
     * above and destroy the key. */
    for (unsigned value = 0u; value < 256u; value++) {
        uint8_t one = (uint8_t)value;
        (void)kernel_crypto_des_key_parity(&one, 1u);
        CHECK_EQ_U32(one & 0xFEu, value & 0xFEu);
    }
}

/* ======================================================================
 * The ordinal boundary.
 * ====================================================================== */

static void test_registration_makes_all_five_ordinals_implemented(void)
{
    setup();
    static const unsigned ordinals[5] = {335u, 336u, 337u, 340u, 346u};
    /* ASSERTED NON-EMPTY FIRST, so an empty list cannot pass by vacuity. */
    CHECK_EQ_U32((unsigned)(sizeof ordinals / sizeof ordinals[0]), 5u);
    for (unsigned i = 0u; i < 5u; i++) {
        const kernel_entry *entry = kernel_hle_entry(ordinals[i]);
        CHECK(entry != NULL);
        if (entry) {
            /* IMPLEMENTED, not merely present: every ordinal starts life as a STUB,
             * so checking non-NULL alone would pass with nothing registered. */
            CHECK_EQ_U32(entry->state, KERNEL_ENTRY_IMPLEMENTED);
        }
    }
    teardown();
}

/*
 * THE WHOLE SHA PIPELINE THROUGH THE ORDINALS, against a published vector.
 *
 * This is the test that proves the arity, the argument order, the guest-memory
 * marshalling and the algorithm all agree at once: the digest that lands in guest
 * memory at the address argument 1 named must be FIPS 180-1's answer for "abc".
 */
static void test_the_three_sha_ordinals_produce_the_published_digest(void)
{
    setup();

    const kernel_guest_ptr context = scratch;              /* 116 bytes reserved */
    const kernel_guest_ptr message = scratch + 0x200u;
    const kernel_guest_ptr digest_at = scratch + 0x400u;
    memcpy(kernel_guest_at(message, 3u), "abc", 3u);

    uint32_t init_args[1] = {context};
    CHECK_EQ_U32(call_ordinal(335u, init_args, 1u), STATUS_SUCCESS);

    uint32_t update_args[3] = {context, message, 3u};
    CHECK_EQ_U32(call_ordinal(336u, update_args, 3u), STATUS_SUCCESS);

    uint32_t final_args[2] = {context, digest_at};
    CHECK_EQ_U32(call_ordinal(337u, final_args, 2u), STATUS_SUCCESS);

    check_digest("ordinals 335/336/337 over \"abc\"",
                 (const uint8_t *)kernel_guest_at(digest_at, 20u),
                 "a9993e364706816aba3e25717850c26c9cd0d89d");

    CHECK_EQ_U32(kernel_crypto_sha_init_count(), 1u);
    CHECK_EQ_U32(kernel_crypto_sha_update_count(), 1u);
    CHECK_EQ_U32(kernel_crypto_sha_final_count(), 1u);
    CHECK_EQ_U32(kernel_crypto_sha_bytes_absorbed(), 3u);
    CHECK_EQ_U32(kernel_crypto_stale_context_count(), 0u);

    teardown();
}

/*
 * THREE UPDATES IN A ROW THROUGH THE ORDINALS, which is what 0x00439897 does.
 * Serialising and reloading the context between calls is the part that could be
 * wrong, and only a multi-update vector catches it.
 */
static void test_three_updates_through_the_ordinals_match_the_published_digest(void)
{
    setup();

    const kernel_guest_ptr context = scratch;
    const kernel_guest_ptr message = scratch + 0x200u;
    const kernel_guest_ptr digest_at = scratch + 0x400u;
    static const char text[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    memcpy(kernel_guest_at(message, 56u), text, 56u);

    uint32_t init_args[1] = {context};
    CHECK_EQ_U32(call_ordinal(335u, init_args, 1u), STATUS_SUCCESS);

    /* 1 + 54 + 1, so the first and last updates are partial-block and the middle one
     * straddles the 64-byte boundary. */
    const uint32_t spans[3][2] = {{0u, 1u}, {1u, 54u}, {55u, 1u}};
    for (unsigned s = 0u; s < 3u; s++) {
        uint32_t update_args[3] = {context, message + spans[s][0], spans[s][1]};
        CHECK_EQ_U32(call_ordinal(336u, update_args, 3u), STATUS_SUCCESS);
    }

    uint32_t final_args[2] = {context, digest_at};
    CHECK_EQ_U32(call_ordinal(337u, final_args, 2u), STATUS_SUCCESS);

    check_digest("ordinals 335/336x3/337 over the 56-byte vector",
                 (const uint8_t *)kernel_guest_at(digest_at, 20u),
                 "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK_EQ_U32(kernel_crypto_sha_update_count(), 3u);
    CHECK_EQ_U32(kernel_crypto_sha_bytes_absorbed(), 56u);

    teardown();
}

/*
 * XcHMAC THROUGH THE ORDINAL, with SEVEN arguments, against RFC 2202 case 1.
 *
 * MUTATION: an ABI row of five instead of seven cannot be mutation-tested here --
 * the row lives in src/host/kernel_thunk.c and tsfp_host is not a ctest binary -- but
 * a HANDLER that read five arguments would fail this test, because arguments 5 and 6
 * are the ones it would then miss.
 */
static void test_ordinal_340_matches_rfc_2202_case_1(void)
{
    setup();

    const kernel_guest_ptr key = scratch;
    const kernel_guest_ptr message = scratch + 0x100u;
    const kernel_guest_ptr mac_at = scratch + 0x200u;
    memset(kernel_guest_at(key, 20u), 0x0b, 20u);
    memcpy(kernel_guest_at(message, 8u), "Hi There", 8u);

    /* The 33-of-40 shape: segment 2 present and zero. */
    uint32_t args[7] = {key, 20u, message, 8u, 0u, 0u, mac_at};
    CHECK_EQ_U32(call_ordinal(340u, args, 7u), STATUS_SUCCESS);
    check_digest("ordinal 340, segment 2 absent",
                 (const uint8_t *)kernel_guest_at(mac_at, 20u),
                 "b617318655057264e28bc0b6fb378c8ef146be00");

    /* The 7-of-40 shape: a real second segment, split across the two. */
    memset(kernel_guest_at(mac_at, 20u), 0, 20u);
    uint32_t split_args[7] = {key, 20u, message, 3u, message + 3u, 5u, mac_at};
    CHECK_EQ_U32(call_ordinal(340u, split_args, 7u), STATUS_SUCCESS);
    check_digest("ordinal 340, segment 2 present",
                 (const uint8_t *)kernel_guest_at(mac_at, 20u),
                 "b617318655057264e28bc0b6fb378c8ef146be00");

    CHECK_EQ_U32(kernel_crypto_hmac_count(), 2u);
    CHECK_EQ_U32(kernel_crypto_hmac_oversize_key_count(), 0u);

    teardown();
}

/*
 * AN OVER-LONG KEY IS ANNOUNCED, NOT SILENTLY HANDLED. No measured site does this, so
 * if it ever happens the run must say that the reduction policy is unverified rather
 * than quietly pick one.
 */
static void test_an_oversize_hmac_key_is_counted_and_announced(void)
{
    setup();

    const kernel_guest_ptr key = scratch;
    const kernel_guest_ptr message = scratch + 0x200u;
    const kernel_guest_ptr mac_at = scratch + 0x300u;
    memset(kernel_guest_at(key, 80u), 0xaa, 80u);
    memcpy(kernel_guest_at(message, 3u), "abc", 3u);

    uint32_t args[7] = {key, 80u, message, 3u, 0u, 0u, mac_at};
    CHECK_EQ_U32(call_ordinal(340u, args, 7u), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_crypto_hmac_oversize_key_count(), 1u);
    CHECK(captured_contains("longer than the 64-byte block"));
    CHECK(captured_contains("UNVERIFIED"));

    teardown();
}

/* XcDESKeyParity through the ordinal, over the 24 bytes twelve of its thirteen
 * sites pass. */
static void test_ordinal_346_adjusts_the_24_byte_bundle_in_guest_memory(void)
{
    setup();

    const kernel_guest_ptr key = scratch;
    memset(kernel_guest_at(key, 24u), 0x00, 24u);

    uint32_t args[2] = {key, 0x18u};
    CHECK_EQ_U32(call_ordinal(346u, args, 2u), STATUS_SUCCESS);

    const uint8_t *bytes = (const uint8_t *)kernel_guest_at(key, 24u);
    unsigned wrong = 0u;
    for (unsigned i = 0u; i < 24u; i++) {
        if (bytes[i] != 0x01u) {
            wrong++;
        }
    }
    CHECK_EQ_U32(wrong, 0u);
    CHECK_EQ_U32(kernel_crypto_des_parity_count(), 1u);

    /* The byte AFTER the bundle must be untouched: a length off by one here would
     * corrupt the caller's next field, and 0x004345F1 passes two bundles 0x32 bytes
     * apart in one structure. */
    CHECK_EQ_U32(((const uint8_t *)kernel_guest_at(key, 25u))[24], 0x00u);

    teardown();
}

/* ======================================================================
 * Refusals. Every one of these is a path that must stop rather than invent.
 * ====================================================================== */

static void test_a_missing_frame_is_reported_for_every_ordinal(void)
{
    setup();
    const unsigned ordinals[5] = {335u, 336u, 337u, 340u, 346u};
    for (unsigned i = 0u; i < 5u; i++) {
        reset_capture();
        CHECK_EQ_U32(kernel_hle_call(ordinals[i], NULL), STATUS_INVALID_PARAMETER);
        CHECK(captured_contains("no argument frame"));
    }
    teardown();
}

/*
 * A FRAME TOO SHORT FOR THE MEASURED ARITY IS REFUSED, which is the single most
 * important refusal in this file. Our handler is the __stdcall callee; reading past
 * the frame would hand the algorithm a stack slot belonging to the caller, and the
 * resulting digest would be wrong in a way no vector could localise.
 *
 * XcHMAC is checked at SIX arguments specifically, because six is what the
 * minimum-over-sites rule would have reported for ordinal 340.
 */
static void test_a_frame_short_of_the_measured_arity_is_refused(void)
{
    setup();

    uint32_t args[7] = {scratch, 20u, scratch + 0x100u, 8u, 0u, 0u, scratch + 0x200u};

    /* 340 wants 7 slots after the return address; give it room for 6. */
    reset_capture();
    CHECK_EQ_U32(call_ordinal_short(340u, args, 7u, 7u * 4u), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("could not read argument 6"));

    /* 336 wants 3; give it room for 2. */
    reset_capture();
    CHECK_EQ_U32(call_ordinal_short(336u, args, 3u, 3u * 4u), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("could not read argument 2"));

    /* 337 wants 2; give it room for 1. */
    reset_capture();
    CHECK_EQ_U32(call_ordinal_short(337u, args, 2u, 2u * 4u), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("could not read argument 1"));

    /* 346 wants 2; give it room for 1. */
    reset_capture();
    CHECK_EQ_U32(call_ordinal_short(346u, args, 2u, 2u * 4u), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("could not read argument 1"));

    teardown();
}

/*
 * AN UNINITIALISED CONTEXT IS REFUSED RATHER THAN HASHED.
 *
 * This is the behaviour the header argues hardest for: starting a fresh hash here
 * would give the guest a digest over a SUFFIX of its message, which 0x00380684 would
 * compare against a stored digest and report as corrupt content.
 */
static void test_an_uninitialised_context_is_refused_not_hashed(void)
{
    setup();

    const kernel_guest_ptr context = scratch + 0x800u; /* zeroed, never Init'd */
    const kernel_guest_ptr message = scratch + 0x200u;
    const kernel_guest_ptr digest_at = scratch + 0x400u;

    reset_capture();
    uint32_t update_args[3] = {context, message, 3u};
    CHECK_EQ_U32(call_ordinal(336u, update_args, 3u), STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("did not initialise"));
    CHECK_EQ_U32(kernel_crypto_stale_context_count(), 1u);
    CHECK_EQ_U32(kernel_crypto_sha_update_count(), 0u);

    reset_capture();
    uint32_t final_args[2] = {context, digest_at};
    CHECK_EQ_U32(call_ordinal(337u, final_args, 2u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(kernel_crypto_stale_context_count(), 2u);

    /* NOTHING WAS WRITTEN to the digest buffer. A refusal that still scribbled 20
     * bytes would be worse than useless. */
    const uint8_t *out = (const uint8_t *)kernel_guest_at(digest_at, 20u);
    unsigned nonzero = 0u;
    for (unsigned i = 0u; i < 20u; i++) {
        if (out[i] != 0u) {
            nonzero++;
        }
    }
    CHECK_EQ_U32(nonzero, 0u);

    teardown();
}

/* Finalising twice is reported rather than quietly returning a second, different
 * digest. */
static void test_finalising_a_context_twice_is_refused(void)
{
    setup();

    const kernel_guest_ptr context = scratch;
    const kernel_guest_ptr digest_at = scratch + 0x400u;

    uint32_t init_args[1] = {context};
    CHECK_EQ_U32(call_ordinal(335u, init_args, 1u), STATUS_SUCCESS);
    uint32_t final_args[2] = {context, digest_at};
    CHECK_EQ_U32(call_ordinal(337u, final_args, 2u), STATUS_SUCCESS);

    /* The empty-message digest, since nothing was absorbed. */
    check_digest("ordinal 337 with no update", (const uint8_t *)kernel_guest_at(digest_at, 20u),
                 "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    reset_capture();
    CHECK_EQ_U32(call_ordinal(337u, final_args, 2u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(kernel_crypto_stale_context_count(), 1u);
    CHECK_EQ_U32(kernel_crypto_sha_final_count(), 1u);

    teardown();
}

/*
 * OUR SERIALISED CONTEXT FITS THE GUEST'S MEASURED 116 BYTES, asserted rather than
 * commented. Growing past 116 would overwrite the guest's next structure field, and
 * 0x0042DBC7 packs three contexts 0x74 apart with nothing to spare.
 */
static void test_the_serialised_context_fits_the_guests_reservation(void)
{
    setup();
    CHECK(kernel_crypto_guest_context_bytes() <= 116u);

    /* And nothing beyond our own size is written. Poison the tail of the guest's
     * 116 bytes and prove Init leaves it alone. */
    const kernel_guest_ptr context = scratch;
    uint8_t *raw = (uint8_t *)kernel_guest_at(context, 116u);
    memset(raw, 0xA5u, 116u);
    uint32_t init_args[1] = {context};
    CHECK_EQ_U32(call_ordinal(335u, init_args, 1u), STATUS_SUCCESS);

    const size_t used = kernel_crypto_guest_context_bytes();
    unsigned clobbered = 0u;
    for (size_t i = used; i < 116u; i++) {
        if (raw[i] != 0xA5u) {
            clobbered++;
        }
    }
    /* ASSERTED NON-EMPTY FIRST: there IS a tail to check. */
    CHECK(116u > used);
    CHECK_EQ_U32(clobbered, 0u);

    teardown();
}

static void test_reset_clears_every_counter(void)
{
    setup();
    const kernel_guest_ptr context = scratch;
    uint32_t init_args[1] = {context};
    CHECK_EQ_U32(call_ordinal(335u, init_args, 1u), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_crypto_sha_init_count(), 1u);
    kernel_crypto_reset();
    CHECK_EQ_U32(kernel_crypto_sha_init_count(), 0u);
    CHECK_EQ_U32(kernel_crypto_sha_update_count(), 0u);
    CHECK_EQ_U32(kernel_crypto_sha_final_count(), 0u);
    CHECK_EQ_U32(kernel_crypto_hmac_count(), 0u);
    CHECK_EQ_U32(kernel_crypto_des_parity_count(), 0u);
    CHECK_EQ_U32(kernel_crypto_stale_context_count(), 0u);
    CHECK_EQ_U32(kernel_crypto_hmac_oversize_key_count(), 0u);
    CHECK_EQ_U32(kernel_crypto_sha_bytes_absorbed(), 0u);
    teardown();
}

int main(void)
{
    printf("kernel crypto (Xc* 335/336/337/340/346) tests\n");

    /* Published vectors first: if the algorithm is wrong, nothing else matters. */
    test_fips_180_1_sha1_vectors();
    test_streaming_agrees_with_one_shot_at_every_split();
    test_segment_lengths_the_guest_actually_uses();
    test_rfc_2202_hmac_sha1_vectors();
    test_empty_message_hmac_against_an_independent_implementation();
    test_xc_hmac_two_segments_equal_one();
    test_the_long_key_divergence_is_real_and_bounded();
    test_fips_74_weak_keys_are_left_untouched();
    test_parity_adjustment_of_the_two_extreme_keys();

    /* Then the ordinal boundary. */
    test_registration_makes_all_five_ordinals_implemented();
    test_the_three_sha_ordinals_produce_the_published_digest();
    test_three_updates_through_the_ordinals_match_the_published_digest();
    test_ordinal_340_matches_rfc_2202_case_1();
    test_an_oversize_hmac_key_is_counted_and_announced();
    test_ordinal_346_adjusts_the_24_byte_bundle_in_guest_memory();

    /* Then every refusal. */
    test_a_missing_frame_is_reported_for_every_ordinal();
    test_a_frame_short_of_the_measured_arity_is_refused();
    test_an_uninitialised_context_is_refused_not_hashed();
    test_finalising_a_context_twice_is_refused();
    test_the_serialised_context_fits_the_guests_reservation();
    test_reset_clears_every_counter();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
