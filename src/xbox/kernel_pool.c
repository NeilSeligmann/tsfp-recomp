/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_pool.h for the verified ordinal numbers and why the pool is a heap.
 *
 * SIGNATURES, WITH THE STACK-ARGUMENT COUNT MEASURED FROM THIS IMAGE. An arity
 * error is silent -- our handler IS the __stdcall callee, so a wrong count desyncs
 * the guest's esp permanently and the damage appears far from here. The counts
 * below come from the per-site push vote at every call site in the user's own
 * binary (tools/lift/callsites.py), NOT from the `estimate` that file publishes,
 * because the minimum-across-sites rule under-reports both of these:
 *
 *   PVOID __stdcall ExAllocatePoolWithTag(SIZE_T NumberOfBytes, ULONG Tag);
 *       14 sites vote {0: 3, 1: 1, 2: 8, 3: 2}. TRUE COUNT 2.
 *   VOID  __stdcall ExFreePool(PVOID P);
 *       10 bracketed sites vote {0: 2, 1: 8}. TRUE COUNT 1.
 *
 * HOW THE MINORITY VOTES ARE ACCOUNTED FOR, both directions:
 *   - HIGHER than the truth (2 sites at 3 for ordinal 15): the lifter opens its
 *     call-site bracket at the start of the x86 basic block, so the enclosing
 *     function's own callee-saved `push esi` / `push ebx` falls inside it and looks
 *     like an argument. This error is well understood and documented in
 *     tools/lift/callsites.py.
 *   - LOWER than the truth (3 sites at 0 and 1 for ordinal 15, 2 sites at 0 for
 *     ordinal 17): the bracket opens LATE, after the arguments were pushed in an
 *     earlier basic block, so real arguments fall outside it. This is the direction
 *     the published minimum cannot defend against, and it is exactly why
 *     RtlInitAnsiString was independently found to be under-reported as 1.
 * A modal count with a clear majority, flanked by minorities explained in BOTH
 * directions, is better evidence than a minimum that can only be wrong one way.
 *
 * ALSO NOTE, and this is the part that is not in this file's gift: the host's
 * dispatcher takes its esp cleanup from `ABI_TABLE` in src/host/kernel_thunk.c
 * first and the generated measured table second. The measured table carries 0 for
 * BOTH of these ordinals. Until a hand entry exists in ABI_TABLE, a real run will
 * pop 0 argument bytes for calls these handlers read 2 and 1 arguments from, and
 * esp will desync. See kernel_pool.h and the task report: the ABI_TABLE entry is
 * required, not optional.
 */

#include "kernel_pool.h"

#include <pthread.h>

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

#define ORD_ExAllocatePoolWithTag 15u
#define ORD_ExFreePool 17u
#define ORD_ExQueryPoolBlockSize 23u

/* How many live pool allocations can be tracked at once.
 *
 * A fixed array rather than a growing one because the table must be usable from a
 * handler that may be holding other locks, and because a bound that is reached
 * loudly is better than one that is reached silently. 14 call sites cannot produce
 * unbounded concurrent allocations in any plausible run, so 4096 is slack of two
 * orders of magnitude; if it is ever exhausted the allocation is REFUSED rather
 * than issued untracked (see pool_alloc). */
#define KERNEL_POOL_MAX 4096u

typedef struct {
    kernel_guest_ptr address;
    /* What the guest asked for, not what the heap rounded it to. The distinction is
     * the only way to tell a 5-byte request from a 16-byte one after the fact. */
    uint32_t bytes;
    uint32_t tag;
    bool in_use;
} pool_entry;

/* ---------------------------------------------------------------------------
 * THE LOCK.
 *
 * Two guest threads run, and both can allocate. Every race in this table produces
 * a WRONG ANSWER rather than a crash, which is the kind that surfaces far from its
 * cause:
 *
 *   - `pool_heap()` is a test-and-create. Two threads both seeing handle 0 both
 *     call guest_heap_create, and the loser's heap is leaked with every page it
 *     owns -- an invisible leak, because nothing holds its handle any more.
 *   - Claiming a free slot is a read-modify-write. Two threads can claim the SAME
 *     slot for two different addresses; the second overwrites the first, so the
 *     first address has no record and the guest's eventual, legitimate ExFreePool
 *     of it is reported as a bad free. That is our diagnostic blaming the guest for
 *     our own bug, exactly the failure kernel_object.c documents.
 *   - `live_bytes` is a non-atomic accumulate, so a lost add makes the leak report
 *     understate what is outstanding.
 *
 * RECURSIVE, following kernel_object.c and guest_mem.c: these critical sections
 * call kernel_hle_log(), whose sink is caller-supplied and may ask this module a
 * question. Tests install custom sinks, so this is a real path and not a
 * hypothetical one.
 *
 * LOCK ORDER. This module calls into guest_mem.c (which takes its own lock) while
 * holding this one, and guest_mem.c never calls back here. So the order is always
 * pool-then-guest-mem and there is no cycle.
 * ------------------------------------------------------------------------- */

