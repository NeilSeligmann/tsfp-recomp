/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_memory.h for the verified ordinal numbers and the scope boundary,
 * and guest_mem.h for what the underlying model does and does not reproduce.
 *
 * ASSUMED SIGNATURES. The Xbox kernel has a single address space, so its Nt memory
 * calls take no ProcessHandle, unlike desktop NT. The arities assumed here are:
 *
 *   PVOID     MmAllocateContiguousMemory(ULONG NumberOfBytes)
 *   PVOID     MmAllocateContiguousMemoryEx(ULONG NumberOfBytes,
 *                                          ULONG LowestAcceptableAddress,
 *                                          ULONG HighestAcceptableAddress,
 *                                          ULONG Alignment, ULONG ProtectionType)
 *   VOID      MmFreeContiguousMemory(PVOID BaseAddress)
 *   ULONG     MmGetPhysicalAddress(PVOID BaseAddress)
 *   VOID      MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes,
 *                                     BOOLEAN UnlockPages)
 *   VOID      MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes,
 *                                       BOOLEAN Persist)
 *   PVOID     MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread)
 *   VOID      MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit)
 *   ULONG     MmQueryAllocationSize(PVOID BaseAddress)
 *   VOID      MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes,
 *                                 ULONG NewProtect)
 *   NTSTATUS  NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *                                     PULONG AllocationSize, ULONG AllocationType,
 *                                     ULONG Protect)
 *   NTSTATUS  NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize, ULONG FreeType)
 *   NTSTATUS  NtQueryVirtualMemory(PVOID BaseAddress,
 *                                  PMEMORY_BASIC_INFORMATION Buffer)
 *
 * These are recorded explicitly because an arity error is silent: reading one slot
 * too far returns whatever the caller pushed next, which is often a plausible
 * number. If a disassembly of the guest's call sites contradicts one of these,
 * the signature above is the thing to fix.
 */

#include "kernel_memory.h"

#include <pthread.h>
#include <sys/sysinfo.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

/* The frame is supplied by the dispatcher's caller. A NULL one is a recompiler
 * bug rather than a guest error, so it reports every time -- like an unknown
 * ordinal, and for the same reason. */
static const kernel_call_frame *frame_of(void *context, const char *who)
{
    if (!context) {
        kernel_hle_log()("kernel: %s called with no argument frame -- the call "
                         "boundary did not supply one\n",
                         who);
        return NULL;
    }
    return (const kernel_call_frame *)context;
}

/* Fetch `count` arguments at once. Partial reads are useless to a handler: it
 * cannot act on half a signature. */
