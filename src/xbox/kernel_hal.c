/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_hal.h for the resolved ordinal number, the hand-verified arity across
 * all 12 call sites, and why no guest struct layout is derived here.
 */

#include "kernel_hal.h"

#include <pthread.h>

#include "kernel_call.h"
#include "kernel_hle.h"

#define ORD_HalRegisterShutdownNotification 47u
#define ORD_HalReturnToFirmware 49u
#define ORD_PhyGetLinkState 252u
#define ORD_PhyInitialize 253u

/* STATUS_NO_SUCH_DEVICE. The guest only sign-tests a PhyInitialize result (0x43AF83, `jl`),
 * so any failure status takes the same path, and this is the NT status that says what is
 * true here: there is no PHY. */
#define STATUS_NO_PHY_DEVICE 0xC000000Eu

/*
 * How many registrations can be held at once.
 *
 * 12 call sites cannot produce an unbounded number of live registrations, and the
 * two register-indirect sites are a register/deregister pair of a single block, so
 * 64 is slack of several times over. A fixed array rather than a growing one for
 * the reason kernel_pool.c gives: a bound that is reached loudly beats one reached
 * silently, so an overflow is REFUSED and counted rather than dropped.
 */
#define KERNEL_HAL_SHUTDOWN_MAX 64u

/*
 * THE LOCK. Two guest threads run, and the ordinal is reached from thread 2's boot
 * path with thread 1 still live. Claiming a slot is a read-modify-write: without
 * the lock two threads can claim the same slot for two different blocks, and the
 * loser's registration then has no record -- so its later, legitimate deregister is
 * counted as unmatched and our own diagnostic blames the guest for our bug. That is
 * the exact failure mode kernel_object.c documents.
 *
 * RECURSIVE, following kernel_object.c, kernel_pool.c and guest_mem.c: the critical
 * section calls kernel_hle_log(), whose sink is caller-supplied and could re-enter.
 * A non-recursive mutex would turn a chatty log sink into a deadlock.
 */
static pthread_mutex_t shutdown_lock;
static bool shutdown_lock_ready;
static pthread_once_t shutdown_lock_once = PTHREAD_ONCE_INIT;

static void shutdown_lock_init(void)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return;
    }
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
        pthread_mutex_init(&shutdown_lock, &attr) == 0) {
        shutdown_lock_ready = true;
    }
    (void)pthread_mutexattr_destroy(&attr);
}

static void lock(void)
{
    (void)pthread_once(&shutdown_lock_once, shutdown_lock_init);
    if (shutdown_lock_ready) {
        (void)pthread_mutex_lock(&shutdown_lock);
    }
}

static void unlock(void)
{
    if (shutdown_lock_ready) {
        (void)pthread_mutex_unlock(&shutdown_lock);
    }
}

/* Guest pointers only. A zero slot is free, which is sound because 0 is not a
 * usable guest address -- kernel_guest_at() rejects it -- so it cannot also be a
 * legitimate registration. */
static kernel_guest_ptr registrations[KERNEL_HAL_SHUTDOWN_MAX];
static unsigned duplicate_count;
static unsigned unmatched_count;
static unsigned overflow_count;

/* Ordinal 49. The sink is host wiring and is NOT thread-local: a reboot is a property
 * of the console, not of the thread that asked for it, so whichever guest thread calls
 * must reach the same sink. */
static void (*firmware_sink)(uint32_t routine, unsigned pending_notifications);
static unsigned firmware_return_count;
static unsigned phy_initialize_count;
static unsigned phy_link_query_count;
static uint32_t last_firmware_routine;

/* Callers hold the lock. */
static unsigned find_locked(kernel_guest_ptr registration)
{
    for (unsigned i = 0u; i < KERNEL_HAL_SHUTDOWN_MAX; i++) {
        if (registrations[i] == registration) {
            return i;
        }
    }
    return KERNEL_HAL_SHUTDOWN_MAX;
}

