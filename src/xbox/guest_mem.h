/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Guest-visible memory: the low-4 GB page allocator, the allocation registry, and
 * the heap. This is the machinery the Mm* and Nt*VirtualMemory ordinals sit on;
 * it has no ordinal knowledge of its own and is tested directly.
 *
 * THE 4 GB RULE. Every address handed to the guest must be below 2^32, because the
 * guest stores it in a four-byte pointer field. A plain malloc() on a 64-bit host
 * is free to return an address above 4 GB, and truncating one to fit would corrupt
 * the guest arbitrarily far from the allocation that caused it. So allocation goes
 * through mmap into the low 4 GB and FAILS LOUDLY if it cannot be placed there.
 * There is no fallback that narrows a pointer: a reported STATUS_NO_MEMORY is
 * recoverable, a silently truncated pointer is not.
 *
 * Host book-keeping lives in malloc'd sidecar arrays, which may sit anywhere in the
 * 64-bit space precisely because the guest never sees them. Only the pages the
 * guest is handed are constrained.
 *
 * WHAT IS MODELLED
 *   - 4 KB pages and 64 KB allocation granularity, as on the real hardware.
 *   - Alignment, honoured for real: an allocation asking for 64 KB alignment gets a
 *     64 KB-aligned address, not one that happens to be page-aligned.
 *   - Allocation sizes, tracked per region and per heap block, so MmQueryAllocationSize
 *     and the heap's size query answer from a record rather than guessing.
 *   - Reserve versus commit as distinct states.
 *   - A real suballocating heap with first-fit placement, block splitting and
 *     coalescing on free.
 *
 * WHAT IS NOT MODELLED -- these gaps are deliberate and on the record:
 *   - PAGE PROTECTION IS NOT ENFORCED. A protection value is recorded and reported
 *     back by NtQueryVirtualMemory, but no mprotect() is issued. Enforcing it would
 *     convert a fidelity gap into a host SIGSEGV with no diagnostic, which is
 *     strictly worse than a write that should have faulted and did not.
 *   - PHYSICAL ADDRESSES ARE SYNTHETIC. MmGetPhysicalAddress answers from a
 *     private host model, not from any hardware map. Live ranges are distinct,
 *     non-overlapping, page-aligned and translate linearly within a region, which
 *     is what guest code checking "are these two buffers contiguous" needs. They do
 *     NOT match real Xbox physical addresses, and the real kernel's
 *     virtual = physical + 0x80000000 relationship for the contiguous region does
 *     not hold here. Constrained windows use aligned first-fit over live ranges;
 *     freed physical bytes may be reused. Allocation generations are monotonic
 *     across free/reset and identify lifetimes independently of reused addresses.
 *   - NO 64 MB / 128 MB RAM BUDGET. The real console runs out of memory; we do not
 *     model the limit, so a title that depends on an allocation failing will not see
 *     one.
 *   - CONTIGUITY IS NOT VERIFIED. An mmap region is contiguous in the guest's
 *     virtual view, which is all the guest can observe, but nothing guarantees the
 *     host physical pages behind it are contiguous. No GPU DMA reads this memory
 *     yet, so the distinction is currently unobservable.
 *   - NO ZERO-ON-FREE AND NO GUARD PAGES (a contiguous region does get one readable
 *     zero lead-in page below it, see GUEST_CONTIGUOUS_LEAD_BYTES). Fresh mmap pages are zero, so a fresh
 *     allocation is zeroed; a recycled heap block is not.
 *   - CONCURRENT LIFETIME IS NOT PINNED. Allocator, registry and heap operations
 *     serialize through a recursive mutex (guest_mem.c). Returned region metadata
 *     and guest payload pointers outlive that lock only while their allocation
 *     stays live; another operation can free or move them. Compound check-then-act
 *     sequences span separate critical sections, and reset requires guest users
 *     to be quiescent.
 */

#ifndef TSFP_XBOX_GUEST_MEM_H
#define TSFP_XBOX_GUEST_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel_hle.h"
#include "nt_status.h"

/* Xbox page size. Matches the host's on x86-64, which is why mmap granularity
 * can be taken at face value; asserted at init rather than assumed. */
#define GUEST_PAGE_SIZE 0x1000u

/* Reserve/allocation granularity, 64 KB as on NT and the Xbox. A reserve rounds
 * its base down and its size up to this. */
#define GUEST_ALLOCATION_GRANULARITY 0x10000u

