/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mu_fatx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define C(x) do{checks++;if(!(x)){failures++;printf("FAIL line%d: %s\n",__LINE__,#x);}}while(0)
int main(void){
 unsigned checks=0,failures=0;size_t n=FATX_MIN_IMAGE;
 unsigned char *b=malloc(n+FATX_CLUSTER_BYTES),*before=malloc(n),*payload=malloc(FATX_CLUSTER_BYTES),out[128];
 memset(b+n,0xD3,FATX_CLUSTER_BYTES);for(unsigned i=0;i<FATX_CLUSTER_BYTES;i++)payload[i]=(unsigned char)(i*29+11);
 fatx_volume v;C(fatx_format(b,n,0x715839AB)==FATX_OK);C(fatx_open(&v,b,n)==FATX_OK);
 C(v.cluster_count==2);C(fatx_write_file(&v,"BOUNDARY.BIN",payload,FATX_CLUSTER_BYTES)==FATX_OK);
 C(fatx_free_clusters(&v)==0);memcpy(before,b,n);
 C(fatx_write_file(&v,"SECOND.BIN",payload,127)==FATX_E_NO_SPACE);C(memcmp(before,b,n)==0);
 C(fatx_resize_path(&v,"BOUNDARY.BIN",FATX_CLUSTER_BYTES+127,FATX_CLUSTER_BYTES,payload,127)==FATX_E_NO_SPACE);
 C(memcmp(before,b,n)==0);uint32_t got=0;
 C(fatx_read_range(&v,"boundary.bin",FATX_CLUSTER_BYTES-113,out,sizeof out,&got)==FATX_OK);C(got==113);
 C(memcmp(out,payload+FATX_CLUSTER_BYTES-113,113)==0);
 C(fatx_resize_path(&v,"BOUNDARY.BIN",UINT64_MAX,0,NULL,0)==FATX_E_ARGUMENT);C(memcmp(before,b,n)==0);
 C(fatx_write_path(&v,"nonexistent/leaf",payload,1)==FATX_E_PATH_NOT_FOUND);C(memcmp(before,b,n)==0);
 C(fatx_read_range(&v,"BOUNDARY.BIN",UINT64_MAX,out,1,&got)==FATX_OK && got==0);
 unsigned guard_bad=0;for(unsigned i=0;i<FATX_CLUSTER_BYTES;i++)guard_bad|=(b[n+i]!=0xD3);C(guard_bad==0);
 free(b);free(before);free(payload);printf("T1134 clusters: %u checks, %u failures\n",checks,failures);return failures!=0;
}
