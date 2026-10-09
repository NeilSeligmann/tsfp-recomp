/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "disk_identity_xemu.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Source: xemu fc24584ce88f0915ad7f04775bb7712c2e3f49ee,
 * hw/ide/core.c ide_init1/ide_init_drive and ide_identify. This profile declares
 * a fresh console's primary-master default instance (first initialization = 1),
 * never a caller-selected serial or a hash/name-derived host-device identity.
 * T1147's independently captured DATA 41/42 hashes match both counted fields
 * and all fixed-width padding for this policy. The adaptation stays INFERRED. */
static const unsigned char profile_model[] = "QEMU HARDDISK";
static const unsigned char profile_serial[] = "QM00001";

/* Kernel DATA consumes decoded text. ATA wire swapping is independently checked
 * against the pinned primary encoder; no unused IDENTIFY command is fabricated
 * here. A device snapshot has the source policy's fixed-width space padding. */
static void device_text(uint8_t *out, size_t width, const unsigned char *text,
                        size_t length)
{
    memset(out, ' ', width);
    memcpy(out, text, length);
}

disk_xemu_identity_status disk_identity_xemu_acquire(int mounted_image_fd,
                                                    kernel_disk_identity_snapshot *out)
{
    if (!out) {
        return DISK_XEMU_IDENTITY_INVALID;
    }
    if (mounted_image_fd < 0) {
        return DISK_XEMU_IDENTITY_NO_BACKING;
    }
    const int flags = fcntl(mounted_image_fd, F_GETFL);
    if (flags < 0) {
        return DISK_XEMU_IDENTITY_IO_ERROR;
    }
    if ((flags & O_ACCMODE) == O_WRONLY || (flags & O_PATH) != 0) {
        return DISK_XEMU_IDENTITY_INVALID;
    }
    const int held = fcntl(mounted_image_fd, F_DUPFD_CLOEXEC, 3);
    if (held < 0) {
        return DISK_XEMU_IDENTITY_IO_ERROR;
    }
    struct stat backing;
    const int status = fstat(held, &backing);
    if (status < 0) {
        (void)close(held);
        return DISK_XEMU_IDENTITY_IO_ERROR;
    }
    if (!S_ISREG(backing.st_mode) || backing.st_size < 512 ||
        (backing.st_size % 512) != 0) {
        (void)close(held);
        return DISK_XEMU_IDENTITY_INVALID;
    }
    kernel_disk_identity_snapshot candidate = {0};
    device_text(candidate.model, sizeof(candidate.model), profile_model,
                sizeof(profile_model) - 1u);
    device_text(candidate.serial, sizeof(candidate.serial), profile_serial,
                sizeof(profile_serial) - 1u);
    candidate.model_length = sizeof(profile_model) - 1u;
    candidate.serial_length = sizeof(profile_serial) - 1u;
    /* Virtual IDE unit, not a Linux major/minor: both host-only fields remain 0.
     * Guest DATA exports contain only the two measured counted STRING fields. */
    memcpy(out, &candidate, sizeof(candidate));
    (void)close(held);
    return DISK_XEMU_IDENTITY_OK;
}

disk_xemu_identity_status disk_identity_xemu_acquire_backing(
    const kernel_file_device_backing *backing, kernel_disk_identity_snapshot *out)
{
    if (!backing || !out || backing->root_fd < 0 || backing->capacity == 0u) {
        return DISK_XEMU_IDENTITY_INVALID;
    }
    const size_t length = strnlen(backing->filename, sizeof(backing->filename));
    if (length == 0u || length >= sizeof(backing->filename) ||
        strchr(backing->filename, '/') || strchr(backing->filename, '\\') ||
        strcmp(backing->filename, ".") == 0 || strcmp(backing->filename, "..") == 0) {
        return DISK_XEMU_IDENTITY_INVALID;
    }
    const int pinned = openat(backing->root_fd, backing->filename,
                              O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (pinned < 0) {
        return DISK_XEMU_IDENTITY_NO_BACKING;
    }
    struct stat metadata;
    if (fstat(pinned, &metadata) != 0) {
        (void)close(pinned);
        return DISK_XEMU_IDENTITY_IO_ERROR;
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < 512 ||
        (metadata.st_size % 512) != 0 || (uint64_t)metadata.st_size > backing->capacity) {
        (void)close(pinned);
        return DISK_XEMU_IDENTITY_INVALID;
    }
    char descriptor_path[64];
    const int written = snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", pinned);
    if (written < 0 || (size_t)written >= sizeof(descriptor_path)) {
        (void)close(pinned);
        return DISK_XEMU_IDENTITY_IO_ERROR;
    }
    /* Deliberately follows only our retained private proc-fd capability, not a
     * caller-controlled symlink. The checked inode cannot become a raw node. */
    const int readable = open(descriptor_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    struct stat opened;
    if (readable < 0 || fstat(readable, &opened) != 0 ||
        opened.st_dev != metadata.st_dev || opened.st_ino != metadata.st_ino) {
        if (readable >= 0) {
            (void)close(readable);
        }
        (void)close(pinned);
        return DISK_XEMU_IDENTITY_IO_ERROR;
    }
    const disk_xemu_identity_status result = disk_identity_xemu_acquire(readable, out);
    (void)close(readable);
    (void)close(pinned);
    return result;
}
