/* Reuse the existing finite fullscreen Vulkan probe; never invokes its old main. */
#define main t792_probe_reference_main
#include "test_live_vk_texture.c"
#undef main

static bool deny_bookkeeping, deny_completion, deny_pool_reset;
static VkDevice hooked_device;
static PFN_vkGetDeviceProcAddr real_device_proc;
void *__real_realloc(void *pointer,size_t size);
void *__wrap_realloc(void *pointer,size_t size)
{ return deny_bookkeeping ? NULL : __real_realloc(pointer,size); }

static VkResult VKAPI_CALL checked_idle(VkQueue queue)
{
    if(deny_completion)return VK_ERROR_DEVICE_LOST;
    PFN_vkQueueWaitIdle real=(PFN_vkQueueWaitIdle)real_device_proc(hooked_device,"vkQueueWaitIdle");
    return real(queue);
}

static VkResult VKAPI_CALL checked_reset(VkDevice device,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags)
{
    if(deny_pool_reset)return VK_ERROR_OUT_OF_HOST_MEMORY;
    PFN_vkResetDescriptorPool real=(PFN_vkResetDescriptorPool)real_device_proc(device,"vkResetDescriptorPool");
    return real(device,pool,flags);
}
static PFN_vkVoidFunction VKAPI_CALL checked_proc(VkDevice device,const char *name)
{
    if(strcmp(name,"vkQueueWaitIdle")==0)return (PFN_vkVoidFunction)checked_idle;
    if(strcmp(name,"vkResetDescriptorPool")==0)return (PFN_vkVoidFunction)checked_reset;
    return real_device_proc(device,name);
}

static bool deferred_pair(probe *p, VkDescriptorSet descriptor, probe_image *target, uint8_t *readback,
                          VkDescriptorSet (*mutate)(void *), void *context)
{
    const VkDeviceSize bytes = (VkDeviceSize)target->width * target->height * 4u;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes * 2u, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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
    /* The first draw is recorded but not submitted when its source is changed. */
    descriptor = mutate(context);
    if (descriptor == VK_NULL_HANDLE) return false;
    p->vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    p->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    p->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0u, 1u, &descriptor, 0u, NULL);
    p->vkCmdSetViewport(command, 0u, 1u, &viewport);
    p->vkCmdSetScissor(command, 0u, 1u, &scissor);
    p->vkCmdDraw(command, 3u, 1u, 0u, 0u);
    p->vkCmdEndRenderPass(command);
    p->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u,
                           0u, NULL, 0u, NULL, 1u, &to_source);
    VkBufferImageCopy second_region = region;
    second_region.bufferOffset = bytes;
    p->vkCmdCopyImageToBuffer(command, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1u, &second_region);
    p->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u,
                           0u, NULL, 0u, NULL, 1u, &to_sampled);
    if (p->vkEndCommandBuffer(command) != VK_SUCCESS) return false;
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1u, .pCommandBuffers = &command};
    const bool ok = p->vkQueueSubmit(p->native.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS && p->vkQueueWaitIdle(p->native.queue) == VK_SUCCESS;
    if (ok) memcpy(readback, mapped, (size_t)bytes * 2u);
    p->vkFreeCommandBuffers(p->native.device, p->native.command_pool, 1u, &command);
    p->vkUnmapMemory(p->native.device, memory);
    p->vkDestroyBuffer(p->native.device, buffer, NULL);
    p->vkFreeMemory(p->native.device, memory, NULL);
    return ok;
}

typedef struct {
    live_vk_texture_set *set;
    live_texture_cache *cache;
    live_texture_result result;
    live_vk_sampler_desc sampler;
    live_vk_texture_bound first, second;
    unsigned mode;
    uint8_t pixels[68];
} change;

