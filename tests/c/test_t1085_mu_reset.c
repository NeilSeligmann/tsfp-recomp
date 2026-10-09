/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "mu_device.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV); xinput_hle_init(); xinput_devices_reset();
    /* No mapped MU word is permitted during startup cleanup. */
    mu_reset(); CHECK_EQ_U32(mu_mounted_mask(), 0u);
    map_fixed(0x76E000u, 0x1000u);
    const unsigned masks[] = {0u, 1u, 0x18u, 0x81u, 0x5Au, 0xFFu};
    for (unsigned k = 0u; k < sizeof(masks)/sizeof(masks[0]); k++) {
        store(MU_MASK_ADDRESS - 4u, 0xBAD01234u);
        store(MU_MASK_ADDRESS + 4u, 0xCADE5678u);
        store(MU_TITLE_DRIVE_ADDRESS, 0xBAD01234u);
        for (unsigned i = 0u; i < 8u; i++) {
            CHECK(mu_attach_blank(i/2u, i%2u, 8u*1024u*1024u, true, i));
            if ((masks[k] & (1u << i)) != 0u) CHECK_EQ_U32(mu_xmount(i/2u, i%2u, false, NULL), 0u);
        }
        CHECK_EQ_U32(mu_mounted_mask(), masks[k]);
        /* Explicit reset normalizes even a stale mirror after zero mounts. */
        if (masks[k] == 0u) store(MU_MASK_ADDRESS, 0x11223344u);
        mu_reset();
        CHECK_EQ_U32(mu_mounted_mask(), 0u); CHECK_EQ_U32(load(MU_MASK_ADDRESS), 0u);
        CHECK_EQ_U32(load(MU_MASK_ADDRESS - 4u), 0xBAD01234u);
        CHECK_EQ_U32(load(MU_MASK_ADDRESS + 4u), 0xCADE5678u);
        for (unsigned i = 0u; i < 8u; i++) CHECK(!mu_attached(i/2u, i%2u));
        mu_reset(); CHECK_EQ_U32(load(MU_MASK_ADDRESS), 0u);
        CHECK(mu_attach_blank(2u, 1u, 8u*1024u*1024u, true, 1u));
        CHECK_EQ_U32(mu_xmount(2u, 1u, false, NULL), 0u);
        CHECK_EQ_U32(load(MU_MASK_ADDRESS), 0x20u);
        CHECK_EQ_U32(mu_xunmount(2u, 1u), 0u); CHECK(mu_detach(2u, 1u));
    }
    xinput_devices_reset(); environment_end();
    printf("MU reset: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
