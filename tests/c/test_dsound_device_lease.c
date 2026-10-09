/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

/* Callback checks can overlap the main thread's create/join checks. */
#undef CHECK
#undef CHECK_EQ_U32
static void check_threadsafe(bool passed,const char *expression,int line)
{
    __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED);
    if(!passed){__atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);
        fprintf(stderr,"line%d: %s\n",line,expression);}
}
#define CHECK(x) check_threadsafe((x),#x,__LINE__)
#define CHECK_EQ_U32(a,b) CHECK((uint32_t)(a)==(uint32_t)(b))

static bool fail_malloc;
void *__real_malloc(size_t bytes);
void *__wrap_malloc(size_t bytes)
{ if (fail_malloc) { fail_malloc=false; return NULL; } return __real_malloc(bytes); }

typedef struct context {
    pthread_mutex_t child_lock;
    dsound_device_lease_child child;
    uint32_t output, protect_reference;
    void *publication;
    unsigned prepared, aborted, finalized;
    bool releasing, held, fail_prepare, fail_after_alloc, wrong_size;
} context;
static void context_init(context *c,uint32_t output)
{ memset(c,0,sizeof(*c));pthread_mutex_init(&c->child_lock,NULL);c->output=output; }
static bool prepare(const dsound_device_lease *candidate,void *userdata,
                    dsound_device_lease_child *child)
{
    context *c=userdata;pthread_mutex_lock(&c->child_lock);c->held=true;c->prepared++;
    CHECK(candidate->serial!=0u);CHECK(guest_heap_valid(candidate->device_heap));
    if(c->fail_prepare)return false;
    if(!c->releasing && c->child.heap==0u) {
        c->child.heap=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);
        if(c->child.heap==0u)return false;
        c->child.address=guest_heap_alloc(c->child.heap,40u);c->child.bytes=40u;
        if(c->child.address==0u || c->fail_after_alloc)return false;
    }
    if(!c->releasing) {
        uint32_t old;
        if(!kernel_guest_read_u32(c->output,&old) || !kernel_guest_write_u32(c->output,old))return false;
        c->publication=kernel_guest_at(c->output,4u);if(c->publication==NULL)return false;
    }
    *child=c->child;if(c->wrong_size)child->bytes++;
    /* Controlled fault injection after the device's probe; real transactions
     * require quiescent mappings. This exercises the post-prepare abort path. */
    if(c->protect_reference!=0u)
        CHECK(mprotect((void *)(uintptr_t)c->protect_reference,4096u,PROT_READ)==0);
    return true;
}
static void abort_prepare(void *userdata)
{
    context *c=userdata;c->aborted++;
    if(c->protect_reference!=0u)
        CHECK(mprotect((void *)(uintptr_t)c->protect_reference,4096u,PROT_READ|PROT_WRITE)==0);
    if(!c->releasing && c->child.heap!=0u) {
        CHECK(guest_heap_destroy(c->child.heap));memset(&c->child,0,sizeof(c->child));
    }
    c->held=false;pthread_mutex_unlock(&c->child_lock);
}
static void finalize(const dsound_device_lease *token,void *userdata)
{
    context *c=userdata;c->finalized++;CHECK(c->held);CHECK(token->serial!=0u);
    /* All output permissions were checked in prepare; mappings remain quiescent.
     * Release only marks detached. Guest cleanup is checked AFTER lease returns. */
    if(!c->releasing)memcpy(c->publication,&c->child.address,4u);
    c->held=false;pthread_mutex_unlock(&c->child_lock);
}
static const dsound_device_lease_ops ops={prepare,abort_prepare,finalize};
static void unlocked(context *c)
{ CHECK(!c->held);CHECK(pthread_mutex_trylock(&c->child_lock)==0);pthread_mutex_unlock(&c->child_lock); }
static void cleanup(context *c)
{ if(c->child.heap!=0u){CHECK(guest_heap_destroy(c->child.heap));memset(&c->child,0,sizeof(c->child));} }
static uint32_t interface,internal;
static void setup(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();dsound_device_reset();
    dsound_device_set_fatal(catching_fatal);
    map_fixed(0x412000u,0x1000u);map_fixed(0x4A1000u,0x1000u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    interface=load(SCRATCH_DATA);internal=interface-8u;CHECK_EQ_U32(load(internal+4u),6u);
}
static void test_balancing(void)
{
    context a,b;context_init(&a,SCRATCH_DATA+16u);context_init(&b,SCRATCH_DATA+20u);
    dsound_device_lease ta,tb;CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&a,&ta),DSOUND_LEASE_OK);
    CHECK_EQ_U32(load(internal+4u),7u);CHECK_EQ_U32(load(a.output),a.child.address);unlocked(&a);
    context duplicate;context_init(&duplicate,0u);duplicate.child=a.child;duplicate.releasing=true;
    dsound_device_lease unused;
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&duplicate,&unused),DSOUND_LEASE_INVALID);
    CHECK_EQ_U32(load(internal+4u),7u);CHECK(guest_heap_valid(a.child.heap));unlocked(&duplicate);
    CHECK(!dsound_device_reset_checked());dsound_device_reset();CHECK_EQ_U32(load(internal+4u),7u);
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&b,&tb),DSOUND_LEASE_OK);
    CHECK(tb.serial>ta.serial);CHECK_EQ_U32(load(internal+4u),8u);
    CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);CHECK_EQ_U32(load(internal+4u),9u);
    a.releasing=true;b.releasing=true;
    a.fail_prepare=true;CHECK_EQ_U32(dsound_device_release_lease(&ta,&ops,&a),DSOUND_LEASE_PREPARE_FAILED);
    CHECK_EQ_U32(load(internal+4u),9u);unlocked(&a);a.fail_prepare=false;
    a.wrong_size=true;CHECK_EQ_U32(dsound_device_release_lease(&ta,&ops,&a),DSOUND_LEASE_INVALID);
    CHECK_EQ_U32(load(internal+4u),9u);unlocked(&a);a.wrong_size=false;
    CHECK_EQ_U32(dsound_device_release_lease(&ta,&ops,&a),DSOUND_LEASE_OK);cleanup(&a);
    CHECK_EQ_U32(load(internal+4u),8u);
    CHECK_EQ_U32(dsound_device_release_lease(&ta,&ops,&a),DSOUND_LEASE_INVALID);unlocked(&a);
    CHECK_EQ_U32(dsound_device_release_lease(&tb,&ops,&b),DSOUND_LEASE_OK);cleanup(&b);
    CHECK_EQ_U32(load(internal+4u),7u);CHECK_EQ_U32(a.finalized,2u);CHECK_EQ_U32(a.aborted,2u);
    CHECK(dsound_device_reset_checked());store(SCRATCH_DATA,0u);
    CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    interface=load(SCRATCH_DATA);internal=interface-8u;
    CHECK_EQ_U32(dsound_device_release_lease(&ta,&ops,&a),DSOUND_LEASE_INVALID);
    context c;context_init(&c,SCRATCH_DATA+24u);dsound_device_lease tc;
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&tc),DSOUND_LEASE_OK);
    CHECK(tc.serial>tb.serial);CHECK(tc.device_heap!=tb.device_heap);
    c.releasing=true;CHECK_EQ_U32(dsound_device_release_lease(&tc,&ops,&c),DSOUND_LEASE_OK);cleanup(&c);
}
static void test_refusals(void)
{
    context c;context_init(&c,SCRATCH_DATA+32u);dsound_device_lease token,before;
    memset(&before,0xA5,sizeof(before));token=before;store(c.output,0xDEADBEEFu);
    CHECK_EQ_U32(dsound_device_acquire_lease(interface+1u,&ops,&c,&token),DSOUND_LEASE_INVALID);
    CHECK(memcmp(&token,&before,sizeof(token))==0);CHECK_EQ_U32(c.prepared,0u);
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,NULL,&c,&token),DSOUND_LEASE_INVALID);
    fail_malloc=true;CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_OUT_OF_MEMORY);
    CHECK_EQ_U32(c.prepared,0u);CHECK_EQ_U32(load(internal+4u),6u);
    c.fail_prepare=true;CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_PREPARE_FAILED);
    c.fail_prepare=false;c.fail_after_alloc=true;
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_PREPARE_FAILED);
    c.fail_after_alloc=false;c.wrong_size=true;
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_INVALID);c.wrong_size=false;
    CHECK_EQ_U32(load(c.output),0xDEADBEEFu);CHECK(memcmp(&token,&before,sizeof(token))==0);
    CHECK_EQ_U32(load(internal+4u),6u);CHECK_EQ_U32(guest_mem_heap_count(),1u);unlocked(&c);
    for(unsigned i=0u;i<11u;i++) {
        const uint32_t old=load(internal+4u*i);store(internal+4u*i,old^1u);
        CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_INVALID);
        CHECK(memcmp(&token,&before,sizeof(token))==0);
        CHECK(!dsound_device_reset_checked());CHECK_EQ_U32(load(internal+4u*i),old^1u);
        store(internal+4u*i,old);
    }
    store(0x412B30u,internal^4u);CHECK(!dsound_device_reset_checked());
    CHECK_EQ_U32(load(0x412B30u),internal^4u);store(0x412B30u,internal);
    const uintptr_t page=(uintptr_t)internal&~(uintptr_t)4095u;
    CHECK(mprotect((void *)page,4096u,PROT_READ)==0);
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_WRITE_REFUSED);
    CHECK_EQ_U32(load(internal+4u),6u);CHECK(mprotect((void *)page,4096u,PROT_NONE)==0);
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_INVALID);
    CHECK(mprotect((void *)page,4096u,PROT_READ|PROT_WRITE)==0);
    map_fixed(0x21000000u,8192u);store(0x21000FFEu,0x12345678u);
    CHECK(mprotect((void *)0x21001000u,4096u,PROT_READ)==0);c.output=0x21000FFEu;
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_PREPARE_FAILED);
    CHECK_EQ_U32(load(c.output),0x12345678u);CHECK_EQ_U32(load(internal+4u),6u);unlocked(&c);
    CHECK(mprotect((void *)0x21001000u,4096u,PROT_READ|PROT_WRITE)==0);
    c.output=SCRATCH_DATA+32u;c.protect_reference=(uint32_t)page;
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_WRITE_REFUSED);
    CHECK_EQ_U32(load(c.output),0xDEADBEEFu);CHECK_EQ_U32(load(internal+4u),6u);
    CHECK(memcmp(&token,&before,sizeof(token))==0);unlocked(&c);c.protect_reference=0u;
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_OK);
    c.releasing=true;CHECK(mprotect((void *)page,4096u,PROT_READ)==0);
    CHECK_EQ_U32(dsound_device_release_lease(&token,&ops,&c),DSOUND_LEASE_WRITE_REFUSED);
    CHECK_EQ_U32(load(internal+4u),7u);CHECK(mprotect((void *)page,4096u,PROT_READ|PROT_WRITE)==0);
    c.protect_reference=(uint32_t)page;
    CHECK_EQ_U32(dsound_device_release_lease(&token,&ops,&c),DSOUND_LEASE_WRITE_REFUSED);
    CHECK_EQ_U32(load(internal+4u),7u);CHECK(guest_heap_valid(c.child.heap));unlocked(&c);
    c.protect_reference=0u;
    dsound_device_lease foreign=token;foreign.serial++;
    CHECK_EQ_U32(dsound_device_release_lease(&foreign,&ops,&c),DSOUND_LEASE_INVALID);
    CHECK_EQ_U32(dsound_device_release_lease(&token,&ops,&c),DSOUND_LEASE_OK);cleanup(&c);unlocked(&c);
}
static void protected_child_metadata(void)
{
    context c;context_init(&c,SCRATCH_DATA+64u);dsound_device_lease token;
    c.child.heap=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);
    CHECK(c.child.heap!=0u);CHECK(guest_heap_alloc(c.child.heap,4064u)!=0u);
    c.child.address=guest_heap_alloc(c.child.heap,40u);c.child.bytes=40u;
    CHECK_EQ_U32(c.child.address&4095u,0u);
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_OK);
    uint32_t metadata_page=(c.child.address-16u)&~4095u;
    CHECK(mprotect((void *)(uintptr_t)metadata_page,4096u,PROT_NONE)==0);
    uint32_t value;CHECK(kernel_guest_read_u32(c.child.address,&value));
    c.releasing=true;unsigned prepared=c.prepared;
    CHECK_EQ_U32(dsound_device_release_lease(&token,&ops,&c),DSOUND_LEASE_INVALID);
    CHECK_EQ_U32(c.prepared,prepared);CHECK_EQ_U32(load(internal+4u),7u);
    CHECK(mprotect((void *)(uintptr_t)metadata_page,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK_EQ_U32(dsound_device_release_lease(&token,&ops,&c),DSOUND_LEASE_OK);cleanup(&c);
    CHECK_EQ_U32(load(internal+4u),6u);unlocked(&c);
}
static void stale_child(void)
{
    context c;context_init(&c,SCRATCH_DATA+64u);dsound_device_lease token;
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_OK);
    uint32_t heap=c.child.heap,address=c.child.address;CHECK(guest_heap_destroy(heap));
    /* Reclaim the freed VA page BEFORE any fresh allocation. A new chunk's
     * MAP_32BIT placement is randomized per call and can land in the freed hole,
     * which made this map_fixed fail with 0xC0000018 about 0.2% of runs (T254). */
    map_fixed(address&~4095u,4096u);uint32_t replacement=address;
    uint32_t foreign=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);
    CHECK(foreign!=heap);CHECK(guest_heap_alloc(foreign,40u)!=0u);
    store(replacement,0xF00DBAAFu);c.releasing=true;
    CHECK_EQ_U32(dsound_device_release_lease(&token,&ops,&c),DSOUND_LEASE_INVALID);
    CHECK_EQ_U32(load(replacement),0xF00DBAAFu);CHECK_EQ_U32(load(internal+4u),7u);
    CHECK(!dsound_device_reset_checked());CHECK(guest_heap_valid(foreign));
}
static void stale_device(void)
{
    context c;context_init(&c,SCRATCH_DATA+64u);dsound_device_lease token;
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_OK);
    CHECK(guest_heap_destroy(token.device_heap));
    /* Same reclaim-before-allocate ordering as stale_child, for the same reason. */
    map_fixed(internal&~4095u,4096u);uint32_t replacement=internal;
    uint32_t foreign=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);
    CHECK(foreign!=token.device_heap);CHECK(guest_heap_alloc(foreign,44u)!=0u);
    store(replacement,0xF00DBAAFu);c.releasing=true;
    CHECK_EQ_U32(dsound_device_release_lease(&token,&ops,&c),DSOUND_LEASE_INVALID);
    CHECK(!dsound_device_reset_checked());dsound_device_reset();
    CHECK(guest_heap_valid(foreign));CHECK_EQ_U32(load(replacement),0xF00DBAAFu);
}
static void foreign_block_same_heap(void)
{
    context c;context_init(&c,SCRATCH_DATA+64u);dsound_device_lease token;
    CHECK_EQ_U32(dsound_device_acquire_lease(interface,&ops,&c,&token),DSOUND_LEASE_OK);
    c.releasing=true;CHECK_EQ_U32(dsound_device_release_lease(&token,&ops,&c),DSOUND_LEASE_OK);cleanup(&c);
    CHECK(guest_heap_free(token.device_heap,internal));
    uint32_t replacement=guest_heap_alloc(token.device_heap,44u);CHECK_EQ_U32(replacement,internal);
    store(replacement,0xF00DBAAFu);CHECK(!dsound_device_reset_checked());dsound_device_reset();
    CHECK(guest_heap_valid(token.device_heap));CHECK_EQ_U32(load(replacement),0xF00DBAAFu);
    CHECK_EQ_U32(load(0x412B30u),internal);
}
/* T254 regression. A heap chunk lands at a per-call randomized MAP_32BIT address.
 * When a test frees it and wants the same VA back via MAP_FIXED_NOREPLACE, any
 * dynamic allocation made in between can randomize into the freed hole and the
 * fixed request then fails with STATUS_CONFLICTING_ADDRESSES. The squat below is
 * that unlucky placement made deterministic. The second half pins the ordering
 * stale_child and stale_device use now: reclaim the page first, allocate after. */
