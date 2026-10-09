/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T505: a fault inside lifted code names where it happened.
 *
 * The lifted code is faked: `sub_00383DF3` calls `sub_00381000` calls `sub_00381100`, which
 * dereferences null, the shape of the T485 `RtlFreeHeap` fault. They are built at -O0 with
 * frame pointers like the real lifted functions (see CMakeLists.txt) so the handler's `rbp`
 * chain has one frame per call. The guest register file is faked too: the handler reads the
 * guest ebp through the registered reader, and the guest stack is a page mapped at its own
 * VA, as the real guest memory is.
 *
 * Pinned here: the handler records RIP, the host chain and the guest chain as plain stores,
 * survives a wild or looping guest ebp, a frameless leaf is covered by the stack top word,
 * a later non-fault stop does not inherit the evidence, and host_symbols names all of it.
 */
#define _GNU_SOURCE
#include "host_report.h"
#include "host_runtime.h"
#include "host_symbols.h"

#include <link.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static unsigned checks;
static unsigned failures;
#define CHECK(cond)                                                                        \
    do {                                                                                   \
        checks++;                                                                          \
        if (!(cond)) {                                                                     \
            failures++;                                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                         \
        }                                                                                  \
    } while (0)

#define GUEST_STACK 0x30000000u
#define WILD_EBP 0x7F000000u

static _Thread_local uint32_t fake_ebp;
static uint32_t read_fake_ebp(void)
{
    return fake_ebp;
}

static int *volatile null_pointer;
static size_t (*volatile leaf)(const char *) = strlen;
static volatile int leaf_fault;

__attribute__((noinline)) void sub_00381100(void)
{
    if (leaf_fault != 0) {
        (void)leaf((const char *)null_pointer);
    } else {
        *null_pointer = 1;
    }
}

__attribute__((noinline)) void sub_00381000(void)
{
    void (*volatile inner)(void) = sub_00381100;
    inner();
}

__attribute__((noinline)) void sub_00383DF3(void)
{
    void (*volatile inner)(void) = sub_00381000;
    inner();
}

/* A symbol that merely starts with a guest function's name is not a guest function. */
__attribute__((noinline)) void sub_00381F00_ext(void)
{
    void (*volatile inner)(void) = sub_00381100;
    inner();
}

static void put_frame(uint32_t ebp, uint32_t next_ebp, uint32_t return_address)
{
    uint32_t *frame = (uint32_t *)(uintptr_t)ebp;
    frame[0] = next_ebp;
    frame[1] = return_address;
}

/* Three guest frames innermost first: 381050 (in sub_00381000), 383E00 and 383E7F (both in
 * sub_00383DF3). The last one points at itself, which must end the walk. */
static void build_guest_chain(void)
{
    put_frame(GUEST_STACK + 0x100u, GUEST_STACK + 0x200u, 0x00381050u);
    put_frame(GUEST_STACK + 0x200u, GUEST_STACK + 0x300u, 0x00383E00u);
    put_frame(GUEST_STACK + 0x300u, GUEST_STACK + 0x300u, 0x00383E7Fu);
}

static host_stop fault_with(uint32_t ebp, bool leaf_kind)
{
    host_stop copy;
    fake_ebp = ebp;
    leaf_fault = leaf_kind ? 1 : 0;
    memset(&copy, 0, sizeof(copy));
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        sub_00383DF3();
        CHECK(false);
    }
    copy = *host_run_result();
    /* Still armed here: a later ordinary stop must not inherit the fault's evidence. */
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_stop(HOST_STOP_BUDGET, 7u, 0u, "after");
    }
    const host_stop *later = host_run_result();
    CHECK(later->reason == HOST_STOP_BUDGET);
    CHECK(later->fault_rip == 0u && later->fault_stack_top == 0u && later->fault_guest_ebp == 0u);
    CHECK(later->fault_host_frame_count == 0u && later->fault_guest_frame_count == 0u);
    host_run_disarm();
    return copy;
}

