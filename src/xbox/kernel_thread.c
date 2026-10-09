/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_thread.h for the measured arity, for how the start-routine entry
 * convention was derived from the guest's own code, and for the measured KPCR
 * layout. Nothing in this file knows what a lifted function looks like: the host
 * injects that through `kernel_thread_host_ops`.
 */

#include "kernel_thread.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <sys/mman.h>
#include <time.h>

#include "guest_mem.h"
#include "kernel_async_io.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_sync.h"
#include "nt_status.h"

#define ORD_PsCreateSystemThreadEx 255u
#define ORD_PsTerminateSystemThread 258u
#define ORD_KeSetBasePriorityThread 143u
#define ORD_KeSetDisableBoostThread 144u
#define ORD_NtResumeThread 224u
#define ORD_NtSuspendThread 231u
#define ORD_NtYieldExecution 238u
#define ORD_NtWaitForSingleObjectEx 234u
#define ORD_NtWaitForSingleObject 233u
#define ORD_KeDelayExecutionThread 99u
/* INFERRED host bound: measured sleeps are millisecond scale. */
#define DELAY_MAX_100NS UINT64_C(100000000)
#define THREAD_WAIT_TIMEOUT 0x00000102u

/* RECALLED from NT, not measured: the guest only sign-tests these results, so the exact
 * value is not load-bearing. */
#define STATUS_OBJECT_TYPE_MISMATCH 0xC0000024u
#define STATUS_THREAD_IS_TERMINATING 0xC000004Bu
#define STATUS_SUSPEND_COUNT_EXCEEDED 0xC000004Au
/* NT's MAXIMUM_SUSPEND_COUNT. */
#define KERNEL_THREAD_SUSPEND_MAX 127u

/* Argument positions, from the measured 10-argument signature. Named rather than
 * inlined because an off-by-one in an argument index is silent: it would read the
 * neighbouring value, which is also a plausible pointer. */
#define ARG_THREAD_HANDLE 0u
#define ARG_THREAD_EXTENSION_SIZE 1u
#define ARG_KERNEL_STACK_SIZE 2u
#define ARG_TLS_DATA_SIZE 3u
#define ARG_THREAD_ID 4u
#define ARG_START_ROUTINE 5u
#define ARG_START_CONTEXT 6u
#define ARG_CREATE_SUSPENDED 7u
#define ARG_DEBUGGER_THREAD 8u
#define ARG_SYSTEM_ROUTINE 9u

/*
 * The return address pushed under a thread's entry routine.
 *
 * Deliberately not a plausible guest VA. The measured exit path is
 * `PsTerminateSystemThread` and an `int3`, never a return, so if this value ever
 * shows up as an indirect-call target or a faulting address then the entry
 * convention in the header is wrong -- and a recognisable constant says so, where
 * a zero would read as an ordinary null-pointer bug.
 */
#define KERNEL_THREAD_SENTINEL_RETURN 0xDEADF00Du

static kernel_thread_record threads[KERNEL_THREAD_MAX];
static unsigned next_slot;

/* Guards every field of `threads`, `next_slot`, `host_ops` and the two sidecar
 * arrays below. One lock for the whole table: thread creation is rare, the
 * critical sections are short, and a finer-grained scheme would buy nothing but
 * the opportunity to get it wrong. */
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
/* Signalled whenever a guest thread finishes. */
static pthread_cond_t table_cond = PTHREAD_COND_INITIALIZER;

/* Host-thread handles, kept out of the public record so kernel_thread.h does not
 * have to include <pthread.h>. */
static pthread_t slot_thread[KERNEL_THREAD_MAX];
static bool slot_joinable[KERNEL_THREAD_MAX];
static kernel_thread_launch slot_launch[KERNEL_THREAD_MAX];
/* Whether the entry frame was built, so a resume knows the thread can be launched. */
static bool slot_frame_ready[KERNEL_THREAD_MAX];
static unsigned unhonoured_suspends;
static unsigned priority_ignored;

static kernel_thread_host_ops host_ops;
static unsigned running_threads;
static unsigned active_waits;
static unsigned active_creates;
static bool reset_in_progress;
/* Identity belongs to the host thread executing enter, never the creator. */
static _Thread_local bool current_thread_valid;
static _Thread_local unsigned current_thread_slot;
static _Thread_local uint32_t current_thread_handle;

/* T371: record the calling guest thread's blocking wait for the quiescence predicate. Caller holds
 * table_lock. A caller without a thread record (the boot thread, host helpers) is not tracked. */
static void block_enter_locked(kernel_thread_block_state state, uint32_t target)
{
    if (current_thread_valid && current_thread_slot < KERNEL_THREAD_MAX &&
        threads[current_thread_slot].in_use &&
        threads[current_thread_slot].handle == current_thread_handle) {
        threads[current_thread_slot].block_state = state;
        threads[current_thread_slot].block_target = target;
    }
}
static void block_leave_locked(void)
{
    block_enter_locked(KERNEL_THREAD_BLOCK_NONE, 0u);
}

/* ------------------------------------------------------------------------- */
/* Stack and control-block arithmetic. Pure, and tested without any lifted code. */
/* ------------------------------------------------------------------------- */

static uint32_t round_up_u32(uint32_t value, uint32_t granularity)
{
    const uint32_t remainder = value % granularity;
    if (remainder == 0u) {
        return value;
    }
    if (value > 0xFFFFFFFFu - (granularity - remainder)) {
        return 0u; /* would wrap; callers treat 0 as a failure */
    }
    return value + (granularity - remainder);
}

uint32_t kernel_thread_stack_size_for(uint32_t requested)
{
    /* A request of 0 is how the guest says "use the default"; its XAPI wrapper has
     * already substituted the XBE header's PeStackCommit by the time we see it, so
     * reaching here with 0 means nobody chose, and the floor is the answer. */
    if (requested < KERNEL_THREAD_STACK_MIN) {
        return KERNEL_THREAD_STACK_MIN;
    }
    if (requested > KERNEL_THREAD_STACK_MAX) {
        return KERNEL_THREAD_STACK_MAX;
    }
    const uint32_t rounded = round_up_u32(requested, GUEST_PAGE_SIZE);
    if (rounded == 0u || rounded > KERNEL_THREAD_STACK_MAX) {
        return KERNEL_THREAD_STACK_MAX;
    }
    return rounded;
}

bool kernel_thread_stack_region_bytes(uint32_t requested, uint32_t *out_bytes)
{
    if (!out_bytes) {
        return false;
    }
    const uint32_t stack = kernel_thread_stack_size_for(requested);
    const uint32_t guard = KERNEL_THREAD_STACK_GUARD;
    if (stack > 0xFFFFFFFFu - 2u * guard) {
        return false;
    }
    *out_bytes = guard + stack + guard;
    return true;
}

bool kernel_thread_stack_layout(kernel_thread_stack *out, uint32_t region_base,
                                uint32_t requested)
{
    if (!out || region_base == 0u) {
        return false;
    }
    /* Page-aligned, because the guard bands are enforced with mprotect() and
     * mprotect rejects an unaligned base. Checked rather than assumed: an
     * unaligned region would produce an unguarded stack that looked guarded. */
    if ((region_base % GUEST_PAGE_SIZE) != 0u) {
        return false;
    }
    uint32_t region_bytes = 0u;
    if (!kernel_thread_stack_region_bytes(requested, &region_bytes)) {
        return false;
    }
    /* The whole region must stay inside the 4 GB guest window: the guest stores
     * esp in four bytes, so a region that straddles the limit would hand out a
     * pointer that is not the address we mapped. */
    if (region_base > 0xFFFFFFFFu - region_bytes) {
        return false;
    }

    const uint32_t guard = KERNEL_THREAD_STACK_GUARD;
    const uint32_t low = region_base + guard;
    const uint32_t stack = region_bytes - 2u * guard;
    /* Stacks grow down, so `high` is where a fresh esp starts from. Masked to 16
     * even though guard and stack are both page multiples, because the invariant
     * that matters to the guest is the alignment, not how it was arrived at. */
    const uint32_t high = (low + stack) & ~15u;
    if (high <= low) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->region_base = region_base;
    out->region_bytes = region_bytes;
    out->guard_bytes = guard;
    out->low = low;
    out->high = high;
    out->esp = 0u;
    out->frame_slots = 0u;
    /* T1146: tag real backing, never count a merely computed/fictitious layout. */
    (void)guest_region_set_usage(region_base, region_bytes, GUEST_MEMORY_STACK);
    return true;
}

bool kernel_thread_stack_arm_guards(kernel_thread_stack *stack)
{
    if (!stack || stack->guard_bytes == 0u) {
        return false;
    }
    /* PROT_NONE rather than a recorded protection value. guest_mem.h is explicit
     * that it does NOT enforce the guest's own page protections, and that is the
     * right call there -- but a stack guard is only worth having if it actually
     * faults, so this one is enforced by the host MMU. */
    void *low_guard = (void *)(uintptr_t)stack->region_base;
    void *high_guard = (void *)(uintptr_t)(stack->low + (stack->region_bytes
                                                         - 2u * stack->guard_bytes));
    kernel_guest_probe_change_begin();
    if (mprotect(low_guard, stack->guard_bytes, PROT_NONE) != 0) {
        kernel_guest_probe_cache_flush();
        return false;
    }
    if (mprotect(high_guard, stack->guard_bytes, PROT_NONE) != 0) {
        /* Put the low band back, so a half-guarded stack is never handed out: a
         * guard on one side only would make an overflow fault in one direction and
         * corrupt silently in the other, which is worse than neither. */
        (void)mprotect(low_guard, stack->guard_bytes, PROT_READ | PROT_WRITE);
        kernel_guest_probe_cache_flush();
        return false;
    }
    kernel_guest_probe_cache_flush();
    return true;
}

