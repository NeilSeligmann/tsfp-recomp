/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_file.h"
#include "kernel_hle.h"
#include "kernel_io.h"
#include "kernel_object.h"
#include "xnet_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BASE 0x20000000u
static int quiet(const char *format, ...) { (void)format; return 0; }
static void dump(const char *name, uint32_t address, size_t count)
{
    const unsigned char *bytes=kernel_guest_at(address,count);
    printf("%s ",name);
    for(size_t i=0;i<count;++i) printf("%02x",bytes[i]);
    putchar('\n');
}
int main(int argc,char **argv)
{
    if(argc!=5) return 2;
    const unsigned mode=(unsigned)strtoul(argv[1],NULL,0);
    const unsigned prior=(unsigned)strtoul(argv[2],NULL,0);
    kernel_hle_init();kernel_hle_set_log(quiet);
    kernel_object_reset();kernel_file_reset();kernel_io_reset();guest_mem_reset();
    (void)kernel_object_register();(void)kernel_file_register();(void)kernel_io_register();
    kernel_file_set_missing_policy(KERNEL_FILE_MISSING_EMPTY);
    const guest_region_request request={.bytes=0x10000u,.protect=4u,.state=0x1000u,.fixed_base=BASE};
    nt_status status;
    if(guest_region_alloc(&request,&status)!=BASE)return 3;
    memset(kernel_guest_at(BASE,0x10000u),0x96,0x10000u);
    const char *prefix="\\Device\\Harddisk0\\partition0";
    if(mode!=2u&&!kernel_file_mount_host_device(prefix,argv[3],"raw",8192u))return 4;
    const uint32_t name=BASE+0x4000u,attributes=name+8u,path=BASE+0x4100u;
    const uint32_t length=(uint32_t)strlen(prefix);
    if(!kernel_guest_write_bytes(path,prefix,length+1u)||
       !kernel_guest_write_u32(name,length|((length+1u)<<16))||
       !kernel_guest_write_u32(name+4u,path)||
       !kernel_guest_write_u32(attributes,0u)||
       !kernel_guest_write_u32(attributes+4u,name)||
       !kernel_guest_write_u32(attributes+8u,0x40u))return 5;
    const uint32_t args[6]={BASE+0x4200u,0xc0100000u,attributes,BASE+0x4210u,3u,mode==3u?0u:0x10u};
    kernel_call_frame call;
    if(!kernel_frame_build(&call,BASE+0x4300u,32u,args,6u))return 6;
    const uint32_t open=mode==2u?kernel_hle_call(202u,&call):kernel_file_open_stored(&call);
    if(open>=0x80000000u)return 7;
    uint32_t handle;
    if(!kernel_guest_read_u32(BASE+0x4200u,&handle))return 8;
    const xnet_config_kernel source={BASE+0x1000u,BASE+0x2000u,BASE+0x3000u,36u};
    if(prior){
        FILE *file=fopen(argv[4],"rb");
        if(!file||fread(kernel_guest_at(source.sector,512u),1,512u,file)!=512u||fclose(file))return 9;
    }
    bool valid=false;
    const bool executed=xnet_config_read_sector_kernel(&source,handle,0u,BASE+0x5000u,&valid);
    printf("executed %u\nvalid %u\n",(unsigned)executed,(unsigned)valid);
    dump("sector",source.sector,512u);dump("payload",BASE+0x5000u,492u);
    dump("iosb",source.iosb,8u);
    printf("zeros %llu\nfabricated %u\n",(unsigned long long)kernel_file_device_zero_bytes(),kernel_file_fabricated_count());
    kernel_file_reset();kernel_object_reset();guest_mem_reset();
    return 0;
}
