/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1289: the one pass fast path of the small guest accesses and the larger per thread probe cache.
 *
 * MEASURED before: the probe cache had 64 slots, a Story replay lost 4.8 M of its probes to slot conflicts (3 M probe system calls per
 * 30 s) and every u32 access paid a full fence. This test pins the behaviour that must not change (a refusal after a page change, a
 * range over two pages, the counters) and the two things that must (pages that shared a slot no longer evict each other, the
 * counters sum over threads).
 */
#define _GNU_SOURCE
#include "kernel_call.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "probe_cache_wrap.h"

#define BASE 0x45000000u
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while (0)

static void *map_pages(uint32_t address, size_t pages)
{
    return mmap((void *)(uintptr_t)address, pages * (size_t)sysconf(_SC_PAGESIZE), PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
}

static void *reader(void *unused)
{
    (void)unused;
    uint32_t value = 0u;
    for (unsigned count = 0u; count < 5000u; count++) {
        if (!kernel_guest_read_u32(BASE + 8u, &value)) {
            return (void *)1;
        }
    }
    return NULL;
}

int main(void)
{
    const uint32_t page = (uint32_t)sysconf(_SC_PAGESIZE);
    /* Two regions 64 pages apart: the same slot of the old 64 slot direct mapped cache. */
    CHECK(map_pages(BASE, 1u) == (void *)(uintptr_t)BASE);
    CHECK(map_pages(BASE + 64u * page, 1u) == (void *)(uintptr_t)(BASE + 64u * page));
    CHECK(map_pages(BASE + 128u * page, 1u) == (void *)(uintptr_t)(BASE + 128u * page));
    uint32_t value = 0u;
    CHECK(kernel_guest_write_u32(BASE + 8u, 0x11111111u));
    CHECK(kernel_guest_write_u32(BASE + 64u * page + 8u, 0x22222222u));
    CHECK(kernel_guest_write_u32(BASE + 128u * page + 8u, 0x33333333u));
    /* warm: one pass makes the pages known to this thread */
    CHECK(kernel_guest_read_u32(BASE + 8u, &value) && value == 0x11111111u);
    CHECK(kernel_guest_read_u32(BASE + 64u * page + 8u, &value) && value == 0x22222222u);
    CHECK(kernel_guest_read_u32(BASE + 128u * page + 8u, &value) && value == 0x33333333u);

    uint64_t hits_before = 0u, calls_before = 0u, hits_after = 0u, calls_after = 0u;
    uint64_t reads_before = 0u, writes_before = 0u, copy_calls_before = 0u, reads_after = 0u, writes_after = 0u, copy_calls_after = 0u;
    kernel_guest_probe_stats(&hits_before, &calls_before);
    kernel_guest_copy_stats(&reads_before, &writes_before, &copy_calls_before);
    for (unsigned round = 0u; round < 100u; round++) {
        CHECK(kernel_guest_read_u32(BASE + 8u, &value) && value == 0x11111111u);
        CHECK(kernel_guest_read_u32(BASE + 64u * page + 8u, &value) && value == 0x22222222u);
        CHECK(kernel_guest_read_u32(BASE + 128u * page + 8u, &value) && value == 0x33333333u);
        CHECK(kernel_guest_write_u32(BASE + 16u, round));
    }
    kernel_guest_probe_stats(&hits_after, &calls_after);
    kernel_guest_copy_stats(&reads_after, &writes_after, &copy_calls_after);
    /* Pages that shared a slot do not evict each other: not one probe system call, not one slow copy. */
    CHECK(calls_after == calls_before);
    CHECK(copy_calls_after == copy_calls_before);
    /* The counters are exact: 300 reads and 100 writes, and every access answered 1 page from the cache (writes also read the page). */
    CHECK(reads_after - reads_before == 300u);
    CHECK(writes_after - writes_before == 100u);
    CHECK(hits_after - hits_before == 400u);

    /* Another thread's accesses are counted too (each thread counts into its own line, the report sums them). */
    pthread_t thread;
    uint64_t thread_reads_before = 0u, thread_reads_after = 0u;
    kernel_guest_copy_stats(&thread_reads_before, NULL, NULL);
    CHECK(pthread_create(&thread, NULL, reader, NULL) == 0);
    void *result = (void *)1;
    CHECK(pthread_join(thread, &result) == 0);
    CHECK(result == NULL);
    kernel_guest_copy_stats(&thread_reads_after, NULL, NULL);
    CHECK(thread_reads_after - thread_reads_before >= 4900u); /* the first accesses of a new thread probe before they copy */

    /* The miss statistics name the cause. */
    uint64_t cold = 0u, conflict = 0u, stale = 0u, epochs = 0u;
    kernel_guest_probe_miss_stats(&cold, &conflict, &stale, &epochs);
    CHECK(cold >= 3u);

    /* A page change is seen at once by the one pass path: the cached page is refused after the flush and nothing faults. */
    CHECK(mprotect((void *)(uintptr_t)(BASE + 64u * page), page, PROT_NONE) == 0); /* wrapped: change_begin and flush */
    value = 0xDEADBEEFu;
    CHECK(!kernel_guest_read_u32(BASE + 64u * page + 8u, &value) && value == 0xDEADBEEFu);
    CHECK(!kernel_guest_write_u32(BASE + 64u * page + 8u, 1u));
    CHECK(kernel_guest_read_u32(BASE + 8u, &value) && value == 0x11111111u); /* the other pages are fine (probed again) */
    uint64_t epochs_after = 0u;
    kernel_guest_probe_miss_stats(NULL, NULL, NULL, &epochs_after);
    CHECK(epochs_after > epochs);
    CHECK(mprotect((void *)(uintptr_t)(BASE + 64u * page), page, PROT_READ | PROT_WRITE) == 0);
    CHECK(kernel_guest_read_u32(BASE + 64u * page + 8u, &value) && value == 0x22222222u);

    /* A range of two pages: both must be known, and a refused second page refuses the whole access. */
    CHECK(map_pages(BASE + 200u * page, 2u) == (void *)(uintptr_t)(BASE + 200u * page));
    uint8_t sixteen[16], back[16];
    for (unsigned index = 0u; index < 16u; index++) sixteen[index] = (uint8_t)(0xA0u + index);
    CHECK(kernel_guest_write_bytes(BASE + 201u * page - 8u, sixteen, 16u)); /* straddles pages 200 and 201 */
    memset(back, 0, sizeof back);
    CHECK(kernel_guest_read_bytes(BASE + 201u * page - 8u, back, 16u) && memcmp(back, sixteen, 16u) == 0);
    CHECK(mprotect((void *)(uintptr_t)(BASE + 201u * page), page, PROT_NONE) == 0);
    memset(back, 0x77, sizeof back);
    CHECK(!kernel_guest_read_bytes(BASE + 201u * page - 8u, back, 16u));
    CHECK(!kernel_guest_read_u32(BASE + 201u * page - 2u, &value));
    CHECK(kernel_guest_read_bytes(BASE + 201u * page - 16u, back, 8u) && memcmp(back, sixteen, 0u) == 0);

    /* Sizes around the one pass limit (16 bytes) all work and agree. */
    uint8_t big[40], bigback[40];
    for (unsigned index = 0u; index < sizeof big; index++) big[index] = (uint8_t)(index * 7u + 1u);
    for (size_t length = 1u; length <= sizeof big; length++) {
        memset(bigback, 0, sizeof bigback);
        CHECK(kernel_guest_write_bytes(BASE + 300u * 0u + 64u, big, length));
        CHECK(kernel_guest_read_bytes(BASE + 64u, bigback, length) && memcmp(big, bigback, length) == 0);
    }
    /* A null address, an address past the limit, and a zero length keep their answers. */
    CHECK(!kernel_guest_read_u32(0u, &value));
    CHECK(!kernel_guest_write_u32(0u, 1u));
    CHECK(!kernel_guest_read_u32(0xFFFFFFFEu, &value));

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_kernel_guest_small_fast: all checks passed\n");
    return 0;
}
