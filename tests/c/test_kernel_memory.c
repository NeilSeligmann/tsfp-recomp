/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the memory-management and heap group of the kernel HLE.
 *
 * The load-bearing assertion in here is that every address handed to the guest is
 * really below 4 GB and really backed by mapped pages at that exact address. A
 * truncated pointer would corrupt the guest arbitrarily far from the allocation
 * that produced it, so it is checked with msync() rather than by dereferencing:
 * msync reports an unmapped range as ENOMEM, which fails the test cleanly, whereas
 * a dereference of a truncated pointer would take the whole process down and prove
 * only that something went wrong somewhere.
 */

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_memory.h"
#include "nt_status.h"

#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                     \
            /* Flushed immediately: a bad guest address tends to take the process    \
             * down a few lines later, and a buffered diagnostic lost to a SIGSEGV   \
             * turns a precise failure into a bare core dump. */                     \
            fflush(stdout);                                                            \
        }                                                                                \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t check_a = (uint32_t)(actual);                                           \
        uint32_t check_e = (uint32_t)(expected);                                         \
        if (check_a != check_e) {                                                        \
            failures++;                                                                  \
            printf("  FAIL %s:%d  %s == %s (got %#x, want %#x)\n", __FILE__, __LINE__,   \
                   #actual, #expected, check_a, check_e);                                \
            fflush(stdout);                                                              \
        }                                                                                \
    } while (0)

/* Diagnostics capture, matching test_kernel_hle.c's approach. */
static char captured[8192];
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

/* --------------------------------------------------------------------------
 * Helpers.
 * ----------------------------------------------------------------------- */

/* Scratch guest memory for stdcall frames and out-parameters. Frames live at the
 * start, out-parameters from SCRATCH_OUT on, so one region serves both. */
#define SCRATCH_BYTES 0x1000u
#define SCRATCH_OUT 0x200u

static kernel_guest_ptr scratch;

static bool range_is_mapped(kernel_guest_ptr addr, uint32_t length);

static void setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    kernel_memory_register();
    kernel_hle_set_log(capture_printer);
    reset_capture();

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
        printf("  FATAL: could not allocate test scratch memory (status %#x)\n", status);
        exit(EXIT_FAILURE);
    }
    /* Verified without dereferencing it. Every later test writes guest structs
     * through this address, so if the allocator ever hands back something it cannot
     * really back, say so here rather than core-dumping inside whatever used it. */
    if (!range_is_mapped(scratch, SCRATCH_BYTES)) {
        printf("  FATAL: scratch at %#x is not backed by mapped pages -- the "
               "allocator returned an address the guest cannot use\n",
               scratch);
        fflush(stdout);
        exit(EXIT_FAILURE);
    }
}

static void teardown(void)
{
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
}

/* Call an ordinal through the dispatcher with stdcall arguments on a guest stack. */
static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, SCRATCH_OUT, args, count)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    return kernel_hle_call(ordinal, &frame);
}

static kernel_guest_ptr out_slot(unsigned index)
{
    return (kernel_guest_ptr)(scratch + SCRATCH_OUT + index * sizeof(uint32_t));
}

/**
 * A page-aligned guest address that lies inside no tracked region.
 *
 * Hardcoding a literal here would be unsound, and was: the allocator's addresses
 * come from mmap and move with ASLR, so a constant that is unallocated on one run
 * can sit inside a live region on the next. The test then fails for a reason that
 * has nothing to do with what it is checking, which is worse than not testing it.
 */
static kernel_guest_ptr unallocated_address(void)
{
    for (uint32_t candidate = 0x10000000u; candidate < 0xF0000000u;
         candidate += 0x1000000u) {
        if (guest_region_containing(candidate) == NULL) {
            return candidate;
        }
    }
    printf("  FATAL: every candidate address is allocated\n");
    exit(EXIT_FAILURE);
}

/**
 * True when [addr, addr+length) is mapped in this process at exactly that address.
 *
 * This is the real test of the 4 GB rule. msync() on an unmapped range fails with
 * ENOMEM, so a guest address that was truncated down from above 4 GB is caught
 * deterministically instead of faulting.
 */
static bool range_is_mapped(kernel_guest_ptr addr, uint32_t length)
{
    if (addr == 0u || length == 0u) {
        return false;
    }
    if ((uint64_t)addr + (uint64_t)length > 0x100000000ULL) {
        return false;
    }
    /* msync needs a page-aligned start, so round down and extend to cover. */
    uint32_t start = addr & ~(GUEST_PAGE_SIZE - 1u);
    size_t span = (size_t)(addr - start) + (size_t)length;
    errno = 0;
    if (msync((void *)(uintptr_t)start, span, MS_ASYNC) == 0) {
        return true;
    }
    return errno != ENOMEM;
}

static uint32_t alloc_contiguous(uint32_t bytes)
{
    uint32_t args[1] = {bytes};
    return call_ordinal(ORD_MmAllocateContiguousMemory, args, 1u);
}

static uint32_t alloc_contiguous_ex(uint32_t bytes, uint32_t lowest, uint32_t highest,
                                    uint32_t alignment, uint32_t protect)
{
    uint32_t args[5] = {bytes, lowest, highest, alignment, protect};
    return call_ordinal(ORD_MmAllocateContiguousMemoryEx, args, 5u);
}

static uint32_t free_contiguous(uint32_t base)
{
    uint32_t args[1] = {base};
    return call_ordinal(ORD_MmFreeContiguousMemory, args, 1u);
}

static uint32_t query_allocation_size(uint32_t base)
{
    uint32_t args[1] = {base};
    return call_ordinal(ORD_MmQueryAllocationSize, args, 1u);
}

static uint32_t physical_address(uint32_t addr)
{
    uint32_t args[1] = {addr};
    return call_ordinal(ORD_MmGetPhysicalAddress, args, 1u);
}

/* Allocate virtual memory through the ordinal, reporting base and size out-params. */
static nt_status nt_allocate(uint32_t want_base, uint32_t want_size, uint32_t type,
                             uint32_t protect, uint32_t *got_base, uint32_t *got_size)
{
    kernel_guest_ptr base_ptr = out_slot(0);
    kernel_guest_ptr size_ptr = out_slot(1);
    kernel_guest_write_u32(base_ptr, want_base);
    kernel_guest_write_u32(size_ptr, want_size);

    uint32_t args[5] = {base_ptr, 0u, size_ptr, type, protect};
    nt_status status = call_ordinal(ORD_NtAllocateVirtualMemory, args, 5u);
    if (got_base) {
        kernel_guest_read_u32(base_ptr, got_base);
    }
    if (got_size) {
        kernel_guest_read_u32(size_ptr, got_size);
    }
    return status;
}

static nt_status nt_free(uint32_t base, uint32_t size, uint32_t type)
{
    kernel_guest_ptr base_ptr = out_slot(2);
    kernel_guest_ptr size_ptr = out_slot(3);
    kernel_guest_write_u32(base_ptr, base);
    kernel_guest_write_u32(size_ptr, size);
    uint32_t args[3] = {base_ptr, size_ptr, type};
    return call_ordinal(ORD_NtFreeVirtualMemory, args, 3u);
}

/* --------------------------------------------------------------------------
 * The 4 GB rule.
 * ----------------------------------------------------------------------- */

/**
 * The low-memory invariant, checked before anything dereferences an address.
 *
 * Runs first and touches no guest memory: a returned address is validated with
 * msync, never by writing through it. That ordering matters, because a truncated
 * pointer makes the first write anywhere take the process down, and a core dump
 * says only "something broke" where this says which promise was broken.
 */
static void test_allocator_places_memory_where_the_guest_can_reach_it(void)
{
    kernel_hle_init();
    guest_mem_reset();
    kernel_hle_set_log(capture_printer);
    reset_capture();

    for (int i = 0; i < 16; i++) {
        guest_region_request request = {
            .bytes = 0x4000u,
            .alignment = 0u,
            .lowest_physical = 0u,
            .highest_physical = 0u,
            .protect = PAGE_READWRITE,
            .state = MEM_COMMIT,
            .contiguous = true,
            .fixed_base = 0u,
        };
        nt_status status = STATUS_UNSUCCESSFUL;
        kernel_guest_ptr base = guest_region_alloc(&request, &status);
        CHECK_EQ_U32(status, STATUS_SUCCESS);
        CHECK(base != 0u);
        /* The whole range, not just the base, has to fit under 4 GB: the guest
         * computes addresses inside the allocation too. */
        CHECK((uint64_t)base + 0x4000u <= 0x100000000ULL);
        CHECK(range_is_mapped(base, 0x4000u));
    }

    guest_mem_reset();
    kernel_hle_set_log(NULL);
}

static void test_every_allocation_lands_below_4gb(void)
{
    setup();

    /* Enough allocations, of varied sizes, that a host allocator inclined to drift
     * above 4 GB would have done so. */
    static const uint32_t sizes[] = {1u,      16u,      0x1000u,  0x1001u,
                                     0x10000u, 0x30000u, 0x100000u};
    uint32_t bases[sizeof(sizes) / sizeof(sizes[0])] = {0};

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (int repeat = 0; repeat < 4; repeat++) {
            uint32_t base = alloc_contiguous(sizes[i]);
            CHECK(base != 0u);
            if (base == 0u) {
                continue;
            }
            /* Below 2^32 by construction of the type, so the assertion that bites is
             * that the range does not run off the end of the guest space and that it
             * is genuinely mapped here. */
            CHECK((uint64_t)base + sizes[i] <= 0x100000000ULL);
            CHECK(range_is_mapped(base, sizes[i]));
            /* A host pointer derived from the guest address must be the same
             * address: that is what identity mapping means. */
            CHECK((uintptr_t)kernel_guest_at(base, sizes[i]) == (uintptr_t)base);
            if (repeat == 0) {
                bases[i] = base;
            }
        }
    }

    /* The memory must actually be usable, not merely mapped. */
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (bases[i] == 0u) {
            continue;
        }
        unsigned char *host = kernel_guest_at(bases[i], sizes[i]);
        CHECK(host != NULL);
        if (!host) {
            continue;
        }
        memset(host, 0x5A, sizes[i]);
        CHECK(host[0] == 0x5A);
        CHECK(host[sizes[i] - 1u] == 0x5A);
    }

    teardown();
}

static void test_virtual_allocations_land_below_4gb(void)
{
    setup();

    for (int i = 0; i < 8; i++) {
        uint32_t base = 0;
        uint32_t size = 0;
        nt_status status = nt_allocate(0u, 0x20000u, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_READWRITE, &base, &size);
        CHECK_EQ_U32(status, STATUS_SUCCESS);
        CHECK(base != 0u);
        CHECK((uint64_t)base + size <= 0x100000000ULL);
        CHECK(range_is_mapped(base, size));
    }

    teardown();
}

static void test_heap_blocks_land_below_4gb(void)
{
    setup();

    uint32_t heap = guest_heap_create(0u, 0x10000u, 0u);
    CHECK(heap != 0u);
    for (uint32_t bytes = 1u; bytes <= 0x4000u; bytes *= 3u) {
        kernel_guest_ptr block = guest_heap_alloc(heap, bytes);
        CHECK(block != 0u);
        CHECK((uint64_t)block + bytes <= 0x100000000ULL);
        CHECK(range_is_mapped(block, bytes));
    }
    CHECK(guest_heap_destroy(heap));

    teardown();
}

/* --------------------------------------------------------------------------
 * Alignment and page granularity.
 * ----------------------------------------------------------------------- */

static void test_contiguous_memory_is_page_aligned(void)
{
    setup();

    for (uint32_t bytes = 1u; bytes <= 0x8000u; bytes *= 2u) {
        uint32_t base = alloc_contiguous(bytes);
        CHECK(base != 0u);
        /* GPU resources need page-aligned backing, so a merely malloc-aligned
         * address is wrong even though it would "work" for CPU access. */
        CHECK((base % GUEST_PAGE_SIZE) == 0u);
    }

    teardown();
}

