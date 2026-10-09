/* SPDX-License-Identifier: GPL-3.0-or-later
 * Pipeline creation only: deliberately omit PointSize, never submit this pipeline.
 * Run with Khronos validation; its 08773 diagnostic is the negative oracle. */
#define VK_NO_PROTOTYPES
#include "gpu_device_native.h"
#include <stdio.h>
#include <stdlib.h>
#define LOAD(name) PFN_##name name=(PFN_##name)n.get_device_proc_addr(n.device,#name)
int main(int argc,char **argv)
{
    if(argc!=2) return 2;
    FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);
    if(bytes<=0 || bytes%4)return 2;
    uint32_t *words=malloc((size_t)bytes);if(!words)return 2;
    if(fread(words,1,(size_t)bytes,f)!=(size_t)bytes)return 2;
    fclose(f);
    gpu_device *device=NULL;if(gpu_device_create_selected("software",&device)!=GPU_OK)return 77;
    gpu_device_native n;gpu_device_get_native(device,&n);
    LOAD(vkCreateShaderModule);LOAD(vkDestroyShaderModule);LOAD(vkCreatePipelineLayout);
    LOAD(vkDestroyPipelineLayout);LOAD(vkCreateDescriptorSetLayout);LOAD(vkDestroyDescriptorSetLayout);
    LOAD(vkCreateRenderPass);LOAD(vkDestroyRenderPass);LOAD(vkCreateGraphicsPipelines);LOAD(vkDestroyPipeline);
    VkShaderModule shader=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor=VK_NULL_HANDLE;VkRenderPass pass=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=words};
    if(vkCreateShaderModule(n.device,&sm,NULL,&shader)!=VK_SUCCESS)return 3;
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayoutCreateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    if(vkCreateDescriptorSetLayout(n.device,&ds,NULL,&descriptor)!=VK_SUCCESS)return 3;
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&descriptor};
    if(vkCreatePipelineLayout(n.device,&pl,NULL,&layout)!=VK_SUCCESS)return 3;
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.subpassCount=1,.pSubpasses=&sub};
    if(vkCreateRenderPass(n.device,&rp,NULL,&pass)!=VK_SUCCESS)return 3;
    VkVertexInputBindingDescription vb={.binding=0,.stride=32,.inputRate=VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]={{1,0,VK_FORMAT_R32G32B32A32_SFLOAT,0},{2,0,VK_FORMAT_R32G32B32A32_SFLOAT,16}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&vb,.vertexAttributeDescriptionCount=2,.pVertexAttributeDescriptions=attrs};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.rasterizerDiscardEnable=VK_TRUE,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1};
    VkPipelineShaderStageCreateInfo stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=shader,.pName="main"};
    VkGraphicsPipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=1,.pStages=&stage,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pRasterizationState=&rs,.layout=layout,.renderPass=pass};
    VkResult result=vkCreateGraphicsPipelines(n.device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline);
    printf("T1042 creation-only negative result=%d; no pipeline submitted\n",result);
    if(pipeline)vkDestroyPipeline(n.device,pipeline,NULL);
    vkDestroyRenderPass(n.device,pass,NULL);vkDestroyPipelineLayout(n.device,layout,NULL);
    vkDestroyDescriptorSetLayout(n.device,descriptor,NULL);vkDestroyShaderModule(n.device,shader,NULL);
    gpu_device_destroy(device);free(words);return 0;
}
