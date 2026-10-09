/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_visibility.h"
#include "d3d8_gpu.h"
#define OUTPUT 0xD00000u
int main(void)
{
 environment_begin(KERNEL_AV_PACK_HDTV);
 map_fixed(OUTPUT,4096u);
 store(D3D8_GLOBAL_PUSHBUFFER_SIZE,0x100000u);
 store(D3D8_GLOBAL_KICKOFF_SIZE,0x10000u);
 CHECK(d3d8_gpu_create()); CHECK(d3d8_pushbuffer_create());
 uint32_t cursor=load(D3D8_DEVICE_BASE);
 CHECK_EQ_U32(d3d8_visibility_begin(),cursor+12u);
 CHECK_EQ_U32(load(cursor),0x817C8u);CHECK_EQ_U32(load(cursor+4u),1u);CHECK_EQ_U32(load(cursor+8u),1u);
 const uint32_t indices[]={0u,255u,256u,511u};
 for(unsigned i=0u;i<4u;i++) {
  uint32_t index=indices[i];
  cursor=load(D3D8_DEVICE_BASE);
  uint32_t fence=load(D3D8_DEVICE_BASE+0x2Cu);
  CHECK_EQ_U32(d3d8_visibility_end(index),0u);
  uint32_t page=load(D3D8_DEVICE_BASE+0x7D4u+(index>>8u)*4u);
  uint32_t slot=page+(index&255u)*16u;
  uint32_t physical=guest_physical_address(slot);
  CHECK(physical != 0u && physical <= 0xFFFFF0u);
  CHECK_EQ_U32(load(cursor),0x817CCu);CHECK_EQ_U32(load(cursor+4u),0u);CHECK_EQ_U32(load(cursor+8u),0x01000000u|physical);
  CHECK_EQ_U32(load(slot),fence);CHECK_EQ_U32(load(slot+12u),UINT32_MAX);
  store(OUTPUT,0xDEADBEEFu);store(OUTPUT+4u,0xFEEDF00Du);store(OUTPUT+8u,0xBADCAFEu);
  uint64_t inserted=d3d8_gpu_get_stats().fences_inserted;
  CHECK_EQ_U32(d3d8_visibility_result(index,OUTPUT,OUTPUT+4u),0x88760828u);
  CHECK_EQ_U32(load(OUTPUT),0xDEADBEEFu);CHECK_EQ_U32(load(OUTPUT+4u),0xFEEDF00Du);
  CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x2Cu),fence+2u);
  CHECK(d3d8_gpu_get_stats().fences_inserted == inserted+1u);
  CHECK_EQ_U32(d3d8_visibility_result(index,OUTPUT,0u),0x88760828u);
  CHECK(d3d8_gpu_get_stats().fences_inserted == inserted+1u);
  CHECK(!d3d8_visibility_complete(physical+4u,17u,0u));
  CHECK_EQ_U32(load(slot+12u),UINT32_MAX);
  uint64_t generation=guest_allocation_generation(slot);
  CHECK(generation != 0u);
  CHECK(!d3d8_visibility_complete_identity(physical,slot,generation+1u,1u,0u));
  CHECK_EQ_U32(load(slot+12u),UINT32_MAX);
  CHECK(d3d8_visibility_complete_identity(physical,slot,generation,15000u,UINT64_C(0x1122334455667788)));
  CHECK_EQ_U32(load(slot+12u),0u);
  CHECK_EQ_U32(d3d8_visibility_result(index,OUTPUT,OUTPUT+4u),0u);
  CHECK_EQ_U32(load(OUTPUT),15000u);CHECK_EQ_U32(load(OUTPUT+4u),0x55667788u);CHECK_EQ_U32(load(OUTPUT+8u),0x11223344u);
  CHECK_EQ_U32(d3d8_visibility_result(index,OUTPUT,slot+4u),0u);
  CHECK_EQ_U32(load(slot+4u),0x55667788u);
  CHECK_EQ_U32(load(slot+8u),0x55667788u);
  store(slot+12u,0xFFFFFFFEu);
  CHECK_EQ_U32(d3d8_visibility_result(index,OUTPUT,0u),0u);
  store(D3D8_DEVICE_BASE+8u,load(D3D8_DEVICE_BASE+8u)|4u);
  CHECK_EQ_U32(d3d8_visibility_slot(index),slot);CHECK_EQ_U32(load(slot+12u),0xFFFFFFFEu);
  store(D3D8_DEVICE_BASE+8u,load(D3D8_DEVICE_BASE+8u)&~4u);
 }
 CHECK(!d3d8_visibility_complete(0xFFFFFFu,0u,0u));
 CHECK(!d3d8_visibility_complete(0x00F00000u,0u,0u));
 RUN_EXPECTING_FATAL((void)d3d8_visibility_slot(16384u));CHECK(fatal_seen);
 uint32_t oldpage=load(D3D8_DEVICE_BASE+0x7D4u);
 uint32_t oldphysical=guest_physical_address(oldpage);
 uint64_t oldgeneration=guest_allocation_generation(oldpage);
 CHECK(guest_region_free(oldpage));
 const guest_region_request replacement={.bytes=4096u,.alignment=4096u,.highest_physical=0xFFFFFFu,
  .protect=PAGE_READWRITE,.state=MEM_COMMIT,.contiguous=true,.fixed_base=oldpage};
 nt_status replacement_status;
 uint32_t newpage=guest_region_alloc(&replacement,&replacement_status);
 CHECK_EQ_U32(newpage,oldpage);
 CHECK_EQ_U32(guest_physical_address(newpage),oldphysical);
 CHECK(guest_allocation_generation(newpage) != oldgeneration);
 store(D3D8_DEVICE_BASE+0x7D4u,newpage);
 store(newpage,0xDEADBEEFu);store(newpage+4u,0xFEEDF00Du);store(newpage+8u,0xBADCAFEu);
 store(newpage+12u,UINT32_MAX);
 CHECK(!d3d8_visibility_complete_identity(oldphysical,oldpage,oldgeneration,1u,0u));
 CHECK_EQ_U32(load(newpage+12u),UINT32_MAX);
 CHECK_EQ_U32(load(newpage),0xDEADBEEFu);CHECK_EQ_U32(load(newpage+4u),0xFEEDF00Du);CHECK_EQ_U32(load(newpage+8u),0xBADCAFEu);
 environment_end();
 printf("%d checks, %d failures\n",checks,failures);
 return failures != 0;
}
