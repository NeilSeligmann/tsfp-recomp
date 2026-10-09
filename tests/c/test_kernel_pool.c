/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the executive pool: ExAllocatePoolWithTag (15) and ExFreePool (17).
 *
 * THE LOAD-BEARING ASSERTIONS. An allocator can be wrong in ways that do not fault
 * for a long time, so these tests go after the two that matter:
 *
 *   - ALIASING. Two live allocations must never overlap, and a freed block must not
 *     still be reachable as the same tracked allocation. An overlap corrupts the
 *     guest arbitrarily far from the allocation that caused it.
 *   - REACHABILITY. Every address handed out must be genuinely writable guest
 *     memory below 4 GB. An address that merely looks plausible is the worst case,
 *     because the guest writes through it and the damage surfaces elsewhere.
 *
 * And the arity, which is checked by giving the handler a frame that is too short
 * for the second argument: a handler that read only one argument would pass every
 * other test in this file.
 */

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_pool.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                        \
        if (!(cond)) {                                                                    \
            failures++;                                                                   \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
            fflush(stdout);                                                               \
        }                                                                                 \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                 \
        checks++;                                                                         \
        uint32_t check_a = (uint32_t)(actual);                                             \
        uint32_t check_e = (uint32_t)(expected);                                           \
        if (check_a != check_e) {                                                          \
            failures++;                                                                    \
            printf("  FAIL %s:%d  %s == %s (got %#x, want %#x)\n", __FILE__, __LINE__,     \
                   #actual, #expected, check_a, check_e);                                  \
            fflush(stdout);                                                               \
        }                                                                                  \
    } while (0)

static char captured[16384];
static size_t captured_len;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(captured + captured_len, sizeof(captured) - captured_len,
                            format, args);
    va_end(args);
    if (written > 0) {
        captured_len += (size_t)written;
        if (captured_len >= sizeof(captured)) {
            captured_len = sizeof(captured) - 1;
        }
    }
    return written;
}

static void reset_capture(void)
{
    captured[0] = '\0';
    captured_len = 0;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

#define SCRATCH_BYTES 0x1000u
#define SCRATCH_OUT 0x200u

static kernel_guest_ptr scratch;

/* Real tags lifted from this image's own call sites, so the tests exercise what the
 * guest actually passes rather than a made-up constant. 'NETD' and 'hawk' both
 * appear at ordinal 15 call sites. */
#define TAG_NETD 0x4454454Eu
#define TAG_HAWK 0x6B776168u

/* Allocate the scratch region the call frames are built in.
 *
 * Separate from setup() because one test deliberately calls guest_mem_reset(), which
 * unmaps this region. Guest accessors reject the old frame, so that test allocates
 * scratch again to exercise pool behavior with a valid frame. */
static void allocate_scratch(void)
{
    guest_region_request request = {
        .bytes = SCRATCH_BYTES,
        .alignment = 0u,
        .lowest_physical = 0u,
        .highest_physical = 0u,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .contiguous = true,
        .fixed_base = 0u,
    };
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("  FATAL: could not allocate scratch (status %#x)\n", status);
        exit(EXIT_FAILURE);
    }
}

static void setup(void)
{
    kernel_hle_init();
    kernel_pool_reset();
    guest_mem_reset();
    CHECK_EQ_U32(kernel_pool_register(), 3u);
    kernel_hle_set_log(capture_printer);
    reset_capture();
    allocate_scratch();
}

static void teardown(void)
{
    /* Pool before guest_mem: the pool owns a heap inside the memory model, and
     * tearing the model down first is the stale-handle path the implementation
     * reports. One test exercises that order deliberately; every other test uses
     * this one. */
    kernel_pool_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
}

static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, args, count)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    return kernel_hle_call(ordinal, &frame);
}

static kernel_guest_ptr pool_alloc(uint32_t bytes, uint32_t tag)
{
    uint32_t args[2] = {bytes, tag};
    return (kernel_guest_ptr)call_ordinal(15u, args, 2u);
}

static void pool_free(kernel_guest_ptr address)
{
    uint32_t args[1] = {address};
    (void)call_ordinal(17u, args, 1u);
}

