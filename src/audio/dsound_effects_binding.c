/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "dsound_effects_binding.h"
#ifdef TSFP_DSOUND_DSP_ENABLED
#include "dsound_effects_dsp.h"
#endif
#include "dsound_effects_metadata.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "guest_mem.h"
#include "kernel_call.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#define ENTRY 0x004079DBu
#define CALLER 0x000270AFu
#define OOM 0x8007000Eu
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static bool enabled,announced,gp_enabled;
static uint32_t heap,device,device_heap,descriptor,code,state,workspace,y,staging;
#ifdef TSFP_DSOUND_DSP_ENABLED
static dsound_effects_dsp *gp;
static dsound_effects_metadata gp_metadata;
static uint8_t gp_mirror[8088];
#endif
static uint8_t immutable_descriptor[296],immutable_code[8080];
static dsound_effects_binding_fatal_fn fatal_handler;
typedef struct request {
    uint32_t interface,image,bytes,location,output;
    const char *error;
    bool announce;
} request;
static void refuse(const char *message) __attribute__((noreturn));
static void refuse_at(uint32_t entry,const char *message) __attribute__((noreturn));
static void refuse_at(uint32_t entry,const char *message)
{
    dsound_hle_log()("dsound effects: %#x refused: %s\n",entry,message);
    if(fatal_handler!=NULL)fatal_handler(entry,message);
    abort();
}
static void refuse(const char *message) {refuse_at(ENTRY,message);}

