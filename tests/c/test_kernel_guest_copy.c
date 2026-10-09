/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "kernel_call.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
#define BASE 0x43000000u
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while (0)
static size_t page;
static atomic_bool stop;
static void *toggle(void *unused)
{
    (void)unused;
    while (!atomic_load(&stop)) {
        mprotect((void *)(uintptr_t)BASE,page,PROT_NONE);
        mprotect((void *)(uintptr_t)BASE,page,PROT_READ | PROT_WRITE);
        munmap((void *)(uintptr_t)BASE,page);
        void *mapped = mmap((void *)(uintptr_t)BASE,page,PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,-1,0);
        if (mapped == MAP_FAILED) return (void *)1;
    }
    return NULL;
}
int main(void)
{
    page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *memory = mmap((void *)(uintptr_t)BASE,3u*page,PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    CHECK(memory == (void *)(uintptr_t)BASE);
    if (memory != (void *)(uintptr_t)BASE) return 1;
    memset(memory,0xA5,3u*page);
    uint32_t word=0u;
    CHECK(kernel_guest_write_u32(BASE+1u,0x12345678u));
    CHECK(kernel_guest_read_u32(BASE+1u,&word) && word==0x12345678u);
    CHECK(kernel_guest_write_u32(BASE+(uint32_t)page-2u,0x11223344u));
    CHECK(kernel_guest_read_u32(BASE+(uint32_t)page-2u,&word) && word==0x11223344u);
    CHECK(mprotect(memory+page,page,PROT_NONE)==0);
    CHECK(kernel_guest_at(BASE+(uint32_t)page,4u)==NULL); /* PROT_NONE is refused (T81). */
    word=0xDEADBEEFu;
    CHECK(!kernel_guest_read_u32(BASE+(uint32_t)page,&word) && word==0xDEADBEEFu);
    CHECK(!kernel_guest_read_u32(BASE+(uint32_t)page-2u,&word) && word==0xDEADBEEFu);
    CHECK(!kernel_guest_write_u32(BASE+(uint32_t)page,1u));
    uint8_t byte=0xEFu;
    CHECK(!kernel_guest_read_u8(BASE+(uint32_t)page,&byte) && byte==0xEFu);
    CHECK(!kernel_guest_write_u8(BASE+(uint32_t)page,1u));
    uint8_t data[8]; memset(data,0x78,sizeof(data));
    CHECK(!kernel_guest_read_bytes(BASE+(uint32_t)page-2u,data,sizeof(data)));
    CHECK(!kernel_guest_write_bytes(BASE+(uint32_t)page-2u,data,sizeof(data)));
    /* Prefix transfer is permitted on failure; no atomic-write claim. */
    CHECK(mprotect(memory+page,page,PROT_READ)==0);
    CHECK(kernel_guest_read_u32(BASE+(uint32_t)page,&word));
    CHECK(!kernel_guest_write_u32(BASE+(uint32_t)page,1u));
    CHECK(memory[page]==0x22u && memory[page+1u]==0x11u);
    CHECK(munmap(memory+2u*page,page)==0);
    CHECK(!kernel_guest_read_u32(BASE+(uint32_t)(2u*page),&word));
    CHECK(!kernel_guest_read_u32(BASE+(uint32_t)(2u*page)-2u,&word));
    CHECK(!kernel_guest_at(BASE,SIZE_MAX));
    CHECK(!kernel_guest_read_bytes(BASE,data,SIZE_MAX));
    CHECK(!kernel_guest_write_bytes(0xFFFFFFFEu,data,4u));
    CHECK(!kernel_guest_read_bytes(0u,data,0u));
    CHECK(!kernel_guest_read_bytes(BASE,NULL,0u));
    CHECK(!kernel_guest_write_bytes(BASE,NULL,0u));
    CHECK(kernel_guest_read_bytes(1u,data,0u));
    CHECK(kernel_guest_write_bytes(1u,data,0u));
    CHECK(!kernel_guest_read_u32(BASE,NULL));
    CHECK(!kernel_guest_read_u8(BASE,NULL));
    pthread_t worker;
    CHECK(pthread_create(&worker,NULL,toggle,NULL)==0);
    for (unsigned i=0;i<20000u;i++) {
        word=0xDEADBEEFu;
        bool ok=kernel_guest_read_u32(BASE,&word);
        CHECK(ok || word==0xDEADBEEFu);
        (void)kernel_guest_write_u32(BASE,0x55667788u);
    }
    atomic_store(&stop,true);
    void *worker_result = NULL;
    CHECK(pthread_join(worker,&worker_result)==0 && worker_result==NULL);
    CHECK(munmap(memory,2u*page)==0);
    printf("guarded copy checks: %s\n",failures ? "FAIL" : "PASS");
    return failures!=0;
}
