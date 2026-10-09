/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XBOX_XNET_OFFLINE_H
#define TSFP_XBOX_XNET_OFFLINE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* T1070: the XNET library functions whose original body is self contained, so the host answers them
 * exactly (MEASURED against the original bytes under Unicorn, tests/test_xnet_oracle.py). Everything
 * else in the XNET section is a method of the network stack object at [0x7715EC] (built by
 * XNetStartup 0x431BB1 through 0x4319AB, which allocates 0xD50 bytes and reads device config and
 * link state), has no host model, and still stops the run by name. No success is fabricated.
 *
 * 0x00432700 ntohs(x), stdcall ret 4: swaps the two bytes of the low word, result in ax (the upper
 *            half of eax is whatever the caller left, the host returns it zero).
 * 0x0043270F htonl(x), stdcall ret 4: bswap. ntohl and htonl are the same code, one address.
 * 0x00431D53 WSAGetLastError(), a bare jump to GetLastError 0x37E9A7: eax = the thread's last error
 *            word, [[fs:4] + [0x771368]*4] + 4. The host reads that same guest slot through a reader
 *            the host binary installs (the fs base is a host thread local), so a failure recorded by
 *            guest code is what is returned. With no reader the call stops by name. */
#define XNET_NTOHS_ENTRY 0x00432700u
#define XNET_HTONL_ENTRY 0x0043270Fu
#define XNET_WSAGETLASTERROR_ENTRY 0x00431D53u

typedef bool (*xnet_last_error_reader)(uint32_t *last_error);

typedef struct {
    uint64_t ntohs_calls, htonl_calls, last_error_calls;
} xnet_offline_stats;

void xnet_offline_reset(void);
xnet_offline_stats xnet_offline_stats_get(void);
uint16_t xnet_swap16(uint16_t value);
uint32_t xnet_swap32(uint32_t value);
void xnet_offline_set_last_error_reader(xnet_last_error_reader reader);
/* Registers the three handlers in the XNET module, returns how many were taken. */
size_t xnet_offline_register(void);
#endif
