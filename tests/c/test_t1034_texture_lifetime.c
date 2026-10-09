/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent unsubmitted-draw version control; transport helpers are shared,
 * while submission, expected pixels, lifecycle and failure premise are unique. */
#define main t1034_unused_existing_main
#include "test_live_vk_texture.c"
#undef main
typedef struct {VkCommandBuffer command; VkBuffer buffer; VkDeviceMemory memory; void *mapped; VkDeviceSize bytes;} pending;
static bool prepare_pending(probe *p, VkDescriptorSet descriptor, probe_image *target, pending *out)
{
    const VkDeviceSize bytes = (VkDeviceSize)target->width * target->height * 4u;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements requirements;
    if (p->vkCreateBuffer(p->native.device, &buffer_info, NULL, &buffer) != VK_SUCCESS) return false;
    p->vkGetBufferMemoryRequirements(p->native.device, buffer, &requirements);
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = find_memory(p, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (p->vkAllocateMemory(p->native.device, &allocate, NULL, &memory) != VK_SUCCESS ||
        p->vkBindBufferMemory(p->native.device, buffer, memory, 0u) != VK_SUCCESS ||
        p->vkMapMemory(p->native.device, memory, 0u, VK_WHOLE_SIZE, 0u, &mapped) != VK_SUCCESS) return false;
    VkCommandBuffer command;
    const VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = p->native.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1u};
    if (p->vkAllocateCommandBuffers(p->native.device, &command_info, &command) != VK_SUCCESS) return false;
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    const VkClearValue clear = {.color = {{1.0f, 0.0f, 1.0f, 1.0f}}};
    const VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p->render_pass, .framebuffer = target->framebuffer,
        .renderArea = {{0, 0}, {target->width, target->height}}, .clearValueCount = 1u, .pClearValues = &clear};
    const VkViewport viewport = {0.0f, 0.0f, (float)target->width, (float)target->height, 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, {target->width, target->height}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    VkImageMemoryBarrier to_source = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = target->image, .subresourceRange = range};
    VkImageMemoryBarrier to_sampled = to_source;
    to_sampled.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_sampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_sampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u}, .imageExtent = {target->width, target->height, 1u}};
    if (p->vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;
    p->vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    p->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    p->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0u, 1u, &descriptor, 0u, NULL);
    p->vkCmdSetViewport(command, 0u, 1u, &viewport);
    p->vkCmdSetScissor(command, 0u, 1u, &scissor);
    p->vkCmdDraw(command, 3u, 1u, 0u, 0u);
    p->vkCmdEndRenderPass(command);
    p->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &to_source);
    p->vkCmdCopyImageToBuffer(command, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1u, &region);
    p->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &to_sampled);
    if (p->vkEndCommandBuffer(command) != VK_SUCCESS) return false;
    *out = (pending){command, buffer, memory, mapped, bytes};
    return true;
}