void kernel_hal_reset(void)
{
    lock();
    for (unsigned i = 0u; i < KERNEL_HAL_SHUTDOWN_MAX; i++) {
        registrations[i] = 0u;
    }
    duplicate_count = 0u;
    unmatched_count = 0u;
    overflow_count = 0u;
    /* The firmware counters reset too, so one test case cannot leak a reboot into the
     * next and make a later assertion pass for the wrong reason. The SINK is
     * deliberately NOT cleared: it is host wiring, installed once, and a reset that
     * silently detached it would make the next reboot request fail to stop the run. */
    firmware_return_count = 0u;
    last_firmware_routine = 0u;
    phy_initialize_count = 0u;
    phy_link_query_count = 0u;
    unlock();
}

unsigned kernel_hal_shutdown_count(void)
{
    unsigned held = 0u;
    lock();
    for (unsigned i = 0u; i < KERNEL_HAL_SHUTDOWN_MAX; i++) {
        if (registrations[i] != 0u) {
            held++;
        }
    }
    unlock();
    return held;
}

bool kernel_hal_shutdown_registered(kernel_guest_ptr registration)
{
    if (registration == 0u) {
        return false;
    }
    lock();
    const bool found = find_locked(registration) != KERNEL_HAL_SHUTDOWN_MAX;
    unlock();
    return found;
}

unsigned kernel_hal_shutdown_duplicate_count(void)
{
    lock();
    const unsigned count = duplicate_count;
    unlock();
    return count;
}

unsigned kernel_hal_shutdown_unmatched_count(void)
{
    lock();
    const unsigned count = unmatched_count;
    unlock();
    return count;
}

unsigned kernel_hal_shutdown_overflow_count(void)
{
    lock();
    const unsigned count = overflow_count;
    unlock();
    return count;
}

void kernel_hal_set_firmware_sink(void (*sink)(uint32_t routine,
                                               unsigned pending_notifications))
{
    lock();
    firmware_sink = sink;
    unlock();
}

unsigned kernel_hal_firmware_return_count(void)
{
    lock();
    const unsigned count = firmware_return_count;
    unlock();
    return count;
}

uint32_t kernel_hal_last_firmware_routine(void)
{
    lock();
    const uint32_t routine = last_firmware_routine;
    unlock();
    return routine;
}

unsigned kernel_hal_shutdown_capacity(void)
{
    return KERNEL_HAL_SHUTDOWN_MAX;
}

static void add_registration(kernel_guest_ptr registration)
{
    lock();
    if (find_locked(registration) != KERNEL_HAL_SHUTDOWN_MAX) {
        /* Already held. The real kernel would thread the same LIST_ENTRY twice and
         * corrupt its list; we keep one entry and say so. */
        duplicate_count++;
        kernel_hle_log()("kernel: HalRegisterShutdownNotification(0x%08X) is already "
                         "registered -- kept once, counted\n",
                         (unsigned)registration);
        unlock();
        return;
    }
    const unsigned slot = find_locked(0u);
    if (slot == KERNEL_HAL_SHUTDOWN_MAX) {
        overflow_count++;
        kernel_hle_log()("kernel: HalRegisterShutdownNotification(0x%08X) REFUSED -- "
                         "all %u registration slots are in use\n",
                         (unsigned)registration, KERNEL_HAL_SHUTDOWN_MAX);
        unlock();
        return;
    }
    registrations[slot] = registration;
    unlock();
}

static void remove_registration(kernel_guest_ptr registration)
{
    lock();
    const unsigned slot = find_locked(registration);
    if (slot == KERNEL_HAL_SHUTDOWN_MAX) {
        /* Not held. Counted rather than ignored: with argument 0 and argument 1
         * swapped, EVERY call would land here, so a nonzero count is the signature
         * of this module reading its own arguments backwards. */
        unmatched_count++;
        kernel_hle_log()("kernel: HalRegisterShutdownNotification(0x%08X, FALSE) "
                         "deregisters something that was never registered\n",
                         (unsigned)registration);
        unlock();
        return;
    }
    registrations[slot] = 0u;
    unlock();
}

