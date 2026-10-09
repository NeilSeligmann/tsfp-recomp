/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "recomp_cooperative.h"
#include "host_runtime.h"
#include <stdlib.h>
static recomp_cooperative_provider configured_provider;
static void *configured_userdata;
static _Thread_local bool active;
extern int recomp_has_cooperative_calls(void) __attribute__((weak));
bool recomp_cooperative_ready(void)
{return recomp_has_cooperative_calls!=NULL && recomp_has_cooperative_calls()==1;}
bool recomp_cooperative_configure(recomp_cooperative_provider provider,void *userdata)
{
    if(active)return false;
    configured_provider=provider;configured_userdata=userdata;return true;
}
void recomp_call_safepoint(uint32_t callee)
{
    recomp_cooperative_provider provider=configured_provider;
    if(provider==NULL || active)return;
    if(!host_run_armed())abort();
    volatile host_run_scope scope=HOST_RUN_SCOPE_INITIALIZER;
    if(!host_run_scope_init(&scope))abort();
    if(sigsetjmp(*host_run_scope_jmp(&scope),0)==0) {
        if(!host_run_scope_push(&scope)) {
            host_run_stop(HOST_STOP_UNIMPLEMENTED,callee,0u,"cooperative stop scope unavailable");
            abort();
        }
        active=true;provider(callee,configured_userdata);
        active=false;if(!host_run_scope_pop(&scope))abort();
    } else {
        active=false;if(!host_run_scope_pop(&scope))abort();
        host_run_rethrow(host_run_result());
    }
}