static pthread_mutex_t pool_lock;
static pthread_once_t pool_lock_once = PTHREAD_ONCE_INIT;

static void pool_lock_init(void)
{
    pthread_mutexattr_t attr;
    (void)pthread_mutexattr_init(&attr);
    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&pool_lock, &attr);
    (void)pthread_mutexattr_destroy(&attr);
}

static void pool_enter(void)
{
    (void)pthread_once(&pool_lock_once, pool_lock_init);
    (void)pthread_mutex_lock(&pool_lock);
}

static void pool_leave(void)
{
    (void)pthread_mutex_unlock(&pool_lock);
}

static pool_entry entries[KERNEL_POOL_MAX];
static unsigned live_count;
static uint64_t live_bytes;
static uint32_t bad_free_count;
static uint32_t pool_heap_handle;

/* Render a four-character pool tag for a diagnostic.
 *
 * A tag is four ASCII characters packed little-endian, so the bytes come out in
 * source order. Unprintable bytes become '?' rather than being emitted raw: a tag
 * that is actually a mis-passed pointer would otherwise spray control characters
 * through the log and could even look like a different message. */
static void tag_chars(uint32_t tag, char out[5])
{
    for (unsigned i = 0u; i < 4u; i++) {
        uint32_t byte = (tag >> (8u * i)) & 0xFFu;
        out[i] = (byte >= 0x20u && byte < 0x7Fu) ? (char)byte : '?';
    }
    out[4] = '\0';
}

/* The backing heap, created on first use. 0 if it cannot be created.
 *
 * Also the place a heap destroyed behind our back is noticed. guest_mem_reset()
 * destroys every heap, and tests call it; if the table still claimed live
 * allocations afterwards, the next ExFreePool of a now-dead address would be
 * reported as a guest bug. So a stale handle clears the table and says so. */
static uint32_t pool_heap(void)
{
    if (pool_heap_handle != 0u && !guest_heap_valid(pool_heap_handle)) {
        kernel_hle_log()("kernel: pool heap %#x was destroyed underneath the pool "
                         "(%u allocations dropped) -- guest_mem_reset without "
                         "kernel_pool_reset\n",
                         pool_heap_handle, live_count);
        for (unsigned i = 0u; i < KERNEL_POOL_MAX; i++) {
            entries[i] = (pool_entry){0u, 0u, 0u, false};
        }
        live_count = 0u;
        live_bytes = 0u;
        pool_heap_handle = 0u;
    }
    if (pool_heap_handle == 0u) {
        /* Growable: maximum_size 0. The guest's pool is bounded by physical memory,
         * not by a figure we would have to invent. */
        pool_heap_handle = guest_heap_create_typed(0u, GUEST_HEAP_CHUNK_MIN, 0u, GUEST_MEMORY_POOL);
        if (pool_heap_handle == 0u) {
            kernel_hle_log()("kernel: could not create the executive pool heap\n");
        }
    }
    return pool_heap_handle;
}

static pool_entry *find_nolock(kernel_guest_ptr address)
{
    if (address == 0u) {
        return NULL;
    }
    for (unsigned i = 0u; i < KERNEL_POOL_MAX; i++) {
        if (entries[i].in_use && entries[i].address == address) {
            return &entries[i];
        }
    }
    return NULL;
}

