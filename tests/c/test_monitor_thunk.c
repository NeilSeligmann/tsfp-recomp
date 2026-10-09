/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Prcb debug-monitor notify: `src/host/monitor_thunk.c`, the injection point in
 * `src/xbox/kernel_thread.c`, and the production seam in `src/host/xdk_thunk.c`.
 *
 * WHAT THIS SUITE EXISTS TO PROVE, and why a dispatcher that compiles proves none of
 * it. The thing being added is a function pointer the GUEST calls, with a
 * callee-cleanup convention. Three independent things can be wrong and none of them
 * crashes:
 *
 *   1. THE VA CAN BE UNREACHABLE. A synthetic address nothing routes is exactly the
 *      failure the four GPU suites shipped with -- a complete boundary with no caller, which
 *      looks like coverage and provides none. So this suite asserts REACHABILITY
 *      through `recomp_lookup_manual`, the function the lifter's own dispatch macros
 *      call first, and not merely that `monitor_thunk_lookup` answers.
 *   2. THE POP CAN BE WRONG. `__stdcall` is callee cleanup, so the pop is ours.
 *      Popping 4 leaves 8 bytes of the caller's arguments on its stack; popping 16
 *      eats the caller's locals. Neither faults. `esp` just never recovers and every
 *      later observation in the run is fiction. MEASURED arity is 2, unanimous across
 *      all 8 call sites and proven by the stack balance in `sub_0037FDE1`; the
 *      per-site table is in `src/host/monitor_thunk.h`.
 *   3. THE NOTIFICATION CAN BE SILENT. A retail console has no monitor block and
 *      never makes this call, so every notification answered here is a deliberate
 *      divergence. An invisible divergence is the one kind this project cannot
 *      afford, so the trace entry and the count are asserted, not assumed.
 *
 * DELIBERATELY FREE OF LIFTED CODE, OF THE XBE AND OF ANY DISC. The dispatcher reads
 * four guest registers by declaration (`src/host/recomp_abi.h`); this file DEFINES
 * those four, which is also what lets the suite SEE the stack arithmetic -- `g_esp` is
 * a variable it owns rather than something it has to infer. Every frame and every
 * control page is laid out in scratch guest memory the way `tests/c/test_kernel_io.c`
 * does. Nothing generated is required, so it runs in a fresh clone.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED by `tools/mutate/c_suites.py`; each test says
 * what breaks it.
 */

#include "monitor_thunk.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_thread.h"
#include "nt_status.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * THE GUEST REGISTER FILE, DEFINED HERE.
 *
 * In the real program these live in `src/host/recomp_runtime.c`, which includes 2.56 M
 * lines of generated headers. Defining the four the dispatcher touches is what lets
 * this suite link the production dispatcher with none of that. The storage class must
 * match recomp_abi.h's declaration exactly or the link is a silent ABI break rather
 * than an error.
 */
TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t a_ = (uint32_t)(actual);                                               \
        uint32_t e_ = (uint32_t)(expected);                                             \
        if (a_ != e_) {                                                                 \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual, \
                   (unsigned)a_, (unsigned)e_);                                         \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint64_t a_ = (uint64_t)(actual);                                               \
        uint64_t e_ = (uint64_t)(expected);                                             \
        if (a_ != e_) {                                                                 \
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__,       \
                   #actual, (unsigned long long)a_, (unsigned long long)e_);            \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

/* --- captured diagnostics ------------------------------------------------- */

static char captured[32768];
static size_t captured_len;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(captured + captured_len, sizeof(captured) - captured_len,
                            format, args);
    va_end(args);
    if (written > 0) {
        captured_len += (size_t)written;
        if (captured_len >= sizeof(captured)) {
            captured_len = sizeof(captured) - 1u;
        }
    }
    return written;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

static void capture_clear(void)
{
    captured[0] = '\0';
    captured_len = 0u;
}

/* --- scratch guest memory ------------------------------------------------- */

#define SCRATCH_BYTES 0x10000u
/* Well clear of the base so a frame can be built in the middle of the region and an
 * over-pop or under-pop lands in untouched space rather than in another structure. */
#define OFF_FRAME 0x0800u

static kernel_guest_ptr scratch;

static bool scratch_init(void)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = SCRATCH_BYTES;
    request.alignment = 0x1000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    return scratch != 0u;
}

