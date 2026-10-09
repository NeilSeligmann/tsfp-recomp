/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T434 regression: the bytes just below a contiguous allocation are readable.
 *
 * The title reads a 0x34-byte table at the base of one 47 MB MmAllocateContiguousMemoryEx
 * block with the sentinel index -1, so it reads 14 bytes BELOW the base (sub_001555E0
 * at 0x15660C, `MEM16(table - 0x34 + 0x26)`). On the console contiguous memory is a
 * window onto flat physical RAM, so the page below is always mapped. On the host the
 * kernel places every MAP_32BIT mmap at a RANDOM start in [0x40000000, 0x42000000), so
 * in about 3 percent of boots the block lands with an unmapped page below and the read
 * is a SIGSEGV (793 versus 797 calls). The allocator now maps a zero readable lead-in
 * page under every contiguous region.
 *
 * Probed with msync, which reports an unmapped page as ENOMEM, never by dereferencing
 * an address that may fault. The probe is repeated so the random placement gets many
 * draws, and the count of probes is asserted before the equality so an empty loop can
 * never pass.
 */

#include "guest_mem.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            failures++;                                                                 \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                    \
            fflush(stdout);                                                             \
        }                                                                               \
    } while (0)

#define DRAWS 400
/* A block, not the crashing 47 MB one: the synthetic physical cursor is never reclaimed so
 * 400 draws of 47 MB would exhaust it, and the random placement does not depend on size.
 * STEP_BACK_BYTES is the 0x34-byte table entry the title steps back by. */
#define BOOT_BLOCK_BYTES 0x00100000u
#define STEP_BACK_BYTES 0x34u

static bool range_is_mapped(uintptr_t address, size_t length)
{
    uintptr_t page = address & ~(uintptr_t)(GUEST_PAGE_SIZE - 1u);
    size_t span = (size_t)(address + length - page);
    return msync((void *)page, span, MS_ASYNC) == 0;
}

static guest_region_request contiguous_request(uint32_t bytes)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = bytes;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    request.contiguous = true;
    return request;
}

static void test_the_bytes_below_a_contiguous_block_are_readable_zeros(void)
{
    unsigned probed = 0u;
    unsigned readable = 0u;
    unsigned zeroed = 0u;
    for (unsigned draw = 0u; draw < DRAWS; draw++) {
        guest_region_request request = contiguous_request(BOOT_BLOCK_BYTES);
        nt_status status = STATUS_SUCCESS;
        kernel_guest_ptr base = guest_region_alloc(&request, &status);
        CHECK(base != 0u);
        if (base == 0u) {
            continue;
        }
        probed++;
        if (range_is_mapped(base - STEP_BACK_BYTES, STEP_BACK_BYTES)) {
            readable++;
            /* Mapped, so reading is safe now. Zero keeps the title's answer fixed. */
            const uint8_t *below = (const uint8_t *)(uintptr_t)(base - STEP_BACK_BYTES);
            bool all_zero = true;
            for (unsigned i = 0u; i < STEP_BACK_BYTES; i++) {
                all_zero = all_zero && below[i] == 0u;
            }
            if (all_zero) {
                zeroed++;
            }
        }
        /* The lead-in is slack, not guest memory: it must not be a tracked region. */
        CHECK(guest_region_containing(base - 1u) == NULL);
        CHECK(guest_region_free(base));
    }
    CHECK(probed == DRAWS);
    CHECK(probed > 0u);
    CHECK(readable == probed);
    CHECK(zeroed == readable);
}

static void test_every_contiguous_alignment_keeps_the_lead_in(void)
{
    const uint32_t alignments[] = {0u, 0x1000u, 0x4000u, 0x10000u};
    unsigned probed = 0u;
    unsigned readable = 0u;
    for (unsigned round = 0u; round < 50u; round++) {
        for (unsigned a = 0u; a < sizeof(alignments) / sizeof(alignments[0]); a++) {
            guest_region_request request = contiguous_request(0x20000u);
            request.alignment = alignments[a];
            nt_status status = STATUS_SUCCESS;
            kernel_guest_ptr base = guest_region_alloc(&request, &status);
            CHECK(base != 0u);
            if (base == 0u) {
                continue;
            }
            probed++;
            if (range_is_mapped(base - STEP_BACK_BYTES, STEP_BACK_BYTES)) {
                readable++;
            }
            if (alignments[a] > GUEST_PAGE_SIZE) {
                CHECK((base & (alignments[a] - 1u)) == 0u);
            }
            CHECK(guest_region_free(base));
        }
    }
    CHECK(probed > 0u);
    CHECK(readable == probed);
}

int main(void)
{
    guest_mem_reset();
    test_the_bytes_below_a_contiguous_block_are_readable_zeros();
    test_every_contiguous_alignment_keeps_the_lead_in();
    printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