static void test_contiguous_ex_honours_alignment(void)
{
    setup();

    static const uint32_t alignments[] = {0x1000u, 0x2000u, 0x10000u, 0x20000u, 0x100000u};
    for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); i++) {
        uint32_t base = alloc_contiguous_ex(0x3000u, 0u, 0u, alignments[i], PAGE_READWRITE);
        CHECK(base != 0u);
        CHECK_EQ_U32(base % alignments[i], 0u);
        CHECK(range_is_mapped(base, 0x3000u));
    }

    /* Alignment 0 means page alignment, matching the plain variant. */
    uint32_t defaulted = alloc_contiguous_ex(0x100u, 0u, 0u, 0u, PAGE_READWRITE);
    CHECK(defaulted != 0u);
    CHECK_EQ_U32(defaulted % GUEST_PAGE_SIZE, 0u);

    /* A non-power-of-two alignment is a caller bug and must be refused, not
     * rounded into something plausible. */
    reset_capture();
    CHECK_EQ_U32(alloc_contiguous_ex(0x1000u, 0u, 0u, 0x3000u, PAGE_READWRITE), 0u);
    CHECK(captured_contains("not a power of two"));

    teardown();
}

static void test_reserve_uses_64k_granularity(void)
{
    setup();

    /* The NT/Xbox ABI values pinned ONCE as literals: 4 KiB pages, 64 KiB reserve
     * granularity. Every rounding assertion in this suite reads through the macros,
     * so a mutated constant would otherwise move the code and the expectations
     * together (the title computes with these numbers, so they are not ours to
     * choose). */
    CHECK_EQ_U32(GUEST_PAGE_SIZE, 0x1000u);
    CHECK_EQ_U32(GUEST_ALLOCATION_GRANULARITY, 0x10000u);

    uint32_t base = 0;
    uint32_t size = 0;
    nt_status status = nt_allocate(0u, 0x1000u, MEM_RESERVE, PAGE_READWRITE, &base, &size);
    CHECK_EQ_U32(status, STATUS_SUCCESS);
    /* A reserve rounds to 64 KB on the real hardware, and the guest reads the
     * rounded size back out of its own argument. */
    CHECK_EQ_U32(size, GUEST_ALLOCATION_GRANULARITY);
    CHECK_EQ_U32(base % GUEST_ALLOCATION_GRANULARITY, 0u);

    /* A bare commit works in pages, not 64 KB units. */
    uint32_t commit_base = 0;
    uint32_t commit_size = 0;
    status = nt_allocate(0u, 0x1000u, MEM_COMMIT, PAGE_READWRITE, &commit_base,
                         &commit_size);
    CHECK_EQ_U32(status, STATUS_SUCCESS);
    CHECK_EQ_U32(commit_size, GUEST_PAGE_SIZE);

    teardown();
}

/* --------------------------------------------------------------------------
 * Size tracking.
 * ----------------------------------------------------------------------- */

static void test_allocation_size_round_trip(void)
{
    setup();

    struct {
        uint32_t asked;
        uint32_t expect;
    } cases[] = {
        {1u, GUEST_PAGE_SIZE},
        {GUEST_PAGE_SIZE, GUEST_PAGE_SIZE},
        {GUEST_PAGE_SIZE + 1u, 2u * GUEST_PAGE_SIZE},
        {0x4321u, 0x5000u},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t base = alloc_contiguous(cases[i].asked);
        CHECK(base != 0u);
        /* The answer comes from a record of the allocation, so it must be exact
         * rather than a plausible rounding of whatever the caller passed back in. */
        CHECK_EQ_U32(query_allocation_size(base), cases[i].expect);
    }

    /* An address that is not an allocation base answers 0, as the real call does. */
    uint32_t base = alloc_contiguous(0x2000u);
    CHECK(base != 0u);
    CHECK_EQ_U32(query_allocation_size(base + GUEST_PAGE_SIZE), 0u);
    CHECK_EQ_U32(query_allocation_size(unallocated_address()), 0u);

    /* And after a free the size is no longer answerable. */
    free_contiguous(base);
    CHECK_EQ_U32(query_allocation_size(base), 0u);

    teardown();
}

typedef struct size_query_worker {
    uint32_t heap, block, size;
    bool success;
} size_query_worker;
static void *query_heap_size_worker(void *opaque)
{
    size_query_worker *worker=opaque;
    worker->success=guest_heap_block_size(worker->heap,worker->block,&worker->size);
    return NULL;
}
static void test_heap_size_guarded_header(void)
{
    setup();
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    CHECK(page>=64u && page<=UINT32_MAX);
    uint32_t heap=guest_heap_create(0u,0x20000u,0u);
    uint32_t other=guest_heap_create(0u,0x20000u,0u);
    CHECK(heap!=0u && other!=0u);
    uint32_t padding=guest_heap_alloc(heap,(uint32_t)page-32u);
    uint32_t block=guest_heap_alloc(heap,44u);
    uint32_t other_block=guest_heap_alloc(other,24u);
    CHECK(padding!=0u && block!=0u && other_block!=0u);
    CHECK((block % page)==0u);
    if (!padding || !block || !other_block || block%page!=0u) { teardown(); return; }
    uint8_t *header_page=(uint8_t *)(uintptr_t)(block-(uint32_t)page);
    uint8_t *saved=malloc(page);
    CHECK(saved!=NULL); if (!saved) { teardown(); return; }
    memcpy(saved,header_page,page);
    CHECK(kernel_guest_write_u32(block,0x12345678u));
    CHECK(mprotect(header_page,page,PROT_NONE)==0);
    uint32_t size=0xDEADBEEFu, word=0u;
    CHECK(kernel_guest_read_u32(block,&word) && word==0x12345678u);
    CHECK(!guest_heap_block_size(heap,block,&size)); CHECK_EQ_U32(size,0xDEADBEEFu);
    /* A query from another thread would hang if the refusal leaked the heap lock. */
    size_query_worker worker={other,other_block,0u,false}; pthread_t thread;
    int started=pthread_create(&thread,NULL,query_heap_size_worker,&worker);
    CHECK(started==0);
    if (started==0) {
        CHECK(pthread_join(thread,NULL)==0);
        CHECK(worker.success); CHECK_EQ_U32(worker.size,24u);
    }
    CHECK(mprotect(header_page,page,PROT_READ)==0);
    CHECK(guest_heap_block_size(heap,block,&size)); CHECK_EQ_U32(size,44u);
    CHECK(munmap(header_page,page)==0);
    size=0xDEADBEEFu;
    CHECK(kernel_guest_read_u32(block,&word));
    CHECK(!guest_heap_block_size(heap,block,&size)); CHECK_EQ_U32(size,0xDEADBEEFu);
    worker.success=false; worker.size=0u;
    started=pthread_create(&thread,NULL,query_heap_size_worker,&worker);
    CHECK(started==0);
    if (started==0) {
        CHECK(pthread_join(thread,NULL)==0);
        CHECK(worker.success); CHECK_EQ_U32(worker.size,24u);
    }
    void *restored=mmap(header_page,page,PROT_READ|PROT_WRITE,
                        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    CHECK(restored==header_page);
    if (restored==header_page) memcpy(restored,saved,page);
    free(saved);
    CHECK(guest_heap_block_size(heap,block,&size)); CHECK_EQ_U32(size,44u);
    teardown();
}

static void test_heap_size_round_trip(void)
{
    setup();

    uint32_t heap = guest_heap_create(0u, 0x20000u, 0u);
    CHECK(heap != 0u);

    static const uint32_t sizes[] = {1u, 7u, 16u, 17u, 100u, 4095u, 4096u, 9001u};
    kernel_guest_ptr blocks[sizeof(sizes) / sizeof(sizes[0])] = {0};

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        blocks[i] = guest_heap_alloc(heap, sizes[i]);
        CHECK(blocks[i] != 0u);
        /* Payload alignment is a promise, not an accident of the sizes used. */
        CHECK_EQ_U32(blocks[i] % GUEST_HEAP_ALIGNMENT, 0u);
    }

    /* Round-trip after every block exists, so a later allocation overwriting an
     * earlier block's header would be caught. */
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint32_t reported = 0;
        CHECK(guest_heap_block_size(heap, blocks[i], &reported));
        CHECK_EQ_U32(reported, sizes[i]);
    }

    /* Blocks must not overlap: write a per-block pattern, then verify all of them. */
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        unsigned char *host = kernel_guest_at(blocks[i], sizes[i]);
        CHECK(host != NULL);
        if (host) {
            memset(host, (int)(0x10u + i), sizes[i]);
        }
    }
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const unsigned char *host = kernel_guest_at(blocks[i], sizes[i]);
        CHECK(host != NULL);
        if (host) {
            CHECK(host[0] == (unsigned char)(0x10u + i));
            CHECK(host[sizes[i] - 1u] == (unsigned char)(0x10u + i));
        }
    }

    /* A freed block has no size. */
    CHECK(guest_heap_free(heap, blocks[0]));
    uint32_t stale = 0xFFFFFFFFu;
    CHECK(!guest_heap_block_size(heap, blocks[0], &stale));

    CHECK(guest_heap_destroy(heap));
    teardown();
}

/* --------------------------------------------------------------------------
 * Reuse without aliasing.
 * ----------------------------------------------------------------------- */

static void test_freed_region_is_not_reachable_and_does_not_alias(void)
{
    setup();

    uint32_t first = alloc_contiguous(0x4000u);
    CHECK(first != 0u);
    uint32_t first_physical = physical_address(first);
    CHECK(first_physical != 0u);

    free_contiguous(first);
    /* The record is gone, so the old base answers nothing. */
    CHECK(guest_region_at(first) == NULL);
    CHECK_EQ_U32(query_allocation_size(first), 0u);
    CHECK_EQ_U32(physical_address(first), 0u);

    /* A fresh allocation may legitimately reuse the virtual address, but it must
     * never reuse the physical one: two live allocations sharing a physical address
     * would make MmGetPhysicalAddress say two buffers are the same memory. */
    uint32_t second = alloc_contiguous(0x4000u);
    CHECK(second != 0u);
    uint32_t second_physical = physical_address(second);
    CHECK(second_physical != 0u);
    CHECK(second_physical != first_physical);

    teardown();
}

static void test_destroyed_heap_handle_does_not_alias(void)
{
    setup();

    uint32_t first = guest_heap_create(0u, 0x10000u, 0u);
    CHECK(first != 0u);
    CHECK(guest_heap_valid(first));
    kernel_guest_ptr block = guest_heap_alloc(first, 64u);
    CHECK(block != 0u);

    CHECK(guest_heap_destroy(first));
    CHECK(!guest_heap_valid(first));
    /* Destroying twice must fail rather than tear down whatever now occupies the
     * slot. */
    CHECK(!guest_heap_destroy(first));

    uint32_t second = guest_heap_create(0u, 0x10000u, 0u);
    CHECK(second != 0u);
    CHECK(guest_heap_valid(second));
    /* The whole point of the generation in the handle: the slot is reused, the
     * handle value is not. */
    CHECK(first != second);

    /* A stale handle must not operate on the new heap. */
    CHECK_EQ_U32(guest_heap_alloc(first, 64u), 0u);
    CHECK(!guest_heap_free(first, block));
    uint32_t size = 0;
    CHECK(!guest_heap_block_size(first, block, &size));

    CHECK(guest_heap_destroy(second));
    teardown();
}

