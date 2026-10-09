/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "dsound_effects_metadata.h"
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include "kernel_call.h"
static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8u |
           (uint32_t)p[2]<<16u | (uint32_t)p[3]<<24u;
}
static bool range(uint32_t offset, uint32_t length, uint32_t start, uint32_t bytes)
{
    return offset >= start && (uint64_t)offset+length <= (uint64_t)start+bytes;
}
static bool overlap(uint32_t a, uint32_t n, uint32_t b, uint32_t m)
{
    return n != 0u && m != 0u && (uint64_t)a+n>b && (uint64_t)b+m>a;
}
bool dsound_effects_validate_layout(const uint8_t *image, size_t bytes,
                                   dsound_effects_metadata *output)
{
    if (image == NULL || output == NULL || bytes < 0x818u || bytes > UINT32_MAX)
        return false;
    dsound_effects_metadata result = {0};
    result.code_offset=0x818u;
    const uint64_t code_bytes=(uint64_t)word(image+0x804u)*4u;
    const uint64_t state_bytes=(uint64_t)word(image+0x80Cu)*4u;
    const uint64_t state_offset=0x818u+code_bytes;
    const uint64_t descriptor=state_offset+state_bytes;
    if (code_bytes>UINT32_MAX || state_bytes>UINT32_MAX || descriptor+8u>bytes)
        return false;
    result.code_bytes=(uint32_t)code_bytes;
    result.state_offset=(uint32_t)state_offset;result.state_bytes=(uint32_t)state_bytes;
    result.descriptor_offset=(uint32_t)descriptor;
    result.map_count=word(image+descriptor);result.workspace_bytes=word(image+descriptor+4u);
    if (result.map_count==0u || result.map_count>DSOUND_EFFECTS_MAX_MAPS ||
        result.workspace_bytes==0u) return false;
    result.descriptor_bytes=8u+result.map_count*32u;
    const uint64_t iv_offset=descriptor+result.descriptor_bytes;
    result.iv_bytes=result.map_count*8u;
    if (iv_offset+result.iv_bytes>bytes) return false;
    result.iv_offset=(uint32_t)iv_offset;
    for (uint32_t i=0u;i<result.map_count;i++) {
        const uint8_t *p=image+descriptor+8u+i*32u;
        dsound_effects_map *m=&result.maps[i];
        m->code_offset=word(p);m->code_bytes=word(p+4u);
        m->state_offset=word(p+8u);m->state_bytes=word(p+12u);
        m->y_offset=word(p+16u);m->y_bytes=word(p+20u);
        const uint32_t workspace=word(p+24u);m->workspace_bytes=word(p+28u);
        if (!range(m->code_offset,m->code_bytes,result.code_offset,result.code_bytes) ||
            !range(m->state_offset,m->state_bytes,result.state_offset,result.state_bytes) ||
            m->y_offset!=0u || m->y_bytes!=0u || workspace<0xC000u) return false;
        m->workspace_offset=workspace-0xC000u;
        if (!range(m->workspace_offset,m->workspace_bytes,0u,result.workspace_bytes))
            return false;
        for (uint32_t j=0u;j<i;j++) {
            const dsound_effects_map *previous=&result.maps[j];
            if (overlap(m->code_offset,m->code_bytes,previous->code_offset,previous->code_bytes) ||
                overlap(m->state_offset,m->state_bytes,previous->state_offset,previous->state_bytes) ||
                overlap(m->workspace_offset,m->workspace_bytes,
                        previous->workspace_offset,previous->workspace_bytes)) return false;
        }
    }
    *output=result;return true;
}
static bool fingerprint(const uint8_t *image)
{
    uint64_t fnv=UINT64_C(0xCBF29CE484222325);uint32_t crc=UINT32_MAX;
    for (size_t i=0u;i<DSOUND_EFFECTS_IMAGE_BYTES;i++) {
        fnv=(fnv^image[i])*UINT64_C(0x100000001B3);crc^=image[i];
        for (unsigned bit=0u;bit<8u;bit++) crc=(crc>>1u)^((0u-(crc&1u))&0xEDB88320u);
    }
    return fnv==UINT64_C(0x9BBE14297F9AF82B) && ~crc==0xCAD22F6Fu;
}
bool dsound_effects_snapshot_guest(uint32_t address, uint32_t bytes, uint8_t *image,
                                   dsound_effects_metadata *output)
{
    if (address!=DSOUND_EFFECTS_IMAGE_ADDRESS || bytes!=DSOUND_EFFECTS_IMAGE_BYTES ||
        output==NULL || image==NULL) return false;
    const void *source=kernel_guest_at(address,bytes);
    if (source==NULL) return false;
    uint8_t snapshot[DSOUND_EFFECTS_IMAGE_BYTES];
    struct iovec local={snapshot,sizeof(snapshot)},remote={(void *)source,bytes};
    /* Unlike memcpy, a self process_vm_readv returns failure for PROT_NONE. */
    if (process_vm_readv(kernel_host_pid(),&local,1u,&remote,1u,0u)!=(ssize_t)bytes ||
        !fingerprint(snapshot)) return false;
    if (word(snapshot+0x800u)!=0u || word(snapshot+0x804u)!=2020u ||
        word(snapshot+0x808u)!=10152u || word(snapshot+0x80Cu)!=2022u ||
        word(snapshot+0x810u)!=3u || word(snapshot+0x814u)!=0u) return false;
    dsound_effects_metadata result;
    if (!dsound_effects_validate_layout(snapshot,sizeof(snapshot),&result) ||
        result.map_count!=9u || result.workspace_bytes!=393216u ||
        result.descriptor_offset!=0x4740u || result.descriptor_bytes!=296u ||
        result.iv_offset!=0x4868u || result.iv_bytes!=72u) return false;
    memcpy(image,snapshot,sizeof(snapshot));*output=result;return true;
}

bool dsound_effects_parse_guest(uint32_t address,uint32_t bytes,dsound_effects_metadata *output)
{
    uint8_t image[DSOUND_EFFECTS_IMAGE_BYTES];
    return dsound_effects_snapshot_guest(address,bytes,image,output);
}
