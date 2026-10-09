/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_buffer.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsound_hle.h"
#include "guest_mem.h"
#include "kernel_call.h"
#define CREATE 0x004093C8u
#define I3DL2 0x004085AFu
#define MINIMUM 0x00408532u
#define ROLLOFF 0x0040858Bu
#define SET_DATA 0x00408C0Du
#define SET_LOOP_REGION 0x00407AD8u
#define SET_DATA_CALLER 0x000282C0u
#define SET_VOLUME 0x00407A64u
#define SET_VOLUME_CALLER 0x00028348u
#define VOLUME_STARTUP 0xFFFFD8F0u
/* T605: the sound update sub_00028610 sets the volume sub_00028180 computes (-3204) before the first Play. */
#define SET_VOLUME_UPDATE_CALLER 0x00028643u
#define VOLUME_UPDATE 0xFFFFF37Cu
#define PAUSE 0x00407ABCu
#define PAUSE_CALLER 0x0002751Eu
#define SET_FREQUENCY 0x004084F2u
#define SET_FREQUENCY_CALLER 0x00027547u
#define FREQUENCY_STARTUP 22042u
#define MAX_DISTANCE 0x0040850Eu
#define MAX_DISTANCE_CALLER 0x000280B5u
/* T1173: the runtime 3D sound start (sub_00028240) re-applies min distance and the rolloff curve with apply=1. */
#define MINIMUM_START_CALLER 0x00028390u
#define ROLLOFF_START_CALLER 0x000283AEu
#define SET_POSITION 0x00408556u
#define POSITION_CALLER_1 0x00026EF6u
#define POSITION_CALLER_2 0x00027C79u
#define POSITION_CALLER_3 0x00027D28u
#define FREQUENCY_MIN 0xBCu
#define FREQUENCY_MAX 0x2EDEFu
#define DATA_MAX_BYTES 0x04000000u
#define SINGLETON 0x00412B30u
#define GLOBAL_STATE 0x004124A8u
#define VTABLE 0x004A1CE0u
#define CURVE 0x0064BC98u
#define BUFFER_BYTES 36u
#define OOM 0x8007000Eu
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t reset_lock=PTHREAD_MUTEX_INITIALIZER;
static bool completion;
static dsound_buffer_started_fn started_query;
static dsound_buffer_pause_fn pause_hook;
static dsound_buffer_volume_fn volume_hook;
static dsound_buffer_frequency_fn frequency_hook;
static dsound_buffer_voice_running_fn voice_running_hook;
static dsound_buffer_loop_fn loop_hook;
static bool enabled,announced,data_announced,volume_announced,pause_announced,frequency_announced,loop_announced,max_distance_announced,position_announced,apply_announced;
static dsound_buffer_irql_provider irql_provider;
static dsound_buffer_fatal_fn fatal_handler;
typedef struct buffer_node {
    dsound_buffer_snapshot value;
    struct buffer_node *next;
} buffer_node;
static buffer_node *buffers,*detached,*rollback;
static bool overlap(uint32_t a,uint32_t an,uint32_t b,uint32_t bn)
{return (uint64_t)a<(uint64_t)b+bn && (uint64_t)b<(uint64_t)a+an;}
static void refuse(uint32_t entry,const char *reason) __attribute__((noreturn));
static void refuse(uint32_t entry,const char *reason)
{
    pthread_mutex_lock(&lock);dsound_buffer_fatal_fn fatal=fatal_handler;pthread_mutex_unlock(&lock);
    dsound_hle_log()("dsound passive buffer %#x refused: %s\n",entry,reason);
    if(fatal!=NULL)fatal(entry,reason);abort();
}
void dsound_buffer_set_enabled(bool value)
{pthread_mutex_lock(&lock);enabled=value;pthread_mutex_unlock(&lock);}
void dsound_buffer_set_completion(bool value,dsound_buffer_started_fn started)
{pthread_mutex_lock(&lock);completion=value;started_query=value?started:NULL;if(!value){pause_hook=NULL;frequency_hook=NULL;volume_hook=NULL;voice_running_hook=NULL;loop_hook=NULL;}pthread_mutex_unlock(&lock);}
void dsound_buffer_set_completion_voice_running(dsound_buffer_voice_running_fn running)
{pthread_mutex_lock(&lock);voice_running_hook=completion?running:NULL;pthread_mutex_unlock(&lock);}
void dsound_buffer_set_completion_loop(dsound_buffer_loop_fn loop)
{pthread_mutex_lock(&lock);loop_hook=completion?loop:NULL;pthread_mutex_unlock(&lock);}
void dsound_buffer_set_completion_frequency(dsound_buffer_frequency_fn frequency)
{pthread_mutex_lock(&lock);frequency_hook=completion?frequency:NULL;pthread_mutex_unlock(&lock);}
void dsound_buffer_set_completion_pause(dsound_buffer_pause_fn pause)
{pthread_mutex_lock(&lock);pause_hook=completion?pause:NULL;pthread_mutex_unlock(&lock);}
void dsound_buffer_set_completion_volume(dsound_buffer_volume_fn volume)
{pthread_mutex_lock(&lock);volume_hook=volume;pthread_mutex_unlock(&lock);}
void dsound_buffer_set_irql_provider(dsound_buffer_irql_provider provider)
{pthread_mutex_lock(&lock);irql_provider=provider;pthread_mutex_unlock(&lock);}
void dsound_buffer_set_fatal(dsound_buffer_fatal_fn fatal)
{pthread_mutex_lock(&lock);fatal_handler=fatal;pthread_mutex_unlock(&lock);}
/* Called with buffer lock held. Provider must only report current IRQL, never
 * reenter this adapter. Kernel TLS observation does not enter the device lock. */
