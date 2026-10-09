/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_effects_dsp.h"
#include "dsound_effects_cipher.h"
#include "dsp_dma.h"
#include "dsp_dma_regs.h"
#include "interp/dsp_cpu.h"
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
/* Pinned DMA uses a process-global transfer scratch cache. Serialize core runs
 * across all contexts; per-context caller serialization alone is insufficient. */
static pthread_mutex_t dma_lock=PTHREAD_MUTEX_INITIALIZER;
#define STAGE_BYTES 0x6c000u
#define IMAGE_BYTES 18608u
#define STATE_BASE 0x27a8u
#define STATE_WORDS 2022u
struct dsound_effects_dsp {
    dsp_core_t core;
    DSPDMAState dma;
    uint8_t stage[STAGE_BYTES];
    uint32_t map_start[9], map_bytes[9];
    uint32_t pending_start, pending_bytes;
    unsigned interrupts;
    bool halted;
    const char *error;
};
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void put(uint8_t *p,uint32_t v) {
    for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));
}
static uint64_t fingerprint(const uint8_t *p,size_t n) {
    uint64_t h=UINT64_C(0xcbf29ce484222325);
    for(size_t i=0;i<n;i++)h=(h^p[i])*UINT64_C(0x100000001b3);
    return h;
}
static uint32_t crc(const uint8_t *p,size_t n) {
    uint32_t v=UINT32_MAX;
    for(size_t i=0;i<n;i++) {
        v^=p[i];
        for(unsigned bit=0;bit<8;bit++)v=(v>>1)^((0u-(v&1u))&0xedb88320u);
    }
    return ~v;
}
static uint32_t memread(void *p,int space,uint32_t address) {
    return dsp56k_read_memory(&((dsound_effects_dsp *)p)->core,space,address);
}
static void memwrite(void *p,int space,uint32_t address,uint32_t value) {
    dsp56k_write_memory(&((dsound_effects_dsp *)p)->core,space,address,value);
}
static void scratchio(void *p,uint8_t *bytes,uint32_t address,size_t n,bool write) {
    dsound_effects_dsp *d=p;
    if(address>STAGE_BYTES||n>STAGE_BYTES-address) {
        d->error="DSP DMA scratch range unavailable";
        if(!write)memset(bytes,0,n);
        return;
    }
    if(write)memcpy(d->stage+address,bytes,n);else memcpy(bytes,d->stage+address,n);
}
static void fifoio(void *p,uint8_t *bytes,unsigned index,size_t n,bool write) {
    dsound_effects_dsp *d=p;(void)index;
    d->error=write?"DSP FIFO output not connected":"DSP FIFO input not connected";
    if(!write)memset(bytes,0,n);
}
static uint32_t peripheralread(dsp_core_t *core,uint32_t address) {
    dsound_effects_dsp *d=core->opaque;
    switch(address) {
    case 0xffffb3:return 0;
    case 0xffffc5:return d->interrupts|(d->dma.eol?128u:0u);
    case 0xffffd4:return dsp_dma_read(&d->dma,DMA_NEXT_BLOCK);
    case 0xffffd5:return dsp_dma_read(&d->dma,DMA_START_BLOCK);
    case 0xffffd6:return dsp_dma_read(&d->dma,DMA_CONTROL);
    case 0xffffd7:return dsp_dma_read(&d->dma,DMA_CONFIGURATION);
    default:d->error="DSP peripheral read not modeled";return 0;
    }
}
/* Reject the pinned DMA assertion domains before executing a transfer. This
 * is a bounded private-core guard, not a replacement DMA acknowledgment. */