/*
 * ARITY-OK(47): 2 stack arguments, counted by hand at all 12 call sites in this
 * image rather than taken from the measured table, which reports 2 over 10 sites
 * and flags it NON-UNANIMOUS (so stack_args_for() would refuse it). Every one of
 * the 12 pushes exactly two arguments before the return address, and six of them
 * push a 0 or 1 LITERAL as the second argument, which fixes both the count and
 * which argument is the BOOLEAN. The two sites the measured table misses call
 * through a register holding the thunk slot. Full site-by-site table in
 * kernel_hal.h.
 *
 * Returns void on the real kernel, so the 0 below is never read.
 */
static uint32_t hle_hal_register_shutdown_notification(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: HalRegisterShutdownNotification called with no "
                         "argument frame -- the call boundary did not supply one\n");
        return 0u;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t registration = 0u;
    uint32_t register_flag = 0u;
    if (!kernel_frame_arg(frame, 0u, &registration) ||
        !kernel_frame_arg(frame, 1u, &register_flag)) {
        kernel_hle_log()("kernel: HalRegisterShutdownNotification could not read its "
                         "arguments from the guest stack\n");
        return 0u;
    }
    if (registration == 0u) {
        /* The real kernel would fault dereferencing it. Reported instead, because a
         * null here means our argument order is wrong far more likely than it means
         * the guest passed null -- every measured site passes a .data address or a
         * live register. */
        kernel_hle_log()("kernel: HalRegisterShutdownNotification(NULL, %u) -- a null "
                         "registration block is not a thing the guest does\n",
                         (unsigned)register_flag);
        return 0u;
    }

    /* BOOLEAN is a byte. The guest pushes a full slot, and every measured site
     * pushes either a 0/1 literal or a register, so anything nonzero in the low byte
     * is TRUE. Testing the whole slot would differ only for a value whose low byte is
     * zero and whose upper bytes are not, which a BOOLEAN cannot be. */
    if ((register_flag & 0xFFu) != 0u) {
        add_registration(registration);
    } else {
        remove_registration(registration);
    }
    return 0u;
}

/*
 * ARITY: 1 stack argument, from `{49u, 1u, 4u, 1}` -- four sites, unanimous, which
 * clears the three-site corroboration bar, so this needs no hand-written ABI row. The
 * evidence that 49 is HalReturnToFirmware rather than HalRequestSoftwareInterrupt, and
 * that it does not return, is in kernel_hal.h.
 *
 * It never returns on hardware, so the 0 below is reached only when no sink is
 * installed -- and that case says so.
 */
static uint32_t hle_hal_return_to_firmware(void *context)
{
    uint32_t routine = 0u;
    if (!context) {
        /* No frame means we cannot read WHICH firmware routine was asked for. Stopping
         * anyway would be right about the reboot and silent about the reason, so the
         * missing frame is named first. */
        kernel_hle_log()("kernel: HalReturnToFirmware called with no argument frame -- "
                         "the call boundary did not supply one, so the routine value "
                         "is unknown\n");
    } else if (!kernel_frame_arg((const kernel_call_frame *)context, 0u, &routine)) {
        kernel_hle_log()("kernel: HalReturnToFirmware could not read its routine "
                         "argument from the guest stack\n");
    }

    lock();
    firmware_return_count++;
    last_firmware_routine = routine;
    const unsigned attempt = firmware_return_count;
    unlock();

    /* Counted, not called. The real kernel walks this set and invokes each routine,
     * which needs the block layout kernel_hal.h declines to guess. Reporting the count
     * is the part that is true. */
    const unsigned pending = kernel_hal_shutdown_count();

    kernel_hle_log()("kernel: HalReturnToFirmware(%u) -- THE TITLE ASKED TO REBOOT. "
                     "%u shutdown registration(s) would have been notified first "
                     "(none were: no routine is called, see kernel_hal.h)\n",
                     (unsigned)routine, pending);

    void (*sink)(uint32_t, unsigned) = firmware_sink;
    if (sink) {
        sink(routine, pending);
        /* Reached only if the sink came back, which on hardware is impossible. Every
         * observation after this point is from a console that already rebooted. */
        kernel_hle_log()("kernel: the firmware sink RETURNED from "
                         "HalReturnToFirmware(%u). Nothing after this point is a "
                         "faithful trace\n", (unsigned)routine);
    } else {
        kernel_hle_log()("kernel: no firmware sink is installed, so the run CONTINUES "
                         "past a reboot it cannot honestly survive. Correct only for "
                         "unit tests; the host must install one\n");
    }
    if (attempt > 1u) {
        kernel_hle_log()("kernel: this is firmware return %u -- the first one did not "
                         "stop anything\n", attempt);
    }
    return 0u;
}