static bool frame_args(const kernel_call_frame *frame, uint32_t *out, unsigned count,
                       const char *who)
{
    for (unsigned i = 0; i < count; i++) {
        if (!kernel_frame_arg(frame, i, &out[i])) {
            kernel_hle_log()("kernel: %s could not read argument %u from the guest "
                             "stack\n",
                             who, i);
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Contiguous memory. On hardware this is the physically contiguous, page-aligned
 * memory GPU resources live in.
 * ----------------------------------------------------------------------- */

static uint32_t contiguous_alloc(uint32_t bytes, uint32_t lowest, uint32_t highest,
                                 uint32_t alignment, uint32_t protect)
{
    guest_region_request request = {
        .bytes = bytes,
        .alignment = alignment,
        .lowest_physical = lowest,
        .highest_physical = highest,
        .protect = protect,
        .state = MEM_COMMIT,
        .contiguous = true,
        .fixed_base = 0u,
    };
    nt_status status = STATUS_SUCCESS;
    kernel_guest_ptr address = guest_region_alloc(&request, &status);
    if (address == 0u) {
        /* The real function returns NULL, with no status channel, so the reason
         * only exists if we say it here. */
        kernel_hle_log()("kernel: MmAllocateContiguousMemory(%u) failed, status %#x\n",
                         bytes, status);
    }
    return address;
}

static uint32_t hle_mm_allocate_contiguous_memory(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmAllocateContiguousMemory");
    uint32_t args[1];
    if (!frame || !frame_args(frame, args, 1u, "MmAllocateContiguousMemory")) {
        return 0u;
    }
    /* The plain variant is the Ex variant with no constraints: anywhere in memory,
     * page-aligned, read/write. */
    return contiguous_alloc(args[0], 0u, 0u, GUEST_PAGE_SIZE, PAGE_READWRITE);
}

static uint32_t hle_mm_allocate_contiguous_memory_ex(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmAllocateContiguousMemoryEx");
    uint32_t args[5];
    if (!frame || !frame_args(frame, args, 5u, "MmAllocateContiguousMemoryEx")) {
        return 0u;
    }
    uint32_t bytes = args[0];
    uint32_t lowest = args[1];
    uint32_t highest = args[2];
    uint32_t alignment = args[3];
    uint32_t protect = args[4];

    /* Alignment 0 means "page", matching the plain variant. Anything else must be a
     * power of two; a non-power-of-two is a caller bug that would otherwise produce
     * a misaligned buffer and a GPU fault far away. */
    if (alignment == 0u) {
        alignment = GUEST_PAGE_SIZE;
    }
    if ((alignment & (alignment - 1u)) != 0u) {
        kernel_hle_log()("kernel: MmAllocateContiguousMemoryEx alignment %#x is not a "
                         "power of two\n",
                         alignment);
        return 0u;
    }
    if (protect == 0u) {
        protect = PAGE_READWRITE;
    }
    return contiguous_alloc(bytes, lowest, highest, alignment, protect);
}

static uint32_t hle_mm_free_contiguous_memory(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmFreeContiguousMemory");
    uint32_t args[1];
    if (!frame || !frame_args(frame, args, 1u, "MmFreeContiguousMemory")) {
        return STATUS_SUCCESS;
    }

    const guest_region *region = guest_region_at(args[0]);
    if (!region || !region->contiguous) {
        /* Freeing something that was never allocated here is a real bug, and this
         * export returns void, so the report is the only evidence it happened. */
        kernel_hle_log()("kernel: MmFreeContiguousMemory(%#x) is not a contiguous "
                         "allocation base\n",
                         args[0]);
        return STATUS_SUCCESS;
    }
    guest_region_free(args[0]);
    return STATUS_SUCCESS;
}

static uint32_t hle_mm_query_allocation_size(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmQueryAllocationSize");
    uint32_t args[1];
    if (!frame || !frame_args(frame, args, 1u, "MmQueryAllocationSize")) {
        return 0u;
    }
    const guest_region *region = guest_region_at(args[0]);
    if (!region) {
        return 0u;
    }
    /* The size the allocator actually reserved, not the unrounded request: that is
     * what the guest may safely write to. */
    return region->size;
}

static uint32_t hle_mm_query_address_protect(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmQueryAddressProtect");
    uint32_t args[1];
    if (!frame || !frame_args(frame, args, 1u, "MmQueryAddressProtect")) {
        return 0u;
    }
    /* One stdcall argument (measured, call site 0x00381E57). The protection is kept
     * per region, so any address inside one reports that region's current value.
     * An untracked address answers 0 (INFERRED: the real value for unmapped pages is
     * not measured) and logs, so it cannot pass for a real protection. */
    const guest_region *region = guest_region_containing(args[0]);
    if (!region) {
        kernel_hle_log()("kernel: MmQueryAddressProtect(%#x) addresses no tracked "
                         "region, answering 0\n",
                         args[0]);
        return 0u;
    }
    return region->protect;
}

static uint32_t hle_mm_get_physical_address(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmGetPhysicalAddress");
    uint32_t args[1];
    if (!frame || !frame_args(frame, args, 1u, "MmGetPhysicalAddress")) {
        return 0u;
    }
    /* Synthetic, and 0 for an untracked address. See guest_mem.h. */
    return guest_physical_address(args[0]);
}

static uint32_t hle_mm_persist_contiguous_memory(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmPersistContiguousMemory");
    uint32_t args[3];
    if (!frame || !frame_args(frame, args, 3u, "MmPersistContiguousMemory")) {
        return STATUS_SUCCESS;
    }
    /* NumberOfBytes is accepted and ignored: persistence is recorded per region,
     * and nothing reboots, so a sub-range flag would have no reader. */
    (void)args[1];
    if (!guest_region_set_persist(args[0], args[2] != 0u)) {
        kernel_hle_log()("kernel: MmPersistContiguousMemory(%#x) is not an allocation "
                         "base\n",
                         args[0]);
    }
    return STATUS_SUCCESS;
}

static uint32_t hle_mm_set_address_protect(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmSetAddressProtect");
    uint32_t args[3];
    if (!frame || !frame_args(frame, args, 3u, "MmSetAddressProtect")) {
        return STATUS_SUCCESS;
    }
    if (!nt_page_protect_valid(args[2])) {
        kernel_hle_log()("kernel: MmSetAddressProtect(%#x) protection %#x is not a "
                         "valid PAGE_* value\n",
                         args[0], args[2]);
        return STATUS_SUCCESS;
    }
    if (!guest_region_set_protect(args[0], args[1], args[2])) {
        kernel_hle_log()("kernel: MmSetAddressProtect(%#x) addresses no tracked "
                         "region\n",
                         args[0]);
    }
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Buffer page locks (175). See kernel_memory.h for the measured signature.
 *
 * One sorted array of (page number, depth), searched by bisection. Pages with depth 0 are
 * removed, so the array's length is the number of locked pages. A mutex guards it because
 * guest threads are host threads.
 * ----------------------------------------------------------------------- */

typedef struct {
    uint32_t page;
    uint32_t depth;
} page_lock;

static pthread_mutex_t lock_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static page_lock *lock_table;
static size_t lock_table_count;
static size_t lock_table_capacity;

/* Index of the first entry with page >= `page`, under the mutex. */
static size_t lock_table_lower_bound(uint32_t page)
{
    size_t low = 0u;
    size_t high = lock_table_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (lock_table[middle].page < page) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    return low;
}

static void lock_table_clear(void)
{
    pthread_mutex_lock(&lock_table_mutex);
    free(lock_table);
    lock_table = NULL;
    lock_table_count = 0u;
    lock_table_capacity = 0u;
    pthread_mutex_unlock(&lock_table_mutex);
}

/* Add one lock to `page`. False only when the table cannot grow. Under the mutex. */
static bool lock_table_lock_page(uint32_t page)
{
    size_t index = lock_table_lower_bound(page);
    if (index < lock_table_count && lock_table[index].page == page) {
        lock_table[index].depth++;
        return true;
    }
    if (lock_table_count == lock_table_capacity) {
        size_t capacity = lock_table_capacity == 0u ? 64u : lock_table_capacity * 2u;
        page_lock *grown = realloc(lock_table, capacity * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        lock_table = grown;
        lock_table_capacity = capacity;
    }
    memmove(&lock_table[index + 1u], &lock_table[index],
            (lock_table_count - index) * sizeof(*lock_table));
    lock_table[index].page = page;
    lock_table[index].depth = 1u;
    lock_table_count++;
    return true;
}

/* Remove one lock from `page`. False when it had none. Under the mutex. */
static bool lock_table_unlock_page(uint32_t page)
{
    size_t index = lock_table_lower_bound(page);
    if (index >= lock_table_count || lock_table[index].page != page) {
        return false;
    }
    if (--lock_table[index].depth == 0u) {
        memmove(&lock_table[index], &lock_table[index + 1u],
                (lock_table_count - index - 1u) * sizeof(*lock_table));
        lock_table_count--;
    }
    return true;
}

uint32_t kernel_memory_locked_page_total(void)
{
    pthread_mutex_lock(&lock_table_mutex);
    const uint32_t total = (uint32_t)lock_table_count;
    pthread_mutex_unlock(&lock_table_mutex);
    return total;
}

uint32_t kernel_memory_page_lock_count(uint32_t address)
{
    pthread_mutex_lock(&lock_table_mutex);
    const uint32_t page = address / GUEST_PAGE_SIZE;
    const size_t index = lock_table_lower_bound(page);
    const uint32_t depth =
        (index < lock_table_count && lock_table[index].page == page) ? lock_table[index].depth
                                                                     : 0u;
    pthread_mutex_unlock(&lock_table_mutex);
    return depth;
}

static uint32_t hle_mm_lock_unlock_buffer_pages(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmLockUnlockBufferPages");
    uint32_t args[3];
    if (!frame || !frame_args(frame, args, 3u, "MmLockUnlockBufferPages")) {
        return STATUS_SUCCESS;
    }
    const uint32_t base = args[0];
    const uint32_t bytes = args[1];
    const uint32_t unlock = args[2];

    /* Three stdcall arguments, VOID (measured, see kernel_memory.h). Only the literals 0
     * and 1 are measured for UnlockPages, so any other value is a mis-read argument. */
    if (unlock > 1u) {
        kernel_hle_log()("kernel: MmLockUnlockBufferPages(%#x, %#x, %#x) refused: UnlockPages "
                         "is neither 0 (lock) nor 1 (unlock), the only measured values\n",
                         base, bytes, unlock);
        return STATUS_SUCCESS;
    }
    if (bytes == 0u) {
        return STATUS_SUCCESS;
    }
    const uint64_t end = (uint64_t)base + bytes;
    if (end > 0x100000000ULL) {
        kernel_hle_log()("kernel: MmLockUnlockBufferPages(%#x, %#x, %u) refused: the range "
                         "runs past the 4 GB address space\n",
                         base, bytes, unlock);
        return STATUS_SUCCESS;
    }
    const uint32_t first_page = base / GUEST_PAGE_SIZE;
    const uint32_t last_page = (uint32_t)((end - 1u) / GUEST_PAGE_SIZE);

    if (unlock == 0u) {
        /* Against the allocator's tracking: reported, but still recorded, so the title's
         * later unlock balances rather than also reporting an underflow. */
        const guest_region *region = guest_region_containing(base);
        if (region == NULL || end > (uint64_t)region->address + region->size) {
            kernel_hle_log()("kernel: MmLockUnlockBufferPages(%#x, %#x) locks a range outside "
                             "any one tracked region, recording it anyway\n",
                             base, bytes);
        } else if (region->state != MEM_COMMIT) {
            kernel_hle_log()("kernel: MmLockUnlockBufferPages(%#x, %#x) locks memory that is "
                             "not committed (region %#x), recording it anyway\n",
                             base, bytes, region->address);
        }
    }

    uint32_t not_locked = 0u;
    bool exhausted = false;
    pthread_mutex_lock(&lock_table_mutex);
    for (uint64_t page = first_page; page <= last_page; page++) {
        if (unlock == 0u) {
            if (!lock_table_lock_page((uint32_t)page)) {
                exhausted = true;
                break;
            }
        } else if (!lock_table_unlock_page((uint32_t)page)) {
            not_locked++;
        }
    }
    pthread_mutex_unlock(&lock_table_mutex);

    if (exhausted) {
        kernel_hle_log()("kernel: MmLockUnlockBufferPages(%#x, %#x) ran out of host memory "
                         "recording the lock\n",
                         base, bytes);
    }
    if (not_locked != 0u) {
        kernel_hle_log()("kernel: MmLockUnlockBufferPages(%#x, %#x) unlocks %u page(s) that "
                         "were not locked\n",
                         base, bytes, not_locked);
    }
    return STATUS_SUCCESS;
}

static uint32_t physical_refused_count;

uint32_t kernel_memory_physical_refused_count(void)
{
    pthread_mutex_lock(&lock_table_mutex);
    const uint32_t count = physical_refused_count;
    pthread_mutex_unlock(&lock_table_mutex);
    return count;
}

/* One refusal of 176, counted under the table mutex. */
static void physical_refuse(void)
{
    pthread_mutex_lock(&lock_table_mutex);
    physical_refused_count++;
    pthread_mutex_unlock(&lock_table_mutex);
}

/* Two stdcall arguments, VOID (measured, see kernel_memory.h). */
static uint32_t hle_mm_lock_unlock_physical_page(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmLockUnlockPhysicalPage");
    uint32_t args[2];
    if (!frame || !frame_args(frame, args, 2u, "MmLockUnlockPhysicalPage")) {
        return STATUS_SUCCESS;
    }
    const uint32_t physical = args[0];
    const uint32_t unlock = args[1];

    /* All four measured sites push the literal 1, so lock mode (0) is not measured and is
     * refused like any other value rather than being given a meaning here. */
    if (unlock != 1u) {
        physical_refuse();
        kernel_hle_log()("kernel: MmLockUnlockPhysicalPage(%#x, %#x) refused: UnlockPage is "
                         "not 1, the only measured value (lock mode is unmeasured)\n",
                         physical, unlock);
        return STATUS_SUCCESS;
    }
    kernel_guest_ptr address = 0u;
    if (!guest_virtual_from_physical(physical, &address)) {
        physical_refuse();
        kernel_hle_log()("kernel: MmLockUnlockPhysicalPage(%#x, 1) refused: no tracked region "
                         "owns that physical address\n",
                         physical);
        return STATUS_SUCCESS;
    }
    pthread_mutex_lock(&lock_table_mutex);
    const bool was_locked = lock_table_unlock_page(address / GUEST_PAGE_SIZE);
    pthread_mutex_unlock(&lock_table_mutex);
    if (!was_locked) {
        kernel_hle_log()("kernel: MmLockUnlockPhysicalPage(%#x, 1) unlocks the page at %#x, "
                         "which was not locked\n",
                         physical, address & ~(GUEST_PAGE_SIZE - 1u));
    }
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Kernel stacks. The guest keeps one lazily created stack in the dword at
 * 0x7717DC (generated/lifted/gen/recomp_0044.c, sub_00387A55 and sub_00387A28).
 * ----------------------------------------------------------------------- */

static uint32_t hle_mm_create_kernel_stack(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmCreateKernelStack");
    uint32_t args[2];
    if (!frame || !frame_args(frame, args, 2u, "MmCreateKernelStack")) {
        return 0u;
    }
    uint32_t bytes = args[0];
    uint32_t debugger_thread = args[1];

    /* Two stdcall arguments (measured, call site 0x00387A67: NumberOfBytes is the
     * literal 0x6000, DebuggerThread the literal 0). The return value is the stack
     * BASE, which is the TOP of the usable range (measured): sub_00388FAB switches
     * esp to the value published at 0x7717DC and pushes downward from it. Zero is
     * the failure value (measured: the caller maps eax == 0 to 0x8007000E). */
    if (bytes == 0u) {
        kernel_hle_log()("kernel: MmCreateKernelStack with NumberOfBytes 0 refused\n");
        return 0u;
    }
    /* INFERRED: the real function sizes the stack in whole pages. The one measured
     * call passes a page multiple, so any other size is accepted but reported. */
    if ((bytes & (GUEST_PAGE_SIZE - 1u)) != 0u) {
        kernel_hle_log()("kernel: MmCreateKernelStack(%#x) is not a multiple of the "
                         "page size\n",
                         bytes);
    }
    /* Not modelled (measured call passes 0), so say so once rather than per call. */
    static bool debugger_thread_reported;
    if (debugger_thread != 0u && !debugger_thread_reported) {
        debugger_thread_reported = true;
        kernel_hle_log()("kernel: MmCreateKernelStack DebuggerThread %#x is not "
                         "modelled\n",
                         debugger_thread);
    }

    guest_region_request request = {
        .bytes = bytes,
        .alignment = 0u,
        .lowest_physical = 0u,
        .highest_physical = 0u,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .contiguous = false,
        .fixed_base = 0u,
    };
    nt_status status = STATUS_SUCCESS;
    kernel_guest_ptr address = guest_region_alloc_typed(&request, &status, GUEST_MEMORY_STACK);
    if (address == 0u) {
        kernel_hle_log()("kernel: MmCreateKernelStack(%#x) failed, status %#x\n", bytes,
                         status);
        return 0u;
    }
    return (uint32_t)(address + bytes);
}

static uint32_t hle_mm_delete_kernel_stack(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmDeleteKernelStack");
    uint32_t args[2];
    if (!frame || !frame_args(frame, args, 2u, "MmDeleteKernelStack")) {
        return STATUS_SUCCESS;
    }
    uint32_t stack_base = args[0];
    uint32_t stack_limit = args[1];

    /* Two stdcall arguments (measured, call sites 0x00387A4E and 0x00387A9B): the
     * value 169 returned, then that value minus 0x6000. The result is ignored there
     * and the export is VOID (nxdk), so this always returns STATUS_SUCCESS. The
     * region to free starts at StackLimit and spans StackBase - StackLimit (INFERRED
     * from the measured pairing). Nothing is freed unless the model can prove it
     * allocated exactly that span. */
    const guest_region *region = guest_region_at(stack_limit);
    if (stack_base == 0u || stack_base < stack_limit || !region ||
        region->size != stack_base - stack_limit) {
        kernel_hle_log()("kernel: MmDeleteKernelStack(%#x, %#x) does not match a "
                         "tracked stack, nothing freed\n",
                         stack_base, stack_limit);
        return STATUS_SUCCESS;
    }
    guest_region_free(stack_limit);
    return STATUS_SUCCESS;
}

/* Optional loader linkage: kernel-only programs have no XBE image mappings.
 * In the host xbe_map's strong reference pulls in this same loader object. */
extern uint64_t xbe_mapped_bytes(void) __attribute__((weak));

static uint32_t hle_mm_query_statistics(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "MmQueryStatistics");
    uint32_t args[1], length;
    if (!frame || !frame_args(frame, args, 1u, "MmQueryStatistics"))
        return STATUS_INVALID_PARAMETER;
    if (!kernel_guest_read_u32(args[0], &length)) return STATUS_ACCESS_VIOLATION;
    /* Complex4627 8001E547: invalid Length reads ONLY this word, never writes. */
    if (length != GUEST_MM_STATISTICS_SIZE) return STATUS_INVALID_PARAMETER;

    struct sysinfo physical;
    if (sysinfo(&physical) != 0) return STATUS_UNSUCCESSFUL;
    guest_memory_statistics memory;
    guest_mem_statistics(&memory);
    uint64_t image_bytes = xbe_mapped_bytes ? xbe_mapped_bytes() : 0u;
    uint64_t fields[8] = {
        ((uint64_t)physical.totalram * physical.mem_unit) / GUEST_PAGE_SIZE,
        ((uint64_t)physical.freeram * physical.mem_unit) / GUEST_PAGE_SIZE,
        memory.virtual_committed_bytes + image_bytes,
        memory.virtual_reserved_bytes + image_bytes,
        memory.cache_pages, memory.pool_pages, memory.stack_pages,
        image_bytes / GUEST_PAGE_SIZE,
    };
    /* This host has no fixed guest physical-page budget: report real host RAM,
     * actual guest commitments, never the reference machine's 64MB constants.
     * Refuse an unrepresentable host counter rather than truncating/clamping it. */
    for (unsigned i = 0; i < 8u; ++i)
        if (fields[i] > UINT32_MAX) return STATUS_INSUFFICIENT_RESOURCES;
    /* Preserve original store order and successful prefix on a later fault.
     * Guarded host accesses return ACCESS_VIOLATION; original faults in the CPU.
     * Length is input-only. Do not preflight or bulk-copy the whole buffer. */
    for (unsigned i = 0; i < 8u; ++i) {
        uint64_t address = (uint64_t)args[0] + 4u + i * 4u;
        if (address > UINT32_MAX ||
            !kernel_guest_write_u32((kernel_guest_ptr)address, (uint32_t)fields[i]))
            return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Virtual memory.
 * ----------------------------------------------------------------------- */

static uint32_t hle_nt_allocate_virtual_memory(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "NtAllocateVirtualMemory");
    uint32_t args[5];
    if (!frame || !frame_args(frame, args, 5u, "NtAllocateVirtualMemory")) {
        return STATUS_INVALID_PARAMETER;
    }
    kernel_guest_ptr base_ptr = args[0];
    kernel_guest_ptr size_ptr = args[2];
    uint32_t allocation_type = args[3];
    uint32_t protect = args[4];
    /* ZeroBits constrains how many leading bits of the address must be zero. Not
     * modelled: every address we hand out already lives below 4 GB, which satisfies
     * every value a 32-bit guest can usefully pass. */
    (void)args[1];

    uint32_t requested_base = 0;
    uint32_t requested_size = 0;
    if (!kernel_guest_read_u32(base_ptr, &requested_base) ||
        !kernel_guest_read_u32(size_ptr, &requested_size)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (requested_size == 0u) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((allocation_type & (MEM_COMMIT | MEM_RESERVE)) == 0u ||
        (allocation_type & ~MEM_ALLOCATION_TYPE_VALID) != 0u) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!nt_page_protect_valid(protect)) {
        return STATUS_INVALID_PAGE_PROTECTION;
    }

    /* Committing memory that is already reserved at this exact base is the second
     * half of the two-step reserve-then-commit pattern, not a new allocation. */
    if (requested_base != 0u && (allocation_type & MEM_COMMIT) != 0u) {
        const guest_region *existing = guest_region_at(requested_base);
        if (existing && existing->state == MEM_RESERVE) {
            guest_region_set_state(requested_base, MEM_COMMIT);
            guest_region_set_protect(requested_base, existing->size, protect);
            const guest_region *updated = guest_region_at(requested_base);
            if (!kernel_guest_write_u32(base_ptr, requested_base) ||
                !kernel_guest_write_u32(size_ptr, updated ? updated->size : 0u)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return STATUS_SUCCESS;
        }
    }

    guest_region_request request = {
        .bytes = requested_size,
        .alignment = 0u,
        .lowest_physical = 0u,
        .highest_physical = 0u,
        .protect = protect,
        .state = allocation_type,
        .contiguous = false,
        .fixed_base = requested_base,
    };
    nt_status status = STATUS_SUCCESS;
    kernel_guest_ptr address = guest_region_alloc(&request, &status);
    if (address == 0u) {
        return status;
    }

    const guest_region *region = guest_region_at(address);
    /* Both out-parameters are written, as the real call does: the guest reads back
     * the rounded base and size and would otherwise keep its unrounded request. */
    if (!kernel_guest_write_u32(base_ptr, address) ||
        !kernel_guest_write_u32(size_ptr, region ? region->size : 0u)) {
        guest_region_free(address);
        return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

static uint32_t hle_nt_free_virtual_memory(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "NtFreeVirtualMemory");
    uint32_t args[3];
    if (!frame || !frame_args(frame, args, 3u, "NtFreeVirtualMemory")) {
        return STATUS_INVALID_PARAMETER;
    }
    kernel_guest_ptr base_ptr = args[0];
    kernel_guest_ptr size_ptr = args[1];
    uint32_t free_type = args[2];

    uint32_t base = 0;
    uint32_t size = 0;
    if (!kernel_guest_read_u32(base_ptr, &base) ||
        !kernel_guest_read_u32(size_ptr, &size)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if ((free_type & MEM_FREE_TYPE_VALID) == 0u ||
        (free_type & ~MEM_FREE_TYPE_VALID) != 0u) {
        return STATUS_INVALID_PARAMETER;
    }
    /* DECOMMIT and RELEASE together is contradictory, and NT rejects it. */
    if ((free_type & MEM_DECOMMIT) != 0u && (free_type & MEM_RELEASE) != 0u) {
        return STATUS_INVALID_PARAMETER;
    }

    const guest_region *exact = guest_region_at(base);
    if (!exact) {
        /* Distinguish "inside a region but not its base" from "not ours at all":
         * the first is a caller using the wrong pointer, the second is a double
         * free or a stray value, and the status codes differ accordingly. */
        return guest_region_containing(base) ? STATUS_FREE_VM_NOT_AT_BASE
                                             : STATUS_MEMORY_NOT_ALLOCATED;
    }

    if ((free_type & MEM_RELEASE) != 0u) {
        /* A release must name the whole region, which NT expresses as a zero size. */
        if (size != 0u) {
            return STATUS_INVALID_PARAMETER;
        }
        uint32_t released = exact->size;
        if (!guest_region_free(base)) {
            return STATUS_MEMORY_NOT_ALLOCATED;
        }
        if (!kernel_guest_write_u32(base_ptr, base) ||
            !kernel_guest_write_u32(size_ptr, released)) {
            return STATUS_ACCESS_VIOLATION;
        }
        return STATUS_SUCCESS;
    }

    /* DECOMMIT. The pages stay mapped on the host and the region keeps its
     * address, so a later commit of the same base succeeds. Partial decommit of a
     * sub-range is not modelled: the whole region changes state. */
    uint32_t decommitted = exact->size;
    if (!guest_region_set_state(base, MEM_RESERVE)) {
        return STATUS_MEMORY_NOT_ALLOCATED;
    }
    if (!kernel_guest_write_u32(base_ptr, base) ||
        !kernel_guest_write_u32(size_ptr, decommitted)) {
        return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

static uint32_t hle_nt_query_virtual_memory(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "NtQueryVirtualMemory");
    uint32_t args[2];
    if (!frame || !frame_args(frame, args, 2u, "NtQueryVirtualMemory")) {
        return STATUS_INVALID_PARAMETER;
    }
    kernel_guest_ptr address = args[0];
    kernel_guest_ptr info_ptr = args[1];

    if (kernel_guest_at(info_ptr, GUEST_MEMORY_BASIC_INFORMATION_SIZE) == NULL) {
        return STATUS_ACCESS_VIOLATION;
    }

    /* MEMORY_BASIC_INFORMATION as a 32-bit guest sees it: seven four-byte fields,
     * in this order. Written field by field rather than as a host struct, because a
     * host struct containing pointers would be 8-byte-wide per pointer field and
     * would silently shift everything after BaseAddress. */
    const guest_region *region = guest_region_containing(address);
    uint32_t fields[7];
    if (region) {
        fields[0] = address & ~(GUEST_PAGE_SIZE - 1u); /* BaseAddress, page floor */
        fields[1] = region->address;                   /* AllocationBase */
        fields[2] = region->alloc_protect;              /* AllocationProtect */
        /* RegionSize runs from the reported base to the end of the region. */
        fields[3] = region->address + region->size - fields[0];
        fields[4] = region->state;    /* State */
        fields[5] = region->protect;  /* Protect */
        fields[6] = MEM_PRIVATE;      /* Type */
    } else {
        /* Unallocated. The extent of a free gap is NOT computed: we do not track
         * the address space between regions, so RegionSize is reported as 0 rather
         * than as a plausible-but-invented span. */
        fields[0] = address & ~(GUEST_PAGE_SIZE - 1u);
        fields[1] = 0u;
        fields[2] = 0u;
        fields[3] = 0u;
        fields[4] = MEM_FREE;
        fields[5] = PAGE_NOACCESS;
        fields[6] = 0u;
    }

    for (unsigned i = 0; i < 7u; i++) {
        kernel_guest_ptr at = (kernel_guest_ptr)(info_ptr + i * sizeof(uint32_t));
        if (!kernel_guest_write_u32(at, fields[i])) {
            return STATUS_ACCESS_VIOLATION;
        }
    }
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Registration.
 * ----------------------------------------------------------------------- */

typedef struct {
    unsigned ordinal;
    kernel_fn handler;
} memory_binding;

/* ARITY-OK(165): one argument, NumberOfBytes. Low confidence here comes from only
 * 2 call sites, not from the sites disagreeing. Corroborated by the Ex variant at
 * the next ordinal taking that same argument first.
 * ARITY-OK(166): five arguments. The guest pushes exactly five at all 4 sites; the
 * low confidence is sample size again.
 * ARITY-OK(173): one argument, the base address whose physical address is wanted.
 * All 13 sites pass a single pointer they have just computed.
 * ARITY-OK(175): THREE (BaseAddress, NumberOfBytes, UnlockPages). The measured row says 1 and
 * is non-unanimous: at XNET 0x0043A765 and 0x0043AAE3 two of the three pushes sit above an
 * intervening `call 0x439cff` that pops nothing, and the register-dispatched site 0x0040F638
 * is invisible to the bracket. Hand-counted 3 at DSOUND, XNET and XPP sites, and the
 * allocator pairing fixes the argument roles. nxdk MmLockUnlockBufferPages@12.
 * ARITY-OK(176): two arguments, (PhysicalAddress, UnlockPage). All four sites (XPP
 * 0x00473E5F/78 and the register-dispatched 0x00474240/53) push the literal 1 then the
 * physical address; the measured row {176,2,2,1} and nxdk MmLockUnlockPhysicalPage@8 agree.
 * ARITY-OK(169): two arguments. The one static site, 0x00387A67 in sub_00387A55,
 * pushes the literal 0 then the literal 0x6000 before the indirect call, and the
 * nxdk .def row is MmCreateKernelStack@8.
 * ARITY-OK(170): two arguments. Both static sites, 0x00387A4E in sub_00387A28 and
 * 0x00387A9B in sub_00387A55, push exactly StackLimit then StackBase, and the nxdk
 * .def row is MmDeleteKernelStack@8. */
static const memory_binding bindings[] = {
    {ORD_MmAllocateContiguousMemory, hle_mm_allocate_contiguous_memory},
    {ORD_MmAllocateContiguousMemoryEx, hle_mm_allocate_contiguous_memory_ex},
    {ORD_MmCreateKernelStack, hle_mm_create_kernel_stack},
    {ORD_MmDeleteKernelStack, hle_mm_delete_kernel_stack},
    {ORD_MmFreeContiguousMemory, hle_mm_free_contiguous_memory},
    {ORD_MmGetPhysicalAddress, hle_mm_get_physical_address},
    {ORD_MmLockUnlockBufferPages, hle_mm_lock_unlock_buffer_pages},
    {ORD_MmLockUnlockPhysicalPage, hle_mm_lock_unlock_physical_page},
    {ORD_MmPersistContiguousMemory, hle_mm_persist_contiguous_memory},
    {ORD_MmQueryAddressProtect, hle_mm_query_address_protect},
    {ORD_MmQueryAllocationSize, hle_mm_query_allocation_size},
    {ORD_MmQueryStatistics, hle_mm_query_statistics},
    {ORD_MmSetAddressProtect, hle_mm_set_address_protect},
    {ORD_NtAllocateVirtualMemory, hle_nt_allocate_virtual_memory},
    {ORD_NtFreeVirtualMemory, hle_nt_free_virtual_memory},
    {ORD_NtQueryVirtualMemory, hle_nt_query_virtual_memory},
};

#define BINDING_COUNT (sizeof(bindings) / sizeof(bindings[0]))

static unsigned ordinal_list[BINDING_COUNT];

size_t kernel_memory_register(void)
{
    /* A fresh registration is a fresh kernel: the page locks belong to the guest that is
     * gone. */
    lock_table_clear();
    pthread_mutex_lock(&lock_table_mutex);
    physical_refused_count = 0u;
    pthread_mutex_unlock(&lock_table_mutex);
    size_t registered = 0;
    for (size_t i = 0; i < BINDING_COUNT; i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            registered++;
            continue;
        }
        /* The only way this fails is an ordinal this file believes in that the
         * generated table does not have. That is a drift between the two, and
         * silence would turn it into a mysterious stub report later. */
        kernel_hle_log()("kernel: cannot register ordinal %u -- not in the ordinal "
                         "table\n",
                         bindings[i].ordinal);
    }
    return registered;
}

const unsigned *kernel_memory_ordinals(size_t *count)
{
    for (size_t i = 0; i < BINDING_COUNT; i++) {
        ordinal_list[i] = bindings[i].ordinal;
    }
    if (count) {
        *count = BINDING_COUNT;
    }
    return ordinal_list;
}

size_t kernel_memory_ordinal_count(void)
{
    return BINDING_COUNT;
}
