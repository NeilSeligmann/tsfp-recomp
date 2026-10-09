/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_call.h for the stdcall frame layout and its known limitations.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kernel_call.h"

#include <limits.h>
#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

/* One past the last guest address. The guest space is the low 4 GB, so every
 * range check is against this rather than against a host pointer width. */
#define GUEST_ADDRESS_LIMIT 0x100000000ULL

/* getpid() is a real system call on this libc and the guest memory accessors ran it twice per access (T819:
 * 11 percent of a movie run). The pid is read once and dropped in a forked child (the test suites fork). */
static pid_t g_cached_pid;
static pthread_once_t g_pid_once = PTHREAD_ONCE_INIT;

static void forget_pid_in_child(void)
{
    g_cached_pid = 0;
}

static void register_pid_reset(void)
{
    (void)pthread_atfork(NULL, NULL, forget_pid_in_child);
}

/* T1153: under a DMTCP snapshot run libc getpid() returns a VIRTUAL pid that process_vm_readv rejects, and a
 * restored process has a different real pid than the one a cache would hold. The raw system call is always
 * the real pid, so it is read uncached while the host runs under dmtcp_launch (docs/state-snapshot.md). */
static int g_snapshot_layer = -1;

/* Decided once before main, so the fault handler (host_runtime.c safe_read) reads a plain int and never getenv. */
__attribute__((constructor)) static void detect_snapshot_layer(void)
{
    g_snapshot_layer = getenv("DMTCP_COORD_PORT") != NULL || getenv("DMTCP_GZIP") != NULL ||
                       getenv("DMTCP_CHECKPOINT_DIR") != NULL;
}

static bool under_snapshot_layer(void)
{
    return g_snapshot_layer > 0;
}

/* An inline syscall instruction: DMTCP also wraps syscall(SYS_getpid) and would answer the virtual pid again. */
static pid_t raw_getpid(void)
{
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "0"(39L) : "rcx", "r11", "memory");
    return (pid_t)result;
}

pid_t kernel_host_pid(void)
{
    return under_snapshot_layer() ? raw_getpid() : getpid();
}

static pid_t host_pid(void)
{
    if (under_snapshot_layer()) {
        return raw_getpid();
    }
    (void)pthread_once(&g_pid_once, register_pid_reset);
    pid_t pid = g_cached_pid;
    if (pid == 0) {
        pid = getpid();
        g_cached_pid = pid;
    }
    return pid;
}

/* T819: pages the probe already found readable. The host MMU is the guest page table and the probe is one
 * system call, but the title and the D3D8 model read guest memory hundreds of thousands of times per second,
 * so the syscalls were 70 percent of the CPU of a movie run and the guest ran at 0.6 of real time. A positive
 * answer is remembered per thread (direct mapped, PROBE_CACHE_SLOTS pages) and dropped as a whole whenever a guest range is
 * unmapped or a guard band is protected (kernel_guest_probe_cache_flush, called by guest_mem.c and
 * kernel_thread.c, the only places that change guest page protection). A negative answer is never cached. The
 * residual limit is the one already documented for the probe: a range unmapped by another thread between
 * the probe and the access. */
/* T1289: 64 slots were too few. MEASURED (Story replay, 30 s): 4,827,281 of the 4.83 M misses were conflicts (another page of the
 * same slot), 10 epoch flushes, so 3 M probe system calls per 30 s. 4096 slots (64 KB per cache and thread, 16 MB of guest pages
 * before two pages share a slot) remove them. The fast copy length limit stays the old 128 KB: a flagged copy holds up a page change. */
#define PROBE_CACHE_SLOTS 4096u
#define FAST_COPY_LIMIT_BYTES (64u * 4096u / 2u)
/* Page number folded with its bits above the index, so regions 16 MB apart (the title image and its heap) do not share slots. */
static inline unsigned probe_index(uintptr_t page)
{
    const uintptr_t number = page >> 12;
    return (unsigned)((number ^ (number >> 12)) % PROBE_CACHE_SLOTS);
}
static _Atomic uint64_t g_probe_epoch = 1u;
static _Thread_local struct {
    uintptr_t page;
    uint64_t epoch;
} t_probe_cache[PROBE_CACHE_SLOTS];
/* T827: pages a guest write already succeeded on (process_vm_writev honours page protection, so a success proves the
 * page writable). Same epoch and quiescence rules as the readable cache. */
