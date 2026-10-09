/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_callbacks.h"
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const d3d8_surface_entry row = {0x003D3530u, NULL, 1u};
    CHECK(d3d8_hle_init(&row, 1u));
    CHECK_EQ_U32(d3d8_callbacks_register(), 1u);
    const uint32_t devices[] = {D3D8_DEVICE_BASE, 0x003D8001u};
    const uint32_t callbacks[] = {0u, 1u, 0xDEADBEEFu, UINT32_MAX};
    for (size_t d = 0; d < 2; d++) {
        store(0x003E3F58u, devices[d]);
        for (size_t c = 0; c < 4; c++) {
            store(devices[d] + 0x1DB4u, 0x12345678u);
            store(devices[d] + 0x1DBCu, 0x87654321u);
            CHECK_EQ_U32(call_stdcall(0x003D3530u, &callbacks[c], 1u), callbacks[c]);
            CHECK_EQ_U32(load(devices[d] + 0x1DB8u), callbacks[c]);
            CHECK_EQ_U32(load(devices[d] + 0x1DB4u), 0x12345678u);
            CHECK_EQ_U32(load(devices[d] + 0x1DBCu), 0x87654321u);
        }
    }
    const uint32_t invalid[] = {0u, 0x70000000u, UINT32_MAX, 0x003FE247u};
    store(D3D8_DEVICE_BASE + 0x1DB8u, 0xCAFEBABEu);
    for (size_t i = 0; i < 4; i++) {
        store(0x003E3F58u, invalid[i]);
        RUN_EXPECTING_FATAL((void)d3d8_set_vertical_blank_callback(0xDEADBEEFu));
        CHECK(fatal_seen);
        CHECK_EQ_U32(fatal_address, 0x003D3530u);
        CHECK_EQ_U32(load(D3D8_DEVICE_BASE + 0x1DB8u), 0xCAFEBABEu);
    }
    environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
