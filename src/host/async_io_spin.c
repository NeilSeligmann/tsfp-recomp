/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See async_io_spin.h.
 */

#include "async_io_spin.h"

#include "kernel_async_io.h"
#include "thunk_trace.h"

typedef struct {
    uint64_t dispatches; /* this thread's dispatch count when the run of safepoints started */
    unsigned safepoints;
} spin_state;

static volatile unsigned configured_threshold;
static _Thread_local spin_state spin;

void async_io_spin_configure(unsigned threshold)
{
    configured_threshold = threshold;
    spin = (spin_state){0};
}

unsigned async_io_spin_threshold(void)
{
    return configured_threshold;
}

void async_io_spin_note(void)
{
    const unsigned threshold = configured_threshold;
    if (threshold == 0u) {
        return;
    }
    const uint64_t dispatches = thunk_trace_thread_dispatches();
    if (dispatches != spin.dispatches) {
        spin.dispatches = dispatches;
        spin.safepoints = 0u;
        return;
    }
    if (kernel_async_io_pending() == 0u) {
        spin.safepoints = 0u;
        return;
    }
    if (++spin.safepoints < threshold) {
        return;
    }
    spin.safepoints = 0u;
    (void)kernel_async_io_complete_next();
}
