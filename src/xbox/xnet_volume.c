/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xnet_volume.h"
#include "kernel_call.h"
#include "kernel_file.h"
#include "kernel_hle.h"
#include "kernel_io.h"

static bool span(uint32_t first, uint32_t bytes, uint32_t second, uint32_t count)
{
    return (uint64_t)first + bytes <= UINT32_MAX &&
           (uint64_t)second + count <= UINT32_MAX &&
           ((uint64_t)first + bytes <= second || (uint64_t)second + count <= first);
}
static bool implemented(unsigned ordinal)
{
    const kernel_entry *entry = kernel_hle_entry(ordinal);
    return entry && entry->state == KERNEL_ENTRY_IMPLEMENTED && entry->handler;
}
static bool frame(xnet_volume_kernel *source, kernel_call_frame *call,
                  const uint32_t *args, unsigned count)
{
    return kernel_frame_build(call, source->call_stack, source->stack_bytes, args, count);
}
static bool store64(uint32_t output, uint64_t value)
{
    return !output || (kernel_guest_write_u32(output, (uint32_t)value) &&
                       kernel_guest_write_u32(output + 4u, (uint32_t)(value >> 32)));
}
bool xnet_volume_space_kernel(void *context, uint32_t path,
                             const uint32_t output[3], uint32_t *result)
{
    xnet_volume_kernel *source = context;
    if (!source || !path || !output || !result || !source->set_nt_error ||
        !source->scratch || !source->call_stack || source->stack_bytes < 28u ||
        !span(source->scratch, 64u, source->call_stack, source->stack_bytes) ||
        !implemented(202u) || !implemented(218u) || !implemented(187u)) return false;
    uint32_t length = 0u;
    uint8_t byte;
    do {
        if (length >= KERNEL_FILE_PATH_MAX || (uint64_t)path + length > UINT32_MAX ||
            !kernel_guest_read_u8(path + length, &byte)) return false;
        if (byte) ++length;
    } while (byte);
    if (!span(source->scratch, 64u, path, length + 1u) ||
        !span(source->call_stack, source->stack_bytes, path, length + 1u)) return false;
    for (unsigned i = 0u; i < 3u; ++i) {
        if (output[i] && (!span(source->scratch, 64u, output[i], 8u) ||
                          !span(source->call_stack, source->stack_bytes, output[i], 8u)))
            return false;
    }
    const uint32_t name = source->scratch;
    const uint32_t attributes = name + 8u, handle_output = name + 20u;
    const uint32_t iosb = name + 24u, information = name + 32u;
    if (!kernel_guest_write_u32(name, length | ((length + 1u) << 16)) ||
        !kernel_guest_write_u32(name + 4u, path) ||
        !kernel_guest_write_u32(attributes, 0xFFFFFFFDu) ||
        !kernel_guest_write_u32(attributes + 4u, name) ||
        !kernel_guest_write_u32(attributes + 8u, 0x40u)) return false;
    const uint32_t open_args[6] = {handle_output, 0x100001u, attributes, iosb, 3u, 0x800021u};
    kernel_call_frame call;
    if (!frame(source, &call, open_args, 6u)) return false;
    uint32_t status = kernel_file_open_stored(&call);
    if (status >= 0x80000000u) {
        if (!source->set_nt_error(source->error_context, status, true)) return false;
        *result = 0u;
        return true;
    }
    uint32_t handle;
    if (!kernel_guest_read_u32(handle_output, &handle)) return false;
    const uint32_t query_args[5] = {handle, iosb, information, 24u, 3u};
    if (!frame(source, &call, query_args, 5u)) return false;
    status = kernel_io_query_volume_stored(&call);
    const uint32_t close_args[1] = {handle};
    if (!frame(source, &call, close_args, 1u)) return false;
    (void)kernel_hle_call(187u, &call); /* Original ignores NtClose result. */
    if (status >= 0x80000000u) {
        if (!source->set_nt_error(source->error_context, status, false)) return false;
        *result = 0u;
        return true;
    }
    uint32_t total_low, total_high, free_low, free_high, sectors, bytes;
    if (!kernel_guest_read_u32(information, &total_low) ||
        !kernel_guest_read_u32(information + 4u, &total_high) ||
        !kernel_guest_read_u32(information + 8u, &free_low) ||
        !kernel_guest_read_u32(information + 12u, &free_high) ||
        !kernel_guest_read_u32(information + 16u, &sectors) ||
        !kernel_guest_read_u32(information + 20u, &bytes)) return false;
    /* Original IMUL32 unit product, then modulo64 multiplication. */
    const uint32_t unit_bytes = sectors * bytes;
    const uint64_t available = ((uint64_t)free_high << 32 | free_low) * unit_bytes;
    const uint64_t total = ((uint64_t)total_high << 32 | total_low) * unit_bytes;
    if (!store64(output[0], available) || !store64(output[1], total) ||
        !store64(output[2], available)) return false;
    *result = 1u;
    return true;
}
