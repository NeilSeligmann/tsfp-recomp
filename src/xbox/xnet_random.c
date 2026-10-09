/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xnet_random.h"
#include "kernel_call.h"
#include "xnet_hle.h"
#include <errno.h>
#include <stdbool.h>
#include <sys/random.h>

static bool ready(uint32_t entry)
{
    uint32_t stack = 0u;
    if (!kernel_guest_read_u32(XNET_STACK_POINTER, &stack))
        xnet_hle_fatal(entry, "XNET stack readiness word is unreadable");
    return stack != 0u;
}

static uint32_t argument(void *context, unsigned index, uint32_t entry)
{
    uint32_t value = 0u;
    if (!kernel_frame_arg(context, index, &value))
        xnet_hle_fatal(entry, "argument %u is unreadable", index);
    return value;
}

static void fill(uint32_t address, uint32_t length, uint32_t entry)
{
    if (length == 0u) return;
    if (address == 0u || (uint64_t)address + length > (UINT64_C(1) << 32u))
        xnet_hle_fatal(entry, "random output range is null or wraps guest space");
    unsigned char bytes[256];
    uint32_t offset = 0u;
    while (offset < length) {
        const size_t count = length - offset < sizeof(bytes) ? length - offset : sizeof(bytes);
        size_t obtained = 0u;
        while (obtained < count) {
            const ssize_t result = getrandom(bytes + obtained, count - obtained, 0u);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0 || (size_t)result > count - obtained)
                xnet_hle_fatal(entry, "host OS entropy transfer failed");
            obtained += (size_t)result;
        }
        if (!kernel_guest_write_bytes(address + offset, bytes, count))
            xnet_hle_fatal(entry, "random output is not completely writable");
        offset += (uint32_t)count;
    }
}

static uint32_t random_handler(void *context)
{
    if (!ready(XNET_RANDOM_ENTRY)) return XNET_NOT_INITIALIZED;
    const uint32_t length = argument(context, 1u, XNET_RANDOM_ENTRY);
    const uint32_t output = argument(context, 0u, XNET_RANDOM_ENTRY);
    fill(output, length, XNET_RANDOM_ENTRY);
    return 0u;
}

static uint32_t create_key_handler(void *context)
{
    if (!ready(XNET_CREATE_KEY_ENTRY)) return XNET_NOT_INITIALIZED;
    const uint32_t identifier = argument(context, 0u, XNET_CREATE_KEY_ENTRY);
    /* Preserve the original order, including overlapping outputs: generate ID,
     * generate key, then mask the current ID byte after both writes. */
    fill(identifier, 8u, XNET_CREATE_KEY_ENTRY);
    const uint32_t key = argument(context, 1u, XNET_CREATE_KEY_ENTRY);
    fill(key, 16u, XNET_CREATE_KEY_ENTRY);
    uint8_t first = 0u;
    if (!kernel_guest_read_u8(identifier, &first) ||
        !kernel_guest_write_u8(identifier, (uint8_t)(first & 0x0Fu)))
        xnet_hle_fatal(XNET_CREATE_KEY_ENTRY, "key identifier mask access failed");
    return 0u;
}

size_t xnet_random_register(void)
{
    return (size_t)xnet_hle_register(XNET_RANDOM_ENTRY, random_handler) +
           (size_t)xnet_hle_register(XNET_CREATE_KEY_ENTRY, create_key_handler);
}