static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static void test_the_handler_records_rip_and_both_chains(void)
{
    build_guest_chain();
    const host_stop stop = fault_with(GUEST_STACK + 0x100u, false);
    char name[128];
    CHECK(stop.reason == HOST_STOP_FAULT);
    CHECK(stop.signal_number == SIGSEGV);
    CHECK(stop.fault_address == 0u);
    CHECK(strcmp(stop.detail, "faulted inside lifted code") == 0);
    CHECK(stop.fault_rip != 0u);
    CHECK(host_symbols_describe(stop.fault_rip, name, sizeof(name)) && starts_with(name, "sub_00381100+0x"));
    /* innermost first: the caller of the faulting function, then its caller */
    CHECK(stop.fault_host_frame_count >= 2u);
    CHECK(stop.fault_host_frame_count <= HOST_FAULT_HOST_FRAMES);
    CHECK(host_symbols_describe(stop.fault_host_frames[0], name, sizeof(name)) && starts_with(name, "sub_00381000+0x"));
    CHECK(host_symbols_describe(stop.fault_host_frames[1], name, sizeof(name)) && starts_with(name, "sub_00383DF3+0x"));
    CHECK(stop.fault_guest_ebp == GUEST_STACK + 0x100u);
    CHECK(stop.fault_guest_frame_count == 3u);
    CHECK(stop.fault_guest_frames[0] == 0x00381050u);
    CHECK(stop.fault_guest_frames[1] == 0x00383E00u);
    CHECK(stop.fault_guest_frames[2] == 0x00383E7Fu);
    CHECK(host_symbols_guest_function(stop.fault_guest_frames[0], name, sizeof(name)) && strcmp(name, "sub_00381000+0x50") == 0);
    CHECK(host_symbols_guest_function(stop.fault_guest_frames[1], name, sizeof(name)) && strcmp(name, "sub_00383DF3+0xD") == 0);
    CHECK(host_symbols_guest_function(stop.fault_guest_frames[2], name, sizeof(name)) && strcmp(name, "sub_00383DF3+0x8C") == 0);
}

static void test_a_wild_or_empty_guest_ebp_is_recorded_without_a_second_fault(void)
{
    const host_stop wild = fault_with(WILD_EBP, false);
    CHECK(wild.reason == HOST_STOP_FAULT);
    CHECK(wild.fault_guest_ebp == WILD_EBP);
    CHECK(wild.fault_guest_frame_count == 0u);
    CHECK(wild.fault_rip != 0u && wild.fault_host_frame_count >= 2u);
    const host_stop empty = fault_with(0u, false);
    CHECK(empty.reason == HOST_STOP_FAULT && empty.fault_guest_ebp == 0u);
    CHECK(empty.fault_guest_frame_count == 0u);
}

static void test_without_a_registered_reader_only_the_host_chain_is_recorded(void)
{
    host_run_set_fault_guest_ebp(NULL);
    const host_stop stop = fault_with(GUEST_STACK + 0x100u, false);
    host_run_set_fault_guest_ebp(read_fake_ebp);
    CHECK(stop.reason == HOST_STOP_FAULT && stop.fault_rip != 0u);
    CHECK(stop.fault_host_frame_count >= 2u);
    CHECK(stop.fault_guest_ebp == 0u && stop.fault_guest_frame_count == 0u);
}

static void test_a_frameless_leaf_is_covered_by_the_stack_top(void)
{
    char name[128];
    const host_stop stop = fault_with(GUEST_STACK + 0x100u, true);
    CHECK(stop.reason == HOST_STOP_FAULT);
    /* RIP is inside strlen, which pushes no frame: the rbp chain starts one call too high */
    CHECK(!(host_symbols_describe(stop.fault_rip, name, sizeof(name)) && starts_with(name, "sub_")));
    CHECK(stop.fault_host_frame_count >= 1u);
    CHECK(host_symbols_describe(stop.fault_host_frames[0], name, sizeof(name)) && starts_with(name, "sub_00381000+0x"));
    CHECK(stop.fault_stack_top != 0u);
    CHECK(host_symbols_describe(stop.fault_stack_top, name, sizeof(name)) && starts_with(name, "sub_00381100+0x"));
}

static bool never_a_thunk(uint32_t va)
{
    (void)va;
    return false;
}

static char *report_text(const host_stop *stop)
{
    static const host_report_names names = {
        .stop_reason_str = host_stop_reason_str,
        .thunk_is_va = never_a_thunk,
        .host_symbol = host_symbols_describe,
        .guest_function = host_symbols_guest_function,
    };
    char *buffer = NULL;
    size_t length = 0u;
    FILE *out = open_memstream(&buffer, &length);
    const uint32_t ring[1] = {0u};
    host_report_stop(out, stop, 0x1000u, 0u, ring, 0u, 1u, &names);
    fclose(out);
    return buffer;
}

