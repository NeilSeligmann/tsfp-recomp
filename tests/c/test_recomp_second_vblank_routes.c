/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "host_runtime.h"
#include "recomp_abi.h"
#include "recomp_callback.h"
#include "recomp_cooperative.h"
#include "recomp_second_vblank.h"
#include "d3d8_gpu.h"
#include "kernel_clock.h"
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
#include <fenv.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
static unsigned checks;
#define CHECK(c) do {checks++;if(!(c)){fprintf(stderr,"second route failed %d: %s\n",__LINE__,#c);abort();}}while(0)
#define EACH_X(M) M(g_xmm0) M(g_xmm1) M(g_xmm2) M(g_xmm3) M(g_xmm4) M(g_xmm5) M(g_xmm6) M(g_xmm7)
#define EACH_M(M) M(g_mm0) M(g_mm1) M(g_mm2) M(g_mm3) M(g_mm4) M(g_mm5) M(g_mm6) M(g_mm7)
#define SEED_X(v) memset(&(v),0x62,sizeof(v));
#define SEED_M(v) memset(&(v),0x73,sizeof(v));
#define CHECK_X(v) CHECK(!memcmp(&(v),xbytes,sizeof(v)));
#define CHECK_M(v) CHECK(!memcmp(&(v),mbytes,sizeof(v)));
static void store(uint32_t at,uint32_t value){*(uint32_t *)(uintptr_t)at=value;}
static uint32_t load(uint32_t at){return *(uint32_t *)(uintptr_t)at;}
static void map(uint32_t at,uint32_t n)
{CHECK(mmap((void *)(uintptr_t)at,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void *)(uintptr_t)at);}
static void seed(void)
{
 g_eax=1;g_ecx=2;g_edx=3;g_ebx=4;g_esi=5;g_edi=6;g_ebp=7;
 g_esp=0x22000000;g_fs_base=0x21002000;g_seh_ebp=8;g_df=0;
 g_fp_top=2;g_fp_cmp=3;g_fp_cc=4;g_fp_control_word=5;
 memset(g_fp_stack,0x33,sizeof(double)*8);EACH_X(SEED_X) EACH_M(SEED_M)
 store(g_fs_base,0xabcdef);(void)fesetround(FE_UPWARD);(void)feclearexcept(FE_ALL_EXCEPT);
}
static void preserved(void)
{
 CHECK(g_eax==1&&g_ecx==2&&g_edx==3&&g_ebx==4&&g_esi==5&&g_edi==6&&g_ebp==7);
 CHECK(g_esp==0x22000000&&g_fs_base==0x21002000&&g_seh_ebp==8&&g_df==0);
 CHECK(g_fp_top==2&&g_fp_cmp==3&&g_fp_cc==4&&g_fp_control_word==5);
 unsigned char bytes[64],xbytes[16],mbytes[8];memset(bytes,0x33,64);memset(xbytes,0x62,16);memset(mbytes,0x73,8);
 CHECK(!memcmp(bytes,g_fp_stack,64));EACH_X(CHECK_X) EACH_M(CHECK_M)
 CHECK(load(g_fs_base)==0xabcdef);CHECK(fegetround()==FE_UPWARD&&!fetestexcept(FE_ALL_EXCEPT));
 CHECK(host_run_scope_depth()==0u);
}
static bool confirmed(uint32_t handle,uint32_t fs){return handle==2u&&fs==0x23000000u;}
static void provider(uint32_t callee,void *unused)
{(void)unused;recomp_second_vblank_poll(callee,1u,g_fs_base,g_esp,0u);}
static void *publish(void *unused)
{
 (void)unused;
 if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();recomp_second_vblank_note_wait_completed(true,2u,0x23000000u);}
 else abort();host_run_disarm();return NULL;
}
int main(void)
{
 CHECK(recomp_cooperative_ready());CHECK(recomp_lookup(0x22020u)!=NULL);
 map(0x3E3000u,0x4000u);map(0x563000u,4096u);map(0x21000000u,0x3000u);map(0x22000000u,4096u);
 seed();store(0x3E3F58u,0x3E3F60u);store(0x3E3F60u+0x1DE8u,1u);store(0x3E3F60u+0x1DDCu,0x02480104u);
 CHECK(recomp_second_vblank_configure(true,recomp_callback_run,0x21000000u,0x21001000u,confirmed));
 recomp_second_vblank_bind_owner(1u,g_fs_base,0x3801D9u,0x37FE1Du);
 recomp_second_vblank_note_registration(0x22020u,1u,g_fs_base);store(0x3E3F60u+0x1DB8u,0x22020u);
 CHECK(recomp_cooperative_configure(provider,NULL));
 const uint64_t clock=kernel_clock_peek();const d3d8_gpu_stats stats=d3d8_gpu_get_stats();
 store(g_esp,0x3D455u);
 if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();recomp_call_safepoint(0x1538C0u);}
 else CHECK(false);host_run_disarm();preserved();CHECK(load(0x563918u)==1u&&load(0x3E3F60u+0x1DE8u)==2u);
 pthread_t worker;CHECK(pthread_create(&worker,NULL,publish,NULL)==0);CHECK(pthread_join(worker,NULL)==0);
 store(g_esp,0x3D66Du);
 uint8_t before[0x2450];memcpy(before,(void *)(uintptr_t)0x3E3F60u,sizeof(before));
 if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();recomp_call_safepoint(0x1538C0u);}
 else CHECK(false);host_run_disarm();preserved();
 CHECK(load(0x563918u)==2u&&load(0x3E3F60u+0x1DE8u)==3u);
 uint32_t count=3u;memcpy(before+0x1DE8u,&count,4u);CHECK(!memcmp(before,(void *)(uintptr_t)0x3E3F60u,sizeof(before)));
 recomp_second_vblank_snapshot state;recomp_second_vblank_get_snapshot(&state);
 CHECK(state.first_delivered&&state.second_attempted&&state.second_delivered&&!state.inflight&&state.credits==0u);
 if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();recomp_call_safepoint(0x1538C0u);CHECK(false);}
 else CHECK(host_run_result()->reason==HOST_STOP_XDK_UNIMPLEMENTED&&host_run_result()->guest_address==0x1538C0u);
 host_run_disarm();preserved();CHECK(load(0x563918u)==2u&&load(0x3E3F60u+0x1DE8u)==3u);
 CHECK(kernel_clock_peek()==clock);const d3d8_gpu_stats now=d3d8_gpu_get_stats();CHECK(!memcmp(&stats,&now,sizeof(stats)));
 CHECK(recomp_cooperative_configure(NULL,NULL));CHECK(recomp_second_vblank_configure(false,NULL,0u,0u,NULL));CHECK(recomp_second_vblank_reset());
 printf("second callback routes: %u checks passed\n",checks);return 0;
}