static void test_heap_reuses_freed_blocks_without_overlap(void)
{
    setup();

    uint32_t heap = guest_heap_create(0u, 0x10000u, 0u);
    CHECK(heap != 0u);

    kernel_guest_ptr a = guest_heap_alloc(heap, 256u);
    kernel_guest_ptr b = guest_heap_alloc(heap, 256u);
    kernel_guest_ptr c = guest_heap_alloc(heap, 256u);
    CHECK(a != 0u && b != 0u && c != 0u);
    CHECK(a != b && b != c && a != c);

    /* Free the middle block and take it again: the allocator must reuse the hole
     * rather than grow, and must not hand back a block that overlaps its
     * neighbours. */
    CHECK(guest_heap_free(heap, b));
    kernel_guest_ptr again = guest_heap_alloc(heap, 256u);
    CHECK_EQ_U32(again, b);

    unsigned char *host_a = kernel_guest_at(a, 256u);
    unsigned char *host_b = kernel_guest_at(again, 256u);
    unsigned char *host_c = kernel_guest_at(c, 256u);
    CHECK(host_a && host_b && host_c);
    if (host_a && host_b && host_c) {
        memset(host_a, 0xAA, 256u);
        memset(host_b, 0xBB, 256u);
        memset(host_c, 0xCC, 256u);
        CHECK(host_a[0] == 0xAA && host_a[255] == 0xAA);
        CHECK(host_b[0] == 0xBB && host_b[255] == 0xBB);
        CHECK(host_c[0] == 0xCC && host_c[255] == 0xCC);
    }

    /* Coalescing FORWARD: free the later block first, so that freeing the earlier
     * one has to merge with the free block that follows it. Tested separately from
     * the backward case below because an allocator can get one right and the other
     * wrong, and either way the arena quietly fragments until a large allocation
     * fails for no visible reason. */
    CHECK(guest_heap_free(heap, again));
    CHECK(guest_heap_free(heap, a));
    kernel_guest_ptr merged_forward = guest_heap_alloc(heap, 500u);
    CHECK(merged_forward != 0u);
    CHECK_EQ_U32(merged_forward, a);
    CHECK(guest_heap_free(heap, merged_forward));

    /* Coalescing BACKWARD: free in address order, so each free has to merge with
     * the free block preceding it. */
    kernel_guest_ptr d = guest_heap_alloc(heap, 256u);
    kernel_guest_ptr e = guest_heap_alloc(heap, 256u);
    CHECK(d != 0u && e != 0u);
    CHECK(guest_heap_free(heap, d));
    CHECK(guest_heap_free(heap, e));
    CHECK(guest_heap_free(heap, c));
    kernel_guest_ptr big = guest_heap_alloc(heap, 700u);
    CHECK(big != 0u);
    CHECK_EQ_U32(big, d);

    CHECK(guest_heap_destroy(heap));
    teardown();
}

static void test_double_free_is_refused_and_reported(void)
{
    setup();

    uint32_t heap = guest_heap_create(0u, 0x10000u, 0u);
    CHECK(heap != 0u);
    kernel_guest_ptr block = guest_heap_alloc(heap, 128u);
    CHECK(block != 0u);
    CHECK(guest_heap_free(heap, block));

    reset_capture();
    CHECK(!guest_heap_free(heap, block));
    CHECK(captured_contains("double free"));

    CHECK(guest_heap_destroy(heap));
    teardown();
}

/* --------------------------------------------------------------------------
 * Heap isolation and teardown.
 * ----------------------------------------------------------------------- */

static void test_heaps_are_isolated(void)
{
    setup();

    uint32_t a = guest_heap_create(0u, 0x10000u, 0u);
    uint32_t b = guest_heap_create(0u, 0x10000u, 0u);
    CHECK(a != 0u && b != 0u);
    CHECK(a != b);

    kernel_guest_ptr from_a = guest_heap_alloc(a, 200u);
    kernel_guest_ptr from_b = guest_heap_alloc(b, 200u);
    CHECK(from_a != 0u && from_b != 0u);
    CHECK(from_a != from_b);

    /* A block is valid only in the heap it came from. Accepting a cross-heap free
     * would corrupt the other heap's free list, and the symptom would appear in an
     * unrelated allocation later. */
    CHECK(!guest_heap_free(b, from_a));
    CHECK(!guest_heap_free(a, from_b));

    uint32_t size = 0;
    CHECK(!guest_heap_block_size(b, from_a, &size));
    CHECK(!guest_heap_block_size(a, from_b, &size));

    /* And the correct heap still accepts them, so the rejection above was about
     * ownership rather than the pointers being broken. */
    CHECK(guest_heap_block_size(a, from_a, &size));
    CHECK_EQ_U32(size, 200u);
    CHECK(guest_heap_block_size(b, from_b, &size));
    CHECK_EQ_U32(size, 200u);
    CHECK(guest_heap_free(a, from_a));
    CHECK(guest_heap_free(b, from_b));

    CHECK(guest_heap_destroy(a));
    CHECK(guest_heap_destroy(b));
    teardown();
}

static void test_destroying_a_heap_releases_everything(void)
{
    setup();

    uint64_t before = guest_mem_mapped_bytes();
    size_t heaps_before = guest_mem_heap_count();

    uint32_t heap = guest_heap_create(0u, 0x10000u, 0u);
    CHECK(heap != 0u);
    CHECK(guest_mem_heap_count() == heaps_before + 1u);

    /* Force the heap to take several chunks, so the test proves every chunk is
     * released rather than just the first. */
    for (int i = 0; i < 64; i++) {
        CHECK(guest_heap_alloc(heap, 0x4000u) != 0u);
    }
    CHECK(guest_mem_mapped_bytes() > before);

    CHECK(guest_heap_destroy(heap));
    CHECK(guest_mem_heap_count() == heaps_before);
    /* Every page the heap took must be handed back, not merely forgotten. */
    CHECK(guest_mem_mapped_bytes() == before);

    teardown();
}

static void test_reset_releases_every_region_and_heap(void)
{
    setup();

    for (int i = 0; i < 8; i++) {
        CHECK(alloc_contiguous(0x2000u) != 0u);
    }
    uint32_t heap = guest_heap_create(0u, 0x10000u, 0u);
    CHECK(heap != 0u);
    CHECK(guest_heap_alloc(heap, 512u) != 0u);

    CHECK(guest_mem_region_count() > 0u);
    CHECK(guest_mem_heap_count() > 0u);

    guest_mem_reset();
    CHECK_EQ_U32((uint32_t)guest_mem_region_count(), 0u);
    CHECK_EQ_U32((uint32_t)guest_mem_heap_count(), 0u);
    CHECK(guest_mem_mapped_bytes() == 0u);
    CHECK(!guest_heap_valid(heap));

    /* Reset invalidated the scratch region, so do not use teardown()'s helpers. */
    kernel_hle_set_log(NULL);
    scratch = 0u;
}

static void test_heap_respects_its_maximum_size(void)
{
    setup();

    /* A heap capped below what the caller then asks for must fail the allocation
     * rather than quietly growing past its own limit. */
    uint32_t heap = guest_heap_create(0u, 0x10000u, 0x10000u);
    CHECK(heap != 0u);
    CHECK(guest_heap_alloc(heap, 0x8000u) != 0u);
    /* The first chunk is full; growing would exceed the maximum. */
    CHECK_EQ_U32(guest_heap_alloc(heap, 0x20000u), 0u);
    CHECK(guest_heap_destroy(heap));

    /* An initial size above the maximum is contradictory. */
    CHECK_EQ_U32(guest_heap_create(0u, 0x20000u, 0x10000u), 0u);

    teardown();
}

static void test_heap_realloc_preserves_contents(void)
{
    setup();

    uint32_t heap = guest_heap_create(0u, 0x10000u, 0u);
    CHECK(heap != 0u);
    kernel_guest_ptr block = guest_heap_alloc(heap, 64u);
    CHECK(block != 0u);
    unsigned char *host = kernel_guest_at(block, 64u);
    CHECK(host != NULL);
    if (host) {
        for (unsigned i = 0; i < 64u; i++) {
            host[i] = (unsigned char)(i + 1u);
        }
    }

    kernel_guest_ptr grown = guest_heap_realloc(heap, block, 4096u);
    CHECK(grown != 0u);
    uint32_t size = 0;
    CHECK(guest_heap_block_size(heap, grown, &size));
    CHECK_EQ_U32(size, 4096u);

    const unsigned char *moved = kernel_guest_at(grown, 64u);
    CHECK(moved != NULL);
    if (moved) {
        bool preserved = true;
        for (unsigned i = 0; i < 64u; i++) {
            if (moved[i] != (unsigned char)(i + 1u)) {
                preserved = false;
            }
        }
        CHECK(preserved);
    }

    CHECK(guest_heap_destroy(heap));
    teardown();
}

/* --------------------------------------------------------------------------
 * Physical addresses, persistence, protection.
 * ----------------------------------------------------------------------- */

static void test_physical_addresses_are_distinct_and_linear(void)
{
    setup();

    uint32_t first = alloc_contiguous(0x4000u);
    uint32_t second = alloc_contiguous(0x4000u);
    CHECK(first != 0u && second != 0u);

    uint32_t physical_first = physical_address(first);
    uint32_t physical_second = physical_address(second);
    CHECK(physical_first != 0u);
    CHECK(physical_second != 0u);
    CHECK(physical_first != physical_second);
    CHECK_EQ_U32(physical_first % GUEST_PAGE_SIZE, 0u);

    /* Ranges must not overlap, or guest code checking whether two buffers are
     * adjacent would get a wrong answer. */
    bool overlapping = physical_first < physical_second + 0x4000u &&
                       physical_second < physical_first + 0x4000u;
    CHECK(!overlapping);

    /* Linear inside a region: addr + n maps to physical + n. */
    CHECK_EQ_U32(physical_address(first + 0x1000u), physical_first + 0x1000u);
    CHECK_EQ_U32(physical_address(first + 0x3FFFu), physical_first + 0x3FFFu);
    /* One byte past the end is a different region or none at all, never this one.
     * Compared by region identity rather than by physical value: the kernel is free
     * to place `second` virtually adjacent to `first`, and synthetic physicals are
     * handed out linearly, so the physical one past the end can legitimately equal
     * physical_first + 0x4000. A value comparison flaked under ASLR (T254). */
    const guest_region *past_end = guest_region_containing(first + 0x4000u);
    CHECK(past_end != guest_region_containing(first + 0x3FFFu));
    if (past_end != NULL) {
        CHECK_EQ_U32(physical_address(first + 0x4000u),
                     past_end->physical + (first + 0x4000u - past_end->address));
    } else {
        CHECK_EQ_U32(physical_address(first + 0x4000u), 0u);
    }

    /* An untracked address has no physical address, which the real call also
     * signals with 0. */
    CHECK_EQ_U32(physical_address(unallocated_address()), 0u);

    teardown();
}

static void test_physical_range_constraints_are_enforced(void)
{
    setup();

    /* An impossible window must fail rather than return memory outside it. */
    reset_capture();
    CHECK_EQ_U32(alloc_contiguous_ex(0x2000u, 0x1000u, 0x1100u, 0x1000u, PAGE_READWRITE),
                 0u);

    /* A satisfiable window must place the allocation inside it. */
    uint32_t base = alloc_contiguous_ex(0x2000u, 0x10000000u, 0xFFFFFFFFu, 0x1000u,
                                       PAGE_READWRITE);
    CHECK(base != 0u);
    uint32_t physical = physical_address(base);
    CHECK(physical >= 0x10000000u);

    teardown();
}

static void test_persist_flag_is_recorded(void)
{
    setup();

    uint32_t base = alloc_contiguous(0x2000u);
    CHECK(base != 0u);
    const guest_region *region = guest_region_at(base);
    CHECK(region != NULL);
    CHECK(region != NULL && !region->persist);

    uint32_t args[3] = {base, 0x2000u, 1u};
    call_ordinal(ORD_MmPersistContiguousMemory, args, 3u);
    region = guest_region_at(base);
    CHECK(region != NULL && region->persist);

    /* And it can be cleared again. */
    args[2] = 0u;
    call_ordinal(ORD_MmPersistContiguousMemory, args, 3u);
    region = guest_region_at(base);
    CHECK(region != NULL && !region->persist);

    /* An address that is not a base is reported rather than silently ignored. */
    reset_capture();
    uint32_t bad[3] = {unallocated_address(), 0x1000u, 1u};
    call_ordinal(ORD_MmPersistContiguousMemory, bad, 3u);
    CHECK(captured_contains("not an allocation base"));

    teardown();
}