/*
 * Lay out exactly what the guest's `call [eax+0x14]` leaves on the stack and point
 * `g_esp` at it. Returns the esp the dispatcher is entered with.
 *
 * Built BY HAND rather than with `kernel_frame_build`, deliberately: that helper
 * writes a zero return address, and the return address is one of the things the trace
 * entry has to carry. A frame whose provenance is a real `push <return addr>` is what
 * the dispatcher will actually see.
 */
static uint32_t build_notify_frame(uint32_t return_address, uint32_t code,
                                   uint32_t pointer)
{
    const uint32_t esp = scratch + OFF_FRAME;
    /* Guest layout at entry: [esp] return address, [esp+4] arg 0, [esp+8] arg 1. The
     * guest pushes `ptr` first and `code` second, so `code` is arg 0. */
    CHECK(kernel_guest_write_u32(esp, return_address));
    CHECK(kernel_guest_write_u32(esp + 4u, code));
    CHECK(kernel_guest_write_u32(esp + 8u, pointer));
    /* Two sentinels above the arguments, so an over-pop is detectable as a position
     * and not only as a number. */
    CHECK(kernel_guest_write_u32(esp + 12u, 0xDEADBEEFu));
    CHECK(kernel_guest_write_u32(esp + 16u, 0xFEEDFACEu));
    g_esp = esp;
    g_eax = 0xA5A5A5A5u;
    return esp;
}

/* --- a control page, built the way the host builds one -------------------- */

#define OFF_CONTROL 0x2000u
#define OFF_TLS 0x3000u
#define OFF_MONITOR 0x4000u

/* Read the callback exactly as the guest does: PrcbData through KPCR+0x20, the monitor
 * block through PrcbData+0x250, the callback through monitor+0x14. Three hops, because
 * a test that read `monitor+0x14` directly would pass even if `+0x250` pointed
 * somewhere else entirely. */
static uint32_t guest_reads_the_callback(uint32_t control_base)
{
    uint32_t prcb = 0u;
    if (!kernel_guest_read_u32(control_base + KERNEL_PCR_PRCB, &prcb)) {
        return 0xFFFFFFFFu;
    }
    uint32_t monitor = 0u;
    if (!kernel_guest_read_u32(prcb + KERNEL_PRCB_MONITOR, &monitor)) {
        return 0xFFFFFFFFu;
    }
    uint32_t callback = 0u;
    if (!kernel_guest_read_u32(monitor + KERNEL_MONITOR_CALLBACK, &callback)) {
        return 0xFFFFFFFFu;
    }
    return callback;
}

static bool build_control_page(void)
{
    return kernel_thread_control_init(scratch + OFF_CONTROL, scratch + OFF_TLS,
                                      scratch + OFF_MONITOR,
                                      KERNEL_THREAD_MONITOR_BYTES);
}

/* --- the tests ------------------------------------------------------------ */

/*
 * The VA is a slot in the mapped window, and the slot it claims to be.
 *
 * BREAKS IF: the VA stops being `base + slot * 4`. A VA of `base + slot` still sits
 * inside the window and still passes every static assertion in monitor_thunk.h, but it
 * DECODES to ordinal 94 -- so the window's own addressing would disagree with the slot
 * this module documents, and a reader of a stop address would be sent to the wrong
 * ordinal. The round trip is the check that catches it.
 *
 * The two containment facts -- above every ordinal, inside the mapped page -- are
 * `_Static_assert`ed in the header instead, because a compile error is strictly better
 * than a test failure for a constant. A mutation that violates either is scored
 * NOT-A-MUTANT by the harness, which is the defence working rather than a gap.
 */