static VkDescriptorSet mutate(void *context)
{
    change *c=context;
    const char *error=NULL;
    if(c->mode==3u) {
        c->result.needs_upload=false;
        c->sampler=live_vk_sampler_decode(0x00000303u,0x02020000u);
    } else {
        c->result.generation++;
        c->result.needs_upload=true;
        for(size_t i=0u;i<sizeof(c->pixels);i+=4u) {
            c->pixels[i]=0u;c->pixels[i+1u]=0u;c->pixels[i+2u]=255u;c->pixels[i+3u]=255u;
        }
        c->result.rgba=c->pixels;
        if(c->mode==1u) {
            const live_texture_binding binding={0u,FORMAT(0x12,0,0),LINEAR_SIZE(4,4,64),0u,0u};
            CHECK(live_texture_plan_binding(&binding,true,&c->cache->entries[0].plan));
            c->result.width=c->result.height=4u;
        } else if(c->mode==2u) {
            const live_texture_binding binding={0u,(FORMAT(0x06,1,1)&~0xF0000u)|0x20000u,0u,0u,0u};
            CHECK(live_texture_plan_binding(&binding,true,&c->cache->entries[0].plan));
            c->result.levels=2u;
            c->pixels[16u]=0u;c->pixels[17u]=255u;c->pixels[18u]=0u;c->pixels[19u]=255u;
            c->sampler=live_vk_sampler_decode_mips(0x00000303u,0x01030000u,(1u<<18)|(1u<<6),2u);
        }
    }
    const unsigned versions=c->mode==4u?257u:1u;
    for(unsigned i=0u;i<versions;i++) {
        c->result.generation++;
        CHECK(live_vk_texture_bind(c->set,c->cache,&c->result,&c->sampler,&c->second,&error));
        c->result.needs_upload=false;
        live_vk_texture_bound duplicate;
        CHECK(live_vk_texture_bind(c->set,c->cache,&c->result,&c->sampler,&duplicate,&error));
        CHECK(duplicate.descriptor==c->second.descriptor);
        c->result.needs_upload=c->mode!=3u;
    }
    CHECK(c->first.descriptor!=c->second.descriptor);
    if(c->mode==3u) CHECK(c->first.view==c->second.view);
    else CHECK(c->first.view!=c->second.view);
    CHECK(live_vk_texture_get_stats(c->set).image_destroys==0u);
    c->cache->entries[0].in_use=false;
    CHECK(live_vk_texture_trim(c->set,c->cache)==0u);
    c->cache->entries[0].in_use=true;
    return c->second.descriptor;
}

