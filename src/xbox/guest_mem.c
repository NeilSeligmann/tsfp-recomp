/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See guest_mem.h for the 4 GB rule and the full list of what is and is not
 * modelled. Nothing in this file knows about ordinals.
 */

#include "guest_mem.h"
#include "kernel_call.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kernel_call.h"

/* One past the last guest address. */
#define GUEST_ADDRESS_LIMIT 0x100000000ULL

/* How far apart successive placement attempts are when hinting explicitly. Large
 * enough that a few dozen attempts sweep a useful span of the window. */
#define GUEST_LOWMEM_HINT_STRIDE 0x00400000u
#define GUEST_LOWMEM_HINT_ATTEMPTS 64u

#define HEAP_SLOT_BITS 12u
#define HEAP_SLOT_MASK ((1u << HEAP_SLOT_BITS) - 1u)
#define HEAP_MAX_SLOTS 256u
#define HEAP_BLOCK_IN_USE 0x00000001u

/* Diagnostics go through the dispatcher's sink so that a test capturing kernel
 * output captures these too. Formatted here rather than forwarded as a va_list
 * because the sink is itself variadic and there is no portable way to pass one on. */
static void report(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    kernel_hle_log()("%s", line);
}

/* --------------------------------------------------------------------------
 * Arithmetic helpers. Every one of these exists because a silent wrap in an
 * allocator size produces a buffer smaller than the caller believes it got.
 * ----------------------------------------------------------------------- */