bool kernel_thread_stack_write_frame(kernel_thread_stack *stack, const uint32_t *slots,
                                     unsigned count)
{
    if (!stack || !slots || count == 0u || stack->high <= stack->low) {
        return false;
    }
    const uint32_t bytes = count * 4u;
    if (stack->high - stack->low <= bytes) {
        return false;
    }
    const uint32_t esp = stack->high - bytes;
    for (unsigned i = 0u; i < count; i++) {
        if (!kernel_guest_write_u32(esp + i * 4u, slots[i])) {
            return false;
        }
    }
    stack->esp = esp;
    stack->frame_slots = count;
    return true;
}

/* The notify VA the host injected, or 0. See the header for why it is not a
 * parameter of kernel_thread_control_init. */
static uint32_t g_monitor_callback;

void kernel_thread_set_monitor_callback(uint32_t guest_va)
{
    g_monitor_callback = guest_va;
}

uint32_t kernel_thread_monitor_callback(void)
{
    return g_monitor_callback;
}

bool kernel_thread_publish_tls_end(uint32_t control_base, uint32_t tls_base,
                                   uint32_t tls_data_size)
{
    if (control_base == 0u || tls_base == 0u || tls_data_size > 0xFFFFFFFFu - tls_base) {
        return false;
    }
    return kernel_guest_write_u32(control_base + KERNEL_PCR_TLS_END,
                                  tls_base + tls_data_size);
}

bool kernel_thread_control_init(uint32_t control_base, uint32_t tls_base,
                                uint32_t monitor_base, uint32_t monitor_bytes)
{
    if (control_base == 0u || tls_base == 0u) {
        return false;
    }
    /* A null monitor block is not a degraded mode, it is the blocker: the guest's
     * thread startup path tests this field and walks the real kernel's PE headers
     * when it reads 0. Refuse rather than hand back a thread that cannot run.
     *
     * REDUNDANT BY CONSTRUCTION, AND KEPT ANYWAY -- said plainly so nobody has to
     * rediscover it. The zero-filling loop below would reject a null base on its
     * first write, because `kernel_guest_at` returns NULL for address 0, so deleting
     * this `if` does not change what the function returns for any input. Mutation
     * testing confirmed exactly that: removing it kills no test. It stays because it
     * names the precondition at the top of the function instead of leaving it as an
     * emergent property of an unrelated loop, and because the loop is not
     * load-bearing for this -- a future change that pre-zeroes the block elsewhere
     * would silently take the null check away with it. */
    if (monitor_base == 0u) {
        return false;
    }
    /* The guest dereferences monitor+0x24. A block that stops short of that would
     * answer from whatever the allocator placed behind it, which is a wrong answer
     * with no symptom at the point of the mistake. */
    if (monitor_bytes < KERNEL_MONITOR_BYTES_TOUCHED) {
        return false;
    }
    const uint32_t prcb_data = control_base + KERNEL_PCR_PRCB_DATA;
    const uint32_t kthread = control_base + KERNEL_THREAD_KTHREAD_OFFSET;

    /* The Prcb lives inside the control page, so the page has to be long enough for
     * every Prcb byte the guest touches -- up to 0x254 -- and the KTHREAD stub must
     * not start before that. Both are compile-time facts about this layout; they are
     * checked because a future change to either constant would otherwise overlap the
     * two structures silently. */
    if (KERNEL_PCR_PRCB_DATA + KERNEL_PRCB_BYTES_TOUCHED
        > KERNEL_THREAD_KTHREAD_OFFSET) {
        return false;
    }
    if (KERNEL_PCR_PRCB_DATA + KERNEL_PRCB_BYTES_TOUCHED > KERNEL_THREAD_CONTROL_BYTES) {
        return false;
    }

    /* Zero-filled explicitly, not inherited from the allocator. See the header: the
     * guest CALLS monitor+0x14 and dereferences monitor+0x20 and +0x24, so a stale
     * byte here is an indirect call to a made-up address. */
    for (uint32_t offset = 0u; offset < monitor_bytes; offset += 4u) {
        if (!kernel_guest_write_u32(monitor_base + offset, 0u)) {
            return false;
        }
    }

    /* The notify callback, written AFTER the zero-fill so the order cannot silently
     * erase it. Left at zero when nothing was injected, which is the pre-existing
     * behaviour and still the honest one: a host with no callable no-op should get
     * the NULL indirect call and a named stop, not a quietly-skipped notification.
     *
     * +0x20 and +0x24 STAY ZERO, on the record. Both are optional shared blocks the
     * guest only dereferences when non-null, and inventing one would mean inventing
     * the 0x24-byte layout and the 0xABCDEF00 handshake it writes back. A callable
     * notify is one measured fact; those would be guesses. */
    if (g_monitor_callback != 0u
        && !kernel_guest_write_u32(monitor_base + KERNEL_MONITOR_CALLBACK,
                                   g_monitor_callback)) {
        return false;
    }

    /* NtTib.ExceptionList. 0xFFFFFFFF, not 0: __SEH_prolog pushes whatever is
     * here and an unwind walks the chain until it sees the terminator, so a zero
     * would be followed as a frame pointer. */
    if (!kernel_guest_write_u32(control_base + KERNEL_PCR_EXCEPTION_LIST,
                                KERNEL_SEH_CHAIN_END)) {
        return false;
    }
    /* KPCR.Prcb points at this PCR's own PrcbData, which is what makes the 22
     * measured `fs:[0x20]` sites and the 6 `fs:[0x28]` sites agree with each
     * other: both end up reading the same block. */
    if (!kernel_guest_write_u32(control_base + KERNEL_PCR_PRCB, prcb_data)) {
        return false;
    }
    /* KPCR.Irql is a BYTE at 0x24 and the guest reads it as one. The containing word
     * is zeroed first so 0x25..0x27 are known-zero without resting on the allocator
     * handing back zeroed pages, then the byte itself goes through
     * kernel_thread_set_irql -- the same function the host's IRQL publisher calls on
     * every later change, so there is exactly one writer of this field. */
    if (!kernel_guest_write_u32(control_base + KERNEL_PCR_IRQL, 0u)) {
        return false;
    }
    if (!kernel_thread_set_irql(control_base, KERNEL_IRQL_PASSIVE)) {
        return false;
    }
    /* PrcbData.CurrentThread. The startup shim reads this and immediately
     * dereferences it, which is why a thread cannot be entered without one. */
    if (!kernel_guest_write_u32(prcb_data, kthread)) {
        return false;
    }
    if (!kernel_guest_write_u32(kthread + KERNEL_KTHREAD_TLS_DATA, tls_base)) {
        return false;
    }
    /* The title tests only BYTE+4 and reads exit status only when it is nonzero.
     * Initial zero remains truthful until confirmed guest termination. */
    if (!kernel_guest_write_u8(kthread + KERNEL_THREAD_BODY_SIGNAL, 0u) ||
        !kernel_guest_write_u32(kthread + KERNEL_THREAD_BODY_EXIT_STATUS, 0x103u)) {
        return false;
    }
    /* PrcbData+0x250. Written LAST, so a failure anywhere above leaves the field
     * null and the control page is rejected whole rather than half-built with the
     * one field the guest's boot path hinges on already set. */
    if (!kernel_guest_write_u32(prcb_data + KERNEL_PRCB_MONITOR, monitor_base)) {
        return false;
    }
    return true;
}

bool kernel_thread_set_irql(uint32_t control_base, uint32_t level)
{
    if (control_base == 0u) {
        /* A host thread with no KPCR. Rejected rather than ignored: the publisher
         * screens for it, so reaching here means something raised IRQL on a thread
         * that has no guest control page and the guest's copy really is out of
         * date. Saying so beats writing to address 0x24. */
        return false;
    }
    /* KIRQL is a UCHAR. A level above 31 is not representable on the hardware at all,
     * but truncating is still the honest model of a byte-wide field rather than a
     * place to invent a range check the real field does not have. */
    return kernel_guest_write_u8(control_base + KERNEL_PCR_IRQL, (uint8_t)(level & 0xFFu));
}

/* ------------------------------------------------------------------------- */
/* Table bookkeeping. Every counter walks the table under the lock. */
/* ------------------------------------------------------------------------- */

static unsigned count_locked(bool started_wanted, bool unfinished_only)
{
    unsigned count = 0u;
    for (unsigned i = 0u; i < KERNEL_THREAD_MAX; i++) {
        if (!threads[i].in_use || threads[i].started != started_wanted) {
            continue;
        }
        if (unfinished_only && threads[i].finished) {
            continue;
        }
        count++;
    }
    return count;
}

unsigned kernel_thread_unhonoured_suspend_count(void)
{
    pthread_mutex_lock(&table_lock);
    const unsigned count = unhonoured_suspends;
    pthread_mutex_unlock(&table_lock);
    return count;
}

unsigned kernel_thread_priority_ignored_count(void)
{
    pthread_mutex_lock(&table_lock);
    const unsigned count = priority_ignored;
    pthread_mutex_unlock(&table_lock);
    return count;
}

unsigned kernel_thread_unstarted_count(void)
{
    pthread_mutex_lock(&table_lock);
    const unsigned count = count_locked(false, false);
    pthread_mutex_unlock(&table_lock);
    return count;
}

unsigned kernel_thread_started_count(void)
{
    pthread_mutex_lock(&table_lock);
    const unsigned count = count_locked(true, false);
    pthread_mutex_unlock(&table_lock);
    return count;
}