static void run_case(const gpu_window_native *native,PFN_vkGetInstanceProcAddr gipa,unsigned mode)
{
    const char *error=NULL;
    change c={0};c.mode=mode;
    c.set=live_vk_texture_create(native,gipa,&error);
    CHECK(c.set!=NULL);if(c.set==NULL)return;
    c.cache=calloc(1u,sizeof(*c.cache));CHECK(c.cache!=NULL);if(c.cache==NULL)return;
    live_texture_cache_init(c.cache,true);
    const unsigned size=2u;
    const live_texture_binding binding={0u,FORMAT(0x12,0,0),LINEAR_SIZE(size,size,64),0u,0u};
    CHECK(live_texture_plan_binding(&binding,true,&c.cache->entries[0].plan));
    if(!c.cache->entries[0].plan.ok) { live_vk_texture_destroy(c.set);free(c.cache);return; }
    c.cache->entries[0].in_use=true;
    for(size_t i=0u;i<sizeof(c.pixels);i+=4u) {
        c.pixels[i]=255u;c.pixels[i+1u]=c.pixels[i+2u]=0u;c.pixels[i+3u]=255u;
    }
    if(mode==3u) {
        c.pixels[4u]=c.pixels[8u]=0u;c.pixels[6u]=c.pixels[10u]=255u;
    }
    c.result=(live_texture_result){.source=LIVE_TEXTURE_SOURCE_GUEST,.entry=0u,.rgba=c.pixels,
        .width=size,.height=size,.levels=1u,.generation=1u,.needs_upload=true};
    c.sampler=live_vk_sampler_decode(0x00000303u,NEAREST_FILTER);
    CHECK(!live_vk_texture_end_batch(c.set));
    CHECK(live_vk_texture_begin_batch(c.set));
    CHECK(!live_vk_texture_begin_batch(c.set));
    CHECK(live_vk_texture_bind(c.set,c.cache,&c.result,&c.sampler,&c.first,&error));
    probe p;CHECK(probe_init(&p,native,gipa,live_vk_texture_set_layout(c.set)));
    probe_image target;CHECK(probe_image_create(&p,&target,4u,4u));
    uint8_t readback[128];memset(readback,0xCC,sizeof(readback));
    if(mode==5u) {
        uint8_t replacement[16];
        for(size_t i=0u;i<16u;i+=4u) {
            replacement[i]=replacement[i+1u]=0u;replacement[i+2u]=replacement[i+3u]=255u;
        }
        live_texture_result attempted=c.result;attempted.rgba=replacement;attempted.generation++;
        live_vk_texture_bound refused;
        deny_bookkeeping=true;
        CHECK(!live_vk_texture_bind(c.set,c.cache,&attempted,&c.sampler,&refused,&error));
        deny_bookkeeping=false;
        CHECK(error!=NULL&&strstr(error,"bookkeeping")!=NULL);
        CHECK(live_vk_texture_get_stats(c.set).image_destroys==0u);
        CHECK(live_vk_texture_get_stats(c.set).version_refusals==1u);
        CHECK(probe_render(&p,c.first.descriptor,&target,readback));
        for(size_t i=0u;i<64u;i+=4u)
            CHECK(readback[i]==255u&&readback[i+1u]==0u&&readback[i+2u]==0u&&readback[i+3u]==255u);
        CHECK(live_vk_texture_end_batch(c.set));
        goto cleanup;
    }
    CHECK(deferred_pair(&p,c.first.descriptor,&target,readback,mutate,&c));
    if(mode==3u) {
        const size_t pixel=(1u*4u+1u)*4u;
        CHECK(readback[pixel]==255u&&readback[pixel+1u]==0u&&readback[pixel+2u]==0u&&readback[pixel+3u]==255u);
        CHECK(readback[64u+pixel]>=159u&&readback[64u+pixel]<=160u);
        CHECK(readback[64u+pixel+2u]>=95u&&readback[64u+pixel+2u]<=96u);
    } else {
        for(size_t i=0u;i<64u;i+=4u) {
            CHECK(readback[i]==255u&&readback[i+1u]==0u&&readback[i+2u]==0u&&readback[i+3u]==255u);
            CHECK(readback[i+64u]==0u&&readback[i+64u+3u]==255u);
            CHECK(readback[i+64u+(mode==2u?1u:2u)]==255u);
        }
    }
    CHECK(live_vk_texture_batch_active(c.set));
    if(mode==4u) {
        deny_completion=true;
        CHECK(!live_vk_texture_end_batch(c.set));
        deny_completion=false;
        CHECK(live_vk_texture_batch_active(c.set));
        CHECK(live_vk_texture_get_stats(c.set).image_destroys==0u);
        deny_pool_reset=true;
        CHECK(!live_vk_texture_end_batch(c.set));
        deny_pool_reset=false;
        CHECK(live_vk_texture_batch_active(c.set));
        CHECK(live_vk_texture_get_stats(c.set).image_destroys==0u);
        CHECK(live_vk_texture_get_stats(c.set).completion_failures==2u);
        CHECK(live_vk_texture_get_stats(c.set).descriptor_versions_peak==258u);
    }
    CHECK(live_vk_texture_end_batch(c.set));
    CHECK(!live_vk_texture_batch_active(c.set));
    CHECK(live_vk_texture_get_stats(c.set).image_destroys==(mode==3u?0u:(mode==4u?257u:1u)));
    CHECK(live_vk_texture_begin_batch(c.set));
    c.result.needs_upload=false;
    live_vk_texture_bound rebound;
    CHECK(live_vk_texture_bind(c.set,c.cache,&c.result,&c.sampler,&rebound,&error));
    uint8_t reused[64];
    CHECK(probe_render(&p,rebound.descriptor,&target,reused));
    CHECK(memcmp(reused,readback+64u,64u)==0);
    c.cache->entries[0].plan.ok=false;
    CHECK(!live_vk_texture_bind(c.set,c.cache,&c.result,&c.sampler,&rebound,&error));
    CHECK(error!=NULL&&strstr(error,"validated plan")!=NULL);
    c.cache->entries[0].plan.ok=true;
    CHECK(live_vk_texture_end_batch(c.set)); /* Reusable descriptor pools. */
cleanup:
    probe_image_destroy(&p,&target);
    p.vkDestroyPipeline(native->device,p.pipeline,NULL);
    p.vkDestroyPipelineLayout(native->device,p.layout,NULL);
    p.vkDestroyRenderPass(native->device,p.render_pass,NULL);
    live_vk_texture_destroy(c.set);live_texture_cache_free(c.cache);free(c.cache);
}

int main(void)
{
    if(!SDL_Init(SDL_INIT_VIDEO))return unavailable(SDL_GetError());
    SDL_Window *window=SDL_CreateWindow("t1032-lifetime-test",32,32,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    if(window==NULL)return unavailable(SDL_GetError());
    const char *error=NULL;gpu_window *renderer=gpu_window_create(window,&error);
    if(renderer==NULL)return unavailable(error);
    gpu_window_native native;CHECK(gpu_window_get_native(renderer,&native));
    real_device_proc=native.get_device_proc_addr;hooked_device=native.device;
    native.get_device_proc_addr=checked_proc;
    PFN_vkGetInstanceProcAddr gipa=(PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    #ifdef T1032_SINGLE_CONTENT_CONTROL
    run_case(&native,gipa,0u);
#else
    for(unsigned mode=0u;mode<6u;mode++)run_case(&native,gipa,mode);
#endif
    gpu_window_destroy(renderer);SDL_DestroyWindow(window);SDL_Quit();
    printf("T1032 texture lifetime: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
