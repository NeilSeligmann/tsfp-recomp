/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_memory.h"
#include "kernel_pool.h"
#ifdef T1146_FULL_THREAD
#include "kernel_thread.h"
#endif
#include "xbe.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include "probe_cache_wrap.h"

static kernel_guest_ptr scratch, frame_memory;
static uint32_t invoke(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    assert(kernel_frame_build(&frame, frame_memory, 128u, args, count));
    return kernel_hle_call(ordinal, &frame);
}
static uint32_t query(kernel_guest_ptr pointer)
{
    return invoke(181u, &pointer, 1u);
}
static void snap(uint32_t out[9])
{
    kernel_guest_ptr address = scratch + 256u;
    assert(kernel_guest_write_u32(address, 36u));
    assert(query(address) == STATUS_SUCCESS);
    assert(kernel_guest_read_bytes(address, out, 36u));
}
int main(void)
{
    kernel_hle_init();
    assert(kernel_memory_register() == 16u);
    assert(kernel_pool_register() != 0u);
    guest_region_request req = {.bytes=8192u, .protect=PAGE_READWRITE,
                               .state=MEM_COMMIT, .contiguous=true};
    nt_status status;
    scratch = guest_region_alloc(&req, &status);
    assert(scratch && status == STATUS_SUCCESS);
    frame_memory = guest_region_alloc(&req, &status); assert(frame_memory);
    uint32_t invalid[] = {0u, 1u, 4u, 35u, 37u, 40u, 0xA5A5A5A5u, UINT32_MAX};
    for (size_t i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        unsigned char before[96], after[96];
        memset(before, 0xA5, sizeof(before));
        memcpy(before+16, &invalid[i], 4u);
        assert(kernel_guest_write_bytes(scratch+512u, before, sizeof(before)));
        assert(query(scratch+528u) == STATUS_INVALID_PARAMETER);
        assert(kernel_guest_read_bytes(scratch+512u, after, sizeof(after)));
        assert(memcmp(before, after, sizeof(before)) == 0);
    }
    /* Unaligned success, whole-buffer guards and input-only Length. */
    for (unsigned offset=0; offset<4u; ++offset) {
        unsigned char before[96], after[96]; uint32_t length=36u;
        memset(before, 0xA5, sizeof(before)); memcpy(before+16u+offset, &length, 4u);
        assert(kernel_guest_write_bytes(scratch+512u, before, 96u));
        assert(query(scratch+528u+offset) == STATUS_SUCCESS);
        assert(kernel_guest_read_bytes(scratch+512u, after, 96u));
        assert(memcmp(before, after, 20u+offset) == 0);
        assert(memcmp(before+52u+offset, after+52u+offset, 44u-offset) == 0);
    }
    /* Invalid Length requires no suffix access. Valid Length leaves ordered prefix
     * on every page-boundary fault, using genuine mprotect rather than a fake writer. */
    for (unsigned prefix=0; prefix<8u; ++prefix) {
        uint32_t saved[9]; snap(saved);
        kernel_guest_ptr at=scratch+4096u-4u*(prefix+1u);
        memset((void *)(uintptr_t)at, 0xA5, 36u);
        assert(kernel_guest_write_u32(at, 36u));
        assert(mprotect((void *)(uintptr_t)(scratch+4096u),4096u,PROT_NONE)==0);
        assert(query(at)==STATUS_ACCESS_VIOLATION);
        uint32_t first; assert(kernel_guest_read_u32(at,&first) && first==36u);
        /* Physical free pages can change asynchronously; validate the prefix's
         * nonvolatile fields, and prove every expected store occurred. */
        for (unsigned j=0; j<prefix; ++j) {
            uint32_t got; assert(kernel_guest_read_u32(at+4u+4u*j,&got));
            assert(got != 0xA5A5A5A5u);
            if (j>=2u) assert(got==saved[j+1u]);
        }
        assert(mprotect((void *)(uintptr_t)(scratch+4096u),4096u,PROT_READ|PROT_WRITE)==0);
        for (unsigned j=prefix; j<8u; ++j) {
            uint32_t got; assert(kernel_guest_read_u32(at+4u+4u*j,&got));
            assert(got==0xA5A5A5A5u);
        }
        assert(kernel_guest_write_u32(at,35u));
        assert(mprotect((void *)(uintptr_t)(scratch+4096u),4096u,PROT_NONE)==0);
        assert(query(at)==STATUS_INVALID_PARAMETER);
        assert(mprotect((void *)(uintptr_t)(scratch+4096u),4096u,PROT_READ|PROT_WRITE)==0);
    }
    /* Readable-but-read-only suffix permits a subword write in process_vm_writev.
     * PROT_NONE instead refuses the whole DWORD in kernel_guest_at's read probe.
     * Record both, without claiming original exception equivalence. */
    for (unsigned mode=0u; mode<2u; ++mode) for (unsigned bytes=1u; bytes<4u; ++bytes) {
        uint32_t saved[9]; snap(saved);
        kernel_guest_ptr at=scratch+4096u-4u-bytes;
        memset((void *)(uintptr_t)at,0xA5,36u);
        assert(kernel_guest_write_u32(at,36u));
        assert(mprotect((void *)(uintptr_t)(scratch+4096u),4096u,mode ? PROT_READ : PROT_NONE)==0);
        assert(query(at)==STATUS_ACCESS_VIOLATION);
        assert(mprotect((void *)(uintptr_t)(scratch+4096u),4096u,PROT_READ|PROT_WRITE)==0);
        unsigned char expected[32]; memset(expected,0xA5,32u);
        if (mode) memcpy(expected,&saved[1],bytes);
        assert(memcmp((void *)(uintptr_t)(at+4u),expected,32u)==0);
    }
    /* Length on a read-only page, all outputs on the next writable page. */
    assert(kernel_guest_write_u32(scratch+4092u,36u));
    assert(mprotect((void *)(uintptr_t)scratch,4096u,PROT_READ)==0);
    assert(query(scratch+4092u)==STATUS_SUCCESS);
    assert(mprotect((void *)(uintptr_t)scratch,4096u,PROT_READ|PROT_WRITE)==0);
    /* Read-only Length is legal even though subsequent stores fault. */
    assert(kernel_guest_write_u32(scratch+256u,36u));
    assert(mprotect((void *)(uintptr_t)scratch,4096u,PROT_READ)==0);
    assert(query(scratch+256u)==STATUS_ACCESS_VIOLATION);
    assert(mprotect((void *)(uintptr_t)scratch,4096u,PROT_READ|PROT_WRITE)==0);
    assert(query(0u)==STATUS_ACCESS_VIOLATION);
    assert(query(UINT32_MAX-1u)==STATUS_ACCESS_VIOLATION);
    /* Argument slot aliases output: handler consumes pointer before any store. */
    kernel_call_frame alias;
    uint32_t arg=scratch+4u;
    assert(kernel_frame_build(&alias,scratch,128u,&arg,1u));
    assert(kernel_guest_write_u32(scratch+4u,36u));
    /* Actual alias uses Length at stack return word and argument at +4. */
    assert(kernel_guest_write_u32(scratch,36u));
    assert(kernel_guest_write_u32(scratch+4u,scratch));
    assert(kernel_hle_call(181u,&alias)==STATUS_SUCCESS);
    uint32_t base[9], now[9]; snap(base);
    struct sysinfo physical; assert(sysinfo(&physical)==0);
    assert(base[1]==((uint64_t)physical.totalram*physical.mem_unit)/4096u);
    assert(base[2]<=base[1] && base[5]==0u && base[8]==0u);
    /* Reserve/commit are the allocator's actual rounded commitments. */
    req.contiguous=false; req.bytes=12345u; req.state=MEM_RESERVE;
    kernel_guest_ptr region=guest_region_alloc(&req,&status); assert(region);
    uint32_t actual=guest_region_at(region)->size;
    snap(now); assert(now[3]==base[3] && now[4]==base[4]+actual);
    assert(guest_region_set_state(region,MEM_COMMIT));
    snap(now); assert(now[3]==base[3]+actual && now[4]==base[4]+actual);
    assert(!guest_region_set_usage(region+4096u,actual,GUEST_MEMORY_CACHE));
    assert(!guest_region_set_usage(region,actual-4096u,GUEST_MEMORY_CACHE));
    assert(guest_region_set_usage(region,actual,GUEST_MEMORY_CACHE));
    snap(now); assert(now[5]==base[5]+actual/4096u && now[3]==base[3] && now[4]==base[4]);
    assert(guest_region_set_usage(region,actual,GUEST_MEMORY_VIRTUAL));
    assert(guest_region_free(region)); snap(now); assert(now[3]==base[3] && now[4]==base[4]);
    uint32_t stackargs[]={8192u,0u};
    uint32_t top=invoke(169u,stackargs,2u); assert(top);
    snap(now); assert(now[7]==base[7]+2u && now[3]==base[3] && now[4]==base[4]);
    uint32_t freestack[]={top,top-8192u}; invoke(170u,freestack,2u);
    snap(now); assert(now[7]==base[7]);
    uint64_t pool_mapping_before=guest_mem_mapped_bytes();
    uint32_t poolargs[]={4352u,0x1146u};
    uint32_t block=invoke(15u,poolargs,2u); assert(block);
    snap(now); assert(now[6]>base[6] && now[3]==base[3] && now[4]==base[4]);
    assert(now[6]-base[6]==(guest_mem_mapped_bytes()-pool_mapping_before)/4096u);
    uint32_t retained=now[6]; invoke(17u,&block,1u); snap(now); assert(now[6]==retained);
    kernel_pool_reset(); snap(now); assert(now[6]==base[6]);
#ifdef T1146_FULL_THREAD
    uint32_t stackbytes; kernel_thread_stack layout;
    assert(kernel_thread_stack_region_bytes(8192u,&stackbytes));
    req.bytes=stackbytes; req.state=MEM_COMMIT;
    kernel_guest_ptr stackregion=guest_region_alloc(&req,&status); assert(stackregion);
    assert(kernel_thread_stack_layout(&layout,stackregion,8192u));
    snap(now); assert(now[7]==base[7]+stackbytes/4096u && now[3]==base[3] && now[4]==base[4]);
    assert(guest_region_free(stackregion)); snap(now); assert(now[7]==base[7]);
#endif
    /* Real loader mapping, not a caller-supplied page counter. */
    unsigned char data[4096]={0};
    xbe_image image={.base_address=0x10000000u,.size_of_image=12288u,.size_of_headers=4096u};
    assert(xbe_map(&image,data,sizeof(data))==XBE_OK);
    snap(now); assert(now[8]==3u && now[3]==base[3]+12288u && now[4]==base[4]+12288u);
    xbe_unmap(&image); snap(now); assert(now[8]==0u && now[3]==base[3] && now[4]==base[4]);
    guest_mem_reset();
    puts("T1146 actual dispatcher Length/guards/alias/fault/backing/loader controls passed");
    return 0;
}