/* ---------------------------------------------------------------------------
 * PhyInitialize (253) and PhyGetLinkState (252): the Ethernet PHY.
 *
 * THERE IS NO PHY. The answers below are the title's own "no network" states, taken from how
 * the guest uses the two results, and each is announced on its first call.
 *
 * MEASURED, from the guest's call sites (saved startup worktree notes):
 *   PhyInitialize(0, 0)    two sites. The wrapper at 0x3848B2 ignores the result. The NIC
 *                          bring-up at 0x43AF83 does `cmp eax, edi(0); jl` and returns the
 *                          status to its caller on a negative one, which is the title's own
 *                          failure path. A failure status is therefore the true answer
 *                          (no PHY) and the one the title handles.
 *   PhyGetLinkState(mode)  three sites. The wrapper computes `(link & 1) ? link : 0`, and its
 *                          seven callers treat bit 0 clear as "no cable": the XOnline logon
 *                          gives up with an error status (0x419082), the others take a
 *                          different message arm or return false. None waits or loops on it.
 *                          Returning 0 is the unplugged-cable state every title must handle.
 *
 * Neither argument is named: nothing here depends on them and a wrong name is worse than none.
 * They are logged raw so a caller that starts passing something else is visible.
 * ------------------------------------------------------------------------- */

static uint32_t hle_phy_initialize(void *context)
{
    uint32_t args[2] = {0u, 0u};
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    if (frame == NULL || !kernel_frame_arg(frame, 0u, &args[0]) ||
        !kernel_frame_arg(frame, 1u, &args[1])) {
        kernel_hle_log()("kernel: PhyInitialize could not read its two stack arguments -- "
                         "REFUSED with STATUS_INVALID_PARAMETER\n");
        return 0xC000000Du;
    }
    lock();
    const unsigned attempt = ++phy_initialize_count;
    unlock();
    if (attempt == 1u) {
        kernel_hle_log()("kernel: PhyInitialize(%#x, %#x) -- there is NO Ethernet PHY in this "
                         "host. Answering STATUS_NO_SUCH_DEVICE (0x%08X), the title's own "
                         "network-failure path (only the sign is tested at 0x43AF83)\n",
                         (unsigned)args[0], (unsigned)args[1], (unsigned)STATUS_NO_PHY_DEVICE);
    }
    return STATUS_NO_PHY_DEVICE;
}

static uint32_t hle_phy_get_link_state(void *context)
{
    uint32_t mode = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, 0u, &mode)) {
        kernel_hle_log()("kernel: PhyGetLinkState could not read its stack argument -- "
                         "answering 0 (no link) and reporting the refusal\n");
        return 0u;
    }
    lock();
    const unsigned attempt = ++phy_link_query_count;
    unlock();
    if (attempt == 1u) {
        kernel_hle_log()("kernel: PhyGetLinkState(%#x) -- FABRICATED: no link, no speed, no "
                         "duplex (0). The title treats bit 0 clear as an unplugged cable and "
                         "gives up on networking\n",
                         (unsigned)mode);
    }
    return 0u;
}

unsigned kernel_hal_phy_initialize_count(void)
{
    lock();
    const unsigned count = phy_initialize_count;
    unlock();
    return count;
}

unsigned kernel_hal_phy_link_query_count(void)
{
    lock();
    const unsigned count = phy_link_query_count;
    unlock();
    return count;
}

unsigned kernel_hal_register(void)
{
    static const struct {
        unsigned ordinal;
        kernel_fn handler;
    } bindings[] = {
        {ORD_HalRegisterShutdownNotification, hle_hal_register_shutdown_notification},
        {ORD_HalReturnToFirmware, hle_hal_return_to_firmware},
        {ORD_PhyGetLinkState, hle_phy_get_link_state},
        {ORD_PhyInitialize, hle_phy_initialize},
    };

    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
