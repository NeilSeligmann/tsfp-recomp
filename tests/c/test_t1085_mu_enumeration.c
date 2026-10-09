/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "mu_device.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#define TYPE 0x46C6E0u
#define DECL SCRATCH_DATA
#define ADD (SCRATCH_DATA + 0x100u)
#define REMOVE (SCRATCH_DATA + 0x120u)
static const uint32_t declarations[8] = {TYPE,8u,0x46C8A0u,4u,0x46C894u,4u,0x46C75Cu,4u};
static const uint32_t bits[8] = {1u,0x10000u,2u,0x20000u,4u,0x40000u,8u,0x80000u};
static void changes(uint32_t added, uint32_t removed)
{
    CHECK_EQ_U32(xinput_devices_changes(TYPE, ADD, REMOVE), (added | removed) != 0u);
    CHECK_EQ_U32(load(ADD), added); CHECK_EQ_U32(load(REMOVE), removed);
    CHECK_EQ_U32(load(TYPE + 4u), 0u);
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init(); xinput_devices_reset(); mu_reset();
    map_fixed(0x46C000u, 0x1000u); map_fixed(0x771000u, 0x1000u); map_fixed(0x76E000u, 0x1000u);
    memcpy(kernel_guest_at(DECL, 32u), declarations, 32u);
    /* Pre-init attachment leaves original cold tables untouched. */
    CHECK(mu_attach_blank(3u, 1u, FATX_MIN_IMAGE, false, 0u));
    CHECK_EQ_U32(load(TYPE), 0u);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    changes(0x80000u, 0u);
    CHECK(mu_detach(3u, 1u)); changes(0u, 0x80000u);
    for (unsigned i = 0u; i < 8u; i++) {
        const unsigned port = i / 2u, slot = i % 2u;
        CHECK(mu_attach_blank(port, slot, 8u * 1024u * 1024u, true, i));
        CHECK_EQ_U32(load(TYPE), bits[i]);
        CHECK_EQ_U32(load(TYPE + 4u), bits[i]);
        CHECK(!mu_attach_blank(port, slot, FATX_MIN_IMAGE, false, 0u));
        CHECK_EQ_U32(xinput_devices_peek(TYPE, ADD, REMOVE), bits[i]);
        CHECK_EQ_U32(load(TYPE + 4u), bits[i]);
        changes(bits[i], 0u);
        /* Mounted drive mask is different; mount cannot consume/change presence. */
        char letter = 0;
        CHECK_EQ_U32(mu_xmount(port, slot, true, &letter), 0u);
        CHECK_EQ_U32(mu_mounted_mask(), 1u << i);
        CHECK(!mu_detach(port, slot)); changes(0u, 0u);
        CHECK_EQ_U32(mu_xunmount(port, slot), 0u); changes(0u, 0u);
        CHECK(mu_detach(port, slot));
        CHECK(mu_attach_blank(port, slot, FATX_MIN_IMAGE, false, 0u));
        /* Remove+reinsert before consumption reports BOTH, not only net state. */
        changes(bits[i], bits[i]);
        CHECK(mu_detach(port, slot)); changes(0u, bits[i]);
    }
    CHECK(!mu_attach_blank(4u, 0u, FATX_MIN_IMAGE, false, 0u));
    CHECK(!mu_attach_blank(0u, 2u, FATX_MIN_IMAGE, false, 0u));
    CHECK(!mu_attach_image(0u, 0u, NULL, FATX_MIN_IMAGE));
    CHECK(!mu_attach_file(0u, 0u, "/nonexistent/t1085-no-image", 0u));
    changes(0u, 0u);
    CHECK(mu_attach_blank(0u, 1u, FATX_MIN_IMAGE, false, 0u));
    CHECK_EQ_U32(xinput_devices_get(TYPE), 0x10000u); changes(0u, 0u);
    CHECK_EQ_U32(load(0x46C75Cu), 0u);
    /* Reset adapter preserves explicitly attached policy, init reports insertion. */
    xinput_devices_reset(); memset(kernel_guest_at(0x46C000u, 0x1000u), 0, 0x1000u);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u); changes(0x10000u, 0u);
    mu_reset(); changes(0u, 0x10000u); CHECK_EQ_U32(mu_mounted_mask(), 0u);
    xinput_devices_reset(); environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