unsigned kernel_thread_running_count(void)
{
    pthread_mutex_lock(&table_lock);
    const unsigned count = running_threads;
    pthread_mutex_unlock(&table_lock);
    return count;
}

bool kernel_thread_get(uint32_t handle, kernel_thread_record *out)
{
    if (!out) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&table_lock);
    for (unsigned i = 0u; i < KERNEL_THREAD_MAX; i++) {
        if (threads[i].in_use && threads[i].handle == handle) {
            *out = threads[i];
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&table_lock);
    return found;
}

unsigned kernel_thread_snapshot(kernel_thread_record *out, unsigned max)
{
    unsigned count = 0u;
    if (!out) {
        return 0u;
    }
    pthread_mutex_lock(&table_lock);
    for (unsigned i = 0u; i < KERNEL_THREAD_MAX && count < max; i++) {
        if (threads[i].in_use) {
            out[count++] = threads[i];
        }
    }
    pthread_mutex_unlock(&table_lock);
    return count;
}

const kernel_thread_record *kernel_thread_find(uint32_t handle)
{
    for (unsigned i = 0u; i < KERNEL_THREAD_MAX; i++) {
        if (threads[i].in_use && threads[i].handle == handle) {
            return &threads[i];
        }
    }
    return NULL;
}

bool kernel_thread_set_host_ops(const kernel_thread_host_ops *ops)
{
    pthread_mutex_lock(&table_lock);
    if (running_threads != 0u || active_waits != 0u ||
        active_creates != 0u || reset_in_progress) {
        const unsigned running = running_threads, waits = active_waits;
        const unsigned creates = active_creates, resetting = reset_in_progress ? 1u : 0u;
        pthread_mutex_unlock(&table_lock);
        kernel_hle_log()("kernel: refusing to change thread host ops -- running=%u, "
                         "waits=%u, creates=%u, resetting=%u\n", running,
                         waits, creates, resetting);
        return false;
    }
    if (ops) {
        host_ops = *ops;
    } else {
        memset(&host_ops, 0, sizeof(host_ops));
    }
    pthread_mutex_unlock(&table_lock);
    return true;
}

/* Release one record's guest allocations and zero it. Touches no shared state, so
 * it is safe on a scratch copy without the lock and on a table entry with it. */
static void release_record(kernel_thread_record *record)
{
    if (record->stack_region != 0u) {
        /* The guard bands are PROT_NONE, which does not stop the munmap inside
         * guest_region_free -- protection and mapping are separate. */
        (void)guest_region_free(record->stack_region);
    }
    if (record->control_base != 0u) {
        (void)guest_region_free(record->control_base);
    }
    if (record->tls_base != 0u) {
        (void)guest_region_free(record->tls_base);
    }
    if (record->monitor_base != 0u) {
        (void)guest_region_free(record->monitor_base);
    }
    *record = (kernel_thread_record){0};
}

bool kernel_thread_reset(void)
{
    pthread_mutex_lock(&table_lock);
    if (running_threads != 0u || active_waits != 0u ||
        active_creates != 0u || reset_in_progress) {
        const unsigned running = running_threads, waits = active_waits;
        const unsigned creates = active_creates, resetting = reset_in_progress ? 1u : 0u;
        pthread_mutex_unlock(&table_lock);
        kernel_hle_log()("kernel: refusing to reset the thread table -- running=%u, "
                         "waits=%u, creates=%u, resetting=%u\n", running,
                         waits, creates, resetting);
        return false;
    }
    /* Atomic object-lock preflight/detach precedes every possible unmap. A
     * concurrent 246 either owns a reference (refusing reset) or sees no handle. */
    if (!kernel_object_detach_thread_bodies()) {
        pthread_mutex_unlock(&table_lock);
        kernel_hle_log()("kernel: refusing thread reset with retained body references\n");
        return false;
    }
    reset_in_progress = true;
    for (unsigned i = 0u; i < KERNEL_THREAD_MAX; i++) {
        if (slot_joinable[i]) {
            pthread_t handle = slot_thread[i];
            slot_joinable[i] = false;
            pthread_mutex_unlock(&table_lock);
            (void)pthread_join(handle, NULL);
            pthread_mutex_lock(&table_lock);
        }
        release_record(&threads[i]);
        slot_launch[i] = (kernel_thread_launch){0};
        slot_frame_ready[i] = false;
    }
    next_slot = 0u;
    unhonoured_suspends = 0u;
    priority_ignored = 0u;
    reset_in_progress = false;
    pthread_mutex_unlock(&table_lock);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Starting and joining. */
/* ------------------------------------------------------------------------- */

static void *guest_thread_main(void *argument)
{
    const unsigned slot = (unsigned)(uintptr_t)argument;

    pthread_mutex_lock(&table_lock);
    const kernel_thread_launch launch = slot_launch[slot];
    void (*enter)(const kernel_thread_launch *) = host_ops.enter;
    bool (*termination_confirmed)(const kernel_thread_launch *) =
        host_ops.termination_confirmed;
    pthread_mutex_unlock(&table_lock);

    current_thread_slot = slot;
    current_thread_handle = launch.handle;
    current_thread_valid = true;
    if (enter) {
        enter(&launch);
    }
    /* The host knows whether its non-local exit represented guest termination.
     * No table lock is held while consulting that same-thread outcome. */
    const bool confirmed = termination_confirmed && termination_confirmed(&launch);

    pthread_mutex_lock(&table_lock);
    const bool requested = threads[slot].termination_requested;
    const uint32_t body = threads[slot].control_base + KERNEL_THREAD_KTHREAD_OFFSET;
    const uint32_t status = threads[slot].exit_eax;
    pthread_mutex_unlock(&table_lock);
    /* Still counted running, so reset cannot unmap this owned storage. Guarded
     * copies never fault while holding table/object locks. Status precedes signal;
     * this is ordered publication in the existing guest-memory model, not an
     * atomic compound operation or an ISO C acquire/release protocol for raw
     * generated guest loads. External remapping/tampering is unsupported. */
    bool published = false;
    if (requested && confirmed) {
        published = kernel_guest_write_u32(body + KERNEL_THREAD_BODY_EXIT_STATUS, status) &&
                    kernel_guest_write_u8(body + KERNEL_THREAD_BODY_SIGNAL, 1u);
        if (!published) kernel_hle_log()("kernel: thread body termination publication failed\n");
    }
    pthread_mutex_lock(&table_lock);
    threads[slot].terminated = requested && confirmed && published;
    threads[slot].finished = true;
    if (running_threads > 0u) {
        running_threads--;
    }
    pthread_cond_broadcast(&table_cond);
    pthread_mutex_unlock(&table_lock);
    current_thread_valid = false;
    current_thread_handle = 0u;
    return NULL;
}

unsigned kernel_thread_join_all(unsigned timeout_ms)
{
    struct timespec deadline;
    if (timespec_get(&deadline, TIME_UTC) != TIME_UTC) {
        /* No clock, so no watchdog is possible. Say so rather than waiting
         * forever, which is the exact outcome the watchdog exists to prevent. */
        kernel_hle_log()("kernel: timespec_get failed; cannot time a thread join\n");
        return kernel_thread_running_count();
    }
    deadline.tv_sec += (time_t)(timeout_ms / 1000u);
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
    }

    pthread_t reap[KERNEL_THREAD_MAX];
    unsigned reap_count = 0u;

    pthread_mutex_lock(&table_lock);
    while (running_threads > 0u) {
        const int rc = pthread_cond_timedwait(&table_cond, &table_lock, &deadline);
        if (rc == ETIMEDOUT) {
            break;
        }
        if (rc != 0) {
            /* Not a timeout and not success. Stop rather than spin: a failing wait
             * in a loop is an invisible busy-wait. */
            break;
        }
    }
    const unsigned still_running = running_threads;
    /* Reap only the finished ones, so this cannot block past the deadline. */
    for (unsigned i = 0u; i < KERNEL_THREAD_MAX; i++) {
        if (slot_joinable[i] && threads[i].finished) {
            reap[reap_count++] = slot_thread[i];
            slot_joinable[i] = false;
        }
    }
    pthread_mutex_unlock(&table_lock);

    for (unsigned i = 0u; i < reap_count; i++) {
        (void)pthread_join(reap[i], NULL);
    }
    return still_running;
}

/*
 * Allocate a stack, a control page and a TLS block for one thread.
 *
 * Called WITHOUT the table lock held, and that is deliberate: it writes guest
 * memory, and a guest write can fault. A fault inside lifted code is a
 * `siglongjmp` that releases nothing, so a lock held across one stays locked for
 * the life of the process -- and the first thing that would then block on it is
 * `kernel_thread_join_all`, i.e. the watchdog whose entire job is to turn a hang
 * into a report. The watchdog must not be the thing that hangs.
 *
 * `record` is the caller's own scratch copy; nothing is published until it is
 * filled. On any failure every allocation made here is released, so a half-built
 * thread is never recorded: the guest would otherwise hold a handle to a record
 * whose esp points at memory we do not own.
 */