static void test_set_address_protect_is_recorded_and_validated(void)
{
    setup();

    uint32_t base = alloc_contiguous(0x2000u);
    CHECK(base != 0u);
    const guest_region *region = guest_region_at(base);
    CHECK(region != NULL && region->protect == PAGE_READWRITE);

    uint32_t args[3] = {base, 0x2000u, PAGE_READONLY};
    call_ordinal(ORD_MmSetAddressProtect, args, 3u);
    region = guest_region_at(base);
    CHECK(region != NULL && region->protect == PAGE_READONLY);
    /* The protection at allocation time is remembered separately, because
     * NtQueryVirtualMemory reports both. */
    CHECK(region != NULL && region->alloc_protect == PAGE_READWRITE);

    /* A nonsense protection value must be refused. PAGE_READONLY|PAGE_READWRITE
     * names two access modes at once, which cannot be meant. */
    reset_capture();
    uint32_t bogus[3] = {base, 0x2000u, PAGE_READONLY | PAGE_READWRITE};
    call_ordinal(ORD_MmSetAddressProtect, bogus, 3u);
    CHECK(captured_contains("not a valid PAGE_"));
    region = guest_region_at(base);
    CHECK(region != NULL && region->protect == PAGE_READONLY);

    teardown();
}

static void test_query_address_protect_reports_the_containing_region(void)
{
    setup();

    uint32_t base = alloc_contiguous(0x2000u);
    CHECK(base != 0u);
    uint32_t inside[1] = {base + 0x1ff0u};
    CHECK_EQ_U32(call_ordinal(ORD_MmQueryAddressProtect, inside, 1u), PAGE_READWRITE);
    uint32_t set[3] = {base, 0x2000u, PAGE_READONLY};
    call_ordinal(ORD_MmSetAddressProtect, set, 3u);
    CHECK_EQ_U32(call_ordinal(ORD_MmQueryAddressProtect, inside, 1u), PAGE_READONLY);

    /* Untracked memory answers 0 and says so, never a protection it did not measure. */
    reset_capture();
    uint32_t outside[1] = {0x00000010u};
    CHECK_EQ_U32(call_ordinal(ORD_MmQueryAddressProtect, outside, 1u), 0u);
    CHECK(captured_contains("MmQueryAddressProtect"));

    teardown();
}

/* --------------------------------------------------------------------------
 * MmCreateKernelStack / MmDeleteKernelStack.
 * ----------------------------------------------------------------------- */

/* NumberOfBytes at the one static create site, 0x00387A67 in sub_00387A55
 * (generated/lifted/gen/recomp_0044.c), where the literal 0x6000 is pushed. Pinned
 * here so a drift in the measurement shows up as a change to this line. */
#define MEASURED_KERNEL_STACK_BYTES 0x6000u

static uint32_t create_kernel_stack(uint32_t bytes, uint32_t debugger_thread)
{
    uint32_t args[2] = {bytes, debugger_thread};
    return call_ordinal(ORD_MmCreateKernelStack, args, 2u);
}

static void delete_kernel_stack(uint32_t stack_base, uint32_t stack_limit)
{
    uint32_t args[2] = {stack_base, stack_limit};
    call_ordinal(ORD_MmDeleteKernelStack, args, 2u);
}

/* The measured guest sequence: create(0x6000, 0), use the memory below the returned
 * top, then delete(top, top - 0x6000). */
static void test_kernel_stack_create_use_delete_replay(void)
{
    setup();

    reset_capture();
    uint32_t top = create_kernel_stack(MEASURED_KERNEL_STACK_BYTES, 0u);
    CHECK(top != 0u);
    uint32_t limit = top - MEASURED_KERNEL_STACK_BYTES;

    /* sub_00388FAB runs a callback with esp at the top, so both ends must be real
     * writable memory: the lowest word and the first push below the top. */
    CHECK(range_is_mapped(limit, MEASURED_KERNEL_STACK_BYTES));
    CHECK(kernel_guest_write_u32(limit, 0xA5A5A5A5u));
    CHECK(kernel_guest_write_u32(top - 4u, 0x5A5A5A5Au));
    uint32_t readback = 0;
    CHECK(kernel_guest_read_u32(limit, &readback));
    CHECK_EQ_U32(readback, 0xA5A5A5A5u);
    CHECK(kernel_guest_read_u32(top - 4u, &readback));
    CHECK_EQ_U32(readback, 0x5A5A5A5Au);

    delete_kernel_stack(top, limit);
    CHECK(guest_region_at(limit) == NULL);
    CHECK(guest_region_containing(top - 4u) == NULL);
    CHECK_EQ_U32((uint32_t)captured_len, 0u);

    teardown();
}

/* The returned value is the END of the region, not its base. */
static void test_kernel_stack_returns_the_top_of_the_region(void)
{
    setup();

    uint32_t top = create_kernel_stack(MEASURED_KERNEL_STACK_BYTES, 0u);
    CHECK(top != 0u);
    const guest_region *region = guest_region_at(top - MEASURED_KERNEL_STACK_BYTES);
    CHECK(region != NULL);
    CHECK(region != NULL && region->size == MEASURED_KERNEL_STACK_BYTES);
    CHECK(region != NULL && region->protect == PAGE_READWRITE);
    CHECK(region != NULL && region->state == MEM_COMMIT);
    CHECK(guest_region_containing(top - 4u) == region);
    /* One past the end: it must not belong to the stack. */
    CHECK(guest_region_containing(top) != region);

    delete_kernel_stack(top, top - MEASURED_KERNEL_STACK_BYTES);
    teardown();
}

static void test_kernel_stack_delete_refuses_what_it_cannot_prove(void)
{
    setup();

    uint32_t top = create_kernel_stack(MEASURED_KERNEL_STACK_BYTES, 0u);
    CHECK(top != 0u);
    uint32_t limit = top - MEASURED_KERNEL_STACK_BYTES;

    /* A span that disagrees with the tracked region frees nothing. */
    reset_capture();
    delete_kernel_stack(top - GUEST_PAGE_SIZE, limit);
    CHECK(captured_contains("MmDeleteKernelStack"));
    CHECK(guest_region_at(limit) != NULL);
    CHECK(kernel_guest_write_u32(limit, 1u));

    reset_capture();
    delete_kernel_stack(top + GUEST_PAGE_SIZE, limit);
    CHECK(captured_contains("MmDeleteKernelStack"));
    CHECK(guest_region_at(limit) != NULL);

    /* A limit that is inside the region but not its base frees nothing. */
    reset_capture();
    delete_kernel_stack(top, limit + GUEST_PAGE_SIZE);
    CHECK(captured_contains("MmDeleteKernelStack"));
    CHECK(guest_region_at(limit) != NULL);

    /* A StackBase of 0 is refused, even with a real limit. */
    reset_capture();
    delete_kernel_stack(0u, limit);
    CHECK(captured_contains("MmDeleteKernelStack"));
    CHECK(guest_region_at(limit) != NULL);

    /* A stack that was never created is refused. */
    reset_capture();
    kernel_guest_ptr stranger = unallocated_address();
    delete_kernel_stack(stranger + MEASURED_KERNEL_STACK_BYTES, stranger);
    CHECK(captured_contains("MmDeleteKernelStack"));

    /* The region survived all of it and the exact pair still frees it. */
    CHECK(kernel_guest_write_u32(top - 4u, 2u));
    reset_capture();
    delete_kernel_stack(top, limit);
    CHECK(guest_region_at(limit) == NULL);
    CHECK_EQ_U32((uint32_t)captured_len, 0u);

    teardown();
}

static void test_kernel_stack_create_failure_and_oddities(void)
{
    setup();

    /* Zero bytes fails with the value the caller maps to E_OUTOFMEMORY. */
    reset_capture();
    CHECK_EQ_U32(create_kernel_stack(0u, 0u), 0u);
    CHECK(captured_contains("MmCreateKernelStack"));

    /* INFERRED: a non-page-multiple size is accepted but reported. */
    reset_capture();
    uint32_t odd = create_kernel_stack(0x1800u, 0u);
    CHECK(odd != 0u);
    CHECK(captured_contains("page size"));

    /* DebuggerThread is unmodelled and reported once, not per call. */
    reset_capture();
    uint32_t first = create_kernel_stack(MEASURED_KERNEL_STACK_BYTES, 1u);
    CHECK(first != 0u);
    CHECK(captured_contains("DebuggerThread"));
    reset_capture();
    uint32_t second = create_kernel_stack(MEASURED_KERNEL_STACK_BYTES, 1u);
    CHECK(second != 0u);
    CHECK(!captured_contains("DebuggerThread"));
    CHECK(second != first);

    teardown();
}

/* --------------------------------------------------------------------------
 * NtAllocate / NtFree / NtQuery.
 * ----------------------------------------------------------------------- */

static void test_virtual_alloc_and_release_round_trip(void)
{
    setup();

    uint32_t base = 0;
    uint32_t size = 0;
    CHECK_EQ_U32(nt_allocate(0u, 0x5000u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, &base,
                             &size),
                 STATUS_SUCCESS);
    CHECK(base != 0u);
    CHECK_EQ_U32(size, GUEST_ALLOCATION_GRANULARITY);

    const guest_region *region = guest_region_at(base);
    CHECK(region != NULL);
    CHECK(region != NULL && region->state == MEM_COMMIT);
    CHECK(region != NULL && !region->contiguous);

    /* A release names the whole region with a zero size, and reports back how much
     * actually went away. */
    kernel_guest_write_u32(out_slot(3), 0u);
    CHECK_EQ_U32(nt_free(base, 0u, MEM_RELEASE), STATUS_SUCCESS);
    uint32_t released = 0;
    kernel_guest_read_u32(out_slot(3), &released);
    CHECK_EQ_U32(released, GUEST_ALLOCATION_GRANULARITY);
    CHECK(guest_region_at(base) == NULL);

    teardown();
}

static void test_reserve_then_commit_keeps_the_same_base(void)
{
    setup();

    uint32_t base = 0;
    uint32_t size = 0;
    CHECK_EQ_U32(nt_allocate(0u, 0x10000u, MEM_RESERVE, PAGE_NOACCESS, &base, &size),
                 STATUS_SUCCESS);
    CHECK(base != 0u);
    const guest_region *region = guest_region_at(base);
    CHECK(region != NULL && region->state == MEM_RESERVE);

    /* Committing the reservation must reuse it, not allocate a second region at a
     * different address: the guest already holds pointers derived from `base`. */
    uint32_t committed_base = 0;
    uint32_t committed_size = 0;
    CHECK_EQ_U32(nt_allocate(base, 0x1000u, MEM_COMMIT, PAGE_READWRITE, &committed_base,
                             &committed_size),
                 STATUS_SUCCESS);
    CHECK_EQ_U32(committed_base, base);
    region = guest_region_at(base);
    CHECK(region != NULL && region->state == MEM_COMMIT);
    CHECK(region != NULL && region->protect == PAGE_READWRITE);

    /* Decommit returns it to reserved while keeping the address. */
    CHECK_EQ_U32(nt_free(base, 0u, MEM_DECOMMIT), STATUS_SUCCESS);
    region = guest_region_at(base);
    CHECK(region != NULL);
    CHECK(region != NULL && region->state == MEM_RESERVE);

    teardown();
}

