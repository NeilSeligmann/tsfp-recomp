/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "dsound_effects_metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
/* Standalone parser tests need only address translation, no kernel execution. */
pid_t kernel_host_pid(void)
{
    return getpid();
}
void *kernel_guest_at(uint32_t address, size_t length)
{
    if (address!=DSOUND_EFFECTS_IMAGE_ADDRESS || length!=DSOUND_EFFECTS_IMAGE_BYTES)
        return NULL;
    return (void *)(uintptr_t)address;
}
int main(int argc, char **argv)
{
    if (argc!=3) return 2;
    FILE *file=fopen(argv[2],"rb");if (file==NULL) return 2;
    if (fseek(file,0,SEEK_END)!=0) return 2;
    const long length=ftell(file);if (length<0 || fseek(file,0,SEEK_SET)!=0) return 2;
    uint8_t *image=malloc((size_t)length+1u);if(image==NULL) return 2;
    if (fread(image,1u,(size_t)length,file)!=(size_t)length) return 2;
    fclose(file);dsound_effects_metadata metadata,unchanged;
    memset(&metadata,0xA5,sizeof(metadata));unchanged=metadata;bool valid;
    if (strcmp(argv[1],"layout")==0) valid=dsound_effects_validate_layout(image,(size_t)length,&metadata);
    else {
        void *base=mmap((void *)(uintptr_t)(DSOUND_EFFECTS_IMAGE_ADDRESS&~0xFFFu),
                        0x6000u,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
        if(base==MAP_FAILED) return 2;
        if ((size_t)length>DSOUND_EFFECTS_IMAGE_BYTES) return 2;
        memcpy((void *)(uintptr_t)DSOUND_EFFECTS_IMAGE_ADDRESS,image,(size_t)length);
        uint32_t address=DSOUND_EFFECTS_IMAGE_ADDRESS,bytes=(uint32_t)length;
        if(strcmp(argv[1],"wrongva")==0) address++;
        if(strcmp(argv[1],"guard")==0 && mprotect(base,0x6000u,PROT_NONE)!=0) return 2;
        uint8_t snapshot[DSOUND_EFFECTS_IMAGE_BYTES];memset(snapshot,0xA5,sizeof(snapshot));
        valid=dsound_effects_snapshot_guest(address,bytes,snapshot,&metadata);
        if(valid) {if(memcmp(snapshot,image,sizeof(snapshot))!=0)return 3;}
        else {for(size_t i=0u;i<sizeof(snapshot);i++)if(snapshot[i]!=0xA5u)return 3;}
        if(strcmp(argv[1],"guard")==0 && mprotect(base,0x6000u,PROT_READ|PROT_WRITE)!=0) return 2;
        if(memcmp((void *)(uintptr_t)DSOUND_EFFECTS_IMAGE_ADDRESS,image,(size_t)length)!=0) return 3;
        munmap(base,0x6000u);
    }
    free(image);
    if (!valid) {if(memcmp(&metadata,&unchanged,sizeof(metadata))!=0) return 3;puts("REFUSED");return 0;}
    printf("VALID %u %u %u %u %u %u\n",metadata.map_count,metadata.workspace_bytes,
           metadata.code_bytes,metadata.state_bytes,metadata.descriptor_offset,metadata.iv_offset);
    for(uint32_t i=0u;i<metadata.map_count;i++) {
        const dsound_effects_map *m=&metadata.maps[i];
        printf("%u %u %u %u %u %u %u %u\n",m->code_offset,m->code_bytes,m->state_offset,
               m->state_bytes,m->y_offset,m->y_bytes,m->workspace_offset,m->workspace_bytes);
    }
    return 0;
}
