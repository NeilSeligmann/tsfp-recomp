/* SPDX-License-Identifier: GPL-3.0-or-later
 * Headerless inferred texture binding; no Register/alias semantics are claimed. */
#include "guest_mem.h"
#include "kernel_call.h"
#include "d3d8_gpu_pgraph.h"
#include "d3d8_resource.h"
#include "live_texture.h"
#include <stdio.h>
#include <string.h>
static unsigned checks,failures,unexpected_alias;
#define CHECK(x) do {checks++;if(!(x)){failures++;fprintf(stderr,"FAIL %u %s\n",__LINE__,#x);}}while(0)
kernel_log_fn kernel_hle_log(void) {return printf;}
/* This guard adapter is unreachable for header0; it never grants an alias. */
d3d8_resource_alias_result d3d8_resource_try_alias(uint32_t h,uint32_t d,uint32_t b,uint32_t *a,uint64_t *i,const char **r)
{(void)h;(void)d;(void)b;(void)a;(void)i;(void)r;unexpected_alias++;return D3D8_RESOURCE_ALIAS_REFUSED;}
static bool reader(void *unused,uint32_t address,void *out,size_t bytes)
{(void)unused;return kernel_guest_read_bytes(address,out,bytes);}
static kernel_guest_ptr allocate(uint32_t address)
{
    const guest_region_request request={.bytes=4096u,.fixed_base=address,
      .lowest_physical=0x00480000u,.highest_physical=0x00480FFFu,.alignment=4096u,
      .protect=PAGE_READWRITE,.state=MEM_COMMIT,.contiguous=true};
    nt_status status;kernel_guest_ptr got=guest_region_alloc(&request,&status);
    CHECK(got==address && status==STATUS_SUCCESS);return got;
}
int main(void)
{
    guest_mem_reset();kernel_guest_ptr a=allocate(0x91400000u);if(!a)return 1;
    memset((void *)(uintptr_t)a,37,4096);
    const live_texture_binding binding={.format=0x04410029u,.data=0x00480000u}; /* inferred16x16Y8 */
    live_texture_cache cache;live_texture_cache_init(&cache,true);live_texture_result first,hit,again;
    live_texture_lookup_resolved(&cache,&binding,reader,NULL,d3d8_gpu_resolve_texture,NULL,&first);
    CHECK(first.source==LIVE_TEXTURE_SOURCE_GUEST && first.needs_upload);
    CHECK(cache.entries[first.entry].read_address==a);
    uint64_t old=cache.entries[first.entry].backing_identity;CHECK(old!=0);
    live_texture_mark_uploaded(&cache,first.entry,first.generation);
    live_texture_lookup_resolved(&cache,&binding,reader,NULL,d3d8_gpu_resolve_texture,NULL,&hit);
    CHECK(hit.source==LIVE_TEXTURE_SOURCE_GUEST && !hit.needs_upload && hit.entry==first.entry);
    CHECK(guest_region_free(a));
    live_texture_lookup_resolved(&cache,&binding,reader,NULL,d3d8_gpu_resolve_texture,NULL,&again);
    CHECK(again.source==LIVE_TEXTURE_SOURCE_REFUSED);
    a=allocate(0x91400000u);if(!a)return 1;memset((void *)(uintptr_t)a,37,4096);
    live_texture_lookup_resolved(&cache,&binding,reader,NULL,d3d8_gpu_resolve_texture,NULL,&again);
    CHECK(again.source==LIVE_TEXTURE_SOURCE_GUEST && again.needs_upload);
    CHECK(again.entry!=first.entry);CHECK(cache.entries[again.entry].backing_identity>old);
    CHECK(again.rgba[0]==37 && again.rgba[3]==255);
    live_texture_mark_uploaded(&cache,again.entry,again.generation);
    old=cache.entries[again.entry].backing_identity;guest_mem_reset();
    a=allocate(0x91401000u);if(!a)return 1;memset((void *)(uintptr_t)a,82,4096);
    live_texture_lookup_resolved(&cache,&binding,reader,NULL,d3d8_gpu_resolve_texture,NULL,&hit);
    CHECK(hit.source==LIVE_TEXTURE_SOURCE_GUEST && hit.needs_upload);
    CHECK(cache.entries[hit.entry].read_address==a && cache.entries[hit.entry].backing_identity>old);
    CHECK(hit.rgba[0]==82 && hit.rgba[3]==255);
    live_texture_binding refused=binding;refused.data=UINT32_MAX-3u;
    uint32_t address=0x87654321u;uint64_t identity=0;const char *why=NULL;
    CHECK(!d3d8_gpu_resolve_texture(NULL,&refused,256u,&address,&identity,&why));
    CHECK(address==0x87654321u && why!=NULL);
    CHECK(unexpected_alias==0);live_texture_cache_free(&cache);guest_mem_reset();
    CHECK(guest_mem_region_count()==0 && guest_mem_mapped_bytes()==0);
    printf("CACHE %u checks %u failures\n",checks,failures);return failures?1:0;
}
