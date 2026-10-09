/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Runs the LIFTED sub_00445AAF (format 0x24 YUY2 conversion) and its worker sub_004459B8 over guest
 * buffers laid out as tests/test_xmv_format24_original.py lays them out (T472).
 *
 * Guest memory is mapped through g_xbox_mem_offset exactly as production does. Every plane ends on
 * a page boundary followed by a PROT_NONE page so an over-read faults, and the destination, the
 * planes and the stack sit in pages poisoned with `fill` so a stray write is seen afterwards.
 *
 * stdin:  uint32 mb_cols, mb_rows, width, height, pitch, offset, fill, then Y, U, V bytes.
 * stdout: uint32 words (see Reply) then the destination bytes (pitch * height).
 * The planes have the decoder's macroblock-aligned sizes: 256 and 64 bytes per macroblock. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"

ptrdiff_t g_xbox_mem_offset;
__thread uint32_t g_eax, g_ecx, g_edx, g_esp;
__thread uint32_t g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
__thread int g_df; /* EFLAGS.DF model, clear on entry as the ABI requires */
__thread RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;

void sub_00445AAF(void);
void recomp_call_safepoint(uint32_t target) { (void)target; }

#define PAGE 4096u
#define ARENA_BASE 0x20000000u
#define ARENA_SIZE 0x01000000u
#define STACK_END 0x20910000u
#define STACK_SIZE 0x10000u
#define ARGUMENT_SLACK 0x1000u
#define FRAME_ALLOWANCE 0x280u
#define SENTINEL 0xDEADBEEFu
#define EBP_VALUE 0x2468ACE0u

typedef struct {
    uint32_t returned;      /* esp after the call minus esp before: ret 0x24 pops 40 */
    uint32_t ebx, esi, edi; /* values after the call */
    uint32_t ebp;           /* g_ebp after the call */
    uint32_t poison_bad;    /* poison bytes outside the buffers that changed */
    uint32_t planes_bad;    /* plane bytes that changed */
    uint32_t stack_bad;     /* stack bytes outside the frame that changed */
} Reply;

static uint8_t *host_of(uint32_t guest) { return (uint8_t *)(uintptr_t)(guest + g_xbox_mem_offset); }

static void fail(const char *what)
{
    fprintf(stderr, "runner: %s\n", what);
    exit(2);
}

static void read_exact(void *into, size_t size)
{
    if (fread(into, 1, size, stdin) != size) fail("short input");
}

/* Map the pages covering [start, end) of the arena read-write, filled with `fill`. */
static void map_region(uint32_t start, uint32_t end, uint8_t fill)
{
    uint32_t low = start & ~(PAGE - 1u);
    uint32_t high = (end + PAGE - 1u) & ~(PAGE - 1u);
    if (mprotect(host_of(low), high - low, PROT_READ | PROT_WRITE) != 0) fail("mprotect");
    memset(host_of(low), fill, high - low);
}

typedef struct {
    uint32_t start, size, page_low, page_high;
} Region;

static Region region(uint32_t start, uint32_t size)
{
    Region r = {start, size, start & ~(PAGE - 1u), (start + size + PAGE - 1u) & ~(PAGE - 1u)};
    return r;
}

static uint32_t count_changed(const Region *r, const uint8_t *before_fill, uint8_t fill)
{
    uint32_t bad = 0;
    for (uint32_t at = r->page_low; at < r->page_high; at++) {
        if (at >= r->start && at < r->start + r->size) continue;
        uint8_t expected = before_fill ? before_fill[at - r->page_low] : fill;
        if (*host_of(at) != expected) bad++;
    }
    return bad;
}

int main(void)
{
    uint32_t header[7];
    read_exact(header, sizeof header);
    uint32_t mb_cols = header[0], mb_rows = header[1], width = header[2], height = header[3];
    uint32_t pitch = header[4], offset = header[5];
    uint8_t fill = (uint8_t)header[6];
    uint32_t y_size = 256u * mb_cols * mb_rows, c_size = 64u * mb_cols * mb_rows;
    uint32_t dest_size = pitch * height;
    if (y_size > 0x100000u || dest_size > 0x300000u) fail("layout too large");

    uint8_t *arena = mmap(NULL, ARENA_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (arena == MAP_FAILED) fail("mmap");
    g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)arena - ARENA_BASE);

    /* Each buffer owns its pages. A plane ends on the page boundary, the next page stays PROT_NONE. */
    Region y = region(0x20200000u - y_size, y_size);
    Region u = region(0x20300000u - c_size, c_size);
    Region v = region(0x20400000u - c_size, c_size);
    Region dest = region(0x20500000u + offset, dest_size);
    Region stack = region(STACK_END - STACK_SIZE, STACK_SIZE);
    Region all[5] = {y, u, v, dest, stack};
    for (int index = 0; index < 5; index++) map_region(all[index].start, all[index].start + all[index].size, fill);
    map_region(stack.start, STACK_END, fill);

    uint8_t *y_in = malloc(y_size + 1), *u_in = malloc(c_size + 1), *v_in = malloc(c_size + 1);
    read_exact(y_in, y_size);
    read_exact(u_in, c_size);
    read_exact(v_in, c_size);
    memcpy(host_of(y.start), y_in, y_size);
    memcpy(host_of(u.start), u_in, c_size);
    memcpy(host_of(v.start), v_in, c_size);

    uint32_t esp0 = STACK_END - ARGUMENT_SLACK - 4u * 10u;
    uint32_t words[10] = {SENTINEL, mb_cols, mb_rows, y.start, u.start, v.start, width, height, dest.start, pitch};
    memcpy(host_of(esp0), words, sizeof words);
    uint8_t *stack_before = malloc(STACK_SIZE);
    memcpy(stack_before, host_of(stack.start), STACK_SIZE);

    g_esp = esp0;
    g_ebx = 0x13579BDFu;
    g_esi = 0xFEDCBA98u;
    g_edi = 0x89ABCDEFu;
    g_ebp = EBP_VALUE;
    g_eax = g_ecx = g_edx = 0x55AA55AAu;
    sub_00445AAF();

    Reply reply = {g_esp - esp0, g_ebx, g_esi, g_edi, g_ebp, 0, 0, 0};
    Region *buffers[4] = {&y, &u, &v, &dest};
    for (int index = 0; index < 4; index++) reply.poison_bad += count_changed(buffers[index], NULL, fill);
    for (uint32_t at = 0; at < y_size; at++) reply.planes_bad += host_of(y.start)[at] != y_in[at];
    for (uint32_t at = 0; at < c_size; at++) {
        reply.planes_bad += host_of(u.start)[at] != u_in[at];
        reply.planes_bad += host_of(v.start)[at] != v_in[at];
    }
    uint32_t frame_low = esp0 - FRAME_ALLOWANCE, args_high = esp0 + 4u * 10u;
    for (uint32_t at = stack.start; at < STACK_END; at++) {
        if (at >= frame_low && at < args_high) continue;
        reply.stack_bad += *host_of(at) != stack_before[at - stack.start];
    }
    fwrite(&reply, sizeof reply, 1, stdout);
    fwrite(host_of(dest.start), 1, dest_size, stdout);
    fflush(stdout);
    return 0;
}
