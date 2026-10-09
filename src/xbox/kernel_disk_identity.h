/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_KERNEL_DISK_IDENTITY_H
#define TSFP_KERNEL_DISK_IDENTITY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Actual backing-device snapshot. No pathname, inode, random value or image hash
 * substitutes for the model/serial. ATA field widths bound these exports. */
#define KERNEL_DISK_MODEL_CAPACITY 40u
#define KERNEL_DISK_SERIAL_CAPACITY 20u
#define KERNEL_DISK_IDENTITY_STORAGE_BYTES 64u

typedef struct {
    uint8_t model[KERNEL_DISK_MODEL_CAPACITY];
    uint8_t serial[KERNEL_DISK_SERIAL_CAPACITY];
    uint16_t model_length;
    uint16_t serial_length;
    uint32_t device_major;
    uint32_t device_minor;
} kernel_disk_identity_snapshot;

/* Publish two x86 STRING descriptors: USHORT Length/MaximumLength, DWORD Buffer.
 * Nonempty actual source bytes required. Storage and descriptors must be mapped,
 * writable, disjoint. Caller owns storage until guest shutdown. MaximumLength
 * is the measured fixed field capacity40/20; space padding is outside Length.
 * No NUL terminator is promised. No raw identity is logged.
 * Invalid input refuses before writes; mapping faults may leave a written prefix.
 * Caller must not expose import pointers unless the complete publish succeeds. */
bool kernel_disk_identity_publish(uint32_t model_va, uint32_t serial_va,
                                  uint32_t storage_va,
                                  const kernel_disk_identity_snapshot *identity);

/* Read declared descriptors and source bytes for a collector. Zero/empty,
 * malformed bounds or unreadable spans refuse, never produce zero-filled entropy.
 * Output remains unchanged on failure; caller must hold guest mapping lifetime. */
bool kernel_disk_identity_read(uint32_t model_va, uint32_t serial_va,
                               kernel_disk_identity_snapshot *out);
#endif
