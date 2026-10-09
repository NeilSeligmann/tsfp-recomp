/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent finite host-architecture controls, not an Xbox physical-map oracle. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "guest_mem.h"
#include "kernel_call.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"FAIL %u %s\n",__LINE__,#x); } } while (0)
kernel_log_fn kernel_hle_log(void) { return printf; }
static kernel_guest_ptr request(uint32_t bytes,uint32_t low,uint32_t high,uint32_t align,nt_status *status)
{
    guest_region_request r={.bytes=bytes,.lowest_physical=low,.highest_physical=high,
        .alignment=align,.protect=PAGE_READWRITE,.state=MEM_COMMIT,.contiguous=true};
    return guest_region_alloc(&r,status);
}

/* Independent literal page occupancy model over a small constrained window.
 * It never calls allocator arithmetic/search helpers or injects region records. */
#define FLOOR 0x00200000u
#define PAGES 16u
static kernel_guest_ptr slots[PAGES];
static unsigned starts[PAGES], lengths[PAGES];
static uint32_t random_state=0x1048C72Au;
static uint32_t next(void) { random_state=random_state*1664525u+1013904223u; return random_state; }
static void occupancy_model(void)
{
    for(unsigned op=0;op<600;op++) {
        unsigned slot=(next()>>16)%PAGES;
        if(slots[slot]) { CHECK(guest_region_free(slots[slot])); slots[slot]=0; }
        if((next()&3u)==0) continue;
        unsigned count=1+(next()>>16)%4u;
        unsigned first=(next()>>16)%PAGES, end=first+1+(next()>>16)%(PAGES-first);
        unsigned alignment=(next()&1u)?4096u:16384u;
        uint32_t low=FLOOR+first*4096u+((next()&1u)?1u:0u);
        uint32_t high=FLOOR+end*4096u-1u-((next()&1u)?1u:0u);
        uint32_t expected=0;
        for(unsigned page=0;page<PAGES;page++) {
            uint32_t at=FLOOR+page*4096u;
            if(at<low || at%alignment || (uint64_t)at+count*4096u-1u>high) continue;
            bool occupied=false;
            for(unsigned i=0;i<PAGES;i++) if(slots[i] && page<starts[i]+lengths[i] && starts[i]<page+count) occupied=true;
            if(!occupied) { expected=at; break; }
        }
        nt_status status=0x12345678u;
        const uint64_t mapped=guest_mem_mapped_bytes(); const size_t live=guest_mem_region_count();
        kernel_guest_ptr got=request(count*4096u,low,high,alignment,&status);
        CHECK((got!=0)==(expected!=0));
        if(!got) { CHECK(status==STATUS_NO_MEMORY); CHECK(mapped==guest_mem_mapped_bytes()); CHECK(live==guest_mem_region_count()); continue; }
        CHECK(status==STATUS_SUCCESS); CHECK(guest_physical_address(got)==expected);
        CHECK(got%alignment==0); CHECK(guest_region_at(got)->size==count*4096u);
        CHECK(kernel_guest_write_u32(got,op^0xC0981732u));
        uint32_t value=0; CHECK(kernel_guest_read_u32(got,&value) && value==(op^0xC0981732u));
        slots[slot]=got;starts[slot]=(guest_physical_address(got)-FLOOR)/4096u;lengths[slot]=count;
        for(unsigned i=0;i<PAGES;i++) if(slots[i] && i!=slot) {
            CHECK(starts[i]+lengths[i]<=starts[slot] || starts[slot]+lengths[slot]<=starts[i]);
        }
    }
    for(unsigned i=0;i<PAGES;i++) if(slots[i]) { CHECK(guest_region_free(slots[i]));slots[i]=0; }
}