static kernel_guest_ptr pool_alloc(uint32_t bytes, uint32_t tag)
{
    char tag_text[5];

    /* A zero-byte request. The real allocator returns a unique non-NULL pointer for
     * it; the heap underneath us would too, but there is no evidence in this image
     * that the guest ever asks, so refuse and SAY SO rather than quietly inventing
     * a policy that something later depends on. */
    if (bytes == 0u) {
        tag_chars(tag, tag_text);
        kernel_hle_log()("kernel: ExAllocatePoolWithTag(0, '%s') -- zero-byte pool "
                         "request refused; no call site in this image does this, so "
                         "the hardware behaviour is unverified\n",
                         tag_text);
        return 0u;
    }

    uint32_t heap = pool_heap();
    if (heap == 0u) {
        return 0u;
    }

    unsigned slot = KERNEL_POOL_MAX;
    for (unsigned i = 0u; i < KERNEL_POOL_MAX; i++) {
        if (!entries[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot == KERNEL_POOL_MAX) {
        /* REFUSED, not issued untracked. An untracked block would be freed by a
         * guest that is behaving perfectly, and we would report that correct free
         * as a bad one -- turning our own exhausted table into an accusation
         * against the guest. Failing the allocation is at least honest, and
         * ExAllocatePoolWithTag returning NULL is a case the guest must already
         * handle. */
        kernel_hle_log()("kernel: executive pool tracking table full at %u entries "
                         "-- refusing the allocation rather than issuing an "
                         "untracked block\n",
                         KERNEL_POOL_MAX);
        return 0u;
    }

    kernel_guest_ptr address = guest_heap_alloc(heap, bytes);
    if (address == 0u) {
        tag_chars(tag, tag_text);
        kernel_hle_log()("kernel: ExAllocatePoolWithTag(%u, '%s') failed -- the pool "
                         "heap could not satisfy it\n",
                         bytes, tag_text);
        return 0u;
    }

    /* NOT ZEROED, deliberately. The real ExAllocatePoolWithTag hands back whatever
     * was in the block, so a guest that reads an uninitialised field must misbehave
     * here too. Zero-filling would paper over exactly that bug class, and it would
     * do so invisibly. */
    entries[slot].address = address;
    entries[slot].bytes = bytes;
    entries[slot].tag = tag;
    entries[slot].in_use = true;
    live_count++;
    live_bytes += bytes;
    return address;
}

static void pool_free(kernel_guest_ptr address)
{
    /* ExFreePool(NULL) is a bugcheck on hardware, not a no-op like C's free(). It
     * is called out separately from an unknown address because the causes differ: a
     * NULL here usually means an allocation failure went unchecked, whereas an
     * unknown address means a double free or a pointer from somewhere else. */
    if (address == 0u) {
        bad_free_count++;
        kernel_hle_log()("kernel: ExFreePool(NULL) -- the real kernel bugchecks on "
                         "this; an unchecked allocation failure upstream\n");
        return;
    }

    pool_entry *entry = find_nolock(address);
    if (!entry) {
        bad_free_count++;
        kernel_hle_log()("kernel: ExFreePool(%#x) -- the pool never issued this "
                         "address (double free, or a pointer from another "
                         "allocator)\n",
                         address);
        return;
    }

    uint32_t heap = pool_heap_handle;
    if (heap == 0u || !guest_heap_free(heap, address)) {
        /* The table and the heap disagree about who owns this block, which means one
         * of them is corrupt. Drop the record either way: keeping it would turn a
         * one-off into a permanent false "still live" in every later leak report. */
        kernel_hle_log()("kernel: ExFreePool(%#x) -- the pool issued this address "
                         "but the backing heap refuses to free it\n",
                         address);
    }

    live_bytes -= entry->bytes;
    live_count--;
    *entry = (pool_entry){0u, 0u, 0u, false};
}

/* ---------------------------------------------------------------------------
 * Handlers.
 * ------------------------------------------------------------------------- */

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

static bool frame_args(const kernel_call_frame *frame, uint32_t *out, unsigned count,
                       const char *who)
{
    for (unsigned i = 0u; i < count; i++) {
        if (!kernel_frame_arg(frame, i, &out[i])) {
            kernel_hle_log()("kernel: %s could not read argument %u from the guest "
                             "stack\n",
                             who, i);
            return false;
        }
    }
    return true;
}

static uint32_t hle_ex_allocate_pool_with_tag(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "ExAllocatePoolWithTag");
    uint32_t args[2];
    if (!frame || !frame_args(frame, args, 2u, "ExAllocatePoolWithTag")) {
        return 0u;
    }
    pool_enter();
    kernel_guest_ptr address = pool_alloc(args[0], args[1]);
    pool_leave();
    return address;
}

static uint32_t hle_ex_free_pool(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "ExFreePool");
    uint32_t args[1];
    if (!frame || !frame_args(frame, args, 1u, "ExFreePool")) {
        return 0u;
    }
    pool_enter();
    pool_free(args[0]);
    pool_leave();
    /* VOID on hardware. The dispatcher still writes our return into eax, so a
     * caller that wrongly believed this returned something would read 0. */
    return 0u;
}

/* ExQueryPoolBlockSize(PoolBlock): SIZE_T, one stack argument (oracle: STDCALL @4).
 * MEASURED use at both DSOUND sites (0x4096E9, 0x40971E): the result is added to,
 * then subtracted from, a running pool-byte counter around ExAllocatePoolWithTag and
 * ExFreePool, and at the allocating site it is also the byte count of a zero fill
 * (rep stosd/stosb) over the block. So it must never exceed what the block really
 * holds, and must be identical at alloc and free. The REQUESTED size satisfies both
 * (real hardware reports the rounded block size, which nothing observed depends on).
 * An address the pool never issued yields 0, reported, so the zero fill is a no-op. */
