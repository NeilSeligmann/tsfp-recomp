/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic frame/capability fixtures; actual raw leaf separately authenticated. */
#define _GNU_SOURCE
#include "xmv_original.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
TSFP_RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_fs_base;
static unsigned checks, failures, terminations, frames;
static uint32_t expected_esp;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
static void other(void) {}
static void terminate(void) {
 uint32_t decoder=*(uint32_t *)(uintptr_t)(g_esp+4);
 g_eax=decoder;*(unsigned char *)(uintptr_t)(decoder+0x78)=1;g_esp+=8;terminations++;
}
static void frame(void) {
 CHECK(g_esp==expected_esp);CHECK(g_eax==0x12345678);
 CHECK(g_ecx==0x87654321);CHECK(g_edx==0xaabbccdd);
 CHECK(g_ebx==0x11223344);CHECK(g_esi==0x55667788);
 CHECK(g_edi==0x99aabbcc);CHECK(g_ebp==0xddeeff00);
 frames++;g_esp+=20;
}
const char *recomp_xmv_profile_identity(void){return "xmv-original-v1";}
recomp_func_t recomp_xmv_lookup_original(uint32_t a){return a==0x445252?terminate:a==0x44525d?frame:other;}
static void run(uint32_t owner,uint32_t name,uint32_t skip,uint32_t caller,bool enabled) {
 unsigned t=terminations,f=frames;
 expected_esp=0x18001000;g_esp=expected_esp;g_eax=0x12345678;g_ecx=0x87654321;g_edx=0xaabbccdd;
 g_ebx=0x11223344;g_esi=0x55667788;g_edi=0x99aabbcc;g_ebp=0xddeeff00;
 memset((void *)(uintptr_t)g_esp,0,0x400);
 uint32_t *s=(uint32_t *)(uintptr_t)g_esp;s[0]=caller;s[1]=0x18002000;
 s[0x298/4]=owner;s[0x29c/4]=name;s[0x2a0/4]=skip;
 *(unsigned char *)(uintptr_t)(s[1]+0x78)=0;
 unsigned char before[0x400];memcpy(before,(void *)(uintptr_t)g_esp,sizeof before);
 CHECK(xmv_original_set_skip_intro(enabled));
 if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();CHECK(xmv_original_dispatch(0x44525d));}
 else CHECK(false);
 host_run_disarm();
 bool match=enabled&&caller==0x305cc&&skip==0&&((owner==0x70606&&(name==0x47cfc0||name==0x47cfb8||name==0x47cfb0||name==0x47cfa8))||((owner==0x70614||owner==0x7063c)&&name==0x479628)||(owner==0x70631&&name==0x47cfa8));
 CHECK(memcmp(before,(void *)(uintptr_t)expected_esp,sizeof before)==0);
 CHECK(terminations==t+(unsigned)match);CHECK(frames==f+1);CHECK(g_esp==expected_esp+20);
 CHECK(*(unsigned char *)(uintptr_t)(0x18002000+0x78)==(unsigned)match);
}
int main(void){
 CHECK(mmap((void *)0x18000000,0x10000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=(void *)-1);
 CHECK(!xmv_original_set_skip_intro(true));CHECK(xmv_original_configure(true));
 uint32_t names[]={0x47cfc0,0x47cfb8,0x47cfb0,0x47cfa8};
 for(unsigned i=0;i<4;i++){run(0x70606,names[i],0,0x305cc,true);run(0x70606,names[i],0,0x305cc,false);}
 run(0x70614,0x479628,0,0x305cc,true);
 run(0x2d3cc1,0x49f630,1,0x305cc,true);
 run(0x70606,0x49f630,0,0x305cc,true);
 run(0x70614,0x47cfa8,0,0x305cc,true);
 run(0x70631,0x47cfa8,0,0x305cc,true);
 run(0x7063c,0x479628,0,0x305cc,true);
 run(0x70631,0x47cfb8,0,0x305cc,true);
 run(0x70606,0x47cfa8,1,0x305cc,true);
 run(0x70606,0x47cfa8,0,0x305cd,true);
 CHECK(xmv_original_set_skip_intro(false));CHECK(xmv_original_configure(false));
 printf("%u checks %u failures\n",checks,failures);return failures?1:0;
}
