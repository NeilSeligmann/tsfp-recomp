/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FABRICATED driver limits; actual source-built gpu_device constructor. */
#define VK_NO_PROTOTYPES
#include "gpu_device_native.h"
#include <stdio.h>
#include <string.h>
static uint32_t loader_version,physical_version,requested;
static unsigned create_calls,device_calls,destroy_calls,close_calls,checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL line%d %s\n",__LINE__,#x);}}while(0)
static VkResult VKAPI_CALL version(uint32_t *v){*v=loader_version;return VK_SUCCESS;}
static VkResult VKAPI_CALL extensions(const char *layer,uint32_t *n,VkExtensionProperties *p){(void)layer;(void)p;*n=0;return VK_SUCCESS;}
static VkResult VKAPI_CALL create(const VkInstanceCreateInfo *i,const VkAllocationCallbacks *a,VkInstance *v){(void)a;create_calls++;requested=i->pApplicationInfo->apiVersion;if(requested>loader_version)return VK_ERROR_INCOMPATIBLE_DRIVER;*v=(VkInstance)(uintptr_t)2;return VK_SUCCESS;}
static void VKAPI_CALL destroy(VkInstance i,const VkAllocationCallbacks *a){(void)i;(void)a;destroy_calls++;}
static VkResult VKAPI_CALL physical(VkInstance i,uint32_t *n,VkPhysicalDevice *p){(void)i;*n=1;if(p)*p=(VkPhysicalDevice)(uintptr_t)3;return VK_SUCCESS;}
static void VKAPI_CALL properties(VkPhysicalDevice d,VkPhysicalDeviceProperties *p){(void)d;memset(p,0,sizeof *p);p->apiVersion=physical_version;p->deviceType=VK_PHYSICAL_DEVICE_TYPE_CPU;strcpy(p->deviceName,"T1035 fabricated limits");}
static void VKAPI_CALL queues(VkPhysicalDevice d,uint32_t *n,VkQueueFamilyProperties *p){(void)d;*n=1;if(p){memset(p,0,sizeof *p);p->queueFlags=VK_QUEUE_GRAPHICS_BIT;p->queueCount=1;}}
static void VKAPI_CALL features(VkPhysicalDevice d,VkPhysicalDeviceFeatures *p){(void)d;memset(p,0,sizeof *p);}
static void VKAPI_CALL memory(VkPhysicalDevice d,VkPhysicalDeviceMemoryProperties *p){(void)d;memset(p,0,sizeof *p);}
static VkResult VKAPI_CALL device_extensions(VkPhysicalDevice d,const char *l,uint32_t *n,VkExtensionProperties *p){(void)d;return extensions(l,n,p);}
static VkResult VKAPI_CALL create_device(VkPhysicalDevice d,const VkDeviceCreateInfo *i,const VkAllocationCallbacks *a,VkDevice *v){(void)d;(void)i;(void)a;(void)v;device_calls++;return VK_ERROR_INITIALIZATION_FAILED;}
static VkResult VKAPI_CALL surface_support(VkPhysicalDevice d,uint32_t family,VkSurfaceKHR s,VkBool32 *v){(void)d;(void)family;(void)s;*v=VK_TRUE;return VK_SUCCESS;}
static void VKAPI_CALL destroy_surface(VkInstance i,VkSurfaceKHR s,const VkAllocationCallbacks *a){(void)i;(void)s;(void)a;}
static void VKAPI_CALL unused(void){}
static PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance i,const char *name){(void)i;
#define ENTRY(n,f) if(strcmp(name,n)==0)return(PFN_vkVoidFunction)f
 ENTRY("vkGetPhysicalDeviceSurfaceSupportKHR",surface_support);ENTRY("vkDestroySurfaceKHR",destroy_surface);ENTRY("vkEnumerateInstanceVersion",version);ENTRY("vkCreateInstance",create);ENTRY("vkEnumerateInstanceExtensionProperties",extensions);ENTRY("vkDestroyInstance",destroy);ENTRY("vkEnumeratePhysicalDevices",physical);ENTRY("vkGetPhysicalDeviceProperties",properties);ENTRY("vkGetPhysicalDeviceQueueFamilyProperties",queues);ENTRY("vkGetPhysicalDeviceFeatures",features);ENTRY("vkGetPhysicalDeviceMemoryProperties",memory);ENTRY("vkEnumerateDeviceExtensionProperties",device_extensions);ENTRY("vkCreateDevice",create_device);
 return unused;
}
void *__wrap_dlopen(const char *name,int flags){(void)name;(void)flags;return(void *)(uintptr_t)1;}
void *__wrap_dlsym(void *h,const char *name){(void)h;(void)name;union{PFN_vkGetInstanceProcAddr f;void *p;}u={.f=gipa};return u.p;}
int __wrap_dlclose(void *h){(void)h;close_calls++;return 0;}
static void arm(uint32_t loader,uint32_t physical_api,bool low_loader,bool low_physical){
 loader_version=loader;physical_version=physical_api;requested=0;create_calls=device_calls=destroy_calls=close_calls=0;
 gpu_device *out=(gpu_device *)(uintptr_t)0x1234;
 gpu_result result=gpu_device_create_selected("software",&out);
 CHECK(out==NULL);CHECK(result!=GPU_OK);CHECK(close_calls==1u);
 if(low_loader){CHECK(device_calls==0u);CHECK(destroy_calls==0u);CHECK(result==GPU_ERR_NO_INSTANCE || result==GPU_ERR_NO_LOADER);}
 else if(low_physical){CHECK(create_calls==1u);CHECK(requested>=VK_API_VERSION_1_1);CHECK(device_calls==0u);CHECK(destroy_calls==1u);}
 else{CHECK(create_calls==1u);CHECK(requested==VK_API_VERSION_1_1);CHECK(device_calls>0u);CHECK(destroy_calls==1u);CHECK(result==GPU_ERR_NO_DEVICE);}
 printf("T1035 limits loader=%u physical=%u requested=%u status=%d create=%u device=%u destroyed=%u closed=%u\n",loader,physical_api,requested,result,create_calls,device_calls,destroy_calls,close_calls);
}
int main(void){arm(VK_API_VERSION_1_0,VK_API_VERSION_1_1,true,false);arm(VK_API_VERSION_1_1,VK_API_VERSION_1_0,false,true);arm(VK_API_VERSION_1_1,VK_API_VERSION_1_1,false,false);printf("T1035 minimum %u checks %u failures\n",checks,failures);return failures?1:0;}
