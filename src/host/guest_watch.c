/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1741: guest write watch by page protection, see guest_watch.h.
 */
#define _GNU_SOURCE
#include "guest_watch.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "host_runtime.h"
#include "host_symbols.h"
#include "kernel_call.h"

#define PAGE_SIZE_BYTES 4096u
#define STACK_WORDS 64u
#define RETIRED_MAX 64u
#define TRAP_FLAG 0x100ull

typedef struct {
    uint64_t present;
    uint64_t polls;
    uintptr_t pc;
    uint32_t fault;
    uint32_t low;
    uint32_t length;
    uint32_t slot;
    int tid;
    uint8_t before[GUEST_WATCH_MAX_LENGTH];
    uint8_t after[GUEST_WATCH_MAX_LENGTH];
    uint64_t stack[STACK_WORDS];
    _Atomic int done;
} watch_record;

typedef struct {
    int pending;
    uintptr_t pc;
    uint32_t fault;
    uint64_t stack[STACK_WORDS];
    uint32_t low[GUEST_WATCH_MAX_RANGES];
    uint8_t before[GUEST_WATCH_MAX_RANGES][GUEST_WATCH_MAX_LENGTH];
    uintptr_t pages[2];
    unsigned page_count;
} step_state;

static guest_dump_set watch_set;
static _Atomic uint32_t slot_low[GUEST_WATCH_MAX_RANGES]; /* resolved address, 0 = unarmed */
static _Atomic uintptr_t retired[RETIRED_MAX];
static _Atomic unsigned retired_next;
static watch_record *ring;
static unsigned ring_max;
static _Atomic unsigned ring_next;
static _Atomic unsigned long long page_faults, dropped, off_range;
static FILE *log_file;
static bool log_is_stderr;
static pthread_t poller;
static _Atomic int poller_run;
static bool running;
static unsigned drained_count;
static struct sigaction previous_segv, previous_trap;
static uint64_t (*present_fn)(void);
static uint64_t (*polls_fn)(void);
static __thread step_state step;

static uintptr_t page_of(uintptr_t address)
{
    return address & ~(uintptr_t)(PAGE_SIZE_BYTES - 1u);
}

static bool slot_covers_page(unsigned slot, uintptr_t page)
{
    const uint32_t low = atomic_load(&slot_low[slot]);
    if (low == 0u) {
        return false;
    }
    return page >= page_of(low) && page <= page_of((uintptr_t)low + watch_set.ranges[slot].length - 1u);
}

static bool page_watched(uintptr_t page)
{
    for (unsigned slot = 0u; slot < watch_set.count; slot++) {
        if (slot_covers_page(slot, page)) {
            return true;
        }
    }
    return false;
}

static bool page_retired(uintptr_t page)
{
    for (unsigned index = 0u; index < RETIRED_MAX; index++) {
        if (atomic_load(&retired[index]) == page) {
            return true;
        }
    }
    return false;
}

static void chain(const struct sigaction *previous, int sig, siginfo_t *info, void *context)
{
    if ((previous->sa_flags & SA_SIGINFO) != 0 && previous->sa_sigaction != NULL) {
        previous->sa_sigaction(sig, info, context);
    } else if (previous->sa_handler == SIG_DFL) {
        signal(sig, SIG_DFL); /* an unrelated fault: re-fault under the default action */
    } else if (previous->sa_handler != SIG_IGN) {
        previous->sa_handler(sig);
    }
}

