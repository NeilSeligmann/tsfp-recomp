/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Differential-test subject for xboxrecomp-generated code.
 *
 * Loads a flat guest memory image, identity-maps the guest window at its
 * Xbox virtual address, then runs one lifted guest function per test case
 * over a line-based stdin/stdout protocol (see the protocol comment near
 * main()). A Unicorn-based oracle runs the original x86-32 bytes over the
 * same initial state; the two write-sets are compared externally.
 *
 * DIRTY-PAGE TRACKING
 * --------------------
 * A naive subject would snapshot/restore and diff the whole guest window
 * (16 MB) on every case, which makes the per-case cost O(window size)
 * instead of O(pages the function actually touched). Instead:
 *
 *   - Before each RUN, the whole window is mprotect()'d PROT_READ.
 *   - The first store to any guest page faults (SIGSEGV). The handler saves
 *     that page's pre-write bytes into a side table, promotes just that
 *     page to PROT_READ|PROT_WRITE, marks it dirty, and returns -- the
 *     faulting store instruction then retries and succeeds.
 *   - After the call, only dirty pages are diffed against their saved
 *     pre-write snapshot, and only dirty pages are restored from the
 *     pristine image before the next case.
 *
 * This is the same trick JITs/GCs use for write barriers (and the one
 * PostgreSQL's pg_rewind-style tools use for incremental diffing): the cost
 * is proportional to the number of pages written, not the window size.
 *
 * WHY -pie IS MANDATORY
 * ----------------------
 * The guest window is [0x00010000, 0x01000000). A -no-pie binary is an
 * ET_EXEC loaded at the fixed base address 0x400000, which falls *inside*
 * that window, so the kernel's own placement of our code/data collides with
 * the guest image and the identity mmap(MAP_FIXED_NOREPLACE) below fails.
 * Linking -pie makes this an ET_DYN that the kernel places at a high,
 * randomized address, leaving the low window free for us to claim.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>

/* The fs segment base both sides run with. Must equal KPCR_BASE in seeding.py (a test
 * pins it): the oracle sets the FS_BASE MSR to the same page (T469). */
#define HARNESS_FS_BASE 0x00D40000u
#define GUEST_LO 0x00010000u
#define GUEST_HI 0x01000000u          /* 16 MB covers image + globals + stack */
#define GUEST_SPAN (GUEST_HI - GUEST_LO)

#define PAGE_SIZE 4096u
#define NUM_PAGES (GUEST_SPAN / PAGE_SIZE)
_Static_assert((GUEST_SPAN % PAGE_SIZE) == 0,
               "GUEST_SPAN must be an exact multiple of PAGE_SIZE");

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);

extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __thread uint32_t g_ebx, g_esi, g_edi, g_ebp;
extern __thread uint32_t g_seh_ebp;
extern __thread uint32_t g_fs_base;
extern __thread int g_df;
/* Opt-in EFLAGS publication (a --publish-eflags lift). Defined by
 * runtime_min.c unconditionally; written only by a publishing tree's rets and
 * by harness_stub_call, which clears the mask so a stubbed tail exit reads as
 * unpublished. Reset to 0 before every RUN, so a stale publish from a previous
 * case can never be reported as this one's. */
extern __thread uint32_t g_harness_eflags, g_harness_eflags_mask;
extern __thread double g_fp_stack[8];
extern __thread int g_fp_top;
extern __thread uint16_t g_fp_control_word;
extern int g_harness_icall_failures;
extern void harness_vector_set(const uint64_t words[16], uint32_t mxcsr) __attribute__((weak));
extern void harness_vector_get(uint64_t words[16], uint32_t *mxcsr) __attribute__((weak));
/* T1620 fp-scalar-v1 real MXCSR (tools/harness/runtime_min.c). */
extern void harness_mxcsr_load_real(uint32_t mxcsr) __attribute__((weak));
extern void harness_mxcsr_capture_real(void) __attribute__((weak));
extern void harness_mxcsr_restore_default(void) __attribute__((weak));

/* Callee stubbing (tools/harness/call_stub.c). Only active for functions the
 * harness selected as call-bearing, and only in a subject whose lifted objects
 * were built with the interception shim -- see HARNESS_STUB_OBJECTS below. */
void harness_stub_reset(void);
int harness_stub_add(uint32_t va, uint32_t pop_bytes);
int harness_passthrough_add(uint32_t va, uint32_t caller_va);
int harness_stackprobe_supported(void);
extern uint32_t g_passthrough_applied, g_passthrough_fault;
uint32_t harness_stub_count(void);
extern int g_stub_strict;
extern uint32_t g_stub_applied;
extern uint32_t g_stub_misses;
extern uint32_t g_stub_miss_va;

/* Set by tools/harness/build_subject.sh when, and only when, it has verified with
 * nm(1) that the linked lifted objects actually route their calls through
 * harness_stub_call. It is reported at READY so the harness can refuse to run a
 * stubbed selection against a subject that would silently execute the real
 * callees -- which would compare a stubbed oracle against an unstubbed subject
 * and call the difference a lifter defect. */
#ifndef HARNESS_STUB_OBJECTS
#define HARNESS_STUB_OBJECTS 0
#endif

/* Set by tools/harness/build_subject.sh when, and only when, the generated
 * header declares the EFLAGS publication globals -- i.e. the linked tree is a
 * --publish-eflags lift. Reported at READY as eflags=, so the harness can
 * refuse a --compare-eflags run against a subject that publishes nothing:
 * such a run would report every case as unpublished and verify no flag. */
#ifndef HARNESS_EFLAGS_OBJECTS
#define HARNESS_EFLAGS_OBJECTS 0
#endif