static void test_the_synthetic_va_round_trips_to_its_own_slot(void)
{
    /* The two external facts, pinned as LITERALS first. The round trips below expand
     * both sides from the same macros, so on their own they pass for ANY base or
     * slot value: the window base is hardcoded by the lifter (RECOMP_ICALL_IS_CODE),
     * and the notify VA is MEASURED 0xFE0005EC in the lifted code. A mutation of
     * KERNEL_THUNK_VA_BASE or of the slot arithmetic must fail here. */
    CHECK_EQ_U32(KERNEL_THUNK_VA_BASE, 0xFE000000u);
    CHECK_EQ_U32(MONITOR_THUNK_NOTIFY_VA, 0xFE0005ECu);

    CHECK_EQ_U32(KERNEL_THUNK_ORDINAL(MONITOR_THUNK_NOTIFY_VA),
                 MONITOR_THUNK_NOTIFY_SLOT);
    CHECK_EQ_U32(MONITOR_THUNK_NOTIFY_VA,
                 KERNEL_THUNK_VA_BASE + MONITOR_THUNK_NOTIFY_SLOT * 4u);
    /* Adjacent to the ordinals rather than parked at an arbitrary offset, which is
     * what keeps "the next synthetic callable gets 380" true. */
    CHECK_EQ_U32(MONITOR_THUNK_NOTIFY_SLOT, XBOX_KERNEL_ORDINAL_MAX + 1u);
    /* Inside the page the host maps, asserted here as well as at compile time so the
     * relationship is visible to a reader of the suite. */
    CHECK(MONITOR_THUNK_NOTIFY_VA >= KERNEL_THUNK_VA_BASE);
    CHECK(MONITOR_THUNK_NOTIFY_VA < KERNEL_THUNK_VA_BASE + KERNEL_THUNK_WINDOW_BYTES);
}

/*
 * The lookup answers for exactly one address.
 *
 * BREAKS IF: the comparison becomes a range test. A `>=` would swallow every slot
 * above the ordinals, so a future synthetic callable would silently become a monitor
 * notification -- and it would pop 2 arguments whatever that callable's arity was.
 */
static void test_the_lookup_answers_for_exactly_one_address(void)
{
    CHECK(monitor_thunk_lookup(MONITOR_THUNK_NOTIFY_VA) != NULL);

    CHECK(monitor_thunk_lookup(MONITOR_THUNK_NOTIFY_VA - 4u) == NULL);
    CHECK(monitor_thunk_lookup(MONITOR_THUNK_NOTIFY_VA + 4u) == NULL);
    /* An unaligned address inside the same slot is not the slot. */
    CHECK(monitor_thunk_lookup(MONITOR_THUNK_NOTIFY_VA + 1u) == NULL);
    /* Every real kernel ordinal's own VA, both ends of the range. */
    CHECK(monitor_thunk_lookup(KERNEL_THUNK_VA_BASE) == NULL);
    CHECK(monitor_thunk_lookup(KERNEL_THUNK_VA(XBOX_KERNEL_ORDINAL_MAX)) == NULL);
    /* And addresses that have nothing to do with the window. */
    CHECK(monitor_thunk_lookup(0u) == NULL);
    CHECK(monitor_thunk_lookup(0x0037CBAEu) == NULL);
    CHECK(monitor_thunk_lookup(0xFFFFFFFFu) == NULL);
}

/*
 * THE REACHABILITY TEST, and the most important one in this file.
 *
 * `recomp_lookup_manual` is the function the lifter's own `RECOMP_ICALL_SAFE` macro
 * consults FIRST on every indirect call in the program. If it does not answer for this
 * VA, the module is unreachable no matter how correct it is -- which is exactly the
 * state the four GPU suites shipped in, three complete boundaries with no caller.
 *
 * ASSERTED WITH NO XDK SURFACE ADOPTED, on purpose. That is the state a fresh process
 * is in before `xdk_thunk_init` runs, and the XDK path's own O(1) reject returns NULL
 * for everything in it. So this also pins the ORDER: the monitor check has to come
 * before that reject, not after it.
 *
 * BREAKS IF: the call to `monitor_thunk_lookup` is removed from `recomp_lookup_manual`,
 * or moved below the bounds reject. Either way the guest's call falls through to
 * `recomp_lookup` and `recomp_lookup_kernel`, both of which refuse a slot above
 * XBOX_KERNEL_ORDINAL_MAX, and the run stops at a NULL indirect call again.
 */