/* Where the allocator looks for low memory when it has to hint explicitly. Starts
 * at 1 GB to stay clear of a loaded XBE, which sits at 0x10000 plus its image
 * size, and stops below 4 GB so that no allocation can straddle the limit. */
#define GUEST_LOWMEM_HINT_BASE 0x40000000u
#define GUEST_LOWMEM_HINT_LIMIT 0xF0000000u

/* Zero, readable slack mapped under every contiguous region (T434). On the console
 * contiguous memory is a window onto flat physical RAM, so a read just under a block
 * is always mapped. The host kernel places each MAP_32BIT mmap at a random start, so
 * without this the page below a block was unmapped in about 3 percent of boots and
 * the title's table[-1] read (14 bytes under the 47 MB block) took a SIGSEGV. The
 * lead-in is not part of the region: guest_region_containing does not see it. */
#define GUEST_CONTIGUOUS_LEAD_BYTES GUEST_PAGE_SIZE

/** A tracked guest allocation: one contiguous or virtual region. */
typedef struct {
    /* Address handed to the guest, aligned as requested. */
    kernel_guest_ptr address;
    /* Usable bytes from `address`, rounded up as the modelled granularity requires. */
    uint32_t size;
    /* What the guest actually asked for, before rounding. Kept because it is the
     * only way to tell a 5-byte request from a 4096-byte one after the fact. */
    uint32_t requested;
    /* Synthetic physical base; see the header comment. Page-aligned, never 0. */
    uint32_t physical;
    uint32_t protect;
    /* Protection at allocation time, which NtQueryVirtualMemory reports separately
     * from the current one. */
    uint32_t alloc_protect;
    /* MEM_COMMIT or MEM_RESERVE. */
    uint32_t state;
    bool contiguous;
    /* Set by MmPersistContiguousMemory. Recorded only: reboots are not modelled. */
    bool persist;
    /* Nonzero lifetime identity, never reused even when physical/virtual bytes are. */
    uint64_t generation;
} guest_region;

/* Counter categories describe actual tracked backing, not requested payloads. */
typedef enum {
    GUEST_MEMORY_VIRTUAL = 0,
    GUEST_MEMORY_POOL,
    GUEST_MEMORY_STACK,
    GUEST_MEMORY_CACHE
} guest_memory_usage;

typedef struct {
    uint64_t virtual_committed_bytes;
    uint64_t virtual_reserved_bytes;
    uint64_t pool_pages;
    uint64_t stack_pages;
    uint64_t cache_pages;
} guest_memory_statistics;

/* Coherent registry snapshot under the memory lock; contiguous regions are not VM. */
void guest_mem_statistics(guest_memory_statistics *out);
/* Classify only a complete non-contiguous tracked mapping, under the registry lock. */
bool guest_region_set_usage(kernel_guest_ptr base, uint32_t bytes, guest_memory_usage usage);

/** What to allocate. A struct because the Ex variant has five dimensions. */
typedef struct {
    uint32_t bytes;
    /* Power of two, or 0 for page alignment. */
    uint32_t alignment;
    /* Synthetic physical window the allocation must fall inside. A `highest` of 0
     * is normalised to "no limit" so a zero-initialised request is unconstrained. */
    uint32_t lowest_physical;
    uint32_t highest_physical;
    uint32_t protect;
    /* MEM_COMMIT or MEM_RESERVE. */
    uint32_t state;
    bool contiguous;
    /* Non-zero to demand this exact base; the allocation fails rather than
     * silently landing elsewhere. */
    kernel_guest_ptr fixed_base;
} guest_region_request;

/**
 * Allocate a region. Returns its guest address, or 0 with `*status` set.
 *
 * Guaranteed on success: the address is below 2^32, is aligned as requested, and
 * `*status` is STATUS_SUCCESS. On failure the return is 0 and `*status` says why,
 * so a caller never has to infer the reason from the absence of an address.
 */
kernel_guest_ptr guest_region_alloc(const guest_region_request *request,
                                    nt_status *status);

/* Typed allocation is atomic with publication in the registry. */
kernel_guest_ptr guest_region_alloc_typed(const guest_region_request *request,
                                         nt_status *status, guest_memory_usage usage);
uint32_t guest_heap_create_typed(uint32_t flags, uint32_t initial_size,
                                 uint32_t maximum_size, guest_memory_usage usage);

/** Release a region by its exact base address. False if that is not a base. */
bool guest_region_free(kernel_guest_ptr base);

