/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_DISK_IDENTITY_XEMU_H
#define TSFP_DISK_IDENTITY_XEMU_H
#include "kernel_disk_identity.h"
#include "kernel_file.h"

typedef enum {
    DISK_XEMU_IDENTITY_OK,
    DISK_XEMU_IDENTITY_NO_BACKING,
    DISK_XEMU_IDENTITY_IO_ERROR,
    DISK_XEMU_IDENTITY_INVALID,
} disk_xemu_identity_status;

/* Explicit INFERRED virtual-console profile: primary IDE master is initialized
 * first under xemu 0.8.136's default device policy. It uses that device's public
 * model/serial policy, not bytes from media or the host's physical identity.
 * The supplied fd must be the actual mounted guest image, opened for reading.
 * A private duplicate pins a regular, nonempty, sector-aligned image throughout
 * acquisition; caller keeps its fd valid until return. Directories, raw devices,
 * empty images and write-only fds refuse.
 * This does not attest sector contents, entropy, geometry or XNET readiness.
 * No raw strings are logged; output is unchanged on failure. */
disk_xemu_identity_status disk_identity_xemu_acquire(int mounted_image_fd,
                                                    kernel_disk_identity_snapshot *out);
/* Opens only the actual mounted backing capability above. Metadata-only O_PATH
 * pins the selected inode before any readable open, so a pathname race cannot
 * redirect acquisition into a host raw device or FIFO. No media writes occur. */
disk_xemu_identity_status disk_identity_xemu_acquire_backing(
    const kernel_file_device_backing *backing, kernel_disk_identity_snapshot *out);
#endif
