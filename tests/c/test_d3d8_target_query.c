/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_target_query.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
#define HEADER 0x00D00000u
#define PARENT 0x00D01000u
#define GRAND 0x00D02000u
static void begin(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(HEADER, 0x4000u);
    store(0x003E3F58u, D3D8_DEVICE_BASE);
    store(D3D8_DEVICE_BASE + 0x1A04u, HEADER);
    store(D3D8_DEVICE_BASE + 0x1A08u, PARENT);
    store(HEADER, 0x010D0003u);
    store(PARENT, 0x010D0002u);
}
static void test_values_and_null(void)
{
    begin();
    CHECK_EQ_U32(d3d8_get_render_target2(), HEADER);
    CHECK_EQ_U32(load(HEADER), 0x010D0004u);
    CHECK_EQ_U32(load(PARENT), 0x010D0002u);
    CHECK_EQ_U32(d3d8_get_depth_stencil_surface2(), PARENT);
    CHECK_EQ_U32(load(PARENT), 0x010D0003u);
    store(D3D8_DEVICE_BASE + 0x1A04u, 0u);
    store(D3D8_DEVICE_BASE + 0x1A08u, 0u);
    CHECK_EQ_U32(d3d8_get_render_target2(), 0u);
    CHECK_EQ_U32(d3d8_get_depth_stencil_surface2(), 0u);
    store(D3D8_DEVICE_BASE + 0x1A04u, HEADER);
    store(HEADER, UINT32_MAX);
    CHECK_EQ_U32(d3d8_get_render_target2(), HEADER);
    CHECK_EQ_U32(load(HEADER), 0u);
    environment_end();
}
static void test_parent_permissions(void)
{
    begin();
    store(HEADER, 0x50000u); store(HEADER + 20u, PARENT);
    store(PARENT, 0x50000u); store(PARENT + 20u, GRAND);
    store(GRAND, 0x40001u);
    uint8_t before[0x3000];
    memcpy(before, kernel_guest_at(HEADER, sizeof before), sizeof before);
    CHECK(mprotect((void *)(uintptr_t)PARENT, 0x1000u, PROT_READ) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_get_render_target2());
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(HEADER), 0x50000u);
    CHECK_EQ_U32(load(PARENT), 0x50000u);
    CHECK_EQ_U32(load(GRAND), 0x40001u);
    CHECK(memcmp(before, kernel_guest_at(HEADER, sizeof before), sizeof before) == 0);
    CHECK(mprotect((void *)(uintptr_t)PARENT, 0x1000u, PROT_READ | PROT_WRITE) == 0);
    CHECK(mprotect((void *)(uintptr_t)GRAND, 0x1000u, PROT_NONE) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_get_render_target2());
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(HEADER), 0x50000u);
    CHECK_EQ_U32(load(PARENT), 0x50000u);
    CHECK(mprotect((void *)(uintptr_t)GRAND, 0x1000u, PROT_READ | PROT_WRITE) == 0);
    CHECK_EQ_U32(load(GRAND), 0x40001u);
    CHECK(memcmp(before, kernel_guest_at(HEADER, sizeof before), sizeof before) == 0);
    CHECK_EQ_U32(d3d8_get_render_target2(), HEADER);
    CHECK_EQ_U32(load(HEADER), 0x50001u);
    CHECK_EQ_U32(load(PARENT), 0x50001u);
    CHECK_EQ_U32(load(GRAND), 0x40002u);
    environment_end();
}
static void test_guarded_binding_and_header(void)
{
    begin();
    uint8_t before[0x30000];
    memcpy(before, kernel_guest_at(0x003D0000u, sizeof before), sizeof before);
    CHECK(mprotect((void *)(uintptr_t)HEADER, 0x1000u, PROT_READ) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_get_render_target2());
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(HEADER), 0x010D0003u);
    CHECK(memcmp(before, kernel_guest_at(0x003D0000u, sizeof before), sizeof before) == 0);
    CHECK(mprotect((void *)(uintptr_t)HEADER, 0x1000u, PROT_NONE) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_get_render_target2());
    CHECK(fatal_seen);
    CHECK(mprotect((void *)(uintptr_t)HEADER, 0x1000u, PROT_READ | PROT_WRITE) == 0);
    CHECK_EQ_U32(load(HEADER), 0x010D0003u);
    CHECK(mprotect((void *)(uintptr_t)0x003E5000u, 0x1000u, PROT_NONE) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_get_depth_stencil_surface2());
    CHECK(fatal_seen);
    CHECK(mprotect((void *)(uintptr_t)0x003E5000u, 0x1000u, PROT_READ | PROT_WRITE) == 0);
    CHECK_EQ_U32(load(PARENT), 0x010D0002u);
    CHECK(mprotect((void *)(uintptr_t)0x003E3000u, 0x1000u, PROT_NONE) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_get_render_target2()); CHECK(fatal_seen);
    CHECK(mprotect((void *)(uintptr_t)0x003E3000u, 0x1000u, PROT_READ | PROT_WRITE) == 0);
    CHECK(memcmp(before, kernel_guest_at(0x003D0000u, sizeof before), sizeof before) == 0);
    CHECK_EQ_U32(load(HEADER), 0x010D0003u);
    store(D3D8_DEVICE_BASE + 0x1A04u, UINT32_MAX - 1u);
    RUN_EXPECTING_FATAL((void)d3d8_get_render_target2()); CHECK(fatal_seen);
    CHECK_EQ_U32(load(HEADER), 0x010D0003u);
    store(D3D8_DEVICE_BASE + 0x1A04u, HEADER);
    store(0x003E3F58u, 0u);
    RUN_EXPECTING_FATAL((void)d3d8_get_render_target2()); CHECK(fatal_seen);
    store(0x003E3F58u, UINT32_MAX - 4u);
    RUN_EXPECTING_FATAL((void)d3d8_get_depth_stencil_surface2()); CHECK(fatal_seen);
    CHECK_EQ_U32(load(HEADER), 0x010D0003u);
    environment_end();
}
int main(void)
{
    test_values_and_null(); test_parent_permissions(); test_guarded_binding_and_header();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