static void test_the_report_names_the_chain(void)
{
    build_guest_chain();
    const host_stop stop = fault_with(GUEST_STACK + 0x100u, false);
    char *text = report_text(&stop);
    CHECK(text != NULL);
    CHECK(strstr(text, "stopped: host fault\n") != NULL);
    CHECK(strstr(text, "  fault rip      0x") != NULL);
    CHECK(strstr(text, "  sub_00381100+0x") != NULL);
    CHECK(strstr(text, "  sub_00381000+0x") != NULL);
    CHECK(strstr(text, "  sub_00383DF3+0x") != NULL);
    CHECK(strstr(text, "  guest ebp      0x30000100\n") != NULL);
    CHECK(strstr(text, "    #0  0x00381050  sub_00381000+0x50\n") != NULL);
    CHECK(strstr(text, "    #1  0x00383E00  sub_00383DF3+0xD\n") != NULL);
    /* RIP is lifted code, so no stack top caller hint */
    CHECK(strstr(text, "stack top") == NULL);
    free(text);

    const host_stop leaf_stop = fault_with(GUEST_STACK + 0x100u, true);
    text = report_text(&leaf_stop);
    CHECK(text != NULL && strstr(text, "  stack top      0x") != NULL);
    CHECK(text != NULL && strstr(text, "sub_00381100+0x") != NULL);
    free(text);
}

static int first_object_bias(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    *(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
    return 1;
}

static void test_symbol_tables_apply_the_load_bias_and_refuse_non_elf_input(void)
{
    uintptr_t bias = 0u;
    char name[128];
    (void)dl_iterate_phdr(first_object_bias, &bias);
    host_symbol_table *table = host_symbol_table_load("/proc/self/exe", bias);
    CHECK(table != NULL);
    const uintptr_t start = (uintptr_t)&sub_00381100;
    CHECK(host_symbol_table_describe(table, start, name, sizeof(name)) && strcmp(name, "sub_00381100+0x0") == 0);
    CHECK(host_symbol_table_describe(table, start + 3u, name, sizeof(name)) && strcmp(name, "sub_00381100+0x3") == 0);
    CHECK(!host_symbol_table_describe(table, 0x10u, name, sizeof(name)));
    CHECK(!host_symbol_table_describe(table, start, name, 0u));
    CHECK(!host_symbol_table_describe(NULL, start, name, sizeof(name)));
    /* the guest lookup: a start, a byte in, and below the first lifted function */
    CHECK(host_symbol_table_guest_function(table, 0x00381000u, name, sizeof(name)) && strcmp(name, "sub_00381000+0x0") == 0);
    CHECK(host_symbol_table_guest_function(table, 0x003810FFu, name, sizeof(name)) && strcmp(name, "sub_00381000+0xFF") == 0);
    CHECK(host_symbol_table_guest_function(table, 0x00381100u, name, sizeof(name)) && strcmp(name, "sub_00381100+0x0") == 0);
    CHECK(!host_symbol_table_guest_function(table, 0x00001000u, name, sizeof(name)));
    CHECK(host_symbol_table_guest_function(table, 0x00381F10u, name, sizeof(name)) && strcmp(name, "sub_00381100+0xE10") == 0);
    /* but the host side names every function symbol, whatever it is called */
    CHECK(host_symbol_table_describe(table, (uintptr_t)&sub_00381F00_ext, name, sizeof(name)) && strcmp(name, "sub_00381F00_ext+0x0") == 0);
    /* far past the last lifted function it is not that function's code: the guest stack's
     * sentinel return address and heap addresses stay unnamed */
    CHECK(host_symbol_table_guest_function(table, 0x00383DF3u + 0x3FFFFu, name, sizeof(name)));
    CHECK(!host_symbol_table_guest_function(table, 0x00383DF3u + 0x40000u, name, sizeof(name)));
    CHECK(!host_symbol_table_guest_function(table, 0xDEAD0000u, name, sizeof(name)));
    host_symbol_table_free(table);
    if (bias != 0u) {
        /* a PIE symbol table loaded without its bias must not find the function */
        table = host_symbol_table_load("/proc/self/exe", 0u);
        CHECK(table != NULL && !host_symbol_table_describe(table, start, name, sizeof(name)));
        host_symbol_table_free(table);
    }
    CHECK(host_symbol_table_load("/nonexistent/elf", 0u) == NULL);
    CHECK(host_symbol_table_load("/proc/self/cmdline", 0u) == NULL);
    CHECK(host_symbol_table_load("/dev/null", 0u) == NULL);
    host_symbol_table_free(NULL);
}

int main(void)
{
    void *stack = mmap((void *)(uintptr_t)GUEST_STACK, 0x1000u, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (stack != (void *)(uintptr_t)GUEST_STACK) {
        printf("cannot map the fake guest stack at %#x\n", GUEST_STACK);
        return 2;
    }
    host_run_set_fault_guest_ebp(read_fake_ebp);
    test_the_handler_records_rip_and_both_chains();
    test_a_wild_or_empty_guest_ebp_is_recorded_without_a_second_fault();
    test_without_a_registered_reader_only_the_host_chain_is_recorded();
    test_a_frameless_leaf_is_covered_by_the_stack_top();
    test_the_report_names_the_chain();
    test_symbol_tables_apply_the_load_bias_and_refuse_non_elf_input();
    printf("host_fault_report: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
