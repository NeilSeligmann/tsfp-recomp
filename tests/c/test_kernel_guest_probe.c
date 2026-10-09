/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T81: kernel_guest_at() must refuse a PROT_NONE guard page, not just an unmapped one.
 *
 * The old probe was msync(MS_ASYNC), which answers "mapped". A PROT_NONE page IS
 * mapped, so the old probe said yes and the caller's dereference faulted. This test
 * demonstrates the wrong old answer directly (msync returns 0 on the PROT_NONE page)
 * and asserts the new process_vm_readv probe refuses it, alongside the cases that
 * must keep working: mapped-readable, read-only, unmapped, ranges straddling the
 * readable/PROT_NONE boundary, and a range wide enough to cross the probe's
 * 64-page batching boundary.
 */
#define _GNU_SOURCE
#include "kernel_call.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BASE 0x44000000u
#define PAGES 160u

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while (0)

int main(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *memory = mmap((void *)(uintptr_t)BASE, PAGES * page, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(memory == (void *)(uintptr_t)BASE);
    if (memory != (void *)(uintptr_t)BASE) return 1;
    memset(memory, 0x5A, PAGES * page);

    /* Mapped and readable: translated, for a single byte and for a whole-range span
     * wide enough to cross the probe's internal 64-page iovec batch at least twice. */
    CHECK(kernel_guest_at(BASE, 1u) != NULL);
    CHECK(kernel_guest_at(BASE + 3u, (size_t)(PAGES * page) - 3u) != NULL);
    CHECK(kernel_guest_range_readable(BASE, PAGES * page));

    /* Page 1 becomes a PROT_NONE guard band. */
    CHECK(mprotect(memory + page, page, PROT_NONE) == 0);
    kernel_guest_probe_cache_flush(); /* T819: the contract of the per-thread positive cache */

    /* THE OLD ANSWER, SHOWN WRONG IN PLACE: msync still calls the guard page mapped.
     * If the probe were still msync-based, kernel_guest_at would hand back a pointer
     * whose first dereference faults. */
    CHECK(msync(memory + page, page, MS_ASYNC) == 0);

    /* The new probe refuses the guard page and every range that touches it. */
    CHECK(kernel_guest_at(BASE + (uint32_t)page, 1u) == NULL);
    CHECK(kernel_guest_at(BASE + (uint32_t)page, 4u) == NULL);
    CHECK(!kernel_guest_range_readable(BASE + (uint32_t)page, 4u));

    /* Boundary: a range straddling readable page 0 into the guard page is refused,
     * while the same start kept inside page 0 still passes. Same on the far side:
     * straddling out of the guard into readable page 2 is refused, page 2 alone is
     * not. */
    CHECK(kernel_guest_at(BASE + (uint32_t)page - 2u, 4u) == NULL);
    CHECK(kernel_guest_at(BASE + (uint32_t)page - 2u, 2u) != NULL);
    CHECK(kernel_guest_at(BASE + 2u * (uint32_t)page - 2u, 4u) == NULL);
    CHECK(kernel_guest_at(BASE + 2u * (uint32_t)page, 4u) != NULL);

    /* A guard page beyond the first probe batch must also be seen: page 100 sits in
     * the second 64-page iovec batch of a whole-range probe. */
    CHECK(mprotect(memory + 100u * page, page, PROT_NONE) == 0);
    kernel_guest_probe_cache_flush(); /* T819: the contract of the per-thread positive cache */
    CHECK(!kernel_guest_range_readable(BASE, PAGES * page));
    CHECK(kernel_guest_at(BASE + 2u * (uint32_t)page, 99u * page) == NULL);
    CHECK(kernel_guest_at(BASE + 101u * (uint32_t)page, 59u * page) != NULL);
    CHECK(mprotect(memory + 100u * page, page, PROT_READ | PROT_WRITE) == 0);
    kernel_guest_probe_cache_flush(); /* T819: the contract of the per-thread positive cache */

    /* Read-only still passes: the probe means readable, and writability is the
     * documented residual (kernel_call.h). */
    CHECK(mprotect(memory + page, page, PROT_READ) == 0);
    kernel_guest_probe_cache_flush(); /* T819: the contract of the per-thread positive cache */
    CHECK(kernel_guest_at(BASE + (uint32_t)page, 4u) != NULL);

    /* Unmapped is still refused, exactly as before. */
    CHECK(munmap(memory + 2u * page, page) == 0);
    kernel_guest_probe_cache_flush(); /* T819: the contract of the per-thread positive cache */
    CHECK(kernel_guest_at(BASE + 2u * (uint32_t)page, 1u) == NULL);
    CHECK(kernel_guest_at(BASE + 2u * (uint32_t)page - 2u, 4u) == NULL);
    CHECK(!kernel_guest_range_readable(BASE + 2u * (uint32_t)page, 4u));

    /* Zero length names no byte, so it is not probed, matching kernel_guest_at. */
    CHECK(kernel_guest_range_readable(BASE + 2u * (uint32_t)page, 0u));

    /* T819: a readable page is remembered per thread, so the second probe is a cache hit and no system call. */
    kernel_guest_probe_cache_flush();
    uint64_t hits_before = 0u, calls_before = 0u, hits_after = 0u, calls_after = 0u;
    kernel_guest_probe_stats(&hits_before, &calls_before);
    CHECK(kernel_guest_range_readable(BASE + 10u * (uint32_t)page, 1u));
    kernel_guest_probe_stats(&hits_after, &calls_after);
    CHECK(calls_after == calls_before + 1u && hits_after == hits_before);
    CHECK(kernel_guest_range_readable(BASE + 10u * (uint32_t)page + 5u, 8u));
    kernel_guest_probe_stats(&hits_before, &calls_before);
    CHECK(calls_before == calls_after && hits_before == hits_after + 1u);

    /* The cache is dropped as a whole by the flush: a page that became PROT_NONE is seen at once. Until the flush
     * the old answer stands (the documented contract: protection changes call the flush). */
    CHECK(mprotect(memory + 10u * page, page, PROT_NONE) == 0);
    CHECK(kernel_guest_range_readable(BASE + 10u * (uint32_t)page, 1u));
    kernel_guest_probe_cache_flush();
    CHECK(!kernel_guest_range_readable(BASE + 10u * (uint32_t)page, 1u));

    /* A refusal is never remembered: each probe of the guard page asks the kernel again. */
    kernel_guest_probe_stats(&hits_before, &calls_before);
    CHECK(!kernel_guest_range_readable(BASE + 10u * (uint32_t)page, 1u));
    CHECK(!kernel_guest_range_readable(BASE + 10u * (uint32_t)page, 1u));
    kernel_guest_probe_stats(&hits_after, &calls_after);
    CHECK(calls_after == calls_before + 2u && hits_after == hits_before);
    CHECK(mprotect(memory + 10u * page, page, PROT_READ | PROT_WRITE) == 0);
    kernel_guest_probe_cache_flush();
    CHECK(kernel_guest_range_readable(BASE + 10u * (uint32_t)page, 1u));

    /* A read returns the bytes, and a read of an unmapped page is refused (it never faults). */
    uint8_t copy[8] = {0};
    CHECK(kernel_guest_read_bytes(BASE + 10u * (uint32_t)page, copy, sizeof copy) && copy[0] == 0x5A && copy[7] == 0x5A);
    CHECK(!kernel_guest_read_bytes(BASE + 2u * (uint32_t)page, copy, sizeof copy)); /* the unmapped page */

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_kernel_guest_probe: all checks passed\n");
    return 0;
}