static uint32_t hle_ex_query_pool_block_size(void *context)
{
    const kernel_call_frame *frame = frame_of(context, "ExQueryPoolBlockSize");
    uint32_t args[1];
    if (!frame || !frame_args(frame, args, 1u, "ExQueryPoolBlockSize")) {
        return 0u;
    }
    uint32_t bytes = 0u;
    if (!kernel_pool_size_of(args[0], &bytes)) {
        kernel_hle_log()("kernel: ExQueryPoolBlockSize(%#x) -- the pool never issued "
                         "(or already freed) this address, reporting 0\n",
                         args[0]);
        return 0u;
    }
    return bytes;
}

/* ---------------------------------------------------------------------------
 * Observation, for tests and leak reporting.
 * ------------------------------------------------------------------------- */

void kernel_pool_reset(void)
{
    pool_enter();
    if (pool_heap_handle != 0u && guest_heap_valid(pool_heap_handle)) {
        (void)guest_heap_destroy(pool_heap_handle);
    }
    pool_heap_handle = 0u;
    for (unsigned i = 0u; i < KERNEL_POOL_MAX; i++) {
        entries[i] = (pool_entry){0u, 0u, 0u, false};
    }
    live_count = 0u;
    live_bytes = 0u;
    bad_free_count = 0u;
    pool_leave();
}

unsigned kernel_pool_live_count(void)
{
    pool_enter();
    unsigned snapshot = live_count;
    pool_leave();
    return snapshot;
}

uint64_t kernel_pool_live_bytes(void)
{
    pool_enter();
    uint64_t snapshot = live_bytes;
    pool_leave();
    return snapshot;
}

uint32_t kernel_pool_bad_free_count(void)
{
    pool_enter();
    uint32_t snapshot = bad_free_count;
    pool_leave();
    return snapshot;
}

bool kernel_pool_tag_of(kernel_guest_ptr address, uint32_t *out_tag)
{
    pool_enter();
    const pool_entry *entry = find_nolock(address);
    if (entry && out_tag) {
        *out_tag = entry->tag;
    }
    bool found = entry != NULL;
    pool_leave();
    return found;
}

bool kernel_pool_size_of(kernel_guest_ptr address, uint32_t *out_bytes)
{
    pool_enter();
    const pool_entry *entry = find_nolock(address);
    if (entry && out_bytes) {
        *out_bytes = entry->bytes;
    }
    bool found = entry != NULL;
    pool_leave();
    return found;
}

/* ARITY-OK(15): two stack arguments, NumberOfBytes then Tag. The measured table
 * publishes 0, which is its minimum-across-sites rule failing in the LOW direction
 * -- the one direction it cannot defend against. The per-site vote over all 14
 * sites in this image is {0: 3, 1: 1, 2: 8, 3: 2}: a clear mode of 2, with the two
 * high sites explained by the lifter's bracket swallowing the caller's own
 * callee-saved pushes and the four low sites by a bracket that opens after the
 * argument setup. Corroborated by the shape of the arguments at the modal sites:
 * the push order is Tag then NumberOfBytes, so the C order is (NumberOfBytes, Tag),
 * and the tag operand is a four-character ASCII constant at every site that passes
 * a constant one -- 0x4454454E 'NETD', 0x6354454E 'NETc', 0x6B776168 'hawk',
 * 0x5F5F554D 'MU__' -- against sizes of 0xD50, 0x1C and 0x40. Nothing but a tag
 * looks like that. One under-counting site is pinned exactly:
 * generated/lifted/gen/recomp_0062.c:26011 pushes Tag then NumberOfBytes, THEN
 * branches, and the bracket opens at the branch target with no pushes left inside
 * it. The sibling path of that same branch passes the identical pre-pushed pair to
 * sub_00439A45, which confirms the two values really are the arguments.
 *
 * ARITY-OK(17): one stack argument, the block address. Measured 0 for the same
 * low-direction reason: 8 of the 10 bracketed sites push exactly one argument and it
 * is always a bare pointer (`esi`, `eax`, `MEM32(ebp - 4)`), while the 2
 * zero-counting sites push it and then branch, e.g. recomp_0061.c:30679 pushes
 * `eax` before a jump whose target bracket holds only the return address. A
 * deallocator needs the pointer and cannot be told which block to free by any other
 * means, so 0 stack arguments is not a possible reading -- and ExFreePool is not in
 * the `Kf*` or `Obf*` fastcall families, so the argument is not in ECX either. */
static const struct {
    unsigned ordinal;
    kernel_fn handler;
} bindings[] = {
    {ORD_ExAllocatePoolWithTag, hle_ex_allocate_pool_with_tag},
    {ORD_ExFreePool, hle_ex_free_pool},
    {ORD_ExQueryPoolBlockSize, hle_ex_query_pool_block_size},
};

size_t kernel_pool_register(void)
{
    size_t bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