static nt_status provision_thread(kernel_thread_record *record,
                                  kernel_thread_stack *stack)
{
    uint32_t region_bytes = 0u;
    if (!kernel_thread_stack_region_bytes(record->kernel_stack_size, &region_bytes)) {
        return STATUS_INVALID_PARAMETER;
    }

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = region_bytes;
    request.alignment = GUEST_ALLOCATION_GRANULARITY;
    /* Both are mandatory. guest_region_alloc answers
     * STATUS_INVALID_PAGE_PROTECTION for a zero `protect` and rejects a state
     * with neither MEM_COMMIT nor MEM_RESERVE set. */
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;

    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr region = guest_region_alloc(&request, &status);
    if (region == 0u) {
        return status;
    }
    if (!kernel_thread_stack_layout(stack, region, record->kernel_stack_size)) {
        (void)guest_region_free(region);
        return STATUS_INVALID_PARAMETER;
    }
    record->stack_region = region;
    record->stack_region_bytes = stack->region_bytes;
    record->stack_low = stack->low;
    record->stack_high = stack->high;

    request.bytes = KERNEL_THREAD_CONTROL_BYTES;
    request.alignment = 0u;
    const kernel_guest_ptr control = guest_region_alloc(&request, &status);
    if (control == 0u) {
        (void)guest_region_free(region);
        record->stack_region = 0u;
        return status;
    }
    record->control_base = control;

    uint32_t tls_bytes = record->tls_data_size;
    if (tls_bytes < KERNEL_THREAD_TLS_MIN) {
        tls_bytes = KERNEL_THREAD_TLS_MIN;
    }
    tls_bytes = round_up_u32(tls_bytes, GUEST_PAGE_SIZE);
    if (tls_bytes == 0u) {
        (void)guest_region_free(region);
        (void)guest_region_free(control);
        record->stack_region = 0u;
        record->control_base = 0u;
        return STATUS_INVALID_PARAMETER;
    }
    request.bytes = tls_bytes;
    const kernel_guest_ptr tls = guest_region_alloc(&request, &status);
    if (tls == 0u) {
        (void)guest_region_free(region);
        (void)guest_region_free(control);
        record->stack_region = 0u;
        record->control_base = 0u;
        return status;
    }
    record->tls_base = tls;

    /* The Prcb monitor block, in its own region. A separate allocation rather than
     * a corner of the control page, for the reason in kernel_thread.h: the guest
     * calls through monitor+0x14, and a block sharing a page with the KPCR would let
     * whatever answers that call corrupt the KPCR instead of faulting. */
    request.bytes = KERNEL_THREAD_MONITOR_BYTES;
    const kernel_guest_ptr monitor = guest_region_alloc(&request, &status);
    if (monitor == 0u) {
        /* release_record handles the three regions already recorded. */
        release_record(record);
        return status;
    }
    record->monitor_base = monitor;

    if (!kernel_thread_control_init(control, tls, monitor,
                                    KERNEL_THREAD_MONITOR_BYTES)) {
        release_record(record);
        return STATUS_INVALID_PARAMETER;
    }
    if (!kernel_thread_publish_tls_end(control, tls, record->tls_data_size)) {
        release_record(record);
        return STATUS_INVALID_PARAMETER;
    }

    /* Guards last, so the two failure paths above never leave a PROT_NONE band
     * behind on a region that has already been handed back to the allocator. */
    record->guards_armed = kernel_thread_stack_arm_guards(stack);
    if (!record->guards_armed) {
        /* Not fatal: an unguarded stack still runs. It is reported because a
         * stack overflow would then corrupt a neighbour silently, and that is a
         * bug class we would otherwise spend days misattributing. */
        kernel_hle_log()("kernel: WARNING could not guard the stack at %#x; an "
                         "overflow will corrupt a neighbour instead of faulting\n",
                         (unsigned)stack->region_base);
    }
    return STATUS_SUCCESS;
}

/*
 * Decide which routine the thread is entered at and lay out its entry frame.
 *
 * Called WITHOUT the table lock, because it writes guest memory. Operates on the
 * caller's scratch record; nothing is published.
 */
static bool build_entry_frame(kernel_thread_record *record, kernel_thread_stack *stack)
{
    /* The entry convention, derived in kernel_thread.h: enter SystemRoutine with
     * (StartRoutine, StartContext), because the routine the guest supplies calls
     * its first argument and passes its second. With none supplied, enter
     * StartRoutine with (StartContext) -- which is what that routine would have
     * done anyway. */
    uint32_t slots[3];
    unsigned slot_count;
    if (record->system_routine != 0u) {
        record->entry_va = record->system_routine;
        slots[0] = KERNEL_THREAD_SENTINEL_RETURN;
        slots[1] = record->start_routine;
        slots[2] = record->start_context;
        slot_count = 3u;
    } else {
        record->entry_va = record->start_routine;
        slots[0] = KERNEL_THREAD_SENTINEL_RETURN;
        slots[1] = record->start_context;
        slot_count = 2u;
    }

    if (!kernel_thread_stack_write_frame(stack, slots, slot_count)) {
        kernel_hle_log()("kernel: thread %#x could not write its entry frame into "
                         "the stack at %#x-%#x\n",
                         (unsigned)record->handle, (unsigned)stack->low,
                         (unsigned)stack->high);
        return false;
    }
    record->initial_esp = stack->esp;
    return true;
}

/*
 * Publish a fully-provisioned record into the table and start its host thread.
 *
 * Caller holds the lock. Touches NO guest memory, so nothing in here can fault and
 * strand the lock.
 *
 * Returns false when nothing was started, having already said why. That is not a
 * failure of the ordinal: a recorded-but-unstarted thread is the documented
 * fallback and the guest's handle is still valid either way.
 */
static bool launch_locked(unsigned slot)
{
    kernel_thread_record *record = &threads[slot];

    if (!host_ops.enter) {
        kernel_hle_log()("kernel: thread %#x requested at start_routine %#x -- NOT "
                         "STARTED (no host thread ops registered)\n",
                         (unsigned)record->handle, (unsigned)record->start_routine);
        return false;
    }
    if (host_ops.has_code && !host_ops.has_code(record->entry_va)) {
        kernel_hle_log()("kernel: thread %#x NOT STARTED -- no runnable code at its "
                         "entry %#x (start_routine %#x, system_routine %#x)\n",
                         (unsigned)record->handle, (unsigned)record->entry_va,
                         (unsigned)record->start_routine,
                         (unsigned)record->system_routine);
        return false;
    }

    slot_launch[slot] = (kernel_thread_launch){
        .handle = record->handle,
        .entry_va = record->entry_va,
        .esp = record->initial_esp,
        .fs_base = record->control_base,
        .start_routine = record->start_routine,
        .start_context = record->start_context,
        .stack_low = record->stack_low,
        .stack_high = record->stack_high,
    };

    /* Marked started BEFORE the host thread exists, because the new thread
     * publishes `finished` and decrements the running count: a thread that
     * finished before we got round to marking it started would be recorded as
     * never-started-but-finished, which is not a state anything can interpret. */
    record->started = true;
    record->finished = false;
    running_threads++;

    pthread_t handle;
    const int rc = pthread_create(&handle, NULL, guest_thread_main,
                                  (void *)(uintptr_t)slot);
    if (rc != 0) {
        record->started = false;
        running_threads--;
        kernel_hle_log()("kernel: thread %#x NOT STARTED -- pthread_create failed "
                         "(errno %d)\n",
                         (unsigned)record->handle, rc);
        return false;
    }
    slot_thread[slot] = handle;
    slot_joinable[slot] = true;

    kernel_hle_log()("kernel: thread %#x STARTED  entry %#x  esp %#x  fs %#x  "
                     "stack %#x-%#x (%u KiB, %u KiB guards)\n",
                     (unsigned)record->handle, (unsigned)record->entry_va,
                     (unsigned)record->initial_esp, (unsigned)record->control_base,
                     (unsigned)record->stack_low, (unsigned)record->stack_high,
                     (unsigned)((record->stack_high - record->stack_low) / 1024u),
                     (unsigned)(KERNEL_THREAD_STACK_GUARD / 1024u));
    return true;
}

static bool publish_and_start_locked(unsigned slot, const kernel_thread_record *built,
                                     bool entry_frame_ready)
{
    threads[slot] = *built;
    slot_frame_ready[slot] = entry_frame_ready;
    kernel_thread_record *record = &threads[slot];

    if (!entry_frame_ready) {
        return false; /* build_entry_frame already said why */
    }
    if (record->created_suspended) {
        kernel_hle_log()("kernel: thread %#x created SUSPENDED at %#x -- NOT STARTED until "
                         "NtResumeThread takes its suspend count (%u) to 0\n",
                         (unsigned)record->handle, (unsigned)record->entry_va,
                         (unsigned)record->suspend_count);
        return false;
    }
    return launch_locked(slot);
}

/*
 * Give back a slot claimed in phase 1 whose provisioning then failed.
 *
 * The guest allocations are released BEFORE the lock is taken, so no `munmap` runs
 * inside the critical section. `next_slot` is rewound only when this slot is still
 * the most recent claim; under a concurrent create it will not be, and leaving one
 * slot free-but-skipped costs capacity whereas rewinding past another thread's
 * claim would hand the same record to two threads.
 *
 * The handle issued by `kernel_object_create` is WITHDRAWN here. It used to leak one
 * object-table entry per failed creation, recorded as a known defect because the table
 * had no release call and never recycled; it has both now, and a released slot comes back
 * under a bumped generation so the withdrawn value cannot alias whatever reuses the slot.
 */
static void unclaim_slot(unsigned slot, kernel_thread_record *built)
{
    /* Captured BEFORE release_record, which zeroes the whole record, handle included: the
     * first version of this fix released handle 0 and leaked exactly as before. */
    const uint32_t issued_handle = built->handle;
    /* Identity must disappear before its owned mapping. No successful handle
     * publication can reach this rollback path, so outstanding refs are fatal. */
    if (!kernel_object_rollback_thread_body(issued_handle, slot)) abort();
    release_record(built);
    pthread_mutex_lock(&table_lock);
    active_creates--;
    threads[slot] = (kernel_thread_record){0};
    if (next_slot == slot + 1u) {
        next_slot--;
    }
    pthread_mutex_unlock(&table_lock);
}