static _Thread_local struct {
    uintptr_t page;
    uint64_t epoch;
} t_write_cache[PROBE_CACHE_SLOTS];
static uint64_t g_probe_cache_hits;
static uint64_t g_probe_syscalls;
/* T1289: why a page was not in the per-thread cache: never probed (cold), a different page held the slot (conflict), or the
 * entry is from an older epoch (stale: a guest page change flushed it). */
static uint64_t g_probe_miss_cold, g_probe_miss_conflict, g_probe_miss_stale;
static uint64_t g_copy_fast_reads, g_copy_fast_writes, g_copy_syscalls; /* T827 */

/* T827: guest reads without a system call. kernel_guest_read_bytes was a process_vm_readv per access (26 percent of
 * the guest thread CPU in the movies) only so that a concurrent unmap returns false instead of faulting. The same
 * promise is kept without the syscall by a quiescence protocol: a reader that finds every page in its probe cache
 * raises its own flag, re-checks that no guest page change is pending and the epoch is unchanged, copies, and lowers
 * the flag. A page change (guest_mem.c unmap, the kernel_thread.c guard bands, tests) brackets itself with
 * kernel_guest_probe_change_begin (stop new fast reads, wait for the flagged ones to finish) and
 * kernel_guest_probe_cache_flush (new epoch, fast reads allowed again). While a change is pending every read takes
 * the old process_vm_readv path, which reports EFAULT. */
#define COPY_SLOTS 256u
static _Atomic int g_copy_flag[COPY_SLOTS][16]; /* 64 byte stride: one cache line per thread */
static _Atomic int g_copy_owned[COPY_SLOTS];
static _Atomic int g_change_pending;
/* T1289: the small access fast path counts into its thread's own cache line (written by the owner only, summed by the
 * report), not into the shared globals above: those were a lock prefixed add on one shared line, twice per u32 access. */
typedef struct {
    uint64_t probe_hits, fast_reads, fast_writes;
    uint64_t pad[5];
} copy_counts;
static copy_counts g_copy_counts[COPY_SLOTS] __attribute__((aligned(64)));
static pthread_key_t g_copy_key;
static pthread_once_t g_copy_once = PTHREAD_ONCE_INIT;
static _Thread_local int t_copy_slot = -2; /* -2 unassigned, -1 none free */

static void copy_slot_release(void *value)
{
    const intptr_t slot = (intptr_t)value - 1;
    if (slot >= 0) {
        atomic_store(&g_copy_flag[slot][0], 0);
        atomic_store(&g_copy_owned[slot], 0);
    }
}

/* T1289: the reader's "my flag is visible before I look at g_change_pending" step was a seq_cst store, an xchg (a full fence) on EVERY guest
 * access: 6.6 percent of the guest thread CPU, MEASURED (a -g build, 125 thousand accesses per frame). A page change happens about 10 times in
 * a run, so the fence moves to the writer: with sys_membarrier(PRIVATE_EXPEDITED) the writer forces a full barrier on every thread of the
 * process, and the readers use a plain store plus a compiler barrier (the asymmetric Dekker of userspace RCU). Without the system call
 * (old kernel, a sandbox that refuses it) the readers keep the seq_cst store. */
static bool g_membarrier_ok;

static void copy_key_create(void)
{
    (void)pthread_key_create(&g_copy_key, copy_slot_release);
#if defined(__linux__) && defined(__NR_membarrier)
    g_membarrier_ok = syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0;
#endif
}

/* A reader announces itself before it checks that no page change is pending. */
static inline __attribute__((always_inline)) void copy_flag_raise(int slot)
{
    if (g_membarrier_ok) {
        atomic_store_explicit(&g_copy_flag[slot][0], 1, memory_order_relaxed);
        atomic_signal_fence(memory_order_seq_cst);
    } else {
        atomic_store(&g_copy_flag[slot][0], 1); /* seq_cst: the flag is visible before the checks that follow */
    }
}

