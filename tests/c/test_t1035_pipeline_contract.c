/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent actual Vulkan create-info observation; existing pixel controls are
 * supplementary. The proxy passes every argument/result through unchanged. */
#define main inherited_pixel_main
#include "test_live_vk_draw.c"
#undef main
static PFN_vkGetDeviceProcAddr real_gdp;
static PFN_vkCreateRenderPass real_pass;
static PFN_vkCreateGraphicsPipelines real_pipelines;
static VkRenderPass tracked_pass[512];
static bool tracked_depth[512];
static unsigned tracked_count, observed, missing, clears, colour_only, disabled, contract_failures;
static VkResult VKAPI_CALL observe_pass(VkDevice d, const VkRenderPassCreateInfo *i,
 const VkAllocationCallbacks *a, VkRenderPass *p)
{
 VkResult result=real_pass(d,i,a,p);
 if(result==VK_SUCCESS && tracked_count<512u){tracked_pass[tracked_count]=*p;
 const VkAttachmentReference *depth=i->pSubpasses[0].pDepthStencilAttachment;
 tracked_depth[tracked_count++]=depth!=NULL && depth->attachment!=VK_ATTACHMENT_UNUSED;}
 return result;
}
static VkResult VKAPI_CALL observe_pipeline(VkDevice d,VkPipelineCache cache,uint32_t n,
 const VkGraphicsPipelineCreateInfo *i,const VkAllocationCallbacks *a,VkPipeline *p)
{
 for(uint32_t j=0;j<n;j++){
  bool found=false,depth=false;
  for(unsigned k=0;k<tracked_count;k++)if(tracked_pass[k]==i[j].renderPass){found=true;depth=tracked_depth[k];}
  if(!found){contract_failures++;continue;}
  observed++;
  const VkPipelineDepthStencilStateCreateInfo *s=i[j].pDepthStencilState;
  const bool clear=i[j].pVertexInputState->vertexBindingDescriptionCount==0u;
  if(depth && s==NULL){missing++;contract_failures++;}
  if(clear){clears++;if(s!=NULL && (s->depthTestEnable || s->depthWriteEnable || s->stencilTestEnable || s->depthBoundsTestEnable))contract_failures++;}
  if(!depth){colour_only++;if(s!=NULL && (s->depthTestEnable || s->depthWriteEnable || s->stencilTestEnable))contract_failures++;}
  if(depth && s!=NULL && !s->depthTestEnable && !s->stencilTestEnable)disabled++;
 }
 return real_pipelines(d,cache,n,i,a,p);
}
static PFN_vkVoidFunction VKAPI_CALL proxy_gdp(VkDevice d,const char *name)
{
 PFN_vkVoidFunction f=real_gdp(d,name);
 if(strcmp(name,"vkCreateRenderPass")==0){real_pass=(PFN_vkCreateRenderPass)f;return (PFN_vkVoidFunction)observe_pass;}
 if(strcmp(name,"vkCreateGraphicsPipelines")==0){real_pipelines=(PFN_vkCreateGraphicsPipelines)f;return (PFN_vkVoidFunction)observe_pipeline;}
 return f;
}
void __real_gpu_device_get_native(const gpu_device *,gpu_device_native *);
void __wrap_gpu_device_get_native(const gpu_device *d,gpu_device_native *n)
{
 __real_gpu_device_get_native(d,n);real_gdp=n->get_device_proc_addr;n->get_device_proc_addr=proxy_gdp;
}
int main(void)
{
 int pixels=inherited_pixel_main(0,NULL);
 printf("T1035 contracts observed=%u missing=%u clears=%u colour_only=%u disabled_depth=%u failures=%u\n",observed,missing,clears,colour_only,disabled,contract_failures);
 if(observed==0u || clears==0u || colour_only==0u || disabled==0u)contract_failures++;
 return pixels!=0 || contract_failures!=0u ? 1 : 0;
}