/* WHICH LIFTED TREE THIS BINARY WAS BUILT FROM.
 *
 * Baked in by tools/harness/build_subject.sh and reported on the READY line, so
 * that no result can be read without knowing what produced it. It is compiled in
 * rather than passed to the harness on the command line for the same reason
 * stub= is derived from the objects by inspection: a figure a human asserts is a
 * figure a human can assert wrongly, and this one was. A subject built from a
 * lift ten hours older than the fix it was meant to be testing was reported as a
 * lifter defect and adopted into the project record twice.
 *
 * UNKNOWN when something other than build_subject.sh linked this binary. That is
 * deliberately a value and not a blank: the harness refuses to run against it. */
#ifndef HARNESS_GEN_DIR
#define HARNESS_GEN_DIR "UNKNOWN"
#endif
#ifndef HARNESS_TREE_SHA
#define HARNESS_TREE_SHA "UNKNOWN"
#endif

/* HAND-WRITTEN REPLACEMENTS (src/game), present only in a subject built with
 * HARNESS_REPLACEMENT=1 by `tools/replace build-subject`.
 *
 * The replacements are linked as STRONG definitions of the same `sub_XXXXXXXX` symbols the
 * lifted chunks define weakly, so `recomp_lookup` below returns the hand-written adapter
 * exactly as it would in the shipped host. Nothing here substitutes a function by a side
 * channel, which is the point: the harness exercises the same link-time mechanism the
 * product uses.
 *
 * HARNESS_REPL_SHA is the content digest of src/game when the subject was built, reported
 * at READY for the same reason HARNESS_TREE_SHA is: so a subject built from yesterday's
 * hand code cannot be mistaken for today's. */
/* T1510 opt-in raw x87 backend. OFF unless the subject is built with -DHARNESS_X87RAW=1 and
 * linked with x87_runtime.c + x87_native.c (the authenticated raw-state runtime); the default
 * driver is byte-identical in behaviour and still refuses X87RAW. Only bodies that go through
 * the harness_x87_* primitives (hand/isolated lifts) drive that state: a body that used the
 * legacy double model leaves g_fp_* changed and the case is refused (see x87raw-legacy-model). */
#ifndef HARNESS_X87RAW
#define HARNESS_X87RAW 0
#endif
#if HARNESS_X87RAW
#include "x87_runtime.h"
#endif
/* T1508 opt-in code-write stop channel. OFF unless the subject is built with -DHARNESS_CODEWRITE=1
 * (the faithful-codewrite-arbiter-v2 proof): the default driver prints exactly what it printed
 * before. A replacement of a root whose original rewrites its own code ends a case it cannot
 * complete (a fault the original would raise, or an opcode outside its interpreter) through
 * harness_codewrite_stop, which publishes the registers and the write set the case had reached
 * as a normal result block plus one CWSTOP line, instead of the bare FAULT line. */
#ifndef HARNESS_CODEWRITE
#define HARNESS_CODEWRITE 0
#endif
#if HARNESS_CODEWRITE
#define HARNESS_CODEWRITE_READY " codewrite=1"
#else
#define HARNESS_CODEWRITE_READY ""
#endif
#ifndef HARNESS_REPLACEMENT
#define HARNESS_REPLACEMENT 0
#endif
/* T1576 arm witness: a clang -fprofile-instr-generate build writes one profile per case to the
 * file named by HARNESS_WITNESS_FILE (counters reset immediately before the case). Off by
 * default: no code, no READY change, byte-identical protocol. */
#ifndef HARNESS_WITNESS
#define HARNESS_WITNESS 0
#endif
#if HARNESS_WITNESS
extern void __llvm_profile_reset_counters(void);
extern void __llvm_profile_set_filename(const char *);
extern int __llvm_profile_write_file(void);
static void witness_dump(void)
{
    const char *path = getenv("HARNESS_WITNESS_FILE");
    if (path != NULL) {
        __llvm_profile_set_filename(path);
        __llvm_profile_write_file();
    }
}
#endif
#ifndef HARNESS_REPL_SHA
#define HARNESS_REPL_SHA "NONE"
#endif
#if HARNESS_REPLACEMENT
#include "game_replace.h"

#endif

#if HARNESS_REPLACEMENT
static const game_replacement *find_replacement(uint32_t va)
{
    size_t count = 0;
    const game_replacement *table = game_replacement_table(&count);
    for (size_t k = 0; k < count; k++) {
        if (table[k].va == va) return &table[k];
    }
    return NULL;
}
#endif

static uint8_t *g_guest;          /* identity-mapped guest window */
static uint8_t *g_pristine;       /* image as loaded, GUEST_SPAN bytes */
static uint8_t *g_snapshot_pool;  /* pre-write snapshot, indexed like g_guest */

/* Dirty-page bookkeeping. Sized for the worst case (every page dirty) so
 * nothing in the SIGSEGV handler ever needs to allocate -- malloc is not
 * async-signal-safe, so all storage here is allocated once at startup. */
static uint8_t  g_page_dirty[NUM_PAGES];
static uint32_t g_dirty_list[NUM_PAGES];
static uint32_t g_dirty_count;

/* "Must be restored before the next case" is NOT the same set as "snapshotted
 * and promoted to writable", and conflating the two is a silent correctness bug.
 * PATCH writes while the window is still fully writable, so it modifies pages
 * without ever faulting and therefore without entering the dirty list. If those
 * pages are not restored, one case's stack frame survives into the next, whose
 * pre-write snapshot then captures the stale bytes as its baseline -- and the
 * two sides' write-sets diverge for a reason that has nothing to do with the
 * lifted code. That was observed as a false DISAGREE before this was split out.
 *
 * The restore set is a superset of the dirty set, so the dirty flags are cleared
 * while walking it. */
static uint8_t  g_page_restore[NUM_PAGES];
static uint32_t g_restore_list[NUM_PAGES];
static uint32_t g_restore_count;

/* Mark a page as needing restoration before the next case. Must stay
 * async-signal-safe: no allocation, fixed-size storage only. */