static uint32_t handle_ps_create_system_thread_ex(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t handle_out = 0u;
    uint32_t id_out = 0u;
    uint32_t stack_size = 0u;
    uint32_t tls_size = 0u;
    uint32_t start_routine = 0u;
    uint32_t start_context = 0u;
    uint32_t system_routine = 0u;
    uint32_t suspended = 0u;

    /* The handle out-parameter is the only one we cannot proceed without: the guest
     * dereferences it immediately. Everything else is recorded for whoever
     * eventually starts the thread. */
    if (!kernel_frame_arg(frame, ARG_THREAD_HANDLE, &handle_out) || handle_out == 0u) {
        kernel_hle_log()("kernel: PsCreateSystemThreadEx with no ThreadHandle out-pointer\n");
        return STATUS_INVALID_PARAMETER;
    }
    (void)kernel_frame_arg(frame, ARG_THREAD_ID, &id_out);
    (void)kernel_frame_arg(frame, ARG_KERNEL_STACK_SIZE, &stack_size);
    (void)kernel_frame_arg(frame, ARG_TLS_DATA_SIZE, &tls_size);
    (void)kernel_frame_arg(frame, ARG_START_ROUTINE, &start_routine);
    (void)kernel_frame_arg(frame, ARG_START_CONTEXT, &start_context);
    (void)kernel_frame_arg(frame, ARG_CREATE_SUSPENDED, &suspended);
    (void)kernel_frame_arg(frame, ARG_SYSTEM_ROUTINE, &system_routine);

    /* ---- phase 1: claim a slot and a handle, under the lock ---------------
     *
     * Claimed by setting `in_use` immediately, which the original code did not do:
     * a slot that is chosen but not marked can be chosen again by a concurrent
     * create, and both threads then fill the same record. The claimed-but-unfilled
     * state is honestly describable -- "requested, not started" -- so the counters
     * stay correct throughout. */
    pthread_mutex_lock(&table_lock);
    if (reset_in_progress) {
        pthread_mutex_unlock(&table_lock);
        kernel_hle_log()("kernel: PsCreateSystemThreadEx refused during reset\n");
        return STATUS_NOT_IMPLEMENTED;
    }
    if (next_slot >= KERNEL_THREAD_MAX) {
        pthread_mutex_unlock(&table_lock);
        kernel_hle_log()("kernel: PsCreateSystemThreadEx exhausted %u thread slots\n",
                         (unsigned)KERNEL_THREAD_MAX);
        return STATUS_NO_MEMORY;
    }
    const unsigned slot = next_slot;

    /* Issue through the shared handle table so NtClose can find this without
     * knowing that threads exist. The boot trace shows the guest closing exactly
     * the handle we return here, so the two must agree on handle space. */
    const uint32_t issued = kernel_object_create(KERNEL_OBJECT_THREAD, slot);
    if (issued == 0u) {
        pthread_mutex_unlock(&table_lock);
        kernel_hle_log()("kernel: PsCreateSystemThreadEx could not issue a handle\n");
        return STATUS_NO_MEMORY;
    }

    kernel_thread_record built = {0};
    built.handle = issued;
    built.thread_id = issued;
    built.start_routine = start_routine;
    built.start_context = start_context;
    built.system_routine = system_routine;
    built.kernel_stack_size = stack_size;
    built.tls_data_size = tls_size;
    built.created_suspended = (suspended != 0u);
    built.suspend_count = built.created_suspended ? 1u : 0u;
    built.in_use = true;

    threads[slot] = built;
    next_slot++;
    active_creates++;
    pthread_mutex_unlock(&table_lock);

    /* ---- phase 2: allocate and write guest memory, UNLOCKED ---------------
     *
     * Everything here can fault, and a fault is a siglongjmp that would strand the
     * table lock and hang the watchdog. `built` is this thread's own copy. */
    kernel_thread_stack stack;
    memset(&stack, 0, sizeof(stack));
    const nt_status provisioned = provision_thread(&built, &stack);
    if (provisioned != STATUS_SUCCESS) {
        unclaim_slot(slot, &built);
        kernel_hle_log()("kernel: PsCreateSystemThreadEx could not provision a "
                         "%u-byte stack for %#x (status %#x)\n",
                         (unsigned)kernel_thread_stack_size_for(stack_size),
                         (unsigned)start_routine, (unsigned)provisioned);
        return provisioned;
    }

    const bool frame_ready = build_entry_frame(&built, &stack);

    /* Publish the complete record, mapped identity and handle before launch.
     * The object lock spans identity binding + guarded handle-out copy, so a
     * failed copy cannot leave an externally referenced mapping to roll back. */
    pthread_mutex_lock(&table_lock);
    threads[slot] = built;
    if (!kernel_object_publish_thread_body(built.handle, slot,
            built.control_base + KERNEL_THREAD_KTHREAD_OFFSET, handle_out)) {
        pthread_mutex_unlock(&table_lock);
        unclaim_slot(slot, &built);
        kernel_hle_log()("kernel: PsCreateSystemThreadEx cannot publish ThreadHandle at %#x\n",
                         (unsigned)handle_out);
        return STATUS_INVALID_PARAMETER;
    }
    if (id_out != 0u && !kernel_object_write_optional_thread_id(id_out, built.thread_id)) {
        kernel_hle_log()("kernel: PsCreateSystemThreadEx cannot write ThreadId at %#x\n",
                         (unsigned)id_out);
    }
    active_creates--;
    const bool launched = publish_and_start_locked(slot, &built, frame_ready);
    const unsigned unstarted = count_locked(false, false);
    pthread_mutex_unlock(&table_lock);

    if (!launched) {
        /* Loud by design. If the guest later waits on this handle it waits
         * forever, and this count is what explains why. */
        kernel_hle_log()("kernel: %u guest thread(s) requested and not running\n",
                         unstarted);
    }
    return STATUS_SUCCESS;
}

static uint32_t handle_ps_terminate_system_thread(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t status = 0u;
    if (!kernel_frame_arg(frame, 0u, &status)) {
        kernel_hle_log()("kernel: PsTerminateSystemThread could not read exit status\n");
        return STATUS_INVALID_PARAMETER;
    }

    pthread_mutex_lock(&table_lock);
    void (*terminate)(uint32_t) = host_ops.terminate;
    const bool identity_valid = current_thread_valid &&
        current_thread_slot < KERNEL_THREAD_MAX &&
        threads[current_thread_slot].in_use &&
        threads[current_thread_slot].handle == current_thread_handle &&
        threads[current_thread_slot].started && !threads[current_thread_slot].finished;
    if (!identity_valid || !terminate) {
        pthread_mutex_unlock(&table_lock);
        kernel_hle_log()("kernel: PsTerminateSystemThread REFUSED -- no current "
                         "guest thread identity or nonreturning host hook\n");
        return STATUS_NOT_IMPLEMENTED;
    }
    threads[current_thread_slot].exit_eax = status;
    threads[current_thread_slot].termination_requested = true;
    pthread_mutex_unlock(&table_lock);

    kernel_hle_log()("kernel: PsTerminateSystemThread(%#x)\n", (unsigned)status);
    terminate(status);
    /* A returning hook did not terminate the guest. Withdraw the request before
     * exposing any later host completion; the current identity remains installed. */
    pthread_mutex_lock(&table_lock);
    threads[current_thread_slot].termination_requested = false;
    threads[current_thread_slot].exit_eax = 0u;
    pthread_mutex_unlock(&table_lock);
    kernel_hle_log()("kernel: PsTerminateSystemThread REFUSED -- host terminate hook returned\n");
    return STATUS_NOT_IMPLEMENTED;
}

/* ---------------------------------------------------------------------------
 * NtSuspendThread, NtResumeThread, KeSetBasePriorityThread, KeSetDisableBoostThread.
 * ------------------------------------------------------------------------- */

/* Classify a handle (or the "object pointer" 143/144 receive, which this host makes the
 * handle too). Reports every refusal. Called without the table lock: kernel_object has its
 * own lock and creation takes the table lock first, so the order stays table then object. */
