/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xnet_offline.h"
#include "kernel_call.h"
#include "xnet_hle.h"

static xnet_offline_stats state;
static xnet_last_error_reader last_error_reader;

void xnet_offline_reset(void) { state = (xnet_offline_stats){0}; }
xnet_offline_stats xnet_offline_stats_get(void) { return state; }
void xnet_offline_set_last_error_reader(xnet_last_error_reader reader) { last_error_reader = reader; }
uint16_t xnet_swap16(uint16_t value) { return (uint16_t)((value << 8) | (value >> 8)); }
uint32_t xnet_swap32(uint32_t value)
{
    return (value << 24) | ((value & 0xFF00u) << 8) | ((value >> 8) & 0xFF00u) | (value >> 24);
}

static uint32_t ntohs_handler(void *context)
{
    uint32_t value = 0u;
    if (!kernel_frame_arg(context, 0u, &value))
        xnet_hle_fatal(XNET_NTOHS_ENTRY, "ntohs argument is unreadable");
    state.ntohs_calls++;
    return xnet_swap16((uint16_t)value);
}

static uint32_t htonl_handler(void *context)
{
    uint32_t value = 0u;
    if (!kernel_frame_arg(context, 0u, &value))
        xnet_hle_fatal(XNET_HTONL_ENTRY, "htonl argument is unreadable");
    state.htonl_calls++;
    return xnet_swap32(value);
}

static uint32_t last_error_handler(void *context)
{
    (void)context;
    uint32_t error = 0u;
    if (last_error_reader == NULL || !last_error_reader(&error))
        xnet_hle_fatal(XNET_WSAGETLASTERROR_ENTRY,
                       "WSAGetLastError needs the thread's TLS last-error slot and none is readable");
    state.last_error_calls++;
    return error;
}

size_t xnet_offline_register(void)
{
    return (size_t)xnet_hle_register(XNET_NTOHS_ENTRY, ntohs_handler) +
           (size_t)xnet_hle_register(XNET_HTONL_ENTRY, htonl_handler) +
           (size_t)xnet_hle_register(XNET_WSAGETLASTERROR_ENTRY, last_error_handler);
}