static bool copy_read(uint32_t address,void *buffer,size_t bytes)
{
    void *mapped=kernel_guest_at(address,bytes);if(mapped==NULL)return false;
    struct iovec local={buffer,bytes},remote={mapped,bytes};
    return process_vm_readv(kernel_host_pid(),&local,1u,&remote,1u,0u)==(ssize_t)bytes;
}
static bool copy_write(uint32_t address,const void *buffer,size_t bytes)
{
    void *mapped=kernel_guest_at(address,bytes);if(mapped==NULL)return false;
    struct iovec local={(void *)buffer,bytes},remote={mapped,bytes};
    return process_vm_writev(kernel_host_pid(),&local,1u,&remote,1u,0u)==(ssize_t)bytes;
}
static bool overlaps(uint32_t a,uint32_t n,uint32_t b,uint32_t m)
{ return (uint64_t)a+n>b && (uint64_t)b+m>a; }
static void put_word(uint8_t *p,uint32_t value)
{ for(unsigned i=0u;i<4u;i++)p[i]=(uint8_t)(value>>(i*8u)); }
void dsound_effects_binding_set_enabled(bool value)
{ pthread_mutex_lock(&lock);enabled=value;pthread_mutex_unlock(&lock); }
bool dsound_effects_binding_set_gp_enabled(bool value)
{
    pthread_mutex_lock(&lock);
    bool ok=heap==0u;
#ifndef TSFP_DSOUND_DSP_ENABLED
    ok=ok && !value;
#endif
    if(ok)gp_enabled=value;
    pthread_mutex_unlock(&lock);return ok;
}
void dsound_effects_binding_set_fatal(dsound_effects_binding_fatal_fn handler)
{ fatal_handler=handler; }
void dsound_effects_binding_reset(void)
{
    pthread_mutex_lock(&lock);
    #ifdef TSFP_DSOUND_DSP_ENABLED
    dsound_effects_dsp_destroy(gp);gp=NULL;
    memset(&gp_metadata,0,sizeof(gp_metadata));memset(gp_mirror,0,sizeof(gp_mirror));
#endif
    if(heap!=0u && guest_heap_valid(heap))(void)guest_heap_destroy(heap);
    heap=device=device_heap=descriptor=code=state=workspace=y=staging=0u;announced=false;
    pthread_mutex_unlock(&lock);
}
static bool intact(void)
{
    const uint32_t addresses[6]={descriptor,code,state,workspace,y,staging};
    const uint32_t sizes[6]={296u,8080u,8088u,393216u,4u,8088u};
    if(!guest_heap_valid(heap))return false;
    for(unsigned i=0u;i<(gp_enabled?6u:5u);i++) {
        uint32_t requested;
        if(!guest_heap_block_size(heap,addresses[i],&requested)||requested!=sizes[i])return false;
    }
    uint8_t actual[8080];
    return copy_read(descriptor,actual,296u) && memcmp(actual,immutable_descriptor,296u)==0 &&
           copy_read(code,actual,8080u) && memcmp(actual,immutable_code,8080u)==0;
}
static void bind_owned(const dsound_device_identity *identity,void *userdata,uint32_t *result)
{
    const uint32_t internal=identity->internal_address;
    request *r=userdata;pthread_mutex_lock(&lock);
    uint32_t location_words[2],out_before;
    dsound_effects_metadata metadata;
    uint8_t image[DSOUND_EFFECTS_IMAGE_BYTES];
#ifdef TSFP_DSOUND_DSP_ENABLED
    dsound_effects_dsp *prepared=NULL;
#endif
    if(!enabled) {r->error="explicit --headless-effects policy is disabled";goto done;}
    if(heap!=0u) {
        r->error=device!=internal||device_heap!=identity->device_heap||!intact()?"binding ownership or immutable views changed":
                                             "repeat binding is not recovered";goto done;
    }
    if(!copy_read(r->location,location_words,8u)||location_words[0]!=3u||location_words[1]!=4u||
       !copy_read(r->output,&out_before,4u)) {
        r->error="expected readable location {3,4} and output DWORD";goto done;
    }
    if(overlaps(r->output,4u,r->location,8u)||overlaps(r->output,4u,r->image,r->bytes)||
       overlaps(r->output,4u,internal,44u)||overlaps(r->location,8u,internal,44u)||
       overlaps(r->location,8u,r->image,r->bytes)||
       overlaps(r->output,4u,0x412B30u,4u)||overlaps(r->output,4u,0x4A1CB0u,12u)) {
        r->error="argument alias with input/device state is not recovered";goto done;
    }
    if(!dsound_effects_snapshot_guest(r->image,r->bytes,image,&metadata)) {
        r->error="image identity/layout is not recovered";goto done;
    }
    /* Verify write permission before allocation; writing identical bytes preserves
     * the output value. Quiescent mappings are required for compound operations. */
    if(!copy_write(r->output,&out_before,4u)) {r->error="output is not writable";goto done;}
    #ifdef TSFP_DSOUND_DSP_ENABLED
    if(gp_enabled) {
        uint8_t firmware[0x5CCu];
        if(!copy_read(0x4124D0u,firmware,sizeof(firmware))) {
            r->error="GP bootstrap source unreadable";goto done;
        }
        prepared=dsound_effects_dsp_create_at_download(firmware,sizeof(firmware),image,sizeof(image));
        if(prepared==NULL) {r->error="GP attestation/decrypt/real firmware bootstrap failed";goto done;}
    }
#endif
    uint32_t new_heap=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);
    uint32_t addresses[6]={0};const uint32_t sizes[6]={296u,8080u,8088u,393216u,4u,8088u};
    if(new_heap==0u) {*result=OOM;goto done;}
    for(unsigned i=0u;i<(gp_enabled?6u:5u);i++) {
        addresses[i]=guest_heap_alloc(new_heap,sizes[i]);
        if(addresses[i]==0u) {guest_heap_destroy(new_heap);*result=OOM;goto done;}
    }
    uint8_t desc[296]={0};put_word(desc,metadata.map_count);put_word(desc+4u,metadata.workspace_bytes);
    for(unsigned i=0u;i<metadata.map_count;i++) {
        const dsound_effects_map *m=&metadata.maps[i];uint8_t *p=desc+8u+i*32u;
        const uint32_t words[8]={addresses[1]+m->code_offset-metadata.code_offset,m->code_bytes,
            addresses[2]+m->state_offset-metadata.state_offset,m->state_bytes,addresses[4],m->y_bytes,
            addresses[3]+m->workspace_offset,m->workspace_bytes};
        for(unsigned j=0u;j<8u;j++)put_word(p+j*4u,words[j]);
    }
    void *work_at=kernel_guest_at(addresses[3],393216u);
    void *y_at=kernel_guest_at(addresses[4],4u);
    if(work_at==NULL||y_at==NULL) {
        guest_heap_destroy(new_heap);r->error="owned passive views unmapped";goto done;
    }
    memset(work_at,0,393216u);memset(y_at,0,4u);
    if(!copy_write(addresses[0],desc,sizeof(desc))||
       !copy_write(addresses[1],image+metadata.code_offset,8080u)||
       !copy_write(addresses[2],image+metadata.state_offset,8088u)||
       (gp_enabled&&!copy_write(addresses[5],image+metadata.state_offset,8088u))||
       !copy_write(r->output,&addresses[0],4u)) {
        guest_heap_destroy(new_heap);r->error="owned view/output became unwritable";goto done;
    }
    heap=new_heap;device=internal;device_heap=identity->device_heap;staging=addresses[5];descriptor=addresses[0];code=addresses[1];state=addresses[2];
    workspace=addresses[3];y=addresses[4];memcpy(immutable_descriptor,desc,296u);
    memcpy(immutable_code,image+metadata.code_offset,8080u);
    #ifdef TSFP_DSOUND_DSP_ENABLED
    gp=prepared;prepared=NULL;gp_metadata=metadata;
    if(gp_enabled)memcpy(gp_mirror,image+metadata.state_offset,sizeof(gp_mirror));