static bool is_power_of_two(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool align_up_u32(uint32_t value, uint32_t alignment, uint32_t *out)
{
    if (!is_power_of_two(alignment)) {
        return false;
    }
    uint32_t mask = alignment - 1u;
    if (value > UINT32_MAX - mask) {
        return false;
    }
    *out = (value + mask) & ~mask;
    return true;
}

static uint32_t align_down_u32(uint32_t value, uint32_t alignment)
{
    return value & ~(alignment - 1u);
}

/* --------------------------------------------------------------------------
 * The low-4 GB page allocator.
 * ----------------------------------------------------------------------- */

static uint64_t mapped_bytes;

static void check_host_page_size(void)
{
    static bool checked;
    if (checked) {
        return;
    }
    checked = true;
    long host_page = sysconf(_SC_PAGESIZE);
    if (host_page > 0 && (uint32_t)host_page != GUEST_PAGE_SIZE) {
        /* Not fatal: a larger host page is still a multiple of 4 KB, so every
         * alignment guarantee still holds. Worth saying once, because it changes
         * how much memory a small allocation really costs. */
        report("guest_mem: host page size is %ld, guest model assumes %u\n", host_page,
               GUEST_PAGE_SIZE);
    }
}

/* Map `length` bytes somewhere below 4 GB, or at exactly `fixed_base`.
 * Returns the guest address, or 0. Never returns an address whose range crosses
 * the 4 GB limit. */
static kernel_guest_ptr lowmem_map(size_t length, kernel_guest_ptr fixed_base)
{
    check_host_page_size();

    if (length == 0u) {
        return 0u;
    }

    if (fixed_base != 0u) {
        if ((uint64_t)fixed_base + (uint64_t)length > GUEST_ADDRESS_LIMIT) {
            return 0u;
        }
        void *want = (void *)(uintptr_t)fixed_base;
        void *got = mmap(want, length, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (got == MAP_FAILED) {
            return 0u;
        }
        if (got != want) {
            /* MAP_FIXED_NOREPLACE should make this impossible, but an older kernel
             * silently degrades it to a hint, and accepting a different address
             * would break the caller's promise that the base is honoured. */
            munmap(got, length);
            return 0u;
        }
        mapped_bytes += length;
        return fixed_base;
    }

#ifdef MAP_32BIT
    /* The direct route on x86-64 Linux: the kernel picks an address in the low
     * 2 GB for us. */
    void *got = mmap(NULL, length, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (got != MAP_FAILED) {
        uintptr_t base = (uintptr_t)got;
        if ((uint64_t)base + (uint64_t)length <= GUEST_ADDRESS_LIMIT) {
            mapped_bytes += length;
            return (kernel_guest_ptr)base;
        }
        /* Cannot happen by definition of MAP_32BIT, but if it ever does, give the
         * memory back rather than hand out a pointer that will be truncated. */
        munmap(got, length);
    }
#endif

    /* Fallback: hint our way through a window that is clear of a loaded XBE. The
     * result is verified rather than trusted, because a plain hint is advisory. */
    for (uint32_t attempt = 0; attempt < GUEST_LOWMEM_HINT_ATTEMPTS; attempt++) {
        uint64_t hint = (uint64_t)GUEST_LOWMEM_HINT_BASE +
                        (uint64_t)attempt * GUEST_LOWMEM_HINT_STRIDE;
        if (hint + length > (uint64_t)GUEST_LOWMEM_HINT_LIMIT) {
            break;
        }
        void *at = mmap((void *)(uintptr_t)hint, length, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (at == MAP_FAILED) {
            continue;
        }
        uintptr_t base = (uintptr_t)at;
        if ((uint64_t)base + (uint64_t)length <= GUEST_ADDRESS_LIMIT) {
            mapped_bytes += length;
            return (kernel_guest_ptr)base;
        }
        munmap(at, length);
    }

    /* Fail loudly. The guest stores what we return in four bytes, so there is no
     * correct way to satisfy this request from above 4 GB. */
    report("guest_mem: FAILED to place %zu bytes below 4 GB -- refusing to return a "
           "pointer the guest cannot store\n",
           length);
    return 0u;
}

static void lowmem_unmap(kernel_guest_ptr base, size_t length)
{
    if (base == 0u || length == 0u) {
        return;
    }
    kernel_guest_probe_change_begin();
    munmap((void *)(uintptr_t)base, length);
    kernel_guest_probe_cache_flush();
    if (mapped_bytes >= (uint64_t)length) {
        mapped_bytes -= (uint64_t)length;
    }
}

/* Allocate `bytes` rounded up to `granularity`, aligned to `alignment`.
 * Reports the mapping to unmap later, which is not the same as the aligned
 * address handed out.
 *
 * `lead_bytes` maps that many zero, readable pages BELOW the mapping the address is
 * carved from. They are slack: never part of the region the guest owns, but a read
 * just under the base lands on them instead of on an unmapped page. Ignored for a
 * fixed base, which must start exactly there. */
static bool lowmem_alloc(uint32_t bytes, uint32_t granularity, uint32_t alignment,
                         uint32_t lead_bytes, kernel_guest_ptr fixed_base,
                         kernel_guest_ptr *out_address,
                         uint32_t *out_size, kernel_guest_ptr *out_map_base,
                         size_t *out_map_length)
{
    uint32_t size = 0;
    if (!align_up_u32(bytes, granularity, &size) || size == 0u) {
        return false;
    }

    /* mmap already returns a page-aligned base, so only a stricter alignment needs
     * slack, and at most alignment - page bytes of it. */
    size_t map_length = size;
    if (alignment > GUEST_PAGE_SIZE) {
        map_length = (size_t)size + (size_t)(alignment - GUEST_PAGE_SIZE);
    }

    const size_t lead = fixed_base == 0u ? (size_t)lead_bytes : 0u;
    const kernel_guest_ptr lead_base = lowmem_map(map_length + lead, fixed_base);
    if (lead_base == 0u) {
        return false;
    }
    const kernel_guest_ptr map_base = lead_base + (kernel_guest_ptr)lead;

    uint32_t address = 0;
    if (!align_up_u32(map_base, alignment, &address)) {
        lowmem_unmap(lead_base, map_length + lead);
        return false;
    }
    if ((uint64_t)address + (uint64_t)size > (uint64_t)map_base + (uint64_t)map_length) {
        lowmem_unmap(lead_base, map_length + lead);
        return false;
    }
    if (fixed_base != 0u && address != fixed_base) {
        lowmem_unmap(lead_base, map_length + lead);
        return false;
    }

    *out_address = address;
    *out_size = size;
    /* The unmap range includes the lead-in, so freeing the region returns it too. */
    *out_map_base = lead_base;
    *out_map_length = map_length + lead;
    return true;
}

/* --------------------------------------------------------------------------
 * Synthetic physical addresses. See guest_mem.h: distinct and linear, but not
 * hardware addresses.
 * ----------------------------------------------------------------------- */

static uint64_t physical_cursor = GUEST_PAGE_SIZE;
static uint64_t allocation_generation;

static bool physical_reserve(uint32_t size, uint32_t lowest, uint32_t highest,
                             uint32_t alignment, uint32_t *out);

/* --------------------------------------------------------------------------
 * The region registry.
 * ----------------------------------------------------------------------- */

typedef struct {
    guest_region info;
    guest_memory_usage usage;
    kernel_guest_ptr map_base;
    size_t map_length;
    bool in_use;
} region_slot;

static region_slot *regions;
static size_t region_capacity;
static size_t region_live;

/* Find a complete aligned range among live intervals. Candidate only advances
 * past an overlap, so each live region can obstruct it at most once. Page zero
 * stays unavailable because the public inverse uses zero as its failure value. */
static bool physical_find(uint32_t size, uint64_t start, uint32_t top,
                          uint32_t alignment, uint32_t *out)
{
    const uint64_t mask = (uint64_t)alignment - 1u;
    uint64_t candidate = (start + mask) & ~mask;
    while (candidate + size <= (uint64_t)top + 1u) {
        uint64_t next = candidate;
        for (size_t i = 0u; i < region_capacity; i++) {
            if (!regions[i].in_use) continue;
            const guest_region *region = &regions[i].info;
            const uint64_t end = (uint64_t)region->physical + region->size;
            if (candidate < end && (uint64_t)region->physical < candidate + size && end > next)
                next = end;
        }
        if (next == candidate) {
            *out = (uint32_t)candidate;
            return true;
        }
        candidate = (next + mask) & ~mask;
    }
    return false;
}

static bool physical_reserve(uint32_t size, uint32_t lowest, uint32_t highest,
                             uint32_t alignment, uint32_t *out)
{
    const uint32_t align = alignment > GUEST_PAGE_SIZE ? alignment : GUEST_PAGE_SIZE;
    const uint32_t top = highest == 0u ? UINT32_MAX : highest;
    const uint64_t floor = lowest > GUEST_PAGE_SIZE ? lowest : GUEST_PAGE_SIZE;
    if (size == 0u || floor > top) return false;
    uint32_t physical;
    /* Preserve unconstrained placement preference without allowing its highwater
     * to turn an otherwise free constrained window into false exhaustion. */
    const uint64_t preferred = top == UINT32_MAX && physical_cursor > floor ? physical_cursor : floor;
    if (!physical_find(size, preferred, top, align, &physical) &&
        (preferred == floor || !physical_find(size, floor, top, align, &physical))) return false;
    const uint64_t end = (uint64_t)physical + size;
    if (physical_cursor < end) physical_cursor = end;
    *out = physical;
    return true;
}

static bool regions_claim(size_t *out_index)
{
    for (size_t i = 0; i < region_capacity; i++) {
        if (!regions[i].in_use) {
            *out_index = i;
            return true;
        }
    }

    size_t grown = region_capacity ? region_capacity * 2u : 32u;
    /* Host book-keeping, so it may live anywhere in the 64-bit space: the guest
     * never sees these addresses. */
    region_slot *resized = realloc(regions, grown * sizeof(*regions));
    if (!resized) {
        report("guest_mem: out of host memory growing the region registry to %zu\n",
               grown);
        return false;
    }
    memset(resized + region_capacity, 0, (grown - region_capacity) * sizeof(*resized));
    regions = resized;
    *out_index = region_capacity;
    region_capacity = grown;
    return true;
}

static kernel_guest_ptr guest_region_alloc_nolock(const guest_region_request *request,
                                   nt_status *status)
{
    nt_status local = STATUS_SUCCESS;
    if (!status) {
        status = &local;
    }

    if (!request || request->bytes == 0u) {
        *status = STATUS_INVALID_PARAMETER;
        return 0u;
    }
    if (!nt_page_protect_valid(request->protect)) {
        *status = STATUS_INVALID_PAGE_PROTECTION;
        return 0u;
    }
    if ((request->state & (MEM_COMMIT | MEM_RESERVE)) == 0u) {
        *status = STATUS_INVALID_PARAMETER;
        return 0u;
    }
    if (request->alignment != 0u && !is_power_of_two(request->alignment)) {
        *status = STATUS_INVALID_PARAMETER;
        return 0u;
    }
    if (request->highest_physical != 0u &&
        request->lowest_physical > request->highest_physical) {
        *status = STATUS_INVALID_PARAMETER;
        return 0u;
    }

    /* A reserve works in 64 KB units as on the real hardware; a bare commit works
     * in pages. */
    uint32_t granularity = (request->state & MEM_RESERVE) ? GUEST_ALLOCATION_GRANULARITY
                                                          : GUEST_PAGE_SIZE;
    uint32_t alignment = request->alignment ? request->alignment : granularity;
    if (alignment < granularity) {
        alignment = granularity;
    }

    kernel_guest_ptr fixed_base = request->fixed_base;
    if (fixed_base != 0u) {
        /* NT rounds a requested base down to granularity rather than refusing it. */
        fixed_base = align_down_u32(fixed_base, granularity);
        if (fixed_base == 0u) {
            *status = STATUS_INVALID_PARAMETER;
            return 0u;
        }
        if ((fixed_base & (alignment - 1u)) != 0u) {
            *status = STATUS_CONFLICTING_ADDRESSES;
            return 0u;
        }
    }

    if (allocation_generation == UINT64_MAX) {
        report("guest_mem: allocation generation exhausted\n");
        *status = STATUS_NO_MEMORY;
        return 0u;
    }

    size_t index = 0;
    if (!regions_claim(&index)) {
        *status = STATUS_NO_MEMORY;
        return 0u;
    }

    kernel_guest_ptr address = 0;
    uint32_t size = 0;
    kernel_guest_ptr map_base = 0;
    size_t map_length = 0;
    const uint32_t lead_bytes = request->contiguous ? GUEST_CONTIGUOUS_LEAD_BYTES : 0u;
    if (!lowmem_alloc(request->bytes, granularity, alignment, lead_bytes, fixed_base,
                      &address, &size, &map_base, &map_length)) {
        *status = fixed_base != 0u ? STATUS_CONFLICTING_ADDRESSES : STATUS_NO_MEMORY;
        return 0u;
    }

    uint32_t physical = 0;
    if (!physical_reserve(size, request->lowest_physical, request->highest_physical,
                          alignment, &physical)) {
        lowmem_unmap(map_base, map_length);
        report("guest_mem: no synthetic physical range for %u bytes in [%#x, %#x]\n", size,
               request->lowest_physical, request->highest_physical);
        *status = STATUS_NO_MEMORY;
        return 0u;
    }

    region_slot *slot = &regions[index];
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->map_base = map_base;
    slot->map_length = map_length;
    slot->info.address = address;
    slot->info.size = size;
    slot->info.requested = request->bytes;
    slot->info.physical = physical;
    slot->info.generation = ++allocation_generation;
    slot->info.protect = request->protect;
    slot->info.alloc_protect = request->protect;
    slot->info.state = (request->state & MEM_COMMIT) ? MEM_COMMIT : MEM_RESERVE;
    slot->info.contiguous = request->contiguous;
    region_live++;

    *status = STATUS_SUCCESS;
    return address;
}

static region_slot *region_slot_at(kernel_guest_ptr base)
{
    for (size_t i = 0; i < region_capacity; i++) {
        if (regions[i].in_use && regions[i].info.address == base) {
            return &regions[i];
        }
    }
    return NULL;
}

static region_slot *region_slot_containing(kernel_guest_ptr addr)
{
    for (size_t i = 0; i < region_capacity; i++) {
        if (!regions[i].in_use) {
            continue;
        }
        const guest_region *info = &regions[i].info;
        if (addr >= info->address && (uint64_t)addr < (uint64_t)info->address + info->size) {
            return &regions[i];
        }
    }
    return NULL;
}

static const guest_region *guest_region_at_nolock(kernel_guest_ptr base)
{
    const region_slot *slot = region_slot_at(base);
    return slot ? &slot->info : NULL;
}

static const guest_region *guest_region_containing_nolock(kernel_guest_ptr addr)
{
    const region_slot *slot = region_slot_containing(addr);
    return slot ? &slot->info : NULL;
}

static bool guest_region_free_nolock(kernel_guest_ptr base)
{
    region_slot *slot = region_slot_at(base);
    if (!slot) {
        return false;
    }
    lowmem_unmap(slot->map_base, slot->map_length);
    memset(slot, 0, sizeof(*slot));
    if (region_live > 0u) {
        region_live--;
    }
    return true;
}

static bool guest_region_set_protect_nolock(kernel_guest_ptr base, uint32_t bytes, uint32_t protect)
{
    /* Range-granular protection is not modelled: the value is recorded against the
     * whole region. A title that protects a single page inside a larger allocation
     * gets a region-wide record, which NtQueryVirtualMemory will then report for
     * every page in it. */
    (void)bytes;
    region_slot *slot = region_slot_containing(base);
    if (!slot) {
        return false;
    }
    slot->info.protect = protect;
    return true;
}

static bool guest_region_set_persist_nolock(kernel_guest_ptr base, bool persist)
{
    region_slot *slot = region_slot_at(base);
    if (!slot) {
        return false;
    }
    slot->info.persist = persist;
    return true;
}

static bool guest_region_set_state_nolock(kernel_guest_ptr base, uint32_t state)
{
    region_slot *slot = region_slot_at(base);
    if (!slot) {
        return false;
    }
    slot->info.state = (state & MEM_COMMIT) ? MEM_COMMIT : MEM_RESERVE;
    return true;
}

static uint32_t guest_physical_address_nolock(kernel_guest_ptr addr)
{
    const region_slot *slot = region_slot_containing(addr);
    if (!slot) {
        return 0u;
    }
    /* Linear within the region, so guest code comparing two addresses inside one
     * allocation sees the same relationship it would on hardware. */
    return slot->info.physical + (addr - slot->info.address);
}

static bool guest_virtual_from_physical_nolock(uint32_t physical, kernel_guest_ptr *out)
{
    /* Page zero is reserved, so zero is never a live physical address. */
    for (size_t i = 0; i < region_capacity; i++) {
        if (!regions[i].in_use) {
            continue;
        }
        const guest_region *info = &regions[i].info;
        if (physical >= info->physical &&
            (uint64_t)physical < (uint64_t)info->physical + info->size) {
            *out = info->address + (physical - info->physical);
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Heaps.
 *
 * Each heap owns one or more arena chunks taken from the page allocator, and
 * suballocates inside them with boundary-tagged blocks: first-fit placement,
 * splitting on allocate, coalescing with both neighbours on free.
 * ----------------------------------------------------------------------- */

/* Block header, in guest memory ahead of each payload. Every field is a uint32_t
 * so the layout is identical whatever the host pointer width -- a host pointer in
 * here would be eight bytes and would silently shift every payload. */
typedef struct {
    /* Payload bytes, a multiple of GUEST_HEAP_ALIGNMENT and never 0. */
    uint32_t size;
    /* Payload bytes of the preceding block, 0 only for the first block in a chunk. */
    uint32_t prev_size;
    /* What the caller asked for. 0 while free. */
    uint32_t requested;
    uint32_t flags;
} heap_block_header;

_Static_assert(sizeof(heap_block_header) == 16u,
               "heap block header must be 16 bytes to keep payloads 16-byte aligned");
_Static_assert(sizeof(heap_block_header) == GUEST_HEAP_ALIGNMENT,
               "block header size and payload alignment are assumed equal");

#define HEAP_HEADER_BYTES ((uint32_t)sizeof(heap_block_header))

typedef struct {
    kernel_guest_ptr base; /* first block header */
    uint32_t length;       /* total bytes of block space, headers included */
    kernel_guest_ptr map_base;
    size_t map_length;
} heap_chunk;

typedef struct {
    uint32_t token; /* 0 when the slot is free */
    uint32_t generation;
    uint32_t flags;
    uint32_t maximum_size; /* 0 means growable without limit */
    uint32_t committed;
    guest_memory_usage usage;
    heap_chunk *chunks;
    size_t chunk_count;
    size_t chunk_capacity;
    bool in_use;
} heap_slot;

static heap_slot heaps[HEAP_MAX_SLOTS];
static size_t heap_live;

static bool block_read(kernel_guest_ptr at, heap_block_header *out)
{
    const void *host = kernel_guest_at(at, sizeof(*out));
    if (!host) {
        return false;
    }
    memcpy(out, host, sizeof(*out));
    return true;
}

static bool block_write(kernel_guest_ptr at, const heap_block_header *in)
{
    void *host = kernel_guest_at(at, sizeof(*in));
    if (!host) {
        return false;
    }
    memcpy(host, in, sizeof(*in));
    return true;
}

static heap_slot *heap_lookup(uint32_t handle)
{
    if (handle == 0u) {
        return NULL;
    }
    uint32_t slot_index = handle & HEAP_SLOT_MASK;
    if (slot_index == 0u || slot_index > HEAP_MAX_SLOTS) {
        return NULL;
    }
    heap_slot *slot = &heaps[slot_index - 1u];
    if (!slot->in_use) {
        return NULL;
    }
    /* The generation in the handle's high bits is what makes a destroyed handle
     * detectable rather than merely dangling. */
    if (slot->token != handle) {
        return NULL;
    }
    return slot;
}

static bool guest_heap_valid_nolock(uint32_t handle)
{
    return heap_lookup(handle) != NULL;
}

/**
 * Add a chunk to a heap.
 *
 * `must_fit` separates the two callers. An allocation that cannot be placed needs a
 * chunk big enough for `need` or nothing; a heap being created is only committing
 * its initial size up front, and a smaller chunk is a perfectly good answer. Without
 * that distinction a heap whose initial size equals its maximum can never be
 * created, because the in-band block header pushes the first chunk past the limit.
 */
static bool heap_grow(heap_slot *heap, uint32_t need, bool must_fit)
{
    uint32_t wanted = 0;
    if (!align_up_u32(need, HEAP_HEADER_BYTES, &wanted)) {
        return false;
    }
    /* Room for the block header, plus a floor so a heap does not take a syscall
     * per small allocation. */
    if (wanted > UINT32_MAX - HEAP_HEADER_BYTES) {
        return false;
    }
    wanted += HEAP_HEADER_BYTES;
    if (wanted < GUEST_HEAP_CHUNK_MIN) {
        wanted = GUEST_HEAP_CHUNK_MIN;
    }

    uint32_t chunk_bytes = 0;
    if (!align_up_u32(wanted, GUEST_PAGE_SIZE, &chunk_bytes)) {
        return false;
    }

    if (heap->maximum_size != 0u) {
        if (heap->committed >= heap->maximum_size) {
            return false;
        }
        uint32_t budget = heap->maximum_size - heap->committed;
        if (chunk_bytes > budget) {
            /* Grow into whatever of the budget remains rather than refusing
             * outright: a capped heap should fill up, not fail early. */
            uint32_t reduced = align_down_u32(budget, GUEST_PAGE_SIZE);
            if (must_fit) {
                if ((uint64_t)reduced < (uint64_t)need + HEAP_HEADER_BYTES) {
                    return false;
                }
            } else if (reduced < HEAP_HEADER_BYTES + GUEST_HEAP_ALIGNMENT) {
                return false;
            }
            chunk_bytes = reduced;
        }
    }

    if (heap->chunk_count == heap->chunk_capacity) {
        size_t grown = heap->chunk_capacity ? heap->chunk_capacity * 2u : 4u;
        heap_chunk *resized = realloc(heap->chunks, grown * sizeof(*resized));
        if (!resized) {
            report("guest_mem: out of host memory growing a heap's chunk list\n");
            return false;
        }
        heap->chunks = resized;
        heap->chunk_capacity = grown;
    }

    kernel_guest_ptr address = 0;
    uint32_t size = 0;
    kernel_guest_ptr map_base = 0;
    size_t map_length = 0;
    if (!lowmem_alloc(chunk_bytes, GUEST_PAGE_SIZE, GUEST_HEAP_ALIGNMENT, 0u, 0u, &address,
                      &size, &map_base, &map_length)) {
        return false;
    }

    /* One free block spanning the chunk. */
    heap_block_header first = {
        .size = size - HEAP_HEADER_BYTES,
        .prev_size = 0u,
        .requested = 0u,
        .flags = 0u,
    };
    if (!block_write(address, &first)) {
        lowmem_unmap(map_base, map_length);
        return false;
    }

    heap_chunk *chunk = &heap->chunks[heap->chunk_count++];
    chunk->base = address;
    chunk->length = size;
    chunk->map_base = map_base;
    chunk->map_length = map_length;
    heap->committed += size;
    return true;
}

static uint32_t guest_heap_create_nolock(uint32_t flags, uint32_t initial_size, uint32_t maximum_size)
{
    if (maximum_size != 0u && initial_size > maximum_size) {
        return 0u;
    }

    heap_slot *slot = NULL;
    uint32_t slot_index = 0;
    for (uint32_t i = 0; i < HEAP_MAX_SLOTS; i++) {
        if (!heaps[i].in_use) {
            slot = &heaps[i];
            slot_index = i + 1u;
            break;
        }
    }
    if (!slot) {
        report("guest_mem: all %u heap slots are in use\n", HEAP_MAX_SLOTS);
        return 0u;
    }

    /* The generation survives destruction, so a reused slot never issues a handle
     * it has issued before. */
    uint32_t generation = slot->generation + 1u;
    if (generation > (UINT32_MAX >> HEAP_SLOT_BITS)) {
        report("guest_mem: heap slot %u has exhausted its handle generations\n",
               slot_index);
        return 0u;
    }

    heap_chunk *keep_chunks = slot->chunks;
    size_t keep_capacity = slot->chunk_capacity;
    memset(slot, 0, sizeof(*slot));
    slot->chunks = keep_chunks;
    slot->chunk_capacity = keep_capacity;
    slot->in_use = true;
    slot->generation = generation;
    slot->flags = flags;
    slot->maximum_size = maximum_size;
    slot->token = (generation << HEAP_SLOT_BITS) | slot_index;

    if (initial_size != 0u && !heap_grow(slot, initial_size, false)) {
        slot->in_use = false;
        slot->token = 0u;
        return 0u;
    }

    heap_live++;
    return slot->token;
}

static void heap_release_chunks(heap_slot *heap)
{
    for (size_t i = 0; i < heap->chunk_count; i++) {
        lowmem_unmap(heap->chunks[i].map_base, heap->chunks[i].map_length);
    }
    heap->chunk_count = 0u;
    heap->committed = 0u;
}

static bool guest_heap_destroy_nolock(uint32_t handle)
{
    heap_slot *heap = heap_lookup(handle);
    if (!heap) {
        return false;
    }
    heap_release_chunks(heap);
    heap->in_use = false;
    heap->token = 0u;
    heap->maximum_size = 0u;
    heap->flags = 0u;
    if (heap_live > 0u) {
        heap_live--;
    }
    return true;
}

/* Patch the prev_size of whichever block follows the one at `offset`. */
static bool heap_fix_following(const heap_chunk *chunk, uint32_t offset, uint32_t size)
{
    uint64_t following = (uint64_t)offset + HEAP_HEADER_BYTES + size;
    if (following >= (uint64_t)chunk->length) {
        return true; /* nothing follows */
    }
    heap_block_header header;
    kernel_guest_ptr at = (kernel_guest_ptr)(chunk->base + following);
    if (!block_read(at, &header)) {
        return false;
    }
    header.prev_size = size;
    return block_write(at, &header);
}

static kernel_guest_ptr heap_alloc_from_chunk(const heap_chunk *chunk, uint32_t need,
                                             uint32_t requested)
{
    uint32_t offset = 0;
    while (offset < chunk->length) {
        heap_block_header header;
        kernel_guest_ptr at = (kernel_guest_ptr)(chunk->base + offset);
        if (!block_read(at, &header)) {
            return 0u;
        }
        /* A zero size would make this loop spin; it can only mean corruption. */
        if (header.size == 0u) {
            report("guest_mem: heap block at %#x has zero size -- arena corrupt\n", at);
            return 0u;
        }

        if ((header.flags & HEAP_BLOCK_IN_USE) == 0u && header.size >= need) {
            uint32_t remainder = header.size - need;
            /* Split only when the tail can hold a header plus a minimum payload;
             * otherwise the whole block goes to the caller. */
            if (remainder >= HEAP_HEADER_BYTES + GUEST_HEAP_ALIGNMENT) {
                uint32_t tail_offset = offset + HEAP_HEADER_BYTES + need;
                uint32_t tail_size = remainder - HEAP_HEADER_BYTES;
                heap_block_header tail = {
                    .size = tail_size,
                    .prev_size = need,
                    .requested = 0u,
                    .flags = 0u,
                };
                if (!block_write((kernel_guest_ptr)(chunk->base + tail_offset), &tail)) {
                    return 0u;
                }
                if (!heap_fix_following(chunk, tail_offset, tail_size)) {
                    return 0u;
                }
                header.size = need;
            }
            header.flags |= HEAP_BLOCK_IN_USE;
            header.requested = requested;
            if (!block_write(at, &header)) {
                return 0u;
            }
            return (kernel_guest_ptr)(at + HEAP_HEADER_BYTES);
        }

        offset += HEAP_HEADER_BYTES + header.size;
    }
    return 0u;
}

static kernel_guest_ptr guest_heap_alloc_nolock(uint32_t handle, uint32_t bytes)
{
    heap_slot *heap = heap_lookup(handle);
    if (!heap) {
        return 0u;
    }

    /* A zero-byte request still returns a distinct, freeable address, as the real
     * heap does; rounding up to the minimum payload is what makes that possible. */
    uint32_t need = 0;
    if (!align_up_u32(bytes == 0u ? 1u : bytes, GUEST_HEAP_ALIGNMENT, &need)) {
        return 0u;
    }

    for (size_t i = 0; i < heap->chunk_count; i++) {
        kernel_guest_ptr got = heap_alloc_from_chunk(&heap->chunks[i], need, bytes);
        if (got != 0u) {
            return got;
        }
    }

    if (!heap_grow(heap, need, true)) {
        return 0u;
    }
    return heap_alloc_from_chunk(&heap->chunks[heap->chunk_count - 1u], need, bytes);
}

/* Locate the chunk of THIS heap holding `ptr`, and the offset of its header.
 * Returning NULL for a pointer owned by a different heap is what gives heaps
 * isolation: a cross-heap free is refused rather than performed. */
static const heap_chunk *heap_find_block(const heap_slot *heap, kernel_guest_ptr ptr,
                                         uint32_t *out_offset)
{
    if (ptr < HEAP_HEADER_BYTES) {
        return NULL;
    }
    kernel_guest_ptr header_at = (kernel_guest_ptr)(ptr - HEAP_HEADER_BYTES);
    for (size_t i = 0; i < heap->chunk_count; i++) {
        const heap_chunk *chunk = &heap->chunks[i];
        if (header_at < chunk->base) {
            continue;
        }
        uint32_t offset = header_at - chunk->base;
        if ((uint64_t)offset + HEAP_HEADER_BYTES > (uint64_t)chunk->length) {
            continue;
        }
        /* Every header sits at a multiple of the alignment, so a pointer that does
         * not is not a block start and must not be treated as one. */
        if ((offset % GUEST_HEAP_ALIGNMENT) != 0u) {
            continue;
        }
        *out_offset = offset;
        return chunk;
    }
    return NULL;
}

static bool guest_heap_block_size_nolock(uint32_t handle, kernel_guest_ptr ptr, uint32_t *out_size)
{
    const heap_slot *heap = heap_lookup(handle);
    if (!heap || !out_size) {
        return false;
    }
    uint32_t offset = 0;
    const heap_chunk *chunk = heap_find_block(heap, ptr, &offset);
    if (!chunk) {
        return false;
    }
    heap_block_header header;
    if (!kernel_guest_read_bytes((kernel_guest_ptr)(chunk->base + offset),
                                 &header, sizeof(header))) {
        return false;
    }
    if ((header.flags & HEAP_BLOCK_IN_USE) == 0u) {
        return false;
    }
    *out_size = header.requested;
    return true;
}

static bool guest_heap_free_nolock(uint32_t handle, kernel_guest_ptr ptr)
{
    heap_slot *heap = heap_lookup(handle);
    if (!heap) {
        return false;
    }
    uint32_t offset = 0;
    const heap_chunk *chunk = heap_find_block(heap, ptr, &offset);
    if (!chunk) {
        return false;
    }

    kernel_guest_ptr at = (kernel_guest_ptr)(chunk->base + offset);
    heap_block_header header;
    if (!block_read(at, &header)) {
        return false;
    }
    if ((header.flags & HEAP_BLOCK_IN_USE) == 0u) {
        /* Double free. Reported rather than tolerated: the second free would
         * corrupt the free list and the symptom would surface elsewhere. */
        report("guest_mem: double free of heap block %#x\n", ptr);
        return false;
    }

    header.flags &= ~HEAP_BLOCK_IN_USE;
    header.requested = 0u;

    /* Coalesce forward. */
    uint64_t next_offset = (uint64_t)offset + HEAP_HEADER_BYTES + header.size;
    if (next_offset < (uint64_t)chunk->length) {
        heap_block_header next;
        if (!block_read((kernel_guest_ptr)(chunk->base + next_offset), &next)) {
            return false;
        }
        if ((next.flags & HEAP_BLOCK_IN_USE) == 0u) {
            header.size += HEAP_HEADER_BYTES + next.size;
        }
    }
    if (!block_write(at, &header)) {
        return false;
    }
    if (!heap_fix_following(chunk, offset, header.size)) {
        return false;
    }

    /* Coalesce backward. prev_size is only 0 for the first block in a chunk, so
     * offset itself is the reliable test for "is there a predecessor". */
    if (offset != 0u) {
        if (header.prev_size == 0u ||
            header.prev_size + HEAP_HEADER_BYTES > offset) {
            report("guest_mem: heap block %#x has an impossible prev_size %u\n", at,
                   header.prev_size);
            return false;
        }
        uint32_t prev_offset = offset - HEAP_HEADER_BYTES - header.prev_size;
        kernel_guest_ptr prev_at = (kernel_guest_ptr)(chunk->base + prev_offset);
        heap_block_header prev;
        if (!block_read(prev_at, &prev)) {
            return false;
        }
        if ((prev.flags & HEAP_BLOCK_IN_USE) == 0u) {
            prev.size += HEAP_HEADER_BYTES + header.size;
            if (!block_write(prev_at, &prev)) {
                return false;
            }
            if (!heap_fix_following(chunk, prev_offset, prev.size)) {
                return false;
            }
        }
    }
    return true;
}

static kernel_guest_ptr guest_heap_realloc_nolock(uint32_t handle, kernel_guest_ptr ptr, uint32_t bytes)
{
    if (ptr == 0u) {
        return guest_heap_alloc(handle, bytes);
    }

    const heap_slot *heap = heap_lookup(handle);
    if (!heap) {
        return 0u;
    }
    uint32_t offset = 0;
    const heap_chunk *chunk = heap_find_block(heap, ptr, &offset);
    if (!chunk) {
        return 0u;
    }
    heap_block_header header;
    if (!block_read((kernel_guest_ptr)(chunk->base + offset), &header)) {
        return 0u;
    }
    if ((header.flags & HEAP_BLOCK_IN_USE) == 0u) {
        return 0u;
    }

    /* Grows and shrinks both move the block when the payload no longer fits.
     * Shrinking in place is not modelled: it would leave the tail unreclaimed,
     * and the copy is cheap at bring-up scale. */
    if (bytes <= header.size) {
        header.requested = bytes;
        if (!block_write((kernel_guest_ptr)(chunk->base + offset), &header)) {
            return 0u;
        }
        return ptr;
    }

    kernel_guest_ptr moved = guest_heap_alloc(handle, bytes);
    if (moved == 0u) {
        return 0u;
    }
    void *destination = kernel_guest_at(moved, bytes);
    const void *source = kernel_guest_at(ptr, header.requested);
    if (!destination || !source) {
        guest_heap_free(handle, moved);
        return 0u;
    }
    memcpy(destination, source, header.requested);
    guest_heap_free(handle, ptr);
    return moved;
}

/* --------------------------------------------------------------------------
 * Teardown and statistics.
 * ----------------------------------------------------------------------- */

static void guest_mem_reset_nolock(void)
{
    for (size_t i = 0; i < region_capacity; i++) {
        if (regions[i].in_use) {
            lowmem_unmap(regions[i].map_base, regions[i].map_length);
            memset(&regions[i], 0, sizeof(regions[i]));
        }
    }
    region_live = 0u;

    for (uint32_t i = 0; i < HEAP_MAX_SLOTS; i++) {
        if (heaps[i].in_use) {
            heap_release_chunks(&heaps[i]);
            heaps[i].in_use = false;
            heaps[i].token = 0u;
        }
        free(heaps[i].chunks);
        heaps[i].chunks = NULL;
        heaps[i].chunk_capacity = 0u;
        heaps[i].chunk_count = 0u;
    }
    heap_live = 0u;

    free(regions);
    regions = NULL;
    region_capacity = 0u;

    /* Unconstrained placement highwater and allocation generations do not rewind.
     * Constrained free physical ranges can be reused; lifetime identities cannot. */
}

static size_t guest_mem_region_count_nolock(void)
{
    return region_live;
}

static size_t guest_mem_heap_count_nolock(void)
{
    return heap_live;
}

static uint64_t guest_mem_mapped_bytes_nolock(void)
{
    return mapped_bytes;
}

/* ---------------------------------------------------------------------------
 * THE LOCK.
 *
 * Everything above runs with `guest_mem_lock` held. The public entry points are
 * the thin wrappers below; the `_nolock` cores are what this file calls
 * internally. That split is the point: the hazards here are COMPOUND, not
 * per-word, so no amount of atomics would do.
 *
 * WHAT IT FIXES, specifically. Without it, two guest threads allocating
 * concurrently can:
 *   (a) both be handed the same free slot index by `regions_claim`, which does not
 *       mark the slot taken -- one `mmap` is then leaked forever and the guest's
 *       own free of that address reports "never allocated" for a pointer we did
 *       hand out;
 *   (b) race a `realloc` of the `regions` array against another thread iterating
 *       it, so `guest_region_free` reads a `map_base` out of freed HOST memory and
 *       `munmap`s garbage, or `memset`s into a freed allocation;
 *   (c) both search the same unclaimed physical range and give two live allocations the
 *       SAME synthetic physical address, which breaks the one invariant
 *       `MmGetPhysicalAddress` promises -- and a title uses it to decide whether
 *       two buffers are the same memory;
 *   (d) both split the same free heap block and return the SAME guest pointer to
 *       two callers, or coalesce a free across a block the other just allocated.
 *
 * RECURSIVE, DELIBERATELY. Two reasons, both live rather than theoretical:
 *   1. `guest_heap_realloc` calls the public `guest_heap_alloc` and
 *      `guest_heap_free`. A plain mutex self-deadlocks on the first guest
 *      `HeapReAlloc` -- every time, and easy to miss because realloc is rarer than
 *      alloc in a smoke test.
 *   2. `report()` runs INSIDE these critical sections and the log sink is
 *      caller-supplied via `kernel_hle_set_log`. A sink that asks for
 *      `guest_mem_region_count()` would deadlock a non-recursive lock, and the
 *      tests do install custom sinks.
 *
 * WHAT IT DOES NOT FIX, on the record, because a lock here cannot:
 *   - `guest_region_at` and `guest_region_containing` return a pointer INTO the
 *     region table, and that pointer outlives the lock. A caller holding one
 *     across another call can be looking at a slot that has since been freed and
 *     reused. Fixing it means returning a COPY, which changes the contract and
 *     every call site.
 *   - Check-then-act sequences spanning several calls are still not atomic: "is
 *     this region reserved?" then "commit it" is two critical sections, not one.
 *     That needs compound operations here, not a lock around each half.
 *
 * AND ONE HARD RULE, because a `siglongjmp` out of lifted code releases nothing:
 * no guest-memory access that can fault may happen while this lock is held.
 * Nothing above touches guest memory except the heap's own block headers, which
 * live in regions this file mapped itself.
 * ------------------------------------------------------------------------- */

static pthread_mutex_t guest_mem_lock;
static pthread_once_t guest_mem_lock_once = PTHREAD_ONCE_INIT;

static void guest_mem_lock_init(void)
{
    pthread_mutexattr_t attr;
    (void)pthread_mutexattr_init(&attr);
    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&guest_mem_lock, &attr);
    (void)pthread_mutexattr_destroy(&attr);
}

static void guest_mem_enter(void)
{
    (void)pthread_once(&guest_mem_lock_once, guest_mem_lock_init);
    (void)pthread_mutex_lock(&guest_mem_lock);
}

static void guest_mem_leave(void)
{
    (void)pthread_mutex_unlock(&guest_mem_lock);
}

kernel_guest_ptr guest_region_alloc(const guest_region_request *request,
                                   nt_status *status)
{
    guest_mem_enter();
    kernel_guest_ptr result = guest_region_alloc_nolock(request, status);
    guest_mem_leave();
    return result;
}

kernel_guest_ptr guest_region_alloc_typed(const guest_region_request *request,
                                         nt_status *status, guest_memory_usage usage)
{
    guest_mem_enter();
    kernel_guest_ptr result = guest_region_alloc_nolock(request, status);
    region_slot *slot = region_slot_at(result);
    if (slot) slot->usage = usage;
    guest_mem_leave();
    return result;
}

uint32_t guest_heap_create_typed(uint32_t flags, uint32_t initial_size,
                                 uint32_t maximum_size, guest_memory_usage usage)
{
    guest_mem_enter();
    uint32_t result = guest_heap_create_nolock(flags, initial_size, maximum_size);
    heap_slot *slot = heap_lookup(result);
    if (slot) slot->usage = usage;
    guest_mem_leave();
    return result;
}

bool guest_region_set_usage(kernel_guest_ptr base, uint32_t bytes, guest_memory_usage usage)
{
    guest_mem_enter();
    region_slot *slot = region_slot_at(base);
    bool valid = slot && slot->info.size == bytes && !slot->info.contiguous &&
                 (unsigned)usage <= GUEST_MEMORY_CACHE;
    if (valid) slot->usage = usage;
    guest_mem_leave();
    return valid;
}

static void statistics_backing(guest_memory_statistics *out, guest_memory_usage usage,
                               uint64_t bytes, bool committed)
{
    if (usage == GUEST_MEMORY_VIRTUAL) {
        out->virtual_reserved_bytes += bytes;
        if (committed) out->virtual_committed_bytes += bytes;
    } else if (committed) {
        uint64_t pages = bytes / GUEST_PAGE_SIZE;
        if (usage == GUEST_MEMORY_POOL) out->pool_pages += pages;
        if (usage == GUEST_MEMORY_STACK) out->stack_pages += pages;
        if (usage == GUEST_MEMORY_CACHE) out->cache_pages += pages;
    }
}

void guest_mem_statistics(guest_memory_statistics *out)
{
    if (!out) return;
    guest_mem_enter();
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < region_capacity; ++i) {
        const region_slot *slot = &regions[i];
        if (slot->in_use && !slot->info.contiguous)
            statistics_backing(out, slot->usage, slot->info.size,
                               slot->info.state == MEM_COMMIT);
    }
    for (size_t i = 0; i < HEAP_MAX_SLOTS; ++i) {
        if (!heaps[i].in_use) continue;
        /* Include headers/alignment and retained free arenas: these are mapped pages. */
        for (size_t j = 0; j < heaps[i].chunk_count; ++j)
            statistics_backing(out, heaps[i].usage, heaps[i].chunks[j].map_length, true);
    }
    guest_mem_leave();
}

const guest_region *guest_region_at(kernel_guest_ptr base)
{
    guest_mem_enter();
    const guest_region *result = guest_region_at_nolock(base);
    guest_mem_leave();
    return result;
}

const guest_region *guest_region_containing(kernel_guest_ptr addr)
{
    guest_mem_enter();
    const guest_region *result = guest_region_containing_nolock(addr);
    guest_mem_leave();
    return result;
}

bool guest_region_free(kernel_guest_ptr base)
{
    guest_mem_enter();
    bool result = guest_region_free_nolock(base);
    guest_mem_leave();
    return result;
}

bool guest_region_set_protect(kernel_guest_ptr base, uint32_t bytes, uint32_t protect)
{
    guest_mem_enter();
    bool result = guest_region_set_protect_nolock(base, bytes, protect);
    guest_mem_leave();
    return result;
}

bool guest_region_set_persist(kernel_guest_ptr base, bool persist)
{
    guest_mem_enter();
    bool result = guest_region_set_persist_nolock(base, persist);
    guest_mem_leave();
    return result;
}

bool guest_region_set_state(kernel_guest_ptr base, uint32_t state)
{
    guest_mem_enter();
    bool result = guest_region_set_state_nolock(base, state);
    guest_mem_leave();
    return result;
}

uint32_t guest_physical_address(kernel_guest_ptr addr)
{
    guest_mem_enter();
    uint32_t result = guest_physical_address_nolock(addr);
    guest_mem_leave();
    return result;
}

uint64_t guest_allocation_generation(kernel_guest_ptr addr)
{
    guest_mem_enter();
    const region_slot *slot = region_slot_containing(addr);
    const uint64_t result = slot != NULL ? slot->info.generation : 0u;
    guest_mem_leave();
    return result;
}

bool guest_virtual_from_physical(uint32_t physical, kernel_guest_ptr *out_address)
{
    guest_mem_enter();
    const bool result = out_address != NULL &&
                        guest_virtual_from_physical_nolock(physical, out_address);
    guest_mem_leave();
    return result;
}

bool guest_heap_valid(uint32_t handle)
{
    guest_mem_enter();
    bool result = guest_heap_valid_nolock(handle);
    guest_mem_leave();
    return result;
}

uint32_t guest_heap_create(uint32_t flags, uint32_t initial_size, uint32_t maximum_size)
{
    guest_mem_enter();
    uint32_t result = guest_heap_create_nolock(flags, initial_size, maximum_size);
    guest_mem_leave();
    return result;
}

bool guest_heap_destroy(uint32_t handle)
{
    guest_mem_enter();
    bool result = guest_heap_destroy_nolock(handle);
    guest_mem_leave();
    return result;
}

kernel_guest_ptr guest_heap_alloc(uint32_t handle, uint32_t bytes)
{
    guest_mem_enter();
    kernel_guest_ptr result = guest_heap_alloc_nolock(handle, bytes);
    guest_mem_leave();
    return result;
}

bool guest_heap_block_size(uint32_t handle, kernel_guest_ptr ptr, uint32_t *out_size)
{
    guest_mem_enter();
    bool result = guest_heap_block_size_nolock(handle, ptr, out_size);
    guest_mem_leave();
    return result;
}

bool guest_heap_free(uint32_t handle, kernel_guest_ptr ptr)
{
    guest_mem_enter();
    bool result = guest_heap_free_nolock(handle, ptr);
    guest_mem_leave();
    return result;
}

kernel_guest_ptr guest_heap_realloc(uint32_t handle, kernel_guest_ptr ptr, uint32_t bytes)
{
    guest_mem_enter();
    kernel_guest_ptr result = guest_heap_realloc_nolock(handle, ptr, bytes);
    guest_mem_leave();
    return result;
}

void guest_mem_reset(void)
{
    guest_mem_enter();
    guest_mem_reset_nolock();
    guest_mem_leave();
}

size_t guest_mem_region_count(void)
{
    guest_mem_enter();
    size_t result = guest_mem_region_count_nolock();
    guest_mem_leave();
    return result;
}

size_t guest_mem_heap_count(void)
{
    guest_mem_enter();
    size_t result = guest_mem_heap_count_nolock();
    guest_mem_leave();
    return result;
}

uint64_t guest_mem_mapped_bytes(void)
{
    guest_mem_enter();
    uint64_t result = guest_mem_mapped_bytes_nolock();
    guest_mem_leave();
    return result;
}
