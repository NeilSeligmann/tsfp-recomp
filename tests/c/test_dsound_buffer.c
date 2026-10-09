/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_buffer.h"
#include "dsound_hle.h"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static uint32_t last_created_heap;
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
{if(fail_heap){fail_heap=false;return 0u;}last_created_heap=__real_guest_heap_create(flags,initial,maximum);return last_created_heap;}
bool __wrap_guest_heap_destroy(uint32_t heap)
{if(fail_destroy){fail_destroy=false;return false;}return __real_guest_heap_destroy(heap);}
kernel_guest_ptr __wrap_guest_heap_alloc(uint32_t heap,uint32_t bytes)
{
    if(fail_alloc){fail_alloc=false;return 0u;}
    if(pad_header && bytes==36u){pad_header=false;if(__real_guest_heap_alloc(heap,4064u)==0u)return 0u;}
    return __real_guest_heap_alloc(heap,bytes);
}
static uint32_t missing_marker;static int marker_result=1;
int recomp_has_stop_boundary(uint32_t address)
{return address==missing_marker?marker_result:1;}
static uint8_t irql;
static bool known_irql=true;
static bool current_irql(uint8_t *out){*out=irql;return known_irql;}
static uint32_t device,desc,format,params;
static void seed(bool spatial)
{
    uint8_t d[24]={0},f[20]={0};uint16_t channels=1u;
    uint32_t words[6]={24u,spatial?16u:0u,0u,format,0u,0u};memcpy(d,words,sizeof(d));
    uint16_t short_words[2]={0x69u,channels};memcpy(f,short_words,4u);
    uint32_t rate=44000u,average=(44000u*36u*channels)>>6u;memcpy(f+4u,&rate,4u);memcpy(f+8u,&average,4u);
    uint16_t rest[4]={(uint16_t)(36u*channels),4u,2u,64u};memcpy(f+12u,rest,8u);
    CHECK(kernel_guest_write_bytes(desc,d,sizeof(d)));CHECK(kernel_guest_write_bytes(format,f,sizeof(f)));
}
static void seed_params(void)
{uint32_t p[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};CHECK(kernel_guest_write_bytes(params,p,sizeof(p)));}
static uint32_t create_one(bool spatial,uint32_t output)
{seed(spatial);store(output,0xAABBCCDDu);CHECK_EQ_U32(dsound_buffer_create(device+8u,desc,output,0u),0u);return load(output);}
static void expect_create_refusal(uint32_t d,uint32_t output)
{
    uint32_t before=load(device+4u),old=load(output);
    RUN_EXPECTING_FATAL((void)dsound_buffer_create(device+8u,d,output,0u));CHECK(fatal_seen);
    CHECK_EQ_U32(load(device+4u),before);CHECK_EQ_U32(load(output),old);
}
static void snapshot_unchanged(uint32_t buffer,const dsound_buffer_snapshot *before)
{dsound_buffer_snapshot after;CHECK(dsound_buffer_get_snapshot(buffer,&after));CHECK(memcmp(&after,before,sizeof(after))==0);}
static void test_policy(void)
{
    uint32_t out=0x5835F0u;seed(true);store(out,0xAABBCCDDu);
    expect_create_refusal(desc,out);dsound_buffer_set_enabled(true);
    const uint32_t targets[]={0x406879u,0x406FA9u,0x406FF0u,0x408040u};
    CHECK(dsound_buffer_stops_ready());
    for(unsigned i=0u;i<4u;i++)for(int result=0;result<=2;result+=2) {
        missing_marker=targets[i];marker_result=result;CHECK(!dsound_buffer_stops_ready());
        expect_create_refusal(desc,out);
    }
    missing_marker=0u;CHECK(dsound_buffer_stops_ready());
    dsound_buffer_set_irql_provider(NULL);expect_create_refusal(desc,out);dsound_buffer_set_irql_provider(current_irql);
    known_irql=false;expect_create_refusal(desc,out);known_irql=true;
    irql=1u;expect_create_refusal(desc,out);irql=0u;
    store(0x4124A8u,1u);expect_create_refusal(desc,out);store(0x4124A8u,0u);
    uint32_t old=load(desc+4u);store(desc+4u,2u);expect_create_refusal(desc,out);store(desc+4u,old);
    expect_create_refusal(0u,out);
    for(unsigned i=1u;i<4u;i++)expect_create_refusal(desc,out+i);
    expect_create_refusal(desc,out+160u);expect_create_refusal(desc,0x5818E8u);
    RUN_EXPECTING_FATAL((void)dsound_buffer_create(device+4u,desc,out,0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_create(device+8u,desc,out,1u));CHECK(fatal_seen);
    CHECK_EQ_U32(load(device+4u),6u);CHECK_EQ_U32(load(out),0xAABBCCDDu);
    expect_create_refusal(desc,desc);expect_create_refusal(desc,format);expect_create_refusal(desc,device+4u);
    store(0x4A1CE0u,0x12345678u);expect_create_refusal(desc,out);store(0x4A1CE0u,0x406879u);
    for(unsigned failure=0u;failure<4u;failure++) {
        fail_malloc=failure==0u;fail_calloc=failure==1u;fail_heap=failure==2u;fail_alloc=failure==3u;
        CHECK_EQ_U32(dsound_buffer_create(device+8u,desc,out,0u),0x8007000Eu);CHECK_EQ_U32(load(out),0xAABBCCDDu);
        CHECK_EQ_U32(load(device+4u),6u);CHECK_EQ_U32(guest_mem_heap_count(),1u);
        CHECK(!fail_malloc && !fail_calloc && !fail_heap && !fail_alloc);
    }
}
static void test_eighty_buffers(void)
{
    for(unsigned i=0u;i<80u;i++) {
        bool spatial=i<40u;uint32_t output=(spatial?0x5835F0u:0x5818E8u)+4u*(i%40u);
        uint32_t buffer=create_one(spatial,output);dsound_buffer_snapshot value;
        CHECK(dsound_buffer_get_snapshot(buffer,&value));CHECK_EQ_U32(load(device+4u),7u+i);
        CHECK_EQ_U32(value.header[0],0x4A1CE0u);CHECK_EQ_U32(value.header[1],1u);
        CHECK_EQ_U32(value.header[2],device);CHECK_EQ_U32(buffer,value.header_address+28u);
        for(unsigned j=3u;j<9u;j++)CHECK_EQ_U32(value.header[j],0u);
        CHECK(value.lease.serial!=0u);CHECK_EQ_U32(value.publication_address,output);
        if(spatial) {
            seed_params();CHECK_EQ_U32(dsound_buffer_cache_i3dl2(buffer,params,0u),0u);
            CHECK_EQ_U32(dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u),0u);
            CHECK_EQ_U32(dsound_buffer_cache_rolloff(buffer,0x64BC98u,1u,0u),0u);
            CHECK(dsound_buffer_get_snapshot(buffer,&value));CHECK_EQ_U32(value.cache_mask,7u);
        }
    }
    seed(true);expect_create_refusal(desc,0x5835F0u);CHECK(!dsound_device_reset_checked());
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
    CHECK_EQ_U32(guest_mem_heap_count(),1u);CHECK(dsound_buffer_reset_checked());
}
static void test_setters(void)
{
    uint32_t buffer=create_one(true,0x5835F0u);dsound_buffer_snapshot value;CHECK(dsound_buffer_get_snapshot(buffer,&value));
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u));CHECK(fatal_seen);snapshot_unchanged(buffer,&value);
    seed_params();store(params+8u,0u);
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_i3dl2(buffer,params,0u));CHECK(fatal_seen);snapshot_unchanged(buffer,&value);
    seed_params();RUN_EXPECTING_FATAL((void)dsound_buffer_cache_i3dl2(buffer,params,1u));CHECK(fatal_seen);snapshot_unchanged(buffer,&value);
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_i3dl2(buffer,buffer,0u));CHECK(fatal_seen);snapshot_unchanged(buffer,&value);
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_i3dl2(buffer,0u,0u));CHECK(fatal_seen);snapshot_unchanged(buffer,&value);
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(buffer,params,0u),0u);CHECK(dsound_buffer_get_snapshot(buffer,&value));
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_i3dl2(buffer,params,0u));CHECK(fatal_seen);snapshot_unchanged(buffer,&value);
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_min_distance(buffer,0x40000000u,0u));CHECK(fatal_seen);snapshot_unchanged(buffer,&value);
    store(0x4124A8u,1u);RUN_EXPECTING_FATAL((void)dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u));CHECK(fatal_seen);
    store(0x4124A8u,0u);snapshot_unchanged(buffer,&value);
    dsound_buffer_set_enabled(false);RUN_EXPECTING_FATAL((void)dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u));CHECK(fatal_seen);
    dsound_buffer_set_enabled(true);snapshot_unchanged(buffer,&value);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u),0u);CHECK(dsound_buffer_get_snapshot(buffer,&value));
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_rolloff(buffer,0x64BC98u,3u,0u));CHECK(fatal_seen);snapshot_unchanged(buffer,&value);
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_rolloff(buffer,0x64BC9Cu,1u,0u));CHECK(fatal_seen);snapshot_unchanged(buffer,&value);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(buffer,0x64BC98u,1u,0u),0u);CHECK(dsound_buffer_get_snapshot(buffer,&value));
    for(unsigned i=0u;i<9u;i++) {
        uint32_t old=load(value.header_address+4u*i);store(value.header_address+4u*i,old^1u);
        dsound_buffer_snapshot sentinel,before;memset(&before,0xA5,sizeof(before));sentinel=before;
        CHECK(!dsound_buffer_get_snapshot(buffer,&sentinel));CHECK(memcmp(&sentinel,&before,sizeof(before))==0);
        RUN_EXPECTING_FATAL((void)dsound_buffer_cache_rolloff(buffer,0x64BC98u,1u,0u));CHECK(fatal_seen);
        CHECK(!dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),7u);store(value.header_address+4u*i,old);
    }
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
    buffer=create_one(false,0x5818E8u);seed_params();
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_i3dl2(buffer,params,0u));CHECK(fatal_seen);
    CHECK(dsound_buffer_reset_checked());
}
static void expect_data_refusal(uint32_t buffer,uint32_t data,uint32_t length)
{
    dsound_buffer_snapshot before;CHECK(dsound_buffer_get_snapshot(buffer,&before));
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_data(buffer,data,length));CHECK(fatal_seen);snapshot_unchanged(buffer,&before);
}
/* T597: the host record, every named refusal and its no-write guarantee. */
static void test_set_data(void)
{
    const uint32_t data=0x23000000u,limit=0x04000000u;map_fixed(data,limit+4096u);
    uint32_t spatial=create_one(true,0x5835F0u),ordinary=create_one(false,0x5818E8u);dsound_buffer_snapshot value;
    /* partial spatial caches are not the measured startup state */
    expect_data_refusal(spatial,data,0x5A0u);seed_params();
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(spatial,params,0u),0u);expect_data_refusal(spatial,data,0x5A0u);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(spatial,0x3F800000u,0u),0u);expect_data_refusal(spatial,data,0x5A0u);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(spatial,0x64BC98u,1u,0u),0u);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));
    CHECK_EQ_U32(value.data_address,0u);CHECK_EQ_U32(value.data_length,0u);CHECK_EQ_U32(value.data_sets,0u);
    uint32_t references=load(device+4u);uint32_t header[9];memcpy(header,value.header,sizeof(header));
    CHECK_EQ_U32(dsound_buffer_set_data(spatial,data+0x100u,0x5A0u),0u);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));
    CHECK_EQ_U32(value.data_address,data+0x100u);CHECK_EQ_U32(value.data_length,0x5A0u);CHECK_EQ_U32(value.data_sets,1u);
    CHECK_EQ_U32(value.cache_mask,7u);CHECK_EQ_U32(load(device+4u),references);
    for(unsigned i=0u;i<9u;i++)CHECK_EQ_U32(load(value.header_address+4u*i),header[i]);
    CHECK(memcmp(value.header,header,sizeof(header))==0);
    /* the original accepts any length (35 and 37 included), so does the record, and a repeat replaces */
    CHECK_EQ_U32(dsound_buffer_set_data(spatial,data+0x100u,0x5A0u),0u);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));CHECK_EQ_U32(value.data_sets,2u);
    CHECK_EQ_U32(dsound_buffer_set_data(spatial,data+1u,35u),0u);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));
    CHECK_EQ_U32(value.data_address,data+1u);CHECK_EQ_U32(value.data_length,35u);CHECK_EQ_U32(value.data_sets,3u);
    CHECK_EQ_U32(dsound_buffer_set_data(spatial,data,limit),0u);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));CHECK_EQ_U32(value.data_length,limit);
    CHECK_EQ_U32(dsound_buffer_set_data(spatial,data,1u),0u);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));CHECK_EQ_U32(value.data_length,1u);
    /* named refusals keep the last record */
    expect_data_refusal(spatial,0u,0u);expect_data_refusal(spatial,0u,0x5A0u);expect_data_refusal(spatial,data,0u);
    expect_data_refusal(spatial,data,limit+1u);expect_data_refusal(spatial,data+4097u,limit);
    expect_data_refusal(spatial,data+limit+4096u,1u);expect_data_refusal(spatial,0x30000000u,0x5A0u);
    expect_data_refusal(spatial,0xFFFFFFF0u,0x40u);expect_data_refusal(spatial,data,0xFFFFFFFFu);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));CHECK_EQ_U32(value.data_address,data);CHECK_EQ_U32(value.data_length,1u);
    CHECK_EQ_U32(value.data_sets,5u);
    const uint32_t internal=value.lease.internal_address;
    expect_data_refusal(spatial,spatial,0x24u);expect_data_refusal(spatial,value.header_address,36u);
    expect_data_refusal(spatial,internal,16u);expect_data_refusal(spatial,0x412B30u,4u);
    expect_data_refusal(spatial,0x4124A8u,4u);expect_data_refusal(spatial,0x4A1CE0u,16u);
    expect_data_refusal(spatial,0x64BC98u,4u);expect_data_refusal(spatial,value.publication_address,4u);
    expect_data_refusal(spatial,0x5835EFu,2u);
    /* policy gates */
    store(0x4124A8u,1u);expect_data_refusal(spatial,data,0x5A0u);store(0x4124A8u,0u);
    irql=1u;expect_data_refusal(spatial,data,0x5A0u);irql=0u;
    known_irql=false;expect_data_refusal(spatial,data,0x5A0u);known_irql=true;
    dsound_buffer_set_irql_provider(NULL);expect_data_refusal(spatial,data,0x5A0u);dsound_buffer_set_irql_provider(current_irql);
    dsound_buffer_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_data(spatial,data,0x5A0u));CHECK(fatal_seen);dsound_buffer_set_enabled(true);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_data(0u,data,0x5A0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_data(spatial+4u,data,0x5A0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_data(device+8u,data,0x5A0u));CHECK(fatal_seen);
    snapshot_unchanged(spatial,&value);
    /* an ordinary (flags 0) startup buffer with no setters is the other measured scope */
    CHECK_EQ_U32(dsound_buffer_set_data(ordinary,data+0x40u,0x240u),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));
    CHECK_EQ_U32(value.data_address,data+0x40u);CHECK_EQ_U32(value.data_length,0x240u);CHECK_EQ_U32(value.data_sets,1u);
    CHECK_EQ_U32(value.cache_mask,0u);
    for(unsigned i=3u;i<9u;i++)CHECK_EQ_U32(value.header[i],0u);
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
    /* a fresh object starts with no record, an unconfigured spatial one is refused */
    spatial=create_one(true,0x5835F0u);CHECK(dsound_buffer_get_snapshot(spatial,&value));
    CHECK_EQ_U32(value.data_sets,0u);expect_data_refusal(spatial,data,0x5A0u);
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
}
static void expect_volume_refusal(uint32_t buffer,int32_t volume)
{
    dsound_buffer_snapshot before;CHECK(dsound_buffer_get_snapshot(buffer,&before));
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_volume(buffer,volume));CHECK(fatal_seen);snapshot_unchanged(buffer,&before);
}
/* T601: the host record of the startup volume, every named refusal and its no-write guarantee. */
static void test_set_volume(void)
{
    const uint32_t data=0x23000000u;/* mapped by test_set_data */
    uint32_t spatial=create_one(true,0x5835F0u),ordinary=create_one(false,0x5818E8u);dsound_buffer_snapshot value;
    /* the measured order is SetBufferData then SetVolume, a fresh buffer refuses */
    expect_volume_refusal(ordinary,-10000);
    CHECK_EQ_U32(dsound_buffer_set_data(ordinary,data,0x240u),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.volume_sets,0u);CHECK_EQ_U32((uint32_t)value.volume,0u);
    uint32_t references=load(device+4u);uint32_t header[9];memcpy(header,value.header,sizeof(header));
    CHECK_EQ_U32(dsound_buffer_set_volume(ordinary,-10000),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));
    CHECK_EQ_U32((uint32_t)value.volume,0xFFFFD8F0u);CHECK_EQ_U32(value.volume_sets,1u);CHECK_EQ_U32(value.cache_mask,0u);
    CHECK_EQ_U32(value.data_address,data);CHECK_EQ_U32(value.data_length,0x240u);CHECK_EQ_U32(value.data_sets,1u);
    CHECK_EQ_U32(load(device+4u),references);
    for(unsigned i=0u;i<9u;i++)CHECK_EQ_U32(load(value.header_address+4u*i),header[i]);
    CHECK(memcmp(value.header,header,sizeof(header))==0);
    /* a repeat replaces the record, SetBufferData after it keeps the volume */
    CHECK_EQ_U32(dsound_buffer_set_volume(ordinary,-10000),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.volume_sets,2u);
    CHECK_EQ_U32(dsound_buffer_set_data(ordinary,data+0x40u,0x240u),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32((uint32_t)value.volume,0xFFFFD8F0u);CHECK_EQ_U32(value.volume_sets,2u);
    /* only the measured volume, every other int32 value is refused and keeps the record */
    const int32_t others[]={0,-1,1,-9999,-10001,-600,-10600,-5000,INT32_MIN,INT32_MAX,-100000};
    for(unsigned i=0u;i<sizeof(others)/sizeof(others[0]);i++)expect_volume_refusal(ordinary,others[i]);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.volume_sets,2u);
    /* the spatial startup scope needs all three caches and a recorded SetBufferData */
    expect_volume_refusal(spatial,-10000);seed_params();
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(spatial,params,0u),0u);expect_volume_refusal(spatial,-10000);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(spatial,0x3F800000u,0u),0u);expect_volume_refusal(spatial,-10000);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(spatial,0x64BC98u,1u,0u),0u);expect_volume_refusal(spatial,-10000);
    CHECK_EQ_U32(dsound_buffer_set_data(spatial,data+0x100u,0x5A0u),0u);
    CHECK_EQ_U32(dsound_buffer_set_volume(spatial,-10000),0u);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));
    CHECK_EQ_U32((uint32_t)value.volume,0xFFFFD8F0u);CHECK_EQ_U32(value.volume_sets,1u);CHECK_EQ_U32(value.cache_mask,7u);
    /* ownership, policy gates */
    expect_volume_refusal(spatial,0);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_volume(0u,-10000));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_volume(spatial+4u,-10000));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_volume(device+8u,-10000));CHECK(fatal_seen);
    store(0x4124A8u,1u);expect_volume_refusal(spatial,-10000);store(0x4124A8u,0u);
    irql=1u;expect_volume_refusal(spatial,-10000);irql=0u;
    known_irql=false;expect_volume_refusal(spatial,-10000);known_irql=true;
    dsound_buffer_set_irql_provider(NULL);expect_volume_refusal(spatial,-10000);dsound_buffer_set_irql_provider(current_irql);
    dsound_buffer_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_volume(spatial,-10000));CHECK(fatal_seen);dsound_buffer_set_enabled(true);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));CHECK_EQ_U32(value.volume_sets,1u);
    /* a reset clears the host record with the object, a new buffer starts fresh */
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
    ordinary=create_one(false,0x5818E8u);CHECK(dsound_buffer_get_snapshot(ordinary,&value));
    CHECK_EQ_U32(value.volume_sets,0u);CHECK_EQ_U32((uint32_t)value.volume,0u);expect_volume_refusal(ordinary,-10000);
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
}
static void expect_pause_refusal(uint32_t buffer,uint32_t mode)
{
    dsound_buffer_snapshot before;CHECK(dsound_buffer_get_snapshot(buffer,&before));
    RUN_EXPECTING_FATAL((void)dsound_buffer_pause(buffer,mode));CHECK(fatal_seen);snapshot_unchanged(buffer,&before);
}
/* T601: Pause(0) after SetBufferData and SetVolume, a host count only, every other request refused. */
static void test_pause(void)
{
    const uint32_t data=0x23000000u;/* mapped by test_set_data */
    uint32_t spatial=create_one(true,0x5835F0u),ordinary=create_one(false,0x5818E8u);dsound_buffer_snapshot value;
    /* the measured order is SetBufferData, SetVolume, Pause(0): each earlier step is needed */
    expect_pause_refusal(ordinary,0u);
    CHECK_EQ_U32(dsound_buffer_set_data(ordinary,data,0x240u),0u);expect_pause_refusal(ordinary,0u);
    CHECK_EQ_U32(dsound_buffer_set_volume(ordinary,-10000),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.pause_sets,0u);
    uint32_t references=load(device+4u);uint32_t header[9];memcpy(header,value.header,sizeof(header));
    CHECK_EQ_U32(dsound_buffer_pause(ordinary,0u),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));
    CHECK_EQ_U32(value.pause_sets,1u);CHECK_EQ_U32(value.volume_sets,1u);CHECK_EQ_U32(value.data_sets,1u);
    CHECK_EQ_U32((uint32_t)value.volume,0xFFFFD8F0u);CHECK_EQ_U32(value.data_address,data);CHECK_EQ_U32(load(device+4u),references);
    for(unsigned i=0u;i<9u;i++)CHECK_EQ_U32(load(value.header_address+4u*i),header[i]);
    CHECK(memcmp(value.header,header,sizeof(header))==0);
    CHECK_EQ_U32(dsound_buffer_pause(ordinary,0u),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.pause_sets,2u);
    /* the original returns the mode itself for a buffer nothing started, the passive model admits mode 0 only */
    for(uint32_t mode=1u;mode<5u;mode++)expect_pause_refusal(ordinary,mode);
    expect_pause_refusal(ordinary,0x80000000u);expect_pause_refusal(ordinary,0xFFFFFFFFu);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.pause_sets,2u);
    /* spatial: caches first, then the three steps */
    expect_pause_refusal(spatial,0u);seed_params();
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(spatial,params,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(spatial,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(spatial,0x64BC98u,1u,0u),0u);expect_pause_refusal(spatial,0u);
    CHECK_EQ_U32(dsound_buffer_set_data(spatial,data+0x100u,0x5A0u),0u);expect_pause_refusal(spatial,0u);
    CHECK_EQ_U32(dsound_buffer_set_volume(spatial,-10000),0u);
    CHECK_EQ_U32(dsound_buffer_pause(spatial,0u),0u);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));CHECK_EQ_U32(value.pause_sets,1u);CHECK_EQ_U32(value.cache_mask,7u);
    /* ownership and policy gates */
    RUN_EXPECTING_FATAL((void)dsound_buffer_pause(0u,0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_pause(spatial+4u,0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_pause(device+8u,0u));CHECK(fatal_seen);
    store(0x4124A8u,1u);expect_pause_refusal(spatial,0u);store(0x4124A8u,0u);
    irql=1u;expect_pause_refusal(spatial,0u);irql=0u;
    known_irql=false;expect_pause_refusal(spatial,0u);known_irql=true;
    dsound_buffer_set_irql_provider(NULL);expect_pause_refusal(spatial,0u);dsound_buffer_set_irql_provider(current_irql);
    dsound_buffer_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_buffer_pause(spatial,0u));CHECK(fatal_seen);dsound_buffer_set_enabled(true);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));CHECK_EQ_U32(value.pause_sets,1u);
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
    ordinary=create_one(false,0x5818E8u);CHECK(dsound_buffer_get_snapshot(ordinary,&value));
    CHECK_EQ_U32(value.pause_sets,0u);expect_pause_refusal(ordinary,0u);
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
}
static void expect_frequency_refusal(uint32_t buffer,uint32_t hertz)
{
    dsound_buffer_snapshot before;CHECK(dsound_buffer_get_snapshot(buffer,&before));
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_frequency(buffer,hertz));CHECK(fatal_seen);snapshot_unchanged(buffer,&before);
}
/* T601: SetFrequency(22042) after SetBufferData, SetVolume and Pause(0), a host record only. */
static void test_set_frequency(void)
{
    const uint32_t data=0x23000000u;/* mapped by test_set_data */
    uint32_t spatial=create_one(true,0x5835F0u),ordinary=create_one(false,0x5818E8u);dsound_buffer_snapshot value;
    /* the measured order is SetBufferData, SetVolume, Pause(0), SetFrequency: each earlier step is needed */
    expect_frequency_refusal(ordinary,22042u);
    CHECK_EQ_U32(dsound_buffer_set_data(ordinary,data,0x240u),0u);expect_frequency_refusal(ordinary,22042u);
    CHECK_EQ_U32(dsound_buffer_set_volume(ordinary,-10000),0u);expect_frequency_refusal(ordinary,22042u);
    CHECK_EQ_U32(dsound_buffer_pause(ordinary,0u),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.frequency_sets,0u);CHECK_EQ_U32(value.frequency,0u);
    uint32_t references=load(device+4u);uint32_t header[9];memcpy(header,value.header,sizeof(header));
    CHECK_EQ_U32(dsound_buffer_set_frequency(ordinary,22042u),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));
    CHECK_EQ_U32(value.frequency,22042u);CHECK_EQ_U32(value.frequency_sets,1u);CHECK_EQ_U32(value.pause_sets,1u);
    CHECK_EQ_U32(value.volume_sets,1u);CHECK_EQ_U32(value.data_sets,1u);CHECK_EQ_U32(value.cache_mask,0u);
    CHECK_EQ_U32(load(device+4u),references);
    for(unsigned i=0u;i<9u;i++)CHECK_EQ_U32(load(value.header_address+4u*i),header[i]);
    CHECK(memcmp(value.header,header,sizeof(header))==0);
    CHECK_EQ_U32(dsound_buffer_set_frequency(ordinary,22042u),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.frequency_sets,2u);
    /* only the measured frequency, neighbours and the title's clamp edges are refused */
    const uint32_t others[]={0u,1u,0xBBu,0xBCu,22041u,22043u,44000u,44100u,0x2EDEFu,0x2EDF0u,0x80000000u,0xFFFFFFFFu};
    for(unsigned i=0u;i<sizeof(others)/sizeof(others[0]);i++)expect_frequency_refusal(ordinary,others[i]);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.frequency_sets,2u);CHECK_EQ_U32(value.frequency,22042u);
    /* spatial: caches first, then the four steps */
    seed_params();CHECK_EQ_U32(dsound_buffer_cache_i3dl2(spatial,params,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(spatial,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(spatial,0x64BC98u,1u,0u),0u);expect_frequency_refusal(spatial,22042u);
    CHECK_EQ_U32(dsound_buffer_set_data(spatial,data+0x100u,0x5A0u),0u);
    CHECK_EQ_U32(dsound_buffer_set_volume(spatial,-10000),0u);expect_frequency_refusal(spatial,22042u);
    CHECK_EQ_U32(dsound_buffer_pause(spatial,0u),0u);
    CHECK_EQ_U32(dsound_buffer_set_frequency(spatial,22042u),0u);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));CHECK_EQ_U32(value.frequency_sets,1u);CHECK_EQ_U32(value.cache_mask,7u);
    /* ownership and policy gates */
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_frequency(0u,22042u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_frequency(spatial+4u,22042u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_frequency(device+8u,22042u));CHECK(fatal_seen);
    store(0x4124A8u,1u);expect_frequency_refusal(spatial,22042u);store(0x4124A8u,0u);
    irql=1u;expect_frequency_refusal(spatial,22042u);irql=0u;
    known_irql=false;expect_frequency_refusal(spatial,22042u);known_irql=true;
    dsound_buffer_set_irql_provider(NULL);expect_frequency_refusal(spatial,22042u);dsound_buffer_set_irql_provider(current_irql);
    dsound_buffer_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_frequency(spatial,22042u));CHECK(fatal_seen);dsound_buffer_set_enabled(true);
    CHECK(dsound_buffer_get_snapshot(spatial,&value));CHECK_EQ_U32(value.frequency_sets,1u);
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
    ordinary=create_one(false,0x5818E8u);CHECK(dsound_buffer_get_snapshot(ordinary,&value));
    CHECK_EQ_U32(value.frequency_sets,0u);expect_frequency_refusal(ordinary,22042u);
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
}
static uint32_t invoke(uint32_t entry,uint32_t caller,const uint32_t *args,unsigned count)
{
    uint32_t stack=SCRATCH_DATA+0x800u;store(stack,caller);
    for(unsigned i=0u;i<count;i++)store(stack+4u+4u*i,args[i]);
    kernel_call_frame frame={0};frame.stack_ptr=stack;frame.stack_limit=stack+4u+4u*count;
    return dsound_hle_call(entry,&frame);
}
/* T605: the sound update SetVolume(-3204, caller 0x28643) after the whole start, a host record only. */
static void test_set_volume_update(void)
{
    const uint32_t data=0x23000000u,update=0xFFFFF37Cu;/* data mapped by test_set_data */
    uint32_t spatial=create_one(true,0x5835F0u),ordinary=create_one(false,0x5818E8u);dsound_buffer_snapshot value;
    /* every earlier step of the start is needed: SetBufferData, SetVolume, Pause(0), SetFrequency */
    expect_volume_refusal(ordinary,(int32_t)update);
    CHECK_EQ_U32(dsound_buffer_set_data(ordinary,data,0x240u),0u);expect_volume_refusal(ordinary,(int32_t)update);
    CHECK_EQ_U32(dsound_buffer_set_volume(ordinary,-10000),0u);expect_volume_refusal(ordinary,(int32_t)update);
    CHECK_EQ_U32(dsound_buffer_pause(ordinary,0u),0u);expect_volume_refusal(ordinary,(int32_t)update);
    CHECK_EQ_U32(dsound_buffer_set_frequency(ordinary,22042u),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));
    uint32_t references=load(device+4u);uint32_t header[9];memcpy(header,value.header,sizeof(header));
    CHECK_EQ_U32(value.volume_sets,1u);CHECK_EQ_U32((uint32_t)value.volume,0xFFFFD8F0u);
    CHECK_EQ_U32(dsound_buffer_set_volume(ordinary,(int32_t)update),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));
    CHECK_EQ_U32((uint32_t)value.volume,update);CHECK_EQ_U32(value.volume_sets,2u);CHECK_EQ_U32(value.frequency_sets,1u);
    CHECK_EQ_U32(value.pause_sets,1u);CHECK_EQ_U32(value.data_sets,1u);CHECK_EQ_U32(value.data_address,data);
    CHECK_EQ_U32(value.cache_mask,0u);CHECK_EQ_U32(load(device+4u),references);
    for(unsigned i=0u;i<9u;i++)CHECK_EQ_U32(load(value.header_address+4u*i),header[i]);
    CHECK(memcmp(value.header,header,sizeof(header))==0);
    /* a repeat counts, the start volume replaces the record and the update is admitted again */
    CHECK_EQ_U32(dsound_buffer_set_volume(ordinary,(int32_t)update),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.volume_sets,3u);
    CHECK_EQ_U32(dsound_buffer_set_volume(ordinary,-10000),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32((uint32_t)value.volume,0xFFFFD8F0u);CHECK_EQ_U32(value.volume_sets,4u);
    CHECK_EQ_U32(dsound_buffer_set_volume(ordinary,(int32_t)update),0u);
    /* only that volume: neighbours, the clamp ends and the title's other values are refused and keep the record */
    const int32_t others[]={-3203,-3205,-3204+600,-3204-600,-3800,-3000,-4000,0,-1,1,-9999,-10001,INT32_MIN,INT32_MAX};
    for(unsigned i=0u;i<sizeof(others)/sizeof(others[0]);i++)expect_volume_refusal(ordinary,others[i]);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.volume_sets,5u);CHECK_EQ_U32((uint32_t)value.volume,update);
    /* the spatial scope (flags 0x10) is not the measured update, even fully started */
    seed_params();CHECK_EQ_U32(dsound_buffer_cache_i3dl2(spatial,params,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(spatial,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(spatial,0x64BC98u,1u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_set_data(spatial,data+0x100u,0x5A0u),0u);
    CHECK_EQ_U32(dsound_buffer_set_volume(spatial,-10000),0u);CHECK_EQ_U32(dsound_buffer_pause(spatial,0u),0u);
    CHECK_EQ_U32(dsound_buffer_set_frequency(spatial,22042u),0u);expect_volume_refusal(spatial,(int32_t)update);
    /* ownership and policy gates */
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_volume(0u,(int32_t)update));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_volume(ordinary+4u,(int32_t)update));CHECK(fatal_seen);
    store(0x4124A8u,1u);expect_volume_refusal(ordinary,(int32_t)update);store(0x4124A8u,0u);
    irql=1u;expect_volume_refusal(ordinary,(int32_t)update);irql=0u;
    known_irql=false;expect_volume_refusal(ordinary,(int32_t)update);known_irql=true;
    dsound_buffer_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_buffer_set_volume(ordinary,(int32_t)update));CHECK(fatal_seen);dsound_buffer_set_enabled(true);
    /* the production frames: each caller keeps its own volume, two arguments in order, the neighbours of the callers are refused */
    uint32_t args[2]={ordinary,update};
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));uint32_t sets=value.volume_sets;
    RUN_EXPECTING_FATAL((void)invoke(0x407A64u,0u,args,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x407A64u,0x28642u,args,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x407A64u,0x28644u,args,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x407A64u,0x28348u,args,2u));CHECK(fatal_seen);
    args[1]=0xFFFFD8F0u;RUN_EXPECTING_FATAL((void)invoke(0x407A64u,0x28643u,args,2u));CHECK(fatal_seen);
    args[0]=update;args[1]=ordinary;RUN_EXPECTING_FATAL((void)invoke(0x407A64u,0x28643u,args,2u));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.volume_sets,sets);
    args[0]=ordinary;args[1]=update;
    CHECK_EQ_U32(invoke(0x407A64u,0x28643u,args,2u),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32(value.volume_sets,sets+1u);CHECK_EQ_U32((uint32_t)value.volume,update);
    args[1]=0xFFFFD8F0u;CHECK_EQ_U32(invoke(0x407A64u,0x28348u,args,2u),0u);
    CHECK(dsound_buffer_get_snapshot(ordinary,&value));CHECK_EQ_U32((uint32_t)value.volume,0xFFFFD8F0u);
    /* exactly the return address and two arguments are readable: admitted, one argument is not */
    args[1]=update;uint32_t stack=SCRATCH_DATA+0x800u;store(stack,0x28643u);store(stack+4u,ordinary);store(stack+8u,update);
    kernel_call_frame frame={0};frame.stack_ptr=stack;frame.stack_limit=stack+12u;CHECK_EQ_U32(dsound_hle_call(0x407A64u,&frame),0u);
    frame.stack_limit=stack+8u;RUN_EXPECTING_FATAL((void)dsound_hle_call(0x407A64u,&frame));CHECK(fatal_seen);
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
}
/* T1068: exact caller and fully configured spatial data state, recording the raw value only. */
static void test_max_distance(void)
{
    const uint32_t data=0x23000000u;uint32_t buffer=create_one(true,0x5835F0u),args[3];
    dsound_buffer_snapshot before,value;seed_params();
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(buffer,params,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(buffer,0x64BC98u,1u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_set_data(buffer,data,0x5A0u),0u);
    CHECK(dsound_buffer_get_snapshot(buffer,&before));CHECK_EQ_U32(before.max_distance_sets,0u);
    uint32_t references=load(device+4u),header[9];memcpy(header,before.header,sizeof(header));
    const uint32_t values[]={0x3F800000u,0x40400000u,0x7FC12345u,0x7F800000u};
    for(unsigned i=0u;i<sizeof(values)/sizeof(values[0]);i++) {
        args[0]=buffer;args[1]=values[i];args[2]=1u;
        CHECK_EQ_U32(invoke(0x40850Eu,0x000280B5u,args,3u),0u);
        CHECK(dsound_buffer_get_snapshot(buffer,&value));CHECK_EQ_U32(value.max_distance_bits,values[i]);
        CHECK_EQ_U32(value.max_distance_sets,i+1u);CHECK_EQ_U32(value.cache_mask,7u);
        CHECK_EQ_U32(value.data_sets,1u);CHECK_EQ_U32(load(device+4u),references);
        CHECK(memcmp(value.header,header,sizeof(header))==0);
    }
    before=value;args[0]=buffer;args[1]=0x40800000u;args[2]=1u;
    RUN_EXPECTING_FATAL((void)invoke(0x40850Eu,0x000280B4u,args,3u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x40850Eu,0x000280B5u,args,2u));CHECK(fatal_seen);
    args[2]=0u;RUN_EXPECTING_FATAL((void)invoke(0x40850Eu,0x000280B5u,args,3u));CHECK(fatal_seen);
    args[2]=2u;RUN_EXPECTING_FATAL((void)invoke(0x40850Eu,0x000280B5u,args,3u));CHECK(fatal_seen);
    store(0x4124A8u,1u);args[2]=1u;
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(invoke(0x40850Eu,0x000280B5u,args,3u),0x80004005u));CHECK(!fatal_seen);store(0x4124A8u,0u);
    CHECK(dsound_buffer_get_snapshot(buffer,&value));CHECK(memcmp(&value,&before,sizeof(value))==0);

    uint32_t ordinary=create_one(false,0x5818E8u);CHECK_EQ_U32(dsound_buffer_set_data(ordinary,data+0x1000u,0x5A0u),0u);
    dsound_buffer_snapshot ordinary_before,ordinary_after;CHECK(dsound_buffer_get_snapshot(ordinary,&ordinary_before));
    args[0]=ordinary;args[1]=0x40400000u;
    RUN_EXPECTING_FATAL((void)invoke(0x40850Eu,0x000280B5u,args,3u));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(ordinary,&ordinary_after));
    CHECK(memcmp(&ordinary_before,&ordinary_after,sizeof(ordinary_before))==0);

    uint32_t fresh=create_one(true,0x5835F4u);seed_params();
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(fresh,params,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(fresh,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(fresh,0x64BC98u,1u,0u),0u);
    dsound_buffer_snapshot fresh_before,fresh_after;CHECK(dsound_buffer_get_snapshot(fresh,&fresh_before));
    args[0]=fresh;RUN_EXPECTING_FATAL((void)invoke(0x40850Eu,0x000280B5u,args,3u));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(fresh,&fresh_after));
    CHECK(memcmp(&fresh_before,&fresh_after,sizeof(fresh_before))==0);
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
}
static void test_handlers(void)
{
    /* SetBufferData is registered only while the policy is on (flags-off registry unchanged). */
    dsound_buffer_set_enabled(false);CHECK_EQ_U32(dsound_buffer_register(),4u);
    CHECK(dsound_hle_entry(0x408556u)->state!=DSOUND_ENTRY_IMPLEMENTED);
    CHECK(dsound_hle_entry(0x408C0Du)->state!=DSOUND_ENTRY_IMPLEMENTED);CHECK(dsound_hle_entry(0x407A64u)->state!=DSOUND_ENTRY_IMPLEMENTED);CHECK(dsound_hle_entry(0x407ABCu)->state!=DSOUND_ENTRY_IMPLEMENTED);CHECK(dsound_hle_entry(0x4084F2u)->state!=DSOUND_ENTRY_IMPLEMENTED);CHECK(dsound_hle_entry(0x40850Eu)->state!=DSOUND_ENTRY_IMPLEMENTED);
    dsound_buffer_set_enabled(true);
    CHECK_EQ_U32(dsound_buffer_register(),10u);CHECK(dsound_hle_entry(0x408C0Du)->state==DSOUND_ENTRY_IMPLEMENTED);
    CHECK(dsound_hle_entry(0x407A64u)->state==DSOUND_ENTRY_IMPLEMENTED);CHECK(dsound_hle_entry(0x407ABCu)->state==DSOUND_ENTRY_IMPLEMENTED);CHECK(dsound_hle_entry(0x4084F2u)->state==DSOUND_ENTRY_IMPLEMENTED);CHECK(dsound_hle_entry(0x40850Eu)->state==DSOUND_ENTRY_IMPLEMENTED);CHECK(dsound_hle_entry(0x408556u)->state==DSOUND_ENTRY_IMPLEMENTED);
    uint32_t out=0x5835F0u;seed(true);store(out,0xABABABABu);
    uint32_t args[4]={device+8u,desc,out,0u};
    RUN_EXPECTING_FATAL((void)invoke(0x4093C8u,0x388DA5u,args,4u));CHECK(fatal_seen);CHECK_EQ_U32(load(out),0xABABABABu);
    RUN_EXPECTING_FATAL((void)invoke(0x4093C8u,0x27B54u,args,4u));CHECK(fatal_seen);
    CHECK_EQ_U32(load(out),0xABABABABu);CHECK_EQ_U32(load(device+4u),6u);
    CHECK_EQ_U32(invoke(0x4093C8u,0x27AA9u,args,4u),0u);uint32_t buffer=load(out);seed_params();
    args[0]=buffer;args[1]=params;args[2]=0u;
    RUN_EXPECTING_FATAL((void)invoke(0x4085AFu,0u,args,3u));CHECK(fatal_seen);
    CHECK_EQ_U32(invoke(0x4085AFu,0x27AE8u,args,3u),0u);
    args[1]=0x3F800000u;
    RUN_EXPECTING_FATAL((void)invoke(0x408532u,0u,args,3u));CHECK(fatal_seen);
    CHECK_EQ_U32(invoke(0x408532u,0x27AF6u,args,3u),0u);
    args[1]=0x64BC98u;args[2]=1u;args[3]=0u;
    RUN_EXPECTING_FATAL((void)invoke(0x40858Bu,0u,args,4u));CHECK(fatal_seen);
    CHECK_EQ_U32(invoke(0x40858Bu,0x27B06u,args,4u),0u);
    /* SetBufferData (this, data, length): the title caller 0x282C0 only, three arguments in order. */
    args[0]=buffer;args[1]=0x23000100u;args[2]=0x5A0u;
    RUN_EXPECTING_FATAL((void)invoke(0x408C0Du,0u,args,3u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x408C0Du,0x282C5u,args,3u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x408C0Du,0x4095A6u,args,3u));CHECK(fatal_seen);
    dsound_buffer_snapshot value;CHECK(dsound_buffer_get_snapshot(buffer,&value));CHECK_EQ_U32(value.data_sets,0u);
    CHECK_EQ_U32(invoke(0x408C0Du,0x282C0u,args,3u),0u);CHECK(dsound_buffer_get_snapshot(buffer,&value));
    CHECK_EQ_U32(value.data_address,0x23000100u);CHECK_EQ_U32(value.data_length,0x5A0u);CHECK_EQ_U32(value.data_sets,1u);
    args[1]=0x5A0u;args[2]=0x23000100u;RUN_EXPECTING_FATAL((void)invoke(0x408C0Du,0x282C0u,args,3u));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(buffer,&value));CHECK_EQ_U32(value.data_address,0x23000100u);
    /* SetVolume (this, volume): the title caller 0x28348 only, two arguments in order, -10000 only. */
    args[0]=buffer;args[1]=0xFFFFD8F0u;
    RUN_EXPECTING_FATAL((void)invoke(0x407A64u,0u,args,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x407A64u,0x2834Du,args,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x407A64u,0x4095A6u,args,2u));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(buffer,&value));CHECK_EQ_U32(value.volume_sets,0u);
    args[1]=0xFFFFD8EFu;RUN_EXPECTING_FATAL((void)invoke(0x407A64u,0x28348u,args,2u));CHECK(fatal_seen);
    args[0]=0xFFFFD8F0u;args[1]=buffer;RUN_EXPECTING_FATAL((void)invoke(0x407A64u,0x28348u,args,2u));CHECK(fatal_seen);
    args[0]=buffer;args[1]=0xFFFFD8F0u;
    CHECK_EQ_U32(invoke(0x407A64u,0x28348u,args,2u),0u);CHECK(dsound_buffer_get_snapshot(buffer,&value));
    CHECK_EQ_U32((uint32_t)value.volume,0xFFFFD8F0u);CHECK_EQ_U32(value.volume_sets,1u);
    /* Pause (this, mode): the title caller 0x2751E only, two arguments in order, mode 0 only. */
    args[0]=buffer;args[1]=0u;
    RUN_EXPECTING_FATAL((void)invoke(0x407ABCu,0u,args,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x407ABCu,0x27523u,args,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x407ABCu,0x4095A6u,args,2u));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(buffer,&value));CHECK_EQ_U32(value.pause_sets,0u);
    args[1]=1u;RUN_EXPECTING_FATAL((void)invoke(0x407ABCu,0x2751Eu,args,2u));CHECK(fatal_seen);
    args[0]=0u;args[1]=buffer;RUN_EXPECTING_FATAL((void)invoke(0x407ABCu,0x2751Eu,args,2u));CHECK(fatal_seen);
    args[0]=buffer;args[1]=0u;
    CHECK_EQ_U32(invoke(0x407ABCu,0x2751Eu,args,2u),0u);CHECK(dsound_buffer_get_snapshot(buffer,&value));
    CHECK_EQ_U32(value.pause_sets,1u);
    /* SetFrequency (this, hertz): the title caller 0x27547 only, two arguments in order, 22042 only. */
    args[0]=buffer;args[1]=22042u;
    RUN_EXPECTING_FATAL((void)invoke(0x4084F2u,0u,args,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x4084F2u,0x2754Cu,args,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x4084F2u,0x4095A6u,args,2u));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(buffer,&value));CHECK_EQ_U32(value.frequency_sets,0u);
    args[1]=44000u;RUN_EXPECTING_FATAL((void)invoke(0x4084F2u,0x27547u,args,2u));CHECK(fatal_seen);
    args[0]=22042u;args[1]=buffer;RUN_EXPECTING_FATAL((void)invoke(0x4084F2u,0x27547u,args,2u));CHECK(fatal_seen);
    args[0]=buffer;args[1]=22042u;
    CHECK_EQ_U32(invoke(0x4084F2u,0x27547u,args,2u),0u);CHECK(dsound_buffer_get_snapshot(buffer,&value));
    CHECK_EQ_U32(value.frequency,22042u);CHECK_EQ_U32(value.frequency_sets,1u);
    CHECK(dsound_buffer_reset_checked());
}
static void test_permissions(void)
{
    uint32_t out=0x5835F0u;seed(true);store(out,0x11223344u);
    map_fixed(0x22000000u,4096u);uint8_t d[24],f[20];
    CHECK(kernel_guest_read_bytes(desc,d,sizeof(d)));CHECK(kernel_guest_read_bytes(format,f,sizeof(f)));
    uint32_t format_copy=0x22000100u;memcpy(d+12u,&format_copy,4u);
    CHECK(kernel_guest_write_bytes(0x22000000u,d,sizeof(d)));
    CHECK(kernel_guest_write_bytes(format_copy,f,sizeof(f)));
    CHECK(mprotect((void *)0x22000000u,4096u,PROT_NONE)==0);
    expect_create_refusal(0x22000000u,out);
    CHECK(mprotect((void *)0x22000000u,4096u,PROT_READ)==0);
    CHECK_EQ_U32(dsound_buffer_create(device+8u,0x22000000u,out,0u),0u);
    dsound_buffer_snapshot saved;CHECK(dsound_buffer_get_snapshot(load(out),&saved));
    CHECK(memcmp(saved.scope.descriptor,d,sizeof(d))==0);
    CHECK(mprotect((void *)0x22000000u,4096u,PROT_READ|PROT_WRITE)==0);
    store(0x22000000u,0xFFFFFFFFu);dsound_buffer_snapshot unchanged;
    CHECK(dsound_buffer_get_snapshot(load(out),&unchanged));
    CHECK(memcmp(&unchanged.scope,&saved.scope,sizeof(saved.scope))==0);
    CHECK(dsound_buffer_reset_checked());store(out,0x11223344u);

    CHECK(mprotect((void *)0x583000u,4096u,PROT_READ)==0);expect_create_refusal(desc,out);
    CHECK(mprotect((void *)0x583000u,4096u,PROT_READ|PROT_WRITE)==0);
    pad_header=true;uint32_t buffer=create_one(true,out);dsound_buffer_snapshot value;
    CHECK(dsound_buffer_get_snapshot(buffer,&value));uint32_t base=value.header_address;
    CHECK_EQ_U32(base&4095u,0u);uint32_t metadata_page=(base-16u)&~4095u;
    CHECK(mprotect((void *)(uintptr_t)metadata_page,4096u,PROT_NONE)==0);
    uint32_t actual;CHECK(kernel_guest_read_u32(buffer,&actual));
    CHECK(!dsound_buffer_get_snapshot(buffer,&value));CHECK(!dsound_buffer_reset_checked());
    CHECK_EQ_U32(load(device+4u),7u);
    CHECK(mprotect((void *)(uintptr_t)metadata_page,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(dsound_buffer_get_snapshot(buffer,&value));
    CHECK(mprotect((void *)(uintptr_t)base,4096u,PROT_NONE)==0);
    CHECK(!dsound_buffer_get_snapshot(buffer,&value));CHECK(!dsound_buffer_reset_checked());
    CHECK(mprotect((void *)(uintptr_t)base,4096u,PROT_READ)==0);
    CHECK(dsound_buffer_get_snapshot(buffer,&value));CHECK(dsound_buffer_reset_checked());
    CHECK_EQ_U32(load(device+4u),6u);
}
static void test_detached_cleanup(void)
{
    uint32_t buffer=create_one(true,0x5835F0u);dsound_buffer_snapshot before;
    CHECK(dsound_buffer_get_snapshot(buffer,&before));CHECK_EQ_U32(load(device+4u),7u);
    fail_destroy=true;CHECK(!dsound_buffer_reset_checked());CHECK(!fail_destroy);
    /* The lease is consumed exactly once. Child remains tracked/private until
     * checked cleanup retries; no atomic lease+free claim or extra decrement. */
    CHECK_EQ_U32(load(device+4u),6u);CHECK(guest_heap_valid(before.buffer_heap));
    CHECK_EQ_U32(load(before.header_address),0x4A1CE0u);dsound_buffer_snapshot sentinel;
    CHECK(!dsound_buffer_get_snapshot(buffer,&sentinel));
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
    CHECK(!guest_heap_valid(before.buffer_heap));CHECK_EQ_U32(guest_mem_heap_count(),1u);
    CHECK(dsound_buffer_reset_checked());CHECK_EQ_U32(load(device+4u),6u);
}
static void test_rollback_retry(void)
{
    seed(true);uint32_t out=0x5835F0u;store(out,0xAABBCCDDu);
    fail_alloc=true;fail_destroy=true;
    RUN_EXPECTING_FATAL((void)dsound_buffer_create(device+8u,desc,out,0u));CHECK(fatal_seen);
    CHECK_EQ_U32(load(out),0xAABBCCDDu);CHECK_EQ_U32(load(device+4u),6u);
    CHECK_EQ_U32(guest_mem_heap_count(),2u);CHECK(dsound_buffer_reset_checked());
    CHECK_EQ_U32(guest_mem_heap_count(),1u);CHECK_EQ_U32(load(device+4u),6u);
    CHECK(dsound_buffer_reset_checked());
}
static void unleased_foreign_replacement(void)
{
    seed(true);uint32_t out=0x5835F0u;store(out,0x12345678u);fail_alloc=true;fail_destroy=true;
    RUN_EXPECTING_FATAL((void)dsound_buffer_create(device+8u,desc,out,0u));CHECK(fatal_seen);
    uint32_t old=last_created_heap;CHECK(guest_heap_destroy(old));
    uint32_t replacement=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);
    CHECK(replacement!=0u && replacement!=old);uint32_t foreign=guest_heap_alloc(replacement,36u);
    CHECK(foreign!=0u);store(foreign,0xFACECAFEu);
    CHECK(!dsound_buffer_reset_checked());CHECK(guest_heap_valid(replacement));
    CHECK_EQ_U32(load(foreign),0xFACECAFEu);CHECK_EQ_U32(load(device+4u),6u);
    CHECK_EQ_U32(load(out),0x12345678u);
}
static void foreign_replacement(void)
{
    uint32_t buffer=create_one(true,0x5835F0u);dsound_buffer_snapshot value;
    CHECK(dsound_buffer_get_snapshot(buffer,&value));CHECK(guest_heap_destroy(value.buffer_heap));
    map_fixed(value.header_address&~4095u,4096u);store(buffer,0xFACECAFEu);
    CHECK(!dsound_buffer_reset_checked());CHECK_EQ_U32(load(buffer),0xFACECAFEu);CHECK_EQ_U32(load(device+4u),7u);
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_i3dl2(buffer,params,0u));CHECK(fatal_seen);
    CHECK_EQ_U32(load(buffer),0xFACECAFEu);CHECK(!dsound_device_reset_checked());
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();dsound_device_set_fatal(catching_fatal);
    dsound_buffer_set_fatal(catching_fatal);dsound_buffer_set_irql_provider(current_irql);
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);
    /* Synthetic safe table: all slots route to a verified unconditional stop.
     * Actual original ordered-table identity is tested separately, not copied here. */
    for(unsigned i=0u;i<4u;i++)store(0x4A1CE0u+4u*i,0x406879u);
    map_fixed(0x581000u,4096u);map_fixed(0x583000u,4096u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    device=load(SCRATCH_DATA)-8u;desc=SCRATCH_DATA+256u;format=SCRATCH_DATA+320u;params=desc+20u;
    test_policy();test_eighty_buffers();test_setters();test_set_data();test_set_volume();test_pause();test_set_frequency();test_handlers();test_set_volume_update();test_max_distance();test_permissions();test_detached_cleanup();test_rollback_retry();
    pid_t pid=fork();CHECK(pid>=0);if(pid==0){failures=0;foreign_replacement();fflush(stdout);_exit(failures?1:0);}
    if(pid>0){int status;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
    pid=fork();CHECK(pid>=0);if(pid==0){failures=0;unleased_foreign_replacement();fflush(stdout);_exit(failures?1:0);}
    if(pid>0){int status;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
    CHECK(dsound_buffer_reset_checked());CHECK(dsound_device_reset_checked());environment_end();
    printf("passive buffer: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
