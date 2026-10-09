/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "recomp_callback.h"
#include "recomp_abi.h"
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
#include "host_runtime.h"
#include "kernel_sync.h"
#include "kernel_event.h"
#include "kernel_hle.h"
#include "kernel_call.h"
#include "guest_mem.h"
#include "xnet_dpc.h"
#include <stdlib.h>
#include <fenv.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
extern _Thread_local uint32_t g_flag_bridge_eflags, g_flag_bridge_mask; /* T554, absent from an older lift */
#define EACH_X(M) M(g_xmm0) M(g_xmm1) M(g_xmm2) M(g_xmm3) M(g_xmm4) M(g_xmm5) M(g_xmm6) M(g_xmm7)
#define EACH_M(M) M(g_mm0) M(g_mm1) M(g_mm2) M(g_mm3) M(g_mm4) M(g_mm5) M(g_mm6) M(g_mm7)
#define SEED_X(v) memset(&(v),0x62,sizeof(v));
#define SEED_M(v) memset(&(v),0x73,sizeof(v));
#define CHANGE(v) memset(&(v),0x94,sizeof(v));
#define CHECK_X(v) CHECK(!memcmp(&(v),xbytes,sizeof(v)));
#define CHECK_M(v) CHECK(!memcmp(&(v),mbytes,sizeof(v)));
static unsigned checks;
#define CHECK(expr) do { ++checks; if(!(expr)){fprintf(stderr,"CHECK failed %s:%d: %s\n",__FILE__,__LINE__,#expr);abort();}} while(0)
static void *fault_page;
static unsigned mode;
static unsigned calls;
static bool missing;
static uint32_t payload[]={11,22,33};
static uint32_t seen_irql=99,seen_pcr=99;
static void leaf(void){
 ++calls;seen_irql=kernel_sync_current_irql();seen_pcr=*(uint8_t *)(uintptr_t)(g_fs_base+0x24);
 if(mode==4)(void)kernel_sync_raise_irql(KERNEL_IRQL_HIGH);
 uint32_t *frame=(void *)(uintptr_t)g_esp;
 CHECK(frame[0]==0);CHECK(memcmp((void *)(uintptr_t)frame[1],payload,12)==0);
 CHECK(*(uint32_t *)(uintptr_t)g_fs_base==UINT32_MAX);
 CHECK(!recomp_callback_run(0x22020,0x21000000,0x21001000,payload));
 *(uint32_t *)(uintptr_t)0x21002010+=1;
 g_eax=91;g_ecx=92;g_edx=93;g_ebx=94;g_esi=95;g_edi=96;g_ebp=97;
 g_df=1;g_seh_ebp=98;g_fp_top=6;g_fp_cmp=4;g_fp_cc=123;g_fp_control_word=456;
 g_flag_bridge_eflags=0xFFFFFFFFu;g_flag_bridge_mask=0xAAAAu;
 memset(g_fp_stack,0x55,sizeof(double)*8);
 EACH_X(CHANGE) EACH_M(CHANGE)
 *(uint32_t *)(uintptr_t)g_fs_base=123;g_fs_base=0;
 (void)fesetround(FE_DOWNWARD);(void)feraiseexcept(FE_INVALID);
 if(mode==2)host_run_stop(HOST_STOP_UNIMPLEMENTED,0x1234,7,"callback stop");
 if(mode==3){*(volatile uint32_t *)fault_page=1;}
 g_esp+=mode==1?8u:4u;
}
static uint32_t xnet_expected[4]={0x11223344u,0x55667788u,0x99AABBCCu,0xDDEEFF00u};
static void xnet_dpc(void) {
 ++calls;
 const uint32_t *frame = (const void *)(uintptr_t)g_esp;
 CHECK(frame[0] == 0u); CHECK(!memcmp(frame+1,xnet_expected,sizeof(xnet_expected)));
 CHECK(kernel_sync_current_irql() == KERNEL_IRQL_DISPATCH);
 CHECK(*(uint32_t *)(uintptr_t)g_fs_base == UINT32_MAX);
 g_eax=91; g_ecx=92; g_edx=93; g_ebx=94; g_esi=95; g_edi=96; g_ebp=97;
 g_df=1; g_seh_ebp=98; g_fp_top=6; g_fp_cmp=4; g_fp_cc=123; g_fp_control_word=456;
 g_flag_bridge_eflags=0xFFFFFFFFu; g_flag_bridge_mask=0xAAAAu;
 memset(g_fp_stack,0x55,sizeof(double)*8); EACH_X(CHANGE) EACH_M(CHANGE)
 (void)fesetround(FE_DOWNWARD); (void)feraiseexcept(FE_INVALID);
 *(uint32_t *)(uintptr_t)g_fs_base=123; g_fs_base=0;
 if (mode==2) host_run_stop(HOST_STOP_UNIMPLEMENTED,0x43A8D3u,150u,"XNET deferred dependency");
 if (mode==3) *(volatile uint32_t *)fault_page=1;
 g_esp += mode==1 ? 16u : 20u;
}
recomp_func_t recomp_lookup(uint32_t va){
 if (missing) return NULL;
 return va==0x22020 ? leaf : va==0x43A184u ? xnet_dpc : NULL;
}
static void seed(void){
 g_eax=1;g_ecx=2;g_edx=3;g_ebx=4;g_esi=5;g_edi=6;g_ebp=7;
 g_esp=0x22000000;g_fs_base=0x21002000;g_seh_ebp=8;g_df=0;
 g_fp_top=2;g_fp_cmp=3;g_fp_cc=4;g_fp_control_word=5;
 g_flag_bridge_eflags=0x40u;g_flag_bridge_mask=0x8C5u;
 memset(g_fp_stack,0x33,sizeof(double)*8);EACH_X(SEED_X) EACH_M(SEED_M)*(uint32_t *)(uintptr_t)g_fs_base=0xabcdef;
 (void)fesetround(FE_UPWARD);(void)feclearexcept(FE_ALL_EXCEPT);
}
static void check(void){
 CHECK(g_eax==1&&g_ecx==2&&g_edx==3&&g_ebx==4&&g_esi==5&&g_edi==6&&g_ebp==7);
 CHECK(g_esp==0x22000000&&g_fs_base==0x21002000&&g_seh_ebp==8&&g_df==0);
 CHECK(g_fp_top==2&&g_fp_cmp==3&&g_fp_cc==4&&g_fp_control_word==5);
 /* T554: a flag bridge word published before a callback is still there after it. */
 CHECK(g_flag_bridge_eflags==0x40u&&g_flag_bridge_mask==0x8C5u);
 unsigned char bytes[64];memset(bytes,0x33,sizeof bytes);CHECK(!memcmp(bytes,g_fp_stack,64));
 unsigned char xbytes[16],mbytes[8];memset(xbytes,0x62,16);memset(mbytes,0x73,8);
 EACH_X(CHECK_X) EACH_M(CHECK_M)
 CHECK(*(uint32_t *)(uintptr_t)g_fs_base==0xabcdef);
 CHECK(fegetround()==FE_UPWARD&&!fetestexcept(FE_ALL_EXCEPT));
}
static void publish_pcr(uint32_t level){
 if(g_fs_base)*(uint8_t *)(uintptr_t)(g_fs_base+0x24)=(uint8_t)level;
}
/* T370: one run of the leaf, returns whether it came back normally. The three exits that must
 * restore the level are a bad return, a host stop and a fault, plus a callback that leaves the
 * level raised itself (mode 4). */