static const char *policy_error(void)
{
    uint32_t state;uint8_t irql;
    if(!enabled)return "explicit headless-buffers policy is disabled";
    if(irql_provider==NULL || !irql_provider(&irql) || irql!=0u)return "only known IRQL0 is supported";
    if(!kernel_guest_read_u32(GLOBAL_STATE,&state) || state!=0u)return "original global audio state must be zero";
    return NULL;
}
static buffer_node *find(uint32_t address)
{for(buffer_node *n=buffers;n!=NULL;n=n->next)if(n->value.buffer_address==address)return n;return NULL;}
static bool token_equal(const dsound_device_lease *a,const dsound_device_lease *b)
{return a->device_heap==b->device_heap && a->internal_address==b->internal_address && a->serial==b->serial;}
static bool valid_node(const buffer_node *node,const dsound_device_lease *identity,uint32_t internal)
{
    uint32_t actual[9],bytes;
    return node!=NULL && token_equal(&node->value.lease,identity) && identity->internal_address==internal &&
        guest_heap_valid(identity->device_heap) &&
        kernel_guest_read_bytes(node->value.header_address,actual,sizeof(actual)) &&
        memcmp(actual,node->value.header,sizeof(actual))==0 && guest_heap_valid(node->value.buffer_heap) &&
        guest_heap_block_size(node->value.buffer_heap,node->value.header_address,&bytes) && bytes==BUFFER_BYTES;
}
static bool aliases_state(uint32_t address,uint32_t bytes,uint32_t internal)
{
    if(overlap(address,bytes,internal,44u) || overlap(address,bytes,SINGLETON,4u) ||
       overlap(address,bytes,GLOBAL_STATE,4u) || overlap(address,bytes,VTABLE,16u) ||
       overlap(address,bytes,CURVE,4u))return true;
    for(buffer_node *n=buffers;n!=NULL;n=n->next)
        if(overlap(address,bytes,n->value.header_address,BUFFER_BYTES) ||
           overlap(address,bytes,n->value.publication_address,4u))return true;
    for(buffer_node *n=detached;n!=NULL;n=n->next)
        if(overlap(address,bytes,n->value.header_address,BUFFER_BYTES))return true;
    for(buffer_node *n=rollback;n!=NULL;n=n->next)
        if(n->value.header_address!=0u && overlap(address,bytes,n->value.header_address,BUFFER_BYTES))return true;
    return false;
}
/* Input buffers are historical stack snapshots, not guest-owned state. The
 * original setter reuses/overlaps these locals after Create has consumed them. */
static bool aliases_headers(uint32_t address,uint32_t bytes,uint32_t internal)
{
    if(overlap(address,bytes,internal,44u) || overlap(address,bytes,SINGLETON,4u) ||
       overlap(address,bytes,GLOBAL_STATE,4u) || overlap(address,bytes,VTABLE,16u))return true;
    for(buffer_node *n=buffers;n!=NULL;n=n->next)
        if(overlap(address,bytes,n->value.header_address,BUFFER_BYTES) ||
           overlap(address,bytes,n->value.publication_address,4u))return true;
    for(buffer_node *n=detached;n!=NULL;n=n->next)
        if(overlap(address,bytes,n->value.header_address,BUFFER_BYTES))return true;
    for(buffer_node *n=rollback;n!=NULL;n=n->next)
        if(n->value.header_address!=0u && overlap(address,bytes,n->value.header_address,BUFFER_BYTES))return true;
    return false;
}
/* Unordered safety facts, NOT a copy of original ordered vtable bytes. Every
 * member has a verified unconditional independent boundary in the embedding host. */
