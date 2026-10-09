/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_listener.h"
#include "dsound_hle.h"
#include "kernel_call.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#define WORK 0x407B40u
#define COMMIT 0x409109u
#define DOPPLER 0x4093ECu
#define POSITION 0x40945Au
#define ORIENTATION 0x409410u
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static bool enabled,announced;
static dsound_listener_irql_provider irql_provider;
static dsound_listener_fatal_fn fatal_handler;
static dsound_listener_snapshot cache;
void dsound_listener_set_enabled(bool value){pthread_mutex_lock(&lock);enabled=value;pthread_mutex_unlock(&lock);}
void dsound_listener_set_irql_provider(dsound_listener_irql_provider value)
{pthread_mutex_lock(&lock);irql_provider=value;pthread_mutex_unlock(&lock);}
void dsound_listener_set_fatal(dsound_listener_fatal_fn value)
{pthread_mutex_lock(&lock);fatal_handler=value;pthread_mutex_unlock(&lock);}
void dsound_listener_reset(void)
{pthread_mutex_lock(&lock);memset(&cache,0,sizeof(cache));announced=false;pthread_mutex_unlock(&lock);}
static void refuse(uint32_t entry,const char *reason) __attribute__((noreturn));
static void refuse(uint32_t entry,const char *reason)
{
    pthread_mutex_lock(&lock);dsound_listener_fatal_fn fatal=fatal_handler;pthread_mutex_unlock(&lock);
    dsound_hle_log()("dsound passive listener %#x refused: %s\n",entry,reason);
    if(fatal!=NULL)fatal(entry,reason);
    abort();
}
static bool same(const dsound_device_identity *a,const dsound_device_identity *b)
{return a->device_heap==b->device_heap && a->internal_address==b->internal_address;}
typedef struct request {unsigned stage;uint32_t values[6],apply;const char *error;bool announce,valid;dsound_listener_snapshot *output;} request;
/* Device -> listener; no fatal/reentry or fallible guest mutation in callback. */
static void owned(const dsound_device_identity *identity,void *userdata,uint32_t *result)
{
    request *r=userdata;pthread_mutex_lock(&lock);
    if(cache.cache_mask!=0u && !same(identity,&cache.identity)){r->error="cached device incarnation changed";goto done;}
    if(r->output!=NULL){dsound_listener_snapshot value=cache;value.identity=*identity;*r->output=value;r->valid=true;goto done;}
    uint8_t irql;uint32_t global;
    if(!enabled){r->error="explicit headless-listener policy is disabled";goto done;}
    if(irql_provider==NULL || !irql_provider(&irql) || irql!=0u){r->error="only known IRQL0 supported";goto done;}
    if(!kernel_guest_read_u32(0x4124A8u,&global) || global!=0u){r->error="original global audio state must be zero";goto done;}
    if(r->stage==3u || r->stage==4u) {
        static const uint32_t basis[6]={0u,0u,0x3F800000u,0u,0x3F800000u,0u};
        static const uint32_t origin[3]={0u};
        if(cache.cache_mask!=7u || cache.doppler_bits!=0u ||
           memcmp(cache.position,origin,sizeof(origin))!=0 ||
           memcmp(cache.orientation,basis,sizeof(basis))!=0) {
            r->error="only completed exact startup listener commit request is supported";
            goto done;
        }
        /* HOST request observation only; no child commit/propagation is claimed. */
        if(r->stage==4u) {
            if(!cache.commit_seen){r->error="DoWork requires recorded listener commit";goto done;}
            /* HOST observation, never notification draining or child progress. */
            cache.work_seen=true;
        } else cache.commit_seen=true;
        r->valid=true;*result=0u;goto done;
    }
    const uint32_t expected_mask[]={0u,1u,3u};
    const uint32_t expected[3][6]={{0u},{0u},{0u,0u,0x3F800000u,0u,0x3F800000u,0u}};
    if(r->apply!=0u || cache.cache_mask!=expected_mask[r->stage] ||
       memcmp(r->values,expected[r->stage],sizeof(r->values))!=0){r->error="unsupported startup scalar bits, apply or order";goto done;}
    cache.identity=*identity;
    if(r->stage==0u)cache.doppler_bits=r->values[0];
    else if(r->stage==1u)memcpy(cache.position,r->values,sizeof(cache.position));
    else memcpy(cache.orientation,r->values,sizeof(cache.orientation));
    cache.cache_mask|=1u<<r->stage;r->valid=true;*result=0u;
    if(!announced){announced=true;r->announce=true;}
done:pthread_mutex_unlock(&lock);
}
static uint32_t invoke(uint32_t interface,uint32_t entry,request *r)
{
    uint32_t result=0u;
    if(!dsound_device_with_owned_identity(interface,owned,r,&result))refuse(entry,"unowned or changed device");
    if(r->error!=NULL)refuse(entry,r->error);
    if(r->announce)dsound_hle_log()("dsound explicit passive listener CPU cache; omitted guest settings/derived3D/voice-list/APU/DSP/FP/critical-section effects; no audio\n");
    return result;
}
uint32_t dsound_listener_cache_doppler(uint32_t interface,uint32_t factor,uint32_t apply)
{request r={.stage=0u,.values={factor},.apply=apply};return invoke(interface,DOPPLER,&r);}
uint32_t dsound_listener_cache_position(uint32_t interface,uint32_t x,uint32_t y,uint32_t z,uint32_t apply)
{request r={.stage=1u,.values={x,y,z},.apply=apply};return invoke(interface,POSITION,&r);}
uint32_t dsound_listener_cache_orientation(uint32_t interface,uint32_t fx,uint32_t fy,uint32_t fz,
                                         uint32_t tx,uint32_t ty,uint32_t tz,uint32_t apply)
{request r={.stage=2u,.values={fx,fy,fz,tx,ty,tz},.apply=apply};return invoke(interface,ORIENTATION,&r);}
uint32_t dsound_listener_cache_commit(uint32_t interface)
{request r={.stage=3u};return invoke(interface,COMMIT,&r);}
uint32_t dsound_listener_cache_work(void)
{
    uint32_t internal;
    if(!kernel_guest_read_u32(0x412B30u,&internal) || internal==0u || internal>UINT32_MAX-8u)
        refuse(WORK,"unavailable or invalid device singleton");
    request r={.stage=4u};return invoke(internal+8u,WORK,&r);
}
bool dsound_listener_get_snapshot(uint32_t interface,dsound_listener_snapshot *output)
{
    if(output==NULL)return false;
    request r={.output=output};uint32_t result=0u;
    return dsound_device_with_owned_identity(interface,owned,&r,&result) && r.valid;
}
static uint32_t handler(void *context,uint32_t entry,uint32_t caller,unsigned count)
{
    const kernel_call_frame *frame=context;uint32_t actual,args[8];
    if(frame==NULL || (entry==WORK && ((uint64_t)frame->stack_ptr+4u>UINT64_C(0x100000000) ||
        (frame->stack_limit!=0u && (uint64_t)frame->stack_ptr+4u>frame->stack_limit))) || !kernel_guest_read_u32(frame->stack_ptr,&actual) || actual!=caller)refuse(entry,"unsupported caller");
    for(unsigned i=0u;i<count;i++)if(!kernel_frame_arg(frame,i,&args[i]))refuse(entry,"unreadable argument");
    if(entry==WORK)return dsound_listener_cache_work();
    if(entry==COMMIT)return dsound_listener_cache_commit(args[0]);
    if(entry==DOPPLER)return dsound_listener_cache_doppler(args[0],args[1],args[2]);
    if(entry==POSITION)return dsound_listener_cache_position(args[0],args[1],args[2],args[3],args[4]);
    return dsound_listener_cache_orientation(args[0],args[1],args[2],args[3],args[4],args[5],args[6],args[7]);
}
static uint32_t doppler_handler(void *c){return handler(c,DOPPLER,0x27B78u,3u);}
static uint32_t position_handler(void *c){return handler(c,POSITION,0x27B88u,5u);}
static uint32_t orientation_handler(void *c){return handler(c,ORIENTATION,0x27BA2u,8u);}
static uint32_t commit_handler(void *c){return handler(c,COMMIT,0x2887Fu,1u);}
static dsound_listener_work_route_fn work_route;
void dsound_listener_set_work_route(dsound_listener_work_route_fn route){work_route=route;}
static uint32_t work_handler(void *c)
{
    const kernel_call_frame *frame=c;uint32_t caller=0u;
    const bool have=frame!=NULL && kernel_guest_read_u32(frame->stack_ptr,&caller);
    /* T392: an installed route (the movie stream) may take DoWork from its own callers first. */
    if(work_route!=NULL && have && work_route(caller))return 0u;
    /* T605: the title's main loop (0x1CE43D, the other game state of sub_001CE400's switch) calls DoWork from a second site. */
    /* T844: the in game frame (game state 24, the default case, call 0x458B0 at 0x1CE56E) is a third: the same function, no argument. */
    if(have && caller==0x1CE573u)return handler(c,WORK,0x1CE573u,0u);
    /* T1156: the other three returns of sub_001CE420's game states (call 0x458B0 at 0x1CE4C5, 0x1CE505, 0x1CE5C3). The original DoWork reads no
       caller state (T844 oracle), the gates below are the same as at every other site, only these exact returns are admitted. */
    if(have && (caller==0x1CE4CAu || caller==0x1CE50Au || caller==0x1CE5C8u))return handler(c,WORK,caller,0u);
    return handler(c,WORK,have && caller==0x1CE45Fu?0x1CE45Fu:0x1CE492u,0u);
}
size_t dsound_listener_register(void)
{return (dsound_hle_register(DOPPLER,doppler_handler)?1u:0u)+(dsound_hle_register(POSITION,position_handler)?1u:0u)+(dsound_hle_register(ORIENTATION,orientation_handler)?1u:0u)+(dsound_hle_register(COMMIT,commit_handler)?1u:0u)+(dsound_hle_register(WORK,work_handler)?1u:0u);}