static int copy_slot(void)
{
    if (t_copy_slot != -2) {
        return t_copy_slot;
    }
    (void)pthread_once(&g_copy_once, copy_key_create);
    t_copy_slot = -1;
    for (unsigned slot = 0u; slot < COPY_SLOTS; slot++) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_copy_owned[slot], &expected, 1)) {
            t_copy_slot = (int)slot;
            (void)pthread_setspecific(g_copy_key, (void *)(intptr_t)(slot + 1u));
            break;
        }
    }
    return t_copy_slot;
}

void kernel_guest_probe_change_begin(void)
{
    (void)pthread_once(&g_copy_once, copy_key_create);
    atomic_fetch_add(&g_change_pending, 1);
#if defined(__linux__) && defined(__NR_membarrier)
    if (g_membarrier_ok && syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) != 0) {
        abort(); /* registered at start: a refusal now would leave readers without their fence, never continue */
    }
#endif
    for (unsigned slot = 0u; slot < COPY_SLOTS; slot++) {
        while (atomic_load(&g_copy_flag[slot][0]) != 0) {
            sched_yield();
        }
    }
}

void kernel_guest_probe_cache_flush(void)
{
    atomic_fetch_add_explicit(&g_probe_epoch, 1u, memory_order_release);
    if (atomic_load(&g_change_pending) > 0) {
        atomic_fetch_sub(&g_change_pending, 1);
    }
}

typedef enum { COUNT_PROBE_HITS, COUNT_FAST_READS, COUNT_FAST_WRITES } count_field;

/* Typed member access, not a byte offset cast: clang >= 20 rejects the cast (-Werror,-Watomic-alignment, alignment 1). */
static uint64_t counts_sum(count_field field)
{
    uint64_t total = 0u;
    for (unsigned slot = 0u; slot < COPY_SLOTS; slot++) {
        const copy_counts *counts = &g_copy_counts[slot];
        const uint64_t *member = field == COUNT_PROBE_HITS ? &counts->probe_hits
                                 : field == COUNT_FAST_READS ? &counts->fast_reads
                                                             : &counts->fast_writes;
        total += __atomic_load_n(member, __ATOMIC_RELAXED);
    }
    return total;
}

void kernel_guest_copy_stats(uint64_t *fast_reads, uint64_t *fast_writes, uint64_t *syscalls)
{
    if (fast_reads != NULL) *fast_reads = __atomic_load_n(&g_copy_fast_reads, __ATOMIC_RELAXED) + counts_sum(COUNT_FAST_READS);
    if (fast_writes != NULL) *fast_writes = __atomic_load_n(&g_copy_fast_writes, __ATOMIC_RELAXED) + counts_sum(COUNT_FAST_WRITES);
    if (syscalls != NULL) *syscalls = __atomic_load_n(&g_copy_syscalls, __ATOMIC_RELAXED);
}

void kernel_guest_probe_stats(uint64_t *cache_hits, uint64_t *syscalls)
{
    if (cache_hits != NULL) {
        *cache_hits = __atomic_load_n(&g_probe_cache_hits, __ATOMIC_RELAXED) + counts_sum(COUNT_PROBE_HITS);
    }
    if (syscalls != NULL) {
        *syscalls = __atomic_load_n(&g_probe_syscalls, __ATOMIC_RELAXED);
    }
}

void kernel_guest_probe_miss_stats(uint64_t *cold, uint64_t *conflict, uint64_t *stale, uint64_t *epochs)
{
    if (cold != NULL) *cold = __atomic_load_n(&g_probe_miss_cold, __ATOMIC_RELAXED);
    if (conflict != NULL) *conflict = __atomic_load_n(&g_probe_miss_conflict, __ATOMIC_RELAXED);
    if (stale != NULL) *stale = __atomic_load_n(&g_probe_miss_stale, __ATOMIC_RELAXED);
    if (epochs != NULL) *epochs = atomic_load(&g_probe_epoch) - 1u;
}

