/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual source-built window constructor; no SDL/display/native window is opened. */
#define main device_controls_main
#include "test_t1035_device_minimum.c"
#undef main
#include "gpu_window.h"
#include <SDL3/SDL_vulkan.h>
const char *const *__wrap_SDL_Vulkan_GetInstanceExtensions(Uint32 *count){static const char *ext[1]={NULL};*count=0;return ext;}
SDL_FunctionPointer __wrap_SDL_Vulkan_GetVkGetInstanceProcAddr(void){return (SDL_FunctionPointer)gipa;}
bool __wrap_SDL_Vulkan_CreateSurface(SDL_Window *w,VkInstance i,const VkAllocationCallbacks *a,VkSurfaceKHR *s){(void)w;(void)i;(void)a;*s=(VkSurfaceKHR)(uintptr_t)4;return true;}
static void window_arm(uint32_t loader,uint32_t physical_api,bool low_loader,bool low_physical){
 loader_version=loader;physical_version=physical_api;requested=0;create_calls=device_calls=destroy_calls=close_calls=0;
 const char *error="unchanged";gpu_window *out=gpu_window_create((SDL_Window *)(uintptr_t)1,&error);
 CHECK(out==NULL);CHECK(error!=NULL);CHECK(strcmp(error,"unchanged")!=0);
 if(low_loader){CHECK(device_calls==0u);CHECK(destroy_calls==0u);}
 else if(low_physical){CHECK(create_calls==1u);CHECK(requested>=VK_API_VERSION_1_1);CHECK(device_calls==0u);CHECK(destroy_calls==1u);}
 else{CHECK(create_calls==1u);CHECK(requested==VK_API_VERSION_1_1);CHECK(device_calls>0u);CHECK(destroy_calls==1u);}
 printf("T1035 window loader=%u physical=%u requested=%u create=%u device=%u destroyed=%u error=%s\n",loader,physical_api,requested,create_calls,device_calls,destroy_calls,error);
}
int main(void){window_arm(VK_API_VERSION_1_0,VK_API_VERSION_1_1,true,false);window_arm(VK_API_VERSION_1_1,VK_API_VERSION_1_0,false,true);window_arm(VK_API_VERSION_1_1,VK_API_VERSION_1_1,false,false);printf("T1035 window %u checks %u failures\n",checks,failures);return failures?1:0;}
