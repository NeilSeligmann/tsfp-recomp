/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_DISK_IDENTITY_LINUX_H
#define TSFP_DISK_IDENTITY_LINUX_H
#include "kernel_disk_identity.h"

typedef enum {
    DISK_IDENTITY_OK,
    DISK_IDENTITY_NO_BACKING,
    DISK_IDENTITY_UNAVAILABLE,
    DISK_IDENTITY_IO_ERROR,
    DISK_IDENTITY_INVALID,
} disk_identity_status;

/* Read-only actual backing-device identity. Opens and pins the selected backing
 * path, obtains st_dev (st_rdev for a block device), verifies its sysfs dev file,
 * and reads model/serial of that device. Partition devices resolve to their
 * actual parent. Overlay, network, loop/dm with no unique hardware identity and
 * devices without both fields refuse; no synthesized image/host UUID fallback.
 * Snapshot copied only after all input validation succeeds; no identity logging.
 * Linux sysfs describes the host backing device, not an Xbox ATA device: that
 * explicit host policy is INFERRED and separately documented from xemu evidence. */
disk_identity_status disk_identity_linux_acquire(const char *backing_path,
                                                 kernel_disk_identity_snapshot *out);
#endif