static void test_query_virtual_memory_reports_the_region(void)
{
    setup();

    uint32_t base = 0;
    uint32_t size = 0;
    CHECK_EQ_U32(nt_allocate(0u, 0x20000u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE,
                             &base, &size),
                 STATUS_SUCCESS);
    CHECK(base != 0u);

    kernel_guest_ptr info = out_slot(8);
    uint32_t args[2] = {base + GUEST_PAGE_SIZE, info};
    CHECK_EQ_U32(call_ordinal(ORD_NtQueryVirtualMemory, args, 2u), STATUS_SUCCESS);

    uint32_t fields[7] = {0};
    for (unsigned i = 0; i < 7u; i++) {
        CHECK(kernel_guest_read_u32((kernel_guest_ptr)(info + i * 4u), &fields[i]));
    }
    CHECK_EQ_U32(fields[0], base + GUEST_PAGE_SIZE); /* BaseAddress, page floor */
    CHECK_EQ_U32(fields[1], base);                   /* AllocationBase */
    CHECK_EQ_U32(fields[2], PAGE_READWRITE);         /* AllocationProtect */
    CHECK_EQ_U32(fields[3], size - GUEST_PAGE_SIZE); /* RegionSize to the end */
    CHECK_EQ_U32(fields[4], MEM_COMMIT);             /* State */
    CHECK_EQ_U32(fields[5], PAGE_READWRITE);         /* Protect */
    CHECK_EQ_U32(fields[6], MEM_PRIVATE);            /* Type */

    /* A changed protection must show up in Protect while AllocationProtect keeps
     * reporting what the region started as. */
    uint32_t protect_args[3] = {base, size, PAGE_READONLY};
    call_ordinal(ORD_MmSetAddressProtect, protect_args, 3u);
    CHECK_EQ_U32(call_ordinal(ORD_NtQueryVirtualMemory, args, 2u), STATUS_SUCCESS);
    CHECK(kernel_guest_read_u32((kernel_guest_ptr)(info + 2u * 4u), &fields[2]));
    CHECK(kernel_guest_read_u32((kernel_guest_ptr)(info + 5u * 4u), &fields[5]));
    CHECK_EQ_U32(fields[2], PAGE_READWRITE);
    CHECK_EQ_U32(fields[5], PAGE_READONLY);

    /* An unallocated address reports MEM_FREE rather than failing. */
    uint32_t free_args[2] = {unallocated_address(), info};
    CHECK_EQ_U32(call_ordinal(ORD_NtQueryVirtualMemory, free_args, 2u), STATUS_SUCCESS);
    CHECK(kernel_guest_read_u32((kernel_guest_ptr)(info + 4u * 4u), &fields[4]));
    CHECK_EQ_U32(fields[4], MEM_FREE);

    teardown();
}

/* --------------------------------------------------------------------------
 * Failure paths and their exact status codes.
 * ----------------------------------------------------------------------- */

static void test_allocation_failure_status_codes(void)
{
    setup();

    uint32_t base = 0;
    uint32_t size = 0;

    /* Zero size. */
    CHECK_EQ_U32(nt_allocate(0u, 0u, MEM_COMMIT, PAGE_READWRITE, &base, &size),
                 STATUS_INVALID_PARAMETER);

    /* Neither COMMIT nor RESERVE. */
    CHECK_EQ_U32(nt_allocate(0u, 0x1000u, 0u, PAGE_READWRITE, &base, &size),
                 STATUS_INVALID_PARAMETER);

    /* An allocation type bit that does not exist. */
    CHECK_EQ_U32(nt_allocate(0u, 0x1000u, MEM_COMMIT | 0x40000000u, PAGE_READWRITE, &base,
                             &size),
                 STATUS_INVALID_PARAMETER);

    /* A free bit is not an allocation bit. */
    CHECK_EQ_U32(nt_allocate(0u, 0x1000u, MEM_COMMIT | MEM_RELEASE, PAGE_READWRITE, &base,
                             &size),
                 STATUS_INVALID_PARAMETER);

    /* Two access modes at once, and no access mode at all. */
    CHECK_EQ_U32(nt_allocate(0u, 0x1000u, MEM_COMMIT, PAGE_READONLY | PAGE_READWRITE,
                             &base, &size),
                 STATUS_INVALID_PAGE_PROTECTION);
    CHECK_EQ_U32(nt_allocate(0u, 0x1000u, MEM_COMMIT, 0u, &base, &size),
                 STATUS_INVALID_PAGE_PROTECTION);
    CHECK_EQ_U32(nt_allocate(0u, 0x1000u, MEM_COMMIT, PAGE_GUARD, &base, &size),
                 STATUS_INVALID_PAGE_PROTECTION);

    /* A NULL out-parameter cannot be written, and that is an access violation
     * rather than a success the guest would never see the result of. */
    uint32_t null_args[5] = {0u, 0u, out_slot(1), MEM_COMMIT, PAGE_READWRITE};
    CHECK_EQ_U32(call_ordinal(ORD_NtAllocateVirtualMemory, null_args, 5u),
                 STATUS_ACCESS_VIOLATION);

    teardown();
}

/**
 * guest_region_alloc's own status codes.
 *
 * Tested directly rather than only through NtAllocateVirtualMemory, because that
 * handler validates its arguments before calling down and so never reaches most of
 * these branches. Going only through the ordinal left the allocator free to return
 * any status it liked for a bad protection -- caught by mutation testing, not by
 * the ordinal-level tests. The function is public API in guest_mem.h, so its
 * contract is worth asserting on its own terms.
 */
static void test_region_alloc_status_codes(void)
{
    setup();

    nt_status status = STATUS_SUCCESS;
    guest_region_request request = {
        .bytes = 0x1000u,
        .alignment = 0u,
        .lowest_physical = 0u,
        .highest_physical = 0u,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .contiguous = false,
        .fixed_base = 0u,
    };

    /* A protection naming two access modes at once is specifically a protection
     * error, not a generic parameter error: the guest distinguishes them. */
    guest_region_request bad_protect = request;
    bad_protect.protect = PAGE_READONLY | PAGE_READWRITE;
    CHECK_EQ_U32(guest_region_alloc(&bad_protect, &status), 0u);
    CHECK_EQ_U32(status, STATUS_INVALID_PAGE_PROTECTION);

    bad_protect.protect = 0u;
    CHECK_EQ_U32(guest_region_alloc(&bad_protect, &status), 0u);
    CHECK_EQ_U32(status, STATUS_INVALID_PAGE_PROTECTION);

    /* Zero bytes. */
    guest_region_request zero = request;
    zero.bytes = 0u;
    CHECK_EQ_U32(guest_region_alloc(&zero, &status), 0u);
    CHECK_EQ_U32(status, STATUS_INVALID_PARAMETER);

    /* A state that is neither commit nor reserve. */
    guest_region_request no_state = request;
    no_state.state = 0u;
    CHECK_EQ_U32(guest_region_alloc(&no_state, &status), 0u);
    CHECK_EQ_U32(status, STATUS_INVALID_PARAMETER);

    /* A non-power-of-two alignment cannot be satisfied. */
    guest_region_request bad_alignment = request;
    bad_alignment.alignment = 0x3000u;
    CHECK_EQ_U32(guest_region_alloc(&bad_alignment, &status), 0u);
    CHECK_EQ_U32(status, STATUS_INVALID_PARAMETER);

    /* An inverted physical window. */
    guest_region_request inverted = request;
    inverted.lowest_physical = 0x20000000u;
    inverted.highest_physical = 0x10000000u;
    CHECK_EQ_U32(guest_region_alloc(&inverted, &status), 0u);
    CHECK_EQ_U32(status, STATUS_INVALID_PARAMETER);

    /* A window too small to hold the request is a memory failure, not a parameter
     * one: the arguments are coherent, there is simply nowhere to put it. */
    guest_region_request cramped = request;
    cramped.lowest_physical = 0x1000u;
    cramped.highest_physical = 0x1100u;
    CHECK_EQ_U32(guest_region_alloc(&cramped, &status), 0u);
    CHECK_EQ_U32(status, STATUS_NO_MEMORY);

    /* No request at all. */
    CHECK_EQ_U32(guest_region_alloc(NULL, &status), 0u);
    CHECK_EQ_U32(status, STATUS_INVALID_PARAMETER);

    /* A NULL status pointer must not crash: the address alone is a valid answer. */
    kernel_guest_ptr fine = guest_region_alloc(&request, NULL);
    CHECK(fine != 0u);

    /* And the success path sets STATUS_SUCCESS rather than leaving the caller's
     * variable untouched. */
    status = STATUS_UNSUCCESSFUL;
    kernel_guest_ptr ok = guest_region_alloc(&request, &status);
    CHECK(ok != 0u);
    CHECK_EQ_U32(status, STATUS_SUCCESS);

    teardown();
}

static void test_free_failure_status_codes(void)
{
    setup();

    uint32_t base = 0;
    uint32_t size = 0;
    CHECK_EQ_U32(nt_allocate(0u, 0x20000u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE,
                             &base, &size),
                 STATUS_SUCCESS);
    CHECK(base != 0u);

    /* Never allocated. */
    CHECK_EQ_U32(nt_free(unallocated_address(), 0u, MEM_RELEASE), STATUS_MEMORY_NOT_ALLOCATED);

    /* Inside a region but not its base: a different mistake, and a different code. */
    CHECK_EQ_U32(nt_free(base + GUEST_PAGE_SIZE, 0u, MEM_RELEASE),
                 STATUS_FREE_VM_NOT_AT_BASE);

    /* A release must name the whole region, which NT spells as a zero size. */
    CHECK_EQ_U32(nt_free(base, 0x1000u, MEM_RELEASE), STATUS_INVALID_PARAMETER);

    /* No free type, an unknown bit, and the contradictory combination. */
    CHECK_EQ_U32(nt_free(base, 0u, 0u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(nt_free(base, 0u, 0x40000000u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(nt_free(base, 0u, MEM_DECOMMIT | MEM_RELEASE), STATUS_INVALID_PARAMETER);

    /* None of those should have freed anything. */
    CHECK(guest_region_at(base) != NULL);

    /* And after a real release, releasing again is not allocated any more. */
    CHECK_EQ_U32(nt_free(base, 0u, MEM_RELEASE), STATUS_SUCCESS);
    CHECK_EQ_U32(nt_free(base, 0u, MEM_RELEASE), STATUS_MEMORY_NOT_ALLOCATED);

    teardown();
}

static void test_query_failure_status_codes(void)
{
    setup();

    /* A NULL information buffer cannot be filled in. */
    uint32_t args[2] = {scratch, 0u};
    CHECK_EQ_U32(call_ordinal(ORD_NtQueryVirtualMemory, args, 2u),
                 STATUS_ACCESS_VIOLATION);

    /* A buffer that would run off the end of the guest address space likewise. */
    uint32_t overrun[2] = {scratch, 0xFFFFFFF8u};
    CHECK_EQ_U32(call_ordinal(ORD_NtQueryVirtualMemory, overrun, 2u),
                 STATUS_ACCESS_VIOLATION);

    teardown();
}

static void test_contiguous_free_rejects_a_non_base(void)
{
    setup();

    uint32_t base = alloc_contiguous(0x4000u);
    CHECK(base != 0u);

    /* MmFreeContiguousMemory returns void, so the only evidence of a bad call is
     * the diagnostic, and the allocation must survive. */
    reset_capture();
    free_contiguous(base + GUEST_PAGE_SIZE);
    CHECK(captured_contains("not a contiguous allocation base"));
    CHECK(guest_region_at(base) != NULL);

    reset_capture();
    free_contiguous(unallocated_address());
    CHECK(captured_contains("not a contiguous allocation base"));

    /* A virtual allocation is not a contiguous one, and must not be freed by the
     * contiguous path. */
    uint32_t virtual_base = 0;
    uint32_t virtual_size = 0;
    CHECK_EQ_U32(nt_allocate(0u, 0x1000u, MEM_COMMIT, PAGE_READWRITE, &virtual_base,
                             &virtual_size),
                 STATUS_SUCCESS);
    reset_capture();
    free_contiguous(virtual_base);
    CHECK(captured_contains("not a contiguous allocation base"));
    CHECK(guest_region_at(virtual_base) != NULL);

    /* The real base still works. */
    free_contiguous(base);
    CHECK(guest_region_at(base) == NULL);

    teardown();
}

static void test_missing_argument_frame_is_reported(void)
{
    setup();

    /* A handler reached with no frame is a call-boundary bug. It must say so and
     * fail, never substitute a zero argument: a zero-byte allocation request is a
     * plausible value that would fail somewhere else entirely. */
    reset_capture();
    CHECK_EQ_U32(kernel_hle_call(ORD_MmAllocateContiguousMemory, NULL), 0u);
    CHECK(captured_contains("no argument frame"));

    reset_capture();
    CHECK_EQ_U32(kernel_hle_call(ORD_NtAllocateVirtualMemory, NULL),
                 STATUS_INVALID_PARAMETER);
    CHECK(captured_contains("no argument frame"));

    /* An argument that falls outside the frame's stack limit is reported too,
     * rather than read from whatever lies beyond. */
    kernel_call_frame narrow = {scratch, (kernel_guest_ptr)(scratch + 8u), 0u, 0u, false, 0u, false};
    reset_capture();
    CHECK_EQ_U32(kernel_hle_call(ORD_MmAllocateContiguousMemoryEx, &narrow), 0u);
    CHECK(captured_contains("could not read argument"));

    teardown();
}

/* --------------------------------------------------------------------------
 * MmLockUnlockBufferPages (175): (BaseAddress, NumberOfBytes, UnlockPages).
 * ----------------------------------------------------------------------- */

static uint32_t lock_pages(uint32_t base, uint32_t bytes, uint32_t unlock)
{
    uint32_t args[3] = {base, bytes, unlock};
    return call_ordinal(ORD_MmLockUnlockBufferPages, args, 3u);
}

/* The measured happy path: lock a contiguous buffer, unlock it, silent, VOID (0).
 * BREAKS THIS: swapping the mode sense (0 = lock), reading the arguments in the wrong
 * order, not counting a lock, or a handler that logs on the measured path. */
static void test_lock_then_unlock_round_trip_is_silent(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x3000u);
    CHECK(base != 0u);
    reset_capture();
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    CHECK_EQ_U32(lock_pages(base, 0x3000u, 0u), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 3u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x2FFFu), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x3000u), 0u);
    CHECK_EQ_U32(lock_pages(base, 0x3000u, 1u), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 0u);
    CHECK_EQ_U32((uint32_t)captured_len, 0u);
    free_contiguous(base);
    teardown();
}

/* A byte range covers every page it touches: an unaligned base and a length that crosses
 * a page boundary lock two pages, a length of one byte locks one.
 * BREAKS THIS: not rounding the end up, not rounding the base down, counting bytes. */
static void test_lock_covers_every_page_the_range_touches(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x4000u);
    CHECK(base != 0u);
    lock_pages(base + 0x800u, 0x1000u, 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 2u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x2000u), 0u);
    lock_pages(base + 0x3FFFu, 1u, 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x3000u), 1u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 3u);
    /* the same unaligned range unlocks the same two pages and no neighbour */
    lock_pages(base + 0x800u, 0x1000u, 1u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 0u);
    teardown();
}

