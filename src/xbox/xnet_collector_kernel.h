/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XBOX_XNET_COLLECTOR_KERNEL_H
#define TSFP_XBOX_XNET_COLLECTOR_KERNEL_H
#include "xnet_collector.h"

/* Real HLE adapter. call_stack is actual writable guest scratch, at least 24
 * bytes, disjoint from collector output/scratch and live caller arguments.
 * Volume helper must query an actual mounted volume (or genuine absence).
 * No stub invocation: absent required kernel implementations refuse collection. */
typedef struct {
    uint32_t call_stack;
    uint32_t stack_bytes;
    bool (*volume_space)(void *context, uint32_t path, const uint32_t output[3],
                         uint32_t *result);
    void *volume_context;
    bool (*disk_identity_ready)(void *context);
    void *disk_context;
} xnet_collector_kernel;
bool xnet_collector_kernel_source(xnet_collector_kernel *kernel, xnet_collector_source *source);
#endif
