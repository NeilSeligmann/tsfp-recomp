/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Batch runner for the LIFTED XMV kernels, driven by tests/test_xmv_kernels_lifted.py (T394).
 *
 * The harness gives every case as a full description of guest memory (regions, registers, esp), so
 * this file knows nothing about any one function: it maps the regions, loads the registers, calls the
 * lifted function and reports the registers and the final bytes of every region. Guest memory is
 * mapped through g_xbox_mem_offset as in production. The retail image sits read-only at its real
 * addresses so constant tables resolve, and a write to it faults exactly as it does in the oracle
 * unless the case lists a region over those pages (an image window), which is writable for that case.
 * Pages outside the case regions stay PROT_NONE, so an over-read or stray write faults.
 *
 * A fault (SIGSEGV), a call that leaves the extracted cut (generated stubs) and a runaway loop (5 s)
 * are reported as a status, never as a crash, so one bad case cannot hide the others.
 *
 * usage: runner IMAGE CASES RESULTS
 *   IMAGE   guest bytes of 0x00010000.. (the retail image)
 *   CASES   uint32 count, then per case: addr, eax ecx edx ebx esi edi ebp esp, 8 uint64 mm values,
 *           8 xmm values (2 uint64 each), nregions, then per region: base, size (multiple of
 *           4096), size bytes
 *   RESULTS per case: uint32 status (0 ok, 1 fault, 2 left the cut, 3 timeout), fault address, stub
 *           address, eax ecx edx ebx esi edi ebp esp, then the bytes of every region in order. */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>

#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "xmv_functions.inc"

ptrdiff_t g_xbox_mem_offset;
__thread uint32_t g_eax, g_ecx, g_edx, g_esp;
__thread uint32_t g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp, g_fs_base;
__thread int g_df;
__thread RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
__thread RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
uint32_t g_xbox_code_lo, g_xbox_code_hi;

#define IMAGE_LOW 0x00010000u
#define WINDOW_HIGH 0x21000000u
#define PAGE 4096u

enum { OK = 0, FAULT = 1, LEFT_CUT = 2, TIMEOUT = 3 };

static sigjmp_buf g_escape;
static volatile int g_status;
static volatile uint32_t g_fault_address, g_stub_address;

static uint8_t *host_of(uint32_t guest) { return (uint8_t *)(uintptr_t)(guest + g_xbox_mem_offset); }

static void fail(const char *what)
{
    fprintf(stderr, "runner: %s\n", what);
    exit(2);
}

static void on_signal(int number, siginfo_t *info, void *context)
{
    (void)context;
    if (number == SIGALRM) {
        g_status = TIMEOUT;
    } else {
        g_status = FAULT;
        g_fault_address = (uint32_t)((uintptr_t)info->si_addr - (uintptr_t)g_xbox_mem_offset);
    }
    siglongjmp(g_escape, 1);
}

/* Every call out of the extracted cut lands here (the generated stubs and the runtime hooks). */
void runner_left_cut(uint32_t address)
{
    g_status = LEFT_CUT;
    g_stub_address = address;
    siglongjmp(g_escape, 1);
}

recomp_func_t recomp_lookup(uint32_t va) { runner_left_cut(va); return 0; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { runner_left_cut(va); return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
void recomp_call_safepoint(uint32_t target) { (void)target; }
void recomp_unimpl(const char *text, uint32_t va) { (void)text; runner_left_cut(va); }
void recomp_unimpl_trap(const char *text, uint32_t va) { (void)text; runner_left_cut(va); }
void recomp_flags_unresolved_trap(const char *cc, uint32_t va) { (void)cc; runner_left_cut(va); }
void recomp_icall_unresolved_trap(uint32_t va, const char *kind) { (void)kind; runner_left_cut(va); }

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) fail("cannot open input");
    fseek(file, 0, SEEK_END);
    *size = (size_t)ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = malloc(*size + 1);
    if (!bytes || fread(bytes, 1, *size, file) != *size) fail("short read");
    fclose(file);
    return bytes;
}

static recomp_func_t find(uint32_t address)
{
#define X(va, fn) if (address == (va)) return fn;
    XMV_FUNCTIONS
#undef X
    return 0;
}

static uint32_t take(const uint8_t **cursor)
{
    uint32_t value;
    memcpy(&value, *cursor, 4);
    *cursor += 4;
    return value;
}

