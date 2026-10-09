/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T827: guest reads and writes are plain copies once the page is known good (no process_vm_readv/writev per access,
 * 31 percent of the guest thread CPU in the intro movies), and a concurrent unmap still makes the accessor return
 * false instead of faulting. The tests: the counters show the fast path is taken, a page change bracketed by
 * kernel_guest_probe_change_begin disables it at once (the read after a raw munmap must refuse, not crash), a flush
 * without a begin still invalidates by epoch, a write fast path needs a prior successful write (a read-only page is
 * never written to), and thread slots are released at thread exit (more than 256 threads in sequence).
 */
#define _GNU_SOURCE
#include "kernel_call.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BASE 0x46000000u
#define PAGES 8u

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static void stats(uint64_t *fast_reads, uint64_t *fast_writes, uint64_t *calls)
{
    kernel_guest_copy_stats(fast_reads, fast_writes, calls);
}

static void *thread_read(void *argument)
{
    uint8_t value[4];
    int *fast_seen = argument;
    uint64_t before = 0u, after = 0u;
    (void)kernel_guest_read_bytes(BASE, value, sizeof value); /* warms this thread's cache with a system call */
    stats(&before, NULL, NULL);
    *fast_seen = kernel_guest_read_bytes(BASE, value, sizeof value) && value[0] == 0x5A;
    stats(&after, NULL, NULL);
    *fast_seen = *fast_seen && after == before + 1u;
    return NULL;
}

typedef struct {
    atomic_uint *ready;
    atomic_bool *go;
    int *ok;
} cold_probe_argument;

static void *cold_probe(void *argument)
{
    cold_probe_argument *probe = argument;
    uint8_t value = 0u;
    atomic_fetch_add_explicit(probe->ready, 1u, memory_order_release);
    while (!atomic_load_explicit(probe->go, memory_order_acquire)) sched_yield();
    *probe->ok = kernel_guest_read_bytes(BASE, &value, sizeof value) && value == 0x5Au;
    return NULL;
}

