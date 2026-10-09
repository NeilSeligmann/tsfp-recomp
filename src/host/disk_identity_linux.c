/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "disk_identity_linux.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifndef T1147_SYSFS_ROOT
#define T1147_SYSFS_ROOT "/sys/dev/block"
#endif

static disk_identity_status read_field(int directory, const char *name,
                                       uint8_t *out, uint16_t capacity, uint16_t *length)
{
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return errno == ENOENT ? DISK_IDENTITY_UNAVAILABLE : DISK_IDENTITY_IO_ERROR;
    }
    uint8_t buffer[128];
    ssize_t got;
    do {
        got = read(fd, buffer, sizeof(buffer));
    } while (got < 0 && errno == EINTR);
    (void)close(fd);
    if (got < 0) return DISK_IDENTITY_IO_ERROR;
    size_t size = (size_t)got;
    /* sysfs emits exactly one line. Keep actual space bytes; remove the line
     * delimiter and ATA trailing space padding, never truncate a disk field. */
    if (size != 0u && buffer[size - 1u] == '\n') size--;
    while (size != 0u && buffer[size - 1u] == ' ') size--;
    if (size == 0u || size > capacity) return DISK_IDENTITY_INVALID;
    for (size_t i = 0u; i < size; i++) {
        if (buffer[i] < 0x20u || buffer[i] > 0x7eu) return DISK_IDENTITY_INVALID;
    }
    memcpy(out, buffer, size);
    *length = (uint16_t)size;
    return DISK_IDENTITY_OK;
}

disk_identity_status disk_identity_linux_acquire(const char *backing_path,
                                                 kernel_disk_identity_snapshot *out)
{
    if (!backing_path || !out) return DISK_IDENTITY_NO_BACKING;
    int backing = open(backing_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (backing < 0) return DISK_IDENTITY_IO_ERROR;
    struct stat st;
    if (fstat(backing, &st) != 0) {
        (void)close(backing);
        return DISK_IDENTITY_IO_ERROR;
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISBLK(st.st_mode)) {
        (void)close(backing);
        return DISK_IDENTITY_INVALID;
    }
    dev_t device = S_ISBLK(st.st_mode) ? st.st_rdev : st.st_dev;
    char path[512];
    int size = snprintf(path, sizeof(path), "%s/%u:%u", T1147_SYSFS_ROOT,
                        major(device), minor(device));
    if (size < 0 || (size_t)size >= sizeof(path)) {
        (void)close(backing);
        return DISK_IDENTITY_INVALID;
    }
    int sysfs = open(path, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (sysfs < 0) {
        (void)close(backing);
        return DISK_IDENTITY_UNAVAILABLE;
    }
    /* Bind the sysfs record to the held backing descriptor's device number. */
    uint8_t devtext[64];
    uint16_t devlength = 0u;
    disk_identity_status status = read_field(sysfs, "dev", devtext, 63u, &devlength);
    char expected[64];
    int expected_size = snprintf(expected, sizeof(expected), "%u:%u",
                                 major(device), minor(device));
    if (status != DISK_IDENTITY_OK || expected_size < 0 ||
        devlength != (uint16_t)expected_size ||
        memcmp(devtext, expected, devlength) != 0) {
        (void)close(sysfs);
        (void)close(backing);
        return status == DISK_IDENTITY_OK ? DISK_IDENTITY_INVALID : status;
    }
    int hardware = openat(sysfs, "device", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (hardware < 0) {
        int partition = openat(sysfs, "partition", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (partition >= 0) {
            (void)close(partition);
            hardware = openat(sysfs, "../device", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
        }
    }
    (void)close(sysfs);
    if (hardware < 0) {
        (void)close(backing);
        return DISK_IDENTITY_UNAVAILABLE;
    }
    kernel_disk_identity_snapshot snapshot = {0};
    status = read_field(hardware, "model", snapshot.model, KERNEL_DISK_MODEL_CAPACITY,
                        &snapshot.model_length);
    if (status == DISK_IDENTITY_OK) {
        status = read_field(hardware, "serial", snapshot.serial, KERNEL_DISK_SERIAL_CAPACITY,
                            &snapshot.serial_length);
    }
    (void)close(hardware);
    (void)close(backing);
    if (status == DISK_IDENTITY_OK) {
        snapshot.device_major = major(device);
        snapshot.device_minor = minor(device);
        *out = snapshot;
    }
    return status;
}
