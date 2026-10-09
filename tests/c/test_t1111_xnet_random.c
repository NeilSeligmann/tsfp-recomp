/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "xnet_random.h"
#include "xnet_hle.h"
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "xdk_thunk.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/mman.h>
#include <unistd.h>

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
static unsigned checks, failures, entropy_calls, sequence, mode;
static uint32_t base;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)
ssize_t __real_getrandom(void *, size_t, unsigned);
ssize_t __wrap_getrandom(void *out, size_t count, unsigned flags)
{
    entropy_calls++;
    CHECK(flags == 0u && count > 0u && count <= 256u);
    if (mode == 5u) return __real_getrandom(out, count, flags);
    if (mode == 1u && entropy_calls == 1u) { errno = EINTR; return -1; }
    if (mode == 3u) return 0;
    if (mode == 4u) { errno = EIO; return -1; }
    if (mode == 2u && count > 3u) count = 3u;
    for (size_t i = 0; i < count; i++) ((unsigned char *)out)[i] = (unsigned char)++sequence;
    return (ssize_t)count;
}
static void fatal(uint32_t address, const char *message)
{
    host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, message);
}
static kernel_call_frame frame_with(uint32_t a, uint32_t b)
{
    CHECK(kernel_guest_write_u32(base, 0x1234u));
    CHECK(kernel_guest_write_u32(base + 4u, a));
    CHECK(kernel_guest_write_u32(base + 8u, b));
    return (kernel_call_frame){.stack_ptr = base, .stack_limit = base + 12u};
}
static uint32_t call(uint32_t entry, uint32_t a, uint32_t b)
{
    kernel_call_frame f = frame_with(a,b);
    return xnet_hle_call(entry,&f);
}
static bool refuses(uint32_t entry, kernel_call_frame *frame)
{
    volatile bool returned = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm(); (void)xnet_hle_call(entry,frame); returned = true;
    }
    bool result = !returned && host_run_result()->reason == HOST_STOP_XDK_UNIMPLEMENTED &&
                  host_run_result()->guest_address == entry;
    host_run_disarm(); return result;
}
static void reset_entropy(unsigned value) { mode = value; entropy_calls = sequence = 0u; }
int main(void)
{
    const xnet_surface_entry rows[]={{XNET_RANDOM_ENTRY,"XNetRandom",2u},
        {XNET_CREATE_KEY_ENTRY,"XNetCreateKey",2u},{0x431716u,"register key",2u}};
    CHECK(xnet_hle_init(rows,3u)); xnet_hle_set_fatal(fatal);
    CHECK(xnet_random_register() == 2u && xnet_hle_implemented_count() == 2u);
    CHECK(xnet_hle_entry(0x431716u)->state == XNET_ENTRY_STUB);
    guest_region_request r={.bytes=4096u,.protect=PAGE_READWRITE,.state=MEM_COMMIT};
    nt_status status; base=guest_region_alloc(&r,&status); CHECK(base != 0u);
    void *fixed=mmap((void *)(uintptr_t)0x770000u,65536u,PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    CHECK(fixed != MAP_FAILED); if(fixed == MAP_FAILED) return 1;
    CHECK(kernel_guest_write_u32(XNET_STACK_POINTER,0u)); reset_entropy(0);
    CHECK(xnet_hle_call(XNET_RANDOM_ENTRY,NULL)==10093u);
    CHECK(xnet_hle_call(XNET_CREATE_KEY_ENTRY,NULL)==10093u && entropy_calls==0u);
    CHECK(kernel_guest_write_u32(XNET_STACK_POINTER,base+512u));
    CHECK(call(XNET_RANDOM_ENTRY,0u,0u)==0u && entropy_calls==0u);
    unsigned char output[640];
    for(unsigned kind=0;kind<3;kind++) {
        memset(output,0xA5,sizeof(output)); CHECK(kernel_guest_write_bytes(base+64u,output,sizeof(output)));
        reset_entropy(kind); CHECK(call(XNET_RANDOM_ENTRY,base+65u,600u)==0u);
        CHECK(kernel_guest_read_bytes(base+64u,output,sizeof(output)));
        CHECK(output[0]==0xA5 && output[601]==0xA5);
        for(unsigned i=0;i<600;i++) CHECK(output[i+1]==(unsigned char)(i+1u));
        CHECK(sequence==600u); if(kind==1) CHECK(entropy_calls==4u);
    }
    for(unsigned overlap=0;overlap<3;overlap++) {
        reset_entropy(0); sequence=0xF0u; memset(output,0xA5,sizeof(output));
        CHECK(kernel_guest_write_bytes(base+64u,output,sizeof(output)));
        uint32_t id=base+65u,key=overlap==0?base+100u:overlap==1?id:id+4u;
        CHECK(call(XNET_CREATE_KEY_ENTRY,id,key)==0u && sequence==0x108u && entropy_calls==2u);
        unsigned char expected[640]; memset(expected,0xA5,sizeof(expected));
        for(unsigned i=0;i<8;i++) expected[id-(base+64u)+i]=(unsigned char)(i+0xF1u);
        for(unsigned i=0;i<16;i++) expected[key-(base+64u)+i]=(unsigned char)(i+0xF9u);
        expected[id-(base+64u)] &= 0x0Fu;
        CHECK(kernel_guest_read_bytes(base+64u,output,sizeof(output)));
        CHECK(memcmp(output,expected,sizeof(output))==0);
    }
    for(unsigned kind=3;kind<=4;kind++) {
        reset_entropy(kind); kernel_call_frame f=frame_with(base+64u,8u);
        CHECK(refuses(XNET_RANDOM_ENTRY,&f)); CHECK(sequence==0u);
    }
    reset_entropy(0);
    kernel_call_frame f=frame_with(0u,1u); CHECK(refuses(XNET_RANDOM_ENTRY,&f));
    f=frame_with(0xFFFFFFFCu,8u); CHECK(refuses(XNET_RANDOM_ENTRY,&f));
    CHECK(entropy_calls==0u);
    f=frame_with(base+64u,0u); f.stack_limit=base+8u;
    CHECK(refuses(XNET_CREATE_KEY_ENTRY,&f) && sequence==8u);
    /* Exact original read-after-first-fill ordering: ID aliases key argument. */
    reset_entropy(0); f=frame_with(base+8u,base+100u);
    CHECK(refuses(XNET_CREATE_KEY_ENTRY,&f));
    uint32_t changed; CHECK(kernel_guest_read_u32(base+8u,&changed) && changed==0x04030201u);
    reset_entropy(5); CHECK(call(XNET_RANDOM_ENTRY,base+64u,256u)==0u);
    CHECK(entropy_calls>=1u); /* no statistical/random inequality assertion */
    CHECK(call(XNET_CREATE_KEY_ENTRY,base+64u,base+80u)==0u);
    CHECK(kernel_guest_read_bytes(base+64u,output,8u) && (output[0]&0xF0u)==0u);
    /* Production thunk routing and genuine stdcall pop, no invented arity. */
    const xdk_dispatch_entry dispatch[]={{XNET_RANDOM_ENTRY,"XNetRandom",XDK_MODULE_XNET},
        {XNET_CREATE_KEY_ENTRY,"XNetCreateKey",XDK_MODULE_XNET}};
    CHECK(xdk_thunk_init(dispatch,2u)); CHECK(xnet_random_register()==2u);
    CHECK(xdk_thunk_declare_abi(XNET_RANDOM_ENTRY,XDK_CC_STDCALL,2u,0u));
    f=frame_with(base+64u,0u); (void)f; g_esp=base; g_eax=99u;
    xdk_thunk_dispatch_at(XNET_RANDOM_ENTRY); CHECK(g_eax==0u && g_esp==base+12u);
    xdk_thunk_shutdown(); xnet_hle_shutdown(); CHECK(guest_region_free(base));
    CHECK(munmap(fixed,65536u)==0); printf("T1111: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