/** The region based exactly at `base`, or NULL. */
const guest_region *guest_region_at(kernel_guest_ptr base);

/** The region containing `addr` anywhere in its range, or NULL. */
const guest_region *guest_region_containing(kernel_guest_ptr addr);

/** Record a new protection over a range. False if no region holds the base. */
bool guest_region_set_protect(kernel_guest_ptr base, uint32_t bytes, uint32_t protect);

/** Record the persist flag. False if no region is based at `base`. */
bool guest_region_set_persist(kernel_guest_ptr base, bool persist);

/** Move a region between MEM_COMMIT and MEM_RESERVE. False if `base` is not a base. */
bool guest_region_set_state(kernel_guest_ptr base, uint32_t state);

/**
 * Translate a guest address to its synthetic physical address, or 0.
 *
 * Linear within a region, so `addr + n` maps to `physical + n` for any n inside
 * the region. 0 means "not a tracked address", which doubles as the real kernel's
 * failure return.
 */
uint32_t guest_physical_address(kernel_guest_ptr addr);
/* Current tracked allocation lifetime for an interior VA, or zero if absent.
 * Copies metadata under the lock; does not pin payload across concurrent free. */
uint64_t guest_allocation_generation(kernel_guest_ptr addr);

/**
 * The inverse of `guest_physical_address`: the guest address whose synthetic physical address
 * is `physical`, found by the tracked region whose physical range contains it. Physical ranges
 * cannot overlap while live, so the current answer is unique; freed ranges may be reused.
 * False, with `*out_address` untouched, for 0 and for any address no live region owns.
 */
bool guest_virtual_from_physical(uint32_t physical, kernel_guest_ptr *out_address);

/* ---------------------------------------------------------------------------
 * Heaps.
 *
 * NOT REACHABLE BY ORDINAL. The Xbox kernel export table has no heap functions at
 * all: RtlCreateHeap and friends live in the title's statically linked XAPI
 * library, not in xboxkrnl. This is the engine a future XAPI-level HLE binds to,
 * and it is exercised directly by tests in the meantime.
 *
 * A handle is an opaque 32-bit token, not an address. The real article is a pointer
 * to a control block in guest memory, but a token lets a destroyed handle be
 * *rejected* rather than merely dangle: the generation in its high bits will not
 * match the slot's after reuse, so a stale handle can never be mistaken for a live
 * heap that happens to have landed at the same address.
 * ------------------------------------------------------------------------- */

/** Smallest arena chunk a heap takes from the page allocator. */
#define GUEST_HEAP_CHUNK_MIN 0x10000u

/** Payload alignment every heap block satisfies. */
#define GUEST_HEAP_ALIGNMENT 16u

/** Create a heap. Returns a handle, or 0 on failure. `maximum_size` 0 means growable. */
uint32_t guest_heap_create(uint32_t flags, uint32_t initial_size, uint32_t maximum_size);

/** Destroy a heap and release every page it owns. False for an unknown handle. */
bool guest_heap_destroy(uint32_t handle);

/** True only for a live handle. A destroyed or never-issued handle is false. */
bool guest_heap_valid(uint32_t handle);

/** Allocate from a heap. Returns a guest address below 2^32, or 0. */
kernel_guest_ptr guest_heap_alloc(uint32_t handle, uint32_t bytes);

/**
 * Free a heap block.
 *
 * False when `handle` is dead, or when `ptr` was not allocated from THIS heap --
 * a block belonging to another heap is rejected rather than freed, so a
 * cross-heap free is a reported error instead of silent corruption.
 */
bool guest_heap_free(uint32_t handle, kernel_guest_ptr ptr);

/** The size originally requested for a block. False if the block is not this heap's. */
bool guest_heap_block_size(uint32_t handle, kernel_guest_ptr ptr, uint32_t *out_size);

/** Resize a block, preserving contents. Returns the new address, or 0 on failure. */
kernel_guest_ptr guest_heap_realloc(uint32_t handle, kernel_guest_ptr ptr, uint32_t bytes);

/* --------------------------------------------------------------------------- */

/** Release every region and heap. For shutdown, and to isolate tests. */
void guest_mem_reset(void);

/** Live region count. For tests asserting that a teardown released everything. */
size_t guest_mem_region_count(void);

/** Live heap count. */
size_t guest_mem_heap_count(void);

/** Total bytes the page allocator currently holds mapped. */
uint64_t guest_mem_mapped_bytes(void);

#endif /* TSFP_XBOX_GUEST_MEM_H */