static nt_status classify_thread_handle(uint32_t handle, const char *who)
{
    const kernel_object_entry *entry = kernel_object_find(handle);
    if (!entry) {
        kernel_hle_log()("kernel: %s(%#x) -- not a live handle, REFUSED\n", who,
                         (unsigned)handle);
        return STATUS_INVALID_HANDLE;
    }
    if (entry->kind != KERNEL_OBJECT_THREAD) {
        kernel_hle_log()("kernel: %s(%#x) -- handle is not a thread (kind %d), REFUSED\n", who,
                         (unsigned)handle, (int)entry->kind);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    return STATUS_SUCCESS;
}

/* Caller holds the lock. -1 when the object table says thread but this table disagrees. */
static int find_slot_locked(uint32_t handle)
{
    for (unsigned i = 0u; i < KERNEL_THREAD_MAX; i++) {
        if (threads[i].in_use && threads[i].handle == handle) {
            return (int)i;
        }
    }
    return -1;
}

/* An out pointer is checked BEFORE acting, so a refused write never leaves the operation
 * half done. NULL is legal: the count is optional. */
static bool out_pointer_usable(uint32_t pointer, const char *who)
{
    if (pointer != 0u && kernel_guest_at(pointer, sizeof(uint32_t)) == NULL) {
        kernel_hle_log()("kernel: %s -- PreviousSuspendCount pointer %#x is not writable "
                         "guest memory, REFUSED before acting\n",
                         who, (unsigned)pointer);
        return false;
    }
    return true;
}

static void write_previous_count(uint32_t pointer, uint32_t count, const char *who)
{
    if (pointer != 0u && !kernel_guest_write_u32(pointer, count)) {
        kernel_hle_log()("kernel: %s could not write the previous count to %#x\n", who,
                         (unsigned)pointer);
    }
}

/*
 * ARITY-OK(231): TWO stack arguments, (Handle, &PreviousSuspendCount). One call site,
 * 0x37FCC6, below the measured table's 3-site quorum, so it rests on the oracle (nxdk .def
 * `NtSuspendThread@8`, a different kernel build) AND on the wrapper's frame: `push ebp; mov
 * ebp,esp; lea eax,[ebp+8]; push eax; push [ebp+8]; call; ... ret 4` pushes exactly two, the
 * out pointer aliasing the wrapper's own argument slot (so the handle is read before the
 * count is written).
 *
 * A running host thread cannot be paused: the lifted code has no safe point and a pthread
 * cannot be stopped from outside without a signal that would land mid-instruction-sequence
 * in generated C. So a suspend of a started thread is REFUSED, not claimed. The only caller
 * (0x28D8F) ignores the result, and the wrapper maps status<0 to -1 (MEASURED).
 */
static uint32_t handle_nt_suspend_thread(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t handle = 0u;
    uint32_t previous_out = 0u;
    if (!kernel_frame_arg(frame, 0u, &handle) || !kernel_frame_arg(frame, 1u, &previous_out)) {
        kernel_hle_log()("kernel: NtSuspendThread could not read its two arguments\n");
        return STATUS_INVALID_PARAMETER;
    }
    const nt_status classified = classify_thread_handle(handle, "NtSuspendThread");
    if (classified != STATUS_SUCCESS) {
        return classified;
    }
    if (!out_pointer_usable(previous_out, "NtSuspendThread")) {
        return STATUS_ACCESS_VIOLATION;
    }

    pthread_mutex_lock(&table_lock);
    const int slot = find_slot_locked(handle);
    if (slot < 0) {
        pthread_mutex_unlock(&table_lock);
        kernel_hle_log()("kernel: NtSuspendThread(%#x) -- the object table says thread but "
                         "the thread table has no record, REFUSED\n",
                         (unsigned)handle);
        return STATUS_INVALID_HANDLE;
    }
    kernel_thread_record *record = &threads[slot];
    nt_status status = STATUS_SUCCESS;
    const uint32_t previous = record->suspend_count;
    if (record->finished) {
        status = STATUS_THREAD_IS_TERMINATING;
    } else if (record->started) {
        unhonoured_suspends++;
        status = STATUS_NOT_IMPLEMENTED;
    } else if (record->suspend_count >= KERNEL_THREAD_SUSPEND_MAX) {
        status = STATUS_SUSPEND_COUNT_EXCEEDED;
    } else {
        record->suspend_count++;
    }
    const unsigned refused_so_far = unhonoured_suspends;
    pthread_mutex_unlock(&table_lock);

    if (status == STATUS_THREAD_IS_TERMINATING) {
        kernel_hle_log()("kernel: NtSuspendThread(%#x) on a FINISHED thread, REFUSED\n",
                         (unsigned)handle);
    } else if (status == STATUS_NOT_IMPLEMENTED) {
        kernel_hle_log()("kernel: NtSuspendThread(%#x) REFUSED -- the thread is running and a "
                         "host thread cannot be paused, so it KEEPS RUNNING (unhonoured "
                         "suspend %u)\n",
                         (unsigned)handle, refused_so_far);
    } else if (status == STATUS_SUSPEND_COUNT_EXCEEDED) {
        kernel_hle_log()("kernel: NtSuspendThread(%#x) REFUSED -- suspend count is already "
                         "%u\n",
                         (unsigned)handle, (unsigned)previous);
    } else {
        write_previous_count(previous_out, previous, "NtSuspendThread");
    }
    return status;
}

/*
 * ARITY-OK(224): TWO stack arguments, (Handle, &PreviousSuspendCount). One call site,
 * 0x37FCEC, same wrapper shape as 231 (`ret 4` over an aliased argument slot), oracle
 * `NtResumeThread@8`. Its only caller (0x2900F) ignores the result.
 *
 * The count is the thread's REAL suspend count: a CreateSuspended thread begins at 1 and the
 * thread starts when this takes it to 0, which is what the XAPI CreateThread(CREATE_SUSPENDED)
 * then ResumeThread sequence needs. A resume of a thread that is not suspended returns 0 and
 * changes nothing.
 */
static uint32_t handle_nt_resume_thread(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t handle = 0u;
    uint32_t previous_out = 0u;
    if (!kernel_frame_arg(frame, 0u, &handle) || !kernel_frame_arg(frame, 1u, &previous_out)) {
        kernel_hle_log()("kernel: NtResumeThread could not read its two arguments\n");
        return STATUS_INVALID_PARAMETER;
    }
    const nt_status classified = classify_thread_handle(handle, "NtResumeThread");
    if (classified != STATUS_SUCCESS) {
        return classified;
    }
    if (!out_pointer_usable(previous_out, "NtResumeThread")) {
        return STATUS_ACCESS_VIOLATION;
    }

    pthread_mutex_lock(&table_lock);
    const int slot = find_slot_locked(handle);
    if (slot < 0) {
        pthread_mutex_unlock(&table_lock);
        kernel_hle_log()("kernel: NtResumeThread(%#x) -- the object table says thread but "
                         "the thread table has no record, REFUSED\n",
                         (unsigned)handle);
        return STATUS_INVALID_HANDLE;
    }
    kernel_thread_record *record = &threads[slot];
    const uint32_t previous = record->suspend_count;
    const bool finished = record->finished;
    bool wanted_start = false;
    bool launched = false;
    bool frame_missing = false;
    if (!finished && record->suspend_count > 0u) {
        record->suspend_count--;
        wanted_start = record->suspend_count == 0u && !record->started;
        if (wanted_start && slot_frame_ready[slot]) {
            launched = launch_locked((unsigned)slot);
        }
        frame_missing = wanted_start && !slot_frame_ready[slot];
    }
    pthread_mutex_unlock(&table_lock);

    if (finished) {
        kernel_hle_log()("kernel: NtResumeThread(%#x) on a FINISHED thread, nothing to "
                         "resume\n",
                         (unsigned)handle);
    } else if (frame_missing) {
        kernel_hle_log()("kernel: NtResumeThread(%#x) took the count to 0 but the thread has "
                         "no entry frame, NOT STARTED\n",
                         (unsigned)handle);
    } else if (wanted_start && !launched) {
        kernel_hle_log()("kernel: NtResumeThread(%#x) took the count to 0 but the thread was "
                         "NOT STARTED (reason above)\n",
                         (unsigned)handle);
    }
    write_previous_count(previous_out, previous, "NtResumeThread");
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(143): TWO stack arguments, (Thread, Increment). One call site, 0x37FC56, in the
 * SetThreadPriority wrapper at 0x37FC24 (`ret 8`), which maps 15 to 16 and -15 to -16 and
 * then dereferences the thread. Oracle `KeSetBasePriorityThread@8`. No result is used.
 * Returns the previous increment (NT semantics, INFERRED, since the guest ignores it).
 * RECORDED ONLY: priorities do not change host scheduling.
 */
static uint32_t handle_ke_set_base_priority_thread(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t object = 0u;
    uint32_t increment = 0u;
    if (!kernel_frame_arg(frame, 0u, &object) || !kernel_frame_arg(frame, 1u, &increment)) {
        kernel_hle_log()("kernel: KeSetBasePriorityThread could not read its two arguments\n");
        return 0u;
    }
    pthread_mutex_lock(&table_lock);
    kernel_object_entry identity;
    const bool mapped = kernel_object_get_thread_body_copy(object, &identity);
    const int candidate = mapped ? find_slot_locked(identity.handle) : -1;
    const int slot = candidate >= 0 && identity.owner_tag == (unsigned)candidate &&
        threads[candidate].control_base + KERNEL_THREAD_KTHREAD_OFFSET == object ? candidate : -1;
    uint32_t previous = 0u;
    unsigned ignored = 0u;
    if (slot >= 0) {
        previous = (uint32_t)threads[slot].base_priority_increment;
        threads[slot].base_priority_increment = (int32_t)increment;
        threads[slot].base_priority_set = true;
        ignored = ++priority_ignored;
    }
    pthread_mutex_unlock(&table_lock);
    if (slot < 0) {
        kernel_hle_log()("kernel: KeSetBasePriorityThread(%#x) -- no thread record, REFUSED\n",
                         (unsigned)object);
        return 0u;
    }
    kernel_hle_log()("kernel: KeSetBasePriorityThread(%#x, %d) RECORDED ONLY -- host "
                     "scheduling is not changed (ignored request %u)\n",
                     (unsigned)object, (int)(int32_t)increment, ignored);
    return previous;
}

/*
 * ARITY-OK(144): TWO stack arguments, (Thread, Disable). One call site, 0x37FC9C, in the
 * SetThreadPriorityBoost wrapper at 0x37FC76 (`ret 8`), Disable from `setne al`. Oracle
 * `KeSetDisableBoostThread@8`. No result is used. Returns the previous flag (INFERRED).
 * RECORDED ONLY.
 */
static uint32_t handle_ke_set_disable_boost_thread(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t object = 0u;
    uint32_t disable = 0u;
    if (!kernel_frame_arg(frame, 0u, &object) || !kernel_frame_arg(frame, 1u, &disable)) {
        kernel_hle_log()("kernel: KeSetDisableBoostThread could not read its two arguments\n");
        return 0u;
    }
    pthread_mutex_lock(&table_lock);
    kernel_object_entry identity;
    const bool mapped = kernel_object_get_thread_body_copy(object, &identity);
    const int candidate = mapped ? find_slot_locked(identity.handle) : -1;
    const int slot = candidate >= 0 && identity.owner_tag == (unsigned)candidate &&
        threads[candidate].control_base + KERNEL_THREAD_KTHREAD_OFFSET == object ? candidate : -1;
    uint32_t previous = 0u;
    unsigned ignored = 0u;
    if (slot >= 0) {
        previous = threads[slot].boost_disabled ? 1u : 0u;
        /* BOOLEAN is one byte, and the guest pushes a whole dword. */
        threads[slot].boost_disabled = (disable & 0xFFu) != 0u;
        ignored = ++priority_ignored;
    }
    pthread_mutex_unlock(&table_lock);
    if (slot < 0) {
        kernel_hle_log()("kernel: KeSetDisableBoostThread(%#x) -- no thread record, REFUSED\n",
                         (unsigned)object);
        return 0u;
    }
    kernel_hle_log()("kernel: KeSetDisableBoostThread(%#x, %u) RECORDED ONLY -- host "
                     "scheduling is not changed (ignored request %u)\n",
                     (unsigned)object, (unsigned)(disable & 0xFFu), ignored);
    return previous;
}

/* Ordinal 238 is stdcall with no arguments. The original wrapper at
 * 0x37FD5E compares the result with 0x40000024. This bounded policy declines
 * guest scheduler handoff; it does not inspect a ready queue or attempt a host
 * yield, and does not claim that another guest thread could not run. */
static uint32_t handle_nt_yield_execution(void *context)
{
    (void)context;
    const uint32_t irql = kernel_sync_current_irql();
    if (irql != KERNEL_IRQL_PASSIVE) {
        kernel_hle_log()("kernel: NtYieldExecution at IRQL %u REFUSED -- only the "
                         "measured PASSIVE no-handoff scope is supported\n", irql);
        return STATUS_NOT_IMPLEMENTED;
    }
    return STATUS_NO_YIELD_PERFORMED;
}

unsigned kernel_thread_active_wait_count(void)
{
    pthread_mutex_lock(&table_lock);
    const unsigned count = active_waits;
    pthread_mutex_unlock(&table_lock);
    return count;
}

static _Noreturn void refuse_wait(uint32_t handle, kernel_thread_wait_refusal reason,
                                 void (*callback)(uint32_t, kernel_thread_wait_refusal))
{
    if (callback) {
        callback(handle, reason);
    }
    (void)fprintf(stderr, "kernel: NtWaitForSingleObjectEx(%#x) refusal %u: "
                         "missing or returning nonreturning host hook\n",
                  handle, (unsigned)reason);
    abort();
}

static uint32_t wait_single_object(uint32_t args[4]);

/* T371: the calling thread's recorded block state ends with its wait, whichever way it returned. */
static uint32_t block_cleared(uint32_t status)
{
    pthread_mutex_lock(&table_lock);
    block_leave_locked();
    pthread_mutex_unlock(&table_lock);
    return status;
}

static uint32_t handle_nt_wait_for_single_object_ex(void *context)
{
    const kernel_call_frame *frame = context;
    uint32_t args[4];
    for (unsigned i = 0u; i < 4u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    return block_cleared(wait_single_object(args));
}

/* Ordinal 233 (Handle, Alertable, Timeout) is stdcall 3. Measured at 0x0037CD8F:
 * push ebx(0), push ebx(0), push [ebp+8]. INFERRED: WaitMode is UserMode (1) as in
 * the measured 234 site, so it shares the 234 scope and policy. */
static uint32_t handle_nt_wait_for_single_object(void *context)
{
    const kernel_call_frame *frame = context;
    uint32_t guest[3];
    for (unsigned i = 0u; i < 3u; i++) {
        if (!kernel_frame_arg(frame, i, &guest[i])) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    uint32_t args[4] = {guest[0], 1u, guest[1], guest[2]};
    return block_cleared(wait_single_object(args));
}

/* Ordinal 99 (WaitMode, Alertable, Interval*) is stdcall 3. Measured at 0x00380098:
 * push esi (interval), push [ebp+0xC] (alertable), push 1 (mode). The wrapper loops
 * on 0x101 only when alertable, and maps 0xC0 to its own result. Bounded scope:
 * PASSIVE, mode 1, relative (negative) interval up to DELAY_MAX_100NS, or zero.
 * No APC queue exists, so a completed delay is STATUS_SUCCESS even if alertable
 * (INFERRED). Absolute, infinite or oversized delays are refused. */
static uint32_t delay_execution_body(void *context)
{
    const kernel_call_frame *frame = context;
    uint32_t args[3];
    for (unsigned i = 0u; i < 3u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    int64_t interval = 0;
    if (args[2] == 0u || !kernel_guest_read_bytes(args[2], &interval, sizeof(interval))) {
        return STATUS_ACCESS_VIOLATION;
    }
    pthread_mutex_lock(&table_lock);
    void (*callback)(uint32_t, kernel_thread_wait_refusal) = host_ops.wait_refused;
    if (reset_in_progress || kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE ||
        (args[0] & 0xFFu) != 1u || interval > 0 ||
        (interval < 0 && (uint64_t)(-(interval + 1)) >= DELAY_MAX_100NS) ||
        active_waits == UINT_MAX) {
        pthread_mutex_unlock(&table_lock);
        refuse_wait(0u, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE, callback);
    }
    if (interval == 0) {
        pthread_mutex_unlock(&table_lock);
        return STATUS_SUCCESS;
    }
    block_enter_locked(KERNEL_THREAD_BLOCK_HOST_TIMER, 0u);
    active_waits++;
    pthread_mutex_unlock(&table_lock);
    const uint64_t hundred_ns = (uint64_t)(-(interval + 1)) + 1u;
    struct timespec deadline;
    int error = clock_gettime(CLOCK_MONOTONIC, &deadline);
    if (error == 0) {
        deadline.tv_sec += (time_t)(hundred_ns / 10000000u);
        deadline.tv_nsec += (long)((hundred_ns % 10000000u) * 100u);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec++;
        }
        do {
            error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        } while (error == EINTR);
    }
    pthread_mutex_lock(&table_lock);
    active_waits--;
    pthread_mutex_unlock(&table_lock);
    if (error != 0) {
        refuse_wait(0u, KERNEL_THREAD_WAIT_CONDITION_ERROR, callback);
    }
    return STATUS_SUCCESS;
}

static uint32_t handle_ke_delay_execution_thread(void *context)
{
    return block_cleared(delay_execution_body(context));
}

uint32_t kernel_thread_current_identity(void)
{
    return current_thread_valid ? current_thread_handle : KERNEL_THREAD_IDENTITY_BOOT;
}

/* T8g: an EVENT or MUTANT wait. Caller holds table_lock and has passed the scope gate (PASSIVE,
 * WaitMode 1, non-alertable); this returns with it released.
 *
 * MEASURED callers (lifted tree, every wait reaches the kernel through the XAPI wrapper
 * 0x00380029 whose `push 1` is WaitMode and whose alertable flag is the caller's): the title's
 * lock class Lock 0x003ABD40 waits on its one mutant (0x00771C68, built by CreateMutex(0,0,0)) with
 * a NULL timeout (infinite), the poll site 0x003BCD80 passes a zero timeout, overlapped waits
 * (GetOverlappedResult 0x0037EA14, 0x00433179) pass NULL, and the 1000 ms waiters at 0x003DF122
 * wait on a handle that is not an event or mutant. Alertable is 0 at every one of them.
 *
 * MODEL: the wait is satisfied at once when the object allows it (a signaled event, consumed by a
 * type 1 auto-reset event, or a mutant that is unowned or already owned by the calling thread
 * identity, which recurses), STATUS_TIMEOUT for a poll (a timeout pointer to a zero value) that
 * cannot be satisfied, and everything else would BLOCK. The existing blocking machinery does not
 * cover that: a THREAD wait sleeps on table_cond only because a thread's exit broadcasts it, and
 * nothing here would wake an event waiter (NtSetEvent and NtReleaseMutant take no table lock and
 * no scheduler exists to pick a runner), so a block is REFUSED through wait_refused, which stops
 * the run, rather than hanging it. A finite non-zero timeout that cannot be satisfied is refused
 * too: no measured event or mutant wait uses one. An already satisfiable wait returns at once
 * whatever the timeout. */
static uint32_t wait_sync_object(uint32_t handle, kernel_object_kind kind,
                                 uint32_t timeout_pointer, uint64_t timeout)
{
    void (*callback)(uint32_t, kernel_thread_wait_refusal) = host_ops.wait_refused;
    bool acquired = false;
    const nt_status status = kind == KERNEL_OBJECT_EVENT ?
        kernel_object_event_try_wait(handle, &acquired) :
        kernel_object_mutant_try_acquire(handle, kernel_thread_current_identity(), &acquired);
    pthread_mutex_unlock(&table_lock);
    if (acquired) {
        return STATUS_SUCCESS;
    }
    if (status != STATUS_SUCCESS) {
        return status;
    }
    /* T764: with --async-file-io the Event may be the one a PENDING read will set. A blocking wait
     * (NULL timeout) or a relative one advances the virtual clock to that read's due time, never
     * past it or the timeout (kernel_async_io.h part 1), and a poll only completes what is due. */
    if (kind == KERNEL_OBJECT_EVENT && (timeout_pointer == 0u || (int64_t)timeout <= 0)) {
        const kernel_async_wait waited = kernel_async_io_wait_event(
            handle, timeout_pointer != 0u, (uint64_t)0u - timeout);
        if (waited == KERNEL_ASYNC_WAIT_TIMED_OUT) {
            return THREAD_WAIT_TIMEOUT;
        }
        if (waited == KERNEL_ASYNC_WAIT_SATISFIED) {
            bool acquired_now = false;
            (void)kernel_object_event_try_wait(handle, &acquired_now);
            if (acquired_now) {
                return STATUS_SUCCESS;
            }
        }
    }
    if (timeout_pointer != 0u && timeout == 0u) {
        return THREAD_WAIT_TIMEOUT;
    }
    kernel_hle_log()("kernel: NtWaitForSingleObjectEx(%#x) on a %s that is not signaled/free "
                     "would BLOCK (%s timeout) -- no scheduler is modelled to wake it, "
                     "refusing\n",
                     (unsigned)handle, kind == KERNEL_OBJECT_EVENT ? "EVENT" : "MUTANT",
                     timeout_pointer == 0u ? "infinite" : "finite");
    refuse_wait(handle, KERNEL_THREAD_WAIT_WOULD_BLOCK, callback);
}

/* The current-thread pseudo handle pauses the title measures (docs/boot-frontier.md section 6,
 * T577, MEASURED from the XBE image: the only two Sleep-style callers of the XAPI wait wrapper
 * 0x00380029 are the loading bar worker loop 0x00156CB0, `push 8` (-80000), and the loader
 * thread loop 0x00030160, `push 0x10` (-160000)). Both ignore the wait's result and re-test a
 * stop flag, and neither reads a clock around the call, so the interval is only a pace.
 * Returns the host sleep in nanoseconds (the requested relative interval, 100 ns per unit),
 * or 0 for any other timeout, which stays refused. */
#define PSEUDO_PAUSE_8_MS UINT64_C(0xFFFFFFFFFFFEC780)
#define PSEUDO_PAUSE_16_MS UINT64_C(0xFFFFFFFFFFFD8F00)
static long pseudo_pause_nanoseconds(uint64_t timeout)
{
    if (timeout != PSEUDO_PAUSE_8_MS && timeout != PSEUDO_PAUSE_16_MS) {
        return 0L;
    }
    return (long)((UINT64_C(0) - timeout) * UINT64_C(100));
}

/* T764: a wait on a FILE handle, the object a read WITHOUT an Event signals when it completes
 * (kernel_async_io.h part 2). Only with --async-file-io: the file object is otherwise not a waitable
 * object here and the wait stays refused by the caller. A wait nothing pending would end blocks
 * forever on a real console, so it is refused by name (no scheduler wakes it). */
static uint32_t wait_file_object(uint32_t handle, uint32_t timeout_pointer, uint64_t timeout,
                                 void (*callback)(uint32_t, kernel_thread_wait_refusal))
{
    if (timeout_pointer != 0u && (int64_t)timeout > 0) {
        refuse_wait(handle, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE, callback);
    }
    const kernel_async_wait waited =
        kernel_async_io_wait_file(handle, timeout_pointer != 0u, (uint64_t)0u - timeout);
    if (waited == KERNEL_ASYNC_WAIT_SATISFIED) {
        return STATUS_SUCCESS;
    }
    if (waited == KERNEL_ASYNC_WAIT_TIMED_OUT || (timeout_pointer != 0u && timeout == 0u)) {
        return THREAD_WAIT_TIMEOUT;
    }
    kernel_hle_log()("kernel: NtWaitForSingleObjectEx(%#x) on a FILE whose object no pending read "
                     "will signal would BLOCK (%s timeout) -- no scheduler is modelled to wake "
                     "it, refusing\n",
                     (unsigned)handle, timeout_pointer == 0u ? "infinite" : "finite");
    refuse_wait(handle, KERNEL_THREAD_WAIT_WOULD_BLOCK, callback);
}

static uint32_t wait_single_object(uint32_t args[4])
{
    uint64_t timeout = 0u;
    if (args[3] != 0u && !kernel_guest_read_bytes(args[3], &timeout, sizeof(timeout))) {
        return STATUS_ACCESS_VIOLATION;
    }
    const uint32_t handle = args[0];
    pthread_mutex_lock(&table_lock);
    void (*callback)(uint32_t, kernel_thread_wait_refusal) = host_ops.wait_refused;
    /* Measured title pauses: current-thread pseudo handle and exactly -80000 or -160000
     * relative 100ns units (pseudo_pause_nanoseconds). This is a real host deadline of the
     * requested length, not a modeled clock advance, a successful object wait, or an
     * asynchronous delivery. */
    if (handle == 0xFFFFFFFEu) {
        const long pause_nanoseconds = pseudo_pause_nanoseconds(timeout);
        const bool identity_valid = current_thread_valid &&
            current_thread_slot < KERNEL_THREAD_MAX &&
            threads[current_thread_slot].in_use &&
            threads[current_thread_slot].handle == current_thread_handle &&
            threads[current_thread_slot].started &&
            !threads[current_thread_slot].finished;
        if (reset_in_progress || kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE ||
            args[1] != 1u || (args[2] & 0xFFu) != 0u || args[3] == 0u ||
            pause_nanoseconds == 0L || !identity_valid ||
            active_waits == UINT_MAX) {
            pthread_mutex_unlock(&table_lock);
            refuse_wait(handle, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE, callback);
        }
        block_enter_locked(KERNEL_THREAD_BLOCK_HOST_TIMER, 0u);
        active_waits++;
        pthread_mutex_unlock(&table_lock);
        struct timespec deadline;
        int error = clock_gettime(CLOCK_MONOTONIC, &deadline);
        if (error == 0) {
            deadline.tv_nsec += pause_nanoseconds;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_nsec -= 1000000000L;
                deadline.tv_sec++;
            }
            do {
                error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                        &deadline, NULL);
            } while (error == EINTR);
        }
        pthread_mutex_lock(&table_lock);
        active_waits--;
        pthread_mutex_unlock(&table_lock);
        if (error != 0)
            refuse_wait(handle, KERNEL_THREAD_WAIT_CONDITION_ERROR, callback);
        return THREAD_WAIT_TIMEOUT;
    }
    if (reset_in_progress || kernel_sync_current_irql() != KERNEL_IRQL_PASSIVE ||
        args[1] != 1u || (args[2] & 0xFFu) != 0u) {
        pthread_mutex_unlock(&table_lock);
        refuse_wait(handle, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE, callback);
    }
    kernel_object_entry object;
    const bool live = kernel_object_get_copy(handle, &object);
    if (live && (object.kind == KERNEL_OBJECT_EVENT || object.kind == KERNEL_OBJECT_MUTANT)) {
        return wait_sync_object(handle, object.kind, args[3], timeout);
    }
    if (live && object.kind == KERNEL_OBJECT_FILE && kernel_async_io_enabled()) {
        pthread_mutex_unlock(&table_lock);
        return wait_file_object(handle, args[3], timeout, callback);
    }
    /* THREAD waits accept only a NULL or an explicit zero timeout (judged before the handle,
     * as before T8g). */
    if (timeout != 0u) {
        pthread_mutex_unlock(&table_lock);
        refuse_wait(handle, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE, callback);
    }
    if (!live) {
        pthread_mutex_unlock(&table_lock);
        return STATUS_INVALID_HANDLE;
    }
    if (object.kind != KERNEL_OBJECT_THREAD) {
        pthread_mutex_unlock(&table_lock);
        refuse_wait(handle, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE, callback);
    }
    const int slot = find_slot_locked(handle);
    if (slot < 0 || object.owner_tag != (unsigned)slot) {
        pthread_mutex_unlock(&table_lock);
        return STATUS_INVALID_HANDLE;
    }
    kernel_thread_record *record = &threads[slot];
    if (!record->started || record->suspend_count != 0u ||
        (current_thread_valid && current_thread_handle == handle) ||
        active_waits == UINT_MAX) {
        pthread_mutex_unlock(&table_lock);
        refuse_wait(handle, KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE, callback);
    }
    active_waits++;
    for (;;) {
        if (args[3] == 0u) { /* a zero timeout poll never blocks, set again after a spurious wake */
            block_enter_locked(KERNEL_THREAD_BLOCK_THREAD, handle);
        }
        if (record->terminated) {
            active_waits--;
            pthread_mutex_unlock(&table_lock);
            return STATUS_SUCCESS;
        }
        if (record->finished) {
            active_waits--;
            pthread_mutex_unlock(&table_lock);
            refuse_wait(handle, KERNEL_THREAD_WAIT_TERMINAL_HOST_FAILURE, callback);
        }
        if (args[3] != 0u) {
            active_waits--;
            pthread_mutex_unlock(&table_lock);
            return THREAD_WAIT_TIMEOUT;
        }
        if (pthread_cond_wait(&table_cond, &table_lock) != 0) {
            active_waits--;
            pthread_mutex_unlock(&table_lock);
            refuse_wait(handle, KERNEL_THREAD_WAIT_CONDITION_ERROR, callback);
        }
    }
}

unsigned kernel_thread_register(void)
{
    static const struct {
        unsigned ordinal;
        kernel_fn handler;
    } bindings[] = {
        {ORD_PsCreateSystemThreadEx, handle_ps_create_system_thread_ex},
        {ORD_PsTerminateSystemThread, handle_ps_terminate_system_thread},
        {ORD_KeSetBasePriorityThread, handle_ke_set_base_priority_thread},
        {ORD_KeSetDisableBoostThread, handle_ke_set_disable_boost_thread},
        {ORD_NtResumeThread, handle_nt_resume_thread},
        {ORD_NtSuspendThread, handle_nt_suspend_thread},
        {ORD_NtYieldExecution, handle_nt_yield_execution},
        {ORD_NtWaitForSingleObjectEx, handle_nt_wait_for_single_object_ex},
        {ORD_NtWaitForSingleObject, handle_nt_wait_for_single_object},
        {ORD_KeDelayExecutionThread, handle_ke_delay_execution_thread},
    };

    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
