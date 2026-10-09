/* Independent literal vectors; existing Vulkan bootstrap is fixture machinery. */
#define main t792_fixture_main
#include "test_live_vk_texture.c"
#undef main
#include "live_vk_bind.h"
#include "gpu_pgraph_replay.h"
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

static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetPhysicalDeviceProperties real_props;
static bool limited;
static void VKAPI_CALL review_props(VkPhysicalDevice physical, VkPhysicalDeviceProperties *out)
{
    real_props(physical,out);
    if(limited)out->limits.maxSamplerAnisotropy=1.5f; /* FABRICATED lower capability input. */
}
static PFN_vkVoidFunction VKAPI_CALL review_gipa(VkInstance instance,const char *name)
{
    if(strcmp(name,"vkGetPhysicalDeviceProperties")==0)return (PFN_vkVoidFunction)review_props;
    return real_gipa(instance,name);
}
int main(void)
{
    const unsigned order[]={3u,1u,0u,2u};
    const unsigned amounts[]={8u,2u,1u,4u};
    for(unsigned i=0;i<4;i++) {
        uint32_t word=0x401000C0u|(order[i]<<4);
        live_vk_sampler_desc d=live_vk_sampler_decode(0x303u,0x02062000u);
        CHECK(live_vk_sampler_apply_control(&d,word));CHECK(d.max_anisotropy==amounts[i]);
        live_vk_sampler_desc mip=live_vk_sampler_decode_mips(0x303u,0x02062000u,word,3u);
        CHECK(mip.ok&&mip.max_anisotropy==amounts[i]);
        for(unsigned bit=0;bit<32;bit++) if(bit<4u||bit==31u) {
            live_vk_sampler_desc bad=live_vk_sampler_decode(0x303u,0x02062000u);
            CHECK(!live_vk_sampler_apply_control(&bad,word|(1u<<bit)));
            CHECK(!bad.ok&&strstr(bad.refusal,"Control0"));
        }
    }
    if(!SDL_Init(SDL_INIT_VIDEO))return unavailable(SDL_GetError());
    SDL_Window *window=SDL_CreateWindow("T1062",32,32,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    if(!window)return unavailable(SDL_GetError());
    const char *error=NULL;gpu_window *renderer=gpu_window_create(window,&error);
    if(!renderer)return unavailable(error);
    gpu_window_native native;CHECK(gpu_window_get_native(renderer,&native));
    real_gipa=(PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    real_props=(PFN_vkGetPhysicalDeviceProperties)real_gipa(native.instance,"vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties props;real_props(native.physical_device,&props);
    PFN_vkGetPhysicalDeviceFeatures getfeatures=(PFN_vkGetPhysicalDeviceFeatures)real_gipa(native.instance,"vkGetPhysicalDeviceFeatures");
    VkPhysicalDeviceFeatures features;getfeatures(native.physical_device,&features);
    CHECK(native.sampler_anisotropy&&features.samplerAnisotropy);
    printf("device=%s api=%u driver=%u physical-limit=%g enabled=%u\n",props.deviceName,props.apiVersion,props.driverVersion,(double)props.limits.maxSamplerAnisotropy,native.sampler_anisotropy);
    actual_proc=native.get_device_proc_addr;native.get_device_proc_addr=capture_proc;
    live_texture_cache *cache=calloc(1,sizeof(*cache));live_texture_cache_init(cache,true);
    for(unsigned i=0;i<16;i+=4){guest[i]=27;guest[i+1]=83;guest[i+2]=149;guest[i+3]=255;}
    live_texture_binding binding={0u,FORMAT(0x06,1,1),0u,GUEST_BASE,0u};
    live_texture_result result;live_texture_lookup(cache,&binding,reader,NULL,&result);
    CHECK(result.source==LIVE_TEXTURE_SOURCE_GUEST);if(result.source!=LIVE_TEXTURE_SOURCE_GUEST)return 1;
    for(unsigned domain=0;domain<2;domain++) {
        limited=domain==1;creates=0;
        live_vk_texture_set *set=live_vk_texture_create(&native,review_gipa,&error);CHECK(set);
        if(!set)return 1;
        CHECK(live_vk_texture_begin_batch(set));live_vk_texture_bound bound[4];
        for(unsigned i=0;i<4;i++) {
            live_vk_sampler_desc d=live_vk_sampler_decode(0x303u,0x02062000u);
            CHECK(live_vk_sampler_apply_control(&d,0x4003FFC0u|(order[i]<<4)));
            CHECK(live_vk_texture_bind(set,cache,&result,&d,&bound[i],&error));
            float limit=limited?1.5f:props.limits.maxSamplerAnisotropy;
            float expected=(float)amounts[i]>limit?limit:(float)amounts[i];
            CHECK(creates==i+1);CHECK(observed.maxAnisotropy==expected);
            CHECK(observed.anisotropyEnable==(amounts[i]>1?VK_TRUE:VK_FALSE));
            if(i)CHECK(bound[i].sampler!=bound[i-1].sampler&&bound[i].descriptor!=bound[i-1].descriptor);
            result.needs_upload=false;live_vk_texture_bound repeat;
            CHECK(live_vk_texture_bind(set,cache,&result,&d,&repeat,&error));
            CHECK(creates==i+1&&repeat.sampler==bound[i].sampler&&repeat.descriptor==bound[i].descriptor);
        }
        live_vk_bind_source source={.binding=single_binding,.reader=reader};
        live_vk_bind *bridge=live_vk_bind_create(cache,set,&source);CHECK(bridge);
        gpu_pgraph_backend gate={.allowed_inferences=GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING};
        live_vk_bind_backend(bridge,&gate);live_vk_texture_hook hook=live_vk_bind_hook(bridge);
        gpu_pgraph_state snapshot={0};
        snapshot.output_written[GPU_PGRAPH_OUT_TEXTURE_ADDRESS]=true;
        snapshot.output_written[GPU_PGRAPH_OUT_TEXTURE_FILTER]=true;
        snapshot.output_written[GPU_PGRAPH_OUT_TEXTURE_CONTROL0]=true;
        snapshot.output[GPU_PGRAPH_OUT_TEXTURE_ADDRESS]=0x303u;
        snapshot.output[GPU_PGRAPH_OUT_TEXTURE_FILTER]=0x02062000u;
        snapshot.output[GPU_PGRAPH_OUT_TEXTURE_CONTROL0]=0x4003FFE0u;
        char refusal[256];live_vk_texture_binding hooked;
        CHECK(hook.bind(hook.context,0,&snapshot,0,&hooked,refusal,sizeof(refusal)));
        CHECK(hooked.sampler==bound[3].sampler);
        gate.allowed_inferences=0;live_vk_bind_backend(bridge,&gate);
        CHECK(!hook.bind(hook.context,0,&snapshot,0,&hooked,refusal,sizeof(refusal)));
        CHECK(strstr(refusal,"INFERRED"));live_vk_bind_destroy(bridge);
        probe p;CHECK(probe_init(&p,&native,real_gipa,live_vk_texture_set_layout(set)));
        probe_image image;CHECK(probe_image_create(&p,&image,4,4));uint8_t pixels[64];
        CHECK(probe_render(&p,bound[0].descriptor,&image,pixels));
        for(unsigned j=0;j<64;j+=4)CHECK(pixels[j]==149&&pixels[j+1]==83&&pixels[j+2]==27&&pixels[j+3]==255);
        probe_image_destroy(&p,&image);p.vkDestroyPipeline(native.device,p.pipeline,NULL);
        p.vkDestroyPipelineLayout(native.device,p.layout,NULL);p.vkDestroyRenderPass(native.device,p.render_pass,NULL);
        CHECK(live_vk_texture_end_batch(set));live_vk_texture_destroy(set);
    }
    limited=false;gpu_window_native disabled=native;disabled.sampler_anisotropy=false;
    live_vk_texture_set *off=live_vk_texture_create(&disabled,review_gipa,&error);CHECK(off);
    live_vk_sampler_desc d=live_vk_sampler_decode(0x303u,0x02062000u);
    CHECK(live_vk_sampler_apply_control(&d,0x4003FFE0u));unsigned before=creates;live_vk_texture_bound out;
    CHECK(!live_vk_texture_bind(off,cache,&result,&d,&out,&error));
    CHECK(error&&strstr(error,"not enabled")&&creates==before);
    CHECK(live_vk_sampler_apply_control(&d,0x4003FFC0u));
    CHECK(live_vk_texture_bind(off,cache,&result,&d,&out,&error));
    CHECK(creates==before+1&&observed.anisotropyEnable==VK_FALSE&&observed.maxAnisotropy==1.0f);
    live_vk_texture_destroy(off);live_texture_cache_free(cache);free(cache);
    gpu_window_destroy(renderer);SDL_DestroyWindow(window);SDL_Quit();
    printf("T1062: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