static bool probe_cached(uintptr_t page, uint64_t epoch)
{
    const unsigned slot = probe_index(page);
    return t_probe_cache[slot].page == page && t_probe_cache[slot].epoch == epoch;
}

static void probe_remember(uintptr_t page, uint64_t epoch)
{
    const unsigned slot = probe_index(page);
    t_probe_cache[slot].page = page;
    t_probe_cache[slot].epoch = epoch;
}

/* Page size is shared by the per-thread probe/copy paths. Initialize it once because the first guest
 * access can arrive concurrently on several host threads. */
static uintptr_t g_host_page_bytes = 4096u;
static pthread_once_t g_host_page_once = PTHREAD_ONCE_INIT;

static void host_page_bytes_init(void)
{
    const long page = sysconf(_SC_PAGESIZE);
    g_host_page_bytes = (page > 0L) ? (uintptr_t)page : 4096u;
}

static uintptr_t host_page_bytes(void)
{
    (void)pthread_once(&g_host_page_once, host_page_bytes_init);
    return g_host_page_bytes;
}

/*
 * Is every page of [addr, addr + length) readable in this process?
 *
 * The host MMU is the guest page table, so this asks the kernel. The probe here used
 * to be msync(MS_ASYNC), which answers "mapped", not "readable": a PROT_NONE guard
 * page IS mapped, so it passed that probe and the caller's dereference still faulted
 * (T81). process_vm_readv on our own pid answers the question that matters. The
 * kernel copies through the page tables honouring protection, fails with EFAULT on
 * an unmapped page AND on a PROT_NONE page, stops at the first unreadable byte, and
 * never raises a signal in this process. It is also already this file's transfer
 * mechanism (kernel_guest_read_bytes/write_bytes, T157), so no second subsystem is
 * introduced. One byte is sampled per page because protection is per-page, batched
 * PROBE_BATCH pages per syscall, so a 4 KB-to-256 KB range costs one call. MEASURED
 * 192 ns per one-page probe on the dev host (2M calls, mapped page), against about
 * 235 kernel calls per boot. The old msync probe measured 53 ns on the same run, so
 * the honest answer costs about 140 ns more per call, below noise at that call rate.
 *
 * RESIDUAL LIMITS, stated rather than hidden. (1) Readable is not writable: a
 * read-only page passes, and a caller that WRITES through the returned pointer can
 * still fault. The guarded write path (kernel_guest_write_bytes) is immune because
 * process_vm_writev itself honours write protection. (2) It is a check, not a lock:
 * another thread can unmap or re-protect the range between the probe and the access
 * (TOCTOU), which is inherent without stopping the world. Both residual faults land
 * in the structured fault handler while guest code is armed: host_runtime.c installs
 * the SIGSEGV/SIGBUS handlers and every guest thread runs under its own sigsetjmp
 * (host_run_arm in src/host/main.c), so the result is a stop record naming the
 * faulting address, not a bare crash. Rounding outward to page edges is exact, not
 * generous, because mapping and protection are whole pages.
 */
bool kernel_guest_range_readable(kernel_guest_ptr addr, size_t length)
{
    if (length == 0u) {
        return true;
    }
    const uintptr_t page = host_page_bytes();
    const uintptr_t start = (uintptr_t)addr & ~(page - 1u);
    const uintptr_t end = ((uintptr_t)addr + length + page - 1u) & ~(page - 1u);
    enum { PROBE_BATCH = 64 };
    const uint64_t epoch = atomic_load_explicit(&g_probe_epoch, memory_order_acquire);
    uint8_t sample[PROBE_BATCH];
    uintptr_t at = start;
    while (at < end) {
        struct iovec remote[PROBE_BATCH];
        uintptr_t pages[PROBE_BATCH];
        unsigned long count = 0u;
        while (at < end && count < PROBE_BATCH) {
            if (!probe_cached(at, epoch)) {
                const unsigned miss_slot = probe_index(at);
                if (t_probe_cache[miss_slot].page == 0u) {
                    __atomic_fetch_add(&g_probe_miss_cold, 1u, __ATOMIC_RELAXED);
                } else if (t_probe_cache[miss_slot].page != at) {
                    __atomic_fetch_add(&g_probe_miss_conflict, 1u, __ATOMIC_RELAXED);
                } else {
                    __atomic_fetch_add(&g_probe_miss_stale, 1u, __ATOMIC_RELAXED);
                }
                pages[count] = at;
                remote[count].iov_base = (void *)at;
                remote[count].iov_len = 1u;
                count++;
            } else {
                __atomic_fetch_add(&g_probe_cache_hits, 1u, __ATOMIC_RELAXED);
            }
            at += page;
        }
        if (count == 0u) {
            continue;
        }
        struct iovec local = {sample, (size_t)count};
        __atomic_fetch_add(&g_probe_syscalls, 1u, __ATOMIC_RELAXED);
        if (process_vm_readv(host_pid(), &local, 1u, remote, count, 0u) != (ssize_t)count) {
            return false;
        }
        for (unsigned long index = 0u; index < count; index++) {
            probe_remember(pages[index], epoch);
        }
    }
    return true;
}