#endif
    r->announce=!announced;announced=true;*result=0u;
done:
#ifdef TSFP_DSOUND_DSP_ENABLED
    dsound_effects_dsp_destroy(prepared);
#endif
    pthread_mutex_unlock(&lock);
}
uint32_t dsound_effects_binding_bind(uint32_t interface,uint32_t image,uint32_t bytes,
                                    uint32_t location,uint32_t output)
{
    request r={interface,image,bytes,location,output,NULL,false};uint32_t result=0u;
    if(!dsound_device_with_owned_identity(interface,bind_owned,&r,&result)) {
        /* MEASURED default-boot state: the title never tests DirectSoundCreate's HRESULT
         * (lifted 0x000279D5 uses no eax), so a NOT_READY codec leaves the interface
         * global 0x581988 zero and this call arrives with a NULL interface. The ORIGINAL
         * maps it to a NULL device (neg/sbb/and at 0x004079F2..F6) and faults reading
         * [device+8] at 0x00406BCA. This loud stop is that fault's honest stand-in. */
        refuse(interface==0u?
            "NULL interface: DirectSoundCreate's NOT_READY HRESULT was ignored at "
            "0x000279D5; the original maps NULL interface to NULL device (0x004079F2..F6) "
            "and faults reading [device+8] at 0x00406BCA; the default boot models the "
            "always-present console codec (--no-ac97-ready opts out)":
            "SILENT device ownership/header/generation is invalid");
    }
    if(r.error!=NULL)refuse(r.error);
    if(r.announce&&gp_enabled)dsound_hle_log()("dsound: INFERRED opt-in owned CPU-view/GP bridge; attested firmware command3 consumed, no fabricated ACK; no APU/host PCM integration\n");
    if(r.announce&&!gp_enabled)dsound_hle_log()("dsound: explicit HEADLESS effects binding; passive CPU "
        "descriptor/plaintext initial state, opaque encrypted code, zero workspace/Y; "
        "no DSP execution, acknowledgement or nested hardware objects\n");
    return result;
}
typedef struct gp_request {
    uint32_t index,offset,source,bytes,flags;
    unsigned action;
    const uint32_t *input;
    uint32_t *output;
    dsound_effects_binding_gp_view *view;
    const char *error;
    bool valid;
} gp_request;
#ifdef TSFP_DSOUND_DSP_ENABLED
static bool sync_live(gp_request *r)
{
    uint8_t actual[8088];
    if(!copy_read(state,actual,sizeof(actual))) {r->error="GP CPU state view unreadable";return false;}
    for(unsigned map=0;map<gp_metadata.map_count;map++) {
        const dsound_effects_map *m=&gp_metadata.maps[map];
        uint32_t base=m->state_offset-gp_metadata.state_offset;
        for(uint32_t off=0;off<m->state_bytes;off+=4u) {
            if(memcmp(actual+base+off,gp_mirror+base+off,4u)==0)continue;
            if(!dsound_effects_dsp_live_aligned(gp,map,off,actual+base+off,4u)) {
                r->error="GP direct CPU-view write is outside aligned24-bit MMIO domain";return false;
            }
            memcpy(gp_mirror+base+off,actual+base+off,4u);
        }
    }
    return true;
}
static bool publish_live(gp_request *r)
{
    uint8_t actual[8088];memcpy(actual,gp_mirror,sizeof(actual));
    for(unsigned map=0;map<gp_metadata.map_count;map++) {
        const dsound_effects_map *m=&gp_metadata.maps[map];
        uint32_t base=m->state_offset-gp_metadata.state_offset;
        if(!dsound_effects_dsp_read(gp,map,0u,actual+base,m->state_bytes)) {
            r->error="GP live state read failed";return false;
        }
    }
    if(!copy_write(state,actual,sizeof(actual))) {r->error="GP live state mirror unwritable";return false;}
    memcpy(gp_mirror,actual,sizeof(actual));return true;
}
static bool gp_forward_copy(gp_request *r,bool live)
{
    const dsound_effects_map *m=&gp_metadata.maps[r->index];
    uint32_t base=m->state_offset-gp_metadata.state_offset;
    for(uint32_t done=0u;done<r->bytes;) {
        uint32_t width=done<r->bytes-(r->bytes&3u)?4u:1u;
        uint8_t value[4];
        if(!copy_read(r->source+done,value,width)) {
            r->error="GP source read fault after original copy prefix";return false;
        }
        uint32_t target=(live?state:staging)+base+r->offset+done;
        if(live) {
            if(!dsound_effects_dsp_live_aligned(gp,r->index,r->offset+done,value,width)) {
                r->error="GP immediate data outside aligned24-bit MMIO domain";return false;
            }
            if(!copy_write(target,value,width)) {
                r->error="GP CPU live mirror write fault";return false;
            }
            memcpy(gp_mirror+base+r->offset+done,value,width);
        } else {
            if(!copy_write(target,value,width)) {
                r->error="GP staging write fault after original copy prefix";return false;
            }
            if(!dsound_effects_dsp_stage(gp,r->index,r->offset+done,value,width)) {
                r->error="GP real staging range unavailable";return false;
            }
        }
        done+=width;
    }
    return true;
}
#endif
static void gp_owned(const dsound_device_identity *identity,void *userdata,uint32_t *result)
{
    gp_request *r=userdata;pthread_mutex_lock(&lock);
#ifdef TSFP_DSOUND_DSP_ENABLED
    if(!enabled||!gp_enabled||gp==NULL) {r->error="opt-in genuine GP binding is not active";goto done;}
    if(device!=identity->internal_address||device_heap!=identity->device_heap||!intact()) {
        r->error="GP device incarnation or binding ownership changed";goto done;
    }
    if(r->action==3u) {
        dsound_effects_binding_gp_view value={.heap=heap,.device_heap=device_heap,.internal=device,
            .descriptor=descriptor,.state=state,.staging=staging};
        if(!dsound_effects_dsp_pending(gp,&value.pending_start,&value.pending_bytes))goto done;
        *r->view=value;r->valid=true;goto done;
    }
    if(r->action==0u) {
        if(r->index>=gp_metadata.map_count) {*result=0x88780032u;r->valid=true;goto done;}
        const dsound_effects_map *m=&gp_metadata.maps[r->index];
        if(r->offset>m->state_bytes||r->bytes>m->state_bytes-r->offset||
           (uint64_t)r->source+r->bytes>UINT64_C(0x100000000)) {
            r->error="GP effect span outside verified backing domain";goto done;
        }
        if(r->bytes!=0u&&!sync_live(r))goto done;
        if(r->bytes!=0u&&(r->flags&1u)==0u&&
           !copy_write(state,gp_mirror,sizeof(gp_mirror))) {
            r->error="GP immediate CPU state view unwritable";goto done;
        }
        if(!gp_forward_copy(r,false))goto done;
        if((r->flags&1u)==0u&&r->bytes!=0u&&((r->offset&3u)||(r->bytes&3u))) {
            r->error="GP immediate MMIO requires aligned four-byte accesses";goto done;
        }
        if(r->flags&1u) {
            if(!dsound_effects_dsp_defer(gp,r->index,r->offset,r->bytes)) {
                r->error="GP deferred range is not supported";goto done;
            }
        } else if(!gp_forward_copy(r,true))goto done;
        *result=0u;r->valid=true;
    } else {
        if(!sync_live(r))goto done;
        uint8_t probe[8088];
        if(!copy_read(state,probe,sizeof(probe))||!copy_write(state,probe,sizeof(probe))) {
            r->error="GP consumer CPU state mirror unwritable";goto done;
        }
        uint32_t output[1024];
        bool ok=r->action==1u?dsound_effects_dsp_commit(gp):
            dsound_effects_dsp_frame(gp,r->input,output);
        if(!ok) {r->error=dsound_effects_dsp_error(gp);if(!r->error)r->error="GP consumer span/operand refused";goto done;}
        if(!publish_live(r))goto done;
        if(r->action==2u)memcpy(r->output,output,sizeof(output));
        r->valid=true;
    }
done:
#else
    (void)identity;(void)result;
    r->error="genuine GP support was not built";
#endif
    pthread_mutex_unlock(&lock);
}
uint32_t dsound_effects_binding_apply(uint32_t interface,uint32_t index,uint32_t offset,
                                     uint32_t source,uint32_t bytes,uint32_t flags)
{
    uint32_t blocked;
    if(!copy_read(0x4124A8u,&blocked,4u))refuse_at(0x407A02u,"original audio global unreadable");
    if(blocked!=0u)return 0x80004005u;
    gp_request r={.index=index,.offset=offset,.source=source,.bytes=bytes,.flags=flags};
    uint32_t result=0u;
    if(!dsound_device_with_owned_identity(interface,gp_owned,&r,&result))
        refuse_at(0x407A02u,"owned GP device interface invalid");
    if(r.error)refuse_at(0x407A02u,r.error);
    return result;
}
bool dsound_effects_binding_gp_view_get(uint32_t interface,dsound_effects_binding_gp_view *out)
{
    if(!out)return false;
    gp_request r={.action=3u,.view=out};uint32_t result=0u;
    return dsound_device_with_owned_identity(interface,gp_owned,&r,&result)&&r.valid;
}
bool dsound_effects_binding_gp_commit(uint32_t interface)
{
    gp_request r={.action=1u};uint32_t result=0u;
    return dsound_device_with_owned_identity(interface,gp_owned,&r,&result)&&r.valid;
}
bool dsound_effects_binding_gp_frame(uint32_t interface,const uint32_t input[1024],uint32_t output[1024])
{
    if(!input||!output)return false;
    gp_request r={.action=2u,.input=input,.output=output};uint32_t result=0u;
    return dsound_device_with_owned_identity(interface,gp_owned,&r,&result)&&r.valid;
}
static uint32_t apply_handler(void *context)
{
    const kernel_call_frame *frame=context;uint32_t caller,args[6];
    if(frame==NULL||!kernel_guest_read_u32(frame->stack_ptr,&caller)||caller!=0x38669Fu)
        refuse_at(0x407A02u,"only measured gain caller return38669F is supported");
    for(unsigned i=0u;i<6u;i++)if(!kernel_frame_arg(frame,i,&args[i]))
        refuse_at(0x407A02u,"unreadable SetEffectData argument");
    /* The recovered caller emits only four gain bytes at +20, immediate. Its
     * supported firmware maps are the four measured effect programs5..8. */
    if(args[1]<5u||args[1]>8u||args[2]!=0x20u||args[4]!=4u||args[5]!=0u)
        refuse_at(0x407A02u,"gain caller operands outside measured GP effect domain");
    return dsound_effects_binding_apply(args[0],args[1],args[2],args[3],args[4],args[5]);
}
static uint32_t handler(void *context)
{
    const kernel_call_frame *frame=context;uint32_t caller,args[5];
    if(frame==NULL||!kernel_guest_read_u32(frame->stack_ptr,&caller)||caller!=CALLER)
        refuse("only measured startup caller return 0x000270AF is recovered");
    for(unsigned i=0u;i<5u;i++)if(!kernel_frame_arg(frame,i,&args[i]))refuse("unreadable argument");
    return dsound_effects_binding_bind(args[0],args[1],args[2],args[3],args[4]);
}
size_t dsound_effects_binding_register(void)
{
    size_t registered=dsound_hle_register(ENTRY,handler)?1u:0u;
    if(gp_enabled&&dsound_hle_register(0x407A02u,apply_handler))registered++;
    return registered;
}
