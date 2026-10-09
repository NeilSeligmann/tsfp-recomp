/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_flip.h"

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "0") != 0 && strcmp(argv[2], "1") != 0)) {
        return 2;
    }
    const uint32_t ramp = (uint32_t)(argv[2][0] - '0');
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    (void)d3d8_set_push_buffer_size(0x100000u, 0x10000u);
    write_title_parameters(SCRATCH_DATA, 0x140u);
    const uint32_t out = SCRATCH_DATA + 0x200u;
    const uint32_t args[6] = {0u, 1u, 0u, 0u, SCRATCH_DATA, out};
    const uint32_t result = call_stdcall(0x003D9230u, args, 6u);
    if (result != 0u || load(out) != D3D8_DEVICE_BASE) {
        return 3;
    }
    uint8_t ramps[2u * D3D8_FLIP_GAMMA_RAMP_BYTES];
    memcpy(ramps, kernel_guest_at(D3D8_DEVICE_BASE + D3D8_FLIP_DEV_GAMMA_RAMPS,
                                 sizeof(ramps)), sizeof(ramps));
    /* Trigger the existing flip consumer; never seed or replace the gamma bytes. */
    d3d8_flip_reset();
    store(D3D8_DEVICE_BASE + D3D8_FLIP_DEV_CONSUMER, ramp);
    store(D3D8_DEVICE_BASE + D3D8_FLIP_DEV_COUNT, 3u);
    store(D3D8_DEVICE_BASE + D3D8_FLIP_DEV_GAMMA_PENDING + ramp * 4u, 1u);
    const uint32_t slot = D3D8_DEVICE_BASE + D3D8_FLIP_DEV_SLOT0 + ramp * 12u;
    store(slot, 1u);
    store(slot + 4u, 3u);
    store(slot + 8u, 0x01234000u);
    d3d8_flip_lock();
    const uint32_t processed = d3d8_flip_process_locked();
    d3d8_flip_unlock();
    const d3d8_flip_hardware hardware = d3d8_flip_hardware_get();
    FILE *file = fopen(argv[1], "wb");
    if (file == NULL) {
        return 4;
    }
    const uint32_t header[2] = {processed, hardware.gamma_uploads};
    const bool written = fwrite(header, sizeof(header), 1u, file) == 1u &&
                         fwrite(ramps, sizeof(ramps), 1u, file) == 1u &&
                         fwrite(hardware.gamma, sizeof(hardware.gamma), 1u, file) == 1u;
    const bool closed = fclose(file) == 0;
    environment_end();
    return written && closed ? 0 : 5;
}
