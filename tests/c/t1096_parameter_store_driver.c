/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include "game_replace.h"
__thread uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi, g_fs_base;
ptrdiff_t g_xbox_mem_offset;
#include "candidate.c"
#define LO 0x10000u
#define SIZE 0xff0000u
static unsigned char *memory;
static sigjmp_buf jump;
static volatile sig_atomic_t fault;
static void catch_fault(int signal) { fault = signal; siglongjmp(jump, 1); }
int review_init(void)
{
    unsigned char *allocation = mmap(NULL, SIZE + 8192u, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (allocation == MAP_FAILED) return 0;
    memory = allocation + 4096u;
    if (mprotect(memory, SIZE, PROT_READ | PROT_WRITE)) return 0;
    g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)memory - LO); return 1;
}
void review_prepare(const unsigned char *bytes, const uint32_t *regs, uint32_t page, int writable)
{
    mprotect(memory, SIZE, PROT_READ | PROT_WRITE); memcpy(memory, bytes, SIZE);
    g_eax=regs[0];g_ecx=regs[1];g_edx=regs[2];g_ebx=regs[3];g_esp=regs[4];g_ebp=regs[5];g_esi=regs[6];g_edi=regs[7];
    if (page) mprotect(memory + page - LO, 4096u, writable ? PROT_READ : PROT_NONE);
}
void review_invoke(uint32_t *output)
{
    struct sigaction action={0}, old={0}; action.sa_handler=catch_fault; sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &old); fault=0;
    if (!sigsetjmp(jump, 1)) sub_0009E2D0();
    output[0]=g_eax;output[1]=g_ecx;output[2]=g_edx;output[3]=g_ebx;output[4]=g_esp;output[5]=g_ebp;output[6]=g_esi;output[7]=g_edi;output[8]=(uint32_t)fault;
    sigaction(SIGSEGV, &old, NULL); mprotect(memory, SIZE, PROT_READ | PROT_WRITE);
}
void *review_memory(void) { return memory; }