static void test_the_production_lookup_routes_the_notify_va(void)
{
    xdk_thunk_shutdown();
    CHECK_EQ_U64(xdk_thunk_count(), 0u);

    recomp_func_t fn = recomp_lookup_manual(MONITOR_THUNK_NOTIFY_VA);
    CHECK(fn != NULL);
    /* The same body the direct entry runs, so the tested route cannot drift from the
     * production route. */
    CHECK(fn == monitor_thunk_lookup(MONITOR_THUNK_NOTIFY_VA));

    /* And it still refuses everything else with no surface adopted, so the new compare
     * did not turn the reject into an accept. */
    CHECK(recomp_lookup_manual(0x003D57D0u) == NULL);
    CHECK(recomp_lookup_manual(KERNEL_THUNK_VA(24u)) == NULL);
}

/*
 * The pop is 12 bytes: the return address plus two arguments.
 *
 * BREAKS IF: the pop becomes 4 (`ret 0`), 8 (`ret 4`) or 16 (`ret 12`). All three
 * compile and none faults. The sentinels above the arguments mean an over-pop is
 * caught as a position rather than only as a number.
 */
static void test_the_dispatcher_pops_the_return_address_and_two_arguments(void)
{
    monitor_thunk_reset_counts();
    monitor_thunk_set_log(capture_printer);
    capture_clear();

    const uint32_t esp = build_notify_frame(0x0037CBB1u, 2u, 0u);
    monitor_thunk_dispatch_at();

    /* 4 for the return address the lifted caller pushed, plus 4 per stack argument. */
    CHECK_EQ_U32(g_esp, esp + 4u + 4u * MONITOR_THUNK_NOTIFY_STACK_ARGS);
    CHECK_EQ_U32(g_esp, esp + 12u);
    /* Spelled out as a literal as well, so a mutation that changes the named constant
     * cannot change the expectation with it. */
    CHECK_EQ_U32(MONITOR_THUNK_NOTIFY_STACK_ARGS, 2u);
    /* esp lands exactly on the first sentinel, which is where the caller's own next
     * push would go. */
    uint32_t at_esp = 0u;
    CHECK(kernel_guest_read_u32(g_esp, &at_esp));
    CHECK_EQ_U32(at_esp, 0xDEADBEEFu);

    /* And eax is a defined zero rather than whatever the dispatcher left behind. */
    CHECK_EQ_U32(g_eax, 0u);

    monitor_thunk_set_log(NULL);
}

/*
 * The handler reads `code` and `ptr` from the slots the guest pushed them into, in the
 * right order.
 *
 * ORDER MATTERS AND IS EASY TO GET BACKWARDS. The guest pushes `ptr` first and `code`
 * second, so `code` is argument 0 at `esp+4`. Swapping them would report a pointer as a
 * notification code and a code as a pointer, and since several sites push 0 for `ptr`,
 * half the trace would read as "code 0" and look plausible.
 *
 * All four MEASURED codes are exercised: 2 on the reboot arm at 0x0037CBAE, 0xC from
 * thread startup at 0x0037FDF5, and 0xA/0xB which carry a stack buffer.
 *
 * BREAKS IF: the argument indices are swapped, or either is read from the wrong offset.
 */
static void test_the_handler_sees_the_code_and_pointer_the_guest_pushed(void)
{
    static const struct {
        uint32_t return_address;
        uint32_t code;
        uint32_t pointer;
    } sites[] = {
        {0x0037CBB1u, 2u, 0u},        /* 0x0037CBAE, push 0 / push 2 */
        {0x0037FDF8u, 0xCu, 0u},      /* 0x0037FDF5, push 0 / push 0xc */
        {0x00383DD0u, 0xAu, 0x41D3F0A4u}, /* 0x00383DCA, lea ecx / push 0xa */
        {0x003846C6u, 0xBu, 0x41D3F120u}, /* 0x003846C0, lea ecx / push 0xb */
    };
    const unsigned site_count = (unsigned)(sizeof(sites) / sizeof(sites[0]));

    monitor_thunk_reset_counts();
    monitor_thunk_set_log(capture_printer);

    for (unsigned i = 0; i < site_count; i++) {
        capture_clear();
        const uint32_t esp = build_notify_frame(sites[i].return_address, sites[i].code,
                                               sites[i].pointer);
        monitor_thunk_dispatch_at();

        uint32_t code = 0xFFFFFFFFu;
        uint32_t pointer = 0xFFFFFFFFu;
        CHECK(monitor_thunk_last_notification(&code, &pointer));
        CHECK_EQ_U32(code, sites[i].code);
        CHECK_EQ_U32(pointer, sites[i].pointer);
        /* Every site pops the same 12, which is the unanimity the measurement found. */
        CHECK_EQ_U32(g_esp, esp + 12u);
    }

    CHECK_EQ_U64(monitor_thunk_notify_count(), site_count);
    CHECK_EQ_U64(monitor_thunk_unreadable_count(), 0u);

    monitor_thunk_set_log(NULL);
}