static void mark_for_restore(uint32_t page)
{
    if (!g_page_restore[page]) {
        g_page_restore[page] = 1;
        g_restore_list[g_restore_count++] = page;
    }
}

typedef enum {
    FAULT_NONE = 0,
    FAULT_SEGV,
    FAULT_BUS,
    FAULT_FPE,
    FAULT_ILL,
    FAULT_ABORT,
} fault_kind_t;

static sigjmp_buf g_crash_jmp;
static volatile sig_atomic_t g_fault_kind;
/* T1576 producer (b): the raw si_addr of the last genuine SEGV/BUS (0 = none). The guest is
 * identity-mapped (guest VA == host VA, see main), so a fault address below 4 GiB IS the guest
 * address with no translation. Reported only when HARNESS_FAULT_ADDR is set, so default output
 * stays byte-identical. */
static volatile uint64_t g_fault_addr;
static volatile sig_atomic_t g_fault_addr_valid;
static int g_report_fault_addr;

static const char *fault_kind_name(fault_kind_t k)
{
    switch (k) {
    case FAULT_SEGV:  return "SEGV";
    case FAULT_BUS:   return "BUS";
    case FAULT_FPE:   return "FPE";
    case FAULT_ILL:   return "ILL";
    case FAULT_ABORT: return "ABORT";
    default:          return "SEGV";
    }
}

/* SIGSEGV handler: a fault inside the guest window is the expected "first
 * write to this page" signal, not a real crash. Anything outside the
 * window is a genuine fault. */
static void segv_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)sig; (void)ucontext;
    uintptr_t addr = (uintptr_t)info->si_addr;

    if (addr >= GUEST_LO && addr < GUEST_HI) {
        uint32_t page = (uint32_t)((addr - GUEST_LO) / PAGE_SIZE);
        if (!g_page_dirty[page]) {
            uint8_t *page_ptr = g_guest + (size_t)page * PAGE_SIZE;
            memcpy(g_snapshot_pool + (size_t)page * PAGE_SIZE, page_ptr, PAGE_SIZE);
            if (mprotect(page_ptr, PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
                /* Can't safely do anything fancier from inside a signal
                 * handler; a failed mprotect here means we can't make
                 * forward progress at all. */
                _exit(97);
            }
            g_page_dirty[page] = 1;
            g_dirty_list[g_dirty_count++] = page;
            mark_for_restore(page);
        }
        return; /* retry the faulting instruction; it will now succeed */
    }

    g_fault_addr = (uint64_t)addr;
    g_fault_addr_valid = 1;
    g_fault_kind = FAULT_SEGV;
    siglongjmp(g_crash_jmp, 1);
}

/* SIGBUS/SIGFPE/SIGILL/SIGABRT are always genuine faults -- there is no "first
 * write" interpretation for them.
 *
 * SIGABRT IS NOT OPTIONAL. The lifter's trap family (recomp_unimpl_trap,
 * recomp_flags_unresolved_trap, recomp_icall_unresolved_trap,
 * recomp_noreturn_returned) is declared noreturn and its contract is abort().
 * Measured on the shipped tree: 6,575 relocations reach those four symbols. Left
 * unhandled, the first case to touch any of them killed this process, which cost
 * the whole run rather than scoring one case as a fault -- and a run of thousands
 * of cases is expensive enough that losing it is how a harness stops being used.
 *
 * Caught here rather than by arming the runtime's own siglongjmp hook, because
 * this covers abort() from ANY source -- a trap, an assert(), glibc's own
 * heap-corruption abort -- and does not tie the driver to one runtime's internal
 * header. siglongjmp with a mask-saving sigsetjmp restores the signal mask, so
 * SIGABRT is unblocked again for the next case even though abort() raised it with
 * the handler's own delivery mask in force. */
static void fatal_signal_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)ucontext;
    if ((sig == SIGBUS || sig == SIGFPE || sig == SIGILL) && info != NULL) {
        g_fault_addr = (uint64_t)(uintptr_t)info->si_addr;
        g_fault_addr_valid = 1;
    }
    switch (sig) {
    case SIGBUS:  g_fault_kind = FAULT_BUS; break;
    case SIGFPE:  g_fault_kind = FAULT_FPE; break;
    case SIGILL:  g_fault_kind = FAULT_ILL; break;
    case SIGABRT: g_fault_kind = FAULT_ABORT; break;
    default:      g_fault_kind = FAULT_SEGV; break;
    }
    siglongjmp(g_crash_jmp, 1);
}

#if HARNESS_CODEWRITE
static volatile sig_atomic_t g_cw_stopped;
static unsigned g_cw_kind;
static uint32_t g_cw_eip, g_cw_steps;

void harness_codewrite_stop(unsigned kind, uint32_t eip, uint32_t steps)
{
    g_cw_kind = kind;
    g_cw_eip = eip;
    g_cw_steps = steps;
    g_cw_stopped = 1;
    g_fault_kind = FAULT_ABORT;
    siglongjmp(g_crash_jmp, 1);
}
#endif

static void load_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("open image"); exit(2); }
    for (;;) {
        uint32_t va, len;
        if (fread(&va, 4, 1, f) != 1) break;
        if (fread(&len, 4, 1, f) != 1) break;
        if (va < GUEST_LO || (uint64_t)va + len > GUEST_HI) {
            fprintf(stderr, "image record 0x%08X+%u outside window\n", va, len);
            exit(2);
        }
        if (fread(g_guest + (va - GUEST_LO), 1, len, f) != len) {
            fprintf(stderr, "short read for 0x%08X\n", va);
            exit(2);
        }
    }
    fclose(f);
    memcpy(g_pristine, g_guest, GUEST_SPAN);
}

/* Restore only the pages a previous RUN actually dirtied, then forget them.
 * Never touches the rest of the 16 MB window. */
