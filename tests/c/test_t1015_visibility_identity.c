/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_gpu.h"
#include "d3d8_visibility.h"

#define OUTPUT 0x00D00000u

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(OUTPUT, 4096u);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_gpu_create());
    CHECK(d3d8_pushbuffer_create());

    const uint32_t begin_cursor = load(D3D8_DEVICE_BASE);
    CHECK_EQ_U32(d3d8_visibility_begin(), begin_cursor + 12u);
    CHECK_EQ_U32(d3d8_visibility_end(0u), 0u);
    const uint32_t page = load(D3D8_DEVICE_BASE + 0x7D4u);
    const uint32_t physical = guest_physical_address(page);
    const uint64_t old_generation = guest_allocation_generation(page);
    CHECK(page != 0u && physical != 0u && old_generation != 0u);

    store(OUTPUT, 0xDEADBEEFu);
    store(OUTPUT + 4u, 0xFEEDF00Du);
    store(OUTPUT + 8u, 0xBAADF00Du);
    CHECK_EQ_U32(d3d8_visibility_result(0u, OUTPUT, OUTPUT + 4u), 0x88760828u);
    CHECK_EQ_U32(load(OUTPUT), 0xDEADBEEFu);
    CHECK_EQ_U32(load(OUTPUT + 4u), 0xFEEDF00Du);
    CHECK_EQ_U32(load(OUTPUT + 8u), 0xBAADF00Du);

    CHECK(guest_region_free(page));
    const guest_region_request replacement = {
        .bytes = 4096u,
        .lowest_physical = physical & ~4095u,
        .highest_physical = (physical & ~4095u) + 4095u,
        .alignment = 4096u,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .fixed_base = page,
        .contiguous = true,
    };
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr reused_page = guest_region_alloc(&replacement, &status);
    CHECK(reused_page == page && status == STATUS_SUCCESS);
    CHECK(guest_physical_address(reused_page) == (physical & ~4095u));
    const uint64_t new_generation = guest_allocation_generation(reused_page);
    CHECK(new_generation > old_generation);

    store(page, 0xA1B2C3D4u);
    store(page + 4u, 0x11223344u);
    store(page + 8u, 0x55667788u);
    store(page + 12u, UINT32_MAX);
    CHECK(!d3d8_visibility_complete_identity(physical, page, old_generation, 123u,
                                             UINT64_C(0x8877665544332211)));
    CHECK_EQ_U32(load(page), 0xA1B2C3D4u);
    CHECK_EQ_U32(load(page + 4u), 0x11223344u);
    CHECK_EQ_U32(load(page + 8u), 0x55667788u);
    CHECK_EQ_U32(load(page + 12u), UINT32_MAX);

    CHECK_EQ_U32(d3d8_visibility_result(0u, OUTPUT, OUTPUT + 4u), 0x88760828u);
    CHECK_EQ_U32(load(OUTPUT), 0xDEADBEEFu);
    CHECK_EQ_U32(load(OUTPUT + 4u), 0xFEEDF00Du);
    CHECK_EQ_U32(load(OUTPUT + 8u), 0xBAADF00Du);

    CHECK(d3d8_visibility_complete_identity(physical, page, new_generation, 246u,
                                            UINT64_C(0x1122334455667788)));
    CHECK_EQ_U32(load(page), 0x55667788u);
    CHECK_EQ_U32(load(page + 4u), 0x11223344u);
    CHECK_EQ_U32(load(page + 8u), 246u);
    CHECK_EQ_U32(load(page + 12u), 0u);
    CHECK_EQ_U32(d3d8_visibility_result(0u, OUTPUT, OUTPUT + 4u), 0u);
    CHECK_EQ_U32(load(OUTPUT), 246u);
    CHECK_EQ_U32(load(OUTPUT + 4u), 0x55667788u);
    CHECK_EQ_U32(load(OUTPUT + 8u), 0x11223344u);
    environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
