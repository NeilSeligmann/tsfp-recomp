/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_stream.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsound_hle.h"
#include "dsound_audio_runtime.h"
#include "dsound_completion.h"
#include "dsound_stream_frequency.h"
#include "kernel_clock.h"
#include "guest_mem.h"
#include "kernel_call.h"
#define CREATE 0x0040967Cu
#define I3DL2 0x00408637u
#define MINIMUM 0x004085F1u
#define MAX_DISTANCE 0x004085D9u
#define SET_POSITION 0x00408609u
#define ROLLOFF 0x00408632u
#define STATUS 0x004073D3u
#define VOLUME 0x00407B14u
#define PAUSE 0x00407B23u
#define SET_FORMAT 0x00408C2Du
#define HEADROOM 0x00407B19u
#define MIX_VOLUMES 0x00407B1Eu
#define MIX_BINS 0x004085D4u
#define FLUSH_EX 0x00407B28u
#define DISCONTINUITY 0x0040733Bu
#define SET_FREQUENCY 0x004085CFu
#define SINGLETON 0x00412B30u
#define GLOBAL_STATE 0x004124A8u
#define VTABLE 0x004A1D00u
#define SECONDARY 0x004A1CF0u
#define CURVE 0x004B914Cu
#define CURVE_NEAR 0x004B915Cu
#define CURVE_FAR 0x004B916Cu
#define STREAM_BYTES 40u
#define OOM 0x8007000Eu
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t reset_lock=PTHREAD_MUTEX_INITIALIZER;
static bool enabled,announced,completion;
static uint64_t accepted_frequency_sets;
static dsound_stream_pause_note_fn pause_note;
static dsound_stream_routing_note_fn routing_note;
static dsound_stream_frequency_note_fn frequency_note;
static dsound_stream_format_note_fn format_note;
static dsound_stream_control_word_fn frequency_control_word;
static dsound_stream_irql_provider irql_provider;
static dsound_stream_fatal_fn fatal_handler;
static dsound_stream_public_route_fn public_route;
static dsound_stream_reset_route_fn reset_route;
typedef struct stream_node {
    dsound_stream_snapshot value;
    struct stream_node *next;
} stream_node;
static stream_node *streams,*detached,*rollback;
static bool overlap(uint32_t a,uint32_t an,uint32_t b,uint32_t bn)
{return (uint64_t)a<(uint64_t)b+bn && (uint64_t)b<(uint64_t)a+an;}
static void refuse(uint32_t entry,const char *reason) __attribute__((noreturn));
static void refuse(uint32_t entry,const char *reason)
{
    pthread_mutex_lock(&lock);dsound_stream_fatal_fn fatal=fatal_handler;pthread_mutex_unlock(&lock);
    dsound_hle_log()("dsound passive stream %#x refused: %s\n",entry,reason);
    if(fatal!=NULL)fatal(entry,reason);abort();
}
void dsound_stream_set_enabled(bool value)
{pthread_mutex_lock(&lock);enabled=value;pthread_mutex_unlock(&lock);}
void dsound_stream_set_completion(bool value,dsound_stream_pause_note_fn note)
{pthread_mutex_lock(&lock);completion=value;pause_note=value?note:NULL;pthread_mutex_unlock(&lock);}
void dsound_stream_set_routing_note(dsound_stream_routing_note_fn note)
{pthread_mutex_lock(&lock);routing_note=note;pthread_mutex_unlock(&lock);}
uint64_t dsound_stream_frequency_set_count(void)
{pthread_mutex_lock(&lock);const uint64_t count=accepted_frequency_sets;pthread_mutex_unlock(&lock);return count;}
void dsound_stream_set_format_note(dsound_stream_format_note_fn note)
{pthread_mutex_lock(&lock);format_note=note;pthread_mutex_unlock(&lock);}
void dsound_stream_set_frequency_note(dsound_stream_frequency_note_fn note,
    dsound_stream_control_word_fn control_word)
{
    pthread_mutex_lock(&lock);frequency_note=note;frequency_control_word=control_word;
    pthread_mutex_unlock(&lock);
}
void dsound_stream_set_irql_provider(dsound_stream_irql_provider provider)
{pthread_mutex_lock(&lock);irql_provider=provider;pthread_mutex_unlock(&lock);}
void dsound_stream_set_fatal(dsound_stream_fatal_fn fatal)
{pthread_mutex_lock(&lock);fatal_handler=fatal;pthread_mutex_unlock(&lock);}
void dsound_stream_set_extension(dsound_stream_public_route_fn route,dsound_stream_reset_route_fn reset)
{pthread_mutex_lock(&lock);public_route=route;reset_route=reset;pthread_mutex_unlock(&lock);}
/* Called with stream lock held. Provider must only report current IRQL, never
 * reenter this adapter. Kernel TLS observation does not enter the device lock. */
static const char *policy_error(void)
{
    uint32_t state;uint8_t irql;
    if(!enabled)return "explicit headless-streams policy is disabled";
    if(irql_provider==NULL || !irql_provider(&irql) || irql!=0u)return "only known IRQL0 is supported";
    if(!kernel_guest_read_u32(GLOBAL_STATE,&state) || state!=0u)return "original global audio state must be zero";
    return NULL;
}
static stream_node *find(uint32_t address)
{for(stream_node *n=streams;n!=NULL;n=n->next)if(n->value.stream_address==address)return n;return NULL;}
static bool token_equal(const dsound_device_lease *a,const dsound_device_lease *b)
{return a->device_heap==b->device_heap && a->internal_address==b->internal_address && a->serial==b->serial;}
static bool valid_node(const stream_node *node,const dsound_device_lease *identity,uint32_t internal)
{
    uint32_t actual[10],bytes;
    return node!=NULL && token_equal(&node->value.lease,identity) && identity->internal_address==internal &&
        guest_heap_valid(identity->device_heap) &&
        kernel_guest_read_bytes(node->value.stream_address,actual,sizeof(actual)) &&
        memcmp(actual,node->value.header,sizeof(actual))==0 && guest_heap_valid(node->value.stream_heap) &&
        guest_heap_block_size(node->value.stream_heap,node->value.stream_address,&bytes) && bytes==STREAM_BYTES;
}
static bool aliases_state(uint32_t address,uint32_t bytes,uint32_t internal)
{
    if(overlap(address,bytes,internal,44u) || overlap(address,bytes,SINGLETON,4u) ||
       overlap(address,bytes,GLOBAL_STATE,4u) || overlap(address,bytes,SECONDARY,60u) ||
       overlap(address,bytes,CURVE,16u))return true;
    for(stream_node *n=streams;n!=NULL;n=n->next)
        if(overlap(address,bytes,n->value.stream_address,STREAM_BYTES) ||
           overlap(address,bytes,n->value.publication_address,4u) ||
           overlap(address,bytes,n->value.scope.descriptor_address,24u) ||
           overlap(address,bytes,n->value.scope.format_address,20u) ||
           (n->value.i3dl2_address!=0u && overlap(address,bytes,n->value.i3dl2_address,36u)))return true;
    for(stream_node *n=detached;n!=NULL;n=n->next)
        if(overlap(address,bytes,n->value.stream_address,STREAM_BYTES))return true;
    for(stream_node *n=rollback;n!=NULL;n=n->next)
        if(n->value.stream_address!=0u && overlap(address,bytes,n->value.stream_address,STREAM_BYTES))return true;
    return false;
}
/* Input buffers are historical stack snapshots, not guest-owned state. The
 * original setter reuses/overlaps these locals after Create has consumed them. */
