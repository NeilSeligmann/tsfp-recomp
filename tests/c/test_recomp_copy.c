/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic registered metadata only; libc itself performs every copy. */
#include "test_d3d8_support.h"
#include "d3d8_resource.h"
#include "d3d8_gpu_pgraph.h"
#include <pthread.h>
#define SOURCE 0x00665500u
#define DEST 0x00665600u
static uint32_t backing;
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00665000u,0x1000u);
    const guest_region_request request={.bytes=4096u,.fixed_base=0x9125A000u,.contiguous=true,
                                        .state=MEM_COMMIT,.protect=PAGE_READWRITE};
    nt_status status;
    backing=guest_region_alloc(&request,&status);
    CHECK_EQ_U32(backing,request.fixed_base);
    store(SOURCE,0x40001u);store(SOURCE+4u,128u);store(SOURCE+12u,0x04410029u);store(SOURCE+16u,0u);
    (void)d3d8_register_resource(SOURCE,backing);
}
static void expect_alias(uint32_t header,uint32_t expected)
{
    uint32_t address=0u;uint64_t identity=0u;const char *why=NULL;
    CHECK(d3d8_resource_try_alias(header,load(header+4u),256u,&address,&identity,&why)==D3D8_RESOURCE_ALIAS_RESOLVED);
    CHECK_EQ_U32(address,expected);CHECK(identity!=0u);
}
static void test_real_memcpy_header_transfer(void)
{
    initialise();
    const uint32_t unrelated=0x00665700u;
    store(unrelated,0x40001u);store(unrelated+4u,load(SOURCE+4u));store(unrelated+12u,load(SOURCE+12u));
    uint32_t address=0u;uint64_t identity=0u;const char *why=NULL;
    CHECK(d3d8_resource_try_alias(unrelated,load(unrelated+4u),256u,&address,&identity,&why)==D3D8_RESOURCE_ALIAS_ABSENT);
    store(DEST,0x40001u);store(DEST+4u,512u);store(DEST+12u,load(SOURCE+12u));
    (void)d3d8_register_resource(DEST,backing);
    CHECK(memcpy((void *)(uintptr_t)DEST,(const void *)(uintptr_t)SOURCE,52u)==(void *)(uintptr_t)DEST);
    expect_alias(DEST,backing+128u);
    store(SOURCE+4u,0u); /* original compactor clears/reuses source later */
    expect_alias(DEST,backing+128u);
    live_texture_cache cache;live_texture_cache_init(&cache,true);
    live_texture_binding binding={DEST,load(DEST+12u),0u,load(DEST+4u), 0u};
    live_texture_result first,second;store(backing+128u,0x44332211u);
    live_texture_lookup_resolved(&cache,&binding,d3d8_gpu_read_virtual,NULL,d3d8_gpu_resolve_texture,NULL,&first);
    CHECK(first.source==LIVE_TEXTURE_SOURCE_GUEST);CHECK_EQ_U32(first.rgba[0],0x11u);
    store(backing+128u,0x88776655u);
    live_texture_lookup_resolved(&cache,&binding,d3d8_gpu_read_virtual,NULL,d3d8_gpu_resolve_texture,NULL,&second);
    CHECK(second.generation>first.generation);CHECK_EQ_U32(second.rgba[0],0x55u);
    CHECK_EQ_U32(live_texture_note_write(&cache,binding.data,1u),1u);
    live_texture_cache_free(&cache);
    CHECK(guest_region_free(backing));
    CHECK(d3d8_resource_try_alias(DEST,load(DEST+4u),4u,&address,&identity,&why)==D3D8_RESOURCE_ALIAS_REFUSED);
    CHECK(strstr(why,"stale")!=NULL);
    environment_end();
}
static void test_overlap_and_multiple_headers(void)
{
    initialise();
    store(SOURCE+64u,0x40001u);store(SOURCE+68u,512u);store(SOURCE+76u,0x04410029u);
    (void)d3d8_register_resource(SOURCE+64u,backing);
    /* libc memmove snapshots overlapping bytes; observer must snapshot metadata first. */
    CHECK(memmove((void *)(uintptr_t)(SOURCE+4u),(const void *)(uintptr_t)SOURCE,84u)==(void *)(uintptr_t)(SOURCE+4u));
    expect_alias(SOURCE+4u,backing+128u);expect_alias(SOURCE+68u,backing+512u);
    environment_end();
}
static void test_incomplete_unverified_and_changed_copies(void)
{
    initialise();
    (void)memcpy((void *)(uintptr_t)DEST,(const void *)(uintptr_t)SOURCE,19u);
    uint32_t address=0u;uint64_t identity=0u;const char *why=NULL;
    CHECK(d3d8_resource_try_alias(DEST,load(DEST+4u),4u,&address,&identity,&why)==D3D8_RESOURCE_ALIAS_REFUSED);
    CHECK(strstr(why,"copy")!=NULL);
    store(SOURCE+12u,0x08811929u); /* historical Data match does not verify source metadata */
    (void)memcpy((void *)(uintptr_t)(DEST+64u),(const void *)(uintptr_t)SOURCE,20u);
    CHECK(d3d8_resource_try_alias(DEST+64u,load(DEST+68u),4u,&address,&identity,&why)==D3D8_RESOURCE_ALIAS_REFUSED);
    store(SOURCE+12u,0x04410029u);
    void *copy=d3d8_resource_copy_begin(DEST+128u,SOURCE,20u);
    CHECK(copy!=NULL);
    /* Copy bytes with scalar stores then corrupt one field before transaction commit. */
    for(uint32_t i=0u;i<5u;i++)store(DEST+128u+4u*i,load(SOURCE+4u*i));
    store(DEST+140u,0x08811929u);
    CHECK(d3d8_resource_copy_end(copy)==0u);
    CHECK(d3d8_resource_try_alias(DEST+128u,load(DEST+132u),4u,&address,&identity,&why)==D3D8_RESOURCE_ALIAS_REFUSED);
    copy=d3d8_resource_copy_begin(DEST+192u,SOURCE,20u);
    CHECK(copy!=NULL);
    for(uint32_t i=0u;i<5u;i++)store(DEST+192u+4u*i,load(SOURCE+4u*i));
    CHECK(guest_region_free(backing));
    CHECK(d3d8_resource_copy_end(copy)==0u);
    CHECK(d3d8_resource_try_alias(DEST+192u,load(DEST+196u),4u,&address,&identity,&why)==D3D8_RESOURCE_ALIAS_REFUSED);
    CHECK(d3d8_resource_copy_begin(UINT32_MAX-4u,SOURCE,20u)==NULL);
    CHECK(d3d8_resource_copy_begin(DEST,UINT32_MAX-4u,20u)==NULL);
    environment_end();
}
static void *copy_thread(void *context)
{
    const uint32_t destination=*(const uint32_t *)context;
    for(unsigned i=0u;i<1000u;i++)
        (void)memcpy((void *)(uintptr_t)destination,(const void *)(uintptr_t)SOURCE,20u);
    return NULL;
}
static void test_concurrent_separate_header_copies(void)
{
    initialise();
    const uint32_t destinations[2]={DEST+512u,DEST+576u};
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0],NULL,copy_thread,(void *)&destinations[0])==0);
    CHECK(pthread_create(&threads[1],NULL,copy_thread,(void *)&destinations[1])==0);
    CHECK(pthread_join(threads[0],NULL)==0);CHECK(pthread_join(threads[1],NULL)==0);
    expect_alias(destinations[0],backing+128u);expect_alias(destinations[1],backing+128u);
    environment_end();
}

int main(void)
{
    test_real_memcpy_header_transfer();test_overlap_and_multiple_headers();
    test_incomplete_unverified_and_changed_copies();
    test_concurrent_separate_header_copies();
    printf("%d checks, %d failures\n",checks,failures);
    return failures==0?0:1;
}
