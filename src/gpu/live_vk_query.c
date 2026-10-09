/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_vk_query.h"
#include <stdlib.h>
#define QUERY_FUNCTIONS(X) \
 X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdBeginQuery) X(vkCmdEndQuery) \
 X(vkGetQueryPoolResults) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) \
 X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle)
struct live_vk_query {
 live_vk_device device;
 VkQueryPool pool;
 uint32_t used;
#define DECLARE(name) PFN_##name name;
 QUERY_FUNCTIONS(DECLARE)
#undef DECLARE
};
live_vk_query *live_vk_query_create(const live_vk_device *device)
{
 if(device == NULL || !device->occlusion_query_precise || device->get_device_proc_addr == NULL) return NULL;
 live_vk_query *q=calloc(1u,sizeof *q);
 if(q == NULL) return NULL;
 q->device=*device;
#define LOAD(name) q->name=(PFN_##name)device->get_device_proc_addr(device->device,#name); if(q->name == NULL) { free(q); return NULL; }
 QUERY_FUNCTIONS(LOAD)
#undef LOAD
 const VkQueryPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
  .queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=LIVE_VK_MAX_DRAWS_PER_FRAME};
 if(q->vkCreateQueryPool(device->device,&info,NULL,&q->pool) != VK_SUCCESS) { free(q); return NULL; }
 return q;
}
void live_vk_query_destroy(live_vk_query *q)
{
 if(q == NULL) return;
 q->vkDestroyQueryPool(q->device.device,q->pool,NULL);
 free(q);
}
bool live_vk_query_reset(live_vk_query *q)
{
 if(q == NULL) return false;
 VkCommandBuffer command=VK_NULL_HANDLE;
 const VkCommandBufferAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
  .commandPool=q->device.command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1u};
 if(q->vkAllocateCommandBuffers(q->device.device,&allocate,&command) != VK_SUCCESS) return false;
 const VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
  .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
 bool okay=q->vkBeginCommandBuffer(command,&begin) == VK_SUCCESS;
 if(okay) {
  q->vkCmdResetQueryPool(command,q->pool,0u,LIVE_VK_MAX_DRAWS_PER_FRAME);
  okay=q->vkEndCommandBuffer(command) == VK_SUCCESS;
 }
 if(okay) {
  const VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1u,.pCommandBuffers=&command};
  okay=q->vkQueueSubmit(q->device.queue,1u,&submit,VK_NULL_HANDLE) == VK_SUCCESS;
  if(okay && q->vkQueueWaitIdle(q->device.queue) != VK_SUCCESS) return false;
 }
 /* A failed queue wait must not free an in-flight command. Device loss ends use. */
 if(okay) q->used=0u;
 q->vkFreeCommandBuffers(q->device.device,q->device.command_pool,1u,&command);
 return okay;
}
bool live_vk_query_begin(live_vk_query *q,VkCommandBuffer command,uint32_t *slot)
{
 if(q == NULL || slot == NULL || command == VK_NULL_HANDLE || q->used == LIVE_VK_MAX_DRAWS_PER_FRAME) return false;
 *slot=q->used++;
 q->vkCmdBeginQuery(command,q->pool,*slot,VK_QUERY_CONTROL_PRECISE_BIT);
 return true;
}
void live_vk_query_end(live_vk_query *q,VkCommandBuffer command,uint32_t slot)
{
 if(q != NULL && slot < q->used) q->vkCmdEndQuery(command,q->pool,slot);
}
bool live_vk_query_result(live_vk_query *q,uint32_t slot,uint64_t *samples)
{
 if(q == NULL || samples == NULL || slot >= q->used) return false;
 uint64_t result[2]={0u,0u};
 VkResult status=q->vkGetQueryPoolResults(q->device.device,q->pool,slot,1u,sizeof result,result,sizeof result,
  VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
 if(status != VK_SUCCESS || result[1] == 0u) return false;
 *samples=result[0];
 return true;
}
