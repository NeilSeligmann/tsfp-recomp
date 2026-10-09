/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "host_runtime.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static unsigned checks,failures;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL line%d\n",__LINE__);}} while(0)
static host_run_scope outer=HOST_RUN_SCOPE_INITIALIZER,inner=HOST_RUN_SCOPE_INITIALIZER;
static host_stop original;
static volatile sig_atomic_t stages;
static void *guard;
static bool same_stop(const host_stop *a,const host_stop *b)
{return a->reason==b->reason && a->guest_address==b->guest_address && a->ordinal==b->ordinal &&
 a->fault_address==b->fault_address && a->signal_number==b->signal_number && a->detail==b->detail;}
static void nested(bool fault)
{
    stages=0;CHECK(host_run_scope_init(&outer));CHECK(host_run_scope_init(&inner));
    if(sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm();
        if(sigsetjmp(*host_run_scope_jmp(&outer),1)==0) {
            CHECK(host_run_scope_push(&outer));
            if(sigsetjmp(*host_run_scope_jmp(&inner),1)==0) {
                CHECK(host_run_scope_push(&inner));CHECK(host_run_scope_depth()==2u);
                CHECK(!host_run_scope_pop(&outer));CHECK(!host_run_scope_push(&inner));
                CHECK(!host_run_scope_init(&inner));CHECK(host_run_scope_jmp(&inner)==NULL);
                if(fault)*(volatile unsigned char *)guard=7u;
                else host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED,0x12345678u,77u,"retained original detail");
                CHECK(false);
            } else {
                stages=1;original=*host_run_result();CHECK(host_run_scope_depth()==2u);
                if(fault) {
                    CHECK(original.reason==HOST_STOP_FAULT);CHECK(original.signal_number==SIGSEGV);
                    CHECK(original.fault_address==(uintptr_t)guard);CHECK(original.guest_address==0u && original.ordinal==0u);
                } else CHECK(original.guest_address==0x12345678u && original.ordinal==77u);
                CHECK(host_run_scope_pop(&inner));host_run_rethrow(&original);
            }
        } else {
            CHECK(stages==1);stages=2;CHECK(host_run_scope_depth()==1u);
            CHECK(same_stop(host_run_result(),&original));CHECK(host_run_scope_pop(&outer));
            host_run_rethrow(host_run_result());
        }
    } else {
        CHECK(stages==2);stages=3;CHECK(host_run_scope_depth()==0u);
        CHECK(same_stop(host_run_result(),&original));
    }
    host_run_disarm();CHECK(stages==3);CHECK(!host_run_armed());
}
static host_run_scope many[HOST_RUN_SCOPE_MAX+1u];
static void bounds_and_normal_pop(void)
{
    CHECK(!host_run_scope_init(NULL));CHECK(!host_run_scope_push(NULL));CHECK(!host_run_scope_pop(NULL));
    CHECK(host_run_scope_jmp(NULL)==NULL);CHECK(!host_run_scope_push(&outer));
    if(sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm();
        for(unsigned i=0u;i<HOST_RUN_SCOPE_MAX+1u;i++) {
            CHECK(host_run_scope_init(&many[i]));
            if(sigsetjmp(*host_run_scope_jmp(&many[i]),1)!=0)abort();
            CHECK(host_run_scope_push(&many[i])==(i<HOST_RUN_SCOPE_MAX));
        }
        CHECK(host_run_scope_depth()==HOST_RUN_SCOPE_MAX);
        CHECK(!host_run_scope_pop(&many[0]));CHECK(!host_run_scope_push(&many[0]));
        for(unsigned i=HOST_RUN_SCOPE_MAX;i>0u;i--)CHECK(host_run_scope_pop(&many[i-1u]));
        CHECK(!host_run_scope_pop(&many[0]));CHECK(!host_run_scope_push(&many[0]));
        CHECK(host_run_scope_jmp(&many[0])==NULL);
        CHECK(host_run_scope_depth()==0u);host_run_stop(HOST_STOP_BUDGET,9u,10u,"root after normal pop");
    } else CHECK(host_run_result()->guest_address==9u);
    host_run_disarm();
}
static pthread_barrier_t barrier;
static pthread_mutex_t order_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t order_cond=PTHREAD_COND_INITIALIZER;
static bool first_finished;
typedef struct worker_data {host_run_scope scope;unsigned index;bool okay;} worker_data;
static worker_data workers[2];
static void *worker(void *data)
{
    worker_data *w=data;w->okay=host_run_scope_init(&w->scope);
    if(sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm();
        if(sigsetjmp(*host_run_scope_jmp(&w->scope),1)==0) {
            w->okay=w->okay && host_run_scope_push(&w->scope);
            (void)pthread_barrier_wait(&barrier);
            /* Other thread's active storage is stable until second barrier. */
            host_run_scope *other=&workers[1u-w->index].scope;
            w->okay=w->okay && !host_run_scope_init(other) && !host_run_scope_push(other) &&
                !host_run_scope_pop(other) && host_run_scope_jmp(other)==NULL;
            (void)pthread_barrier_wait(&barrier);
            if(w->index==1u) {
                pthread_mutex_lock(&order_lock);while(!first_finished)pthread_cond_wait(&order_cond,&order_lock);
                pthread_mutex_unlock(&order_lock);*(volatile unsigned char *)guard=9u;
            } else host_run_stop(HOST_STOP_KERNEL_UNIMPLEMENTED,0x1000u,33u,"worker first");
        } else {
            const host_stop *r=host_run_result();
            w->okay=w->okay && host_run_scope_depth()==1u &&
                (w->index==0u ? r->ordinal==33u : r->reason==HOST_STOP_FAULT && r->fault_address==(uintptr_t)guard);
            w->okay=w->okay && host_run_scope_pop(&w->scope);
        }
        host_run_disarm();
    } else w->okay=false;
    if(w->index==0u) {
        pthread_mutex_lock(&order_lock);first_finished=true;pthread_cond_broadcast(&order_cond);pthread_mutex_unlock(&order_lock);
    }
    return NULL;
}
static void threads(void)
{
    pthread_t ids[2];CHECK(pthread_barrier_init(&barrier,NULL,2u)==0);
    for(unsigned i=0u;i<2u;i++){workers[i].index=i;CHECK(pthread_create(&ids[i],NULL,worker,&workers[i])==0);}
    for(unsigned i=0u;i<2u;i++){CHECK(pthread_join(ids[i],NULL)==0);CHECK(workers[i].okay);}
    CHECK(pthread_barrier_destroy(&barrier)==0);CHECK(host_run_scope_depth()==0u);
}
static void disposition(int number){(void)number;}
static void failfast(bool arm_again)
{
    pid_t child=fork();CHECK(child>=0);
    if(child==0) {
        host_run_scope scope=HOST_RUN_SCOPE_INITIALIZER;
        if(!host_run_scope_init(&scope))_exit(2);
        if(sigsetjmp(*host_run_jmp(),1)!=0)_exit(3);
        host_run_arm();if(sigsetjmp(*host_run_scope_jmp(&scope),1)!=0)_exit(4);
        if(!host_run_scope_push(&scope))_exit(5);
        if(arm_again)host_run_arm();else host_run_disarm();_exit(6);
    }
    if(child>0){int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGABRT);}
}
int main(void)
{
    guard=mmap(NULL,4096u,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);
    const int signals[]={SIGSEGV,SIGBUS,SIGFPE,SIGILL};struct sigaction originals[4],custom,actual;
    memset(&custom,0,sizeof(custom));custom.sa_handler=disposition;sigemptyset(&custom.sa_mask);sigaddset(&custom.sa_mask,SIGUSR1);
    for(unsigned i=0u;i<4u;i++)CHECK(sigaction(signals[i],&custom,&originals[i])==0);
    nested(false);nested(true);bounds_and_normal_pop();threads();failfast(false);failfast(true);
    for(unsigned i=0u;i<4u;i++) {
        CHECK(sigaction(signals[i],NULL,&actual)==0);CHECK(actual.sa_handler==disposition);
        CHECK(sigismember(&actual.sa_mask,SIGUSR1)==1);CHECK(sigaction(signals[i],&originals[i],NULL)==0);
    }
    CHECK(munmap(guard,4096u)==0);printf("host stop scopes: %u checks, %u failures\n",checks,failures);return failures!=0u;
}
