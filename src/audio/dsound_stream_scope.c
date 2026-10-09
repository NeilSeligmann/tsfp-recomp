/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_stream_scope.h"
#include "kernel_call.h"
#include <string.h>
static uint16_t word16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1]<<8u); }
static uint32_t word32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8u | (uint32_t)p[2]<<16u | (uint32_t)p[3]<<24u; }
bool dsound_stream_scope_validate(const uint8_t *descriptor,size_t descriptor_bytes,
                                  const uint8_t *format,size_t format_bytes,
                                  dsound_stream_scope *output)
{
    if(descriptor==NULL || format==NULL || output==NULL ||
       descriptor_bytes!=DSOUND_STREAM_DESCRIPTOR_BYTES || format_bytes!=DSOUND_STREAM_FORMAT_BYTES)
        return false;
    dsound_stream_scope result={0};
    result.flags=word32(descriptor);result.max_packets=word32(descriptor+4u);
    result.format_address=word32(descriptor+8u);
    result.channels=word16(format+2u);result.sample_rate=word32(format+4u);
    result.average_bytes_per_second=word32(format+8u);result.block_align=word16(format+12u);
    if((result.flags!=0u && result.flags!=0x10u) || result.max_packets!=3u ||
       result.format_address==0u ||
       (uint64_t)result.format_address+DSOUND_STREAM_FORMAT_BYTES>UINT64_C(0x100000000) ||
       word32(descriptor+12u)!=0u || word32(descriptor+16u)!=0u ||
       word32(descriptor+20u)!=0u || word16(format)!=0x69u || result.sample_rate!=44100u ||
       word16(format+14u)!=4u || word16(format+16u)!=2u || word16(format+18u)!=64u)
        return false;
    const uint16_t channels=result.flags==0u?2u:1u;
    if(result.channels!=channels || result.block_align!=36u*channels ||
       result.average_bytes_per_second!=(((uint32_t)result.block_align*44100u)>>6u))
        return false;
    memcpy(result.descriptor,descriptor,sizeof(result.descriptor));
    memcpy(result.format,format,sizeof(result.format));*output=result;return true;
}
bool dsound_stream_scope_snapshot(uint32_t descriptor,dsound_stream_scope *output)
{
    if(output==NULL)return false;
    uint8_t desc[DSOUND_STREAM_DESCRIPTOR_BYTES],format[DSOUND_STREAM_FORMAT_BYTES];
    if(!kernel_guest_read_bytes(descriptor,desc,sizeof(desc)))return false;
    const uint32_t address=word32(desc+8u);
    if(address==0u || (uint64_t)address+sizeof(format)>UINT64_C(0x100000000) ||
       ((uint64_t)descriptor+sizeof(desc)>address && (uint64_t)address+sizeof(format)>descriptor))
        return false;
    dsound_stream_scope result;
    if(!kernel_guest_read_bytes(address,format,sizeof(format)) ||
       !dsound_stream_scope_validate(desc,sizeof(desc),format,sizeof(format),&result))return false;
    result.descriptor_address=descriptor;*output=result;return true;
}