/*
 * The notification is REPORTED, every time, naming the code.
 *
 * BREAKS IF: the log line is removed, or made once-only. A once-only announcement
 * would hide the second code the guest sent, and the code is the only thing
 * distinguishing one notification from another: `sub_00383FE7` fires 0xB and then 0xA
 * within 0x2F bytes, and a boundary that reported one of them would make the pair look
 * like a single event.
 */
static void test_every_notification_is_announced_with_its_code(void)
{
    monitor_thunk_reset_counts();
    monitor_thunk_set_log(capture_printer);
    capture_clear();

    (void)build_notify_frame(0x003846C6u, 0xBu, 0x41D3F120u);
    monitor_thunk_dispatch_at();
    CHECK(captured_contains("code 11"));
    CHECK(captured_contains("0x41D3F120"));
    /* It says what it did and that a console would not have done it. */
    CHECK(captured_contains("NO-OP"));
    CHECK(captured_contains("retail console"));

    /* The SECOND notification is announced too, not swallowed as a repeat. */
    (void)build_notify_frame(0x003846F5u, 0xAu, 0x41D3F130u);
    monitor_thunk_dispatch_at();
    CHECK(captured_contains("code 10"));
    CHECK_EQ_U64(monitor_thunk_notify_count(), 2u);

    monitor_thunk_set_log(NULL);
}

/*
 * The notification lands in the ONE ordered trace, as its own kind.
 *
 * BOTH DIRECTIONS. It has to appear, and it must NOT appear as a kernel ordinal or an
 * XDK address: those two counts are how a reader judges how much of the console's own
 * surface the guest reached, and a synthetic VA this host invented is not part of it.
 *
 * BREAKS IF: the `thunk_trace_append` call is removed (the notification becomes
 * invisible), or its kind is changed to ORDINAL or XDK (the counts start lying).
 */
static void test_the_notification_appears_in_the_ordered_trace(void)
{
    thunk_trace_reset();
    monitor_thunk_reset_counts();
    monitor_thunk_set_log(capture_printer);
    capture_clear();

    (void)build_notify_frame(0x0037CBB1u, 2u, 0u);
    monitor_thunk_dispatch_at();

    size_t count = 0;
    const thunk_trace_entry *trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 1u);
    if (count >= 1u) {
        CHECK(trace[0].kind == THUNK_KIND_MONITOR);
        CHECK_EQ_U32(trace[0].address, MONITOR_THUNK_NOTIFY_VA);
        CHECK_EQ_U32(trace[0].return_address, 0x0037CBB1u);
        CHECK_EQ_U32(trace[0].ordinal, 0u);
        /* Implemented: a no-op that returns IS the implementation. A false here would
         * print "MISSING" for something that answered. */
        CHECK(trace[0].implemented);
    }

    CHECK_EQ_U64(thunk_trace_total_of_kind(THUNK_KIND_MONITOR), 1u);
    CHECK_EQ_U64(thunk_trace_total_of_kind(THUNK_KIND_ORDINAL), 0u);
    CHECK_EQ_U64(thunk_trace_total_of_kind(THUNK_KIND_XDK), 0u);
    CHECK_EQ_U64(thunk_trace_total(), 1u);

    monitor_thunk_set_log(NULL);
}

/*
 * The per-kind totals are sized by the enum and the grand total counts all of them.
 *
 * BREAKS IF: `g_total` goes back to a two-element array (the monitor kind then writes
 * past it, or is folded into the ordinal column), or `thunk_trace_total` goes back to
 * summing only the first two (the grand total silently under-reports, which is the one
 * failure a total must not have -- it would make a complete trace look truncated and a
 * truncated one look complete).
 */
