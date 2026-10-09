/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mu_fatx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures;
#define C(x) do { checks++; if(!(x)){ failures++; fprintf(stderr,"line %d: %s\n",__LINE__,#x); }} while(0)
int main(void) {
 size_t n=8u*1024u*1024u;
 unsigned char *b=malloc(n+64), *old=malloc(n), *data=malloc(40000), out[20];
 memset(b+n,0xA5,64); memset(data,0x6B,40000);
 fatx_volume v; bool dir; uint32_t size, got;
 C(fatx_format(b,n,0x12345678)==FATX_OK); C(fatx_open(&v,b,n)==FATX_OK);
 C(fatx_mkdir_path(&v,"45410066")==FATX_OK);
 C(fatx_mkdir_path(&v,"45410066")==FATX_E_EXISTS);
 C(fatx_write_path(&v,"45410066/STATE.DAT",data,40000)==FATX_OK);
 C(fatx_stat_path(&v,"45410066/state.dat",&dir,&size)==FATX_OK && !dir && size==40000);
 memset(out,0xAB,sizeof out); C(fatx_read_range(&v,"45410066/state.dat",16380,out,12,&got)==FATX_OK && got==12);
 for(unsigned i=0;i<12;i++) C(out[i]==0x6B);
 C(out[12]==0xAB);
 C(fatx_resize_path(&v,"45410066/state.dat",40008,40004,"ABCD",4)==FATX_OK);
 C(fatx_read_range(&v,"45410066/state.dat",40000,out,20,&got)==FATX_OK && got==8);
 C(memcmp(out,"\0\0\0\0ABCD",8)==0);
 C(fatx_resize_path(&v,"45410066/state.dat",3,0,NULL,0)==FATX_OK);
 C(fatx_stat_path(&v,"45410066/state.dat",&dir,&size)==FATX_OK && size==3);
 C(fatx_stat_path(&v,"45410066",&dir,&size)==FATX_OK && dir && size==0);
 C(fatx_write_path(&v,"45410066","x",1)==FATX_E_IS_DIR);
 C(fatx_write_path(&v,"missing/a",data,4)==FATX_E_PATH_NOT_FOUND);
 C(fatx_write_path(&v,"45410066/state.dat/a",data,4)==FATX_E_NOT_DIR);
 C(fatx_write_path(&v,"../a",data,4)==FATX_E_NAME);
 C(fatx_resize_path(&v,"45410066/state.dat",UINT64_MAX,0,NULL,0)==FATX_E_ARGUMENT);
 memcpy(old,b,n); C(fatx_write_path(&v,"45410066/state.dat",data,UINT32_MAX)==FATX_E_NO_SPACE); C(memcmp(b,old,n)==0);
 C(fatx_open(&v,b,n)==FATX_OK); C(fatx_read_range(&v,"45410066/state.dat",0,out,20,&got)==FATX_OK && got==3);
 for(unsigned i=0;i<64;i++) C(b[n+i]==0xA5);
 free(b); free(old); free(data);
 n=FATX_MIN_IMAGE; b=malloc(n+FATX_CLUSTER_BYTES); memset(b+n,0xA5,FATX_CLUSTER_BYTES);
 C(fatx_format(b,n,1)==FATX_OK); C(fatx_open(&v,b,n)==FATX_OK); C(v.cluster_count==2 && fatx_free_clusters(&v)==1);
 data=malloc(FATX_CLUSTER_BYTES+1); memset(data,0x77,FATX_CLUSTER_BYTES+1);
 C(fatx_write_file(&v,"A",data,FATX_CLUSTER_BYTES+1)==FATX_E_NO_SPACE);
 C(fatx_write_file(&v,"A",data,FATX_CLUSTER_BYTES)==FATX_OK); C(fatx_free_clusters(&v)==0);
 C(fatx_write_file(&v,"B",data,1)==FATX_E_NO_SPACE);
 for(unsigned i=0;i<FATX_CLUSTER_BYTES;i++) C(b[n+i]==0xA5);
 free(data); free(b);
 printf("%u checks, %u failures\n",checks,failures); return failures!=0;
}