static bool aliases_headers(uint32_t address,uint32_t bytes,uint32_t internal)
{
    if(overlap(address,bytes,internal,44u) || overlap(address,bytes,SINGLETON,4u) ||
       overlap(address,bytes,GLOBAL_STATE,4u) || overlap(address,bytes,SECONDARY,60u))return true;
    for(stream_node *n=streams;n!=NULL;n=n->next)
        if(overlap(address,bytes,n->value.stream_address,STREAM_BYTES) ||
           overlap(address,bytes,n->value.publication_address,4u))return true;
    for(stream_node *n=detached;n!=NULL;n=n->next)
        if(overlap(address,bytes,n->value.stream_address,STREAM_BYTES))return true;
    for(stream_node *n=rollback;n!=NULL;n=n->next)
        if(n->value.stream_address!=0u && overlap(address,bytes,n->value.stream_address,STREAM_BYTES))return true;
    return false;
}
/* Unordered safety facts, NOT a copy of original ordered vtable bytes. Every
 * member has a verified unconditional T161 boundary in the embedding host. */
static bool safe_tables(const uint32_t table[15])
{
    static const uint32_t stopped[14]={0x00384859u,0x0040686Cu,0x00406879u,0x0040688Au,
        0x0040723Fu,0x00407286u,0x004072D4u,0x0040733Bu,0x00407388u,0x004073D3u,
        0x00407424u,0x00407883u,0x0040788Du,0x004093ADu};
    for(unsigned i=0u;i<15u;i++) {
        bool found=false;for(unsigned j=0u;j<14u;j++)if(table[i]==stopped[j])found=true;
        if(!found)return false;
    }
    return true;
}
bool dsound_stream_original_tables_stopped(void)
{
    uint32_t table[15];
    return kernel_guest_read_bytes(SECONDARY,table,sizeof(table)) && safe_tables(table);
}
typedef struct create_request {
    dsound_stream_scope scope;
    uint32_t output,status;
    void *output_at;
    stream_node *node;
    const char *error;
    bool held,announce;
} create_request;
static bool create_prepare(const dsound_device_lease *candidate,void *userdata,
                           dsound_device_lease_child *child)
{
    create_request *r=userdata;pthread_mutex_lock(&lock);r->held=true;
    r->error=policy_error();if(r->error!=NULL)return false;
    uint32_t old,table[15];
    if(!kernel_guest_read_bytes(SECONDARY,table,sizeof(table)) ||
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
    r->node->value.source_rate_hz=r->scope.sample_rate;
    dsound_stream_routing_init(&r->node->value.routing);
    uint32_t heap=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);r->node->value.stream_heap=heap;
    if(heap==0u){r->status=OOM;return false;}
    uint32_t address=guest_heap_alloc(heap,STREAM_BYTES);r->node->value.stream_address=address;
    if(address==0u){r->status=OOM;return false;}
    uint32_t *header=r->node->value.header;
    header[0]=VTABLE;header[1]=SECONDARY;header[2]=1u;header[3]=candidate->internal_address;
    if(aliases_state(address,STREAM_BYTES,candidate->internal_address) ||
       overlap(address,STREAM_BYTES,r->output,4u) ||
       overlap(address,STREAM_BYTES,r->scope.descriptor_address,24u) ||
       overlap(address,STREAM_BYTES,r->scope.format_address,20u) ||
       !kernel_guest_write_bytes(address,header,STREAM_BYTES)) {
        r->error="new private stream allocation/alias is invalid";return false;
    }
    *child=(dsound_device_lease_child){heap,address,STREAM_BYTES};return true;
}
static void create_abort(void *userdata)
{
    create_request *r=userdata;
    /* Device lease callbacks run under the device lock. Retain the unpublished
     * candidate until acquire_lease returns; cleanup and fatal must run afterward. */
    r->held=false;pthread_mutex_unlock(&lock);
}
static void cleanup_aborted_create(create_request *r)
{
    if(r->node==NULL)return;
    pthread_mutex_lock(&lock);
    const uint32_t heap=r->node->value.stream_heap;
    if(heap==0u || (guest_heap_valid(heap) && guest_heap_destroy(heap))) {
        free(r->node);
    } else {
        /* No committed lease/token exists. The generation-bound exclusive
         * private heap may have no header, or a partially written header. */
        r->node->next=rollback;rollback=r->node;
        r->error="private allocation rollback failed; unleased cleanup retained";
    }
    r->node=NULL;pthread_mutex_unlock(&lock);
}
static void create_finalize(const dsound_device_lease *committed,void *userdata)
{
    create_request *r=userdata;
    /* acquire_lease copies its caller output token AFTER this callback. Store
     * the COMMITTED token here before making the sidecar or guest pointer visible. */
    r->node->value.lease=*committed;r->node->next=streams;streams=r->node;
    r->announce=!announced;announced=true;
    memcpy(r->output_at,&r->node->value.stream_address,4u);
    r->held=false;pthread_mutex_unlock(&lock);
}
static const dsound_device_lease_ops create_ops={create_prepare,create_abort,create_finalize};
uint32_t dsound_stream_create(uint32_t descriptor,uint32_t output)
{
    create_request r={0};r.output=output;
    pthread_mutex_lock(&lock);const char *error=policy_error();pthread_mutex_unlock(&lock);
    if(error!=NULL)refuse(CREATE,error);
    if(!dsound_stream_scope_snapshot(descriptor,&r.scope))refuse(CREATE,"descriptor/format outside measured startup scope");
    uint32_t device;if(!kernel_guest_read_u32(SINGLETON,&device) || device==0u || device>UINT32_MAX-8u)
        refuse(CREATE,"no owned SILENT device");
    dsound_device_lease lease;
    dsound_device_lease_status status=dsound_device_acquire_lease(device+8u,&create_ops,&r,&lease);
    if(status!=DSOUND_LEASE_OK)cleanup_aborted_create(&r);
    if(status==DSOUND_LEASE_OUT_OF_MEMORY || (status==DSOUND_LEASE_PREPARE_FAILED && r.status==OOM && r.error==NULL))return OOM;
    if(status!=DSOUND_LEASE_OK)refuse(CREATE,r.error!=NULL?r.error:"device lease transaction refused");
    if(r.announce)dsound_hle_log()("dsound: explicit HEADLESS streams; passive40-byte public headers and "
        "host startup setter caches; omitted settings/hardware/list/packet objects, APU/DSP/FP/critical-section "
        "operations; no audio output or packet completion\n");
    return 0u;
}
typedef struct access_request {
    uint32_t address,entry,argument,apply,count,position_z;
    dsound_device_lease identity;
    dsound_stream_snapshot *snapshot;
    const char *error;
    bool observer;
} access_request;
static bool identify(uint32_t address,dsound_device_lease *identity)
{
    pthread_mutex_lock(&lock);stream_node *n=find(address);if(n!=NULL)*identity=n->value.lease;
    pthread_mutex_unlock(&lock);return n!=NULL;
}
static void access_owned(uint32_t internal,void *userdata,uint32_t *result)
{
    access_request *r=userdata;pthread_mutex_lock(&lock);stream_node *n=find(r->address);
    if(!valid_node(n,&r->identity,internal)){r->error="stream ownership/header/generation changed";goto done;}
    if(r->observer){*r->snapshot=n->value;*result=0u;goto done;}
    r->error=policy_error();if(r->error!=NULL)goto done;
    if(r->entry==SET_FREQUENCY) {
        const bool stereo=n->value.scope.flags==0u && n->value.scope.channels==2u && n->value.cache_mask==0u;
        const bool spatial=n->value.scope.flags==0x10u && n->value.scope.channels==1u && n->value.cache_mask==7u;
        if(!completion || frequency_note==NULL || (!stereo && !spatial) ||
           !n->value.volume_seen || !n->value.discontinuity_seen) {
            r->error="frequency needs a started stereo or fully configured mono PCM stream";goto done;
        }
        uint32_t hertz=r->argument;
        if(hertz==0u) {
            hertz=n->value.scope.sample_rate;
            if(n->value.format_sets!=0u)memcpy(&hertz,n->value.format+4u,sizeof(hertz));
        }
        int32_t pitch;
        if(!dsound_stream_frequency_pitch(hertz,(uint16_t)r->count,&pitch)) {
            r->error="frequency or guest control word outside measured domain";goto done;
        }
        if(n->value.frequency_sets==UINT32_MAX ||
           !frequency_note(r->address,n->value.lease.serial,n->value.source_rate_hz,hertz)) {
            r->error="frequency PCM/completion transaction refused";goto done;
        }
        n->value.source_rate_hz=hertz;n->value.frequency_pitch=pitch;n->value.frequency_sets++;
        accepted_frequency_sets++;
        *result=0u;goto done;
    }
    if (r->entry == HEADROOM || r->entry == MIX_BINS || r->entry == MIX_VOLUMES) {
        if (!completion || routing_note == NULL || n->value.scope.flags != 0u ||
            n->value.scope.channels != 2u) {
            r->error="real PCM routing requires admitted nonspatial stereo stream and completion policy";
            goto done;
        }
        dsound_stream_routing candidate=n->value.routing;
        bool admitted=true;
        if (r->entry == HEADROOM) dsound_stream_routing_headroom(&candidate,r->argument);
        else if (r->entry == MIX_BINS && r->argument == 0u)
            admitted=dsound_stream_routing_bins(&candidate,NULL,0u,true);
        else {
            uint32_t descriptor[2]={0u,0u};
            if (r->argument == 0u || aliases_headers(r->argument,4u,internal) ||
                !kernel_guest_read_u32(r->argument,&descriptor[0]) ||
                (descriptor[0]!=0u &&
                 ((uint64_t)r->argument+8u>UINT64_C(0x100000000) ||
                  aliases_headers(r->argument+4u,4u,internal) ||
                  !kernel_guest_read_u32(r->argument+4u,&descriptor[1])))) {
                r->error="mix-bin descriptor inaccessible or aliases owned state";goto done;
            }
            const uint32_t maximum=r->entry==MIX_BINS?DSOUND_STREAM_ROUTE_SLOTS:DSOUND_STREAM_ROUTE_BINS;
            dsound_stream_route_pair pairs[DSOUND_STREAM_ROUTE_BINS];
            if (descriptor[0] > maximum || (descriptor[0] != 0u &&
                (descriptor[1]==0u || (uint64_t)descriptor[1]+descriptor[0]*8u>UINT64_C(0x100000000) ||
                 aliases_headers(descriptor[1],descriptor[0]*8u,internal) ||
                 !kernel_guest_read_bytes(descriptor[1],pairs,descriptor[0]*8u)))) {
                r->error="mix-bin pairs outside checked bounds or aliases owned state";goto done;
            }
            admitted=r->entry==MIX_BINS?
                dsound_stream_routing_bins(&candidate,pairs,descriptor[0],false):
                dsound_stream_routing_volumes(&candidate,pairs,descriptor[0]);
        }
        if (!admitted) {r->error="mix-bin state needs unsupported DSP/HRTF route or invalid index";goto done;}
        if (!routing_note(r->address,n->value.lease.serial,&candidate)) {
            r->error="PCM routing rejected clock/history/lifetime or inactive renderer";goto done;
        }
        n->value.routing=candidate;
        *result=0u;goto done;
    }
    if (r->entry == STATUS) {
        uint32_t old;
        bool spatial = n->value.scope.flags == 0x10u && n->value.cache_mask == 7u;
        bool stereo = n->value.scope.flags == 0u && n->value.cache_mask == 0u &&
            n->value.scope.sample_rate == 44100u && n->value.scope.channels == 2u &&
            n->value.scope.block_align == 72u &&
            n->value.scope.average_bytes_per_second == 49612u;
        /* T605: the title's stream update (sub_00029CC0, 0x29CEA) and sub_00029D10 (0x29D28) also ask every started stream:
         * volume -10000, Pause1, FlushEx and Discontinuity recorded (each implied by the recorded Discontinuity), the
         * stereo stream with or without its SetFormat and the spatial one. The original answers 1 for all of them
         * (bit 0 only: the free packet list is non-empty, no packet, voice flags 0x15), see docs/audio-input-recovery.md. */
        bool started = n->value.discontinuity_seen;
        bool fresh = !n->value.pause_seen && !n->value.flush_seen && !n->value.discontinuity_seen;
        if ((!spatial && !stereo) || n->value.scope.max_packets != 3u ||
            !n->value.volume_seen || n->value.volume != -10000 || (!fresh && !started)) {
            r->error = "only exact spatial or fresh stereo silence startup status or the status of a started stream is supported";
            goto done;
        }
        if (r->argument == 0u || (uint64_t)r->argument + 4u > UINT64_C(0x100000000) ||
            aliases_headers(r->argument,4u,internal) || overlap(r->argument,4u,CURVE,16u) ||
            !kernel_guest_read_u32(r->argument,&old) ||
            !kernel_guest_write_u32(r->argument,old) ||
            !kernel_guest_write_u32(r->argument,1u)) {
            r->error = "startup status output is inaccessible or aliases protected state";
            goto done;
        }
        /* Exact reached startup response only. Original derives bit0 from its
         * non-self B0 list; no native packet admission exists. This is not a
         * general playback/readiness query; future packet support must revisit. */
        *result = 0u;
        goto done;
    }
    if (r->entry == SET_FORMAT) {
        uint8_t words[DSOUND_STREAM_FORMAT_BYTES] = {0};
        /* Flags 0 is the stereo 44100 startup scope, dsound_stream_scope.c admits nothing else for it. T1181: the mono flags 0x10
         * scope with its three recorded spatial setters (cache mask 7) is the other started startup slot the title re-formats. */
        const bool stereo = n->value.scope.flags == 0u;
        const bool spatial = n->value.scope.flags == 0x10u && n->value.cache_mask == 7u;
        /* Discontinuity is recorded only after Pause1 and FlushEx(0,0,1), and -10000 is the only volume. */
        bool started = n->value.volume_seen && n->value.discontinuity_seen;
        if (!stereo && !spatial) {
            r->error = "only the stereo startup scope or a configured spatial stream can change format";
            goto done;
        }
        if (!started) {
            r->error = "only a fully started (volume, Pause1, FlushEx, Discontinuity) stream can change format";
            goto done;
        }
        if (aliases_headers(r->argument, sizeof(words), internal) ||
            overlap(r->argument, sizeof(words), CURVE, 16u)) {
            r->error = "SetFormat input aliases protected state";
            goto done;
        }
        /* An unreadable input leaves the zero fill, which is never an XADPCM format.
         * T1159, T1181: the original validates nothing, so the family is policy. XADPCM is tag 0x69, 4 bits, cbSize 2, 64 samples per
         * block, 36 bytes per channel per block, average bytes per second = rate * block / 64 (floored), a rate in the 8000 to 48000 Hz
         * range of the Xbox stream mixer. The channel count is the stream's own (stereo 2, spatial 1): a different count also changes
         * the voice format words (+0xC, +0x14, +0xEC, T602 oracle) the host record does not model. */
        const bool readable = kernel_guest_read_bytes(r->argument, words, sizeof(words));
        const uint32_t channels = stereo ? 2u : 1u, block = 36u * channels;
        uint32_t rate, average;
        memcpy(&rate, words + 4, 4);
        memcpy(&average, words + 8, 4);
        const bool family = words[0] == 0x69u && words[1] == 0u && words[2] == channels && words[3] == 0u &&
            rate >= 8000u && rate <= 48000u && average == (uint32_t)((uint64_t)rate * block / 64u) &&
            words[12] == block && words[13] == 0u && words[14] == 4u && words[15] == 0u &&
            words[16] == 2u && words[17] == 0u && words[18] == 64u && words[19] == 0u;
        if (!readable || !family) {
            r->error = "only an ADPCM format of the stream's channel count (block 36 per channel, 64 samples, average rate*block/64, 8000 to 48000 Hz) at a readable address is supported";
            goto done;
        }
        /* Original copies settings, resets pitch and programs the APU. The
         * optional PCM/completion abort transaction changes source time;
         * guest settings/voice writes and hardware remain omitted. */
        int32_t format_pitch=n->value.frequency_pitch;
        if(n->value.frequency_sets!=0u || (completion && format_note!=NULL)) {
            uint16_t cw=0u;
            const bool known=format_note!=NULL && frequency_control_word!=NULL && frequency_control_word(&cw);
            const bool pitch_valid=known && dsound_stream_frequency_pitch(rate,cw,&format_pitch);
            if(!pitch_valid || !format_note(r->address,n->value.lease.serial,n->value.source_rate_hz,rate,r->argument)) {
                dsound_hle_log()("dsound: format stream=%#x old=%u new=%u cw=%#x known=%u pitch-valid=%u\n",
                    r->address,n->value.source_rate_hz,rate,(unsigned)cw,known?1u:0u,pitch_valid?1u:0u);
                r->error="SetFormat PCM/completion abort/reset rejected ownership, clock, words or guest CW";goto done;
            }
        }
        memcpy(n->value.format, words, sizeof(words));
        n->value.frequency_pitch=format_pitch;
        n->value.format_sets++;
        n->value.source_rate_hz=rate;
        *result = 0u;
        goto done;
    }
    if (r->entry == VOLUME) {
        bool spatial = n->value.scope.flags == 0x10u && n->value.cache_mask == 7u;
        bool stereo = n->value.scope.flags == 0u && n->value.cache_mask == 0u &&
            n->value.scope.max_packets == 3u && n->value.scope.sample_rate == 44100u &&
            n->value.scope.channels == 2u && n->value.scope.block_align == 72u &&
            n->value.scope.average_bytes_per_second == 49612u;
        bool fresh = !n->value.pause_seen && !n->value.flush_seen && !n->value.discontinuity_seen;
        bool paused = stereo && n->value.pause_seen && n->value.pause_mode == 1u &&
            n->value.flush_seen && n->value.flush_time_low == 0u &&
            n->value.flush_time_high == 0u && n->value.flush_flags == 1u &&
            n->value.discontinuity_seen;
        /* T681: with the completion model on, the title's per frame stream update (sub_00029F30, caller 0x29F68)
         * sets the volume it computes (-1137 and every other value), so a started stream takes ANY value and the
         * model records it. Only a stream that never started keeps the startup silence rule. */
        const bool update = completion && (spatial || stereo) && n->value.discontinuity_seen && n->value.volume_seen;
        if (!update && ((!spatial && !stereo) || (!fresh && !paused) || r->argument != 0xFFFFD8F0u)) {
            r->error = "only fresh or fully paused/flushed/discontinued startup silence is supported";
            goto done;
        }
        if (r->count == 1u && n->value.format_sets == 0u) {
            r->error = "SetVolume after SetFormat needs a recorded SetFormat";
            goto done;
        }
        dsound_stream_routing candidate=n->value.routing;
        const int32_t new_volume=update?(int32_t)r->argument:-10000;
        dsound_stream_routing_volume(&candidate,(uint32_t)new_volume);
        if (routing_note != NULL && (stereo || spatial) &&
            !routing_note(r->address,n->value.lease.serial,&candidate)) {
            r->error="stream PCM volume/routing transaction rejected";goto done;
        }
        n->value.routing=candidate;
        /* Original settings values are represented in host-owned state; real
         * PCM uses them under the explicit hook, APU writes remain omitted. */
        n->value.volume_seen = true;
        n->value.volume = update ? (int32_t)r->argument : -10000;
        *result = 0u;
        goto done;
    }
    if (r->entry == DISCONTINUITY) {
        bool stereo = n->value.scope.flags == 0u && n->value.cache_mask == 0u;
        bool spatial = n->value.scope.flags == 0x10u && n->value.cache_mask == 7u;
        if ((!stereo && !spatial) ||
            !n->value.pause_seen || n->value.pause_mode > 1u ||
            !n->value.flush_seen || n->value.flush_time_low != 0u ||
            n->value.flush_time_high != 0u || n->value.flush_flags != 1u) {
            static char detail[192];
            snprintf(detail, sizeof detail, "only empty paused/flushed startup Discontinuity is supported (flags %#x mask %u pause %u/%u flush %u time %u/%u flags %u seen %u)",
                     n->value.scope.flags, n->value.cache_mask, n->value.pause_seen, n->value.pause_mode, n->value.flush_seen,
                     n->value.flush_time_low, n->value.flush_time_high, n->value.flush_flags, n->value.discontinuity_seen);
            r->error = detail;
            goto done;
        }
        /* T1198: measured on the original (spatial and stereo startup streams): Discontinuity is S_OK in pause mode 0 and 1, with or
         * without queued packets, writes no APU register and leaves GetStatus unchanged (only a first call with packets sets one
         * byte of an omitted packet object). Repeated calls are no-ops.
         * HOST request observation only. Original 0x40733B examines omitted
         * voice queue state; this facade has no packet admission. No queue,
         * voice discontinuity flag, callback or completion is synthesized. */
        n->value.discontinuity_seen = true;
        *result = 0u;
        goto done;
    }
    if (r->entry == FLUSH_EX) {
        bool stereo = n->value.scope.flags == 0u && n->value.cache_mask == 0u;
        bool spatial = n->value.scope.flags == 0x10u && n->value.cache_mask == 7u;
        if ((!stereo && !spatial) ||
            !n->value.pause_seen || n->value.pause_mode != 1u ||
            r->argument != 0u || r->count != 0u || r->apply != 1u) {
            r->error = "only empty paused startup FlushEx time0/flags1 is supported";
            goto done;
        }
        /* HOST request metadata only. No Process or packet-admission method is
         * supported by this facade; adopting one requires revisiting this empty
         * scope before FlushEx can continue. No timing, callbacks, voice flags,
         * hardware flush or packet completion is synthesized here. */
        n->value.flush_seen = true;
        n->value.flush_time_low = r->argument;
        n->value.flush_time_high = r->count;
        n->value.flush_flags = r->apply;
        *result = 0u;
        goto done;
    }
    if (r->entry == PAUSE) {
        /* Original 29EA0 reaches 29ED1 with mode1 on the live stream.
         * Its additional caller admission requires the completion-owned started state. */
        if (r->count == 2u && (!completion || pause_note == NULL || !n->value.discontinuity_seen || r->argument != 1u)) {
            r->error = "runtime Pause caller needs completion transport, a started stream and mode1";
            goto done;
        }
        bool stereo = n->value.scope.flags == 0u && n->value.cache_mask == 0u;
        bool spatial = n->value.scope.flags == 0x10u && n->value.cache_mask == 7u;
        /* T681: a started stream (Discontinuity recorded) can also be paused and resumed (mode 0) by the title. */
        const bool resume_ok = completion && n->value.discontinuity_seen && (r->argument == 0u || r->argument == 1u);
        if ((!stereo && !spatial) || (r->argument != 1u && !resume_ok)) {
            r->error = "only measured stereo or fully configured spatial startup Pause mode1 is supported";
            goto done;
        }
        if (r->count == 1u && n->value.format_sets == 0u) {
            r->error = "Pause after SetFormat needs a recorded SetFormat";
            goto done;
        }
        /* Host request observation only. Original wrapper reads stream+0x24
         * and changes the omitted voice flags through 0x40BF18/0x40D09A.
         * No voice, hardware, packet or playback state is synthesized here. */
        n->value.pause_seen = true;
        n->value.pause_mode = r->argument;
        if (pause_note != NULL) pause_note(r->address, r->argument);
        *result = 0u;
        goto done;
    }
    if(r->entry==MAX_DISTANCE) {
        if(n->value.scope.flags!=0x10u || n->value.cache_mask!=7u) {
            r->error="only a fully configured spatial startup stream may record SetMaxDistance";goto done;
        }
        if(r->apply!=1u) {
            r->error="SetMaxDistance apply=0 enters the unmodeled settings commit helper";goto done;
        }
        /* The original writes the exact float word to omitted settings +0x3c, then sets
         * dirty byte +2. It leaves the guest stream/device headers unchanged and the
         * title's two admitted callers both pass apply=1. This is a host observation only. */
        n->value.max_distance_seen=true;n->value.max_distance_bits=r->argument;
        *result=0u;goto done;
    }
    if(r->entry==SET_POSITION) {
        /* T1182: the title's three callers pass apply=1. The original writes only the omitted settings object and its dirty byte in that
         * mode, apply=0 enters the commit helper 0x406E90 and the APU chain. Raw host record on a fully configured spatial stream. */
        if(n->value.scope.flags!=0x10u || n->value.cache_mask!=7u) {
            r->error="only a fully configured spatial startup stream may record SetPosition";goto done;
        }
        if(r->apply!=1u) {
            r->error="SetPosition apply=0 enters the unmodeled settings commit helper";goto done;
        }
        n->value.position_bits[0]=r->argument;n->value.position_bits[1]=r->count;n->value.position_bits[2]=r->position_z;
        n->value.position_sets++;
        *result=0u;goto done;
    }
    if(r->entry==ROLLOFF && n->value.scope.flags==0x10u && n->value.cache_mask==7u && r->apply==0u && r->count==4u &&
       (r->argument==CURVE || r->argument==CURVE_NEAR || r->argument==CURVE_FAR)) {
        /* T1199: the sound update sub_00029E60 (returns 0x29E81, 0x29E90) switches a configured spatial stream between the three
         * 4 point curves. Measured on the original (spatial startup and started streams, paused or resumed): S_OK, a raw store of
         * the curve pointer, the count and the dirty byte in the omitted settings object, no APU write, no kernel call but the
         * critical section pair, public headers untouched. Host record of the pointer only. */
        n->value.curve_address=r->argument;n->value.curve_count=r->count;
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
        if(n->value.cache_mask!=3u || r->argument!=CURVE || r->count!=4u){r->error="only ordered startup curve VA/count4 is supported";goto done;}
        n->value.curve_address=r->argument;n->value.curve_count=r->count;n->value.cache_mask=7u;
    }
    *result=0u;
done:pthread_mutex_unlock(&lock);
}
/* Measured spatial setters check global failure before object/settings access. */
static uint32_t spatial_preflight(uint32_t entry)
{
    pthread_mutex_lock(&lock);
    const bool admitted=enabled;
    uint8_t irql=0u;const bool known=irql_provider!=NULL && irql_provider(&irql);
    pthread_mutex_unlock(&lock);
    if(!admitted || !known || irql!=0u)refuse(entry,"spatial setter needs explicit policy at known IRQL0");
    uint32_t state;
    if(!kernel_guest_read_u32(GLOBAL_STATE,&state))refuse(entry,"global audio state inaccessible");
    return state!=0u?0x80004005u:0u;
}
static uint32_t access_extended(uint32_t entry,uint32_t address,uint32_t argument,uint32_t count,uint32_t apply,uint32_t position_z)
{
    if(entry==SET_POSITION || entry==MAX_DISTANCE) {
        const uint32_t failure=spatial_preflight(entry);
        if(failure!=0u)return failure;
    }
    access_request r={0};r.address=address;r.entry=entry;r.argument=argument;r.count=count;r.apply=apply;r.position_z=position_z;
    if(!identify(address,&r.identity))refuse(entry,"stream is not owned by passive adapter");
    uint32_t result=0u;
    if(!dsound_device_with_owned_interface(r.identity.internal_address+8u,access_owned,&r,&result))
        refuse(entry,"parent SILENT device ownership/generation changed");
    if(r.error!=NULL)refuse(entry,r.error);return result;
}
static uint32_t access(uint32_t entry,uint32_t address,uint32_t argument,uint32_t count,uint32_t apply)
{return access_extended(entry,address,argument,count,apply,0u);}
uint32_t dsound_stream_set_position(uint32_t stream,uint32_t x,uint32_t y,uint32_t z,uint32_t apply)
{return access_extended(SET_POSITION,stream,x,y,apply,z);}
uint32_t dsound_stream_cache_i3dl2(uint32_t stream,uint32_t parameters,uint32_t apply)
{return access(I3DL2,stream,parameters,0u,apply);}
uint32_t dsound_stream_cache_min_distance(uint32_t stream,uint32_t bits,uint32_t apply)
{return access(MINIMUM,stream,bits,0u,apply);}
uint32_t dsound_stream_cache_max_distance(uint32_t stream,uint32_t bits,uint32_t apply)
{return access(MAX_DISTANCE,stream,bits,0u,apply);}
uint32_t dsound_stream_cache_rolloff(uint32_t stream,uint32_t curve,uint32_t count,uint32_t apply)
{return access(ROLLOFF,stream,curve,count,apply);}
uint32_t dsound_stream_cache_pause(uint32_t stream, uint32_t mode)
{return access(PAUSE,stream,mode,0u,0u);}
uint32_t dsound_stream_cache_flush_ex(uint32_t stream, uint32_t time_low,
                                      uint32_t time_high, uint32_t flags)
{return access(FLUSH_EX,stream,time_low,time_high,flags);}
uint32_t dsound_stream_cache_discontinuity(uint32_t stream)
{return access(DISCONTINUITY,stream,0u,0u,0u);}
uint32_t dsound_stream_get_startup_status(uint32_t stream,uint32_t output)
{return access(STATUS,stream,output,0u,0u);}
/* The original global failure precedes dereferencing object/input arguments. */
static uint32_t routing_preflight(uint32_t entry)
{
    pthread_mutex_lock(&lock);
    const bool admitted=enabled && completion && routing_note!=NULL;
    uint8_t irql=0u;
    const bool known=irql_provider!=NULL && irql_provider(&irql);
    pthread_mutex_unlock(&lock);
    if (!admitted || !known || irql!=0u)
        refuse(entry,"routing needs explicit PCM/completion policy at known IRQL0");
    uint32_t state;
    if (!kernel_guest_read_u32(GLOBAL_STATE,&state)) refuse(entry,"global audio state inaccessible");
    return state!=0u?0x80004005u:0u;
}
static uint32_t frequency_preflight(void)
{
    pthread_mutex_lock(&lock);
    const bool admitted=enabled && completion && frequency_note!=NULL;
    uint8_t irql=0u;const bool known=irql_provider!=NULL && irql_provider(&irql);
    pthread_mutex_unlock(&lock);
    if(!admitted || !known || irql!=0u)refuse(SET_FREQUENCY,"frequency needs explicit PCM/completion policy at IRQL0");
    uint32_t state;
    if(!kernel_guest_read_u32(GLOBAL_STATE,&state))refuse(SET_FREQUENCY,"global audio state inaccessible");
    return state!=0u?0x80004005u:0u;
}
uint32_t dsound_stream_set_frequency(uint32_t stream,uint32_t hertz,uint16_t control_word)
{
    const uint32_t failure=frequency_preflight();
    return failure!=0u?failure:access(SET_FREQUENCY,stream,hertz,control_word,0u);
}
uint32_t dsound_stream_set_headroom(uint32_t stream,uint32_t headroom)
{uint32_t failure=routing_preflight(HEADROOM);return failure!=0u?failure:access(HEADROOM,stream,headroom,0u,0u);}
uint32_t dsound_stream_set_mix_bins(uint32_t stream,uint32_t bins)
{uint32_t failure=routing_preflight(MIX_BINS);return failure!=0u?failure:access(MIX_BINS,stream,bins,0u,0u);}
uint32_t dsound_stream_set_mix_bin_volumes(uint32_t stream,uint32_t bins)
{uint32_t failure=routing_preflight(MIX_VOLUMES);return failure!=0u?failure:access(MIX_VOLUMES,stream,bins,0u,0u);}
uint32_t dsound_stream_cache_set_format(uint32_t stream,uint32_t format)
{return access(SET_FORMAT,stream,format,0u,0u);}
uint32_t dsound_stream_cache_volume(uint32_t stream, int32_t volume)
{return access(VOLUME,stream,(uint32_t)volume,0u,0u);}
bool dsound_stream_get_snapshot(uint32_t stream,dsound_stream_snapshot *output)
{
    if(output==NULL)return false;access_request r={0};dsound_stream_snapshot value;
    r.address=stream;r.observer=true;r.snapshot=&value;
    if(!identify(stream,&r.identity))return false;uint32_t result=0u;
    if(!dsound_device_with_owned_interface(r.identity.internal_address+8u,access_owned,&r,&result) || r.error!=NULL)return false;
    *output=value;return true;
}
typedef struct reset_request {uint32_t address;dsound_device_lease identity;stream_node *node;bool held;} reset_request;
static bool reset_prepare(const dsound_device_lease *candidate,void *userdata,dsound_device_lease_child *child)
{
    reset_request *r=userdata;pthread_mutex_lock(&lock);r->held=true;r->node=find(r->address);
    if(!token_equal(candidate,&r->identity) || !valid_node(r->node,&r->identity,candidate->internal_address))return false;
    *child=(dsound_device_lease_child){r->node->value.stream_heap,r->address,STREAM_BYTES};return true;
}
static void reset_abort(void *userdata)
{reset_request *r=userdata;r->held=false;pthread_mutex_unlock(&lock);}
static void reset_finalize(const dsound_device_lease *committed,void *userdata)
{
    reset_request *r=userdata;(void)committed;stream_node **p=&streams;
    while(*p!=r->node)p=&(*p)->next;*p=r->node->next;r->node->next=detached;detached=r->node;
    r->held=false;pthread_mutex_unlock(&lock);
}
static const dsound_device_lease_ops reset_ops={reset_prepare,reset_abort,reset_finalize};
static bool reset_streams(void)
{
    for(;;) {
        reset_request r={0};pthread_mutex_lock(&lock);
        if(streams!=NULL){r.address=streams->value.stream_address;r.identity=streams->value.lease;}
        pthread_mutex_unlock(&lock);if(r.address==0u)break;
        if(dsound_device_release_lease(&r.identity,&reset_ops,&r)!=DSOUND_LEASE_OK)return false;
        /* Detached nodes remain tracked if checked cleanup refuses. No fallible
         * guest cleanup runs under device lock or in finalize. */
        pthread_mutex_lock(&lock);stream_node **p=&detached;
        while(*p!=r.node)p=&(*p)->next;
        if(!valid_node(r.node,&r.identity,r.identity.internal_address) ||
           !guest_heap_destroy(r.node->value.stream_heap)){pthread_mutex_unlock(&lock);return false;}
        *p=r.node->next;free(r.node);pthread_mutex_unlock(&lock);
    }
    pthread_mutex_lock(&lock);
    while(detached!=NULL) {
        stream_node *n=detached;
        if(!valid_node(n,&n->value.lease,n->value.lease.internal_address) ||
           !guest_heap_destroy(n->value.stream_heap)){pthread_mutex_unlock(&lock);return false;}
        detached=n->next;free(n);
    }
    while(rollback!=NULL) {
        stream_node *n=rollback;
        /* Unleased rollback ownership is the exclusive private heap generation,
         * not a device reference. Never release a lease or destroy a replacement. */
        if(!guest_heap_valid(n->value.stream_heap) || !guest_heap_destroy(n->value.stream_heap)) {
            pthread_mutex_unlock(&lock);return false;
        }
        rollback=n->next;free(n);
    }
    announced=false;pthread_mutex_unlock(&lock);return true;
}
bool dsound_stream_reset_checked(void)
{
    /* Extension streams (T392 movie streams) lease the same device, so they go first. */
    pthread_mutex_lock(&lock);dsound_stream_reset_route_fn extension=reset_route;pthread_mutex_unlock(&lock);
    const bool movies=extension==NULL || extension();
    pthread_mutex_lock(&reset_lock);bool result=reset_streams();
    pthread_mutex_unlock(&reset_lock);return movies && result;
}
void dsound_stream_reset(void){(void)dsound_stream_reset_checked();}
/* `second` is a further measured caller inside sub_000299C0 (T602, the sound start re-formats a started stream), 0 for none.
 * `third` (T743) is a caller admitted ONLY with the completion model on: the stream update sub_00029F30 -> sub_00029EE0 starts
 * the stream (Pause(0) of the started stream after its first Process, return address 0x29F13). The original 0x407B23 never
 * reads its caller, the model needs the started state it already checks. 0 for none. */
