/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "guest_mem.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); } } while (0)

static kernel_guest_ptr allocate(uint32_t bytes, uint32_t lowest, uint32_t highest, uint32_t alignment)
{
    const guest_region_request request = {.bytes=bytes,.lowest_physical=lowest,.highest_physical=highest,
        .alignment=alignment,.protect=PAGE_READWRITE,.state=MEM_COMMIT,.contiguous=true};
    nt_status status;
    const kernel_guest_ptr result=guest_region_alloc(&request,&status);
    CHECK(status == (result != 0u ? STATUS_SUCCESS : STATUS_NO_MEMORY));
    return result;
}

static void windows_and_holes(void)
{
    guest_mem_reset();
    const kernel_guest_ptr high=allocate(4096u,0x10000000u,UINT32_MAX,4096u);
    CHECK(high != 0u && guest_physical_address(high) >= 0x10000000u);
    const kernel_guest_ptr low=allocate(4096u,0u,0xFFFFFFu,4096u);
    CHECK(low != 0u && guest_physical_address(low) == 4096u);
    if(low == 0u) return;
    const kernel_guest_ptr a=allocate(4096u,0x2000u,0x7FFFu,4096u);
    const kernel_guest_ptr b=allocate(8192u,0x2000u,0x7FFFu,8192u);
    const kernel_guest_ptr c=allocate(4096u,0x2000u,0x7FFFu,4096u);
    CHECK(a != 0u && b != 0u && c != 0u);
    if(a == 0u || b == 0u || c == 0u) return;
    CHECK(guest_physical_address(a)==0x2000u && guest_physical_address(b)==0x4000u && guest_physical_address(c)==0x3000u);
    memset((void *)(uintptr_t)a,0xA1,4096u);memset((void *)(uintptr_t)c,0xC3,4096u);
    const uint64_t generation=guest_allocation_generation(b);
    CHECK(generation != 0u && guest_allocation_generation(b+8191u)==generation);
    CHECK(allocate(4096u,0x2000u,0x7FFFu,4096u) != 0u);
    CHECK(allocate(4096u,0x2000u,0x7FFFu,4096u) != 0u);
    const uint64_t mapped=guest_mem_mapped_bytes();const size_t regions=guest_mem_region_count();
    CHECK(allocate(4096u,0x2000u,0x7FFFu,4096u)==0u);
    CHECK(guest_mem_mapped_bytes()==mapped && guest_mem_region_count()==regions);
    CHECK(guest_region_free(b));
    const kernel_guest_ptr reused=allocate(4097u,0x2000u,0x7FFFu,8192u);
    CHECK(reused != 0u && guest_physical_address(reused)==0x4000u);
    CHECK(guest_allocation_generation(reused)>generation);
    kernel_guest_ptr inverse=0u;
    CHECK(guest_virtual_from_physical(0x5FFFu,&inverse) && inverse==reused+8191u);
    CHECK(*(const unsigned char *)(uintptr_t)a==0xA1 && *(const unsigned char *)(uintptr_t)c==0xC3);
    CHECK(allocate(8192u,0x2000u,0x7FFFu,8192u)==0u);
    CHECK(allocate(4096u,0u,0xFFFu,4096u)==0u);
    const kernel_guest_ptr upper=allocate(4096u,0xFFFFF000u,UINT32_MAX,4096u);
    CHECK(upper != 0u && guest_physical_address(upper)==0xFFFFF000u);
    CHECK(guest_virtual_from_physical(UINT32_MAX,&inverse) && inverse==upper+4095u);
    /* The 64-bit highwater can reach 2^32 without preventing a genuine free range. */
    CHECK(allocate(4096u,0u,0u,4096u) != 0u);
    guest_mem_reset();
}

static void exact_reuse_identity(void)
{
    const guest_region_request request={.bytes=4096u,.fixed_base=0x91000000u,
        .lowest_physical=0x90000u,.highest_physical=0x90FFFu,.alignment=4096u,
        .protect=PAGE_READWRITE,.state=MEM_COMMIT,.contiguous=true};
    nt_status status;
    const kernel_guest_ptr first=guest_region_alloc(&request,&status);
    CHECK(first==request.fixed_base && status==STATUS_SUCCESS);
    const uint64_t old=guest_allocation_generation(first);
    CHECK(old != 0u && guest_physical_address(first)==0x90000u);
    CHECK(guest_region_free(first));
    CHECK(guest_allocation_generation(first)==0u);
    const kernel_guest_ptr second=guest_region_alloc(&request,&status);
    CHECK(second==first && guest_physical_address(second)==0x90000u);
    const uint64_t next=guest_allocation_generation(second);
    CHECK(next>old);
    guest_mem_reset();
    const kernel_guest_ptr third=guest_region_alloc(&request,&status);
    CHECK(third==first && guest_physical_address(third)==0x90000u);
    CHECK(guest_allocation_generation(third)>next);
    CHECK(guest_allocation_generation(0u)==0u);
    guest_mem_reset();
}

#define THREADS 8u
#define WAVES 20u
static pthread_barrier_t barrier;
static kernel_guest_ptr live[THREADS];
static uint64_t identities[THREADS];
static nt_status statuses[THREADS];
static void *worker(void *context)
{
    const size_t index=(size_t)(uintptr_t)context;
    const guest_region_request request={.bytes=4096u,.lowest_physical=0x100000u,
        .highest_physical=0x107FFFu,.alignment=4096u,.protect=PAGE_READWRITE,
        .state=MEM_COMMIT,.contiguous=true};
    for(unsigned wave=0u;wave<WAVES;wave++) {
        live[index]=guest_region_alloc(&request,&statuses[index]);
        identities[index]=guest_allocation_generation(live[index]);
        (void)pthread_barrier_wait(&barrier);
        (void)pthread_barrier_wait(&barrier);
        (void)guest_region_free(live[index]);
        (void)pthread_barrier_wait(&barrier);
        (void)pthread_barrier_wait(&barrier);
    }
    return NULL;
}
static void concurrency(void)
{
    pthread_t threads[THREADS];
    CHECK(pthread_barrier_init(&barrier,NULL,THREADS+1u)==0);
    for(size_t i=0u;i<THREADS;i++) CHECK(pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)i)==0);
    uint64_t previous=0u;
    for(unsigned wave=0u;wave<WAVES;wave++) {
        (void)pthread_barrier_wait(&barrier);
        uint64_t maximum=previous;
        for(size_t i=0u;i<THREADS;i++) {
            CHECK(live[i] != 0u && statuses[i]==STATUS_SUCCESS && identities[i]>previous);
            const uint32_t physical=guest_physical_address(live[i]);
            CHECK(physical>=0x100000u && physical<=0x107000u && physical%4096u==0u);
            if(identities[i]>maximum) maximum=identities[i];
            for(size_t j=0u;j<i;j++) CHECK(identities[i]!=identities[j] && physical!=guest_physical_address(live[j]));
        }
        previous=maximum;
        (void)pthread_barrier_wait(&barrier);
        (void)pthread_barrier_wait(&barrier);
        CHECK(guest_mem_region_count()==0u);
        (void)pthread_barrier_wait(&barrier);
    }
    for(size_t i=0u;i<THREADS;i++) CHECK(pthread_join(threads[i],NULL)==0);
    CHECK(pthread_barrier_destroy(&barrier)==0);
}

int main(void)
{
    windows_and_holes();exact_reuse_identity();concurrency();guest_mem_reset();
    printf("T1016 physical: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
