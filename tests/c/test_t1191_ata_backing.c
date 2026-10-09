/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "disk_identity_xemu.h"
#include "host_options.h"
#include "kernel_call.h"
#include "kernel_thunk.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "check %u line %d\n", checks, __LINE__); return 1; } } while (0)

int main(void)
{
    options opts;
    char *plain[] = {"host", "declared.xbe"};
    CHECK(parse_options(2, plain, &opts) && !opts.disk_identity_xemu);
    char *missing[] = {"host", "declared.xbe", "--disk-identity-xemu"};
    CHECK(!parse_options(3, missing, &opts));
    char *selected[] = {"host", "declared.xbe", "--disk-identity-xemu", "--hdd", "declared"};
    CHECK(parse_options(5, selected, &opts) && opts.disk_identity_xemu);
    char *reverse[] = {"host", "declared.xbe", "--hdd", "declared", "--disk-identity-xemu"};
    CHECK(parse_options(5, reverse, &opts) && opts.disk_identity_xemu);

    char root[] = "/tmp/tsfp-t1191-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char image[512], moved[512], replacement[512];
    CHECK(snprintf(image, sizeof(image), "%s/image", root) > 0);
    int fd = open(image, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    unsigned char bytes[512]; memset(bytes, 0x96, sizeof(bytes));
    CHECK(write(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes));
    CHECK(close(fd) == 0);
    kernel_file_reset();
    CHECK(kernel_file_mount_host_device("\\Device\\Harddisk0\\partition0", root, "image", 4096u));
    CHECK(kernel_file_mount_host_dir("\\Device\\Harddisk0\\partition1", root));
    kernel_file_device_backing backing;
    memset(&backing, 0xA5, sizeof(backing));
    kernel_file_device_backing before = backing;
    CHECK(!kernel_file_device_backing_acquire("\\Device\\Missing", &backing));
    CHECK(memcmp(&backing, &before, sizeof(backing)) == 0);
    CHECK(!kernel_file_device_backing_acquire("\\Device\\Harddisk0\\partition1", &backing));
    CHECK(memcmp(&backing, &before, sizeof(backing)) == 0);
    CHECK(!kernel_file_device_backing_acquire(NULL, &backing));
    CHECK(!kernel_file_device_backing_acquire("\\Device\\Harddisk0\\partition0", NULL));
    CHECK(kernel_file_device_backing_acquire("\\device\\harddisk0\\PARTITION0", &backing));
    CHECK(backing.root_fd >= 0 && backing.capacity == 4096u && strcmp(backing.filename, "image") == 0);
    CHECK((fcntl(backing.root_fd, F_GETFD) & FD_CLOEXEC) != 0);
    kernel_disk_identity_snapshot snapshot;
    CHECK(disk_identity_xemu_acquire_backing(&backing, &snapshot) == DISK_XEMU_IDENTITY_OK);
    CHECK(snapshot.model_length == 13u && snapshot.serial_length == 7u);
    CHECK(snapshot.model[13] == ' ' && snapshot.serial[7] == ' ');
    CHECK(snprintf(moved, sizeof(moved), "%s-moved", root) > 0);
    CHECK(rename(root, moved) == 0 && mkdir(root, 0700) == 0);
    kernel_file_device_backing again;
    CHECK(kernel_file_device_backing_acquire("\\Device\\Harddisk0\\partition0", &again));
    CHECK(disk_identity_xemu_acquire_backing(&again, &snapshot) == DISK_XEMU_IDENTITY_OK);
    CHECK(close(again.root_fd) == 0);
    kernel_file_unmount_all();
    CHECK(disk_identity_xemu_acquire_backing(&backing, &snapshot) == DISK_XEMU_IDENTITY_OK);
    CHECK(!kernel_file_device_backing_acquire("\\Device\\Harddisk0\\partition0", &again));
    CHECK(close(backing.root_fd) == 0);

    /* Real production DATA annex/patching, before any guest thread. */
    CHECK(kernel_thunk_map_window());
    CHECK(kernel_disk_identity_publish(KERNEL_THUNK_VA_DISK_MODEL,
                                      KERNEL_THUNK_VA_DISK_SERIAL,
                                      KERNEL_THUNK_VA_DISK_STORAGE, &snapshot));
    kernel_thunk_set_disk_identity_available(true);
    uint32_t table = KERNEL_THUNK_VA(500u);
    CHECK(kernel_guest_write_u32(table, 0x80000029u));
    CHECK(kernel_guest_write_u32(table + 4u, 0x8000002au));
    CHECK(kernel_guest_write_u32(table + 8u, 0u));
    CHECK(kernel_thunk_patch_table(table, 3u, NULL) == 2u);
    uint32_t model, serial;
    CHECK(kernel_guest_read_u32(table, &model) && model == KERNEL_THUNK_VA_DISK_MODEL);
    CHECK(kernel_guest_read_u32(table + 4u, &serial) && serial == KERNEL_THUNK_VA_DISK_SERIAL);
    kernel_disk_identity_snapshot exported;
    CHECK(kernel_disk_identity_read(model, serial, &exported));
    CHECK(exported.model_length == snapshot.model_length && exported.serial_length == snapshot.serial_length);
    CHECK(memcmp(exported.model, snapshot.model, snapshot.model_length) == 0 &&
          memcmp(exported.serial, snapshot.serial, snapshot.serial_length) == 0);
    unsigned char stored[60];
    CHECK(kernel_guest_read_bytes(KERNEL_THUNK_VA_DISK_STORAGE, stored, sizeof(stored)));
    CHECK(memcmp(stored, snapshot.model, 40u) == 0 && memcmp(stored + 40u, snapshot.serial, 20u) == 0);
    kernel_thunk_unmap_window();
    CHECK(!kernel_thunk_disk_identity_available());
    CHECK(snprintf(replacement, sizeof(replacement), "%s/image", moved) > 0);
    CHECK(unlink(replacement) == 0 && rmdir(moved) == 0 && rmdir(root) == 0);
    printf("T1191 actual mounted backing/options/DATA: %u checks passed\n", checks);
    return 0;
}