static void restore_dirty_pages(void)
{
    for (uint32_t k = 0; k < g_restore_count; k++) {
        uint32_t page = g_restore_list[k];
        size_t off = (size_t)page * PAGE_SIZE;
        memcpy(g_guest + off, g_pristine + off, PAGE_SIZE);
        g_page_restore[page] = 0;
        g_page_dirty[page] = 0;
    }
    g_restore_count = 0;
    g_dirty_count = 0;
}

/* Emit each maximal contiguous run of bytes that differ from the pre-write
 * snapshot. Clean pages are skipped a whole page at a time -- only dirty
 * pages are ever byte-compared, so cost is O(pages touched), not O(window).
 */
static void report_writes(void)
{
    uint32_t i = 0;
    while (i < GUEST_SPAN) {
        uint32_t page = i / PAGE_SIZE;
        if (!g_page_dirty[page]) { i = (page + 1) * PAGE_SIZE; continue; }
        if (g_guest[i] == g_snapshot_pool[i]) { i++; continue; }

        uint32_t start = i;
        do {
            i++;
        } while (i < GUEST_SPAN && g_page_dirty[i / PAGE_SIZE] &&
                 g_guest[i] != g_snapshot_pool[i]);

        printf("W %08X ", GUEST_LO + start);
        for (uint32_t k = start; k < i; k++) printf("%02X", g_guest[k]);
        printf("\n");
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

/* Dynamically-grown line reader. getline(3) would handle this too, but a
 * hand-rolled cap lets us emit the exact "FATAL line-too-long" diagnostic
 * the protocol calls for instead of just failing on malloc exhaustion. */
#define LINE_INITIAL_CAP (1u << 16)  /* 64 KB, matches the prototype */
#define LINE_MAX_CAP     (1u << 20)  /* 1 MB: comfortably covers any patch */

static char *g_linebuf;
static size_t g_linecap;

/* Returns 1 with a NUL-terminated, newline-stripped line in g_linebuf, or 0
 * at EOF with nothing read. Exits via FATAL line-too-long if a line would
 * exceed LINE_MAX_CAP. */
static int read_line(FILE *f)
{
    if (!g_linebuf) {
        g_linecap = LINE_INITIAL_CAP;
        g_linebuf = malloc(g_linecap);
        if (!g_linebuf) { perror("malloc"); exit(2); }
    }

    size_t len = 0;
    for (;;) {
        if (!fgets(g_linebuf + len, (int)(g_linecap - len), f)) {
            if (len == 0) return 0; /* clean EOF */
            break;                  /* trailing partial line at EOF */
        }
        len += strlen(g_linebuf + len);
        if (len > 0 && g_linebuf[len - 1] == '\n') {
            g_linebuf[len - 1] = '\0';
            len--;
            break;
        }
        if (!feof(f)) {
            /* Buffer filled without hitting a newline: grow and keep going. */
            if (g_linecap >= LINE_MAX_CAP) {
                printf("FATAL line-too-long\n");
                fflush(stdout);
                exit(1);
            }
            size_t newcap = g_linecap * 2;
            if (newcap > LINE_MAX_CAP) newcap = LINE_MAX_CAP;
            char *nb = realloc(g_linebuf, newcap);
            if (!nb) { perror("realloc"); exit(2); }
            g_linebuf = nb;
            g_linecap = newcap;
            continue;
        }
        break; /* EOF without a trailing newline */
    }
    if (len > 0 && g_linebuf[len - 1] == '\r') g_linebuf[len - 1] = '\0';
    return 1;
}

/*
 * Line protocol (stdin -> stdout):
 *
 *   READY base=0x00010000 span=0x00FF0000 stub=0 tree_sha=<hex> gen_dir=<path>
 *       Printed once at startup. tree_sha and gen_dir say which lifted tree
 *       this binary was built from; see the HARNESS_GEN_DIR comment above.
 *
 *   CASE <va> <eax> <ecx> <edx> <ebx> <esp> <ebp> <esi> <edi> <df>
 *       Bare hex. Records the pending case and restores dirty pages to
 *       pristine. Emits nothing.
 *   PATCH <addr> <HEXBYTES>
 *       Bare hex address, even-length uppercase hex payload. Writes into
 *       the guest window. Applies to the pending case; because this
 *       happens before RUN's mprotect(PROT_READ), the patched bytes become
 *       part of the pre-write baseline and are never reported as writes
 *       unless the function itself changes them further. Emits nothing.
 *   LISTREPL
 *       Prints one `REPL <va> <name> <convention> <stack_args> <returns> <scratch> <file>` line
 *       per hand-written replacement linked into this subject, then END. Prints only END
 *       for a subject with none.
 *   RUN
 *       Executes the pending case, emits one result block:
 *         REGS <eax> <ecx> <edx> <ebx> <esp> <ebp> <esi> <edi>
 *         SEHEBP <value>        (the lifter's second frame pointer, not compared)
 *         FLAGS <eflags> <mask> (opt-in EFLAGS publication; mask 0 = unpublished)
 *         W <addr> <HEXBYTES>   (zero or more, ascending by address)
 *         REPL 1                (only for a registered replacement: the dispatch table
 *                                returned the hand-written adapter for this address)
 *         END
 *       or a fault block:
 *         FAULT <SEGV|BUS|FPE|ILL|ABORT|NOFUNC|ICALL|NOT-REPLACED|STUB-MISS:...>
 *         END
 */
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: driver <image>\n"); return 2; }

    g_report_fault_addr = getenv("HARNESS_FAULT_ADDR") != NULL;
    void *want = (void *)(uintptr_t)GUEST_LO;
    g_guest = mmap(want, GUEST_SPAN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (g_guest != want) {
        printf("FATAL identity-map-failed wanted=%p got=%p\n", want, (void *)g_guest);
        fflush(stdout);
        return 3;
    }

    g_pristine = malloc(GUEST_SPAN);
    g_snapshot_pool = malloc(GUEST_SPAN);
    if (!g_pristine || !g_snapshot_pool) { perror("malloc"); return 2; }

    load_image(argv[1]);

    struct sigaction sa_segv; memset(&sa_segv, 0, sizeof sa_segv);
    sa_segv.sa_sigaction = segv_handler;
    sa_segv.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa_segv, NULL);

    struct sigaction sa_fatal; memset(&sa_fatal, 0, sizeof sa_fatal);
    sa_fatal.sa_sigaction = fatal_signal_handler;
    sa_fatal.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa_fatal, NULL);
    sigaction(SIGFPE, &sa_fatal, NULL);
    sigaction(SIGILL, &sa_fatal, NULL);
    sigaction(SIGABRT, &sa_fatal, NULL);

    /* gen_dir is LAST and runs to end of line, so a path containing a space
     * cannot swallow a following field or be truncated by one. */
#if HARNESS_REPLACEMENT
    size_t replacement_count = 0;
    (void)game_replacement_table(&replacement_count);
#else
    size_t replacement_count = 0;
#endif
    printf("READY base=0x%08x span=0x%08x stub=%d stackprobe=%d eflags=%d x87=1 x87raw=%d vector=%d mxcsr=%d" HARNESS_CODEWRITE_READY " tree_sha=%s repl=%zu repl_sha=%s gen_dir=%s\n",
           GUEST_LO, GUEST_SPAN, HARNESS_STUB_OBJECTS, harness_stackprobe_supported(),
           HARNESS_EFLAGS_OBJECTS, HARNESS_X87RAW, harness_vector_set != NULL && harness_vector_get != NULL,
           harness_mxcsr_load_real != NULL && harness_mxcsr_capture_real != NULL
               && harness_mxcsr_restore_default != NULL,
           HARNESS_TREE_SHA, replacement_count, HARNESS_REPL_SHA, HARNESS_GEN_DIR);
    fflush(stdout);

    uint32_t pending_va = 0, pending_regs[8] = {0};
    int pending_df = 0;
    /* x87 entry state from FPIN (T356). Cleared by every CASE so a case without an
     * FPIN line starts with an empty stack and the default control word. */
    unsigned pending_fp_depth = 0;
    uint64_t pending_fp_bits[8] = {0};
    unsigned pending_fp_control = 0x037F;
#if HARNESS_X87RAW
    unsigned char pending_raw[86] = {0};
    int pending_raw_set = 0;
#endif
    uint64_t pending_xmm[16] = {0};
    unsigned pending_mxcsr = 0x1F80;
    int pending_vector = 0;
    int pending_mxcsr_real = 0;

    while (read_line(stdin)) {
        char *line = g_linebuf;

        if (strncmp(line, "CASE ", 5) == 0) {
            unsigned va, r[8], df;
            sscanf(line + 5, "%x %x %x %x %x %x %x %x %x %x",
                   &va, &r[0], &r[1], &r[2], &r[3], &r[4], &r[5], &r[6], &r[7], &df);

            /* Undo the previous RUN's work: put back only what it dirtied,
             * then reopen the whole window for writing so PATCH/this case's
             * restore never has to fault its way through. */
            restore_dirty_pages();
            if (mprotect(g_guest, GUEST_SPAN, PROT_READ | PROT_WRITE) != 0) {
                perror("mprotect rw"); return 2;
            }

            /* The stub table belongs to ONE case. Carrying an entry into the next
             * case would stub a callee the oracle executed for real, so the table
             * is cleared here rather than overwritten per entry. */
            harness_stub_reset();

            pending_va = va;
            for (int k = 0; k < 8; k++) pending_regs[k] = r[k];
            pending_df = (int)df;
            pending_fp_depth = 0;
            pending_fp_control = 0x037F;
#if HARNESS_X87RAW
            pending_raw_set = 0;
#endif
            memset(pending_xmm, 0, sizeof pending_xmm);
            pending_mxcsr = 0x1F80;
            pending_vector = 0;
            pending_mxcsr_real = 0;

        } else if (strcmp(line, "MXCSRREAL") == 0) {
            /* T1620: run this case on the real MXCSR (fp-scalar-v1). */
            if (!harness_mxcsr_load_real || !harness_mxcsr_capture_real
                || !harness_mxcsr_restore_default || pending_mxcsr_real) {
                printf("FATAL bad-mxcsrreal\n"); fflush(stdout); return 2;
            }
            pending_mxcsr_real = 1;

        } else if (strncmp(line, "XMMIN ", 6) == 0) {
            char *cursor = line + 6, *end;
            unsigned long control = strtoul(cursor, &end, 16);
            if (!harness_vector_set || !harness_vector_get ||
                end == cursor || control > 0xFFFFu || pending_vector) {
                printf("FATAL bad-xmmin\n"); fflush(stdout); return 2;
            }
            pending_mxcsr = (unsigned)control;
            cursor = end;
            for (unsigned i = 0; i < 8; ++i) {
                while (*cursor == ' ') ++cursor;
                if (strspn(cursor, "0123456789abcdefABCDEF") != 32) {
                    printf("FATAL bad-xmmin\n"); fflush(stdout); return 2;
                }
                char half[17]; half[16] = 0;
                memcpy(half, cursor, 16);
                pending_xmm[i * 2 + 1] = strtoull(half, NULL, 16);
                memcpy(half, cursor + 16, 16);
                pending_xmm[i * 2] = strtoull(half, NULL, 16);
                cursor += 32;
            }
            while (*cursor == ' ') ++cursor;
            if (*cursor) { printf("FATAL bad-xmmin\n"); fflush(stdout); return 2; }
            pending_vector = 1;

        } else if (strncmp(line, "X87RAW", 6) == 0) {
#if HARNESS_X87RAW
            /* X87RAWIN 1 <tags4> <control4> <status4> <8 x raw80 hex20>, as x87_state.encode_record. */
            unsigned fields[3];
            char *cursor = line;
            if (strncmp(line, "X87RAWIN 1 ", 11) != 0 || pending_raw_set || pending_fp_depth) {
                printf("FATAL bad-x87raw\n"); fflush(stdout); return 2;
            }
            cursor += 11;
            for (int i = 0; i < 3; ++i) {
                while (*cursor == ' ') ++cursor;
                if (strspn(cursor, "0123456789abcdefABCDEF") != 4) {
                    printf("FATAL bad-x87raw\n"); fflush(stdout); return 2;
                }
                char word[5] = {cursor[0], cursor[1], cursor[2], cursor[3], 0};
                fields[i] = (unsigned)strtoul(word, NULL, 16);
                cursor += 4;
            }
            for (int slot = 0; slot < 8; ++slot) {
                while (*cursor == ' ') ++cursor;
                if (strspn(cursor, "0123456789abcdefABCDEF") != 20) {
                    printf("FATAL bad-x87raw\n"); fflush(stdout); return 2;
                }
                for (int byte = 0; byte < 10; ++byte) {
                    char pair[3] = {cursor[2 * (9 - byte)], cursor[2 * (9 - byte) + 1], 0};
                    pending_raw[slot * 10 + byte] = (unsigned char)strtoul(pair, NULL, 16);
                }
                cursor += 20;
            }
            while (*cursor == ' ') ++cursor;
            if (*cursor) { printf("FATAL bad-x87raw\n"); fflush(stdout); return 2; }
            for (int i = 0; i < 3; ++i) {
                pending_raw[80 + 2 * i] = (unsigned char)(fields[i] & 0xff);
                pending_raw[81 + 2 * i] = (unsigned char)(fields[i] >> 8);
            }
            pending_raw_set = 1;
#else
            /* Legacy double lift cannot produce faithful raw80/tags/status. */
            printf("FATAL unsupported-raw-x87-backend\n"); fflush(stdout); return 2;
#endif
        } else if (strncmp(line, "FPIN ", 5) == 0) {
            /* FPIN <control hex> <depth> <double bits hex>... top of stack first. */
            char *cursor = line + 5;
#if HARNESS_X87RAW
            if (pending_raw_set) { printf("FATAL x87raw-with-fpin\n"); fflush(stdout); return 2; }
#endif
            pending_fp_control = (unsigned)strtoul(cursor, &cursor, 16);
            unsigned depth = (unsigned)strtoul(cursor, &cursor, 10);
            if (depth > 7) {
                printf("FATAL bad-fpin-depth\n");
                fflush(stdout);
                return 2;
            }
            pending_fp_depth = depth;
            for (unsigned k = 0; k < depth; k++) {
                pending_fp_bits[k] = strtoull(cursor, &cursor, 16);
            }

        } else if (strncmp(line, "PATCH ", 6) == 0) {
            char *hex = NULL;
            unsigned long addr = strtoul(line + 6, &hex, 16);
            while (*hex == ' ') hex++;
            size_t nbytes = strlen(hex) / 2;

            if (addr < GUEST_LO || addr - GUEST_LO + nbytes > GUEST_SPAN) {
                fprintf(stderr, "PATCH out of range addr=0x%lx len=%zu\n", addr, nbytes);
            } else {
                for (size_t k = 0; k < nbytes; k++) {
                    int hi = hex_nibble(hex[2 * k]);
                    int lo = hex_nibble(hex[2 * k + 1]);
                    g_guest[addr - GUEST_LO + k] = (uint8_t)((hi << 4) | lo);
                }
                /* A patched page must be put back even if this case's function
                 * never writes to it, or its bytes become the NEXT case's
                 * baseline. See the g_page_restore commentary above. */
                if (nbytes > 0) {
                    size_t first = (addr - GUEST_LO) / PAGE_SIZE;
                    size_t last = (addr - GUEST_LO + nbytes - 1) / PAGE_SIZE;
                    for (size_t p = first; p <= last; p++) mark_for_restore((uint32_t)p);
                }
            }

        } else if (strncmp(line, "STUB ", 5) == 0) {
            /* STUB <callee_va> <pop_bytes>: one entry of the table the ORACLE
             * computed. The subject never derives a pop amount for itself, which
             * is what makes "the same callee behaviour on both sides" true by
             * construction instead of by two implementations agreeing. */
            unsigned va = 0, pop = 0;
            if (sscanf(line + 5, "%x %x", &va, &pop) != 2) {
                printf("FATAL bad-stub-line\n");
                fflush(stdout);
                return 2;
            }
            if (harness_stub_add(va, pop) != 0) {
                printf("FATAL stub-table-full\n");
                fflush(stdout);
                return 2;
            }

        } else if (strncmp(line, "PASS ", 5) == 0) {
            unsigned va = 0;
            if (sscanf(line + 5, "%x", &va) != 1 || harness_passthrough_add(va, pending_va) != 0) {
                printf("FATAL unproven-passthrough\n");
                fflush(stdout);
                return 2;
            }
        } else if (strncmp(line, "STUBMODE ", 9) == 0) {
            /* Strict mode. Any call to a VA absent from the table then voids the
             * case instead of running the real callee: running it is exactly the
             * asymmetry that would make the verdict a lie. */
            unsigned on = 0;
            sscanf(line + 9, "%x", &on);
            g_stub_strict = (int)on;

        } else if (strcmp(line, "LISTREPL") == 0) {
#if HARNESS_REPLACEMENT
            size_t count = 0;
            const game_replacement *table = game_replacement_table(&count);
            for (size_t k = 0; k < count; k++) {
                static const char *const cc_names[] = {"cdecl", "stdcall", "thiscall", "fastcall"};
                if (!game_replacement_valid(&table[k])) {
                    printf("ERROR INVALID-REPLACEMENT-ABI\n");
                    break;
                }
                const char *slash = strrchr(table[k].source, '/');
                printf("REPL %08X %s %s %u %s %u %s", table[k].va, table[k].name,
                       cc_names[table[k].convention], (unsigned)table[k].stack_args,
                       table[k].returns_value ? "eax" : "void",
                       game_replacement_scratch(&table[k]),
                       slash != NULL ? slash + 1 : table[k].source);
                if (table[k].input_abi != 0u)
                    printf(" inputs=%s", game_replacement_input_names(&table[k]));
                putchar('\n');
            }
#endif
            printf("END\n");
            fflush(stdout);

        } else if (strcmp(line, "RUN") == 0) {
            recomp_func_t fn = recomp_lookup(pending_va);
            if (!fn) {
                printf("FAULT NOFUNC\nEND\n");
                fflush(stdout);
                continue;
            }
#if HARNESS_REPLACEMENT
            /* A registered address must reach ITS adapter through the dispatch table. If it
             * does not, the weak override did not take (a lower-case digit in the
             * registration, a chunk compiled without its weak header, a stale object set)
             * and the case would silently run the LIFTED body, so an AGREE would be the
             * lifted code agreeing with the oracle and nothing about the replacement.
             * Refused here, loudly, rather than compared. */
            const game_replacement *replacement = find_replacement(pending_va);
            if (replacement != NULL && !game_replacement_valid(replacement)) {
                printf("FAULT INVALID-REPLACEMENT-ABI\nEND\n");
                fflush(stdout);
                continue;
            }
            if (replacement != NULL && (recomp_func_t)replacement->adapter != fn) {
                printf("FAULT NOT-REPLACED\nEND\n");
                fflush(stdout);
                continue;
            }
#endif

            /* Freeze the window; the first store to each page will fault
             * and the handler promotes just that page to RW. This is the
             * step that makes per-case cost O(pages touched). */
            if (mprotect(g_guest, GUEST_SPAN, PROT_READ) != 0) {
                perror("mprotect ro"); return 2;
            }

            g_eax = pending_regs[0]; g_ecx = pending_regs[1];
            g_edx = pending_regs[2]; g_ebx = pending_regs[3];
            g_esp = pending_regs[4]; g_ebp = pending_regs[5];
            g_esi = pending_regs[6]; g_edi = pending_regs[7];
            /* g_seh_ebp is the lifter's "frame pointer currently in effect". A real
             * caller assigns it before transferring control, so a function entered
             * with it stale reads a frame pointer from a previous case. Functions the
             * lifter marks `fpo_leaf` do `ebp = g_seh_ebp` in their prologue and then
             * PUSH that value, so leaving it uninitialised made the subject push a
             * stale word where the hardware pushed EBP -- a memory divergence with
             * perfectly matching registers, which is exactly the case the write-set
             * comparison exists to catch. The initial state must be COMPLETE, not just
             * the eight architectural registers. */
            g_seh_ebp = pending_regs[5];
            g_df = pending_df;
            g_fs_base = HARNESS_FS_BASE;
            /* Cleared per case, not per publish: a case whose executed exit
             * never publishes (a tail call into a stub, a trap) must report
             * mask 0 -- "this exit published nothing" -- rather than the
             * previous case's flags under the previous case's mask. */
            g_harness_eflags = 0;
            g_harness_eflags_mask = 0;
            g_fp_top = (int)((8u - pending_fp_depth) & 7u);
            for (int k = 0; k < 8; k++) g_fp_stack[k] = 0.0;
            for (unsigned k = 0; k < pending_fp_depth; k++) {
                double value;
                memcpy(&value, &pending_fp_bits[k], sizeof value);
                g_fp_stack[((unsigned)g_fp_top + k) & 7u] = value;
            }
            g_fp_control_word = (uint16_t)pending_fp_control;
            if (harness_vector_set) harness_vector_set(pending_xmm, pending_mxcsr);
            if (pending_mxcsr_real) {
                if (!pending_vector) { printf("FATAL mxcsrreal-without-xmmin\n"); fflush(stdout); return 2; }
                harness_mxcsr_load_real(pending_mxcsr);
            }
            g_harness_icall_failures = 0;
#if HARNESS_X87RAW
            if (pending_raw_set) {
                int reset_error = harness_x87_reset(pending_raw);
                if (reset_error) {
                    printf("FATAL x87raw-reset-refused:%d\n", reset_error); fflush(stdout); return 2;
                }
            }
#endif

            g_fault_kind = FAULT_NONE;
            g_fault_addr = 0;
            g_fault_addr_valid = 0;
#if HARNESS_CODEWRITE
            g_cw_stopped = 0;
#endif
#if HARNESS_WITNESS
            __llvm_profile_reset_counters();
#endif
            if (sigsetjmp(g_crash_jmp, 1) == 0) {
                fn();
#if HARNESS_WITNESS
                witness_dump();
#endif
            } else {
#if HARNESS_WITNESS
                witness_dump();
#endif
                if (pending_mxcsr_real) harness_mxcsr_restore_default();
#if HARNESS_CODEWRITE
                if (g_cw_stopped) {
                    /* The state the replacement had when it stopped: registers, write set. */
                    printf("REGS %08X %08X %08X %08X %08X %08X %08X %08X\n",
                           g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
                    printf("CWSTOP %c %08X %u\n", (int)g_cw_kind, g_cw_eip, g_cw_steps);
                    report_writes();
#if HARNESS_REPLACEMENT
                    if (replacement != NULL) printf("REPL 1\n");
#endif
                    printf("END\n");
                    fflush(stdout);
                    continue;
                }
#endif
                if (g_report_fault_addr && g_fault_addr_valid)
                    printf("FAULTADDR %016llX %s\n", (unsigned long long)g_fault_addr,
                           g_fault_addr <= 0xFFFFFFFFull ? "GUEST" : "HOST");
                printf("FAULT %s\nEND\n", g_passthrough_fault ? "PASSTHROUGH-PROOF" : fault_kind_name((fault_kind_t)g_fault_kind));
                fflush(stdout);
                continue;
            }

            if (g_passthrough_fault) {
                printf("FAULT PASSTHROUGH-PROOF\nEND\n");
                fflush(stdout);
                continue;
            }

            if (g_harness_icall_failures) {
                printf("FAULT ICALL\nEND\n");
                fflush(stdout);
                continue;
            }

            /* A call to a callee the oracle stubbed but this side has no entry for.
             * The two sides then ran different code, so there is nothing honest to
             * compare -- the case is voided and counted, never silently compared. */
            if (g_stub_misses) {
                printf("FAULT STUB-MISS:%08X\nEND\n", g_stub_miss_va);
                fflush(stdout);
                continue;
            }

            printf("REGS %08X %08X %08X %08X %08X %08X %08X %08X\n",
                   g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
            /* The lifter's SECOND published frame pointer, which lifter patch 10
             * deliberately does NOT restore because it is a handoff channel to
             * the next callee. It has no hardware counterpart, so it can never be
             * COMPARED against the oracle -- reporting it buys one narrow but real
             * question that no run of this harness could previously answer: did the
             * function under test leave it disagreeing with its own final g_ebp?
             * Seeded equal at entry, so a difference here is the function's own
             * doing. Scored as an observation, never as a verdict. */
            printf("SEHEBP %08X\n", g_seh_ebp);
#if HARNESS_X87RAW
            if (pending_raw_set) {
                /* A body that used the legacy double model changed g_fp_*; its raw state is not
                 * the truth, so refuse rather than report an unchanged raw state. */
                int legacy_used = g_fp_top != 0;
                for (int k = 0; k < 8; k++) if (g_fp_stack[k] != 0.0) legacy_used = 1;
                if (legacy_used) {
                    printf("FATAL x87raw-legacy-model\n"); fflush(stdout); return 2;
                }
                unsigned char out[86];
                harness_x87_copy(out);
                printf("X87RAWOUT 1 %02X%02X %02X%02X %02X%02X", out[81], out[80], out[83], out[82],
                       out[85], out[84]);
                for (int slot = 0; slot < 8; ++slot) {
                    printf(" ");
                    for (int byte = 9; byte >= 0; --byte) printf("%02X", out[slot * 10 + byte]);
                }
                printf("\n");
            } else
#endif
            /* The exit x87 stack: depth, then each slot as double bits, top first. */
            {
                unsigned depth = (8u - (unsigned)g_fp_top) & 7u;
                printf("FPOUT %u", depth);
                for (unsigned k = 0; k < depth; k++) {
                    uint64_t bits;
                    memcpy(&bits, &g_fp_stack[((unsigned)g_fp_top + k) & 7u], sizeof bits);
                    printf(" %016llX", (unsigned long long)bits);
                }
                printf("\n");
                /* The control word at exit, then the same stack as exact 80-bit
                 * registers `<mantissa hex>:<sign+exponent hex>` (a double widens to
                 * extended without loss). A value the oracle holds with more than a
                 * double's precision cannot be held here: the compare says so. */
                printf("FPCW %04X\n", (unsigned)g_fp_control_word);
                /* The lifted x87 model tracks the architectural TOP index. Other status
                 * flags are not synthesized, so publish that exact subset and let the
                 * comparator report only the common modeled bits. */
                printf("FPSTATUS %04X %04X\n",
                       ((unsigned)g_fp_top & 7u) << 11, 0x3800u);
                printf("FPEXT %u", depth);
                for (unsigned k = 0; k < depth; k++) {
                    long double wide = (long double)g_fp_stack[((unsigned)g_fp_top + k) & 7u];
                    unsigned char raw[16] = {0};
                    uint64_t mantissa;
                    uint16_t sign_exponent;
                    memcpy(raw, &wide, 10);
                    memcpy(&mantissa, raw, 8);
                    memcpy(&sign_exponent, raw + 8, 2);
                    printf(" %016llX:%04X", (unsigned long long)mantissa, (unsigned)sign_exponent);
                }
                printf("\n");
            }
            /* The published EFLAGS word and the mask of bits the executed
             * exit's model actually answered. Always printed -- a subject that
             * cannot publish prints `FLAGS 0 0`, and the harness only reads
             * the line from a subject whose READY said eflags=1. */
            printf("FLAGS %08X %03X\n", g_harness_eflags, g_harness_eflags_mask);
            if (pending_vector) {
                uint64_t words[16]; uint32_t control;
                if (pending_mxcsr_real) harness_mxcsr_capture_real();
                harness_vector_get(words, &control);
                printf("XMMOUT %04X", control);
                for (unsigned i = 0; i < 8; ++i)
                    printf(" %016llX%016llX", (unsigned long long)words[i * 2 + 1],
                           (unsigned long long)words[i * 2]);
                printf("\n");
            }
            report_writes();
            /* How many stubbed callees this side actually reached. The oracle counts
             * the same thing, and a mismatch means the two sides took different
             * paths through the caller -- which is a divergence even when the final
             * registers happen to coincide. */
            printf("STUBS %u\n", g_stub_applied);
            printf("PASSES %u\n", g_passthrough_applied);
#if HARNESS_REPLACEMENT
            if (replacement != NULL) printf("REPL 1\n");
#endif
            printf("END\n");
            fflush(stdout);
        }
        /* Unrecognized lines (e.g. a blank trailing line) are ignored. */
    }

    return 0;
}
