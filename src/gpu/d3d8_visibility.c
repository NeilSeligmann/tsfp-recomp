/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_visibility.h"
#include "d3d8_guest.h"
#include "d3d8_gpu.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "guest_mem.h"
#include "kernel_call.h"
#define PAGE_TABLE 0x7D4u
#define QUERY_LIMIT 0x4000u
#define PENDING 0x88760828u
/* T1246: the live renderer completes a frame's reports on its own thread when it runs one frame behind the guest. Every entry that
 * reads or rewrites a report slot first waits until no queued frame still has a report to write into THAT slot (the hook gets the
 * slot's guest address), so the guest sees a report exactly when the serial renderer made it visible (at the Swap that submitted
 * it). NULL when the renderer is serial. */
static void (*drain_hook)(uint32_t slot_address);
void d3d8_visibility_set_drain_hook(void (*hook)(uint32_t slot_address))
{
 drain_hook=hook;
}
static void drain(uint32_t slot_address)
{
 if(drain_hook != NULL) drain_hook(slot_address);
}
static uint32_t read_word(uint32_t entry,uint32_t address)
{
 uint32_t value;
 if(!kernel_guest_read_bytes(address,&value,sizeof value)) d3d8_hle_fatal(entry,"visibility read is unmapped");
 return value;
}
static void write_span(uint32_t entry,uint32_t address,const void *data,size_t bytes)
{
 if(!kernel_guest_write_bytes(address,data,bytes)) d3d8_hle_fatal(entry,"visibility write is unmapped");
}
static uint32_t page_word(uint32_t index,uint32_t entry)
{
 if(index >= QUERY_LIMIT) d3d8_hle_fatal(entry,"visibility index exceeds recovered 16384-slot device policy");
 return D3D8_DEVICE_BASE+PAGE_TABLE+(index>>8u)*4u;
}
static uint32_t emit(uint32_t entry,uint32_t method,uint32_t first,uint32_t second)
{
 d3d8_pushbuffer_sim sim=d3d8_pushbuffer_sim_start();
 const uint32_t cursor=d3d8_pushbuffer_sim_site(&sim,entry,12u);
 uint32_t old[3];
 if(!kernel_guest_read_bytes(cursor,old,sizeof old) || !kernel_guest_write_bytes(cursor,old,sizeof old))
  d3d8_hle_fatal(entry,"visibility packet output span is unmapped");
 if(d3d8_pushbuffer_begin() != cursor) d3d8_hle_fatal(entry,"visibility refill disagrees with preview");
 const uint32_t packet[3]={method,first,second};
 write_span(entry,cursor,packet,sizeof packet);
 d3d8_pushbuffer_end(cursor+12u);
 return cursor+12u;
}
uint32_t d3d8_visibility_begin(void)
{
 return emit(0x3D4250u,0x817C8u,1u,1u);
}
uint32_t d3d8_visibility_slot(uint32_t index)
{
 const uint32_t entry=0x3D4280u;
 const uint32_t table=page_word(index,entry);
 uint32_t page=read_word(entry,table);
 if(page == 0u) {
  const guest_region_request request={.bytes=4096u,.alignment=4096u,.highest_physical=0xFFFFFFu,
   .protect=PAGE_READWRITE,.state=MEM_COMMIT,.contiguous=true};
  nt_status status;
  page=guest_region_alloc(&request,&status);
  write_span(entry,table,&page,sizeof page);
 }
 if(page == 0u) return 0u;
 const uint32_t slot=page+(index&255u)*16u;
 if(slot < page) d3d8_hle_fatal(entry,"visibility slot address wraps");
 drain(slot);
 if((read_word(entry,D3D8_DEVICE_BASE+8u)&4u) == 0u) {
  const uint32_t pending=UINT32_MAX;
  write_span(entry,slot+12u,&pending,sizeof pending);
 }
 return slot;
}
uint32_t d3d8_visibility_end(uint32_t index)
{
 const uint32_t entry=0x3D42F0u;
 const uint32_t slot=d3d8_visibility_slot(index);
 if(slot == 0u) return 0x8007000Eu;
 const uint32_t physical=guest_physical_address(slot);
 if(physical == 0u || physical > 0xFFFFF0u || (physical&15u) != 0u)
  d3d8_hle_fatal(entry,"visibility notifier is outside aligned 16MiB report DMA");
 const uint32_t fence=read_word(entry,D3D8_DEVICE_BASE+0x2Cu);
 write_span(entry,slot,&fence,sizeof fence);
 (void)emit(entry,0x817CCu,0u,0x01000000u|physical);
 return 0u;
}
uint32_t d3d8_visibility_result(uint32_t index,uint32_t count,uint32_t timestamp)
{
 const uint32_t entry=0x3D34C0u;
 const uint32_t page=read_word(entry,page_word(index,entry));
 const uint32_t slot=page+(index&255u)*16u;
 drain(slot);
 const uint32_t done=read_word(entry,slot+12u);
 if(done == UINT32_MAX) {
  if(read_word(entry,slot) == read_word(entry,D3D8_DEVICE_BASE+0x2Cu)) (void)d3d8_gpu_fence_insert(0u);
  return PENDING;
 }
 const uint32_t samples=read_word(entry,slot+8u);
 write_span(entry,count,&samples,sizeof samples);
 if(timestamp != 0u) {
  uint32_t time=read_word(entry,slot);
  write_span(entry,timestamp,&time,sizeof time);
  time=read_word(entry,slot+4u);
  write_span(entry,timestamp+4u,&time,sizeof time);
 }
 return 0u;
}
bool d3d8_visibility_complete(uint32_t offset,uint32_t samples,uint64_t timestamp)
{
 if(offset > 0xFFFFF0u || (offset&15u) != 0u) return false;
 kernel_guest_ptr address;
 if(!guest_virtual_from_physical(offset,&address)) return false;
 bool registered=false;
 for(uint32_t page=0u;page<QUERY_LIMIT/256u;page++) {
  uint32_t base;
  if(!kernel_guest_read_bytes(D3D8_DEVICE_BASE+PAGE_TABLE+page*4u,&base,sizeof base)) return false;
  if(base != 0u && address >= base && (uint64_t)address+16u <= (uint64_t)base+4096u) { registered=true; break; }
 }
 if(!registered) return false;
 const uint32_t payload[3]={(uint32_t)timestamp,(uint32_t)(timestamp>>32u),samples};
 const uint32_t done=0u;
 return kernel_guest_write_bytes(address,payload,sizeof payload) && kernel_guest_write_bytes(address+12u,&done,sizeof done);
}
bool d3d8_visibility_complete_identity(uint32_t offset,uint32_t expected,uint64_t generation,uint32_t samples,uint64_t timestamp)
{
 kernel_guest_ptr address;
 if(generation == 0u || !guest_virtual_from_physical(offset,&address) || address != expected ||
    guest_allocation_generation(address) != generation) return false;
 return d3d8_visibility_complete(offset,samples,timestamp);
}
static uint32_t begin_handler(void *context) { (void)context; return d3d8_visibility_begin(); }
static uint32_t end_handler(void *context)
{
 uint32_t index;
 if(!kernel_frame_arg(context,0u,&index)) d3d8_hle_fatal(0x3D42F0u,"visibility index argument unreadable");
 return d3d8_visibility_end(index);
}
static uint32_t result_handler(void *context)
{
 uint32_t index,count,time;
 if(!kernel_frame_arg(context,0u,&index) || !kernel_frame_arg(context,1u,&count) || !kernel_frame_arg(context,2u,&time))
  d3d8_hle_fatal(0x3D34C0u,"visibility result arguments unreadable");
 return d3d8_visibility_result(index,count,time);
}
size_t d3d8_visibility_register(void)
{
 return (size_t)d3d8_hle_register(0x3D4250u,begin_handler)+(size_t)d3d8_hle_register(0x3D42F0u,end_handler)+
  (size_t)d3d8_hle_register(0x3D34C0u,result_handler);
}