/* Write a byte pattern over a block and read it back, proving the address is real
 * writable guest memory and not merely a plausible number. */
static bool block_is_writable(kernel_guest_ptr address, uint32_t bytes)
{
    unsigned char *host = (unsigned char *)kernel_guest_at(address, bytes);
    if (!host) {
        return false;
    }
    for (uint32_t i = 0u; i < bytes; i++) {
        host[i] = (unsigned char)(i & 0xFFu);
    }
    for (uint32_t i = 0u; i < bytes; i++) {
        if (host[i] != (unsigned char)(i & 0xFFu)) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Allocation.
 * ------------------------------------------------------------------------- */

static void test_an_allocation_is_real_writable_guest_memory(void)
{
    setup();
    kernel_guest_ptr block = pool_alloc(0xD50u, TAG_NETD);
    CHECK(block != 0u);
    /* 0xD50 is one of the sizes this image's own call sites pass. */
    CHECK(block_is_writable(block, 0xD50u));
    /* Below 4 GB by construction: a guest pointer is 4 bytes, so an address that
     * did not fit would have been truncated on the way out. */
    CHECK(((uint64_t)block + 0xD50u) <= 0xFFFFFFFFu);
    CHECK_EQ_U32(kernel_pool_live_count(), 1u);
    CHECK_EQ_U32(kernel_pool_live_bytes(), 0xD50u);
    teardown();
}

static void test_the_tag_and_size_are_recorded_not_dropped(void)
{
    setup();
    kernel_guest_ptr block = pool_alloc(0x1Cu, TAG_HAWK);
    CHECK(block != 0u);

    uint32_t tag = 0u;
    CHECK(kernel_pool_tag_of(block, &tag));
    CHECK_EQ_U32(tag, TAG_HAWK);

    uint32_t bytes = 0u;
    CHECK(kernel_pool_size_of(block, &bytes));
    /* The REQUESTED size, not the rounded one. 0x1C rounds up to the heap's 16-byte
     * alignment, so a handler that recorded the rounded figure would report 0x20
     * and the distinction between a 28-byte and a 32-byte request would be lost. */
    CHECK_EQ_U32(bytes, 0x1Cu);
    teardown();
}

static void test_the_two_arguments_are_in_the_measured_order(void)
{
    setup();
    /* MEASURED: the push order at every call site is Tag then NumberOfBytes, so the
     * C order is (NumberOfBytes, Tag). Swapping them is the single easiest mistake
     * to make here, and it would be nearly silent -- so this pins it with values
     * that cannot be confused. 0x40 is a real size from the image; TAG_NETD is a
     * real tag. If the handler read them the other way round it would try to
     * allocate 0x4454454E bytes and record a tag of 0x40. */
    kernel_guest_ptr block = pool_alloc(0x40u, TAG_NETD);
    CHECK(block != 0u);
    uint32_t bytes = 0u;
    uint32_t tag = 0u;
    CHECK(kernel_pool_size_of(block, &bytes));
    CHECK(kernel_pool_tag_of(block, &tag));
    CHECK_EQ_U32(bytes, 0x40u);
    CHECK_EQ_U32(tag, TAG_NETD);
    teardown();
}

static void test_live_allocations_never_overlap(void)
{
    setup();
    enum { COUNT = 24 };
    kernel_guest_ptr blocks[COUNT];
    static const uint32_t sizes[COUNT] = {
        16u, 1u,   64u,  4u,   0xD50u, 32u,  7u,    128u,
        3u,  256u, 11u,  512u, 0x1Cu,  13u,  1024u, 5u,
        48u, 96u,  192u, 384u, 9u,     768u, 17u,   0x40u,
    };

    for (unsigned i = 0u; i < COUNT; i++) {
        blocks[i] = pool_alloc(sizes[i], TAG_NETD);
        CHECK(blocks[i] != 0u);
    }
    CHECK_EQ_U32(kernel_pool_live_count(), COUNT);

    /* Pairwise non-overlap over the REQUESTED extents. The guest is entitled to
     * write every requested byte, so two requests whose extents intersect means one
     * guest write silently corrupts another subsystem's data. */
    for (unsigned i = 0u; i < COUNT; i++) {
        for (unsigned j = i + 1u; j < COUNT; j++) {
            bool disjoint = (blocks[i] + sizes[i] <= blocks[j])
                            || (blocks[j] + sizes[j] <= blocks[i]);
            CHECK(disjoint);
        }
    }

    /* Each block independently writable, and writing one does not disturb another:
     * stamp every block with its own index, then verify all of them afterwards. */
    for (unsigned i = 0u; i < COUNT; i++) {
        unsigned char *host = (unsigned char *)kernel_guest_at(blocks[i], sizes[i]);
        CHECK(host != NULL);
        if (host) {
            memset(host, (int)(i + 1u), sizes[i]);
        }
    }
    for (unsigned i = 0u; i < COUNT; i++) {
        const unsigned char *host =
            (const unsigned char *)kernel_guest_at(blocks[i], sizes[i]);
        CHECK(host != NULL);
        if (host) {
            bool intact = true;
            for (uint32_t b = 0u; b < sizes[i]; b++) {
                if (host[b] != (unsigned char)(i + 1u)) {
                    intact = false;
                }
            }
            CHECK(intact);
        }
    }
    teardown();
}

static void test_a_zero_byte_request_is_refused_and_reported(void)
{
    setup();
    CHECK_EQ_U32(pool_alloc(0u, TAG_NETD), 0u);
    CHECK_EQ_U32(kernel_pool_live_count(), 0u);
    CHECK(captured_contains("zero-byte pool request refused"));
    /* The tag is rendered readably, so a report names the subsystem. */
    CHECK(captured_contains("NETD"));
    teardown();
}

/* ---------------------------------------------------------------------------
 * Freeing.
 * ------------------------------------------------------------------------- */

static void test_free_releases_the_block_and_the_accounting(void)
{
    setup();
    kernel_guest_ptr block = pool_alloc(64u, TAG_NETD);
    CHECK(block != 0u);
    CHECK_EQ_U32(kernel_pool_live_count(), 1u);
    CHECK_EQ_U32(kernel_pool_live_bytes(), 64u);

    pool_free(block);
    CHECK_EQ_U32(kernel_pool_live_count(), 0u);
    CHECK_EQ_U32(kernel_pool_live_bytes(), 0u);
    CHECK_EQ_U32(kernel_pool_bad_free_count(), 0u);
    /* No longer a tracked allocation. */
    CHECK(!kernel_pool_tag_of(block, NULL));
    teardown();
}

static void test_a_double_free_is_refused_and_counted(void)
{
    setup();
    kernel_guest_ptr block = pool_alloc(64u, TAG_NETD);
    pool_free(block);
    CHECK_EQ_U32(kernel_pool_bad_free_count(), 0u);

    pool_free(block);
    CHECK_EQ_U32(kernel_pool_bad_free_count(), 1u);
    CHECK(captured_contains("the pool never issued this address"));
    /* The accounting must not go negative and wrap: live_bytes is unsigned, so a
     * second decrement would make it enormous rather than obviously wrong. */
    CHECK_EQ_U32(kernel_pool_live_count(), 0u);
    CHECK_EQ_U32(kernel_pool_live_bytes(), 0u);
    teardown();
}

static void test_freeing_null_is_reported_as_a_bugcheck_not_ignored(void)
{
    setup();
    pool_free(0u);
    /* C's free(NULL) is a no-op; ExFreePool(NULL) is a bugcheck. Treating it as
     * harmless would hide an unchecked allocation failure upstream. */
    CHECK_EQ_U32(kernel_pool_bad_free_count(), 1u);
    CHECK(captured_contains("ExFreePool(NULL)"));
    teardown();
}

static void test_freeing_an_address_from_elsewhere_is_refused(void)
{
    setup();
    /* A real, mapped, perfectly valid guest address that this pool did not issue.
     * It must be refused on provenance, not accepted because it happens to be
     * readable. */
    pool_free(scratch);
    CHECK_EQ_U32(kernel_pool_bad_free_count(), 1u);
    CHECK(captured_contains("the pool never issued this address"));
    teardown();
}

static void test_an_interior_pointer_is_not_a_valid_free(void)
{
    setup();
    kernel_guest_ptr block = pool_alloc(128u, TAG_NETD);
    CHECK(block != 0u);
    /* Freeing the middle of a block must fail: the real allocator needs the exact
     * base, and accepting an interior pointer would corrupt the heap's bookkeeping
     * while appearing to work. */
    pool_free((kernel_guest_ptr)(block + 16u));
    CHECK_EQ_U32(kernel_pool_bad_free_count(), 1u);
    /* The real block is untouched and still live. */
    CHECK_EQ_U32(kernel_pool_live_count(), 1u);
    CHECK(kernel_pool_tag_of(block, NULL));
    teardown();
}

static void test_a_freed_block_can_be_reused_without_aliasing_a_live_one(void)
{
    setup();
    kernel_guest_ptr keep = pool_alloc(64u, TAG_NETD);
    kernel_guest_ptr drop = pool_alloc(64u, TAG_HAWK);
    CHECK(keep != 0u);
    CHECK(drop != 0u);
    pool_free(drop);

    kernel_guest_ptr again = pool_alloc(64u, TAG_HAWK);
    CHECK(again != 0u);
    /* Reuse is fine and expected. Aliasing the block we KEPT is not. */
    CHECK(again != keep);
    bool disjoint = (again + 64u <= keep) || (keep + 64u <= again);
    CHECK(disjoint);
    CHECK_EQ_U32(kernel_pool_live_count(), 2u);
    teardown();
}

/* ---------------------------------------------------------------------------
 * Arity and the call boundary.
 * ------------------------------------------------------------------------- */

static void test_alloc_needs_two_arguments(void)
{
    setup();
    /* Room for a return address and one argument only. A handler that read a single
     * argument -- which is what the measured table's 0 would encourage -- passes
     * every other test in this file and fails only here. */
    kernel_call_frame narrow = {scratch, (kernel_guest_ptr)(scratch + 8u), 0u, 0u, false, 0u, false};
    CHECK_EQ_U32(kernel_hle_call(15u, &narrow), 0u);
    CHECK(captured_contains("could not read argument 1"));
    CHECK_EQ_U32(kernel_pool_live_count(), 0u);
    teardown();
}

static void test_free_needs_one_argument(void)
{
    setup();
    kernel_call_frame narrow = {scratch, (kernel_guest_ptr)(scratch + 4u), 0u, 0u, false, 0u, false};
    (void)kernel_hle_call(17u, &narrow);
    CHECK(captured_contains("could not read argument 0"));
    /* Crucially NOT counted as a bad free: we never got an address to judge. */
    CHECK_EQ_U32(kernel_pool_bad_free_count(), 0u);
    teardown();
}

static void test_missing_frames_are_reported(void)
{
    setup();
    (void)kernel_hle_call(15u, NULL);
    CHECK(captured_contains("ExAllocatePoolWithTag called with no argument frame"));
    (void)kernel_hle_call(17u, NULL);
    CHECK(captured_contains("ExFreePool called with no argument frame"));
    teardown();
}

/* ---------------------------------------------------------------------------
 * Lifecycle.
 * ------------------------------------------------------------------------- */

static void test_reset_releases_everything(void)
{
    setup();
    for (unsigned i = 0u; i < 8u; i++) {
        CHECK(pool_alloc(64u, TAG_NETD) != 0u);
    }
    CHECK_EQ_U32(kernel_pool_live_count(), 8u);
    size_t heaps_before = guest_mem_heap_count();
    CHECK(heaps_before >= 1u);

    kernel_pool_reset();
    CHECK_EQ_U32(kernel_pool_live_count(), 0u);
    CHECK_EQ_U32(kernel_pool_live_bytes(), 0u);
    CHECK_EQ_U32(kernel_pool_bad_free_count(), 0u);
    /* The backing heap is gone too, so reset does not leak pages. */
    CHECK(guest_mem_heap_count() < heaps_before);
    teardown();
}

static void test_a_heap_destroyed_underneath_the_pool_is_reported_not_blamed_on_the_guest(void)
{
    setup();
    kernel_guest_ptr block = pool_alloc(64u, TAG_NETD);
    CHECK(block != 0u);

    /* Tear the memory model down WITHOUT resetting the pool. The pool's heap handle
     * is now stale. The danger is that the next allocation silently succeeds while
     * the table still claims the old block is live -- and then a legitimate free of
     * a reissued address gets reported as a guest bug. */
    guest_mem_reset();
    reset_capture();
    /* The reset unmapped scratch too. Guest accessors would reject the old frame,
     * so allocate fresh scratch to exercise the stale pool handle instead. */
    allocate_scratch();

    kernel_guest_ptr after = pool_alloc(64u, TAG_NETD);
    CHECK(after != 0u);
    CHECK(captured_contains("was destroyed underneath the pool"));
    /* The stale record was dropped, so exactly one allocation is live. */
    CHECK_EQ_U32(kernel_pool_live_count(), 1u);
    CHECK_EQ_U32(kernel_pool_live_bytes(), 64u);
    teardown();
}

static uint32_t query_block_size(kernel_guest_ptr address)
{
    uint32_t args[1] = {address};
    return (uint32_t)call_ordinal(23u, args, 1u);
}

static void test_query_block_size_reports_the_requested_size_until_freed(void)
{
    setup();
    kernel_guest_ptr block = pool_alloc(0x1Cu, TAG_HAWK);
    CHECK(block != 0u);
    CHECK_EQ_U32(query_block_size(block), 0x1Cu);
    /* Interior pointer and unknown address are not blocks: 0, reported. */
    CHECK_EQ_U32(query_block_size(block + 4u), 0u);
    CHECK(captured_contains("ExQueryPoolBlockSize"));
    pool_free(block);
    CHECK_EQ_U32(query_block_size(block), 0u);
    teardown();
}

static void test_query_block_size_needs_one_argument_and_a_frame(void)
{
    setup();
    kernel_call_frame narrow = {scratch, (kernel_guest_ptr)(scratch + 4u), 0u, 0u, false, 0u, false};
    CHECK_EQ_U32(kernel_hle_call(23u, &narrow), 0u);
    CHECK(captured_contains("could not read argument 0"));
    (void)kernel_hle_call(23u, NULL);
    CHECK(captured_contains("ExQueryPoolBlockSize called with no argument frame"));
    teardown();
}

static void test_registration_makes_both_ordinals_implemented(void)
{
    setup();
    static const unsigned ordinals[] = {15u, 17u, 23u};
    CHECK_EQ_U32(kernel_hle_implemented_count(ordinals, 3u), 3u);
    teardown();
}

static void test_implemented_ordinals_do_not_report_themselves_as_stubs(void)
{
    setup();
    kernel_guest_ptr block = pool_alloc(64u, TAG_NETD);
    pool_free(block);
    /* A real implementation must never emit the stub notice, or the backlog report
     * would keep listing work that is already done. */
    CHECK(!captured_contains("not implemented"));
    CHECK(!captured_contains("stub"));
    teardown();
}

int main(void)
{
    printf("kernel pool HLE tests\n");

    test_an_allocation_is_real_writable_guest_memory();
    test_the_tag_and_size_are_recorded_not_dropped();
    test_the_two_arguments_are_in_the_measured_order();
    test_live_allocations_never_overlap();
    test_a_zero_byte_request_is_refused_and_reported();

    test_free_releases_the_block_and_the_accounting();
    test_a_double_free_is_refused_and_counted();
    test_freeing_null_is_reported_as_a_bugcheck_not_ignored();
    test_freeing_an_address_from_elsewhere_is_refused();
    test_an_interior_pointer_is_not_a_valid_free();
    test_a_freed_block_can_be_reused_without_aliasing_a_live_one();

    test_alloc_needs_two_arguments();
    test_free_needs_one_argument();
    test_missing_frames_are_reported();

    test_reset_releases_everything();
    test_a_heap_destroyed_underneath_the_pool_is_reported_not_blamed_on_the_guest();
    test_query_block_size_reports_the_requested_size_until_freed();
    test_query_block_size_needs_one_argument_and_a_frame();
    test_registration_makes_both_ordinals_implemented();
    test_implemented_ordinals_do_not_report_themselves_as_stubs();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