/* True when the bytes were copied without a system call (every page in the probe cache, no page change pending). */
static bool guest_copy_fast(void *out, const void *at, size_t length, bool writing, const void *source)
{
    const int slot = copy_slot();
    if (slot < 0 || length > FAST_COPY_LIMIT_BYTES) {
        return false;
    }
    const uintptr_t page = host_page_bytes();
    const uintptr_t first = (uintptr_t)at & ~(page - 1u);
    const uintptr_t last = ((uintptr_t)at + length - 1u) & ~(page - 1u);
    const uint64_t epoch = atomic_load_explicit(&g_probe_epoch, memory_order_acquire);
    for (uintptr_t cursor = first; cursor <= last; cursor += page) {
        const unsigned index = probe_index(cursor);
        const bool known = writing ? (t_write_cache[index].page == cursor && t_write_cache[index].epoch == epoch)
                                   : probe_cached(cursor, epoch);
        if (!known) {
            return false;
        }
    }
    copy_flag_raise(slot);
    if (atomic_load(&g_change_pending) != 0 || atomic_load_explicit(&g_probe_epoch, memory_order_acquire) != epoch) {
        atomic_store_explicit(&g_copy_flag[slot][0], 0, memory_order_release);
        return false;
    }
    if (writing) {
        memcpy((void *)(uintptr_t)at, source, length);
    } else {
        memcpy(out, at, length);
    }
    atomic_store_explicit(&g_copy_flag[slot][0], 0, memory_order_release);
    return true;
}

static void write_remember(const void *at, size_t length, uint64_t epoch)
{
    const uintptr_t page = host_page_bytes();
    for (uintptr_t cursor = (uintptr_t)at & ~(page - 1u); cursor <= (((uintptr_t)at + length - 1u) & ~(page - 1u));
         cursor += page) {
        const unsigned index = probe_index(cursor);
        t_write_cache[index].page = cursor;
        t_write_cache[index].epoch = epoch;
    }
}

kernel_guest_ptr kernel_guest_add(kernel_guest_ptr base, uint32_t offset)
{
    const uint64_t sum = (uint64_t)base + (uint64_t)offset;
    if (base == 0u || sum >= GUEST_ADDRESS_LIMIT) {
        return 0u;
    }
    return (kernel_guest_ptr)sum;
}

void *kernel_guest_at(kernel_guest_ptr addr, size_t length)
{
    if (addr == 0u) {
        return NULL;
    }
    if ((uint64_t)length > GUEST_ADDRESS_LIMIT - (uint64_t)addr) {
        return NULL;
    }
    if (length != 0u && !kernel_guest_range_readable(addr, length)) {
        return NULL;
    }
    return (void *)(uintptr_t)addr;
}

/* T1289: one pass for an access of 1..16 bytes (a field, a u32 of the device or the push buffer) whose pages are all in this thread's
 * caches. It makes the same checks in the same order as kernel_guest_at + guest_copy_fast (page cache, epoch, change pending, flag),
 * with one epoch load, no function calls, an inlined copy and thread local counters. False: nothing was copied, take the general path. */
