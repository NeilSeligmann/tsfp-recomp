/* Literal xemu register expectations plus real Vulkan create-state/readback controls. */
#define main t792_reference_main
#include "test_live_vk_texture.c"
#undef main
#include "gpu_pgraph_replay.h"
#include "live_vk_bind.h"
static PFN_vkGetDeviceProcAddr actual_proc;
static VkSamplerCreateInfo observed;
static unsigned creates;
static VkResult VKAPI_CALL capture_sampler(VkDevice d,const VkSamplerCreateInfo *i,const VkAllocationCallbacks *a,VkSampler *s)
{
    observed=*i;creates++;
    PFN_vkCreateSampler real=(PFN_vkCreateSampler)actual_proc(d,"vkCreateSampler");
    return real(d,i,a,s);
}
static PFN_vkVoidFunction VKAPI_CALL capture_proc(VkDevice d,const char *name)
{
    if(strcmp(name,"vkCreateSampler")==0)return (PFN_vkVoidFunction)capture_sampler;
    return actual_proc(d,name);
}
static bool single_binding(void *context,size_t draw,uint32_t stage,live_texture_binding *out,const char **refusal)
{
    (void)context;(void)draw;(void)stage;(void)refusal;
    *out=(live_texture_binding){0u,FORMAT(0x06,1,1),0u,GUEST_BASE,0u};return true;
}
int main(void)
{
    static const uint32_t words[]={0x4003FFC0u,0x4003FFD0u,0x4003FFE0u,0x4003FFF0u};
    static const uint32_t expected[]={1u,2u,4u,8u};
    for(unsigned i=0;i<4;i++) {
        live_vk_sampler_desc a=live_vk_sampler_decode(0x303u,0x02062000u);
        CHECK(live_vk_sampler_apply_control(&a,words[i]));CHECK(a.max_anisotropy==expected[i]);
        CHECK(a.measured==(i==0));
        live_vk_sampler_desc b=live_vk_sampler_decode_mips(0x303u,0x02062000u,words[i],6u);
        CHECK(b.ok&&b.max_anisotropy==expected[i]&&b.max_lod==5.0f);
    }
    const uint32_t forbidden[]={1u,2u,4u,8u,0x80000000u};
    for(unsigned i=0;i<5;i++) {
        live_vk_sampler_desc a=live_vk_sampler_decode(0x303u,0x02062000u);
        CHECK(!live_vk_sampler_apply_control(&a,0x4003FFC0u|forbidden[i]));
        CHECK(!a.ok&&strstr(a.refusal,"Control0")!=NULL);
    }
    for(unsigned stage=0;stage<4;stage++) {
        for(unsigned code=0;code<4;code++) {
            gpu_pgraph_state state={0};
            state.output_written[GPU_PGRAPH_OUT_TEXTURE_CONTROL0+stage]=true;
            state.output[GPU_PGRAPH_OUT_TEXTURE_CONTROL0+stage]=words[code];
            gpu_pgraph_backend backend={.output_groups=GPU_PGRAPH_OUTPUT_TEXTURE,
                .allowed_inferences=GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE|GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING};
            gpu_pgraph_output output;gpu_pgraph_report report={0};
            CHECK(gpu_pgraph_resolve_output(&state,&backend,32,32,&output,&report)==GPU_PGRAPH_OK);
            backend.allowed_inferences=GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE;
            CHECK(gpu_pgraph_resolve_output(&state,&backend,32,32,&output,&report)==(code?GPU_PGRAPH_ERR_UNMEASURED:GPU_PGRAPH_OK));
            backend.allowed_inferences=GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING;
            CHECK(gpu_pgraph_resolve_output(&state,&backend,32,32,&output,&report)==GPU_PGRAPH_ERR_UNMEASURED);
            backend.allowed_inferences=GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE|GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING;
            for(unsigned bad=0;bad<5;bad++) {
                state.output[GPU_PGRAPH_OUT_TEXTURE_CONTROL0+stage]=words[code]|forbidden[bad];
                CHECK(gpu_pgraph_resolve_output(&state,&backend,32,32,&output,&report)==GPU_PGRAPH_ERR_UNMEASURED);
            }
        }
    }
    if(!SDL_Init(SDL_INIT_VIDEO))return unavailable(SDL_GetError());
    SDL_Window *window=SDL_CreateWindow("t1051",32,32,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    if(!window)return unavailable(SDL_GetError());
    const char *error=NULL;gpu_window *renderer=gpu_window_create(window,&error);
    if(!renderer)return unavailable(error);
    gpu_window_native native;CHECK(gpu_window_get_native(renderer,&native));
    CHECK(native.sampler_anisotropy);
    actual_proc=native.get_device_proc_addr;native.get_device_proc_addr=capture_proc;
    PFN_vkGetInstanceProcAddr gipa=(PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    VkPhysicalDeviceProperties props;
    PFN_vkGetPhysicalDeviceProperties getprops=(PFN_vkGetPhysicalDeviceProperties)gipa(native.instance,"vkGetPhysicalDeviceProperties");
    getprops(native.physical_device,&props);
    live_vk_texture_set *set=live_vk_texture_create(&native,gipa,&error);CHECK(set!=NULL);
    live_texture_cache *cache=calloc(1,sizeof(*cache));live_texture_cache_init(cache,true);
    for(unsigned i=0;i<16;i+=4) {guest[i]=0;guest[i+1]=0;guest[i+2]=255;guest[i+3]=255;}
    live_texture_binding binding={0u,FORMAT(0x06,1,1),0u,GUEST_BASE,0u};
    live_texture_result result;live_texture_lookup(cache,&binding,reader,NULL,&result);
    CHECK(result.source==LIVE_TEXTURE_SOURCE_GUEST);
    if(result.source!=LIVE_TEXTURE_SOURCE_GUEST) { fprintf(stderr,"fixture refused: %s\n",result.refusal);return 1; }
    probe p;CHECK(probe_init(&p,&native,gipa,live_vk_texture_set_layout(set)));
    probe_image image;CHECK(probe_image_create(&p,&image,4,4));
    live_vk_texture_bound bound[4];CHECK(live_vk_texture_begin_batch(set));
    for(unsigned i=0;i<4;i++) {
        live_vk_sampler_desc a=live_vk_sampler_decode(0x303u,0x02062000u);
        CHECK(live_vk_sampler_apply_control(&a,words[i]));
        CHECK(live_vk_texture_bind(set,cache,&result,&a,&bound[i],&error));
        CHECK(creates==i+1);CHECK(observed.anisotropyEnable==(i?VK_TRUE:VK_FALSE));
        float max=(float)expected[i];if(max>props.limits.maxSamplerAnisotropy)max=props.limits.maxSamplerAnisotropy;
        CHECK(observed.maxAnisotropy==max);
        if(i)CHECK(bound[i].sampler!=bound[i-1].sampler&&bound[i].descriptor!=bound[i-1].descriptor);
        uint8_t pixels[64];CHECK(probe_render(&p,bound[i].descriptor,&image,pixels));
        for(unsigned j=0;j<64;j+=4)CHECK(pixels[j]==255&&pixels[j+1]==0&&pixels[j+2]==0&&pixels[j+3]==255);
        result.needs_upload=false;
        live_vk_texture_bound again;CHECK(live_vk_texture_bind(set,cache,&result,&a,&again,&error));
        CHECK(creates==i+1&&again.sampler==bound[i].sampler&&again.descriptor==bound[i].descriptor);
    }
    CHECK(live_vk_texture_end_batch(set));
    live_vk_bind_source source={.binding=single_binding,.reader=reader};
    live_vk_bind *bridge=live_vk_bind_create(cache,set,&source);CHECK(bridge!=NULL);
    gpu_pgraph_backend gate={.allowed_inferences=GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING};
    live_vk_bind_backend(bridge,&gate);
    live_vk_texture_hook hook=live_vk_bind_hook(bridge);
    gpu_pgraph_state snapshot={0};
    snapshot.output_written[GPU_PGRAPH_OUT_TEXTURE_ADDRESS]=true;
    snapshot.output_written[GPU_PGRAPH_OUT_TEXTURE_FILTER]=true;
    snapshot.output_written[GPU_PGRAPH_OUT_TEXTURE_CONTROL0]=true;
    snapshot.output[GPU_PGRAPH_OUT_TEXTURE_ADDRESS]=0x303u;
    snapshot.output[GPU_PGRAPH_OUT_TEXTURE_FILTER]=0x02062000u;
    snapshot.output[GPU_PGRAPH_OUT_TEXTURE_CONTROL0]=0x4003FFD0u;
    char refusal[256];live_vk_texture_binding hooked;
    CHECK(hook.bind(hook.context,0,&snapshot,0,&hooked,refusal,sizeof(refusal)));
    CHECK(hooked.sampler==bound[1].sampler); /* Control0 survives the single-level bridge. */
    gate.allowed_inferences=0;live_vk_bind_backend(bridge,&gate);
    CHECK(!hook.bind(hook.context,0,&snapshot,0,&hooked,refusal,sizeof(refusal)));
    CHECK(strstr(refusal,"INFERRED")!=NULL);
    live_vk_bind_destroy(bridge);
    gpu_window_native disabled=native;disabled.sampler_anisotropy=false;
    live_vk_texture_set *off=live_vk_texture_create(&disabled,gipa,&error);CHECK(off!=NULL);
    live_vk_sampler_desc a=live_vk_sampler_decode(0x303u,0x02062000u);
    CHECK(live_vk_sampler_apply_control(&a,0x4003FFD0u));
    unsigned before=creates;live_vk_texture_bound rejected;
    CHECK(!live_vk_texture_bind(off,cache,&result,&a,&rejected,&error));
    CHECK(strstr(error,"not enabled")!=NULL&&creates==before);
    live_vk_texture_destroy(off);probe_image_destroy(&p,&image);
    p.vkDestroyPipeline(native.device,p.pipeline,NULL);p.vkDestroyPipelineLayout(native.device,p.layout,NULL);
    p.vkDestroyRenderPass(native.device,p.render_pass,NULL);
    live_vk_texture_destroy(set);live_texture_cache_free(cache);free(cache);
    gpu_window_destroy(renderer);SDL_DestroyWindow(window);SDL_Quit();
    printf("T1051: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