/* Locks nest per page: two locks need two unlocks.
 * BREAKS THIS: a boolean flag instead of a count. */
static void test_locks_nest_per_page(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x2000u);
    lock_pages(base, 0x2000u, 0u);
    lock_pages(base, 0x1000u, 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 2u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 1u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 2u);
    lock_pages(base, 0x2000u, 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 1u);
    teardown();
}

/* Pages locked in any order are found again: the table is sorted, so descending and
 * interleaved inserts exercise the shift, and a deep unlock removes from the middle.
 * BREAKS THIS: appending without keeping the table sorted, a binary search off by one,
 * not shifting on removal. */
static void test_many_pages_in_any_order(void)
{
    setup();
    uint32_t base = alloc_contiguous(64u * 0x1000u);
    CHECK(base != 0u);
    for (int page = 63; page >= 0; page -= 2) {
        lock_pages(base + (uint32_t)page * 0x1000u, 0x1000u, 0u);
    }
    for (int page = 0; page < 64; page += 2) {
        lock_pages(base + (uint32_t)page * 0x1000u, 0x1000u, 0u);
    }
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 64u);
    for (uint32_t page = 0; page < 64u; page++) {
        CHECK_EQ_U32(kernel_memory_page_lock_count(base + page * 0x1000u), 1u);
    }
    for (uint32_t page = 10u; page < 20u; page++) {
        lock_pages(base + page * 0x1000u, 0x1000u, 1u);
    }
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 54u);
    for (uint32_t page = 0; page < 64u; page++) {
        const uint32_t want = (page >= 10u && page < 20u) ? 0u : 1u;
        CHECK_EQ_U32(kernel_memory_page_lock_count(base + page * 0x1000u), want);
    }
    CHECK_EQ_U32((uint32_t)captured_len, 0u);
    teardown();
}

/* An unlock with no lock is reported and leaves the count at zero rather than wrapping.
 * BREAKS THIS: decrementing below zero (count becomes 0xFFFFFFFF), not reporting.
 * An unlock that spans one locked and one unlocked page releases the locked one. */
static void test_unlock_without_lock_is_reported_and_does_not_underflow(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x2000u);
    reset_capture();
    CHECK_EQ_U32(lock_pages(base, 0x2000u, 1u), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    CHECK(captured_contains("MmLockUnlockBufferPages"));
    CHECK(captured_contains("not locked"));

    lock_pages(base, 0x1000u, 0u);
    reset_capture();
    lock_pages(base, 0x2000u, 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 0u);
    CHECK(captured_contains("not locked"));
    teardown();
}

/* Only the two measured modes exist. All 14 sites pass the literal 0 or 1, so anything else
 * is a mis-read argument and is refused with NO state change.
 * BREAKS THIS: treating any nonzero as unlock, or any nonzero as lock. */
static void test_unmeasured_modes_are_refused_loudly(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x1000u);
    lock_pages(base, 0x1000u, 0u);
    static const uint32_t modes[] = {2u, 0x100u, 0x80000000u, 0xFFFFFFFFu};
    for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); index++) {
        reset_capture();
        CHECK_EQ_U32(lock_pages(base, 0x1000u, modes[index]), 0u);
        CHECK_EQ_U32(kernel_memory_page_lock_count(base), 1u);
        CHECK_EQ_U32(kernel_memory_locked_page_total(), 1u);
        CHECK(captured_contains("MmLockUnlockBufferPages"));
        CHECK(captured_contains("refused"));
    }
    teardown();
}

/* A zero length is a no-op and silent (a page count of 0 is a legitimate computed value at
 * 0x0040C2D5). A range that wraps the 4 GB space is refused loudly.
 * BREAKS THIS: locking one page for a zero length, wrapping silently. */
static void test_zero_length_is_a_silent_no_op_and_wrap_is_refused(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x1000u);
    reset_capture();
    lock_pages(base, 0u, 0u);
    lock_pages(base, 0u, 1u);
    /* an UNALIGNED base is where "last byte = base - 1" would still land on a page */
    lock_pages(base + 0x800u, 0u, 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    CHECK_EQ_U32((uint32_t)captured_len, 0u);

    CHECK_EQ_U32(lock_pages(0xFFFFF000u, 0x2000u, 0u), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    CHECK(captured_contains("refused"));
    reset_capture();
    lock_pages(0xFFFFFFFFu, 0xFFFFFFFFu, 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    CHECK(captured_contains("refused"));

    /* A range ending EXACTLY on 4 GB is legal (its last byte is 0xFFFFFFFF), so it is locked
     * and unlocked, not refused. BREAKS THIS: the bound as `>=` instead of `>`. */
    reset_capture();
    lock_pages(0xFFFFF000u, 0x1000u, 0u);
    CHECK(!captured_contains("refused"));
    CHECK_EQ_U32(kernel_memory_page_lock_count(0xFFFFF000u), 1u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 1u);
    lock_pages(0xFFFFF000u, 0x1000u, 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(0xFFFFF000u), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    teardown();
}

/* The lock table grows past its first 64 entries without losing or corrupting any: every
 * page keeps its depth, and the table's own heap block is not overrun (a table that fails
 * to grow writes past its block, which glibc reports as heap corruption when the table is
 * freed at teardown). BREAKS THIS: growth that does not enlarge the table. */
static void test_the_lock_table_grows_past_its_initial_capacity(void)
{
    setup();
    const uint32_t first = 0x10000000u;
    const uint32_t pages = 4096u;
    lock_pages(first, pages * 0x1000u, 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), pages);
    CHECK_EQ_U32(kernel_memory_page_lock_count(first), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(first + (pages - 1u) * 0x1000u), 1u);
    lock_pages(first, pages * 0x1000u, 1u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    teardown();
}

/* Against region tracking: a range the allocator does not know, one that runs off the end
 * of its region, and one in reserved-but-uncommitted memory are each reported, but the
 * guest's pin is still recorded so its later unlock balances instead of underflowing.
 * BREAKS THIS: not consulting the region, refusing (and so unbalancing the later unlock),
 * accepting the over-long range silently, or ignoring the reserve state. */
static void test_lock_outside_tracked_committed_memory_is_reported_but_recorded(void)
{
    setup();
    uint32_t outside = unallocated_address();
    reset_capture();
    lock_pages(outside, 0x1000u, 0u);
    CHECK(captured_contains("outside"));
    CHECK_EQ_U32(kernel_memory_page_lock_count(outside), 1u);
    reset_capture();
    lock_pages(outside, 0x1000u, 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(outside), 0u);
    CHECK_EQ_U32((uint32_t)captured_len, 0u);

    uint32_t base = alloc_contiguous(0x1000u);
    reset_capture();
    lock_pages(base, 0x2000u, 0u);
    CHECK(captured_contains("outside"));
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 1u);
    lock_pages(base, 0x2000u, 1u);

    uint32_t reserved = 0u;
    CHECK_EQ_U32(nt_allocate(0u, 0x10000u, MEM_RESERVE, PAGE_READWRITE, &reserved, NULL),
                 STATUS_SUCCESS);
    reset_capture();
    lock_pages(reserved, 0x1000u, 0u);
    CHECK(captured_contains("not committed"));
    CHECK_EQ_U32(kernel_memory_page_lock_count(reserved), 1u);
    teardown();
}

/* A frame the stack cannot supply is reported and changes nothing.
 * BREAKS THIS: defaulting a missing argument to 0 (which is a valid lock request). */
static void test_lock_with_no_readable_arguments_is_reported(void)
{
    setup();
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    frame.stack_ptr = 0xFFFFFFF0u;
    reset_capture();
    CHECK_EQ_U32(kernel_hle_call(ORD_MmLockUnlockBufferPages, &frame), 0u);
    CHECK(captured_contains("MmLockUnlockBufferPages"));
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    reset_capture();
    CHECK_EQ_U32(kernel_hle_call(ORD_MmLockUnlockBufferPages, NULL), 0u);
    CHECK(captured_contains("MmLockUnlockBufferPages"));
    teardown();
}

/* --------------------------------------------------------------------------
 * MmLockUnlockPhysicalPage (176): (PhysicalAddress, UnlockPage).
 *
 * MEASURED pairing, the XPP USB driver's transfer descriptors: the submit path (0x00473F81)
 * calls MmLockUnlockBufferPages(va, len, 0), then stores MmGetPhysicalAddress(va) at TD+4
 * and MmGetPhysicalAddress(va + len - 1) at TD+0xC; the completion path (0x00473E5F and
 * 0x00473E78, and the register-dispatched twins at 0x00474240 and 0x00474253) calls
 * MmLockUnlockPhysicalPage(TD+4, 1) and, when TD+0xC is on a different page,
 * MmLockUnlockPhysicalPage(TD+0xC, 1). So the physical unlock must release the page lock the
 * VIRTUAL lock took, through the synthetic physical map.
 * ----------------------------------------------------------------------- */

static uint32_t unlock_physical(uint32_t physical, uint32_t unlock)
{
    uint32_t args[2] = {physical, unlock};
    return call_ordinal(ORD_MmLockUnlockPhysicalPage, args, 2u);
}

/* The measured round trip, in the order and shape the XPP driver uses, spanning two pages:
 * lock the virtual range, take both physical addresses, unlock each by physical address.
 * Silent, VOID (0), and the table is empty again.
 * BREAKS THIS: not translating physical to virtual (page numbers would be the physical ones
 * and nothing would be released), unlocking every page of the range instead of the page
 * named, swapping the argument order, or logging on the measured path. */
static void test_physical_unlock_releases_what_the_virtual_lock_took(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x3000u);
    CHECK(base != 0u);
    const uint32_t buffer = base + 0xF00u;
    const uint32_t length = 0x200u; /* crosses from page 0 into page 1 */
    CHECK_EQ_U32(lock_pages(buffer, length, 0u), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 2u);
    const uint32_t first_physical = physical_address(buffer);
    const uint32_t last_physical = physical_address(buffer + length - 1u);
    CHECK((first_physical & ~0xFFFu) != (last_physical & ~0xFFFu));

    reset_capture();
    CHECK_EQ_U32(unlock_physical(first_physical, 1u), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 1u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 1u);
    CHECK_EQ_U32(unlock_physical(last_physical, 1u), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    CHECK_EQ_U32((uint32_t)captured_len, 0u);
    CHECK_EQ_U32(kernel_memory_physical_refused_count(), 0u);
    free_contiguous(base);
    teardown();
}

/* A buffer inside one page: the title unlocks only the first physical address (the second is
 * on the same page and the driver skips it), and that releases the whole page lock.
 * An address in the MIDDLE of a page names that page.
 * BREAKS THIS: requiring a page-aligned physical address, or translating the offset away
 * into the next page. */
static void test_a_mid_page_physical_address_names_its_page(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x2000u);
    lock_pages(base + 0x1100u, 0x20u, 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 1u);
    CHECK_EQ_U32(unlock_physical(physical_address(base + 0x1100u) + 0x7u, 1u), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    /* The last byte of a page, and the first byte of the next, are different pages. */
    lock_pages(base, 0x2000u, 0u);
    CHECK_EQ_U32(unlock_physical(physical_address(base + 0xFFFu), 1u), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 1u);
    CHECK_EQ_U32(unlock_physical(physical_address(base + 0x1000u), 1u), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    teardown();
}

/* Locks nest per page across both doors: two virtual locks need two physical unlocks, and a
 * virtual unlock balances a physical lock's worth equally (one table).
 * BREAKS THIS: a second table for physical locks, or unlocking to zero. */
static void test_physical_unlock_shares_the_virtual_lock_depth(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x1000u);
    lock_pages(base, 0x1000u, 0u);
    lock_pages(base, 0x1000u, 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 2u);
    unlock_physical(physical_address(base), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 1u);
    lock_pages(base, 0x1000u, 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 0u);
    reset_capture();
    unlock_physical(physical_address(base), 1u);
    CHECK(captured_contains("not locked"));
    teardown();
}

/* Unlocking a page nobody locked is REPORTED, changes nothing and never underflows, and does
 * not touch a neighbouring page that IS locked.
 * BREAKS THIS: decrementing below zero, silence, or unlocking the wrong page. */
static void test_physical_unlock_of_an_unlocked_page_is_reported(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x2000u);
    lock_pages(base + 0x1000u, 0x1000u, 0u);
    reset_capture();
    CHECK_EQ_U32(unlock_physical(physical_address(base), 1u), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base + 0x1000u), 1u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 1u);
    CHECK(captured_contains("MmLockUnlockPhysicalPage"));
    CHECK(captured_contains("not locked"));

    /* The log names the PAGE, not the mid-page address that was passed. BREAKS THIS: the page
     * mask dropped from the reported address. */
    char expected[64];
    snprintf(expected, sizeof(expected), "unlocks the page at %#x, which", base);
    CHECK(captured_contains(expected));
    reset_capture();
    CHECK_EQ_U32(unlock_physical(physical_address(base) + 0x123u, 1u), 0u);
    CHECK(captured_contains(expected));
    teardown();
}