int main(int argc,char **argv)
{
    (void)argv; nt_status status;
    guest_mem_reset();
    kernel_guest_ptr high=request(4096u,0x10000000u,0x10000FFFu,4096u,&status);
    CHECK(high!=0 && status==STATUS_SUCCESS && guest_physical_address(high)==0x10000000u);
    kernel_guest_ptr notifier=request(4096u,0u,0x00FFFFFFu,4096u,&status);
    printf("notifier status=%08x physical=%08x live=%zu\n",status,guest_physical_address(notifier),guest_mem_region_count());
    if(argc>1) { CHECK(notifier==0 && status==STATUS_NO_MEMORY); guest_mem_reset();printf("BASELINE %u checks %u failures\n",checks,failures);return failures?1:0; }
    CHECK(notifier!=0 && status==STATUS_SUCCESS);
    if(!notifier) return 1;
    CHECK(guest_physical_address(notifier)>=4096u && (uint64_t)guest_physical_address(notifier)+4095u<=0xFFFFFFu);
    CHECK(guest_region_free(notifier));CHECK(guest_region_free(high));
    /* Inclusive endpoints and an unaligned minimum rounding to the next page. */
    kernel_guest_ptr a=request(4096u,0x003FFFFFu,0x00400FFFu,4096u,&status);
    CHECK(a!=0 && status==STATUS_SUCCESS && guest_physical_address(a)==0x00400000u);
    kernel_guest_ptr b=request(4096u,0x00400000u,0x00400FFFu,4096u,&status);
    CHECK(b==0 && status==STATUS_NO_MEMORY);
    CHECK(request(4096u,0x00500000u,0x00500FFEu,4096u,&status)==0 && status==STATUS_NO_MEMORY);
    CHECK(request(1u,0u,0xFFFu,4096u,&status)==0 && status==STATUS_NO_MEMORY);
    CHECK(request(4096u,0x6000u,0x5FFFu,4096u,&status)==0 && status==STATUS_INVALID_PARAMETER);
    CHECK(request(4096u,0u,0xFFFFFFu,12288u,&status)==0 && status==STATUS_INVALID_PARAMETER);
    CHECK(request(UINT32_MAX,0u,0xFFFFFFu,4096u,&status)==0 && status==STATUS_NO_MEMORY);
    CHECK(guest_region_free(a));
    /* Last byte UINT32_MAX is legal; highwater at 2^32 must fall back safely. */
    a=request(4096u,0xFFFFF000u,UINT32_MAX,4096u,&status);
    CHECK(a!=0 && status==STATUS_SUCCESS && guest_physical_address(a)==0xFFFFF000u);
    kernel_guest_ptr inverse=0xDEADBEEFu;
    CHECK(guest_virtual_from_physical(UINT32_MAX,&inverse) && inverse==a+4095u);
    b=request(4096u,0u,0u,4096u,&status);
    CHECK(b!=0 && status==STATUS_SUCCESS && guest_physical_address(b)==4096u);
    CHECK(guest_region_free(a));CHECK(guest_region_free(b));
    /* Reserved virtual regions also occupy synthetic physical extents. */
    guest_region_request reserve={.bytes=1u,.lowest_physical=0x00700000u,
        .highest_physical=0x0070FFFFu,.protect=PAGE_READWRITE,.state=MEM_RESERVE};
    a=guest_region_alloc(&reserve,&status);
    CHECK(a!=0 && guest_region_at(a)->size==65536u && !guest_region_at(a)->contiguous);
    CHECK(guest_physical_address(a)==0x00700000u);
    CHECK(request(4096u,0x00701000u,0x00701FFFu,4096u,&status)==0 && status==STATUS_NO_MEMORY);
    inverse=0xDEADBEEFu;
    CHECK(!guest_virtual_from_physical(0x00710000u,&inverse) && inverse==0xDEADBEEFu);
    CHECK(guest_region_free(a));
    /* Genuine positive probe cache must be invalidated when real mmap disappears. */
    a=request(4096u,0x00600000u,0x00600FFFu,4096u,&status);
    CHECK(a!=0);uint32_t value=0;CHECK(kernel_guest_write_u32(a,0x123ABCDu));
    CHECK(kernel_guest_read_u32(a,&value)); CHECK(kernel_guest_read_u32(a,&value));
    uint64_t hits=0,calls=0;kernel_guest_probe_stats(&hits,&calls);CHECK(hits>0 && calls>0);
#ifdef T1020_GENERATION
    uint64_t generation=guest_allocation_generation(a);CHECK(generation!=0);
#endif
    CHECK(guest_region_free(a));CHECK(!kernel_guest_read_u32(a,&value));
#ifdef T1020_GENERATION
    CHECK(guest_allocation_generation(a)==0);
#endif
    kernel_guest_ptr reused=request(4096u,0x00600000u,0x00600FFFu,4096u,&status);
    CHECK(reused!=0 && guest_physical_address(reused)==0x00600000u);
#ifdef T1020_GENERATION
    CHECK(guest_allocation_generation(reused)>generation);
    generation=guest_allocation_generation(reused);
#endif
    guest_mem_reset();CHECK(!kernel_guest_read_u32(reused,&value));
    reused=request(4096u,0x00600000u,0x00600FFFu,4096u,&status);
    CHECK(reused!=0);
#ifdef T1020_GENERATION
    CHECK(guest_allocation_generation(reused)>generation);
#endif
    CHECK(guest_region_free(reused));occupancy_model();
    CHECK(guest_mem_region_count()==0 && guest_mem_mapped_bytes()==0);
    guest_mem_reset();printf("CANDIDATE %u checks %u failures\n",checks,failures);return failures?1:0;
}
