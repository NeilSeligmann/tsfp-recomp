/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Prcb debug-monitor notify. See monitor_thunk.h for the measured arity, the
 * stack-balance proof behind it, and why this is a library module rather than a
 * special case inside kernel_thunk.c.
 */

#include "monitor_thunk.h"

#include <pthread.h>
#include <stdio.h>

#include "kernel_call.h"
#include "kernel_thread.h"
#include "thunk_trace.h"

static uint64_t g_notifications;
static uint64_t g_unreadable;
static bool g_have_last;
static uint32_t g_last_code;
static uint32_t g_last_pointer;

/* Counters are shared across guest threads. Every critical section here is a few
 * plain stores with no calls in it, for the reason thunk_trace.c documents: a lock
 * held across anything that can `host_run_stop` is a lock held across a `siglongjmp`,
 * and stopping is the expected end of a bring-up run. */
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;

static monitor_log_fn g_log;

static int default_log(const char *format, ...);

static int default_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vfprintf(stderr, format, args);
    va_end(args);
    return written;
}

void monitor_thunk_set_log(monitor_log_fn printer)
{
    g_log = printer;
}

static monitor_log_fn log_printer(void)
{
    return g_log ? g_log : default_log;
}

bool monitor_thunk_arm(void)
{
    kernel_thread_set_monitor_callback(MONITOR_THUNK_NOTIFY_VA);
    /* Read back rather than assumed. The setter lives in another translation unit and
     * another library, so "we called it" and "it took" are different statements. */
    return kernel_thread_monitor_callback() == MONITOR_THUNK_NOTIFY_VA;
}

bool monitor_thunk_is_armed(void)
{
    return kernel_thread_monitor_callback() == MONITOR_THUNK_NOTIFY_VA;
}

uint64_t monitor_thunk_notify_count(void)
{
    pthread_mutex_lock(&count_lock);
    const uint64_t snapshot = g_notifications;
    pthread_mutex_unlock(&count_lock);
    return snapshot;
}

uint64_t monitor_thunk_unreadable_count(void)
{
    pthread_mutex_lock(&count_lock);
    const uint64_t snapshot = g_unreadable;
    pthread_mutex_unlock(&count_lock);
    return snapshot;
}

bool monitor_thunk_last_notification(uint32_t *code, uint32_t *pointer)
{
    pthread_mutex_lock(&count_lock);
    const bool have = g_have_last;
    const uint32_t last_code = g_last_code;
    const uint32_t last_pointer = g_last_pointer;
    pthread_mutex_unlock(&count_lock);
    if (have) {
        if (code) {
            *code = last_code;
        }
        if (pointer) {
            *pointer = last_pointer;
        }
    }
    return have;
}

void monitor_thunk_reset_counts(void)
{
    pthread_mutex_lock(&count_lock);
    g_notifications = 0u;
    g_unreadable = 0u;
    g_have_last = false;
    g_last_code = 0u;
    g_last_pointer = 0u;
    pthread_mutex_unlock(&count_lock);
}

/*
 * The notification itself.
 *
 * A NO-OP THAT RETURNS, and that is the whole design. Nothing in the image ever
 * WRITES to the monitor block at any offset, so the guest has no way to observe what
 * a real debug monitor would have done with `code` and `ptr`; the only thing it can
 * observe is that the call returned and `esp` is where `ret 8` would have left it.
 * Doing anything else would be inventing devkit behaviour.
 *
 * `eax` IS SET TO ZERO, on the record. No site tests the return value: every one of
 * the 8 either falls through or `jmp`s, and the two that follow with a `test eax,eax`
 * (`sub_0037FDE1` at 0x0037FDFC) are testing the value loaded BEFORE the call. Zero
 * is therefore unobservable here, and it is chosen anyway because a stale `eax` left
 * over from the dispatcher would be a value that varies with our own internals.
 */