static inline __attribute__((always_inline)) bool guest_small_fast(kernel_guest_ptr addr, void *out, const void *source, size_t length, bool writing)
{
    const int slot = t_copy_slot;
    if (slot < 0 || addr == 0u || length == 0u || length > 16u || (uint64_t)length > GUEST_ADDRESS_LIMIT - (uint64_t)addr) {
        return false;
    }
    const uintptr_t page = g_host_page_bytes;
    const uintptr_t first = (uintptr_t)addr & ~(page - 1u);
    const uintptr_t last = ((uintptr_t)addr + length - 1u) & ~(page - 1u);
    const uint64_t epoch = atomic_load_explicit(&g_probe_epoch, memory_order_acquire);
    unsigned pages = 0u;
    for (uintptr_t cursor = first; cursor <= last; cursor += page) {
        const unsigned index = probe_index(cursor);
        if (t_probe_cache[index].page != cursor || t_probe_cache[index].epoch != epoch ||
            (writing && (t_write_cache[index].page != cursor || t_write_cache[index].epoch != epoch))) {
            return false;
        }
        pages++;
    }
    copy_flag_raise(slot);
    if (atomic_load(&g_change_pending) != 0 || atomic_load_explicit(&g_probe_epoch, memory_order_acquire) != epoch) {
        atomic_store_explicit(&g_copy_flag[slot][0], 0, memory_order_release);
        return false;
    }
    const void *from = writing ? source : (const void *)(uintptr_t)addr;
    void *to = writing ? (void *)(uintptr_t)addr : out;
    if (length == 4u) {
        memcpy(to, from, 4u);
    } else if (length == 8u) {
        memcpy(to, from, 8u);
    } else if (length == 1u) {
        memcpy(to, from, 1u);
    } else {
        memcpy(to, from, length);
    }
    atomic_store_explicit(&g_copy_flag[slot][0], 0, memory_order_release);
    copy_counts *counts = &g_copy_counts[slot];
    __atomic_store_n(&counts->probe_hits, counts->probe_hits + pages, __ATOMIC_RELAXED);
    if (writing) {
        __atomic_store_n(&counts->fast_writes, counts->fast_writes + 1u, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&counts->fast_reads, counts->fast_reads + 1u, __ATOMIC_RELAXED);
    }
    return true;
}

bool kernel_guest_read_bytes(kernel_guest_ptr addr, void *out, size_t length)
{
    if (!out || length > (size_t)SSIZE_MAX) return false;
    if (guest_small_fast(addr, out, NULL, length, false)) return true;
    void *at = kernel_guest_at(addr, length);
    if (!at) return false;
    if (length == 0u) return true;
    if (guest_copy_fast(out, at, length, false, NULL)) {
        __atomic_fetch_add(&g_copy_fast_reads, 1u, __ATOMIC_RELAXED);
        return true;
    }
    __atomic_fetch_add(&g_copy_syscalls, 1u, __ATOMIC_RELAXED);
    /* The slow path is a process_vm_readv: a concurrent unmap returns false, never faults (test_kernel_guest_copy). */
    struct iovec local = {out, length}, remote = {at, length};
    return process_vm_readv(host_pid(), &local, 1u, &remote, 1u, 0u) == (ssize_t)length;
}

bool kernel_guest_write_bytes(kernel_guest_ptr addr, const void *source, size_t length)
{
    if (!source || length > (size_t)SSIZE_MAX) return false;
    if (guest_small_fast(addr, NULL, source, length, true)) return true;
    void *at = kernel_guest_at(addr, length);
    if (!at) return false;
    if (length == 0u) return true;
    if (guest_copy_fast(NULL, at, length, true, source)) {
        __atomic_fetch_add(&g_copy_fast_writes, 1u, __ATOMIC_RELAXED);
        return true;
    }
    __atomic_fetch_add(&g_copy_syscalls, 1u, __ATOMIC_RELAXED);
    const uint64_t epoch = atomic_load_explicit(&g_probe_epoch, memory_order_acquire);
    struct iovec local = {(void *)source, length}, remote = {at, length};
    if (process_vm_writev(host_pid(), &local, 1u, &remote, 1u, 0u) != (ssize_t)length) return false;
    write_remember(at, length, epoch);
    return true;
}