static uint32_t frame_handler(void *context,uint32_t entry,uint32_t caller,uint32_t second,uint32_t third,unsigned count)
{
    const kernel_call_frame *frame=context;uint32_t actual,args[4];
    pthread_mutex_lock(&lock);dsound_stream_public_route_fn route=public_route;const bool third_admitted=completion;pthread_mutex_unlock(&lock);
    if(route!=NULL && frame!=NULL && kernel_guest_read_u32(frame->stack_ptr,&actual)){
        uint32_t extension;
        /* T392: an installed extension (the movie stream) may answer its own streams first. */
        if(route(entry,frame,actual,&extension))return extension;
    }
    if(frame==NULL || !kernel_guest_read_u32(frame->stack_ptr,&actual) ||
       (actual!=caller && (second==0u || actual!=second) && (!third_admitted || third==0u || actual!=third) &&
        !(third_admitted && entry==PAUSE && actual==0x29ED1u))) {
        /* The actual return address is named (T743), the text keeps its measured prefix. */
        static char reason[96];
        const bool have=frame!=NULL && kernel_guest_read_u32(frame->stack_ptr,&actual);
        (void)snprintf(reason,sizeof reason,"only measured startup caller is supported, got caller %#x",have?actual:0u);
        refuse(entry,reason);
    }
    for(unsigned i=0u;i<count;i++)if(!kernel_frame_arg(frame,i,&args[i]))refuse(entry,"unreadable argument");
    if(entry==CREATE)return dsound_stream_create(args[0],args[1]);
    if(entry==I3DL2)return dsound_stream_cache_i3dl2(args[0],args[1],args[2]);
    if(entry==MINIMUM)return dsound_stream_cache_min_distance(args[0],args[1],args[2]);
    if(entry==MAX_DISTANCE)return dsound_stream_cache_max_distance(args[0],args[1],args[2]);
    if(entry==SET_FORMAT)return dsound_stream_cache_set_format(args[0],args[1]);
    /* The second SetVolume caller is the one after Pause and Discontinuity of the re-format, so it needs a recorded SetFormat. */
    if(entry==VOLUME)return access(VOLUME,args[0],args[1],actual==second?1u:0u,0u);
    /* The second Pause caller is the one right after SetFormat, so it needs a recorded SetFormat. */
    if(entry==PAUSE)return access(PAUSE,args[0],args[1],actual==second?1u:(actual==0x29ED1u?2u:0u),0u);
    if(entry==FLUSH_EX)return dsound_stream_cache_flush_ex(args[0],args[1],args[2],args[3]);
    return dsound_stream_cache_rolloff(args[0],args[1],args[2],args[3]);
}
static uint32_t create_handler(void *c){return frame_handler(c,CREATE,0x29943u,0u,0u,2u);}
static uint32_t i3dl2_handler(void *c){return frame_handler(c,I3DL2,0x2998Au,0u,0u,3u);}
static uint32_t minimum_handler(void *c){return frame_handler(c,MINIMUM,0x29998u,0u,0u,3u);}
static uint32_t max_distance_handler(void *context)
{
    const kernel_call_frame *frame=context;uint32_t actual,args[3];
    if(frame==NULL || !kernel_guest_read_u32(frame->stack_ptr,&actual) ||
       (actual!=0x29ADAu && actual!=0x29D86u))
        refuse(MAX_DISTANCE,"only the two measured spatial startup/update callers are supported");
    for(unsigned i=0u;i<3u;i++)if(!kernel_frame_arg(frame,i,&args[i]))
        refuse(MAX_DISTANCE,"unreadable SetMaxDistance argument");
    return dsound_stream_cache_max_distance(args[0],args[1],args[2]);
}
static uint32_t set_position_handler(void *context)
{
    const kernel_call_frame *frame=context;uint32_t actual,args[5];
    if(frame==NULL || !kernel_guest_read_u32(frame->stack_ptr,&actual) ||
       (actual!=0x29ACAu && actual!=0x29E2Bu && actual!=0x29FADu))
        refuse(SET_POSITION,"only the three measured spatial stream SetPosition callers are supported");
    for(unsigned i=0u;i<5u;i++)if(!kernel_frame_arg(frame,i,&args[i]))
        refuse(SET_POSITION,"unreadable SetPosition argument");
    return dsound_stream_set_position(args[0],args[1],args[2],args[3],args[4]);
}
static uint32_t rolloff_handler(void *context)
{
    const kernel_call_frame *frame=context;uint32_t actual,args[4];
    if(frame==NULL || !kernel_guest_read_u32(frame->stack_ptr,&actual) ||
       (actual!=0x299A8u && actual!=0x29E81u && actual!=0x29E90u)) {
        static char reason[112];
        const bool have=frame!=NULL && kernel_guest_read_u32(frame->stack_ptr,&actual);
        (void)snprintf(reason,sizeof reason,"only measured startup caller is supported, got caller %#x",have?actual:0u);
        refuse(ROLLOFF,reason);
    }
    for(unsigned i=0u;i<4u;i++)if(!kernel_frame_arg(frame,i,&args[i]))refuse(ROLLOFF,"unreadable SetRolloffCurve argument");
    return dsound_stream_cache_rolloff(args[0],args[1],args[2],args[3]);
}
static uint32_t pause_handler(void *c){return frame_handler(c,PAUSE,0x29B5Au,0x29A51u,0x29F13u,2u);}
static uint32_t flush_ex_handler(void *c){return frame_handler(c,FLUSH_EX,0x29B69u,0u,0u,4u);}
static uint32_t set_format_handler(void *c){return frame_handler(c,SET_FORMAT,0x299FAu,0u,0u,2u);}
static uint32_t set_frequency_handler(void *context)
{
    const uint32_t failure=frequency_preflight();if(failure!=0u)return failure;
    const kernel_call_frame *frame=context;uint32_t actual,stream,hertz;uint16_t cw;
    if(frame==NULL || !kernel_guest_read_u32(frame->stack_ptr,&actual) || actual!=0x29B35u)
        refuse(SET_FREQUENCY,"only measured return address 0x29B35 is supported");
    if(!kernel_frame_arg(frame,0u,&stream) || !kernel_frame_arg(frame,1u,&hertz))
        refuse(SET_FREQUENCY,"unreadable frequency arguments");
    pthread_mutex_lock(&lock);
    const bool known=frequency_control_word!=NULL && frequency_control_word(&cw);
    pthread_mutex_unlock(&lock);
    if(!known)refuse(SET_FREQUENCY,"guest x87 control word unavailable");
    return access(SET_FREQUENCY,stream,hertz,cw,0u);
}
static uint32_t volume_handler(void *c){return frame_handler(c,VOLUME,0x29F68u,0x29A67u,0u,2u);}
/* Original routing wrappers are STDCALL2/RET8 and have no caller restriction;
 * the checked adapter instead proves live ownership and its PCM capability. */
