/* SPDX-License-Identifier: GPL-3.0-or-later
 * Reuse only the SDL/Vulkan readback transport. The independent review main
 * uses new quadrant-coded mip pixels and literal LOD/cap expectations. */
#define main t995_existing_transport_main
#include "test_live_vk_texture.c"
#undef main
static const uint32_t t995_dim[6]={128u,64u,32u,16u,8u,4u};
static const uint32_t t995_offset[6]={0u,16384u,20480u,21504u,21760u,21824u};
static uint32_t t995_square_morton(uint32_t x,uint32_t y)
{
 uint32_t value=0;
 for(unsigned bit=0;bit<7u;bit++){value|=((x>>bit)&1u)<<(bit*2u);value|=((y>>bit)&1u)<<(bit*2u+1u);}
 return value;
}
static uint8_t t995_alpha(unsigned level,bool right,bool bottom)
{return (uint8_t)(level*32u+(right?7u:1u)+(bottom?13u:3u));}
static void t995_pixels(const uint8_t *pixels,unsigned level)
{
 for(unsigned y=0;y<16u;y++)for(unsigned x=0;x<16u;x++){
  const uint8_t *pixel=pixels+(y*16u+x)*4u;
  CHECK(pixel[0]==255u && pixel[1]==255u && pixel[2]==255u && pixel[3]==t995_alpha(level,x>=8u,y>=8u));
 }
}
int main(void)
{
 if(!SDL_Init(SDL_INIT_VIDEO))return unavailable(SDL_GetError());
 SDL_Window *window=SDL_CreateWindow("T995 independent mip review",96,64,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
 if(!window)return unavailable(SDL_GetError());
 const char *error=NULL;
 gpu_window *renderer=gpu_window_create(window,&error);if(!renderer)return unavailable(error);
 gpu_window_native native;CHECK(gpu_window_get_native(renderer,&native));
 PFN_vkGetInstanceProcAddr gipa=(PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
 live_vk_texture_set *set=live_vk_texture_create(&native,gipa,&error);CHECK(set!=NULL);if(!set)return 1;
 probe p;CHECK(probe_init(&p,&native,gipa,live_vk_texture_set_layout(set)));
 probe_image target;CHECK(probe_image_create(&p,&target,16u,16u));
 live_texture_cache *cache=calloc(1,sizeof *cache);CHECK(cache!=NULL);if(!cache)return 1;
 live_texture_cache_init(cache,true);
 for(unsigned level=0;level<6u;level++)for(uint32_t y=0;y<t995_dim[level];y++)for(uint32_t x=0;x<t995_dim[level];x++)
  guest[t995_offset[level]+t995_square_morton(x,y)]=t995_alpha(level,x>=t995_dim[level]/2u,y>=t995_dim[level]/2u);
 live_texture_binding binding={.format=0x07761929u,.data=GUEST_BASE};
 live_texture_result result;live_texture_lookup(cache,&binding,reader,NULL,&result);
 CHECK(result.source==LIVE_TEXTURE_SOURCE_GUEST && result.upload_bytes==87360u && result.levels==6u);
 live_vk_texture_bound bound;uint8_t pixels[16u*16u*4u];
 for(unsigned level=0;level<6u;level++){
  live_vk_sampler_desc sampler=live_vk_sampler_decode_mips(0x303u,0x01030000u,(level<<18)|(level<<6),6u);
  CHECK(sampler.ok && sampler.min_lod==(float)level && sampler.max_lod==(float)level);
  CHECK(live_vk_texture_bind(set,cache,&result,&sampler,&bound,&error));
  CHECK(probe_render(&p,bound.descriptor,&target,pixels));t995_pixels(pixels,level);
  live_texture_lookup(cache,&binding,reader,NULL,&result);
 }
 CHECK(live_vk_texture_get_stats(set).uploads==1u && live_vk_texture_get_stats(set).upload_bytes==87360u);
 CHECK(live_vk_texture_get_stats(set).sampler_creates==6u);
 live_texture_mark_uploaded(cache,result.entry,result.generation);
 live_texture_lookup(cache,&binding,reader,NULL,&result);CHECK(!result.needs_upload && result.upload_bytes==0u);
 live_vk_sampler_desc lod0=live_vk_sampler_decode_mips(0x303u,0x01020000u,5u<<6,6u);
 CHECK(lod0.ok && lod0.max_lod==0.0f);
 CHECK(live_vk_texture_bind(set,cache,&result,&lod0,&bound,&error));
 CHECK(probe_render(&p,bound.descriptor,&target,pixels));t995_pixels(pixels,0u);
 live_vk_sampler_desc reversed=live_vk_sampler_decode_mips(0x303u,0x01030000u,(5u<<18)|(1u<<6),6u);
 CHECK(!reversed.ok && strcmp(reversed.refusal,"reversed LOD clamps")==0);
 CHECK(!live_vk_texture_bind(set,cache,&result,&reversed,&bound,&error));
 binding.mip_limit=2u;live_texture_lookup(cache,&binding,reader,NULL,&result);
 CHECK(result.levels==2u && result.upload_bytes==81920u);
 live_vk_sampler_desc capped=live_vk_sampler_decode_mips(0x303u,0x01030000u,(5u<<18)|(5u<<6),2u);
 CHECK(capped.ok && capped.min_lod==1.0f && capped.max_lod==1.0f);
 CHECK(live_vk_texture_bind(set,cache,&result,&capped,&bound,&error));
 CHECK(probe_render(&p,bound.descriptor,&target,pixels));t995_pixels(pixels,1u);
 /* A byte beyond the capped source cannot dirty its image; full view still tracks it. */
 CHECK(live_texture_note_write(cache,GUEST_BASE+21839u,1u)==1u);
 CHECK(cache->entries[result.entry].valid);
 binding.mip_limit=0u;guest[21839u]=239u;
 live_texture_lookup(cache,&binding,reader,NULL,&result);CHECK(result.needs_upload && result.levels==6u);
 live_vk_sampler_desc tail=live_vk_sampler_decode_mips(0x303u,0x01030000u,(5u<<18)|(5u<<6),6u);
 CHECK(live_vk_texture_bind(set,cache,&result,&tail,&bound,&error));
 CHECK(probe_render(&p,bound.descriptor,&target,pixels));
 CHECK(pixels[(15u*16u+15u)*4u+3u]==239u && pixels[3u]==t995_alpha(5u,false,false));
 printf("T995 Vulkan: device=%s checks=%d failures=%d uploads=%llu\n",gpu_window_device_name(renderer),checks,failures,(unsigned long long)live_vk_texture_get_stats(set).uploads);
 p.vkQueueWaitIdle(native.queue);probe_image_destroy(&p,&target);
 p.vkDestroyPipeline(native.device,p.pipeline,NULL);p.vkDestroyPipelineLayout(native.device,p.layout,NULL);p.vkDestroyRenderPass(native.device,p.render_pass,NULL);
 live_vk_texture_destroy(set);live_texture_cache_free(cache);free(cache);gpu_window_destroy(renderer);SDL_DestroyWindow(window);SDL_Quit();return failures?1:0;
}
