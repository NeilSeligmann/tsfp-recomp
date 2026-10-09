/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kernel_thunk.h"
#include "kernel_disk_identity.h"
#include "kernel_call.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    if (!kernel_thunk_map_window()) return 1;
    uint32_t table = KERNEL_THUNK_VA(500u);
    uint32_t model, serial, adjacent;
    if (!kernel_guest_write_u32(table, 0x80000029u) ||
        !kernel_guest_write_u32(table + 4u, 0x8000002au) ||
        !kernel_guest_write_u32(table + 8u, 0x8000009cu) ||
        !kernel_guest_write_u32(table + 12u, 0u)) return 2;
    if (kernel_thunk_patch_table(table, 4u, NULL) != 3u) return 3;
    if (!kernel_guest_read_u32(table, &model) || model != 0u ||
        !kernel_guest_read_u32(table + 4u, &serial) || serial != 0u ||
        !kernel_guest_read_u32(table + 8u, &adjacent) || adjacent != KERNEL_THUNK_VA(156u)) return 4;
    kernel_disk_identity_snapshot snapshot = {0};
    memcpy(snapshot.model, "TEST-MODEL", 10u);
    memcpy(snapshot.serial, "TEST-SERIAL", 11u);
    snapshot.model_length = 10u;
    snapshot.serial_length = 11u;
    if (!kernel_disk_identity_publish(KERNEL_THUNK_VA_DISK_MODEL,
                                     KERNEL_THUNK_VA_DISK_SERIAL,
                                     KERNEL_THUNK_VA_DISK_STORAGE, &snapshot)) return 5;
    kernel_thunk_set_disk_identity_available(true);
    if (!kernel_thunk_disk_identity_available()) return 6;
    if (!kernel_guest_write_u32(table, 0x80000029u) ||
        !kernel_guest_write_u32(table + 4u, 0x8000002au)) return 7;
    if (kernel_thunk_patch_table(table, 4u, NULL) != 2u) return 8;
    if (!kernel_guest_read_u32(table, &model) || model != KERNEL_THUNK_VA_DISK_MODEL ||
        !kernel_guest_read_u32(table + 4u, &serial) || serial != KERNEL_THUNK_VA_DISK_SERIAL) return 9;
    kernel_disk_identity_snapshot copy;
    if (!kernel_disk_identity_read(model, serial, &copy) || copy.model_length != 10u ||
        copy.serial_length != 11u || memcmp(copy.model, "TEST-MODEL", 10u) ||
        memcmp(copy.serial, "TEST-SERIAL", 11u)) return 10;
    kernel_thunk_unmap_window();
    if (kernel_thunk_disk_identity_available()) return 11;
    if (!kernel_thunk_map_window() || kernel_thunk_disk_identity_available()) return 12;
    kernel_thunk_unmap_window();
    puts("disk thunk: unavailable NULL / counted annex / stable snapshot / lifetime reset pass");
    return 0;
}