static void test_the_totals_cover_every_kind(void)
{
    CHECK_EQ_U32((unsigned)THUNK_KIND_COUNT, 3u);

    thunk_trace_reset();
    (void)thunk_trace_append(THUNK_KIND_ORDINAL, 279u, 0u, 0x0037C958u, 0u, false);
    (void)thunk_trace_append(THUNK_KIND_XDK, 0u, 0x003D57D0u, 0x00021200u, 0u, true);
    (void)thunk_trace_append(THUNK_KIND_MONITOR, 0u, MONITOR_THUNK_NOTIFY_VA,
                             0x0037CBB1u, 0u, true);

    CHECK_EQ_U64(thunk_trace_total_of_kind(THUNK_KIND_ORDINAL), 1u);
    CHECK_EQ_U64(thunk_trace_total_of_kind(THUNK_KIND_XDK), 1u);
    CHECK_EQ_U64(thunk_trace_total_of_kind(THUNK_KIND_MONITOR), 1u);
    CHECK_EQ_U64(thunk_trace_total(), 3u);

    /* And a reset clears all three, not two of them. */
    thunk_trace_reset();
    CHECK_EQ_U64(thunk_trace_total(), 0u);
    CHECK_EQ_U64(thunk_trace_total_of_kind(THUNK_KIND_MONITOR), 0u);
}

/*
 * A frame the dispatcher cannot read is counted as such, and STILL POPPED.
 *
 * The pop is not conditional on understanding the arguments, and that is deliberate:
 * the guest has already pushed them, so something must take them off or `esp` desyncs
 * for the rest of the run. Refusing to pop would turn an unreadable diagnostic into a
 * permanent corruption.
 *
 * BREAKS IF: the unreadable path returns early, or stops counting separately. A
 * notification whose arguments could not be read is not the same fact as one that
 * reported code 0, and a single counter cannot tell them apart.
 */
static void test_an_unreadable_frame_is_counted_and_still_popped(void)
{
    monitor_thunk_reset_counts();
    monitor_thunk_set_log(capture_printer);
    capture_clear();

    /* esp of 0 is the one address `kernel_guest_at` refuses outright, so both argument
     * reads fail without needing an unmapped page. */
    g_esp = 0u;
    monitor_thunk_dispatch_at();

    CHECK_EQ_U32(g_esp, 12u);
    CHECK_EQ_U64(monitor_thunk_notify_count(), 1u);
    CHECK_EQ_U64(monitor_thunk_unreadable_count(), 1u);
    CHECK(captured_contains("UNREADABLE"));
    /* Nothing is claimed about what the arguments were. */
    CHECK(!monitor_thunk_last_notification(NULL, NULL));

    monitor_thunk_set_log(NULL);
}

/*
 * Arming writes the VA where the guest will look for it, following the guest's own
 * three hops.
 *
 * BREAKS IF: `kernel_thread_control_init` stops writing `monitor+0x14`, or writes it
 * BEFORE the zero-fill loop that would then erase it. The second is the dangerous one:
 * it compiles, it leaves every other field correct, and the only symptom is a NULL
 * indirect call much later.
 */
static void test_arming_puts_the_va_where_the_guest_reads_it(void)
{
    CHECK(monitor_thunk_arm());
    CHECK(monitor_thunk_is_armed());
    CHECK_EQ_U32(kernel_thread_monitor_callback(), MONITOR_THUNK_NOTIFY_VA);

    CHECK(build_control_page());
    CHECK_EQ_U32(guest_reads_the_callback(scratch + OFF_CONTROL),
                 MONITOR_THUNK_NOTIFY_VA);

    /* And the monitor block's other two measured offsets are STILL ZERO. Arming the
     * notify must not have armed the two optional shared blocks: a non-null +0x20
     * invites a 0x24-byte write and a 0xABCDEF00 handshake we have invented nothing
     * for, and +0x24 invites `[p] = 1`. */
    uint32_t block_a = 0xFFFFFFFFu;
    uint32_t block_b = 0xFFFFFFFFu;
    CHECK(kernel_guest_read_u32(scratch + OFF_MONITOR + KERNEL_MONITOR_BLOCK_A,
                                &block_a));
    CHECK(kernel_guest_read_u32(scratch + OFF_MONITOR + KERNEL_MONITOR_BLOCK_B,
                                &block_b));
    CHECK_EQ_U32(block_a, 0u);
    CHECK_EQ_U32(block_b, 0u);
}