static uint32_t routing_handler(void *context,uint32_t entry)
{
    const kernel_call_frame *frame=context;uint32_t stream,argument;
    const uint32_t failure=routing_preflight(entry);if (failure!=0u) return failure;
    if (!kernel_frame_arg(frame,0u,&stream) || !kernel_frame_arg(frame,1u,&argument))
        refuse(entry,"unreadable routing arguments");
    return access(entry,stream,argument,0u,0u);
}
static uint32_t headroom_handler(void *c){return routing_handler(c,HEADROOM);}
static uint32_t mix_bins_handler(void *c){return routing_handler(c,MIX_BINS);}
static uint32_t mix_volumes_handler(void *c){return routing_handler(c,MIX_VOLUMES);}
size_t dsound_stream_register(void)
{
    size_t count=0u;count+=dsound_hle_register(CREATE,create_handler)?1u:0u;
    count+=dsound_hle_register(I3DL2,i3dl2_handler)?1u:0u;
    count+=dsound_hle_register(MINIMUM,minimum_handler)?1u:0u;
    count+=dsound_hle_register(ROLLOFF,rolloff_handler)?1u:0u;
    count+=dsound_hle_register(PAUSE,pause_handler)?1u:0u;
    count+=dsound_hle_register(FLUSH_EX,flush_ex_handler)?1u:0u;
    /* Only with the policy on, so a flags-off boot keeps its exact registry (T602). */
    pthread_mutex_lock(&lock);const bool policy=enabled;pthread_mutex_unlock(&lock);
    if(policy)count+=dsound_hle_register(SET_FORMAT,set_format_handler)?1u:0u;
    if(policy)count+=dsound_hle_register(MAX_DISTANCE,max_distance_handler)?1u:0u;
    if(policy)count+=dsound_hle_register(SET_POSITION,set_position_handler)?1u:0u;
    pthread_mutex_lock(&lock);const bool real_frequency=policy && frequency_note!=NULL && frequency_control_word!=NULL;pthread_mutex_unlock(&lock);
    if(real_frequency)count+=dsound_hle_register(SET_FREQUENCY,set_frequency_handler)?1u:0u;
    pthread_mutex_lock(&lock);const bool real_routing=policy && routing_note!=NULL;pthread_mutex_unlock(&lock);
    if (real_routing) {
        count+=dsound_hle_register(HEADROOM,headroom_handler)?1u:0u;
        count+=dsound_hle_register(MIX_BINS,mix_bins_handler)?1u:0u;
        count+=dsound_hle_register(MIX_VOLUMES,mix_volumes_handler)?1u:0u;
    }
    count+=dsound_hle_register(VOLUME,volume_handler)?1u:0u;return count;
}
