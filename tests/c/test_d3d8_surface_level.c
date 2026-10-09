/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_cube_surface.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
#define TEXTURE 0x00665504u
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_cube_surface_reset();
    map_fixed(0x00665000u, 0x1000u);
    store_byte(0x003E182Fu, 0xA1u);
    const uint32_t words[8] = {0x40001u,0x01234000u,0u,0x0661072Du,0u,0u,0xAAAAAAAAu,0xBBBBBBBBu};
    for (unsigned i = 0; i < 8; i++) store(TEXTURE + 4u * i, words[i]);
}
static void valid(void)
{
    initialise();
    const d3d8_surface_entry rows[] = {{0x003D4E10u,NULL,1u},{0x003D4E90u,NULL,1u}};
    CHECK(d3d8_hle_init(rows,2u));
    CHECK_EQ_U32(d3d8_cube_surface_register(),2u);
    const uint32_t args[] = {TEXTURE,1u};
    const uint32_t header = call_stdcall(0x003D4E10u,args,2u);
    CHECK(header != 0u);
    const uint32_t expected[] = {0x01050001u,0x01238000u,0u,0x0551072Du,0u,TEXTURE};
    for (unsigned i = 0; i < 6; i++) CHECK_EQ_U32(load(header+4u*i),expected[i]);
    CHECK(d3d8_cube_surface_owned(header));
    CHECK_EQ_U32(load(TEXTURE),0x40002u);
    CHECK_EQ_U32(load(TEXTURE+24u),0xAAAAAAAAu);
    CHECK(d3d8_cube_surface_free_owned(header));
    CHECK(d3d8_cube_surface_retired(header));
    CHECK(!d3d8_cube_surface_owned(header));
    uint8_t retired_before[24];
    memcpy(retired_before,kernel_guest_at(header,24u),sizeof(retired_before));
    RUN_EXPECTING_FATAL((void)d3d8_get_surface_level2(header,0u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address,0x003D4E10u);
    CHECK(memcmp(retired_before,kernel_guest_at(header,24u),sizeof(retired_before))==0);
    CHECK(d3d8_cube_surface_retired(header));
    CHECK_EQ_U32(load(TEXTURE),0x40002u);
    store(TEXTURE+16u,0x0303F03Fu);
    const uint32_t linear = d3d8_get_surface_level2(TEXTURE,UINT32_MAX);
    CHECK(linear != 0u);
    CHECK_EQ_U32(load(linear+4u),0x01234000u);
    CHECK_EQ_U32(load(linear+12u),0x0661072Du);
    d3d8_cube_surface_reset();
    environment_end();
}
static void refusal(void)
{
    initialise();
    uint8_t before[32]; memcpy(before,kernel_guest_at(TEXTURE,32u),32u);
    CHECK(mprotect((void *)(uintptr_t)0x00665000u,0x1000u,PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)d3d8_get_surface_level2(TEXTURE,1u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address,0x003D4E10u);
    CHECK(memcmp(before,kernel_guest_at(TEXTURE,32u),32u)==0);
    CHECK(mprotect((void *)(uintptr_t)0x00665000u,0x1000u,PROT_READ|PROT_WRITE)==0);
    CHECK(mprotect((void *)(uintptr_t)0x003E1000u,0x1000u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_get_surface_level2(TEXTURE,1u)); CHECK(fatal_seen);
    CHECK(memcmp(before,kernel_guest_at(TEXTURE,32u),32u)==0);
    CHECK(mprotect((void *)(uintptr_t)0x003E1000u,0x1000u,PROT_READ|PROT_WRITE)==0);
    store(TEXTURE,0x50000u); store(TEXTURE+20u,1u);
    RUN_EXPECTING_FATAL((void)d3d8_get_surface_level2(TEXTURE,0u)); CHECK(fatal_seen);
    CHECK_EQ_U32(load(TEXTURE),0x50000u);
    store(TEXTURE,0x40001u); store(TEXTURE+20u,0u);
    RUN_EXPECTING_FATAL((void)d3d8_get_surface_level2(UINT32_MAX-15u,0u)); CHECK(fatal_seen);
    store(0x00665FECu,0x50000u);
    store(0x00665FF0u,0u);
    store(0x00665FF4u,0u);
    store(0x00665FF8u,0x0661072Du);
    store(0x00665FFCu,0u);
    RUN_EXPECTING_FATAL((void)d3d8_get_surface_level2(0x00665FECu,0u));
    CHECK(fatal_seen); CHECK_EQ_U32(load(0x00665FECu),0x50000u);
    /* If refusal allocated a hidden heap, exhausting all available slots would
     * still permit this module to allocate. It must instead return NULL. */
    uint32_t heaps[256], count=0u;
    while (count<256u) { uint32_t heap=guest_heap_create(0u,0u,0u); if (!heap) break; heaps[count++]=heap; }
    CHECK(count>0u);
    CHECK_EQ_U32(d3d8_get_surface_level2(TEXTURE,0u),0u);
    CHECK_EQ_U32(load(TEXTURE),0x40001u);
    for (unsigned i=0u;i<count;i++) CHECK(guest_heap_destroy(heaps[i]));
    CHECK(d3d8_get_surface_level2(TEXTURE,0u)!=0u);
    d3d8_cube_surface_reset(); environment_end();
}
int main(void)
{
    valid(); refusal();
    printf("%d checks, %d failures\n",checks,failures);
    return failures != 0;
}
