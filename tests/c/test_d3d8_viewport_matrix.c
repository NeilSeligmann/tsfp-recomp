/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_viewport_matrix.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static void identity(uint32_t address)
{
    for(unsigned i=0u;i<16u;i++)store(address+i*4u,i%5u==0u?0x3F800000u:0u);
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    identity(0x3D8000u);identity(0x3D8100u);
    CHECK_EQ_U32(d3d8_multiply_matrix(0x3D8000u,0x3D8000u,0x3D8100u),0x3D8000u);
    for(unsigned i=0u;i<16u;i++)CHECK_EQ_U32(load(0x3D8000u+i*4u),i%5u==0u?0x3F800000u:0u);
    RUN_EXPECTING_FATAL((void)d3d8_multiply_matrix(0x3D8001u,0x3D8000u,0x3D8100u));
    CHECK(fatal_seen);CHECK_EQ_U32(load(0x3D8000u),0x3F800000u);
    RUN_EXPECTING_FATAL((void)d3d8_multiply_matrix(0x3D8000u,0xFFFFfff0u,0x3D8100u));
    CHECK(fatal_seen);CHECK_EQ_U32(load(0x3D8000u),0x3F800000u);
    store(0x3D8100u,0x7FA12345u);
    RUN_EXPECTING_FATAL((void)d3d8_multiply_matrix(0x3D8000u,0x3D8000u,0x3D8100u));
    CHECK(fatal_seen);CHECK_EQ_U32(load(0x3D8000u),0x3F800000u);
    identity(0x3D8100u);
    CHECK_EQ_U32(d3d8_multiply_matrix(0x3D8010u,0x3D8000u,0x3D8100u),0x3D8000u);
    for(unsigned i=0u;i<16u;i++)CHECK_EQ_U32(load(0x3D8010u+i*4u),i%5u==0u?0x3F800000u:0u);
    for(unsigned i=0u;i<16u;i++)store(0x3D9000u+i*4u,0xDEADBEEFu);
    /* Translate while readable: kernel_guest_at refuses a PROT_NONE page (T81), so
     * the pointer must be held across the protection change, not re-derived. */
    void *matrix_page=kernel_guest_at(0x3D8000u,0x1000u);
    CHECK(matrix_page!=NULL&&mprotect(matrix_page,0x1000u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_multiply_matrix(0x3D9000u,0x3D8000u,0x3D8100u));CHECK(fatal_seen);
    for(unsigned i=0u;i<16u;i++)CHECK_EQ_U32(load(0x3D9000u+i*4u),0xDEADBEEFu);
    CHECK(mprotect(matrix_page,0x1000u,PROT_READ|PROT_WRITE)==0);
    store(0x3E3F58u,D3D8_DEVICE_BASE);identity(D3D8_DEVICE_BASE+0xCA0u);
    store(D3D8_DEVICE_BASE+0xEE8u,123u);store(D3D8_DEVICE_BASE+0xEECu,77u);
    store(D3D8_DEVICE_BASE+0x95Cu,0x3F800000u);store(D3D8_DEVICE_BASE+0x960u,0x3F800000u);
    store(D3D8_DEVICE_BASE+0x948u,0x3F800000u);store(D3D8_DEVICE_BASE+0xEF4u,0x3F800000u);
    store(0x475CCCu,0x4F800000u);store(0x475CD4u,0x3F000000u);store(0x4760C4u,0x3F000000u);store(0x475C78u,0x3F800000u);
    store(D3D8_GLOBAL_DIRTY_MASK,0x80000001u);
    CHECK_EQ_U32(d3d8_rebuild_viewport_matrix(),D3D8_DEVICE_BASE+0xCA0u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x980u),0x42760000u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x994u),0x421A0000u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x9B4u),0xC21A0000u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK),0x80000201u);
    uint8_t saved_cache[64];
    memcpy(saved_cache,kernel_guest_at(D3D8_DEVICE_BASE+0x980u,64u),64u);
    CHECK(mprotect(kernel_guest_at(0x3E3000u,0x1000u),0x1000u,PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)d3d8_rebuild_viewport_matrix());CHECK(fatal_seen);
    CHECK(memcmp(saved_cache,kernel_guest_at(D3D8_DEVICE_BASE+0x980u,64u),64u)==0);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK),0x80000201u);
    CHECK(mprotect(kernel_guest_at(0x3E3000u,0x1000u),0x1000u,PROT_READ|PROT_WRITE)==0);
    CHECK(mprotect(kernel_guest_at(0x3E4000u,0x1000u),0x1000u,PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)d3d8_rebuild_viewport_matrix());CHECK(fatal_seen);
    CHECK(memcmp(saved_cache,kernel_guest_at(D3D8_DEVICE_BASE+0x980u,64u),64u)==0);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK),0x80000201u);
    CHECK(mprotect(kernel_guest_at(0x3E4000u,0x1000u),0x1000u,PROT_READ|PROT_WRITE)==0);
    store(D3D8_DEVICE_BASE+0x95Cu,0x7FC12345u);
    RUN_EXPECTING_FATAL((void)d3d8_rebuild_viewport_matrix());CHECK(fatal_seen);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x980u),0x42760000u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK),0x80000201u);
    store(0x3E3F58u,0u);
    RUN_EXPECTING_FATAL((void)d3d8_rebuild_viewport_matrix());CHECK(fatal_seen);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK),0x80000201u);
    uint32_t saved_mxcsr;
    __asm__ volatile("stmxcsr %0":"=m"(saved_mxcsr));
    uint32_t changed_mxcsr=saved_mxcsr|0x8000u;
    __asm__ volatile("ldmxcsr %0": :"m"(changed_mxcsr));
    RUN_EXPECTING_FATAL((void)d3d8_multiply_matrix(0x3D8000u,0x3D8000u,0x3D8100u));
    __asm__ volatile("ldmxcsr %0": :"m"(saved_mxcsr));
    CHECK(fatal_seen);CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK),0x80000201u);
    environment_end();printf("viewport matrix: %d checks, %d failures\n",checks,failures);
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
