/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XNET_CONFIG_H
#define TSFP_XNET_CONFIG_H
#include <stdbool.h>
#include <stdint.h>
/* Genuine writable guest spans: sector512 with prior contents retained, iosb16
 * (IOSB8 + original LARGE_INTEGER8), call_stack>=36, disjoint from payload492.
 * Caller supplies an actual synchronous open handle. No source/open/seed is faked. */
typedef struct {
    uint32_t sector;
    uint32_t iosb;
    uint32_t call_stack;
    uint32_t stack_bytes;
} xnet_config_kernel;
/* Original434068 real219 + validator. False is host invocation/source refusal.
 * *valid is original read/sector BOOL, distinct from host invocation success. */
bool xnet_config_read_sector_kernel(const xnet_config_kernel *source,
    uint32_t handle, uint32_t slot, uint32_t payload, bool *valid);
#endif