/* The lock mode (UnlockPage 0) is NOT measured (all four sites push the literal 1), so it is
 * refused loudly with no state change, as is any other value. The refusal is counted.
 * BREAKS THIS: treating 0 as a lock (a page would become locked), any nonzero as unlock, or
 * a silent refusal. */
static void test_physical_lock_mode_and_odd_values_are_refused_loudly(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x1000u);
    lock_pages(base, 0x1000u, 0u);
    const uint32_t physical = physical_address(base);
    static const uint32_t modes[] = {0u, 2u, 0x100u, 0x80000000u, 0xFFFFFFFFu};
    for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); index++) {
        reset_capture();
        CHECK_EQ_U32(unlock_physical(physical, modes[index]), 0u);
        CHECK_EQ_U32(kernel_memory_page_lock_count(base), 1u);
        CHECK_EQ_U32(kernel_memory_locked_page_total(), 1u);
        CHECK(captured_contains("MmLockUnlockPhysicalPage"));
        CHECK(captured_contains("refused"));
        CHECK_EQ_U32(kernel_memory_physical_refused_count(), (uint32_t)index + 1u);
    }
    teardown();
}

/* A physical address that maps to no tracked region (0, one never handed out, one whose
 * region was freed) is refused loudly with no state change: it cannot be tied to a page.
 * BREAKS THIS: unlocking page 0, treating the physical number as a virtual one, or tracking
 * a freed region's physical range forever. */
static void test_an_unmapped_physical_address_is_refused(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x1000u);
    uint32_t other = alloc_contiguous(0x1000u);
    lock_pages(base, 0x1000u, 0u);
    lock_pages(other, 0x1000u, 0u);
    const uint32_t gone = physical_address(other);
    free_contiguous(other);
    const uint32_t unmapped[] = {0u, 0xDEAD0000u, 0xFFFFFFFFu, gone};
    for (size_t index = 0; index < sizeof(unmapped) / sizeof(unmapped[0]); index++) {
        reset_capture();
        CHECK_EQ_U32(unlock_physical(unmapped[index], 1u), 0u);
        CHECK(captured_contains("MmLockUnlockPhysicalPage"));
        CHECK(captured_contains("refused: no tracked"));
        CHECK_EQ_U32(kernel_memory_page_lock_count(base), 1u);
        CHECK_EQ_U32(kernel_memory_physical_refused_count(), (uint32_t)index + 1u);
    }
    /* An address passed as a VIRTUAL one is not accepted either: physical and virtual are
     * different numbers here. */
    reset_capture();
    unlock_physical(base, 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 1u);
    teardown();
}

/* Two regions: a physical address unlocks ITS region's page and no other.
 * BREAKS THIS: matching on page index alone, or a lookup that returns the first region. */
static void test_physical_unlock_touches_only_its_own_region(void)
{
    setup();
    uint32_t first = alloc_contiguous(0x2000u);
    uint32_t second = alloc_contiguous(0x2000u);
    lock_pages(first, 0x2000u, 0u);
    lock_pages(second, 0x2000u, 0u);
    unlock_physical(physical_address(second + 0x1000u), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(first), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(first + 0x1000u), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(second), 1u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(second + 0x1000u), 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 3u);
    teardown();
}

/* The inverse of the synthetic physical map: first and last byte of a region round-trip,
 * one past the end and 0 do not.
 * BREAKS THIS: an off-by-one at either end of the region. */
static void test_virtual_from_physical_is_the_inverse_of_the_physical_map(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x3000u);
    kernel_guest_ptr found = 0u;
    CHECK(guest_virtual_from_physical(physical_address(base), &found));
    CHECK_EQ_U32(found, base);
    CHECK(guest_virtual_from_physical(physical_address(base + 0x2FFFu), &found));
    CHECK_EQ_U32(found, base + 0x2FFFu);
    CHECK(guest_virtual_from_physical(physical_address(base + 0x1234u), &found));
    CHECK_EQ_U32(found, base + 0x1234u);
    /* The physical range of the NEXT allocation starts where this one ends (the synthetic map
     * is linear), and its first byte must resolve to THAT allocation, not to this one. */
    uint32_t next = alloc_contiguous(0x1000u);
    const guest_region *own = guest_region_containing(base);
    CHECK(own != NULL);
    /* Deterministic: physical addresses are synthetic and handed out by a linear cursor, so
     * this is asserted rather than assumed (a skipped branch would test nothing). */
    CHECK(own != NULL && own->physical + own->size == physical_address(next));
    if (own != NULL) {
        found = 0u;
        CHECK(guest_virtual_from_physical(own->physical + own->size, &found));
        CHECK_EQ_U32(found, next);
        CHECK(guest_virtual_from_physical(own->physical + own->size - 1u, &found));
        CHECK_EQ_U32(found, base + own->size - 1u);
    }
    /* One past the end of the physical range names no region of this allocation. */
    const guest_region *region = guest_region_containing(base);
    CHECK(region != NULL);
    found = 0xABCDu;
    const bool past = guest_virtual_from_physical(region->physical + region->size, &found);
    CHECK(!past || guest_region_containing(found) != region);
    found = 0xABCDu;
    CHECK(!guest_virtual_from_physical(0u, &found));
    CHECK_EQ_U32(found, 0xABCDu); /* untouched on failure */
    /* A NULL out-parameter is a refusal, not a crash, even for an address that maps. */
    CHECK(!guest_virtual_from_physical(physical_address(base), NULL));
    CHECK(!guest_virtual_from_physical(0u, &found));
    /* One below the start belongs to whichever region precedes it in physical space (the
     * scratch region here), never to this one. */
    const bool below = guest_virtual_from_physical(region->physical - 1u, &found);
    CHECK(!below || guest_region_containing(found) != region);
    teardown();
}

/* A frame the stack cannot supply is reported and changes nothing (a zero would be a valid
 * address-looking value, and a missing UnlockPage must not default to the measured 1). */
static void test_physical_unlock_with_no_readable_arguments_is_reported(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x1000u);
    lock_pages(base, 0x1000u, 0u);
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    frame.stack_ptr = 0xFFFFFFF0u;
    reset_capture();
    CHECK_EQ_U32(kernel_hle_call(ORD_MmLockUnlockPhysicalPage, &frame), 0u);
    CHECK(captured_contains("MmLockUnlockPhysicalPage"));
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 1u);
    reset_capture();
    CHECK_EQ_U32(kernel_hle_call(ORD_MmLockUnlockPhysicalPage, NULL), 0u);
    CHECK(captured_contains("MmLockUnlockPhysicalPage"));
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 1u);
    teardown();
}

/* Registration is a fresh kernel for the refusal counter too.
 * BREAKS THIS: a counter that survives kernel_memory_register. */
static void test_registration_clears_the_physical_refusal_count(void)
{
    setup();
    unlock_physical(0u, 1u);
    CHECK_EQ_U32(kernel_memory_physical_refused_count(), 1u);
    kernel_hle_init();
    kernel_memory_register();
    CHECK_EQ_U32(kernel_memory_physical_refused_count(), 0u);
    teardown();
}

/* Lock state belongs to one registration: re-registering (a fresh kernel) starts clean.
 * BREAKS THIS: static state that survives kernel_memory_register. */
static void test_registration_clears_the_lock_table(void)
{
    setup();
    uint32_t base = alloc_contiguous(0x2000u);
    lock_pages(base, 0x2000u, 0u);
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 2u);
    kernel_hle_init();
    kernel_memory_register();
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 0u);
    CHECK_EQ_U32(kernel_memory_page_lock_count(base), 0u);
    teardown();
}