static bool run_leaf(unsigned selected,bool *stopped){
 mode=selected;seed();bool ok=false;*stopped=false;
 if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();
  ok=recomp_callback_run(0x22020,0x21000000,0x21001000,payload);
 }else *stopped=true;
 host_run_disarm();return ok;
}
static void dispatch_level_checks(void){
 kernel_sync_set_irql_publisher(publish_pcr);
 for(unsigned enabled=0;enabled<2;++enabled){
  recomp_callback_set_dispatch_level(enabled);
  const uint32_t expected=enabled?KERNEL_IRQL_DISPATCH:KERNEL_IRQL_PASSIVE;
  for(unsigned selected=0;selected<5;++selected){
   kernel_sync_reset();seen_irql=seen_pcr=99;const unsigned before=calls;bool stopped;
   const bool ok=run_leaf(selected,&stopped);
   CHECK(calls==before+1);
   CHECK(seen_irql==expected);CHECK(seen_pcr==expected);
   CHECK(stopped==(selected==2||selected==3));CHECK(ok==(selected==0||selected==4));
   /* Complete restoration on every exit, in the model and in the guest's own copy. */
   /* Default off the runner never touches the level, so a callback that left it raised stays raised. */
   const uint32_t after=(!enabled&&selected==4)?KERNEL_IRQL_HIGH:KERNEL_IRQL_PASSIVE;
   CHECK(kernel_sync_current_irql()==after);
   CHECK(*(uint8_t *)(uintptr_t)(0x21002000+0x24)==after);
   check();
  }
  /* Entered at APC level the callback still sees DISPATCH and the caller gets APC back. */
  kernel_sync_reset();(void)kernel_sync_raise_irql(KERNEL_IRQL_APC);seen_irql=seen_pcr=99;
  bool stopped;const unsigned before=calls;CHECK(run_leaf(0,&stopped));
  CHECK(calls==before+1&&seen_irql==(enabled?2u:1u)&&seen_pcr==(enabled?2u:1u));
  CHECK(kernel_sync_current_irql()==KERNEL_IRQL_APC);
  CHECK(*(uint8_t *)(uintptr_t)(0x21002000+0x24)==KERNEL_IRQL_APC);
  /* Above DISPATCH is never lowered to run a callback: refused before the leaf runs. */
  kernel_sync_reset();(void)kernel_sync_raise_irql(3);const unsigned refused_before=calls;
  mode=0;seed();CHECK(sigsetjmp(*host_run_jmp(),1)==0);host_run_arm();
  const bool high_ok=recomp_callback_run(0x22020,0x21000000,0x21001000,payload);host_run_disarm();
  CHECK(high_ok==!enabled);CHECK(calls==refused_before+(enabled?0u:1u));
  CHECK(kernel_sync_current_irql()==3);
 }
 recomp_callback_set_dispatch_level(false);kernel_sync_reset();kernel_sync_set_irql_publisher(NULL);
}
static void xnet_dpc_checks(void) {
 const uint32_t arguments[4] = {0x11223344u,0x55667788u,0x99AABBCCu,0xDDEEFF00u};
 for (mode=0;mode<4u;++mode) {
  kernel_sync_reset();
  (void)kernel_sync_raise_irql(KERNEL_IRQL_APC);
  seed();
  CHECK(sigsetjmp(*host_run_jmp(),0)==0);
  host_run_arm();
  const unsigned before=calls;
  host_stop stopped;
  bool captured=false;
  const bool ok=recomp_callback_run_xnet_dpc(0x43A184u,0x21000000u,0x21001000u,
                                            arguments,&stopped,&captured);
  CHECK(ok==(mode==0u));
  CHECK(captured==(mode==2u||mode==3u));
  if (captured) {
   CHECK(stopped.reason==(mode==2u?HOST_STOP_UNIMPLEMENTED:HOST_STOP_FAULT));
   if(mode==2u) CHECK(stopped.guest_address==0x43A8D3u&&stopped.ordinal==150u);
  }
  CHECK(calls==before+1u);
  CHECK(kernel_sync_current_irql()==KERNEL_IRQL_APC);
  CHECK(host_run_scope_depth()==0u);
  check();
  host_run_disarm();
 }
 kernel_sync_reset();
}
static uint32_t scheduler_call(unsigned ordinal, const uint32_t args[3]) {
 kernel_call_frame frame;
 CHECK(kernel_frame_build(&frame,0x21000800u,32u,args,3u));
 return kernel_hle_call(ordinal,&frame);
}
static void scheduler_checks(void) {
 kernel_hle_init();kernel_event_reset();kernel_sync_reset();guest_mem_reset();
 CHECK(kernel_event_register()==11u);
 xnet_expected[0]=0x21000500u;xnet_expected[1]=0x55667788u;
 const uint32_t init[3]={xnet_expected[0],0x43A184u,xnet_expected[1]};
 const uint32_t insert[3]={xnet_expected[0],xnet_expected[2],xnet_expected[3]};
 for (unsigned scenario=0;scenario<4u;++scenario) {
  mode=scenario;
  (void)scheduler_call(107u,init);
  CHECK(scheduler_call(119u,insert)==1u);
  CHECK(host_xnet_dpc_configure(false));
  seed(); const unsigned before=calls;
  host_xnet_dpc_service_hook();CHECK(calls==before);
  CHECK(host_xnet_dpc_configure(true));
  volatile bool propagated=false;
  if(sigsetjmp(*host_run_jmp(),0)==0) {
   host_run_arm();host_xnet_dpc_service_hook();
  } else {
   propagated=true;
   CHECK(host_run_result()->reason==(mode==2u?HOST_STOP_UNIMPLEMENTED:HOST_STOP_FAULT));
  }
  CHECK(propagated==(mode>=2u));
  CHECK(calls==before+1u);check();
  CHECK(kernel_sync_current_irql()==KERNEL_IRQL_PASSIVE);
  const kernel_event_object *object=kernel_event_object_at(xnet_expected[0]);
  CHECK(object!=NULL&&object->dpc_queued==(mode!=0u));
  /* A bad cleanup/refused callback remains queued and can be retried; a stop
   * is observed only after scheduler locks and original context are restored. */
  mode=0u;host_xnet_dpc_service_hook();
  CHECK(!kernel_event_object_at(xnet_expected[0])->dpc_queued);
  host_run_disarm();CHECK(host_xnet_dpc_shutdown());

 }
 kernel_event_reset();guest_mem_reset();
}
int main(void){
 fault_page=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 CHECK(fault_page!=MAP_FAILED);
 CHECK(mmap((void *)0x21000000,0x3000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void *)0x21000000);
 CHECK(!recomp_callback_run(0x22020,0x21000000,0x21001000,payload));
 for(mode=0;mode<4;++mode){
  seed();if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();
   CHECK(!recomp_callback_run(0,0x21000000,0x21001000,payload));
   CHECK(!recomp_callback_run(0xfe000001,0x21000000,0x21001000,payload));
   missing=true;CHECK(!recomp_callback_run(0x22020,0x21000000,0x21001000,payload));missing=false;
   CHECK(!recomp_callback_run(0x22020,0xfffffff0,0x10,payload));
   CHECK(!recomp_callback_run(0x22020,0x21000000,0x21000010,payload));
   CHECK(!recomp_callback_run(0x22020,0x21000000,0x21001001,payload));
   CHECK(!recomp_callback_run(0x22020,0x21002000,0x21003000,payload));
   /* Head is outside stack, but stack covers PCR IRQL/TLS control fields. */
   CHECK(!recomp_callback_run(0x22020,0x21002004,0x21003000,payload));
   extern int g_force_return;g_force_return=1;
   CHECK(!recomp_callback_run(0x22020,0x21000000,0x21001000,payload));CHECK(g_force_return==1);g_force_return=0;
   CHECK(mprotect((void *)0x21000000,0x1000,PROT_READ)==0);
   CHECK(!recomp_callback_run(0x22020,0x21000000,0x21001000,payload));
   CHECK(mprotect((void *)0x21000000,0x1000,PROT_READ|PROT_WRITE)==0);
   CHECK(mprotect((void *)0x21002000,0x1000,PROT_READ)==0);
   CHECK(!recomp_callback_run(0x22020,0x21000000,0x21001000,payload));
   CHECK(mprotect((void *)0x21002000,0x1000,PROT_READ|PROT_WRITE)==0);
   check();bool ok=recomp_callback_run(0x22020,0x21000000,0x21001000,payload);
   CHECK(mode<2&&ok==(mode==0));
  }else{CHECK(mode>=2);const host_stop *s=host_run_result();
   CHECK(s->reason==(mode==2?HOST_STOP_UNIMPLEMENTED:HOST_STOP_FAULT));
   if(mode==2)CHECK(s->guest_address==0x1234&&s->ordinal==7&&!strcmp(s->detail,"callback stop"));
  }check();host_run_disarm();
 }
 CHECK(calls==4&&*(uint32_t *)0x21002010==4);
 dispatch_level_checks();
 xnet_dpc_checks();
 scheduler_checks();
 printf("callback context: %u checks passed\n",checks);return 0;
}