static void reclaimed_hole_collision(void)
{
    uint32_t heap=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);CHECK(heap!=0u);
    uint32_t address=guest_heap_alloc(heap,40u);CHECK(address!=0u);
    const uint32_t page=address&~4095u;CHECK(guest_heap_destroy(heap));
    void *squat=mmap((void *)(uintptr_t)page,4096u,PROT_NONE,
                     MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    CHECK(squat==(void *)(uintptr_t)page);
    guest_region_request request;memset(&request,0,sizeof(request));
    request.bytes=4096u;request.protect=PAGE_READWRITE;request.state=MEM_COMMIT;
    request.fixed_base=page;nt_status status=STATUS_SUCCESS;
    CHECK_EQ_U32(guest_region_alloc(&request,&status),0u);
    CHECK_EQ_U32(status,STATUS_CONFLICTING_ADDRESSES);
    CHECK(munmap(squat,4096u)==0);
    /* The surviving order: nothing can take the hole between the destroy and the
     * reclaim, so the fixed map cannot lose. */
    heap=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);CHECK(heap!=0u);
    address=guest_heap_alloc(heap,40u);CHECK(address!=0u);
    const uint32_t page2=address&~4095u;CHECK(guest_heap_destroy(heap));
    map_fixed(page2,4096u);store(page2,0xF00DBAAFu);
    uint32_t foreign=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);
    CHECK(foreign!=0u);CHECK(guest_heap_alloc(foreign,40u)!=0u);
    CHECK_EQ_U32(load(page2),0xF00DBAAFu);
}
static void isolated(void (*test)(void))
{
    pid_t pid=fork();CHECK(pid>=0);if(pid==0){failures=0;test();fflush(stdout);_exit(failures?1:0);}
    if(pid>0){int status=0;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
}
typedef struct worker {context c;dsound_device_lease token;dsound_device_lease_status status;} worker;
static void *worker_acquire(void *data)
{worker *w=data;w->status=dsound_device_acquire_lease(interface,&ops,&w->c,&w->token);return NULL;}
static void test_concurrency(void)
{
    worker workers[8];pthread_t threads[8];
    for(unsigned i=0u;i<8u;i++){context_init(&workers[i].c,SCRATCH_DATA+128u+4u*i);
        CHECK(pthread_create(&threads[i],NULL,worker_acquire,&workers[i])==0);}
    for(unsigned i=0u;i<8u;i++){CHECK(pthread_join(threads[i],NULL)==0);CHECK_EQ_U32(workers[i].status,DSOUND_LEASE_OK);
        for(unsigned j=0u;j<i;j++)CHECK(workers[i].token.serial!=workers[j].token.serial);}
    CHECK_EQ_U32(load(internal+4u),14u);
    for(unsigned i=0u;i<8u;i++){workers[i].c.releasing=true;
        CHECK_EQ_U32(dsound_device_release_lease(&workers[i].token,&ops,&workers[i].c),DSOUND_LEASE_OK);
        cleanup(&workers[i].c);unlocked(&workers[i].c);}
    CHECK_EQ_U32(load(internal+4u),6u);
}
int main(void)
{
    setup();test_balancing();test_refusals();protected_child_metadata();isolated(stale_child);isolated(stale_device);isolated(foreign_block_same_heap);isolated(reclaimed_hole_collision);test_concurrency();
    CHECK(dsound_device_reset_checked());environment_end();
    printf("dsound device lease: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