#define LOCK_THREADS 4
#define LOCKS_PER_THREAD 500
static uint32_t lock_thread_base;

static void *lock_worker(void *opaque)
{
    /* A private guest stack per thread inside the scratch region: the shared call_ordinal
     * frame would race. */
    const kernel_guest_ptr stack = (kernel_guest_ptr)(scratch + 0x800u +
                                   (uint32_t)(uintptr_t)opaque * 0x20u);
    const uint32_t words[4] = {0u, lock_thread_base, 0x4000u, 0u};
    for (unsigned word = 0; word < 4u; word++) {
        kernel_guest_write_u32(stack + word * 4u, words[word]);
    }
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    frame.stack_ptr = stack;
    for (int index = 0; index < LOCKS_PER_THREAD; index++) {
        (void)kernel_hle_call(ORD_MmLockUnlockBufferPages, &frame);
    }
    return NULL;
}

/* Guest threads are host threads: concurrent locks of overlapping ranges lose no count.
 * BREAKS THIS: removing the table mutex (lost updates, torn shifts; run a few times). */
static void test_concurrent_locks_lose_no_count(void)
{
    setup();
    lock_thread_base = alloc_contiguous(0x4000u);
    pthread_t threads[LOCK_THREADS];
    for (int index = 0; index < LOCK_THREADS; index++) {
        CHECK(pthread_create(&threads[index], NULL, lock_worker, (void *)(uintptr_t)index) == 0);
    }
    for (int index = 0; index < LOCK_THREADS; index++) {
        pthread_join(threads[index], NULL);
    }
    for (uint32_t page = 0; page < 4u; page++) {
        CHECK_EQ_U32(kernel_memory_page_lock_count(lock_thread_base + page * 0x1000u),
                     (uint32_t)(LOCK_THREADS * LOCKS_PER_THREAD));
    }
    CHECK_EQ_U32(kernel_memory_locked_page_total(), 4u);
    teardown();
}

/* --------------------------------------------------------------------------
 * Registration bookkeeping.
 * ----------------------------------------------------------------------- */

static void test_registration_raises_implemented_count_exactly(void)
{
    kernel_hle_init();
    guest_mem_reset();

    size_t expected = 0;
    const unsigned *ordinals = kernel_memory_ordinals(&expected);
    CHECK(ordinals != NULL);
    CHECK(expected == kernel_memory_ordinal_count());
    CHECK(expected == 16u); /* T1146 measured ordinal181. */

    CHECK_EQ_U32((uint32_t)kernel_hle_implemented_count(ordinals, expected), 0u);

    size_t registered = kernel_memory_register();
    /* A short count means an ordinal in kernel_memory.c is not in the generated
     * table, i.e. the two have drifted. */
    CHECK_EQ_U32((uint32_t)registered, (uint32_t)expected);
    CHECK_EQ_U32((uint32_t)kernel_hle_implemented_count(ordinals, expected),
                 (uint32_t)expected);

    /* Exactly these and no others: sweep the whole table so an accidental extra
     * registration is caught rather than absorbed. */
    unsigned all[XBOX_KERNEL_ORDINAL_MAX + 1];
    size_t all_count = 0;
    for (unsigned ordinal = 0; ordinal <= XBOX_KERNEL_ORDINAL_MAX; ordinal++) {
        all[all_count++] = ordinal;
    }
    CHECK_EQ_U32((uint32_t)kernel_hle_implemented_count(all, all_count),
                 (uint32_t)expected);

    /* Every registered ordinal must have a name, which proves the numbers are real
     * table entries rather than plausible guesses. */
    for (size_t i = 0; i < expected; i++) {
        const kernel_entry *entry = kernel_hle_entry(ordinals[i]);
        CHECK(entry != NULL);
        CHECK(entry != NULL && entry->name != NULL);
        CHECK(entry != NULL && entry->state == KERNEL_ENTRY_IMPLEMENTED);
    }

    /* The names must be the ones this module thinks it implements. Checked by name
     * because the ordinal numbers are the thing most likely to be wrong. */
    CHECK(strcmp(kernel_hle_entry(ORD_MmAllocateContiguousMemory)->name,
                 "MmAllocateContiguousMemory") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_MmAllocateContiguousMemoryEx)->name,
                 "MmAllocateContiguousMemoryEx") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_MmCreateKernelStack)->name,
                 "MmCreateKernelStack") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_MmDeleteKernelStack)->name,
                 "MmDeleteKernelStack") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_MmFreeContiguousMemory)->name,
                 "MmFreeContiguousMemory") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_MmGetPhysicalAddress)->name,
                 "MmGetPhysicalAddress") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_MmLockUnlockBufferPages)->name,
                 "MmLockUnlockBufferPages") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_MmLockUnlockPhysicalPage)->name,
                 "MmLockUnlockPhysicalPage") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_MmPersistContiguousMemory)->name,
                 "MmPersistContiguousMemory") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_MmQueryAllocationSize)->name,
                 "MmQueryAllocationSize") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_MmSetAddressProtect)->name,
                 "MmSetAddressProtect") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_NtAllocateVirtualMemory)->name,
                 "NtAllocateVirtualMemory") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_NtFreeVirtualMemory)->name,
                 "NtFreeVirtualMemory") == 0);
    CHECK(strcmp(kernel_hle_entry(ORD_NtQueryVirtualMemory)->name,
                 "NtQueryVirtualMemory") == 0);

    guest_mem_reset();
}

static void test_implemented_ordinals_are_silent(void)
{
    setup();

    /* An implemented function must not report itself: the backlog is defined by
     * what logs, so a chatty implementation would read as unfinished work. */
    reset_capture();
    uint32_t base = alloc_contiguous(0x1000u);
    CHECK(base != 0u);
    CHECK_EQ_U32((uint32_t)captured_len, 0u);
    CHECK(query_allocation_size(base) != 0u);
    CHECK(physical_address(base) != 0u);
    free_contiguous(base);
    CHECK_EQ_U32((uint32_t)captured_len, 0u);

    /* The kernel stack pair is silent on the measured happy path too. */
    uint32_t stack_create[2] = {MEASURED_KERNEL_STACK_BYTES, 0u};
    uint32_t stack_top = call_ordinal(ORD_MmCreateKernelStack, stack_create, 2u);
    CHECK(stack_top != 0u);
    uint32_t stack_delete[2] = {stack_top, stack_top - MEASURED_KERNEL_STACK_BYTES};
    call_ordinal(ORD_MmDeleteKernelStack, stack_delete, 2u);
    CHECK_EQ_U32((uint32_t)captured_len, 0u);

    /* They must also not appear in the backlog report. */
    size_t count = 0;
    const unsigned *ordinals = kernel_memory_ordinals(&count);
    reset_capture();
    kernel_hle_report_missing(ordinals, count);
    CHECK(captured_contains("0 of 16 imported ordinals"));
    CHECK(!captured_contains("MmAllocateContiguousMemory"));

    teardown();
}

/* --- fastcall register arguments ------------------------------------------
 *
 * The Kf* family is __fastcall, not __stdcall. KfLowerIrql (ordinal 161) has 133
 * call sites, more than any other ordinal in the image, and is reached by
 * `mov cl, al` with nothing pushed. A handler reading kernel_frame_arg(0) would
 * silently get the return address instead of the IRQL.
 */

static void test_fastcall_register_arguments(void)
{
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    uint32_t value = 0xDEADu;

    /* Unset registers must be refused, NOT reported as zero: a zero IRQL is a
     * perfectly legitimate value, so a substituted zero is undetectable. */
    CHECK(!kernel_frame_reg_arg(&frame, 0u, &value));
    CHECK(!kernel_frame_reg_arg(&frame, 1u, &value));

    kernel_frame_set_registers(&frame, 0x2u, 0x7u);
    CHECK(kernel_frame_reg_arg(&frame, 0u, &value));
    CHECK_EQ_U32(value, 0x2u);
    CHECK(kernel_frame_reg_arg(&frame, 1u, &value));
    CHECK_EQ_U32(value, 0x7u);

    /* A legitimate zero must be readable, which is the whole point of has_registers. */
    kernel_frame_set_registers(&frame, 0u, 0u);
    CHECK(kernel_frame_reg_arg(&frame, 0u, &value));
    CHECK_EQ_U32(value, 0u);

    /* x86 fastcall passes at most two arguments in registers. */
    CHECK(!kernel_frame_reg_arg(&frame, 2u, &value));
    CHECK(!kernel_frame_reg_arg(NULL, 0u, &value));
    CHECK(!kernel_frame_reg_arg(&frame, 0u, NULL));
}

int main(void)
{
    printf("kernel memory HLE tests\n");

    test_allocator_places_memory_where_the_guest_can_reach_it();
    test_every_allocation_lands_below_4gb();
    test_virtual_allocations_land_below_4gb();
    test_heap_blocks_land_below_4gb();

    test_contiguous_memory_is_page_aligned();
    test_contiguous_ex_honours_alignment();
    test_reserve_uses_64k_granularity();

    test_allocation_size_round_trip();
    test_heap_size_guarded_header();
    test_heap_size_round_trip();

    test_freed_region_is_not_reachable_and_does_not_alias();
    test_destroyed_heap_handle_does_not_alias();
    test_heap_reuses_freed_blocks_without_overlap();
    test_double_free_is_refused_and_reported();

    test_heaps_are_isolated();
    test_destroying_a_heap_releases_everything();
    test_reset_releases_every_region_and_heap();
    test_heap_respects_its_maximum_size();
    test_heap_realloc_preserves_contents();

    test_physical_addresses_are_distinct_and_linear();
    test_physical_range_constraints_are_enforced();
    test_persist_flag_is_recorded();
    test_set_address_protect_is_recorded_and_validated();
    test_query_address_protect_reports_the_containing_region();

    test_kernel_stack_create_use_delete_replay();
    test_kernel_stack_returns_the_top_of_the_region();
    test_kernel_stack_delete_refuses_what_it_cannot_prove();
    test_kernel_stack_create_failure_and_oddities();

    test_lock_then_unlock_round_trip_is_silent();
    test_lock_covers_every_page_the_range_touches();
    test_locks_nest_per_page();
    test_many_pages_in_any_order();
    test_unlock_without_lock_is_reported_and_does_not_underflow();
    test_unmeasured_modes_are_refused_loudly();
    test_zero_length_is_a_silent_no_op_and_wrap_is_refused();
    test_the_lock_table_grows_past_its_initial_capacity();
    test_lock_outside_tracked_committed_memory_is_reported_but_recorded();
    test_lock_with_no_readable_arguments_is_reported();
    test_registration_clears_the_lock_table();
    test_physical_unlock_releases_what_the_virtual_lock_took();
    test_a_mid_page_physical_address_names_its_page();
    test_physical_unlock_shares_the_virtual_lock_depth();
    test_physical_unlock_of_an_unlocked_page_is_reported();
    test_physical_lock_mode_and_odd_values_are_refused_loudly();
    test_an_unmapped_physical_address_is_refused();
    test_physical_unlock_touches_only_its_own_region();
    test_virtual_from_physical_is_the_inverse_of_the_physical_map();
    test_physical_unlock_with_no_readable_arguments_is_reported();
    test_registration_clears_the_physical_refusal_count();
    test_concurrent_locks_lose_no_count();

    test_virtual_alloc_and_release_round_trip();
    test_reserve_then_commit_keeps_the_same_base();
    test_query_virtual_memory_reports_the_region();

    test_allocation_failure_status_codes();
    test_region_alloc_status_codes();
    test_free_failure_status_codes();
    test_query_failure_status_codes();
    test_contiguous_free_rejects_a_non_base();
    test_missing_argument_frame_is_reported();

    test_registration_raises_implemented_count_exactly();
    test_implemented_ordinals_are_silent();
    test_fastcall_register_arguments();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
