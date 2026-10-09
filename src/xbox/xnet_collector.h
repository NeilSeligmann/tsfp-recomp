/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XBOX_XNET_COLLECTOR_H
#define TSFP_XBOX_XNET_COLLECTOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A source invokes an actual dependency. False means the dependency could not
 * execute, distinct from its genuine return/status value (which may fail).
 * Kernel calls have normal ordinal/stdcall argument order and EDX:EAX results.
 * Volume is the original 0037D4F9 T: helper with three guest output pointers.
 * Neither callback supplies an invented "entropy" blob. */
typedef struct {
    bool (*kernel_call)(void *context, unsigned ordinal, const uint32_t *arguments,
                        size_t count, uint64_t *result);
    bool (*volume_space)(void *context, uint32_t path, const uint32_t output[3],
                         uint32_t *result);
    void *context;
    /* Production must prove actual disk exports ready, even at capacity zero. */
    bool (*disk_identity_ready)(void *context);
} xnet_collector_source;

/* Original 00441588 collector, bounded to the 512-byte constructor capacity.
 * scratch is genuine writable guest storage, at least 0x200 bytes, with prior
 * bytes retained: original MmQueryStatistics receives uninitialized bytes and
 * its status is ignored. The source must execute the call, not clear these bytes
 * to hide a missing implementation. Imports are read from the retail slots.
 * All dependencies execute even with capacity zero; oversized segments skip
 * whole, and later fitting segments are still appended. Invocation/mapping
 * failure returns false, preserving any prefix already written; written is set
 * only on a complete collection. No guest ABI/production registration here. */
bool xnet_collect_entropy(uint32_t output, uint32_t capacity, uint32_t scratch,
                          const xnet_collector_source *source, uint32_t *written);
#endif