/*
 * Unarmed is the OLD behaviour, exactly: `monitor+0x14` stays zero.
 *
 * This is what makes the arming a decision somebody makes rather than something that
 * happens by linking a library. A host that never calls `monitor_thunk_arm` gets the
 * NULL indirect call and the named stop it got before, which is the honest answer for
 * a host with no callable no-op.
 *
 * BREAKS IF: the injected VA gains a non-zero default, or the write stops being
 * conditional on one having been injected.
 */
static void test_an_unarmed_host_still_gets_a_null_notify(void)
{
    kernel_thread_set_monitor_callback(0u);
    CHECK(!monitor_thunk_is_armed());
    CHECK_EQ_U32(kernel_thread_monitor_callback(), 0u);

    CHECK(build_control_page());
    CHECK_EQ_U32(guest_reads_the_callback(scratch + OFF_CONTROL), 0u);

    /* The gate at 0x00381E76 still reads non-zero, so the GDT path is still skipped.
     * Leaving the notify null must not have reinstated the OTHER blocker. */
    uint32_t prcb = 0u;
    CHECK(kernel_guest_read_u32(scratch + OFF_CONTROL + KERNEL_PCR_PRCB, &prcb));
    uint32_t monitor = 0u;
    CHECK(kernel_guest_read_u32(prcb + KERNEL_PRCB_MONITOR, &monitor));
    CHECK(monitor != 0u);

    /* Re-arm, because the remaining tests and the report expect the armed state. */
    CHECK(monitor_thunk_arm());
}

/*
 * The report states the count, the code and the divergence, and says plainly when
 * nothing was armed.
 *
 * BREAKS IF: the report goes silent on a zero count. "Armed and never called" and "not
 * wired up at all" are different facts, and a boundary that prints nothing cannot tell
 * them apart -- which is the GPU suites' failure again, in reporting form.
 */
static void test_the_report_distinguishes_unarmed_from_uncalled(void)
{
    monitor_thunk_set_log(capture_printer);

    /* Not armed at all. */
    kernel_thread_set_monitor_callback(0u);
    monitor_thunk_reset_counts();
    capture_clear();
    monitor_thunk_report();
    CHECK(captured_contains("NOT ARMED"));
    CHECK(!captured_contains("ARMED AND NEVER CALLED"));

    /* Armed, never called. */
    CHECK(monitor_thunk_arm());
    monitor_thunk_reset_counts();
    capture_clear();
    monitor_thunk_report();
    CHECK(captured_contains("ARMED AND NEVER CALLED"));
    CHECK(!captured_contains("NOT ARMED"));

    /* Armed and called: the count, the last code, and the divergence. */
    (void)build_notify_frame(0x0037CBB1u, 2u, 0u);
    monitor_thunk_dispatch_at();
    capture_clear();
    monitor_thunk_report();
    CHECK(captured_contains("1 answered as a no-op"));
    CHECK(captured_contains("code 2"));
    CHECK(captured_contains("DELIBERATE DIVERGENCE"));
    /* And it names the address and arity, so a reader can check the slot without
     * reading the source. */
    CHECK(captured_contains("2 stack argument(s)"));

    monitor_thunk_set_log(NULL);
}

int main(void)
{
    if (!scratch_init()) {
        printf("FAIL could not allocate scratch guest memory\n");
        return 1;
    }

    test_the_synthetic_va_round_trips_to_its_own_slot();
    test_the_lookup_answers_for_exactly_one_address();
    test_the_production_lookup_routes_the_notify_va();
    test_the_dispatcher_pops_the_return_address_and_two_arguments();
    test_the_handler_sees_the_code_and_pointer_the_guest_pushed();
    test_every_notification_is_announced_with_its_code();
    test_the_notification_appears_in_the_ordered_trace();
    test_the_totals_cover_every_kind();
    test_an_unreadable_frame_is_counted_and_still_popped();
    test_arming_puts_the_va_where_the_guest_reads_it();
    test_an_unarmed_host_still_gets_a_null_notify();
    test_the_report_distinguishes_unarmed_from_uncalled();

    printf("%s: %d checks, %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", checks,
           failures);
    return failures == 0 ? 0 : 1;
}
