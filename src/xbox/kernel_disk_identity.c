/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kernel_disk_identity.h"
#include "kernel_call.h"
#include <string.h>

static bool range(uint32_t address, size_t bytes)
{
    return address != 0u && (uint64_t)address + bytes <= UINT64_C(0x100000000);
}

static bool overlap(uint32_t a, size_t na, uint32_t b, size_t nb)
{
    return (uint64_t)a < (uint64_t)b + nb && (uint64_t)b < (uint64_t)a + na;
}

static void descriptor(uint8_t out[8], uint16_t length, uint16_t maximum, uint32_t buffer)
{
    out[0] = (uint8_t)length;
    out[1] = (uint8_t)(length >> 8);
    out[2] = (uint8_t)maximum;
    out[3] = (uint8_t)(maximum >> 8);
    out[4] = (uint8_t)buffer;
    out[5] = (uint8_t)(buffer >> 8);
    out[6] = (uint8_t)(buffer >> 16);
    out[7] = (uint8_t)(buffer >> 24);
}

bool kernel_disk_identity_publish(uint32_t model_va, uint32_t serial_va,
                                  uint32_t storage_va,
                                  const kernel_disk_identity_snapshot *identity)
{
    if (!identity || identity->model_length == 0u || identity->serial_length == 0u ||
        identity->model_length > KERNEL_DISK_MODEL_CAPACITY ||
        identity->serial_length > KERNEL_DISK_SERIAL_CAPACITY ||
        !range(model_va, 8u) || !range(serial_va, 8u) ||
        !range(storage_va, KERNEL_DISK_IDENTITY_STORAGE_BYTES) ||
        overlap(model_va, 8u, serial_va, 8u) ||
        overlap(model_va, 8u, storage_va, KERNEL_DISK_IDENTITY_STORAGE_BYTES) ||
        overlap(serial_va, 8u, storage_va, KERNEL_DISK_IDENTITY_STORAGE_BYTES)) {
        return false;
    }
    uint8_t storage[KERNEL_DISK_IDENTITY_STORAGE_BYTES] = {0};
    uint8_t model[8], serial[8];
    memset(storage, 0x20, 60u);
    memcpy(storage, identity->model, identity->model_length);
    memcpy(storage + 40u, identity->serial, identity->serial_length);
    descriptor(model, identity->model_length, KERNEL_DISK_MODEL_CAPACITY, storage_va);
    descriptor(serial, identity->serial_length, KERNEL_DISK_SERIAL_CAPACITY, storage_va + 40u);
    /* Publish payload first. Caller publishes import slots only after both
     * descriptors have been written, before starting guest threads. */
    return kernel_guest_write_bytes(storage_va, storage, sizeof(storage)) &&
           kernel_guest_write_bytes(model_va, model, sizeof(model)) &&
           kernel_guest_write_bytes(serial_va, serial, sizeof(serial));
}

static bool read_string(uint32_t address, uint8_t *out, uint16_t *length, uint16_t cap)
{
    uint8_t descriptor_bytes[8];
    if (!kernel_guest_read_bytes(address, descriptor_bytes, sizeof(descriptor_bytes))) {
        return false;
    }
    const uint16_t size = (uint16_t)((unsigned)descriptor_bytes[0] |
                                   ((unsigned)descriptor_bytes[1] << 8));
    const uint16_t maximum = (uint16_t)((unsigned)descriptor_bytes[2] |
                                      ((unsigned)descriptor_bytes[3] << 8));
    const uint32_t pointer = (uint32_t)descriptor_bytes[4] |
                            ((uint32_t)descriptor_bytes[5] << 8) |
                            ((uint32_t)descriptor_bytes[6] << 16) |
                            ((uint32_t)descriptor_bytes[7] << 24);
    if (size == 0u || size > cap || maximum < size || !range(pointer, size) ||
        !kernel_guest_read_bytes(pointer, out, size)) {
        return false;
    }
    *length = size;
    return true;
}

bool kernel_disk_identity_read(uint32_t model_va, uint32_t serial_va,
                               kernel_disk_identity_snapshot *out)
{
    if (!out) {
        return false;
    }
    kernel_disk_identity_snapshot snapshot = {0};
    if (!read_string(model_va, snapshot.model, &snapshot.model_length,
                     KERNEL_DISK_MODEL_CAPACITY) ||
        !read_string(serial_va, snapshot.serial, &snapshot.serial_length,
                     KERNEL_DISK_SERIAL_CAPACITY)) {
        return false;
    }
    *out = snapshot;
    return true;
}
