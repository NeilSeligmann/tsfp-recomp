/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xnet_dpc.h"
#include "recomp_callback.h"
#include "guest_mem.h"
#include "kernel_event.h"
#include "host_runtime.h"
#include <stdint.h>
#include <string.h>

#define XNET_DPC_STACK_BYTES 65536u
static bool enabled;
static uint32_t stack;
static uint64_t stack_generation;
static bool captured;
static host_stop stopped;
static bool dispatch(uint32_t routine, uint32_t dpc, uint32_t context,
                     uint32_t argument1, uint32_t argument2, void *opaque)
{
    (void)opaque;
    if (!enabled || captured || routine != 0x43A184u) return false;
    if (!stack) {
        const guest_region_request request = {.bytes = XNET_DPC_STACK_BYTES,
            .alignment = 4096u, .protect = 4u, .state = 0x1000u};
        nt_status status;
        stack = guest_region_alloc(&request, &status);
        if (!stack) return false;
        stack_generation = guest_allocation_generation(stack);
    }
    if (!stack_generation || guest_allocation_generation(stack) != stack_generation) return false;
    const uint32_t arguments[4] = {dpc, context, argument1, argument2};
    return recomp_callback_run_xnet_dpc(routine, stack, stack + XNET_DPC_STACK_BYTES,
                                       arguments, &stopped, &captured);
}
bool host_xnet_dpc_configure(bool selected)
{
    if (enabled && selected) return true;
    if (!enabled && stack && selected) return false;
    if (enabled && !host_xnet_dpc_shutdown()) return false;
    enabled = selected;
    captured = false;
    memset(&stopped, 0, sizeof(stopped));
    kernel_event_set_dispatch(selected ? dispatch : NULL, NULL);
    return true;
}
void host_xnet_dpc_service_hook(void)
{
    if (!enabled || !host_run_armed()) return;
    unsigned delivered;
    (void)kernel_event_service(&delivered);
    /* Scheduler has restored IRQL, pending state and both locks before rethrow. */
    if (captured) {
        const host_stop result = stopped;
        captured = false;
        host_run_rethrow(&result);
    }
}
bool host_xnet_dpc_shutdown(void)
{
    kernel_event_set_dispatch(NULL, NULL);
    enabled = false;
    captured = false;
    if (stack && (!stack_generation || guest_allocation_generation(stack) != stack_generation ||
                  !guest_region_free(stack))) return false;
    stack = 0u;
    stack_generation = 0u;
    return true;
}