int main(int argc, char **argv)
{
    if (argc != 4) fail("usage: runner IMAGE CASES RESULTS");
    size_t image_size, cases_size;
    uint8_t *image = slurp(argv[1], &image_size);
    uint8_t *cases = slurp(argv[2], &cases_size);
    FILE *results = fopen(argv[3], "wb");
    if (!results) fail("cannot open results");

    size_t window = WINDOW_HIGH - IMAGE_LOW;
    uint8_t *base = mmap(NULL, window, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) fail("mmap");
    g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)base - IMAGE_LOW);
    size_t mapped = (image_size + PAGE - 1) & ~(size_t)(PAGE - 1);
    if (mprotect(host_of(IMAGE_LOW), mapped, PROT_READ | PROT_WRITE) != 0) fail("mprotect image");
    memcpy(host_of(IMAGE_LOW), image, image_size);
    if (mprotect(host_of(IMAGE_LOW), mapped, PROT_READ) != 0) fail("protect image");

    static uint8_t alternate[1 << 16];
    stack_t stack = {.ss_sp = alternate, .ss_size = sizeof alternate, .ss_flags = 0};
    sigaltstack(&stack, NULL);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_signal;
    action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGALRM, &action, NULL);

    const uint8_t *cursor = cases;
    uint32_t count = take(&cursor);
    for (uint32_t index = 0; index < count; index++) {
        uint32_t address = take(&cursor);
        uint32_t regs[8];
        for (int slot = 0; slot < 8; slot++) regs[slot] = take(&cursor);
        uint64_t vectors[8 + 16];
        memcpy(vectors, cursor, sizeof vectors);
        cursor += sizeof vectors;
        uint32_t region_count = take(&cursor);
        uint32_t bases[16], sizes[16];
        if (region_count > 16) fail("too many regions");
        for (uint32_t region = 0; region < region_count; region++) {
            bases[region] = take(&cursor);
            sizes[region] = take(&cursor);
            if (mprotect(host_of(bases[region]), sizes[region], PROT_READ | PROT_WRITE) != 0) fail("mprotect region");
            memcpy(host_of(bases[region]), cursor, sizes[region]);
            cursor += sizes[region];
        }
        recomp_func_t function = find(address);
        if (!function) fail("function is not in the table");

        g_status = OK;
        g_fault_address = g_stub_address = 0;
        g_eax = regs[0], g_ecx = regs[1], g_edx = regs[2], g_ebx = regs[3];
        g_esi = regs[4], g_edi = regs[5], g_ebp = regs[6], g_esp = regs[7];
        g_df = 0;
        g_mm0.q = vectors[0], g_mm1.q = vectors[1], g_mm2.q = vectors[2], g_mm3.q = vectors[3];
        g_mm4.q = vectors[4], g_mm5.q = vectors[5], g_mm6.q = vectors[6], g_mm7.q = vectors[7];
        RecompXmm *wide[8] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3, &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
        for (int slot = 0; slot < 8; slot++) {
            wide[slot]->q[0] = vectors[8 + 2 * slot];
            wide[slot]->q[1] = vectors[9 + 2 * slot];
        }
        struct itimerval timer = {{0, 0}, {5, 0}};
        if (sigsetjmp(g_escape, 1) == 0) {
            setitimer(ITIMER_REAL, &timer, NULL);
            function();
        }
        struct itimerval stop = {{0, 0}, {0, 0}};
        setitimer(ITIMER_REAL, &stop, NULL);

        uint32_t reply[11] = {(uint32_t)g_status, g_fault_address, g_stub_address, g_eax, g_ecx, g_edx,
                              g_ebx, g_esi, g_edi, g_ebp, g_esp};
        fwrite(reply, sizeof reply, 1, results);
        for (uint32_t region = 0; region < region_count; region++) {
            fwrite(host_of(bases[region]), 1, sizes[region], results);
            if (bases[region] + sizes[region] <= IMAGE_LOW + mapped) {
                /* A window onto the image: put the pristine bytes back and make it read-only again. */
                memcpy(host_of(bases[region]), image + (bases[region] - IMAGE_LOW), sizes[region]);
                mprotect(host_of(bases[region]), sizes[region], PROT_READ);
                continue;
            }
            mprotect(host_of(bases[region]), sizes[region], PROT_NONE);
            madvise(host_of(bases[region]), sizes[region], MADV_DONTNEED);
        }
    }
    fclose(results);
    return 0;
}
