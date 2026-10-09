/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xnet_collector_kernel.h"
#include "kernel_call.h"
#include "kernel_config.h"
#include "kernel_hle.h"

static bool invoke(void *context, unsigned ordinal, const uint32_t *arguments,
                   size_t count, uint64_t *result)
{
    const xnet_collector_kernel *kernel = context;
    if (!result || count > 5u || (count && !arguments)) return false;
    const kernel_entry *entry = kernel_hle_entry(ordinal);
    if (!entry || entry->state != KERNEL_ENTRY_IMPLEMENTED || !entry->handler) return false;
    /* An absent EEPROM source is a host admission refusal, not an invented
     * guest NTSTATUS for the real FFFF export. */
    if (ordinal == 24u && (count != 5u || arguments[0] != 0xffffu ||
        arguments[3] != 256u || !kernel_config_eeprom_available())) return false;
    kernel_call_frame frame;
    if (!kernel_frame_build(&frame, kernel->call_stack, kernel->stack_bytes,
                            arguments, (unsigned)count)) return false;
    const uint32_t low = ordinal == 24u ? kernel_config_query_stored(&frame) :
                                         kernel_hle_call(ordinal, &frame);
    *result = low;
    if (frame.has_result_high) *result |= (uint64_t)frame.result_high << 32;
    return true;
}
static bool volume(void *context, uint32_t path, const uint32_t output[3], uint32_t *result)
{
    const xnet_collector_kernel *kernel = context;
    return kernel->volume_space(kernel->volume_context, path, output, result);
}
static bool disk_ready(void *context)
{
    const xnet_collector_kernel *kernel = context;
    return kernel->disk_identity_ready(kernel->disk_context);
}
bool xnet_collector_kernel_source(xnet_collector_kernel *kernel, xnet_collector_source *source)
{
    if (!kernel || !source || !kernel->call_stack || kernel->stack_bytes < 24u ||
        !kernel->volume_space || !kernel->disk_identity_ready ||
        (uint64_t)kernel->call_stack + kernel->stack_bytes > UINT32_MAX) return false;
    *source = (xnet_collector_source){invoke, volume, kernel, disk_ready};
    return true;
}