int main(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *memory = mmap((void *)(uintptr_t)BASE, PAGES * page, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(memory == (void *)(uintptr_t)BASE);
    if (memory != (void *)(uintptr_t)BASE) return 1;
    memset(memory, 0x5A, PAGES * page);

    /* The first accessor call races across guest threads in a cold process. Keep this before any call that can
     * reach host_page_bytes(), so the test covers its one-time initialization rather than a warmed cache. */
    enum { COLD_PROBE_THREADS = 16 };
    atomic_uint ready = 0u;
    atomic_bool go = false;
    pthread_t cold_threads[COLD_PROBE_THREADS];
    int cold_ok[COLD_PROBE_THREADS] = {0};
    cold_probe_argument cold_args[COLD_PROBE_THREADS];
    unsigned cold_created = 0u;
    for (; cold_created < COLD_PROBE_THREADS; cold_created++) {
        cold_args[cold_created] = (cold_probe_argument){&ready, &go, &cold_ok[cold_created]};
        if (pthread_create(&cold_threads[cold_created], NULL, cold_probe, &cold_args[cold_created]) != 0) {
            CHECK(false);
            break;
        }
    }
    while (atomic_load_explicit(&ready, memory_order_acquire) < cold_created) sched_yield();
    atomic_store_explicit(&go, true, memory_order_release);
    for (unsigned index = 0u; index < cold_created; index++) {
        CHECK(pthread_join(cold_threads[index], NULL) == 0);
        CHECK(cold_ok[index]);
    }
    CHECK(cold_created == COLD_PROBE_THREADS);
    kernel_guest_probe_cache_flush();

    /* A cold page costs one probe system call and is then read with a plain copy, a warm page costs nothing but the
     * copy. No read ever needs the copy system call while nothing changes protection. */
    uint8_t value[8];
    uint64_t reads0 = 0u, writes0 = 0u, calls0 = 0u, reads1 = 0u, writes1 = 0u, calls1 = 0u;
    uint64_t hits0 = 0u, probes0 = 0u, hits1 = 0u, probes1 = 0u;
    stats(&reads0, &writes0, &calls0);
    kernel_guest_probe_stats(&hits0, &probes0);
    CHECK(kernel_guest_read_bytes(BASE + 4u, value, sizeof value) && value[0] == 0x5A);
    stats(&reads1, &writes1, &calls1);
    kernel_guest_probe_stats(&hits1, &probes1);
    CHECK(reads1 == reads0 + 1u && calls1 == calls0 && probes1 == probes0 + 1u);
    CHECK(kernel_guest_read_bytes(BASE + 4u, value, sizeof value) && value[7] == 0x5A);
    stats(&reads0, &writes0, &calls0);
    kernel_guest_probe_stats(&hits0, &probes0);
    CHECK(reads0 == reads1 + 1u && calls0 == calls1 && probes0 == probes1 && hits0 == hits1 + 1u);

    /* A write needs one successful system-call write first, then it is a plain copy and lands in memory. */
    const uint32_t word = 0x11223344u;
    CHECK(kernel_guest_write_bytes(BASE + 16u, &word, sizeof word));
    stats(&reads1, &writes1, &calls1);
    CHECK(writes1 == writes0 && calls1 == calls0 + 1u);
    const uint32_t second = 0x55667788u;
    CHECK(kernel_guest_write_bytes(BASE + 16u, &second, sizeof second));
    stats(&reads0, &writes0, &calls0);
    CHECK(writes0 == writes1 + 1u && calls0 == calls1);
    uint32_t check_word = 0u;
    memcpy(&check_word, memory + 16u, sizeof check_word);
    CHECK(check_word == second);

    /* A page change bracketed by change_begin: the cached page is unmapped, and the read refuses instead of faulting.
     * Without the pending flag the fast path would copy from the unmapped page and kill this test with SIGSEGV. */
    CHECK(kernel_guest_read_bytes(BASE + 3u * (uint32_t)page + 8u, value, sizeof value)); /* page 3 is known good */
    CHECK(kernel_guest_write_bytes(BASE + 3u * (uint32_t)page + 8u, &word, sizeof word));
    stats(NULL, NULL, &calls0);
    kernel_guest_probe_change_begin();
    CHECK(munmap(memory + 3u * page, page) == 0);
    CHECK(!kernel_guest_read_bytes(BASE + 3u * (uint32_t)page + 8u, value, sizeof value));
    CHECK(!kernel_guest_write_bytes(BASE + 3u * (uint32_t)page + 8u, &word, sizeof word));
    stats(NULL, NULL, &calls1);
    CHECK(calls1 == calls0 + 2u); /* the two refused accesses took the system call, the fast path was off */
    kernel_guest_probe_cache_flush();
    CHECK(!kernel_guest_read_bytes(BASE + 3u * (uint32_t)page + 8u, value, sizeof value));
    /* Reads of the pages that remain are fine again, and fast once re-probed. */
    CHECK(kernel_guest_read_bytes(BASE + 4u, value, sizeof value));
    stats(&reads0, NULL, &calls0);
    CHECK(kernel_guest_read_bytes(BASE + 4u, value, sizeof value));
    stats(&reads1, NULL, &calls1);
    CHECK(reads1 == reads0 + 1u && calls1 == calls0);

    /* A flush alone (no begin) still invalidates by epoch: the next read of a page unmapped before the flush refuses. */
    CHECK(kernel_guest_read_bytes(BASE + 4u * (uint32_t)page, value, sizeof value)); /* warm */
    CHECK(kernel_guest_read_bytes(BASE + 4u * (uint32_t)page, value, sizeof value)); /* fast */
    CHECK(munmap(memory + 4u * page, page) == 0);
    kernel_guest_probe_cache_flush();
    CHECK(!kernel_guest_read_bytes(BASE + 4u * (uint32_t)page, value, sizeof value));

    /* Read-only pages: a read is fast, a write is refused by the system call and never reaches the fast path. */
    CHECK(mprotect(memory + 5u * page, page, PROT_READ) == 0);
    kernel_guest_probe_cache_flush();
    CHECK(kernel_guest_read_bytes(BASE + 5u * (uint32_t)page, value, sizeof value));
    CHECK(!kernel_guest_write_bytes(BASE + 5u * (uint32_t)page, &word, sizeof word));
    CHECK(!kernel_guest_write_bytes(BASE + 5u * (uint32_t)page, &word, sizeof word)); /* a refusal is not remembered */

    /* A write across two pages takes the fast path only when both pages are proven writable. */
    CHECK(kernel_guest_write_bytes(BASE + 6u * (uint32_t)page, &word, sizeof word)); /* proves page 6 */
    stats(NULL, &writes0, &calls0);
    CHECK(kernel_guest_write_bytes(BASE + 7u * (uint32_t)page - 2u, &word, sizeof word)); /* pages 6 and 7 */
    stats(NULL, &writes1, &calls1);
    CHECK(writes1 == writes0 && calls1 == calls0 + 1u); /* page 7 unproven: the system call */
    CHECK(kernel_guest_write_bytes(BASE + 7u * (uint32_t)page - 2u, &word, sizeof word));
    stats(NULL, &writes0, &calls0);
    CHECK(writes0 == writes1 + 1u && calls0 == calls1); /* both proven now: a plain copy */

    /* change_begin returns at once when no read is in flight, and nests. */
    kernel_guest_probe_change_begin();
    kernel_guest_probe_change_begin();
    kernel_guest_probe_cache_flush();
    kernel_guest_probe_cache_flush();
    stats(&reads0, NULL, &calls0);
    CHECK(kernel_guest_read_bytes(BASE + 4u, value, sizeof value)); /* re-probe */
    CHECK(kernel_guest_read_bytes(BASE + 4u, value, sizeof value));
    stats(&reads1, NULL, &calls1);
    CHECK(reads1 == reads0 + 2u && calls1 == calls0); /* both pending counts were released: plain copies again */

    /* Thread slots are released at thread exit: 300 threads in sequence, the last still gets the fast path. */
    int fast_seen = 0;
    for (unsigned index = 0; index < 300u; index++) {
        pthread_t worker;
        fast_seen = 0;
        CHECK(pthread_create(&worker, NULL, thread_read, &fast_seen) == 0);
        CHECK(pthread_join(worker, NULL) == 0);
        if (!fast_seen) break;
    }
    CHECK(fast_seen);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_kernel_guest_fast_copy: all checks passed\n");
    return 0;
}