bool guest_watch_fault(int sig, void *siginfo, void *context)
{
    (void)sig;
    const siginfo_t *info = siginfo;
    ucontext_t *uc = context;
    const uintptr_t fault_page = page_of((uintptr_t)info->si_addr);
    const bool is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    if (ring == NULL || info->si_code != SEGV_ACCERR || !is_write) {
        return false;
    }
    if (!page_watched(fault_page)) {
        return page_retired(fault_page); /* raced with a re-arm: the page is writable again, retry the store */
    }
    atomic_fetch_add(&page_faults, 1ull);
    if (!step.pending) {
        step.pending = 1;
        step.page_count = 0u;
        step.pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        step.fault = (uint32_t)(uintptr_t)info->si_addr;
        const uint64_t *rsp = (const uint64_t *)(uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
        for (unsigned index = 0u; index < STACK_WORDS; index++) {
            step.stack[index] = rsp[index];
        }
        for (unsigned slot = 0u; slot < watch_set.count; slot++) {
            step.low[slot] = atomic_load(&slot_low[slot]);
            if (step.low[slot] != 0u) {
                memcpy(step.before[slot], (const void *)(uintptr_t)step.low[slot], watch_set.ranges[slot].length);
            }
        }
    }
    if (step.page_count < 2u) {
        step.pages[step.page_count++] = fault_page;
    }
    mprotect((void *)fault_page, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE);
    uc->uc_mcontext.gregs[REG_EFL] |= (greg_t)TRAP_FLAG;
    return true;
}

static void on_segv(int sig, siginfo_t *info, void *context)
{
    if (!guest_watch_fault(sig, info, context)) {
        chain(&previous_segv, sig, info, context);
    }
}

static void on_trap(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    if (!step.pending) {
        chain(&previous_trap, sig, info, context);
        return;
    }
    uc->uc_mcontext.gregs[REG_EFL] &= ~(greg_t)TRAP_FLAG;
    bool any_hit = false;
    for (unsigned slot = 0u; slot < watch_set.count; slot++) {
        const uint32_t low = step.low[slot];
        const uint32_t length = watch_set.ranges[slot].length;
        if (low == 0u) {
            continue;
        }
        const bool changed = memcmp(step.before[slot], (const void *)(uintptr_t)low, length) != 0;
        if (!guest_watch_is_hit(step.fault, low, length, changed)) {
            continue;
        }
        any_hit = true;
        const unsigned index = atomic_fetch_add(&ring_next, 1u);
        if (index >= ring_max) {
            atomic_fetch_add(&dropped, 1ull);
            continue;
        }
        watch_record *record = &ring[index];
        record->present = present_fn != NULL ? present_fn() : 0u;
        record->polls = polls_fn != NULL ? polls_fn() : 0u;
        record->pc = step.pc;
        record->fault = step.fault;
        record->low = low;
        record->length = length;
        record->slot = slot;
        record->tid = (int)syscall(SYS_gettid);
        memcpy(record->before, step.before[slot], length);
        memcpy(record->after, (const void *)(uintptr_t)low, length);
        memcpy(record->stack, step.stack, sizeof record->stack);
        atomic_store(&record->done, 1);
    }
    if (!any_hit) {
        atomic_fetch_add(&off_range, 1ull);
    }
    for (unsigned index = 0u; index < step.page_count; index++) {
        if (page_watched(step.pages[index])) {
            mprotect((void *)step.pages[index], PAGE_SIZE_BYTES, PROT_READ);
        }
    }
    step.pending = 0;
}

static bool read_u32(uint32_t address, uint32_t *out)
{
    uint8_t raw[4];
    if (address == 0u || !kernel_guest_read_bytes(address, raw, sizeof raw)) {
        return false;
    }
    *out = (uint32_t)raw[0] | (uint32_t)raw[1] << 8 | (uint32_t)raw[2] << 16 | (uint32_t)raw[3] << 24;
    return true;
}

/* Resolve a range to a guest address, 0 when the chain is null or unreadable. */
static uint32_t resolve(const guest_dump_range *range)
{
    uint32_t first = 0u, slot = 0u, second = 0u, final = 0u;
    if (!range->indirect) {
        return range->address;
    }
    if (!read_u32(range->address, &first)) {
        return 0u;
    }
    if (range->twice) {
        if (guest_dump_twice_slot(range, first, &slot) != NULL || !read_u32(slot, &second) ||
            guest_dump_twice_target(range, second, &final) != NULL) {
            return 0u;
        }
        return final;
    }
    return guest_dump_indirect_target(range, first, &final) == NULL ? final : 0u;
}

static void protect_range(uint32_t low, uint32_t length)
{
    for (uintptr_t page = page_of(low); page <= page_of((uintptr_t)low + length - 1u); page += PAGE_SIZE_BYTES) {
        if (mprotect((void *)page, PAGE_SIZE_BYTES, PROT_READ) != 0) {
            fprintf(log_file, "# arm failed page 0x%08lX\n", (unsigned long)page);
        }
    }
}

static void unprotect_range(uint32_t low, uint32_t length)
{
    for (uintptr_t page = page_of(low); page <= page_of((uintptr_t)low + length - 1u); page += PAGE_SIZE_BYTES) {
        if (!page_watched(page)) {
            atomic_store(&retired[atomic_fetch_add(&retired_next, 1u) % RETIRED_MAX], page);
            mprotect((void *)page, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE);
        }
    }
}

static void rearm(unsigned slot)
{
    const guest_dump_range *range = &watch_set.ranges[slot];
    const uint32_t wanted = resolve(range);
    const uint32_t old = atomic_load(&slot_low[slot]);
    if (wanted == old) {
        return;
    }
    atomic_store(&slot_low[slot], wanted); /* publish before protecting: a fault is never unknown */
    if (wanted != 0u) {
        protect_range(wanted, range->length);
    }
    if (old != 0u) {
        unprotect_range(old, range->length);
    }
    fprintf(log_file, "armed slot %u at 0x%08X (was 0x%08X)\n", slot, (unsigned)wanted, (unsigned)old);
}

static void write_record(unsigned index)
{
    const watch_record *record = &ring[index];
    char where[96];
    if (!host_symbols_describe(record->pc, where, sizeof where)) {
        snprintf(where, sizeof where, "host:0x%lX", (unsigned long)record->pc);
    }
    fprintf(log_file, "hit %u present %llu poll %llu tid %d slot %u addr 0x%08X fault 0x%08X pc %s\n  before", index,
            (unsigned long long)record->present, (unsigned long long)record->polls, record->tid, record->slot,
            (unsigned)record->low, (unsigned)record->fault, where);
    for (uint32_t byte = 0u; byte < record->length; byte++) {
        fprintf(log_file, " %02X", record->before[byte]);
    }
    fputs("\n  after ", log_file);
    for (uint32_t byte = 0u; byte < record->length; byte++) {
        fprintf(log_file, " %02X", record->after[byte]);
    }
    fputs("\n  callers", log_file);
    unsigned printed = 0u;
    for (unsigned word = 0u; word < STACK_WORDS && printed < 6u; word++) {
        const uintptr_t value = (uintptr_t)record->stack[word];
        char name[96];
        /* a return address: it names a lifted function and the byte run before it is a call rel32 */
        if (value > 8u && host_symbols_describe(value, name, sizeof name) && strncmp(name, "sub_", 4) == 0 &&
            ((const uint8_t *)value)[-5] == 0xE8) {
            fprintf(log_file, " %s", name);
            printed++;
        }
    }
    fputc('\n', log_file);
}

static void drain(void)
{
    const unsigned total = atomic_load(&ring_next);
    const unsigned limit = total < ring_max ? total : ring_max;
    while (drained_count < limit && atomic_load(&ring[drained_count].done) != 0) {
        write_record(drained_count);
        drained_count++;
    }
    fflush(log_file);
}

static void *poll_main(void *unused)
{
    (void)unused;
    while (atomic_load(&poller_run) != 0) {
        for (unsigned slot = 0u; slot < watch_set.count; slot++) {
            rearm(slot);
        }
        drain();
        const struct timespec pause = {0, 20 * 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    return NULL;
}

bool guest_watch_start(const guest_dump_set *set, const char *log_path, unsigned max_records, uint64_t (*present)(void),
                       uint64_t (*polls)(void))
{
    if (running || !guest_watch_check(set, NULL, 0u) || max_records == 0u || max_records > GUEST_WATCH_HARD_RECORDS) {
        return false;
    }
    log_is_stderr = log_path == NULL;
    log_file = log_is_stderr ? stderr : fopen(log_path, "a");
    ring = calloc(max_records, sizeof *ring);
    if (log_file == NULL || ring == NULL) {
        free(ring);
        ring = NULL;
        if (!log_is_stderr && log_file != NULL) {
            fclose(log_file);
        }
        log_file = NULL;
        return false;
    }
    watch_set = *set;
    ring_max = max_records;
    present_fn = present;
    polls_fn = polls;
    drained_count = 0u;
    atomic_store(&ring_next, 0u);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    action.sa_sigaction = on_segv;
    sigaction(SIGSEGV, &action, &previous_segv);
    action.sa_sigaction = on_trap;
    sigaction(SIGTRAP, &action, &previous_trap);
    host_run_set_fault_hook(guest_watch_fault);
    fprintf(log_file, "# guest-watch 1 ranges %u records %u\n", set->count, max_records);
    for (unsigned slot = 0u; slot < set->count; slot++) {
        rearm(slot);
    }
    atomic_store(&poller_run, 1);
    running = pthread_create(&poller, NULL, poll_main, NULL) == 0;
    if (!running) {
        guest_watch_stop();
        return false;
    }
    return true;
}

void guest_watch_stop(void)
{
    if (log_file == NULL) {
        return;
    }
    if (running) {
        atomic_store(&poller_run, 0);
        pthread_join(poller, NULL);
        running = false;
    }
    for (unsigned slot = 0u; slot < watch_set.count; slot++) {
        const uint32_t low = atomic_exchange(&slot_low[slot], 0u);
        if (low != 0u) {
            unprotect_range(low, watch_set.ranges[slot].length);
        }
    }
    host_run_set_fault_hook(NULL);
    sigaction(SIGSEGV, &previous_segv, NULL);
    sigaction(SIGTRAP, &previous_trap, NULL);
    drain();
    fprintf(log_file, "summary hits %u logged %u dropped %llu page_faults %llu off_range_faults %llu\n",
            atomic_load(&ring_next), drained_count, (unsigned long long)atomic_load(&dropped),
            (unsigned long long)atomic_load(&page_faults), (unsigned long long)atomic_load(&off_range));
    fflush(log_file);
    if (!log_is_stderr) {
        fclose(log_file);
    }
    log_file = NULL;
    free(ring);
    ring = NULL;
}