static bool dma_span(uint32_t address,uint32_t count,int *space,uint32_t *base) {
    uint32_t end;
    if(address<0x1800u) {*space=DSP_SPACE_X;*base=address;end=0x1800u;}
    else if(address<0x2000u) {*space=DSP_SPACE_Y;*base=address-0x1800u;end=0x2000u;}
    else if(address>=0x2800u&&address<0x3800u) {*space=DSP_SPACE_P;*base=address-0x2800u;end=0x3800u;}
    else return false;
    return count<end-address;
}
static bool dma_supported(dsound_effects_dsp *d) {
    uint32_t next=d->dma.next_block;
    for(unsigned node=0u;node<4096u;node++) {
        if(next&NODE_POINTER_EOL)return true;
        int space;uint32_t base;
        if(!dma_span(next&NODE_POINTER_VAL,6u,&space,&base))return false;
        next=memread(d,space,base);
        uint32_t control=memread(d,space,base+1u),count=memread(d,space,base+2u);
        uint32_t address=memread(d,space,base+3u),format=(control>>10u)&7u;
        uint32_t buffer=(control>>5u)&15u;
        if((control&0x200Cu)!=0u||
           (format!=0u&&format!=1u&&format!=2u&&format!=6u)||
           !dma_span(address,count,&space,&base))return false;
        if(buffer!=14u&&buffer!=15u)return false;
        if((control&1u)!=0u&&((control&2u)==0u||format==0u))return false;
    }
    return false;
}
static void peripheralwrite(dsp_core_t *core,uint32_t address,uint32_t value) {
    dsound_effects_dsp *d=core->opaque;
    switch(address) {
    case 0xffffc4:d->halted|=(value&1u)!=0;break;
    case 0xffffc5:d->interrupts&=~value;if(value&128u)d->dma.eol=false;break;
    case 0xffffd4:dsp_dma_write(&d->dma,DMA_NEXT_BLOCK,value);break;
    case 0xffffd5:dsp_dma_write(&d->dma,DMA_START_BLOCK,value);break;
    case 0xffffd6:
        if((value&DMA_CONTROL_ACTION)<DMA_CONTROL_ACTION_START||
           (value&DMA_CONTROL_ACTION)>DMA_CONTROL_ACTION_UNFREEZE) {
            d->error="DSP DMA control action unsupported";break;
        }
        if(((value&DMA_CONTROL_ACTION)==DMA_CONTROL_ACTION_START||
            ((value&DMA_CONTROL_ACTION)==DMA_CONTROL_ACTION_UNFREEZE&&
             (d->dma.control&DMA_CONTROL_RUNNING)))&&!dma_supported(d)) {
            d->error="DSP DMA descriptor outside supported pinned domain";break;
        }
        dsp_dma_write(&d->dma,DMA_CONTROL,value);break;
    case 0xffffd7:dsp_dma_write(&d->dma,DMA_CONFIGURATION,value);break;
    /* Pinned xemu ignores these writes; they do not acknowledge a command. */
    case 0xffffb0:case 0xffffb1:case 0xffffb2:break;
    default:d->error="DSP peripheral write not modeled";break;
    }
}
static bool run(dsound_effects_dsp *d,bool command_only) {
    bool ok=false;
    pthread_mutex_lock(&dma_lock);
    for(unsigned i=0;i<1000000u&&!d->error;i++) {
        if(command_only&&word(d->stage+0x810u)==0u) {ok=true;break;}
        if(d->halted) {
            if(!command_only) {ok=true;break;}
            d->halted=false;d->interrupts|=2u;d->core.is_idle=false;
        }
        dsp56k_execute_instruction(&d->core);
        if(d->dma.error)d->error="DSP DMA rejected transfer";
    }
    if(!ok&&!d->error)d->error="DSP instruction budget exhausted";
    pthread_mutex_unlock(&dma_lock);return ok;
}
void dsound_effects_dsp_destroy(dsound_effects_dsp *d) {free(d);}
static dsound_effects_dsp *create(const uint8_t *firmware,size_t fn,
                                  const uint8_t *image,size_t n,bool finish_frame) {
    if(!firmware||!image||fn!=0x5ccu||n!=IMAGE_BYTES||
       fingerprint(firmware,fn)!=UINT64_C(0x432765cccdec4764)||
       fingerprint(image,n)!=UINT64_C(0x9bbe14297f9af82b)||crc(image,n)!=0xcad22f6fu)
        return NULL;
    dsound_effects_dsp *d=calloc(1,sizeof(*d));if(!d)return NULL;
    memcpy(d->stage,image,n);memcpy(d->stage,firmware,fn);
    uint8_t key[8],ivs[72];dsound_effects_cipher_setup(key);
    if(!dsound_effects_cipher_decode(key,image+0x4868u,ivs,sizeof(ivs)))goto failure;
    for(unsigned i=0;i<9;i++) {
        const uint8_t *m=image+0x4748u+i*32u;
        uint32_t co=word(m),cn=word(m+4u),so=word(m+8u),sn=word(m+12u);
        if(co<0x818u||co>STATE_BASE||cn>STATE_BASE-co||so<STATE_BASE||
           so>STATE_BASE+STATE_WORDS*4u||sn>STATE_BASE+STATE_WORDS*4u-so)goto failure;
        if(!dsound_effects_cipher_decode(ivs+i*8u,image+co,d->stage+co,cn))goto failure;
        d->map_start[i]=so-STATE_BASE;d->map_bytes[i]=sn;
    }
    d->pending_start=0x8000u;
    d->core.is_gp=true;dsp56k_reset_cpu(&d->core);d->core.opaque=d;
    d->core.read_peripheral=peripheralread;d->core.write_peripheral=peripheralwrite;
    d->dma.mem_opaque=d;d->dma.mem_read=memread;d->dma.mem_write=memwrite;
    d->dma.rw_opaque=d;d->dma.scratch_rw=scratchio;d->dma.fifo_rw=fifoio;
    d->interrupts=2u;
    for(unsigned i=0;i<0x800u;i++)
        dsp56k_write_memory(&d->core,DSP_SPACE_P,i,i*4u<fn?word(firmware+i*4u)&0xffffffu:0u);
    if(!run(d,true))goto failure;
    for(unsigned i=0;i<STATE_WORDS;i++)
        if(dsp56k_read_memory(&d->core,DSP_SPACE_X,0x80u+i)!=
           (word(d->stage+STATE_BASE+i*4u)&0xffffffu))goto failure;
    if(finish_frame&&!run(d,false))goto failure;
    return d;
failure:free(d);return NULL;
}
dsound_effects_dsp *dsound_effects_dsp_create(const uint8_t *firmware,size_t fn,
                                             const uint8_t *image,size_t n) {
    return create(firmware,fn,image,n,true);
}
dsound_effects_dsp *dsound_effects_dsp_create_at_download(const uint8_t *firmware,size_t fn,
                                                         const uint8_t *image,size_t n) {
    return create(firmware,fn,image,n,false);
}
static bool span(dsound_effects_dsp *d,unsigned map,size_t offset,size_t n,uint32_t *start) {
    if(!d||d->error||map>=9u||offset>d->map_bytes[map]||n>d->map_bytes[map]-offset)return false;
    *start=d->map_start[map]+(uint32_t)offset;return true;
}
bool dsound_effects_dsp_defer(dsound_effects_dsp *d,unsigned map,size_t offset,size_t n) {
    uint32_t start;
    if(!span(d,map,offset,n,&start))return false;
    start+=STATE_BASE;
    uint32_t add=(uint32_t)n,old=d->pending_start;
    if(old!=0x8000u) {
        uint32_t end=old+d->pending_bytes;
        if(end<start)add+=start-d->pending_bytes-old;
        else if(old>start+add)add=old-start;
    }
    if(!(old<start))old=start;
    d->pending_start=old;
    d->pending_bytes+=add;
    return true;
}
bool dsound_effects_dsp_pending(const dsound_effects_dsp *d,uint32_t *start,uint32_t *bytes) {
    if(!d||d->error||!start||!bytes)return false;
    *start=d->pending_start;*bytes=d->pending_bytes;return true;
}
bool dsound_effects_dsp_stage(dsound_effects_dsp *d,unsigned map,size_t offset,
                              const uint8_t *source,size_t n) {
    uint32_t start;
    if((!source&&n)||!span(d,map,offset,n,&start))return false;
    if(n)memcpy(d->stage+STATE_BASE+start,source,n);
    return true;
}
bool dsound_effects_dsp_live_aligned(dsound_effects_dsp *d,unsigned map,size_t offset,
                                     const uint8_t *source,size_t n) {
    uint32_t start;
    if((!source&&n)||!span(d,map,offset,n,&start)||(start&3u)||(n&3u))return false;
    for(size_t i=0;i<n;i+=4u)if(word(source+i)>0xffffffu)return false;
    for(size_t i=0;i<n;i+=4u)
        dsp56k_write_memory(&d->core,DSP_SPACE_X,0x80u+(start+(uint32_t)i)/4u,word(source+i));
    return true;
}
bool dsound_effects_dsp_write(dsound_effects_dsp *d,unsigned map,size_t offset,
                              const uint8_t *source,size_t n,bool deferred) {
    uint32_t start;if((!source&&n)||!span(d,map,offset,n,&start))return false;
    if(!n)return true;
    memcpy(d->stage+STATE_BASE+start,source,n);
    if(deferred) {
        return dsound_effects_dsp_defer(d,map,offset,n);
    } else {
        for(size_t i=0;i<n;i++) {
            uint32_t pos=start+(uint32_t)i,address=0x80u+pos/4u;
            uint32_t v=dsp56k_read_memory(&d->core,DSP_SPACE_X,address);
            unsigned shift=(pos&3u)*8u;
            v=((v&~(0xffu<<shift))|((uint32_t)source[i]<<shift))&0xffffffu;
            dsp56k_write_memory(&d->core,DSP_SPACE_X,address,v);
        }
    }
    return true;
}
bool dsound_effects_dsp_commit(dsound_effects_dsp *d) {
    if(!d||d->error)return false;
    if(d->pending_start==0x8000u||d->pending_bytes==0u)return true;
    if(d->pending_start<STATE_BASE||d->pending_start>STATE_BASE+STATE_WORDS*4u||
       d->pending_bytes>STATE_BASE+STATE_WORDS*4u-d->pending_start)return false;
    if(word(d->stage+0x810u)!=0u)return false;
    put(d->stage+0x800u,(d->pending_start/4u)-2020u-0x206u);
    put(d->stage+0x808u,d->pending_start);
    put(d->stage+0x80cu,d->pending_bytes/4u);
    put(d->stage+0x810u,2u);
    if(!run(d,true))return false;
    d->pending_start=0x8000u;d->pending_bytes=0u;return true;
}
bool dsound_effects_dsp_frame(dsound_effects_dsp *d,const uint32_t input[1024],
                              uint32_t output[1024]) {
    if(!d||d->error||!input||!output)return false;
    for(unsigned i=0;i<1024u;i++)if(input[i]>0xffffffu)return false;
    if(!d->halted&&!run(d,false))return false;
    memcpy(d->core.mixbuffer,input,sizeof(d->core.mixbuffer));
    if(d->halted){d->halted=false;d->interrupts|=2u;d->core.is_idle=false;}
    if(!run(d,false))return false;
    memcpy(output,d->core.mixbuffer,sizeof(d->core.mixbuffer));return true;
}
bool dsound_effects_dsp_read(dsound_effects_dsp *d,unsigned map,size_t offset,
                             uint8_t *output,size_t n) {
    uint32_t start;if((!output&&n)||!span(d,map,offset,n,&start))return false;
    for(size_t i=0;i<n;i++) {
        uint32_t pos=start+(uint32_t)i;
        output[i]=(uint8_t)(dsp56k_read_memory(&d->core,DSP_SPACE_X,0x80u+pos/4u)>>((pos&3u)*8u));
    }
    return true;
}
const char *dsound_effects_dsp_error(const dsound_effects_dsp *d) {
    return d?d->error:"DSP context absent";
}
