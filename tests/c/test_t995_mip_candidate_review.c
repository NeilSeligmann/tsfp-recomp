/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent finite inferred A8 vector/corner and resolved-cache controls.
 * Only Format/Data/header identity have a retained original receipt; the Size,
 * draw cap and synthetic pixels here are test inputs, not reconstructed state. */
#include "live_texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); } } while(0)
static const uint32_t dimensions[6]={128u,64u,32u,16u,8u,4u};
static const uint32_t offsets[6]={0u,16384u,20480u,21504u,21760u,21824u};
static const uint32_t lengths[6]={16384u,4096u,1024u,256u,64u,16u};
static const uint32_t corners[6][4]={
 {0u,5461u,10922u,16383u},{0u,1365u,2730u,4095u},
 {0u,341u,682u,1023u},{0u,85u,170u,255u},{0u,21u,42u,63u},{0u,5u,10u,15u}};
static uint8_t source[21840u];
static uint32_t requested, resolved_bytes;
static size_t available=sizeof source;
static uint64_t identity=61u;
static bool resolver(void *ctx,const live_texture_binding *binding,uint32_t bytes,uint32_t *address,uint64_t *id,const char **refusal)
{
 (void)ctx;(void)refusal;
 CHECK(binding->format==0x07761929u && binding->data==0x0203D200u);
 resolved_bytes=bytes;*address=0x4203D200u;*id=identity;return true;
}
static bool reader(void *ctx,uint32_t address,void *out,size_t bytes)
{
 (void)ctx;CHECK(address==0x4203D200u);requested=(uint32_t)bytes;
 if(bytes>available) return false;
 memcpy(out,source,bytes);return true;
}
static void fill(void)
{
 for(unsigned level=0;level<6;level++) {
  memset(source+offsets[level],(int)(level*30u+10u),lengths[level]);
  for(unsigned corner=0;corner<4;corner++) source[offsets[level]+corners[level][corner]]=(uint8_t)(level*30u+corner+1u);
 }
}
static void check_level_pixels(const uint8_t *rgba,unsigned level,uint32_t rgba_offset)
{
 uint32_t side=dimensions[level];
 uint32_t pixel[4]={0u,side-1u,(side-1u)*side,side*side-1u};
 for(unsigned corner=0;corner<4;corner++) {
  const uint8_t *p=rgba+rgba_offset+pixel[corner]*4u;
  CHECK(p[0]==255u && p[1]==255u && p[2]==255u && p[3]==level*30u+corner+1u);
 }
}
int main(void)
{
 fill();
 live_texture_plan extra;
 live_texture_binding rectangular={.format=0x04781929u};
 CHECK(live_texture_plan_binding(&rectangular,true,&extra) && extra.source_bytes==2735u);
 rectangular.mip_limit=3u;
 CHECK(live_texture_plan_binding(&rectangular,true,&extra) && extra.source_bytes==2688u);
 rectangular.format=0x02450C29u;rectangular.mip_limit=0u;
 CHECK(live_texture_plan_binding(&rectangular,true,&extra) && extra.source_bytes==72u);
 rectangular.format=0x04250E29u;
 CHECK(live_texture_plan_binding(&rectangular,true,&extra) && extra.source_bytes==144u);
 live_texture_binding binding={.header=0x42010C58u,.format=0x07761929u,.data=0x0203D200u};
 live_texture_plan plan;
 CHECK(!live_texture_plan_binding(&binding,false,&plan));
 CHECK(strcmp(plan.refusal,"inferred mip chain")==0);
 CHECK(live_texture_plan_binding(&binding,true,&plan));
 CHECK(!plan.measured && plan.levels==6u && plan.source_bytes==21840u && plan.rgba_bytes==87360u);
 uint8_t *rgba=malloc(87360u+32u);CHECK(rgba!=NULL);if(!rgba)return 1;
 memset(rgba,0xCD,87360u+32u);
 CHECK(!live_texture_decode(&plan,source,21839u,rgba));
 CHECK(rgba[0]==0xCD && rgba[87359u]==0xCD && rgba[87360u]==0xCD);
 CHECK(live_texture_decode(&plan,source,sizeof source,rgba));
 uint32_t rgba_offset=0;
 for(unsigned level=0;level<6;level++) {
  CHECK(plan.mip[level].width==dimensions[level] && plan.mip[level].height==dimensions[level]);
  CHECK(plan.mip[level].source_offset==offsets[level] && plan.mip[level].source_bytes==lengths[level]);
  CHECK(plan.mip[level].rgba_offset==rgba_offset && plan.mip[level].rgba_bytes==4u*lengths[level]);
  check_level_pixels(rgba,level,rgba_offset);rgba_offset+=4u*lengths[level];
 }
 for(unsigned i=0;i<32u;i++)CHECK(rgba[87360u+i]==0xCD);
 live_texture_cache *cache=calloc(1,sizeof *cache);CHECK(cache!=NULL);if(!cache)return 1;
 live_texture_cache_init(cache,true);
 live_texture_result full,cap,again;
 live_texture_lookup_resolved(cache,&binding,reader,NULL,resolver,NULL,&full);
 CHECK(full.source==LIVE_TEXTURE_SOURCE_GUEST && full.levels==6u && full.upload_bytes==87360u);
 CHECK(requested==21840u && resolved_bytes==21840u);
 CHECK(cache->entries[full.entry].read_address==0x4203D200u && cache->entries[full.entry].backing_identity==61u);
 live_texture_mark_uploaded(cache,full.entry,full.generation);
 live_texture_lookup_resolved(cache,&binding,reader,NULL,resolver,NULL,&again);
 CHECK(again.entry==full.entry && !again.needs_upload && again.upload_bytes==0u);
 binding.mip_limit=3u;
 live_texture_lookup_resolved(cache,&binding,reader,NULL,resolver,NULL,&cap);
 CHECK(cap.source==LIVE_TEXTURE_SOURCE_GUEST && cap.levels==3u && cap.upload_bytes==86016u);
 CHECK(requested==21504u && resolved_bytes==21504u && cap.entry!=full.entry);
 live_texture_mark_uploaded(cache,cap.entry,cap.generation);
 CHECK(live_texture_note_write(cache,0x4203D200u+21840u,1u)==0u);
 CHECK(live_texture_note_write(cache,0x4203D200u+21839u,1u)==1u);
 CHECK(!cache->entries[full.entry].valid && cache->entries[cap.entry].valid);
 binding.mip_limit=0u;
 source[21839u]=221u;
 live_texture_lookup_resolved(cache,&binding,reader,NULL,resolver,NULL,&again);
 CHECK(again.entry==full.entry && again.generation!=full.generation && again.needs_upload);
 CHECK(again.rgba[87359u]==221u);
 live_texture_mark_uploaded(cache,again.entry,again.generation);
 source[21839u]=222u; /* No notification: resolved backing snapshot must notice. */
 live_texture_lookup_resolved(cache,&binding,reader,NULL,resolver,NULL,&again);
 CHECK(again.needs_upload && again.rgba[87359u]==222u);
 identity=62u;
 live_texture_lookup_resolved(cache,&binding,reader,NULL,resolver,NULL,&again);
 CHECK(again.entry!=full.entry && again.needs_upload);
 available=21839u;
 live_texture_lookup_resolved(cache,&binding,reader,NULL,resolver,NULL,&again);
 CHECK(again.source==LIVE_TEXTURE_SOURCE_REFUSED && strcmp(again.refusal,"unreadable bytes")==0);
 binding.mip_limit=3u;
 live_texture_lookup_resolved(cache,&binding,reader,NULL,resolver,NULL,&again);
 CHECK(again.source==LIVE_TEXTURE_SOURCE_GUEST && requested==21504u);
 live_texture_cache_free(cache);free(cache);free(rgba);
 printf("T995 CPU: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
