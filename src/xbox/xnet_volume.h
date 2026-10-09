/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XNET_VOLUME_H
#define TSFP_XNET_VOLUME_H
#include <stdbool.h>
#include <stdint.h>

/* Original0037D4F9 volume helper using real strict NtOpenFile/volume218 and
 * NtClose. Genuine writable guest scratch>=64 bytes, call_stack>=28 bytes,
 * mutually disjoint and disjoint from name/output. Error callback must execute
 * original NTSTATUS-to-TLS handling; open_phase includes original2->3 mapping.
 * False means unavailable invocation/mapping, not a fabricated Win32 result. */
typedef struct {
    uint32_t scratch;
    uint32_t call_stack;
    uint32_t stack_bytes;
    bool (*set_nt_error)(void *context, uint32_t status, bool open_phase);
    void *error_context;
} xnet_volume_kernel;

bool xnet_volume_space_kernel(void *context, uint32_t path,
                             const uint32_t output[3], uint32_t *result);
#endif