static void monitor_dispatch(void)
{
    (void)thunk_trace_thread_id();

    uint32_t return_address = 0u;
    (void)kernel_guest_read_u32((kernel_guest_ptr)g_esp, &return_address);

    kernel_call_frame frame = {
        .stack_ptr = (kernel_guest_ptr)g_esp,
        .stack_limit = 0u,
        .ecx = 0u,
        .edx = 0u,
        .has_registers = false,
    };
    uint32_t code = 0u;
    uint32_t pointer = 0u;
    const bool read_code = kernel_frame_arg(&frame, 0u, &code);
    const bool read_pointer = kernel_frame_arg(&frame, 1u, &pointer);

    pthread_mutex_lock(&count_lock);
    g_notifications++;
    if (!read_code || !read_pointer) {
        g_unreadable++;
    } else {
        g_have_last = true;
        g_last_code = code;
        g_last_pointer = pointer;
    }
    pthread_mutex_unlock(&count_lock);

    if (read_code && read_pointer) {
        /* REPORTED, NOT SILENT, and every time rather than once. This is a
         * divergence from a retail console -- which has no monitor block and never
         * reaches this call at all -- so each occurrence is a fact about what the
         * guest believes happened. A once-only announcement would hide the second
         * code the guest sent, and the code is the only thing distinguishing one
         * notification from another. */
        log_printer()("monitor: debug-monitor notify code %u (%#x), ptr 0x%08X -- "
                      "ANSWERED AS A NO-OP from 0x%08X. A retail console has no "
                      "monitor block and never makes this call\n",
                      (unsigned)code, (unsigned)code, pointer, return_address);
    } else {
        log_printer()("monitor: debug-monitor notify from 0x%08X with an UNREADABLE "
                      "frame at esp 0x%08X -- answered anyway, because the guest has "
                      "already pushed and something must pop\n",
                      return_address, g_esp);
    }

    (void)thunk_trace_append(THUNK_KIND_MONITOR, 0u, MONITOR_THUNK_NOTIFY_VA,
                             return_address, 0u, true);

    g_eax = 0u;
    /* Callee cleanup: the return address the lifted caller pushed, plus the two
     * stack arguments a real `ret 8` would have taken with it. The count is a named
     * constant and not a literal 2 precisely because this line is the one that
     * cannot be wrong -- see monitor_thunk.h. */
    g_esp += 4u + 4u * MONITOR_THUNK_NOTIFY_STACK_ARGS;
}

recomp_func_t monitor_thunk_lookup(uint32_t va)
{
    if (va != MONITOR_THUNK_NOTIFY_VA) {
        return NULL;
    }
    return monitor_dispatch;
}

void monitor_thunk_dispatch_at(void)
{
    monitor_dispatch();
}

void monitor_thunk_report(void)
{
    monitor_log_fn out = log_printer();

    out("\n--- the Prcb debug-monitor notify ---\n");
    out("slot           %u at 0x%08X (%u stack argument(s), __stdcall)\n",
        (unsigned)MONITOR_THUNK_NOTIFY_SLOT, (unsigned)MONITOR_THUNK_NOTIFY_VA,
        (unsigned)MONITOR_THUNK_NOTIFY_STACK_ARGS);
    if (!monitor_thunk_is_armed()) {
        out("NOT ARMED, so `monitor+0x14` is still the zero the block is filled with\n"
            "and the guest's next notification stops the run at a NULL indirect call.\n");
        return;
    }
    out("notifications  %llu answered as a no-op, %llu on an unreadable frame\n",
        (unsigned long long)monitor_thunk_notify_count(),
        (unsigned long long)monitor_thunk_unreadable_count());
    uint32_t code = 0u;
    uint32_t pointer = 0u;
    if (monitor_thunk_last_notification(&code, &pointer)) {
        out("last           code %u (%#x), ptr 0x%08X\n", (unsigned)code, (unsigned)code,
            pointer);
    }
    if (monitor_thunk_notify_count() == 0u) {
        out("ARMED AND NEVER CALLED. That is what a run that stays off every\n"
            "storage-failure arm looks like, not evidence the no-op is unnecessary.\n");
        return;
    }
    /* Printed whenever the count is nonzero, because the number above it is easy to
     * read as progress. It is not: it is a count of lies the guest accepted. */
    out("EVERY ONE OF THESE IS A DELIBERATE DIVERGENCE. A retail console has no\n"
        "debug-monitor block, so `Prcb+0x250` is zero there and none of these calls\n"
        "happens at all. This host answers non-null because a zero routes the guest\n"
        "into the GDT path at 0x00381D63, which needs `sgdt` and a real kernel image.\n"
        "See src/xbox/kernel_thread.h for the trade and src/host/monitor_thunk.h for\n"
        "why the no-op is the only honest body for it.\n");
}
