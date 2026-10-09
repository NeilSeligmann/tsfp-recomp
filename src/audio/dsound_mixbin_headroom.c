/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_mixbin_headroom.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "kernel_call.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#define ENTRY 0x00407A2Cu
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static dsound_mixbin_pcm_note pcm_note;
static dsound_mixbin_bind_note bind_note;
static dsound_mixbin_irql_fn irql_provider;
static dsound_mixbin_fatal_fn fatal_handler;
static uint64_t current_identity;
static uint8_t current_amounts[32];
static void refuse(const char *reason) __attribute__((noreturn));
static void refuse(const char *reason)
{
 pthread_mutex_lock(&lock);dsound_mixbin_fatal_fn fatal=fatal_handler;pthread_mutex_unlock(&lock);
 dsound_hle_log()("dsound global mix-bin headroom refused: %s\n",reason);
 if(fatal!=NULL)fatal(ENTRY,reason);abort();
}
static bool bind_device(uint32_t heap,uint32_t internal)
{
 const uint64_t identity=((uint64_t)heap<<32u)|internal;
 pthread_mutex_lock(&lock);
 bool ok=bind_note!=NULL && bind_note(identity);
 if(ok){current_identity=identity;memset(current_amounts,1u,31u);current_amounts[31]=0u;}
 pthread_mutex_unlock(&lock);
 return ok;
}
void dsound_mixbin_headroom_configure(dsound_mixbin_pcm_note note,dsound_mixbin_bind_note bind,dsound_mixbin_irql_fn irql,
                                      dsound_mixbin_fatal_fn fatal)
{
 pthread_mutex_lock(&lock);pcm_note=note;bind_note=bind;irql_provider=irql;fatal_handler=fatal;
 current_identity=0u;memset(current_amounts,0,sizeof(current_amounts));pthread_mutex_unlock(&lock);
 dsound_device_set_create_note(note!=NULL?bind_device:NULL);
}
typedef struct request {uint32_t bin,headroom;const char *error;uint8_t *snapshot;} request;
static void owned(const dsound_device_identity *device,void *userdata,uint32_t *result)
{
 request *r=userdata;const uint64_t identity=((uint64_t)device->device_heap<<32u)|device->internal_address;
 pthread_mutex_lock(&lock);
 uint8_t candidate[32]={0};
 if(identity==current_identity)memcpy(candidate,current_amounts,sizeof(candidate));
 else memset(candidate,1u,31u);
 if(r->snapshot!=NULL){memcpy(r->snapshot,candidate,32u);*result=0u;goto done;}
 if(pcm_note==NULL){r->error="PCM policy inactive";goto done;}
 if(r->bin>=32u){r->error="bin outside original 32-bin submix range";goto done;}
 candidate[r->bin]=(uint8_t)r->headroom;
 if(!pcm_note(identity,r->bin,r->headroom)){
  r->error="PCM rejected clock/history/device lifetime or inactive renderer";goto done;
 }
 memcpy(current_amounts,candidate,32u);current_identity=identity;*result=0u;
done:pthread_mutex_unlock(&lock);
}
uint32_t dsound_mixbin_headroom_set(uint32_t interface,uint32_t bin,uint32_t headroom)
{
 pthread_mutex_lock(&lock);bool policy=pcm_note!=NULL;uint8_t irql=0u;
 bool known=irql_provider!=NULL && irql_provider(&irql);pthread_mutex_unlock(&lock);
 if(!policy || !known || irql!=0u)refuse("requires explicit PCM policy and known IRQL0");
 uint32_t global;
 if(!kernel_guest_read_u32(0x4124A8u,&global))refuse("global audio state inaccessible");
 if(global!=0u)return 0x80004005u;
 request r={.bin=bin,.headroom=headroom};uint32_t result=0u;
 if(!dsound_device_with_owned_identity(interface,owned,&r,&result))refuse("device ownership/header invalid");
 if(r.error!=NULL)refuse(r.error);return result;
}
bool dsound_mixbin_headroom_snapshot(uint32_t interface,uint8_t amounts[32])
{
 if(amounts==NULL)return false;request r={.snapshot=amounts};uint32_t result=0u;
 return dsound_device_with_owned_identity(interface,owned,&r,&result);
}
static uint32_t handler(void *context)
{
 const kernel_call_frame *frame=context;uint32_t args[3];
 for(unsigned i=0u;i<3u;i++)if(!kernel_frame_arg(frame,i,&args[i]))refuse("unreadable original stack argument");
 return dsound_mixbin_headroom_set(args[0],args[1],args[2]);
}
size_t dsound_mixbin_headroom_register(void)
{
 pthread_mutex_lock(&lock);bool active=pcm_note!=NULL;pthread_mutex_unlock(&lock);
 return active && dsound_hle_register(ENTRY,handler)?1u:0u;
}