static void release_pending(probe *p, pending *record)
{
    p->vkFreeCommandBuffers(p->native.device,p->native.command_pool,1u,&record->command);
    p->vkUnmapMemory(p->native.device,record->memory);
    p->vkDestroyBuffer(p->native.device,record->buffer,NULL);
    p->vkFreeMemory(p->native.device,record->memory,NULL);
}
static void probe_destroy(probe *p)
{
 p->vkDestroyPipeline(p->native.device,p->pipeline,NULL);
 p->vkDestroyPipelineLayout(p->native.device,p->layout,NULL);
 p->vkDestroyRenderPass(p->native.device,p->render_pass,NULL);
}
#ifdef T1034_CANDIDATE
/* Explicit synthetic failure responses, not genuine device loss or OOM. */
static PFN_vkGetDeviceProcAddr real_get_device;
static PFN_vkQueueWaitIdle real_wait;
static PFN_vkCreateImage real_create_image;
static bool fail_wait,fail_image;
static VKAPI_ATTR VkResult VKAPI_CALL controlled_wait(VkQueue queue)
{return fail_wait?VK_ERROR_DEVICE_LOST:real_wait(queue);}
static VKAPI_ATTR VkResult VKAPI_CALL controlled_image(VkDevice device,const VkImageCreateInfo *info,const VkAllocationCallbacks *allocator,VkImage *image)
{if(fail_image){fail_image=false;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}return real_create_image(device,info,allocator,image);}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL controlled_get_device(VkDevice device,const char *name)
{
 if(strcmp(name,"vkQueueWaitIdle")==0)return (PFN_vkVoidFunction)controlled_wait;
 if(strcmp(name,"vkCreateImage")==0)return (PFN_vkVoidFunction)controlled_image;
 return real_get_device(device,name);
}
#endif
int main(void)
{
    if(!SDL_Init(SDL_INIT_VIDEO))return unavailable(SDL_GetError());
    SDL_Window *window=SDL_CreateWindow("T1034 private lifetime probe",64,64,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    if(!window)return unavailable(SDL_GetError());
    const char *error=NULL;gpu_window *renderer=gpu_window_create(window,&error);
    if(!renderer)return unavailable(error);
    gpu_window_native native;CHECK(gpu_window_get_native(renderer,&native));
    PFN_vkGetInstanceProcAddr gipa=(PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    gpu_window_native texture_native=native;
#ifdef T1034_CANDIDATE
    real_get_device=native.get_device_proc_addr;
    real_wait=(PFN_vkQueueWaitIdle)real_get_device(native.device,"vkQueueWaitIdle");
    real_create_image=(PFN_vkCreateImage)real_get_device(native.device,"vkCreateImage");
    texture_native.get_device_proc_addr=controlled_get_device;
#endif
    live_vk_texture_set *set=live_vk_texture_create(&texture_native,gipa,&error);CHECK(set!=NULL);
    if(!set)return 1;
    probe p;CHECK(probe_init(&p,&native,gipa,live_vk_texture_set_layout(set)));
    live_texture_cache *cache=calloc(1u,sizeof *cache);live_texture_cache_init(cache,false);live_texture_watch_reset();
    live_texture_binding binding={0u,FORMAT(0x12,0,0),LINEAR_SIZE(2u,2u,64u),GUEST_BASE,0u};
    live_vk_sampler_desc repeat=live_vk_sampler_decode(0x101u,NEAREST_FILTER);
    live_vk_sampler_desc clamp=live_vk_sampler_decode(0x303u,NEAREST_FILTER);
    live_texture_result result;live_vk_texture_bound a,steady,sampler_version,b;
    probe_image targets[3];pending records[3];
#ifdef T1034_CANDIDATE
    CHECK(!live_vk_texture_end_batch(set));CHECK(live_vk_texture_begin_batch(set));
    CHECK(!live_vk_texture_begin_batch(set));CHECK(live_vk_texture_batch_active(set));
#endif
    for(unsigned y=0;y<2;y++)for(unsigned x=0;x<2;x++){size_t i=y*64u+x*4u;guest[i]=0;guest[i+1]=0;guest[i+2]=255;guest[i+3]=255;}
    live_texture_lookup(cache,&binding,reader,NULL,&result);
    CHECK(live_vk_texture_bind(set,cache,&result,&repeat,&a,&error));
    live_texture_lookup(cache,&binding,reader,NULL,&result);
    CHECK(live_vk_texture_bind(set,cache,&result,&repeat,&steady,&error));
    CHECK(steady.image==a.image && steady.view==a.view && steady.descriptor==a.descriptor && !steady.uploaded);
    CHECK(probe_image_create(&p,&targets[0],2u,2u));CHECK(prepare_pending(&p,a.descriptor,&targets[0],&records[0]));
    CHECK(live_vk_texture_bind(set,cache,&result,&clamp,&sampler_version,&error));
#ifdef T1034_CANDIDATE
    CHECK(sampler_version.descriptor!=a.descriptor && sampler_version.image==a.image && sampler_version.sampler!=a.sampler);
#endif
    CHECK(probe_image_create(&p,&targets[1],2u,2u));CHECK(prepare_pending(&p,sampler_version.descriptor,&targets[1],&records[1]));
    const bool resize=getenv("T1034_RESIZE")!=NULL;
    
    for(unsigned y=0;y<2;y++)for(unsigned x=0;x<(resize?4u:2u);x++){size_t i=y*64u+x*4u;guest[i]=255;guest[i+1]=0;guest[i+2]=0;guest[i+3]=255;}
    if(resize){
        live_texture_watch_note(GUEST_BASE,128u);CHECK(live_texture_watch_drain(cache)==1u);
        reader_fails=true;live_texture_lookup(cache,&binding,reader,NULL,&result);reader_fails=false;
        CHECK(result.source==LIVE_TEXTURE_SOURCE_REFUSED);
        const uint64_t destroyed=live_vk_texture_get_stats(set).image_destroys;
        #ifdef T1034_CANDIDATE
        /* Prepared API expectation only; candidate has not been frozen/executed. */
        CHECK(live_vk_texture_trim(set,cache)==0u);
        CHECK(live_vk_texture_get_stats(set).image_destroys==destroyed);
#else
        CHECK(live_vk_texture_trim(set,cache)==1u);
        CHECK(live_vk_texture_get_stats(set).image_destroys==destroyed+1u);
#endif
        for(unsigned index=0;index<LIVE_TEXTURE_CACHE_ENTRIES;index++){
            live_texture_binding pressure={0u,FORMAT(0x0C,2,2),0u,GUEST_BASE+0x1000u+index*64u,0u};
            live_texture_result ignored;live_texture_lookup(cache,&pressure,reader,NULL,&ignored);CHECK(ignored.source==LIVE_TEXTURE_SOURCE_GUEST);
        }
    }
    if(resize)binding.size_word=LINEAR_SIZE(4u,2u,64u);
    live_texture_watch_note(GUEST_BASE,128u);CHECK(live_texture_watch_drain(cache)==(resize?0u:1u));
    live_texture_lookup(cache,&binding,reader,NULL,&result);CHECK(result.needs_upload);
#ifdef T1034_CANDIDATE
    const uint64_t before_failure=live_vk_texture_get_stats(set).image_destroys;
    fail_image=true;CHECK(!live_vk_texture_bind(set,cache,&result,&clamp,&b,&error));
    CHECK(live_vk_texture_batch_active(set));
    CHECK(live_vk_texture_get_stats(set).image_destroys==before_failure);
#endif
    CHECK(live_vk_texture_bind(set,cache,&result,&clamp,&b,&error));
    printf("T1034 versions image_equal=%u view_equal=%u descriptor_equal=%u sampler_descriptor_equal=%u\n",a.image==b.image,a.view==b.view,a.descriptor==b.descriptor,a.descriptor==sampler_version.descriptor);fflush(stdout);
#ifndef T1034_CANDIDATE
    /* Baseline necessarily invalidated open draws: discard them, never submit
     * dangling resources just to provoke a driver crash. Validation output is retained. */
    CHECK((resize || a.descriptor==b.descriptor) && a.descriptor==sampler_version.descriptor);
    for(unsigned i=0;i<2;i++){release_pending(&p,&records[i]);probe_image_destroy(&p,&targets[i]);}
    live_vk_texture_destroy(set);probe_destroy(&p);live_texture_cache_free(cache);free(cache);gpu_window_destroy(renderer);SDL_DestroyWindow(window);SDL_Quit();
    printf("T1034 baseline refusal: unsubmitted version overwritten; %d checks %d failures\n",checks,failures);return failures?1:42;
#else
    CHECK(a.image!=b.image && a.view!=b.view && a.descriptor!=b.descriptor);
    CHECK(probe_image_create(&p,&targets[2],2u,2u));CHECK(prepare_pending(&p,b.descriptor,&targets[2],&records[2]));
    /* Refusal must neither upload nor retire prior recorded versions. */
    live_vk_sampler_desc bad=live_vk_sampler_decode(0u,0u);live_vk_texture_bound refused;
    const live_vk_texture_stats before=live_vk_texture_get_stats(set);
    CHECK(!live_vk_texture_bind(set,cache,&result,&bad,&refused,&error));
    CHECK(live_vk_texture_get_stats(set).image_destroys==before.image_destroys);
    CHECK(live_vk_texture_get_stats(set).uploads==before.uploads);
    VkCommandBuffer commands[]={records[0].command,records[1].command,records[2].command};
    const VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=3u,.pCommandBuffers=commands};
    VkResult submitted=p.vkQueueSubmit(p.native.queue,1u,&submit,VK_NULL_HANDLE);CHECK(submitted==VK_SUCCESS);
    VkResult completed=p.vkQueueWaitIdle(p.native.queue);CHECK(completed==VK_SUCCESS);
    for(unsigned image=0;image<3;image++)for(unsigned pixel=0;pixel<4;pixel++){
        const uint8_t *rgba=(const uint8_t *)records[image].mapped+pixel*4u;
        CHECK(rgba[0]==(image==2?0:255) && rgba[1]==0 && rgba[2]==(image==2?255:0) && rgba[3]==255);
    }
    for(unsigned i=0;i<3;i++){release_pending(&p,&records[i]);probe_image_destroy(&p,&targets[i]);}
    fail_wait=true;CHECK(!live_vk_texture_end_batch(set));
    CHECK(live_vk_texture_batch_active(set));
    CHECK(live_vk_texture_get_stats(set).image_destroys==before.image_destroys);
    fail_wait=false;
    CHECK(live_vk_texture_end_batch(set));CHECK(!live_vk_texture_batch_active(set));
    CHECK(live_vk_texture_get_stats(set).image_destroys>before.image_destroys);
    CHECK(!live_vk_texture_end_batch(set));
    CHECK(live_vk_texture_begin_batch(set));live_texture_lookup(cache,&binding,reader,NULL,&result);
    CHECK(live_vk_texture_bind(set,cache,&result,&clamp,&steady,&error));CHECK(!steady.uploaded);
    CHECK(live_vk_texture_end_batch(set));
    live_vk_texture_destroy(set);probe_destroy(&p);live_texture_cache_free(cache);free(cache);gpu_window_destroy(renderer);SDL_DestroyWindow(window);SDL_Quit();
    printf("T1034 %d checks %d failures; submitted=%d completed=%d\n",checks,failures,submitted,completed);return failures?1:0;
#endif
}