extern int recomp_has_stop_boundary(uint32_t address) __attribute__((weak));
static const uint32_t stopped[4]={0x00406879u,0x00406FA9u,0x00406FF0u,0x00408040u};
bool dsound_buffer_stops_ready(void)
{
    if(recomp_has_stop_boundary==NULL)return false;
    for(unsigned i=0u;i<4u;i++)if(recomp_has_stop_boundary(stopped[i])!=1)return false;
    return true;
}
static bool safe_tables(const uint32_t table[4])
{
    for(unsigned i=0u;i<4u;i++) {
        bool found=false;for(unsigned j=0u;j<4u;j++)if(table[i]==stopped[j])found=true;
        if(!found)return false;
    }
    return true;
}
static bool output_scope(uint32_t flags,uint32_t output)
{
    uint32_t base=flags==16u?0x005835F0u:0x005818E8u;
    return output>=base && output-base<160u && (output-base)%4u==0u;
}
typedef struct create_request {
    dsound_buffer_scope scope;
    uint32_t output,status;
    void *output_at;
    buffer_node *node;
    const char *error;
    bool held,announce;
} create_request;
static bool create_prepare(const dsound_device_lease *candidate,void *userdata,
                           dsound_device_lease_child *child)
{
    create_request *r=userdata;pthread_mutex_lock(&lock);r->held=true;
    r->error=policy_error();if(r->error!=NULL)return false;
    if(!dsound_buffer_stops_ready()){r->error="compiled buffer stop boundaries missing";return false;}
    if(aliases_headers(r->scope.descriptor_address,24u,candidate->internal_address) ||
       aliases_headers(r->scope.format_address,20u,candidate->internal_address)) {r->error="input aliases owned state";return false;}
    uint32_t old,table[4];
    if(!kernel_guest_read_bytes(VTABLE,table,sizeof(table)) ||
       !kernel_guest_read_u32(r->output,&old)) {r->error="output/original vtables unreadable";return false;}
    if(!safe_tables(table)){r->error="original vtable contains an unprotected target";return false;}
    if(aliases_state(r->output,4u,candidate->internal_address) ||
       overlap(r->output,4u,r->scope.descriptor_address,24u) ||
       overlap(r->output,4u,r->scope.format_address,20u) ||
       overlap(r->scope.descriptor_address,24u,candidate->internal_address,44u) ||
       overlap(r->scope.format_address,20u,candidate->internal_address,44u)) {
        r->error="unproven output/input alias with owned state";return false;
    }
    /* Same-byte probe preserves contents, but is explicitly a guest write. */
    if(!kernel_guest_write_u32(r->output,old) ||
       (r->output_at=kernel_guest_at(r->output,4u))==NULL) {
        r->error="output is not writable";return false;
    }
    r->node=calloc(1u,sizeof(*r->node));if(r->node==NULL){r->status=OOM;return false;}
    r->node->value.scope=r->scope;r->node->value.publication_address=r->output;
    uint32_t heap=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);r->node->value.buffer_heap=heap;
    if(heap==0u){r->status=OOM;return false;}
    uint32_t address=guest_heap_alloc(heap,BUFFER_BYTES);r->node->value.header_address=address;r->node->value.buffer_address=address+28u;
    if(address==0u){r->status=OOM;return false;}
    uint32_t *header=r->node->value.header;
    header[0]=VTABLE;header[1]=1u;header[2]=candidate->internal_address;
    if(aliases_state(address,BUFFER_BYTES,candidate->internal_address) ||
       overlap(address,BUFFER_BYTES,r->output,4u) ||
       overlap(address,BUFFER_BYTES,r->scope.descriptor_address,24u) ||
       overlap(address,BUFFER_BYTES,r->scope.format_address,20u) ||
       !kernel_guest_write_bytes(address,header,BUFFER_BYTES)) {
        r->error="new private buffer allocation/alias is invalid";return false;
    }
    *child=(dsound_device_lease_child){heap,address,BUFFER_BYTES};return true;
}
static void create_abort(void *userdata)
{
    create_request *r=userdata;
    /* Cleanup is fallible and happens after the device transaction unlocks. */
    r->held=false;pthread_mutex_unlock(&lock);
}
static void create_finalize(const dsound_device_lease *committed,void *userdata)
{
    create_request *r=userdata;
    /* acquire_lease copies its caller output token AFTER this callback. Store
     * the COMMITTED token here before making the sidecar or guest pointer visible. */
    r->node->value.lease=*committed;r->node->next=buffers;buffers=r->node;
    r->announce=!announced;announced=true;
    memcpy(r->output_at,&r->node->value.buffer_address,4u);
    r->held=false;pthread_mutex_unlock(&lock);
}
static const dsound_device_lease_ops create_ops={create_prepare,create_abort,create_finalize};
static uint32_t create_from_scope(uint32_t interface,const dsound_buffer_scope *scope,uint32_t output,uint32_t outer)
{
    create_request r={0};r.output=output;r.scope=*scope;
    pthread_mutex_lock(&lock);const char *error=policy_error();pthread_mutex_unlock(&lock);
    if(error!=NULL)refuse(CREATE,error);
    if(outer!=0u)refuse(CREATE,"aggregation is unsupported");
    uint32_t device;if(!kernel_guest_read_u32(SINGLETON,&device) || device==0u || device>UINT32_MAX-8u)
        refuse(CREATE,"no owned SILENT device");
    if(interface!=device+8u || !output_scope(r.scope.flags,output))
        refuse(CREATE,"device/output outside measured startup scope");
    dsound_device_lease lease;
    dsound_device_lease_status status=dsound_device_acquire_lease(device+8u,&create_ops,&r,&lease);
    if(status!=DSOUND_LEASE_OK && r.node!=NULL) {
        pthread_mutex_lock(&lock);
        uint32_t heap=r.node->value.buffer_heap;
        if(heap!=0u && !guest_heap_destroy(heap)) {
            r.node->next=rollback;rollback=r.node;r.error="private allocation rollback needs retry";
        } else free(r.node);
        r.node=NULL;pthread_mutex_unlock(&lock);
    }
    if(status==DSOUND_LEASE_OUT_OF_MEMORY || (status==DSOUND_LEASE_PREPARE_FAILED && r.status==OOM && r.error==NULL))return OOM;
    if(status!=DSOUND_LEASE_OK)refuse(CREATE,r.error!=NULL?r.error:"device lease transaction refused");
    if(r.announce)dsound_hle_log()("dsound: explicit HEADLESS buffers; passive36-byte public headers and "
        "host startup setter caches; omitted settings/hardware/list/packet objects, APU/DSP/FP/critical-section "
        "operations; no audio output or packet completion\n");
    return 0u;
}
uint32_t dsound_buffer_create(uint32_t interface,uint32_t descriptor,uint32_t output,uint32_t outer)
{
    dsound_buffer_scope scope;
    if(!dsound_buffer_scope_snapshot(descriptor,&scope))refuse(CREATE,"descriptor/format outside measured startup scope");
    return create_from_scope(interface,&scope,output,outer);
}
typedef struct access_request {
    uint32_t address,entry,argument,apply,count;
    uint32_t position_z;
    dsound_device_lease identity;
    dsound_buffer_snapshot *snapshot;
    const char *error;
    bool observer,announce;
} access_request;
static bool identify(uint32_t address,dsound_device_lease *identity)
{
    pthread_mutex_lock(&lock);buffer_node *n=find(address);if(n!=NULL)*identity=n->value.lease;
    pthread_mutex_unlock(&lock);return n!=NULL;
}
static void access_owned(uint32_t internal,void *userdata,uint32_t *result)
{
    access_request *r=userdata;pthread_mutex_lock(&lock);buffer_node *n=find(r->address);
    if(!valid_node(n,&r->identity,internal)){r->error="buffer ownership/header/generation changed";goto done;}
    if(r->observer){*r->snapshot=n->value;*result=0u;goto done;}
    r->error=policy_error();if(r->error!=NULL)goto done;
    if(r->entry==SET_LOOP_REGION) {
        /* T1209: the original's SetLoopRegion (0x407AD8 to 0x40717C) writes only settings+0xCC/+0xD0 and never reads the 3D state, so a
         * spatial startup buffer (flags 0x10, all three caches) answers as the ordinary one (tests/test_t1209_spatial_loop_region.py).
         * The Story control run stopped on the 0/0 reset at 0x275BB of such a buffer. */
        const bool shape_ok=n->value.scope.flags==0u || (n->value.scope.flags==0x10u && n->value.cache_mask==7u);
        if(!completion || !shape_ok || n->value.data_sets==0u) {
            /* named so a stop says which of the three conditions failed */
            r->error=!completion?"loop regions need passive completion and ordinary buffer data (passive completion is off)":
                     !shape_ok?"loop regions need passive completion and ordinary buffer data (a flags 0x10 buffer without its three 3D caches, or another flag)":
                     "loop regions need passive completion and ordinary buffer data (SetBufferData has not been recorded)";
            goto done;
        }
        /* T1209: the original's tail call 0x40E711 touches the voice only while the voice flags (+0x12 of the voice object) read 3
         * (playing). After Play plus Stop, or for a voice that finished, it makes no hardware write and enters/leaves only the critical
         * section, exactly like an unplayed buffer (tests/test_t1209_played_loop_region.py), so the title's reuse of a played buffer
         * (SetBufferData, SetVolume, Pause, SetFrequency, SetLoopRegion 0/0) is the unplayed record. A voice still running is the
         * APU reprogram of a live loop, not modelled. */
        const bool running=started_query==NULL || (started_query(r->address,n->value.lease.serial) &&
           (voice_running_hook==NULL || voice_running_hook(r->address,n->value.lease.serial)));
        if(running && loop_hook==NULL) {
            r->error="loop region changes while the voice is running (after Play, before Stop or the end of its data) are not supported";goto done;
        }
        const uint64_t end=(uint64_t)r->argument+r->count;
        if(end>UINT32_MAX || r->argument>n->value.data_length) {
            r->error="wrapped or out-of-range loop start is not supported";goto done;
        }
        if(r->count!=0u && end>n->value.data_length) {*result=0x88780032u;goto done;}
        const uint32_t length=r->count!=0u?r->count:n->value.data_length-r->argument;
        if(length==0u || r->argument%36u!=0u || length%36u!=0u) {
            r->error="only nonempty 36-byte ADPCM block loop regions are supported";goto done;
        }
        /* T1524: a running voice takes the new region from now on (the completion model moves its cursor and the mixer voice).
         * The original reprograms the APU voice's loop words, the cursor so far is the old region's. A refusal leaves the record alone. */
        if(running && !loop_hook(r->address,n->value.lease.serial,r->argument,length,n->value.frequency)) {
            r->error="the completion model refused the live loop region of the running voice (empty at the clock resolution, or the mixer refused it)";goto done;
        }
        n->value.loop_start=r->argument;n->value.loop_length=length;n->value.loop_sets++;
        r->announce=!loop_announced;loop_announced=true;
        *result=0u;goto done;
    }
    if(r->entry==SET_VOLUME) {
        /* A recorded SetBufferData already proves a measured startup scope (it admits only flags 0x10 with all
         * three caches or flags 0 with none, and caches never go away), so the order check is the scope check. */
        if(n->value.data_sets==0u){r->error="SetVolume before SetBufferData is not the measured sound start order";goto done;}
        /* T733: with the completion model on the call site is not part of the state check. The original answers every volume the same
         * S_OK way and never reads its caller (tests/test_dsound_buffer_set_volume_oracle.py), so the host record takes the value of
         * the sound start (T722, measured 0 for the second sound), of the sound update (T681, -3204 and every other one) and of
         * the stream update alike, after the recorded SetBufferData, which already proves a measured startup scope. */
        if(!completion){
            if(r->argument!=VOLUME_STARTUP && r->argument!=VOLUME_UPDATE){r->error="only the measured volumes -10000 (start) and -3204 (update) are supported";goto done;}
            /* The update volume is the sound update's, after the whole start (SetBufferData, SetVolume, Pause, SetFrequency) of
             * an ordinary buffer. The original answers every volume the same way, the order and scope are policy. */
            if(r->argument!=VOLUME_STARTUP && (n->value.frequency_sets==0u || n->value.scope.flags!=0u)){
                r->error="the update volume -3204 needs a started ordinary buffer (SetBufferData, SetVolume, Pause and SetFrequency recorded)";goto done;
            }
        }
        /* HOST record only. The original stores volume minus its +0x20 word in the omitted settings
         * object (+0x1C) and, for an allocated voice, programs APU voice registers 0xFE8202F8..0xFE820368. */
        if(completion && volume_hook!=NULL &&
           !volume_hook(r->address,n->value.lease.serial,(int32_t)r->argument)) {
            r->error="real buffer PCM rejected volume or its timestamp/history";goto done;
        }
        n->value.volume=(int32_t)r->argument;n->value.volume_sets++;
        r->announce=!volume_announced;volume_announced=true;
        *result=0u;goto done;
    }
    if(r->entry==PAUSE) {
        /* A recorded SetVolume implies the recorded SetBufferData and the measured startup scope, see SetVolume. */
        if(n->value.volume_sets==0u){r->error="Pause before SetBufferData and SetVolume is not the measured sound start order";goto done;}
        /* T722: a PLAYED buffer's Pause (0 resume, 1 and 2 pause) is the completion model's, measured under Unicorn. */
        if(started_query!=NULL && started_query(r->address,n->value.lease.serial)){
            if(pause_hook==NULL || !pause_hook(r->address,n->value.lease.serial,r->argument)){
                r->error="only Pause(0) of a played buffer, and Pause(1 or 2) of a playing buffer, are modelled";goto done;
            }
            n->value.pause_sets++;
            r->announce=!pause_announced;pause_announced=true;
            *result=0u;goto done;
        }
        if(r->argument!=0u){r->error="only Pause(0) (resume) on a never started buffer is supported";goto done;}
        /* HOST count only. A buffer no Play ever started has voice state bits 0x101 (low two bits not 3), so the
         * original changes nothing (no guest or APU write) and returns the argument, 0. Only the critical section
         * and IRQL calls are omitted. A future Play admission must refuse Pause on a started buffer. */
        n->value.pause_sets++;
        r->announce=!pause_announced;pause_announced=true;
        *result=0u;goto done;
    }
    if(r->entry==SET_FREQUENCY) {
        /* A recorded Pause implies the whole earlier order and the measured startup scope, see SetVolume. */
        if(n->value.pause_sets==0u){r->error="SetFrequency before SetBufferData, SetVolume and Pause is not the measured sound start order";goto done;}
        if(r->argument!=FREQUENCY_STARTUP && !(completion && r->argument>=FREQUENCY_MIN && r->argument<=FREQUENCY_MAX)){
            r->error="only the measured startup frequency 22042 is supported (the completion model takes the title's clamp 0xBC..0x2EDEF)";goto done;
        }
        /* T722: measured under Unicorn, SetFrequency of a played buffer is S_OK, leaves the status and writes only the voice
         * pitch (APU 0xFE82037C), so the host record changes and the completion model rescales the time left. */
        if(started_query!=NULL && started_query(r->address,n->value.lease.serial) &&
           (frequency_hook==NULL || !frequency_hook(r->address,n->value.lease.serial,n->value.frequency,r->argument))){
            r->error="SetFrequency on a played buffer the completion model does not know is refused";goto done;
        }
        /* HOST record only. The original stores the pitch derived from the frequency at +0x18 of the omitted settings
         * object and writes the voice's APU pitch register 0xFE82037C (voice index in 0xFE8202F8). */
        n->value.frequency=r->argument;n->value.frequency_sets++;
        r->announce=!frequency_announced;frequency_announced=true;
        *result=0u;goto done;
    }
    if(r->entry==SET_DATA) {
        bool spatial=n->value.scope.flags==0x10u && n->value.cache_mask==7u;
        bool ordinary=n->value.scope.flags==0u && n->value.cache_mask==0u;
        if(!spatial && !ordinary){r->error="only fully configured spatial or fresh ordinary startup buffers accept SetBufferData";goto done;}
        if(r->argument==0u || r->count==0u){r->error="only non-null data with non-zero length is supported (the original allocates or clears otherwise)";goto done;}
        if(r->count>DATA_MAX_BYTES || kernel_guest_at(r->argument,r->count)==NULL){r->error="buffer data range is longer than 64 MiB or not readable guest memory";goto done;}
        if(aliases_state(r->argument,r->count,internal)){r->error="buffer data aliases owned state";goto done;}
        /* HOST record only. The original stores pointer and length in the omitted settings object
         * (+0xBC, +0xC0), sets its bit31, locks the pages and programs APU 0xFE820804/808. */
        /* Original resets play/loop regions only when the data pair changes. */
        if(n->value.data_address!=r->argument || n->value.data_length!=r->count) {
            n->value.loop_start=0u;n->value.loop_length=r->count;
        }
        n->value.data_address=r->argument;n->value.data_length=r->count;n->value.data_sets++;
        r->announce=!data_announced;data_announced=true;
        *result=0u;goto done;
    }
    if(r->entry==MAX_DISTANCE) {
        /* The retail title's sole caller passes apply=1. The original updates
         * the nested settings word and marks it dirty, but skips commit/APU work.
         * Requiring the recorded spatial setup and data lease keeps this host-only
         * value on buffers this adapter can identify; it is not spatial playback. */
        if(n->value.scope.flags!=0x10u || n->value.cache_mask!=7u ||
           n->value.data_sets==0u || r->apply!=1u) {
            r->error="only the measured apply=1 update on a configured spatial buffer with recorded data is supported";
            goto done;
        }
        n->value.max_distance_bits=r->argument;n->value.max_distance_sets++;
        r->announce=!max_distance_announced;max_distance_announced=true;
        *result=0u;goto done;
    }
    if(r->entry==SET_POSITION) {
        /* The title's three direct call sites pass apply=1. The original writes
         * only the omitted settings object and dirty bit in that mode; apply=0
         * enters 0x406E90 and the APU commit chain. Keep only the raw host record. */
        if(n->value.scope.flags!=0x10u || n->value.cache_mask!=7u ||
           n->value.data_sets==0u) {
            r->error="only a configured spatial buffer with recorded data is supported";
            goto done;
        }
        if(r->apply!=1u) {
            r->error="SetPosition apply=0 enters the unmodeled settings/APU commit";
            goto done;
        }
        n->value.position_bits[0]=r->argument;
        n->value.position_bits[1]=r->count;
        n->value.position_bits[2]=r->position_z;
        n->value.position_sets++;
        r->announce=!position_announced;position_announced=true;
        *result=0u;goto done;
    }
    if((r->entry==MINIMUM || r->entry==ROLLOFF) && r->apply==1u) {
        /* T1173: apply=1 is the sound start's re-apply on a pooled spatial buffer. The original (0x407E72 and 0x407EF7, the same shape as
         * SetMaxDistance 0x407E3F) stores the raw value (+0x38) or the curve pointer and count (+0x50, +0x54) in the omitted nested settings
         * object, marks it dirty (+2 bit 0x20, +3 bit 0x01) and skips the commit helper 0x406E90 and every APU write, S_OK. Host record only. */
        if(n->value.scope.flags!=0x10u || n->value.cache_mask!=7u || n->value.data_sets==0u) {
            r->error="only the measured apply=1 update on a configured spatial buffer with recorded data is supported";
            goto done;
        }
        if(r->entry==MINIMUM) {
            n->value.min_distance_bits=r->argument;n->value.min_distance_sets++;
        } else {
            n->value.curve_address=r->argument;n->value.curve_count=r->count;n->value.rolloff_sets++;
        }
        r->announce=!apply_announced;apply_announced=true;
        *result=0u;goto done;
    }
    if(n->value.scope.flags!=0x10u || r->apply!=0u){r->error="only startup spatial setters/apply0 are supported";goto done;}
    if(r->entry==I3DL2) {
        uint32_t words[9],expected[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
        if(n->value.cache_mask!=0u || aliases_headers(r->argument,36u,internal) ||
           !kernel_guest_read_bytes(r->argument,words,sizeof(words)) || memcmp(words,expected,sizeof(words))!=0) {
            r->error="only exact first startup I3DL2 cache is supported";goto done;
        }
        memcpy(n->value.i3dl2,words,sizeof(words));n->value.i3dl2_address=r->argument;n->value.cache_mask=1u;
    } else if(r->entry==MINIMUM) {
        if(n->value.cache_mask!=1u || r->argument!=0x3F800000u){r->error="only ordered startup minDistance1.0 is supported";goto done;}
        n->value.min_distance_bits=r->argument;n->value.cache_mask=3u;
    } else {
        if(n->value.cache_mask!=3u || r->argument!=CURVE || r->count!=1u){r->error="only ordered startup curve VA/count1 is supported";goto done;}
        n->value.curve_address=r->argument;n->value.curve_count=r->count;n->value.cache_mask=7u;
    }
    *result=0u;
done:pthread_mutex_unlock(&lock);
}
/* Original global error precedes buffer/settings access for these setters. */
static uint32_t setter_preflight(uint32_t entry)
{
    pthread_mutex_lock(&lock);
    const bool admitted=enabled && (entry!=SET_LOOP_REGION || completion);
    uint8_t irql=0u;const bool known=irql_provider!=NULL && irql_provider(&irql);
    pthread_mutex_unlock(&lock);
    if(!admitted || !known || irql!=0u)refuse(entry,"buffer setter needs explicit policy at known IRQL0");
    uint32_t state;
    if(!kernel_guest_read_u32(GLOBAL_STATE,&state))refuse(entry,"global audio state inaccessible");
    return state!=0u?0x80004005u:0u;
}
static uint32_t access_extended(uint32_t entry,uint32_t address,uint32_t argument,
                                uint32_t count,uint32_t apply,uint32_t position_z)
{
    if(entry==SET_POSITION || entry==SET_LOOP_REGION || entry==MAX_DISTANCE) {
        const uint32_t failure=setter_preflight(entry);
        if(failure!=0u)return failure;
    }
    access_request r={0};r.address=address;r.entry=entry;r.argument=argument;r.count=count;r.apply=apply;r.position_z=position_z;
    if(!identify(address,&r.identity))refuse(entry,"buffer is not owned by passive adapter");
    uint32_t result=0u;
    if(!dsound_device_with_owned_interface(r.identity.internal_address+8u,access_owned,&r,&result))
        refuse(entry,"parent SILENT device ownership/generation changed");
    if(r.error!=NULL){
        /* T722: a refusal carries the buffer and the argument, so the next stop is read from the log. */
        char text[256];
        snprintf(text,sizeof text,"%s (buffer %#x, argument %#x, count %#x)",r.error,address,argument,count);
        refuse(entry,text);
    }
    if(r.announce && r.entry==SET_LOOP_REGION)dsound_hle_log()("dsound: explicit passive SetLoopRegion; original byte region recorded on host; virtual loop cursor and Stop drain INFERRED, no guest settings/APU write or buffer audio output\n");
    if(r.announce && r.entry==SET_DATA)dsound_hle_log()("dsound: explicit HEADLESS SetBufferData; the passive buffer records the data pointer "
        "and length on the host only (no guest word, settings object, page lock, APU write or critical section), S_OK, no playback\n");
    if(r.announce && r.entry==SET_FREQUENCY)dsound_hle_log()("dsound: explicit HEADLESS SetFrequency; the passive buffer records the startup frequency "
        "22042 on the host only (no guest word, settings pitch, APU pitch register or critical section), S_OK, no playback\n");
    if(r.announce && r.entry==PAUSE)dsound_hle_log()("dsound: explicit HEADLESS buffer Pause(0); a buffer nothing started has no state to change, "
        "the passive buffer counts the call on the host only (no guest word, APU write or critical section), S_OK, no playback\n");
    if(r.announce && r.entry==SET_VOLUME)dsound_hle_log()("dsound: explicit HEADLESS SetVolume; the passive buffer records the startup volume "
        "-10000 and the sound update volume -3204 on the host only (no guest word, settings object, APU voice write or critical section), S_OK, no playback\n");
    if(r.announce && (r.entry==MINIMUM || r.entry==ROLLOFF) && r.apply==1u)dsound_hle_log()("dsound: explicit HEADLESS apply=1 SetMinDistance/SetRolloffCurve; the sound start's raw value (or curve pointer and count) is recorded on the host only (no guest settings word, commit, APU write or spatial playback)\n");
    if(r.announce && r.entry==MAX_DISTANCE)dsound_hle_log()("dsound: explicit HEADLESS SetMaxDistance; the measured apply=1 caller's raw float bits are recorded on the host only (no guest settings word, commit, APU write or spatial playback)\n");
    if(r.announce && r.entry==SET_POSITION)dsound_hle_log()("dsound: explicit HEADLESS SetPosition; measured apply=1 title callers' raw xyz bits are recorded on the host only (no guest settings words, commit, APU write or spatial playback)\n");
    return result;
}
static uint32_t access(uint32_t entry,uint32_t address,uint32_t argument,uint32_t count,uint32_t apply)
{return access_extended(entry,address,argument,count,apply,0u);}
uint32_t dsound_buffer_cache_i3dl2(uint32_t buffer,uint32_t parameters,uint32_t apply)
{return access(I3DL2,buffer,parameters,0u,apply);}
uint32_t dsound_buffer_cache_min_distance(uint32_t buffer,uint32_t bits,uint32_t apply)
{return access(MINIMUM,buffer,bits,0u,apply);}
uint32_t dsound_buffer_cache_rolloff(uint32_t buffer,uint32_t curve,uint32_t count,uint32_t apply)
{return access(ROLLOFF,buffer,curve,count,apply);}
uint32_t dsound_buffer_cache_max_distance(uint32_t buffer,uint32_t bits,uint32_t apply)
{return access(MAX_DISTANCE,buffer,bits,0u,apply);}
uint32_t dsound_buffer_set_position(uint32_t buffer,uint32_t x_bits,uint32_t y_bits,
                                    uint32_t z_bits,uint32_t apply)
{return access_extended(SET_POSITION,buffer,x_bits,y_bits,apply,z_bits);}
uint32_t dsound_buffer_set_data(uint32_t buffer,uint32_t data,uint32_t length)
{return access(SET_DATA,buffer,data,length,0u);}
uint32_t dsound_buffer_set_loop_region(uint32_t buffer,uint32_t start,uint32_t length)
{return access(SET_LOOP_REGION,buffer,start,length,0u);}
uint32_t dsound_buffer_set_volume(uint32_t buffer,int32_t volume)
{return access(SET_VOLUME,buffer,(uint32_t)volume,0u,0u);}
uint32_t dsound_buffer_pause(uint32_t buffer,uint32_t mode)
{return access(PAUSE,buffer,mode,0u,0u);}
uint32_t dsound_buffer_set_frequency(uint32_t buffer,uint32_t hertz)
{return access(SET_FREQUENCY,buffer,hertz,0u,0u);}
bool dsound_buffer_get_snapshot(uint32_t buffer,dsound_buffer_snapshot *output)
{
    if(output==NULL)return false;access_request r={0};dsound_buffer_snapshot value;
    r.address=buffer;r.observer=true;r.snapshot=&value;
    if(!identify(buffer,&r.identity))return false;uint32_t result=0u;
    if(!dsound_device_with_owned_interface(r.identity.internal_address+8u,access_owned,&r,&result) || r.error!=NULL)return false;
    *output=value;return true;
}
typedef struct reset_request {uint32_t address;dsound_device_lease identity;buffer_node *node;bool held;} reset_request;
static bool reset_prepare(const dsound_device_lease *candidate,void *userdata,dsound_device_lease_child *child)
{
    reset_request *r=userdata;pthread_mutex_lock(&lock);r->held=true;r->node=find(r->address);
    if(!token_equal(candidate,&r->identity) || !valid_node(r->node,&r->identity,candidate->internal_address))return false;
    *child=(dsound_device_lease_child){r->node->value.buffer_heap,r->node->value.header_address,BUFFER_BYTES};return true;
}
static void reset_abort(void *userdata)
{reset_request *r=userdata;r->held=false;pthread_mutex_unlock(&lock);}
static void reset_finalize(const dsound_device_lease *committed,void *userdata)
{
    reset_request *r=userdata;(void)committed;buffer_node **p=&buffers;
    while(*p!=r->node)p=&(*p)->next;*p=r->node->next;r->node->next=detached;detached=r->node;
    r->held=false;pthread_mutex_unlock(&lock);
}
static const dsound_device_lease_ops reset_ops={reset_prepare,reset_abort,reset_finalize};
static bool reset_buffers(void)
{
    for(;;) {
        reset_request r={0};pthread_mutex_lock(&lock);
        if(buffers!=NULL){r.address=buffers->value.buffer_address;r.identity=buffers->value.lease;}
        pthread_mutex_unlock(&lock);if(r.address==0u)break;
        if(dsound_device_release_lease(&r.identity,&reset_ops,&r)!=DSOUND_LEASE_OK)return false;
        /* Detached nodes remain tracked if checked cleanup refuses. No fallible
         * guest cleanup runs under device lock or in finalize. */
        pthread_mutex_lock(&lock);buffer_node **p=&detached;
        while(*p!=r.node)p=&(*p)->next;
        if(!valid_node(r.node,&r.identity,r.identity.internal_address) ||
           !guest_heap_destroy(r.node->value.buffer_heap)){pthread_mutex_unlock(&lock);return false;}
        *p=r.node->next;free(r.node);pthread_mutex_unlock(&lock);
    }
    pthread_mutex_lock(&lock);
    while(detached!=NULL) {
        buffer_node *n=detached;
        if(!valid_node(n,&n->value.lease,n->value.lease.internal_address) ||
           !guest_heap_destroy(n->value.buffer_heap)){pthread_mutex_unlock(&lock);return false;}
        detached=n->next;free(n);
    }
    while(rollback!=NULL) {
        buffer_node *n=rollback;
        if(!guest_heap_valid(n->value.buffer_heap) || !guest_heap_destroy(n->value.buffer_heap)) {
            pthread_mutex_unlock(&lock);return false;
        }
        rollback=n->next;free(n);
    }
    loop_announced=false;data_announced=false;volume_announced=false;pause_announced=false;frequency_announced=false;
    max_distance_announced=false;position_announced=false;apply_announced=false;announced=false;pthread_mutex_unlock(&lock);return true;
}
bool dsound_buffer_reset_checked(void)
{
    pthread_mutex_lock(&reset_lock);bool result=reset_buffers();
    pthread_mutex_unlock(&reset_lock);return result;
}
void dsound_buffer_reset(void){(void)dsound_buffer_reset_checked();}
static uint32_t frame_handler(void *context,uint32_t entry,uint32_t caller,unsigned count)
{
    const kernel_call_frame *frame=context;uint32_t actual,args[5];
    /* T733: with the completion model on, the buffer methods are gated on the state the model checks, not on the call site (the
     * originals never read their caller). Create and the three startup setters keep their measured callers. */
    pthread_mutex_lock(&lock);const bool completion_on=completion;pthread_mutex_unlock(&lock);
    const bool caller_free=completion_on && (entry==SET_DATA || entry==SET_VOLUME || entry==PAUSE || entry==SET_FREQUENCY);
    const bool loop_caller_free=completion_on && entry==SET_LOOP_REGION;
    if(frame==NULL || !kernel_guest_read_u32(frame->stack_ptr,&actual))
        refuse(entry,"unreadable return address");
    const bool position_caller=entry==SET_POSITION &&
        (actual==POSITION_CALLER_1 || actual==POSITION_CALLER_2 || actual==POSITION_CALLER_3);
    if(!caller_free && !loop_caller_free && entry!=SET_POSITION &&
       (entry==CREATE ? (actual!=0x27AA9u && actual!=0x27B54u) :
        (actual!=caller && !(entry==SET_VOLUME && actual==SET_VOLUME_UPDATE_CALLER) &&
         !(entry==MINIMUM && actual==MINIMUM_START_CALLER) && !(entry==ROLLOFF && actual==ROLLOFF_START_CALLER))))
        refuse(entry,"only measured startup caller is supported");
    if(entry==SET_POSITION && !position_caller)
        refuse(entry,"only the three measured SetPosition callers are supported");
    for(unsigned i=0u;i<count;i++)if(!kernel_frame_arg(frame,i,&args[i]))refuse(entry,"unreadable argument");
    /* T605: each measured SetVolume caller keeps its own volume, the start (0x28348) -10000 and the update (0x28643) -3204. */
    if(entry==SET_VOLUME && (actual==SET_VOLUME_UPDATE_CALLER ? (!completion && args[1]!=VOLUME_UPDATE) : (!completion && args[1]!=VOLUME_STARTUP)))
    {
        char text[160];
        snprintf(text,sizeof text,"a measured SetVolume caller is paired with its own volume, got return address %#x args (%#x, %d)",
            actual,args[0],(int32_t)args[1]);
        refuse(entry,text);
    }
    /* T1173: the startup setters pass apply=0 and the sound start's re-apply passes apply=1, each caller keeps its own. */
    if((entry==MINIMUM || entry==ROLLOFF) && !caller_free) {
        const uint32_t apply=entry==MINIMUM?args[2]:args[3];
        if((actual==MINIMUM_START_CALLER || actual==ROLLOFF_START_CALLER)!=(apply==1u))
            refuse(entry,"a measured spatial setter caller is paired with its own apply value");
    }
    if(entry==CREATE) {
        dsound_buffer_scope scope;
        if(!dsound_buffer_scope_snapshot(args[1],&scope) ||
           scope.flags!=(actual==0x27AA9u?16u:0u))refuse(entry,"caller/descriptor class mismatch");
        return create_from_scope(args[0],&scope,args[2],args[3]);
    }
    if(entry==I3DL2)return dsound_buffer_cache_i3dl2(args[0],args[1],args[2]);
    if(entry==MINIMUM)return dsound_buffer_cache_min_distance(args[0],args[1],args[2]);
    if(entry==SET_LOOP_REGION)return dsound_buffer_set_loop_region(args[0],args[1],args[2]);
    if(entry==SET_DATA)return dsound_buffer_set_data(args[0],args[1],args[2]);
    if(entry==SET_VOLUME)return access(SET_VOLUME,args[0],args[1],0u,0u);
    if(entry==PAUSE)return dsound_buffer_pause(args[0],args[1]);
    if(entry==SET_FREQUENCY)return dsound_buffer_set_frequency(args[0],args[1]);
    if(entry==MAX_DISTANCE)return dsound_buffer_cache_max_distance(args[0],args[1],args[2]);
    if(entry==SET_POSITION)return dsound_buffer_set_position(args[0],args[1],args[2],args[3],args[4]);
    return dsound_buffer_cache_rolloff(args[0],args[1],args[2],args[3]);
}
static uint32_t create_handler(void *c){return frame_handler(c,CREATE,0u,4u);}
static uint32_t i3dl2_handler(void *c){return frame_handler(c,I3DL2,0x27AE8u,3u);}
static uint32_t minimum_handler(void *c){return frame_handler(c,MINIMUM,0x27AF6u,3u);}
static uint32_t rolloff_handler(void *c){return frame_handler(c,ROLLOFF,0x27B06u,4u);}
static uint32_t set_data_handler(void *c){return frame_handler(c,SET_DATA,SET_DATA_CALLER,3u);}
static uint32_t set_volume_handler(void *c){return frame_handler(c,SET_VOLUME,SET_VOLUME_CALLER,2u);}
static uint32_t pause_handler(void *c){return frame_handler(c,PAUSE,PAUSE_CALLER,2u);}
static uint32_t frequency_handler(void *c){return frame_handler(c,SET_FREQUENCY,SET_FREQUENCY_CALLER,2u);}
static uint32_t max_distance_handler(void *c){return frame_handler(c,MAX_DISTANCE,MAX_DISTANCE_CALLER,3u);}
static uint32_t position_handler(void *c){return frame_handler(c,SET_POSITION,0u,5u);}
static uint32_t loop_region_handler(void *c){return frame_handler(c,SET_LOOP_REGION,0u,3u);}
size_t dsound_buffer_register(void)
{
    pthread_mutex_lock(&lock);const bool policy=enabled;const bool loops=enabled && completion;pthread_mutex_unlock(&lock);
    size_t count=0u;count+=dsound_hle_register(CREATE,create_handler)?1u:0u;
    /* Only with the policy on, so a flags-off boot keeps its exact registry (T597, T601). */
    if(loops)count+=dsound_hle_register(SET_LOOP_REGION,loop_region_handler)?1u:0u;
    if(policy)count+=dsound_hle_register(SET_DATA,set_data_handler)?1u:0u;
    if(policy)count+=dsound_hle_register(SET_VOLUME,set_volume_handler)?1u:0u;
    if(policy)count+=dsound_hle_register(PAUSE,pause_handler)?1u:0u;
    if(policy)count+=dsound_hle_register(SET_FREQUENCY,frequency_handler)?1u:0u;
    if(policy)count+=dsound_hle_register(MAX_DISTANCE,max_distance_handler)?1u:0u;
    if(policy)count+=dsound_hle_register(SET_POSITION,position_handler)?1u:0u;
    count+=dsound_hle_register(I3DL2,i3dl2_handler)?1u:0u;
    count+=dsound_hle_register(MINIMUM,minimum_handler)?1u:0u;
    count+=dsound_hle_register(ROLLOFF,rolloff_handler)?1u:0u;return count;
}