/* T1289: the scalar accessors enter the one pass fast path directly (one call less per access, the length is a constant so the copy inlines). */
bool kernel_guest_read_u32(kernel_guest_ptr addr, uint32_t *out)
{
    uint32_t value;
    if (!out) return false;
    if (!guest_small_fast(addr, &value, NULL, sizeof(value), false) && !kernel_guest_read_bytes(addr, &value, sizeof(value))) return false;
    *out = value;
    return true;
}

bool kernel_guest_write_u32(kernel_guest_ptr addr, uint32_t value)
{
    return guest_small_fast(addr, NULL, &value, sizeof(value), true) || kernel_guest_write_bytes(addr, &value, sizeof(value));
}

bool kernel_guest_read_u8(kernel_guest_ptr addr, uint8_t *out)
{
    uint8_t value;
    if (!out) return false;
    if (!guest_small_fast(addr, &value, NULL, sizeof(value), false) && !kernel_guest_read_bytes(addr, &value, sizeof(value))) return false;
    *out = value;
    return true;
}

bool kernel_guest_write_u8(kernel_guest_ptr addr, uint8_t value)
{
    return guest_small_fast(addr, NULL, &value, sizeof(value), true) || kernel_guest_write_bytes(addr, &value, sizeof(value));
}

bool kernel_frame_arg(const kernel_call_frame *frame, unsigned index, uint32_t *out)
{
    if (!frame || !out || frame->stack_ptr == 0u) {
        return false;
    }

    /* Argument N sits one slot past the return address. Computed in 64 bits so
     * that a large index cannot wrap into a plausible-looking address. */
    uint64_t slot = (uint64_t)frame->stack_ptr + sizeof(uint32_t) +
                    (uint64_t)index * sizeof(uint32_t);
    if (slot + sizeof(uint32_t) > GUEST_ADDRESS_LIMIT) {
        return false;
    }
    if (frame->stack_limit != 0u &&
        slot + sizeof(uint32_t) > (uint64_t)frame->stack_limit) {
        return false;
    }
    return kernel_guest_read_u32((kernel_guest_ptr)slot, out);
}

bool kernel_frame_build(kernel_call_frame *frame, kernel_guest_ptr buffer,
                        uint32_t buffer_bytes, const uint32_t *args, unsigned arg_count)
{
    if (!frame || buffer == 0u) {
        return false;
    }
    if (arg_count > 0u && !args) {
        return false;
    }

    uint64_t needed = ((uint64_t)arg_count + 1u) * sizeof(uint32_t);
    if (needed > (uint64_t)buffer_bytes) {
        return false;
    }
    if ((uint64_t)buffer + needed > GUEST_ADDRESS_LIMIT) {
        return false;
    }

    /* Return address slot. Zero because nothing returns through it: a recompiled
     * guest calls the handler directly. */
    if (!kernel_guest_write_u32(buffer, 0u)) {
        return false;
    }
    for (unsigned i = 0; i < arg_count; i++) {
        kernel_guest_ptr slot = (kernel_guest_ptr)(buffer + (i + 1u) * sizeof(uint32_t));
        if (!kernel_guest_write_u32(slot, args[i])) {
            return false;
        }
    }

    frame->result_high = 0u;
    frame->has_result_high = false;
    frame->stack_ptr = buffer;
    frame->stack_limit = (kernel_guest_ptr)(buffer + buffer_bytes);
    return true;
}

bool kernel_frame_reg_arg(const kernel_call_frame *frame, unsigned index, uint32_t *out)
{
    if (!frame || !out || !frame->has_registers || index > 1u) {
        return false;
    }
    *out = (index == 0u) ? frame->ecx : frame->edx;
    return true;
}

void kernel_frame_set_registers(kernel_call_frame *frame, uint32_t ecx, uint32_t edx)
{
    if (!frame) {
        return;
    }
    frame->ecx = ecx;
    frame->edx = edx;
    frame->has_registers = true;
}
