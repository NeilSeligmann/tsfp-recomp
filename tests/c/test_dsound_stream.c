/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_stream.h"
#include "dsound_hle.h"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static uint32_t failed_destroy_heap;
static bool fail_malloc,fail_calloc,fail_heap,fail_alloc,fail_destroy,pad_header;
void *__real_malloc(size_t bytes);
void *__real_calloc(size_t count,size_t bytes);
uint32_t __real_guest_heap_create(uint32_t flags,uint32_t initial,uint32_t maximum);
bool __real_guest_heap_destroy(uint32_t heap);
kernel_guest_ptr __real_guest_heap_alloc(uint32_t heap,uint32_t bytes);
void *__wrap_malloc(size_t bytes)
{if(fail_malloc){fail_malloc=false;return NULL;}return __real_malloc(bytes);}
void *__wrap_calloc(size_t count,size_t bytes)
{if(fail_calloc){fail_calloc=false;return NULL;}return __real_calloc(count,bytes);}
uint32_t __wrap_guest_heap_create(uint32_t flags,uint32_t initial,uint32_t maximum)
{if(fail_heap){fail_heap=false;return 0u;}return __real_guest_heap_create(flags,initial,maximum);}
bool __wrap_guest_heap_destroy(uint32_t heap)
{if(fail_destroy){fail_destroy=false;failed_destroy_heap=heap;return false;}return __real_guest_heap_destroy(heap);}
kernel_guest_ptr __wrap_guest_heap_alloc(uint32_t heap,uint32_t bytes)
{
    if(fail_alloc){fail_alloc=false;return 0u;}
    if(pad_header && bytes==40u){pad_header=false;if(__real_guest_heap_alloc(heap,4064u)==0u)return 0u;}
    return __real_guest_heap_alloc(heap,bytes);
}
static uint8_t irql;
static bool known_irql=true;
static bool current_irql(uint8_t *out){*out=irql;return known_irql;}
static uint32_t device,desc,format,params;
static void seed(bool spatial)
{
    uint8_t d[24]={0},f[20]={0};uint16_t channels=spatial?1u:2u;
    uint32_t words[6]={spatial?16u:0u,3u,format,0u,0u,0u};memcpy(d,words,sizeof(d));
    uint16_t short_words[2]={0x69u,channels};memcpy(f,short_words,4u);
    uint32_t rate=44100u,average=(44100u*36u*channels)>>6u;memcpy(f+4u,&rate,4u);memcpy(f+8u,&average,4u);
    uint16_t rest[4]={(uint16_t)(36u*channels),4u,2u,64u};memcpy(f+12u,rest,8u);
    CHECK(kernel_guest_write_bytes(desc,d,sizeof(d)));CHECK(kernel_guest_write_bytes(format,f,sizeof(f)));
}
static void seed_params(void)
{uint32_t p[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};CHECK(kernel_guest_write_bytes(params,p,sizeof(p)));}
static uint32_t create_one(bool spatial,uint32_t output)
{seed(spatial);store(output,0xAABBCCDDu);CHECK_EQ_U32(dsound_stream_create(desc,output),0u);return load(output);}
static void expect_create_refusal(uint32_t d,uint32_t output)
{
    uint32_t before=load(device+4u),old=load(output);
    RUN_EXPECTING_FATAL((void)dsound_stream_create(d,output));CHECK(fatal_seen);
    CHECK_EQ_U32(load(device+4u),before);CHECK_EQ_U32(load(output),old);
}
static void snapshot_unchanged(uint32_t stream,const dsound_stream_snapshot *before)
{dsound_stream_snapshot after;CHECK(dsound_stream_get_snapshot(stream,&after));CHECK(memcmp(&after,before,sizeof(after))==0);}
static void test_policy(void)
{
    uint32_t out=SCRATCH_DATA+128u;seed(true);store(out,0xAABBCCDDu);
    expect_create_refusal(desc,out);dsound_stream_set_enabled(true);
    dsound_stream_set_irql_provider(NULL);expect_create_refusal(desc,out);dsound_stream_set_irql_provider(current_irql);
    known_irql=false;expect_create_refusal(desc,out);known_irql=true;
    irql=1u;expect_create_refusal(desc,out);irql=0u;
    store(0x4124A8u,1u);expect_create_refusal(desc,out);store(0x4124A8u,0u);
    uint32_t old=load(desc+4u);store(desc+4u,2u);expect_create_refusal(desc,out);store(desc+4u,old);
    expect_create_refusal(0u,out);
    expect_create_refusal(desc,desc);expect_create_refusal(desc,format);expect_create_refusal(desc,device+4u);
    store(0x4A1CF0u,0x12345678u);expect_create_refusal(desc,out);store(0x4A1CF0u,0x406879u);
    for(unsigned failure=0u;failure<4u;failure++) {
        fail_malloc=failure==0u;fail_calloc=failure==1u;fail_heap=failure==2u;fail_alloc=failure==3u;
        CHECK_EQ_U32(dsound_stream_create(desc,out),0x8007000Eu);CHECK_EQ_U32(load(out),0xAABBCCDDu);
        CHECK_EQ_U32(load(device+4u),6u);CHECK_EQ_U32(guest_mem_heap_count(),1u);
        CHECK(!fail_malloc && !fail_calloc && !fail_heap && !fail_alloc);
    }
}
static void test_ten_streams(void)
{
    dsound_stream_snapshot first={0};
    for(unsigned i=0u;i<10u;i++) {
        uint32_t output=SCRATCH_DATA+128u+i*8u,stream=create_one(i<5u,output);
        CHECK_EQ_U32(load(device+4u),7u+i);dsound_stream_snapshot value;
        CHECK(dsound_stream_get_snapshot(stream,&value));CHECK_EQ_U32(value.header[0],0x4A1D00u);
        CHECK_EQ_U32(value.header[1],0x4A1CF0u);CHECK_EQ_U32(value.header[2],1u);
        CHECK_EQ_U32(value.header[3],device);
        for(unsigned j=4u;j<10u;j++)CHECK_EQ_U32(value.header[j],0u);
        CHECK(value.lease.serial!=0u);CHECK_EQ_U32(value.lease.internal_address,device);
        CHECK(guest_heap_valid(value.lease.device_heap));
        CHECK_EQ_U32(value.scope.descriptor_address,desc);CHECK_EQ_U32(value.scope.format_address,format);
        CHECK_EQ_U32(value.publication_address,output);CHECK_EQ_U32(value.cache_mask,0u);
        if(i==0u)first=value;
        if(i<5u) {
            seed_params();CHECK_EQ_U32(dsound_stream_cache_i3dl2(stream,params,0u),0u);
            CHECK_EQ_U32(dsound_stream_cache_min_distance(stream,0x3F800000u,0u),0u);
            /* Curve is deliberately unmapped: this policy stores VA/count only. */
            CHECK_EQ_U32(dsound_stream_cache_rolloff(stream,0x4B914Cu,4u,0u),0u);
            CHECK(dsound_stream_get_snapshot(stream,&value));CHECK_EQ_U32(value.cache_mask,7u);
            CHECK_EQ_U32(value.i3dl2_address,params);CHECK_EQ_U32(value.i3dl2[2],0xFFFFF448u);
            CHECK_EQ_U32(value.min_distance_bits,0x3F800000u);CHECK_EQ_U32(value.curve_address,0x4B914Cu);
            CHECK_EQ_U32(value.curve_count,4u);
        }
        CHECK(memcmp(value.scope.format,first.scope.format,20u)==0 || i>=5u);
    }
    seed(true);expect_create_refusal(desc,SCRATCH_DATA+128u);
    dsound_stream_snapshot value;CHECK(dsound_stream_get_snapshot(first.stream_address,&value));
    CHECK(memcmp(value.scope.descriptor,first.scope.descriptor,24u)==0);CHECK_EQ_U32(value.scope.channels,1u);
    CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);CHECK_EQ_U32(load(device+4u),17u);
    CHECK(!dsound_device_reset_checked());CHECK(dsound_stream_reset_checked());CHECK_EQ_U32(load(device+4u),7u);
    CHECK_EQ_U32(guest_mem_heap_count(),1u);CHECK(dsound_stream_reset_checked());
    CHECK(!dsound_stream_get_snapshot(first.stream_address,&value));
    /* Restore cold base6 for independent vectors; stream reset does not lose Create refs. */
    CHECK(dsound_device_reset_checked());store(SCRATCH_DATA,0u);
    CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);device=load(SCRATCH_DATA)-8u;
}
static void test_setters(void)
{
    uint32_t stream=create_one(true,SCRATCH_DATA+128u);dsound_stream_snapshot value;CHECK(dsound_stream_get_snapshot(stream,&value));
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_min_distance(stream,0x3F800000u,0u));CHECK(fatal_seen);snapshot_unchanged(stream,&value);
    seed_params();store(params+8u,0u);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(stream,params,0u));CHECK(fatal_seen);snapshot_unchanged(stream,&value);
    seed_params();RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(stream,params,1u));CHECK(fatal_seen);snapshot_unchanged(stream,&value);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(stream,stream,0u));CHECK(fatal_seen);snapshot_unchanged(stream,&value);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(stream,0u,0u));CHECK(fatal_seen);snapshot_unchanged(stream,&value);
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(stream,params,0u),0u);CHECK(dsound_stream_get_snapshot(stream,&value));
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(stream,params,0u));CHECK(fatal_seen);snapshot_unchanged(stream,&value);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_min_distance(stream,0x40000000u,0u));CHECK(fatal_seen);snapshot_unchanged(stream,&value);
    store(0x4124A8u,1u);RUN_EXPECTING_FATAL((void)dsound_stream_cache_min_distance(stream,0x3F800000u,0u));CHECK(fatal_seen);
    store(0x4124A8u,0u);snapshot_unchanged(stream,&value);
    dsound_stream_set_enabled(false);RUN_EXPECTING_FATAL((void)dsound_stream_cache_min_distance(stream,0x3F800000u,0u));CHECK(fatal_seen);
    dsound_stream_set_enabled(true);snapshot_unchanged(stream,&value);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(stream,0x3F800000u,0u),0u);CHECK(dsound_stream_get_snapshot(stream,&value));
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_rolloff(stream,0x4B914Cu,3u,0u));CHECK(fatal_seen);snapshot_unchanged(stream,&value);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_rolloff(stream,0x4B9150u,4u,0u));CHECK(fatal_seen);snapshot_unchanged(stream,&value);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(stream,0x4B914Cu,4u,0u),0u);CHECK(dsound_stream_get_snapshot(stream,&value));
    for(unsigned i=0u;i<10u;i++) {
        uint32_t old=load(stream+4u*i);store(stream+4u*i,old^1u);
        dsound_stream_snapshot sentinel,before;memset(&before,0xA5,sizeof(before));sentinel=before;
        CHECK(!dsound_stream_get_snapshot(stream,&sentinel));CHECK(memcmp(&sentinel,&before,sizeof(before))==0);
        RUN_EXPECTING_FATAL((void)dsound_stream_cache_rolloff(stream,0x4B914Cu,4u,0u));CHECK(fatal_seen);
        CHECK(!dsound_stream_reset_checked());CHECK_EQ_U32(load(device+4u),7u);store(stream+4u*i,old);
    }
    CHECK(dsound_stream_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
    stream=create_one(false,SCRATCH_DATA+128u);seed_params();
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(stream,params,0u));CHECK(fatal_seen);
    CHECK(dsound_stream_reset_checked());
}
static uint32_t invoke(uint32_t entry,uint32_t caller,const uint32_t *args,unsigned count)
{
    uint32_t stack=SCRATCH_DATA+0x800u;store(stack,caller);
    for(unsigned i=0u;i<count;i++)store(stack+4u+4u*i,args[i]);
    kernel_call_frame frame={0};frame.stack_ptr=stack;frame.stack_limit=stack+4u+4u*count;
    return dsound_hle_call(entry,&frame);
}
static void test_handlers(void)
{
    CHECK_EQ_U32(dsound_stream_register(), 10u); /* T1245 stays unregistered pending a verified lease transaction. */uint32_t out=SCRATCH_DATA+128u;seed(true);store(out,0xABABABABu);
    uint32_t args[4]={desc,out,0u,0u};
    RUN_EXPECTING_FATAL((void)invoke(0x40967Cu,0x388DA5u,args,2u));CHECK(fatal_seen);CHECK_EQ_U32(load(out),0xABABABABu);
    CHECK_EQ_U32(invoke(0x40967Cu,0x29943u,args,2u),0u);uint32_t stream=load(out);seed_params();
    args[0]=stream;args[1]=params;args[2]=0u;
    RUN_EXPECTING_FATAL((void)invoke(0x408637u,0u,args,3u));CHECK(fatal_seen);
    CHECK_EQ_U32(invoke(0x408637u,0x2998Au,args,3u),0u);
    args[1]=0x3F800000u;
    RUN_EXPECTING_FATAL((void)invoke(0x4085F1u,0u,args,3u));CHECK(fatal_seen);
    CHECK_EQ_U32(invoke(0x4085F1u,0x29998u,args,3u),0u);
    args[1]=0x4B914Cu;args[2]=4u;args[3]=0u;
    RUN_EXPECTING_FATAL((void)invoke(0x408632u,0u,args,4u));CHECK(fatal_seen);
    CHECK_EQ_U32(invoke(0x408632u,0x299A8u,args,4u),0u);
    /* T1199: the sound update (callers 0x29E81, 0x29E90) switches the configured stream between the three 4 point curves, apply 0. */
    dsound_stream_snapshot curve_state;
    args[1]=0x4B915Cu;CHECK_EQ_U32(invoke(0x408632u,0x29E81u,args,4u),0u);
    CHECK(dsound_stream_get_snapshot(stream,&curve_state));CHECK_EQ_U32(curve_state.curve_address,0x4B915Cu);CHECK_EQ_U32(curve_state.curve_count,4u);
    args[1]=0x4B916Cu;CHECK_EQ_U32(invoke(0x408632u,0x29E90u,args,4u),0u);
    CHECK(dsound_stream_get_snapshot(stream,&curve_state));CHECK_EQ_U32(curve_state.curve_address,0x4B916Cu);
    RUN_EXPECTING_FATAL((void)invoke(0x408632u,0x29E82u,args,4u));CHECK(fatal_seen);
    args[1]=0x4B917Cu;RUN_EXPECTING_FATAL((void)invoke(0x408632u,0x29E81u,args,4u));CHECK(fatal_seen);
    args[1]=0x4B915Cu;args[2]=3u;RUN_EXPECTING_FATAL((void)invoke(0x408632u,0x29E81u,args,4u));CHECK(fatal_seen);
    args[2]=4u;args[3]=1u;RUN_EXPECTING_FATAL((void)invoke(0x408632u,0x29E81u,args,4u));CHECK(fatal_seen);
    CHECK(dsound_stream_reset_checked());
}
static void test_permissions(void)
{
    map_fixed(0x22000000u,8192u);seed(true);store(0x22000FFEu,0x11223344u);
    CHECK(mprotect((void *)0x22001000u,4096u,PROT_READ)==0);expect_create_refusal(desc,0x22000FFEu);
    CHECK(mprotect((void *)0x22001000u,4096u,PROT_READ|PROT_WRITE)==0);
    uint32_t out=SCRATCH_DATA+128u;pad_header=true;uint32_t stream=create_one(true,out);
    CHECK_EQ_U32(stream&4095u,0u);uint32_t metadata_page=(stream-16u)&~4095u;
    CHECK(mprotect((void *)(uintptr_t)metadata_page,4096u,PROT_NONE)==0);
    uint32_t actual;CHECK(kernel_guest_read_u32(stream,&actual));dsound_stream_snapshot value;
    CHECK(!dsound_stream_get_snapshot(stream,&value));CHECK(!dsound_stream_reset_checked());CHECK_EQ_U32(load(device+4u),7u);
    seed_params();RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(stream,params,0u));CHECK(fatal_seen);
    CHECK(mprotect((void *)(uintptr_t)metadata_page,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(dsound_stream_get_snapshot(stream,&value));CHECK(dsound_stream_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
    /* Use a separate input page so protecting it cannot hide output/stack. */
    uint8_t saved[24];seed(true);CHECK(kernel_guest_read_bytes(desc,saved,sizeof(saved)));
    CHECK(kernel_guest_write_bytes(0x22000000u,saved,sizeof(saved)));
    CHECK(mprotect((void *)0x22000000u,4096u,PROT_NONE)==0);
    expect_create_refusal(0x22000000u,out);
    CHECK(mprotect((void *)0x22000000u,4096u,PROT_READ|PROT_WRITE)==0);
    /* Read-only descriptor/format/parameter inputs are supported, and snapshots
     * retain the original values after shared stack input bytes are overwritten. */
    uint8_t format_copy[20];CHECK(kernel_guest_read_bytes(format,format_copy,sizeof(format_copy)));
    CHECK(kernel_guest_write_bytes(0x22000100u,format_copy,sizeof(format_copy)));store(0x22000008u,0x22000100u);
    uint32_t parameter_words[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(0x22000200u,parameter_words,sizeof(parameter_words)));
    CHECK(mprotect((void *)0x22000000u,4096u,PROT_READ)==0);store(out,0x11223344u);
    CHECK_EQ_U32(dsound_stream_create(0x22000000u,out),0u);stream=load(out);
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(stream,0x22000200u,0u),0u);
    CHECK(dsound_stream_get_snapshot(stream,&value));CHECK_EQ_U32(value.scope.descriptor_address,0x22000000u);
    CHECK_EQ_U32(value.scope.format_address,0x22000100u);CHECK_EQ_U32(value.i3dl2_address,0x22000200u);
    uint32_t header_page=stream&~4095u;
    CHECK(mprotect((void *)(uintptr_t)header_page,4096u,PROT_NONE)==0);
    CHECK(!dsound_stream_get_snapshot(stream,&value));CHECK(!dsound_stream_reset_checked());
    CHECK_EQ_U32(load(device+4u),7u);
    CHECK(mprotect((void *)(uintptr_t)header_page,4096u,PROT_READ)==0);
    CHECK(dsound_stream_get_snapshot(stream,&value));CHECK(dsound_stream_reset_checked());
    CHECK_EQ_U32(load(device+4u),6u);
    CHECK(mprotect((void *)0x22000000u,4096u,PROT_READ|PROT_WRITE)==0);
}
static void test_detached_cleanup(void)
{
    uint32_t stream=create_one(true,SCRATCH_DATA+128u);dsound_stream_snapshot before;
    CHECK(dsound_stream_get_snapshot(stream,&before));CHECK_EQ_U32(load(device+4u),7u);
    fail_destroy=true;CHECK(!dsound_stream_reset_checked());CHECK(!fail_destroy);
    /* The lease is consumed exactly once. Child remains tracked/private until
     * checked cleanup retries; no atomic lease+free claim or extra decrement. */
    CHECK_EQ_U32(load(device+4u),6u);CHECK(guest_heap_valid(before.stream_heap));
    CHECK_EQ_U32(load(stream),0x4A1D00u);dsound_stream_snapshot sentinel;
    CHECK(!dsound_stream_get_snapshot(stream,&sentinel));
    const uint32_t live=create_one(true,SCRATCH_DATA+136u);
    dsound_stream_snapshot unchanged;CHECK(dsound_stream_get_snapshot(live,&unchanged));
    const uint32_t quarantined_params=stream+32u;
    const uint32_t words[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(quarantined_params,words,sizeof(words)));
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(live,quarantined_params,0u));CHECK(fatal_seen);
    snapshot_unchanged(live,&unchanged);
    CHECK(dsound_stream_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
    CHECK(!guest_heap_valid(before.stream_heap));CHECK_EQ_U32(guest_mem_heap_count(),1u);
    CHECK(dsound_stream_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
}
static void inspect_parent(uint32_t internal,void *userdata,uint32_t *result)
{(void)userdata;*result=load(internal+4u);}
static void test_unleased_rollback(void)
{
    const uint32_t out=SCRATCH_DATA+128u;
    seed(true);store(out,0xAABBCCDDu);
    const uint32_t reference=load(device+4u);
    fail_alloc=true;fail_destroy=true;
    RUN_EXPECTING_FATAL((void)dsound_stream_create(desc,out));CHECK(fatal_seen);
    CHECK(!fail_alloc && !fail_destroy);CHECK(failed_destroy_heap!=0u);
    CHECK_EQ_U32(load(out),0xAABBCCDDu);CHECK_EQ_U32(load(device+4u),reference);
    CHECK_EQ_U32(guest_mem_heap_count(),2u);CHECK(guest_heap_valid(failed_destroy_heap));
    /* Calling lease observation and reset after caught fatal verifies both locks
     * are released. This rollback has no committed child lease/reference. */
    uint32_t observed=0u;
    CHECK(dsound_device_with_owned_interface(device+8u,inspect_parent,NULL,&observed));
    CHECK_EQ_U32(observed,reference);
    fail_destroy=true;CHECK(!dsound_stream_reset_checked());CHECK(!fail_destroy);
    CHECK_EQ_U32(guest_mem_heap_count(),2u);CHECK_EQ_U32(load(device+4u),reference);
    CHECK(dsound_stream_reset_checked());CHECK_EQ_U32(guest_mem_heap_count(),1u);
    CHECK_EQ_U32(load(device+4u),reference);CHECK_EQ_U32(load(out),0xAABBCCDDu);
    CHECK(dsound_stream_reset_checked());CHECK_EQ_U32(load(device+4u),reference);
}
static void stale_unleased_rollback(void)
{
    const uint32_t out=SCRATCH_DATA+128u;seed(true);store(out,0xAABBCCDDu);
    const uint32_t reference=load(device+4u);fail_alloc=true;fail_destroy=true;
    RUN_EXPECTING_FATAL((void)dsound_stream_create(desc,out));CHECK(fatal_seen);
    const uint32_t old=failed_destroy_heap;CHECK(guest_heap_destroy(old));
    const uint32_t replacement=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);
    CHECK(replacement!=old);const uint32_t foreign=guest_heap_alloc(replacement,40u);
    CHECK(foreign!=0u);store(foreign,0xFACECAFEu);
    CHECK(!dsound_stream_reset_checked());CHECK(guest_heap_valid(replacement));
    CHECK_EQ_U32(load(foreign),0xFACECAFEu);CHECK_EQ_U32(load(device+4u),reference);
    CHECK_EQ_U32(load(out),0xAABBCCDDu);
}
static void foreign_replacement(void)
{
    uint32_t stream=create_one(true,SCRATCH_DATA+128u);dsound_stream_snapshot value;
    CHECK(dsound_stream_get_snapshot(stream,&value));CHECK(guest_heap_destroy(value.stream_heap));
    map_fixed(stream&~4095u,4096u);store(stream,0xFACECAFEu);
    CHECK(!dsound_stream_reset_checked());CHECK_EQ_U32(load(stream),0xFACECAFEu);CHECK_EQ_U32(load(device+4u),7u);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(stream,params,0u));CHECK(fatal_seen);
    CHECK_EQ_U32(load(stream),0xFACECAFEu);CHECK(!dsound_device_reset_checked());
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();dsound_device_set_fatal(catching_fatal);
    dsound_stream_set_fatal(catching_fatal);dsound_stream_set_irql_provider(current_irql);
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);
    /* Synthetic safe table: all slots route to a verified unconditional stop.
     * Actual original ordered-table identity is tested separately, not copied here. */
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    device=load(SCRATCH_DATA)-8u;desc=SCRATCH_DATA+256u;format=SCRATCH_DATA+320u;params=desc+20u;
    test_policy();test_ten_streams();test_setters();test_handlers();test_permissions();test_detached_cleanup();test_unleased_rollback();
    pid_t pid=fork();CHECK(pid>=0);if(pid==0){failures=0;foreign_replacement();fflush(stdout);_exit(failures?1:0);}
    if(pid>0){int status;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
    pid=fork();CHECK(pid>=0);if(pid==0){failures=0;stale_unleased_rollback();fflush(stdout);_exit(failures?1:0);}
    if(pid>0){int status;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
    CHECK(dsound_stream_reset_checked());CHECK(dsound_device_reset_checked());environment_end();
    printf("passive stream: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
