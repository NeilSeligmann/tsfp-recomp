/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The address-keyed XDK dispatch path: src/host/xdk_thunk.c and src/host/thunk_trace.c.
 *
 * WHAT THIS SUITE EXISTS TO PROVE, and why a compiling dispatcher proves none of it.
 *
 * `d3d8_hle_call`, `dsound_hle_call` and `xinput_hle_call` were committed, reviewed and
 * tested with NO CALLER anywhere outside their own suites. Three complete boundaries
 * were unreachable, the XDK-surface metric read 0/236, and it would have kept reading
 * zero however much of each module was implemented -- because the surface table proves
 * the functions were FOUND and nothing proved any of them was REACHABLE. That is the
 * "completeness test checks only one direction" failure, and the fix is not a
 * dispatcher that builds. It is these assertions:
 *
 *   1. A call to a known XDK address reaches the RIGHT module's handler, and the WRONG
 *      module's handler is NOT reached. Both directions, because a dispatcher that
 *      called all three would pass a one-directional test.
 *   2. An address with no implementation STOPS the run rather than returning a
 *      fabricated value. A stub returning 0 produces a plausible, wrong trace that
 *      cannot be falsified from the inside, which is this project's worst failure mode.
 *   3. An address with no established calling convention STOPS, and the handler is not
 *      reached. `__stdcall` is callee-cleanup, so a guessed argument count desyncs
 *      `esp` permanently and silently.
 *   4. All three `*_call` entry points have a PRODUCTION caller, counted by the
 *      dispatcher itself, so the thing this work exists to fix is checkable in CI
 *      rather than by inspection.
 *
 * DELIBERATELY FREE OF LIFTED CODE, OF THE XBE AND OF ANY DISC. The dispatcher reads
 * four guest registers by declaration (src/host/recomp_abi.h); this file DEFINES those
 * four, lays every frame out in scratch guest memory exactly as tests/c/test_kernel_io.c
 * does, and injects its own surface rows. Nothing generated is required, so it runs in
 * a fresh clone.
 *
 * ADDRESSES ARE REAL ONES FROM THE RETAIL IMAGE, not invented. The DSOUND and XAPI
 * rows have to be, because `dsound_hle_register` and `xinput_hle_register` refuse an
 * address absent from their compiled-in measured tables -- which is the behaviour that
 * makes a routing test meaningful rather than circular.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED by tools/mutate/c_suites.py; each test says what
 * breaks it.
 */

#include "xdk_thunk.h"

#include "d3d8_hle.h"
#include "dsound_hle.h"
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "monitor_thunk.h"
#include "nt_status.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xinput_hle.h"
#include "xgrph_hle.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

/*
 * THE GUEST REGISTER FILE, DEFINED HERE.
 *
 * In the real program these live in src/host/recomp_runtime.c, which includes 2.56 M
 * lines of generated headers. Defining the four the dispatcher actually touches is
 * what lets this suite link the production dispatcher with none of that -- and it is
 * also the mechanism by which the suite can SEE the dispatcher's stack arithmetic,
 * because `g_esp` is a variable it owns rather than something it has to infer.
 *
 * The storage class must match recomp_abi.h's declaration exactly or the link is a
 * silent ABI break rather than an error.
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
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__,        \
                   #actual, (unsigned long long)a_, (unsigned long long)e_);             \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

/* --- captured diagnostics ------------------------------------------------- */

static char captured[65536];
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
/* Far from the base, so the frame can be built downwards without leaving the region
 * and an underrun lands in untouched space rather than in another structure. */
#define OFF_STACK 0x0800u

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

/* --- the injected surface ------------------------------------------------- */

/*
 * MEASURED addresses from the retail image, taken from the generated surface table.
 * The site counts are in the comments as provenance only; this dispatcher does not
 * rank anything.
 */
#define ADDR_D3D_HOT 0x003D57D0u    /* D3D, 93 sites, the busiest row in the image */
#define ADDR_D3D_CREATE 0x003D9230u /* D3D, Direct3D_CreateDevice, 1 site */
#define ADDR_DSOUND_CREATE 0x00409635u /* DSOUND, DirectSoundCreate, 3 sites */
#define ADDR_XINPUT_GETSTATE 0x0046E36Du /* XPP, XInputGetState, 1 site */
#define ADDR_XGRPH 0x003EF236u      /* XGRPH, 32 sites */

/* Not in the surface. Deliberately four bytes past a row that IS, because
 * "returns a neighbour" is the lookup bug a sorted table invites. */
#define ADDR_NEAR_MISS (ADDR_D3D_HOT + 4u)
/* Deep in game `.text`, i.e. what the overwhelming majority of indirect calls carry. */
#define ADDR_UNROUTED 0x00432700u /* measured XNET ntohs */
#define ADDR_XGRPH_CUBE 0x003E66AEu
#define ADDR_GAME_TEXT 0x00100000u

static const xdk_dispatch_entry SURFACE[] = {
    {ADDR_D3D_HOT, NULL, XDK_MODULE_D3D8},
    {ADDR_D3D_CREATE, "Direct3D_CreateDevice", XDK_MODULE_D3D8},
    {ADDR_DSOUND_CREATE, "DirectSoundCreate", XDK_MODULE_DSOUND},
    {ADDR_XINPUT_GETSTATE, "XInputGetState", XDK_MODULE_XINPUT},
    {ADDR_XGRPH, NULL, XDK_MODULE_XGRPH},
    {ADDR_XGRPH_CUBE, "XGSetCubeTextureHeader", XDK_MODULE_XGRPH},
    {ADDR_UNROUTED, "ntohs", XDK_MODULE_NONE},
};
#define SURFACE_COUNT (sizeof(SURFACE) / sizeof(SURFACE[0]))

/* The D3D module takes its table by injection too, and refuses to register a handler
 * for an address absent from it. Mirrors the two D3D rows above. */
static const d3d8_surface_entry D3D_SURFACE[] = {
    {ADDR_D3D_HOT, NULL, 93u},
    {ADDR_D3D_CREATE, "Direct3D_CreateDevice", 1u},
};
#define D3D_SURFACE_COUNT (sizeof(D3D_SURFACE) / sizeof(D3D_SURFACE[0]))

/* --- recording handlers --------------------------------------------------- */

/*
 * One recorder per module, so "the right one ran" and "the wrong one did not" are two
 * separate observations rather than one.
 *
 * Each records what it saw of the FRAME as well as that it ran, because the frame is
 * the other half of the proof: a dispatcher that called the right module with a frame
 * it had not built would pass a bare did-it-run test.
 */
typedef struct {
    unsigned calls;
    uint32_t arg0;
    uint32_t arg1;
    bool arg1_readable;
    uint32_t ecx;
    uint32_t edx;
    bool registers_supplied;
} recorder;

static recorder d3d_seen;
static recorder dsound_seen;
static recorder xinput_seen;

/* What each handler hands back, chosen distinct and non-zero: zero is what a
 * fabricating stub would return, so a test that accepted zero could not tell a real
 * dispatch from the failure mode this whole path exists to prevent. */
#define D3D_RESULT 0x0D3D0001u
#define DSOUND_RESULT 0x05D50002u
#define XINPUT_RESULT 0x01AB0003u

static void record(recorder *into, void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    into->calls++;
    into->arg0 = 0u;
    into->arg1 = 0u;
    into->arg1_readable = false;
    into->ecx = 0u;
    into->edx = 0u;
    into->registers_supplied = false;
    if (!frame) {
        return;
    }
    (void)kernel_frame_arg(frame, 0u, &into->arg0);
    into->arg1_readable = kernel_frame_arg(frame, 1u, &into->arg1);
    into->registers_supplied = kernel_frame_reg_arg(frame, 0u, &into->ecx)
                              && kernel_frame_reg_arg(frame, 1u, &into->edx);
}

static uint32_t d3d_handler(void *context)
{
    record(&d3d_seen, context);
    return D3D_RESULT;
}

static uint32_t dsound_handler(void *context)
{
    record(&dsound_seen, context);
    return DSOUND_RESULT;
}

static uint32_t xinput_handler(void *context)
{
    record(&xinput_seen, context);
    return XINPUT_RESULT;
}

static void clear_recorders(void)
{
    memset(&d3d_seen, 0, sizeof(d3d_seen));
    memset(&dsound_seen, 0, sizeof(dsound_seen));
    memset(&xinput_seen, 0, sizeof(xinput_seen));
}

/* --- the harness ---------------------------------------------------------- */

/* A value `eax` cannot plausibly acquire, so "the dispatcher left eax alone" is
 * observable rather than inferred from a zero that a stub would also produce. */
#define EAX_POISON 0xFEEDFACEu

#define RETURN_ADDRESS 0x0002A8E4u /* a real call site of ADDR_D3D_HOT */

static bool stopped;
static host_stop stop_record;
static uint32_t esp_at_entry;

/* Lay out a __stdcall frame at the top of the scratch region: the return address at
 * `g_esp`, then `arg_count` arguments above it, exactly the shape kernel_call.h
 * describes. Nothing here uses kernel_frame_build: the point is to feed the dispatcher
 * raw guest memory and let IT build the frame. */
static void prepare_frame(const uint32_t *args, unsigned arg_count)
{
    const kernel_guest_ptr base = scratch + OFF_STACK;
    g_esp = base;
    CHECK(kernel_guest_write_u32(base, RETURN_ADDRESS));
    for (unsigned i = 0; i < arg_count; i++) {
        CHECK(kernel_guest_write_u32(base + 4u + 4u * i, args[i]));
    }
    esp_at_entry = g_esp;
    g_eax = EAX_POISON;
}

/*
 * Dispatch through the PRODUCTION path and record how it ended.
 *
 * `recomp_lookup_manual` is the function the lifter's own dispatch macros call, so
 * going through it rather than reaching inside the module means this suite exercises
 * the same two steps the generated code does: resolve, then call. A test that called
 * an internal dispatch function directly would not notice if the lookup stopped
 * answering at all.
 */
static bool dispatch(uint32_t address)
{
    recomp_func_t fn = recomp_lookup_manual(address);
    if (!fn) {
        return false;
    }
    stopped = false;
    memset(&stop_record, 0, sizeof(stop_record));
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        fn();
    } else {
        stopped = true;
        stop_record = *host_run_result();
    }
    host_run_disarm();
    return true;
}

static void reset_all(void)
{
    capture_clear();
    clear_recorders();
    thunk_trace_reset();
    const xgrph_surface_entry utilities[] = {
        {ADDR_XGRPH, NULL, 32u}, {ADDR_XGRPH_CUBE, "XGSetCubeTextureHeader", 2u}};
    CHECK(xgrph_hle_init(utilities, 2u));
    xgrph_hle_set_log(capture_printer);
    dsound_hle_init();
    xinput_hle_init();
    CHECK(d3d8_hle_init(D3D_SURFACE, D3D_SURFACE_COUNT));
    CHECK(xdk_thunk_init(SURFACE, SURFACE_COUNT));
    xdk_thunk_set_stop_on_missing(true);
    xdk_thunk_set_log(capture_printer);
    d3d8_hle_set_log(capture_printer);
    dsound_hle_set_log(capture_printer);
    xinput_hle_set_log(capture_printer);
}

/* Register all three recorders and give all three a hand-established ABI, so a routing
 * test is about routing and not about one module happening to be the only one wired. */
static void implement_all_three(void)
{
    CHECK(d3d8_hle_register(ADDR_D3D_HOT, d3d_handler));
    CHECK(dsound_hle_register(ADDR_DSOUND_CREATE, dsound_handler));
    CHECK(xinput_hle_register(ADDR_XINPUT_GETSTATE, xinput_handler));
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 2u, 0u));
    CHECK(xdk_thunk_declare_abi(ADDR_DSOUND_CREATE, XDK_CC_STDCALL, 3u, 0u));
    CHECK(xdk_thunk_declare_abi(ADDR_XINPUT_GETSTATE, XDK_CC_STDCALL, 2u, 0u));
}

/* ======================================================================== */

/*
 * THE LOOKUP ANSWERS FOR EXACTLY THE MEASURED ADDRESSES AND NOTHING ELSE.
 *
 * Both directions, and the negative half is the important one. `recomp_lookup_manual`
 * runs on EVERY indirect call in the lifted program, so a lookup that claimed one
 * address too many would hijack a call to a game function and route it into an HLE
 * module -- which would then refuse it for want of an ABI and stop the run at an
 * address nothing is wrong with.
 *
 * BREAKS THIS: a lookup that returns a neighbouring row instead of NULL on a miss; a
 * range test that is inclusive at the wrong end; dropping the exact-match test.
 */
static void test_the_lookup_answers_for_measured_addresses_only(void)
{
    reset_all();
    CHECK(recomp_lookup_manual(ADDR_D3D_HOT) != NULL);
    CHECK(recomp_lookup_manual(ADDR_DSOUND_CREATE) != NULL);
    CHECK(recomp_lookup_manual(ADDR_XINPUT_GETSTATE) != NULL);
    CHECK(recomp_lookup_manual(ADDR_XGRPH) != NULL);

    /* Four bytes past a row that exists. A sorted table searched sloppily answers
     * here, and the answer is the previous row -- a wrong function, silently. */
    CHECK(recomp_lookup_manual(ADDR_NEAR_MISS) == NULL);
    CHECK(recomp_lookup_manual(ADDR_D3D_HOT - 4u) == NULL);
    CHECK(recomp_lookup_manual(ADDR_GAME_TEXT) == NULL);
    CHECK(recomp_lookup_manual(0u) == NULL);
    CHECK(recomp_lookup_manual(0xFFFFFFFFu) == NULL);

    CHECK(xdk_thunk_is_target(ADDR_D3D_HOT));
    CHECK(!xdk_thunk_is_target(ADDR_NEAR_MISS));
    CHECK_EQ_U32(xdk_thunk_count(), SURFACE_COUNT);
}

/*
 * A CALL REACHES THE RIGHT MODULE, AND THE WRONG MODULES ARE NOT REACHED.
 *
 * The negative half is not padding. A dispatcher that fanned a call out to all three
 * modules, or that routed everything to whichever module came first in a chain of
 * `if`s, would satisfy "the D3D handler ran" and be comprehensively wrong. Each case
 * therefore asserts one recorder at 1 and the other two at 0.
 *
 * It also asserts the RESULT reaches `eax`. A dispatcher that called the handler and
 * discarded its answer would pass a call-count test and hand the guest a zero, which
 * is the fabrication this path exists to prevent.
 *
 * BREAKS THIS: routing D3D8 to dsound_hle_call or vice versa; dropping the `break` in
 * the routing switch; returning 0 instead of the handler's value.
 */
static void test_a_call_reaches_the_right_module_and_not_the_others(void)
{
    const uint32_t args[3] = {0x11112222u, 0x33334444u, 0x55556666u};

    reset_all();
    implement_all_three();

    prepare_frame(args, 3u);
    CHECK(dispatch(ADDR_D3D_HOT));
    CHECK(!stopped);
    CHECK_EQ_U32(d3d_seen.calls, 1u);
    CHECK_EQ_U32(dsound_seen.calls, 0u);
    CHECK_EQ_U32(xinput_seen.calls, 0u);
    CHECK_EQ_U32(g_eax, D3D_RESULT);

    clear_recorders();
    prepare_frame(args, 3u);
    CHECK(dispatch(ADDR_DSOUND_CREATE));
    CHECK(!stopped);
    CHECK_EQ_U32(dsound_seen.calls, 1u);
    CHECK_EQ_U32(d3d_seen.calls, 0u);
    CHECK_EQ_U32(xinput_seen.calls, 0u);
    CHECK_EQ_U32(g_eax, DSOUND_RESULT);

    clear_recorders();
    prepare_frame(args, 3u);
    CHECK(dispatch(ADDR_XINPUT_GETSTATE));
    CHECK(!stopped);
    CHECK_EQ_U32(xinput_seen.calls, 1u);
    CHECK_EQ_U32(d3d_seen.calls, 0u);
    CHECK_EQ_U32(dsound_seen.calls, 0u);
    CHECK_EQ_U32(g_eax, XINPUT_RESULT);
}

/*
 * THE THREE `*_call` ENTRY POINTS HAVE A PRODUCTION CALLER, COUNTED BY THE DISPATCHER.
 *
 * This is the assertion the whole task exists to make checkable in CI rather than by
 * inspection. `xdk_thunk_module_call_count` is incremented inside
 * `src/host/xdk_thunk.c` immediately before it calls `d3d8_hle_call`,
 * `dsound_hle_call` or `xinput_hle_call`, and nowhere else -- so a nonzero value
 * cannot be produced by this suite calling a module directly, only by production code
 * routing into it.
 *
 * BREAKS THIS: deleting any arm of the routing switch; moving the counter so it counts
 * attempts rather than calls; a dispatcher that compiles and is never reached.
 */
static void test_all_three_modules_have_a_production_caller(void)
{
    const uint32_t args[3] = {1u, 2u, 3u};

    reset_all();
    implement_all_three();
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_D3D8), 0u);

    prepare_frame(args, 3u);
    CHECK(dispatch(ADDR_D3D_HOT));
    prepare_frame(args, 3u);
    CHECK(dispatch(ADDR_DSOUND_CREATE));
    prepare_frame(args, 3u);
    CHECK(dispatch(ADDR_XINPUT_GETSTATE));

    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_D3D8), 1u);
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_DSOUND), 1u);
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_XINPUT), 1u);
    /* Nothing was routed to a section with no module, and nothing was refused. */
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_NONE), 0u);
    CHECK_EQ_U64(xdk_thunk_abi_refused_count(), 0u);
    CHECK_EQ_U64(xdk_thunk_unknown_count(), 0u);
    CHECK_EQ_U64(xdk_thunk_call_count(), 3u);

    /* And the modules agree they were called, from their own side of the boundary. */
    const d3d8_entry *d3d = d3d8_hle_entry(ADDR_D3D_HOT);
    CHECK(d3d != NULL && d3d->call_count == 1u);
    const dsound_entry *dsound = dsound_hle_entry(ADDR_DSOUND_CREATE);
    CHECK(dsound != NULL && dsound->call_count == 1u);
    const xinput_entry *xinput = xinput_hle_entry(ADDR_XINPUT_GETSTATE);
    CHECK(xinput != NULL && xinput->call_count == 1u);
}

/*
 * THE DISPATCHER BUILDS THE FRAME, AND THE HANDLER SEES THE GUEST'S OWN ARGUMENTS.
 *
 * The frame is the second half of the reachability proof. `kernel_frame_arg(frame, 0)`
 * must read the dword at `g_esp + 4`, which is the guest's leftmost argument, and the
 * registers must arrive marked as supplied -- without that flag a fastcall or thiscall
 * handler cannot tell a caller that forgot to set ecx from one passing a legitimate
 * zero, and zero is a perfectly plausible `this`.
 *
 * NOTE WHAT THIS ALSO ESTABLISHES: no change was needed to any of the three `*_call`
 * signatures. All three already took `void *context`, and `context` is a
 * `kernel_call_frame *` -- the same type the kernel boundary passes. One frame type
 * serves both boundaries and all four conventions.
 *
 * BREAKS THIS: pointing `stack_ptr` at `g_esp + 4` instead of `g_esp`, which shifts
 * every argument by one slot; failing to call kernel_frame_set_registers, which leaves
 * `has_registers` false and makes every register argument unreadable.
 */
static void test_the_handler_sees_the_frame_the_dispatcher_built(void)
{
    const uint32_t args[2] = {0xA0A0A0A0u, 0xB1B1B1B1u};

    reset_all();
    implement_all_three();
    prepare_frame(args, 2u);
    g_ecx = 0xC0FFEE01u;
    g_edx = 0xDEADBEE2u;

    CHECK(dispatch(ADDR_D3D_HOT));
    CHECK(!stopped);
    CHECK_EQ_U32(d3d_seen.calls, 1u);
    CHECK_EQ_U32(d3d_seen.arg0, args[0]);
    CHECK(d3d_seen.arg1_readable);
    CHECK_EQ_U32(d3d_seen.arg1, args[1]);
    CHECK(d3d_seen.registers_supplied);
    CHECK_EQ_U32(d3d_seen.ecx, 0xC0FFEE01u);
    CHECK_EQ_U32(d3d_seen.edx, 0xDEADBEE2u);
}

/*
 * AN ADDRESS WITH NO IMPLEMENTATION STOPS, AND FABRICATES NOTHING.
 *
 * `ADDR_D3D_CREATE` is in the surface and has an ABI declared, so the ONLY thing
 * missing is an implementation. All three modules would happily answer it as a STUB
 * that reports once and returns its default -- which is exactly the plausible, wrong
 * value a trace cannot be falsified against -- so the dispatcher must check the entry
 * state itself and stop before the module is ever asked.
 *
 * Three separate things are asserted, because each can fail alone:
 *   - the stop reason and the address it names;
 *   - `eax` and `esp` untouched, i.e. nothing was fabricated and no cleanup happened;
 *   - the call is IN THE TRACE, recorded BEFORE the stop, so the trace ENDS WITH the
 *     thing that stopped it rather than without it.
 *
 * BREAKS THIS: inverting the stop policy; treating a module STUB as implemented;
 * appending to the trace after the stop instead of before it, which drops the last
 * entry entirely because host_run_stop never returns.
 */
static void test_an_unimplemented_address_stops_rather_than_returning_zero(void)
{
    const uint32_t args[1] = {0x77778888u};

    reset_all();
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_CREATE, XDK_CC_STDCALL, 1u, 0u));
    prepare_frame(args, 1u);

    CHECK(dispatch(ADDR_D3D_CREATE));
    CHECK(stopped);
    CHECK_EQ_U32(stop_record.reason, HOST_STOP_XDK_UNIMPLEMENTED);
    CHECK_EQ_U32(stop_record.guest_address, ADDR_D3D_CREATE);
    /* The detail is the row's `.XTLID` name, so an operator reading the stop knows
     * what to write rather than only where. */
    CHECK(stop_record.detail != NULL
          && strcmp(stop_record.detail, "Direct3D_CreateDevice") == 0);

    CHECK_EQ_U32(g_eax, EAX_POISON);
    CHECK_EQ_U32(g_esp, esp_at_entry);
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_D3D8), 0u);

    size_t count = 0;
    const thunk_trace_entry *trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 1u);
    if (count == 1u) {
        CHECK_EQ_U32(trace[0].kind, THUNK_KIND_XDK);
        CHECK_EQ_U32(trace[0].address, ADDR_D3D_CREATE);
        CHECK_EQ_U32(trace[0].return_address, RETURN_ADDRESS);
        CHECK(!trace[0].implemented);
        CHECK(!trace[0].result_known);
    }
}

/*
 * `--continue-on-missing` RELAXES THAT, AND ONLY THAT.
 *
 * The pair exists so the two paths can be compared rather than one assumed, exactly as
 * it does on the kernel side. With the policy off the module's own stub answers, the
 * announced default reaches `eax`, and the stack discipline STILL happens -- which is
 * the part that matters, because a continued run with an unpopped frame desyncs `esp`
 * and every later observation is fiction.
 *
 * BREAKS THIS: making the relaxation unconditional; skipping the pop on the stub path.
 */
static void test_continue_on_missing_relaxes_the_stop_but_not_the_cleanup(void)
{
    const uint32_t args[1] = {0x77778888u};

    reset_all();
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_CREATE, XDK_CC_STDCALL, 1u, 0u));
    CHECK(d3d8_hle_set_default_return(ADDR_D3D_CREATE, 0x8876000Au));
    xdk_thunk_set_stop_on_missing(false);
    prepare_frame(args, 1u);

    CHECK(dispatch(ADDR_D3D_CREATE));
    CHECK(!stopped);
    CHECK_EQ_U32(g_eax, 0x8876000Au);
    /* One argument plus the return address. */
    CHECK_EQ_U32(g_esp, esp_at_entry + 8u);
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_D3D8), 1u);

    size_t count = 0;
    const thunk_trace_entry *trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 1u);
    if (count == 1u) {
        /* Still marked MISSING -- the run continued, it did not become implemented --
         * but the result the guest actually received is recorded. */
        CHECK(!trace[0].implemented);
        CHECK_EQ_U32(trace[0].result, 0x8876000Au);
        CHECK(trace[0].result_known);
    }
}

/*
 * AN ADDRESS WITH NO ESTABLISHED CONVENTION STOPS, EVEN WITH A HANDLER WAITING.
 *
 * This is the arity refusal, and it is the one check in this suite that protects
 * against a failure that does not crash. `__stdcall` and `__thiscall` are
 * callee-cleanup: the dispatcher pops what the caller pushed. Over-popping eats the
 * caller's own locals and `esp` never recovers; under-popping leaves bytes behind and
 * the damage surfaces arbitrarily far from its cause. Neither aborts. Both produce a
 * plausible, wrong trace.
 *
 * So the handler must NOT be reached. A dispatcher that called it and then guessed at
 * the cleanup would pass a test that only looked at the return value.
 *
 * BREAKS THIS: removing the refusal and defaulting to zero arguments; making the
 * refusal conditional on --continue-on-missing, which would relax the one policy that
 * exists because we do not KNOW something rather than because nobody has WRITTEN
 * something.
 */
static void test_an_address_with_no_abi_is_refused_and_the_handler_is_not_reached(void)
{
    const uint32_t args[2] = {1u, 2u};

    reset_all();
    CHECK(d3d8_hle_register(ADDR_D3D_HOT, d3d_handler));
    CHECK(!xdk_thunk_abi_known(ADDR_D3D_HOT));
    prepare_frame(args, 2u);

    CHECK(dispatch(ADDR_D3D_HOT));
    CHECK(stopped);
    CHECK_EQ_U32(stop_record.reason, HOST_STOP_XDK_ABI_UNKNOWN);
    CHECK_EQ_U32(stop_record.guest_address, ADDR_D3D_HOT);
    CHECK_EQ_U32(d3d_seen.calls, 0u);
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_D3D8), 0u);
    CHECK_EQ_U64(xdk_thunk_abi_refused_count(), 1u);
    CHECK_EQ_U32(g_eax, EAX_POISON);
    CHECK_EQ_U32(g_esp, esp_at_entry);
    /* The diagnostic names the address, which is what makes the stop a bug report. */
    CHECK(captured_contains("0x003D57D0"));
    CHECK(captured_contains("no calling convention"));

    /* And it stays refused under --continue-on-missing: that flag relaxes "nobody has
     * written this yet", not "we do not know how to unwind this". */
    xdk_thunk_set_stop_on_missing(false);
    prepare_frame(args, 2u);
    CHECK(dispatch(ADDR_D3D_HOT));
    CHECK(stopped);
    CHECK_EQ_U32(stop_record.reason, HOST_STOP_XDK_ABI_UNKNOWN);
    CHECK_EQ_U32(d3d_seen.calls, 0u);
}

/*
 * THE MEASURED-ARITY QUORUM, MIRRORING `stack_args_for()` EXACTLY.
 *
 * Both gates, because each catches a different real failure the kernel side measured:
 *
 *   - NON-UNANIMOUS is refused. Of 13 kernel ordinals whose arity was independently
 *     verified against their call sites, the measured table was WRONG for 6, and all
 *     six are flagged non-unanimous. The flag was right and the code was ignoring it.
 *   - FEWER THAN THREE VOTERS is refused, because "unanimous" over one site is a
 *     tautology: one voter always agrees with itself. Ordinal 196 is the case that
 *     proved it -- 12 arguments, 1 site, flagged unanimous, and 12 is more than that
 *     export takes. Over-popping is the worse direction.
 *
 * A HAND VERIFICATION GETS NO QUORUM TEST, and that asymmetry is deliberate: a count
 * read off the guest's own call sites by a person is evidence, not a vote.
 *
 * BREAKS THIS: lowering XDK_MEASURED_ARITY_MIN_SITES to 1 or 2; accepting a
 * non-unanimous row; applying the quorum to the hand path as well, which would make
 * the one mechanism that can carry a count this process cannot establish useless.
 */
static void test_the_measured_arity_quorum_refuses_what_it_should(void)
{
    reset_all();

    /* Non-unanimous, however many sites. */
    CHECK(!xdk_thunk_declare_measured_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 2u, 0u, 93u,
                                          false));
    CHECK(!xdk_thunk_abi_known(ADDR_D3D_HOT));
    CHECK(captured_contains("did not agree"));

    /* Unanimous but with one voter: the tautology. */
    capture_clear();
    CHECK(!xdk_thunk_declare_measured_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 2u, 0u, 1u,
                                          true));
    CHECK(!xdk_thunk_abi_known(ADDR_D3D_HOT));
    CHECK(captured_contains("always agrees with itself"));

    /* Two voters is still refused: two correlated sites are closer to one than three. */
    CHECK(!xdk_thunk_declare_measured_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 2u, 0u, 2u,
                                          true));
    CHECK(!xdk_thunk_abi_known(ADDR_D3D_HOT));

    /* Three, unanimous: corroborated, so accepted. */
    CHECK(xdk_thunk_declare_measured_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 2u, 0u,
                                         XDK_MEASURED_ARITY_MIN_SITES, true));
    CHECK(xdk_thunk_abi_known(ADDR_D3D_HOT));

    /* A hand verification needs no quorum at all, and one site is fine for it. */
    reset_all();
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_CREATE, XDK_CC_STDCALL, 1u, 0u));
    CHECK(xdk_thunk_abi_known(ADDR_D3D_CREATE));

    /* An address outside the surface cannot acquire an ABI by either route: that would
     * mean the caller and the measurement disagree, and accepting it hides which. */
    CHECK(!xdk_thunk_declare_abi(ADDR_NEAR_MISS, XDK_CC_STDCALL, 1u, 0u));
    CHECK(!xdk_thunk_declare_measured_abi(ADDR_NEAR_MISS, XDK_CC_STDCALL, 1u, 0u, 9u,
                                          true));
    CHECK(captured_contains("not in the measured surface"));
}

/*
 * A CONVENTION THAT DOES NOT HAVE THE DECLARED SHAPE IS REFUSED.
 *
 * `register_args` is not a free parameter. Nothing travels in a register under stdcall
 * or cdecl, exactly one thing does under thiscall (`this`, in ecx), and one or two do
 * under fastcall. A declaration that disagrees is a caller who has the convention
 * wrong, and accepting it would attach registers to a frame whose handler reads the
 * stack -- or, worse, subtract register arguments from a stack pop that never had them.
 *
 * BREAKS THIS: dropping any arm of abi_shape_ok; letting the stack-argument bound go.
 */
static void test_an_incoherent_convention_is_refused(void)
{
    reset_all();
    CHECK(!xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 1u, 1u));
    CHECK(!xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_CDECL, 1u, 2u));
    CHECK(!xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_THISCALL, 1u, 0u));
    CHECK(!xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_THISCALL, 1u, 2u));
    CHECK(!xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_FASTCALL, 1u, 0u));
    CHECK(!xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_FASTCALL, 1u, 3u));
    CHECK(!xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_STDCALL,
                                 XDK_ABI_MAX_STACK_ARGS + 1u, 0u));
    CHECK(!xdk_thunk_abi_known(ADDR_D3D_HOT));

    /* The shapes that ARE coherent, all four of them. */
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 4u, 0u));
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_CDECL, 4u, 0u));
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_THISCALL, 4u, 1u));
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_FASTCALL, 4u, 2u));
}

/*
 * THE CLEANUP IS PER CONVENTION, AND `esp` IS MEASURED RATHER THAN ASSERTED IN THE
 * ABSTRACT.
 *
 * Every case dispatches for real and reads `g_esp` afterwards, because that is the
 * number the guest's next instruction depends on. `xdk_thunk_pop_bytes` is checked
 * alongside it so a disagreement between the arithmetic and the dispatcher shows up as
 * a failure here rather than as a desync a hundred calls later.
 *
 * CDECL IS THE ROW THAT MATTERS. Under cdecl the CALLER cleans up, so the dispatcher
 * must pop the return address and NOTHING ELSE however many arguments there are.
 * Popping them as well eats the caller's locals -- and the lifter's own per-function
 * headers classify several D3D entries as cdecl, so this is live rather than
 * theoretical.
 *
 * BREAKS THIS: one pop formula for all four conventions; counting fastcall's register
 * arguments in the stack pop; popping arguments under cdecl.
 */
static void test_the_cleanup_matches_the_convention(void)
{
    const uint32_t args[4] = {1u, 2u, 3u, 4u};
    uint32_t pop = 0u;

    reset_all();
    CHECK(d3d8_hle_register(ADDR_D3D_HOT, d3d_handler));

    /* stdcall, 3 stack arguments: return address plus 3 dwords. */
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 3u, 0u));
    CHECK(xdk_thunk_pop_bytes(ADDR_D3D_HOT, &pop));
    CHECK_EQ_U32(pop, 16u);
    prepare_frame(args, 4u);
    CHECK(dispatch(ADDR_D3D_HOT));
    CHECK(!stopped);
    CHECK_EQ_U32(g_esp, esp_at_entry + 16u);

    /* thiscall: `this` in ecx, so the same pop as stdcall for the same stack count. */
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_THISCALL, 3u, 1u));
    CHECK(xdk_thunk_pop_bytes(ADDR_D3D_HOT, &pop));
    CHECK_EQ_U32(pop, 16u);
    prepare_frame(args, 4u);
    CHECK(dispatch(ADDR_D3D_HOT));
    CHECK_EQ_U32(g_esp, esp_at_entry + 16u);

    /* fastcall with 2 in registers and 1 on the stack: the register pair is NOT on the
     * stack and must not be popped. */
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_FASTCALL, 1u, 2u));
    CHECK(xdk_thunk_pop_bytes(ADDR_D3D_HOT, &pop));
    CHECK_EQ_U32(pop, 8u);
    prepare_frame(args, 4u);
    CHECK(dispatch(ADDR_D3D_HOT));
    CHECK_EQ_U32(g_esp, esp_at_entry + 8u);

    /* cdecl: the return address only, however many arguments were pushed. */
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_CDECL, 4u, 0u));
    CHECK(xdk_thunk_pop_bytes(ADDR_D3D_HOT, &pop));
    CHECK_EQ_U32(pop, 4u);
    prepare_frame(args, 4u);
    CHECK(dispatch(ADDR_D3D_HOT));
    CHECK_EQ_U32(g_esp, esp_at_entry + 4u);

    /* A zero-argument stdcall still pops its return address. */
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 0u, 0u));
    CHECK(xdk_thunk_pop_bytes(ADDR_D3D_HOT, &pop));
    CHECK_EQ_U32(pop, 4u);
}

/*
 * A MEASURED SECTION WITH NO MODULE STOPS UNCONDITIONALLY.
 *
 * XNET (41/156), XONLINE (30/58) and XMV (7/7) are
 * measured boundaries with nothing behind them. There is no module to supply a default,
 * so there is no honest value to return -- which is why this one is NOT relaxed by
 * --continue-on-missing. That flag means "proceed past something nobody has written
 * yet, using its announced default"; here there is nothing to announce one.
 *
 * BREAKS THIS: a routing switch with a `default:` arm that picks a module; honouring
 * --continue-on-missing here, which would hand the guest a fabricated zero from a
 * subsystem that does not exist.
 */
static void test_a_section_with_no_module_stops_whatever_the_policy(void)
{
    const uint32_t args[1] = {1u};

    reset_all();
    /* Even with an ABI declared, so the refusal cannot be mistaken for the arity one. */
    CHECK(xdk_thunk_declare_abi(ADDR_UNROUTED, XDK_CC_STDCALL, 1u, 0u));
    prepare_frame(args, 1u);
    CHECK(dispatch(ADDR_UNROUTED));
    CHECK(stopped);
    CHECK_EQ_U32(stop_record.reason, HOST_STOP_XDK_UNROUTED);
    CHECK_EQ_U32(stop_record.guest_address, ADDR_UNROUTED);
    CHECK_EQ_U32(g_eax, EAX_POISON);
    CHECK_EQ_U32(g_esp, esp_at_entry);
    CHECK_EQ_U64(xdk_thunk_unrouted_count(), 1u);
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_D3D8), 0u);
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_DSOUND), 0u);
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_XINPUT), 0u);

    xdk_thunk_set_stop_on_missing(false);
    prepare_frame(args, 1u);
    CHECK(dispatch(ADDR_UNROUTED));
    CHECK(stopped);
    CHECK_EQ_U32(stop_record.reason, HOST_STOP_XDK_UNROUTED);
}

/*
 * SECTION NAMES MAP TO MODULES IN EXACTLY ONE PLACE, AND THE UNOWNED ONES STAY UNOWNED.
 *
 * `main.c` adopts the generated table by walking it through this function, so a
 * mapping that drifted would route a whole section into the wrong module at once. The
 * sections with no module are asserted explicitly rather than left to a default,
 * because "we have no module for XNET" is a fact worth failing a test over if it
 * silently changes.
 *
 * BREAKS THIS: adding a `default:` that guesses; mapping XGRPH to D3D8 on the grounds
 * that both are graphics -- XGRPH is the texture and shader utility library, not the
 * driver, and XGAssembleShader is not a D3D function.
 */
/*
 * THE CALLEE PATH, and the point is what it does NOT gate on.
 *
 * 165 of the 199 addresses whose arity was read from their own `ret imm16` have fewer
 * than three call sites. Routing them through the measured path would refuse every one,
 * despite the number being stated outright by the instruction rather than inferred from
 * a vote. So this path has no quorum, and these cases pin that absence -- a future
 * tidy-up that "unified" the three entry points would silently destroy 83% of the
 * measurement, and nothing else would fail.
 */
static void test_the_callee_path_has_no_quorum_because_sites_are_not_evidence(void)
{
    reset_all();

    /* ONE terminator, which the measured path would reject as a tautology if it were a
     * site. It is not a site: `ret imm16` states the byte count outright. */
    CHECK(xdk_thunk_declare_callee_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 2u, 0u, 1u, true));
    CHECK(xdk_thunk_abi_known(ADDR_D3D_HOT));

    /* And the same address, same numbers, through the measured path with one voter is
     * REFUSED. Asserting both in one test is what makes the distinction visible: the two
     * paths disagree on identical inputs, on purpose. */
    reset_all();
    CHECK(!xdk_thunk_declare_measured_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 2u, 0u, 1u,
                                          true));
    CHECK(!xdk_thunk_abi_known(ADDR_D3D_HOT));
}

static void test_the_callee_path_refuses_a_conflict_rather_than_reducing_it(void)
{
    reset_all();

    /* Two returns disagreeing means the control-flow walk crossed a function boundary.
     * A push count can only be inflated by a register save, so reducing it is defensible;
     * a wrong EXTENT is not biased in any knowable direction, so reducing THIS would
     * launder an extent bug into an arity. */
    CHECK(!xdk_thunk_declare_callee_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 2u, 0u, 2u, false));
    CHECK(!xdk_thunk_abi_known(ADDR_D3D_HOT));
    CHECK(captured_contains("the extent is wrong"));

    /* No terminator read at all is nothing measured, not a zero-argument function.
     * Accepting it would give a plausible stack_args of 0 to a function nobody read. */
    capture_clear();
    CHECK(!xdk_thunk_declare_callee_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 0u, 0u, 0u, true));
    CHECK(!xdk_thunk_abi_known(ADDR_D3D_HOT));
    CHECK(captured_contains("nothing was measured"));
}

static void test_the_callee_path_still_refuses_what_every_path_refuses(void)
{
    reset_all();
    /* The shared checks are not bypassed by the new entry point: an address outside the
     * adopted surface, and register arguments under a convention that passes none. */
    CHECK(!xdk_thunk_declare_callee_abi(0xDEADBEEFu, XDK_CC_STDCALL, 1u, 0u, 1u, true));
    CHECK(!xdk_thunk_declare_callee_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 1u, 2u, 1u, true));
    CHECK(!xdk_thunk_abi_known(ADDR_D3D_HOT));
}

static void test_section_names_map_to_modules_in_one_place(void)
{
    CHECK_EQ_U32(xdk_module_for_section("D3D"), XDK_MODULE_D3D8);
    CHECK_EQ_U32(xdk_module_for_section("DSOUND"), XDK_MODULE_DSOUND);
    CHECK_EQ_U32(xdk_module_for_section("XPP"), XDK_MODULE_XINPUT);
    CHECK_EQ_U32(xdk_module_for_section("XGRPH"), XDK_MODULE_XGRPH);
    CHECK_EQ_U32(xdk_module_for_section("XNET"), XDK_MODULE_XNET);
    CHECK_EQ_U32(xdk_module_for_section("XONLINE"), XDK_MODULE_XONLINE);
    CHECK_EQ_U32(xdk_module_for_section("XMV"), XDK_MODULE_NONE);
    CHECK_EQ_U32(xdk_module_for_section(NULL), XDK_MODULE_NONE);
    CHECK_EQ_U32(xdk_module_for_section(""), XDK_MODULE_NONE);
    /* Not a prefix match: "D3DX" is not "D3D". */
    CHECK_EQ_U32(xdk_module_for_section("D3DX"), XDK_MODULE_NONE);

    /* Every module has a name, including NONE, because a log line that printed
     * "(null)" through %s is a crash in the code that exists to explain crashes. */
    for (unsigned m = 0; m <= (unsigned)XDK_MODULE_COUNT; m++) {
        CHECK(xdk_module_name((xdk_module)m) != NULL);
    }
    for (unsigned c = 0; c <= (unsigned)XDK_CC_CDECL + 1u; c++) {
        CHECK(xdk_cc_name((xdk_cc)c) != NULL);
    }
}

/*
 * ADOPTION REFUSES A TABLE IT CANNOT DISPATCH FROM, AND A FAILED RE-INIT LOSES NOTHING.
 *
 * A duplicate address is refused because the lookup would otherwise be
 * order-dependent, and "which of the two rows won" is not a question a dispatcher
 * should have an answer to. The surviving-previous-table rule is the same one
 * d3d8_hle_init follows: losing a working surface is worse than failing to upgrade it.
 *
 * BREAKS THIS: tolerating duplicates; freeing the old table before the new one is known
 * good, which turns a refused upgrade into no surface at all.
 */
static void test_adoption_refuses_a_table_it_cannot_dispatch_from(void)
{
    static const xdk_dispatch_entry duplicated[] = {
        {ADDR_D3D_HOT, NULL, XDK_MODULE_D3D8},
        {ADDR_DSOUND_CREATE, NULL, XDK_MODULE_DSOUND},
        {ADDR_D3D_HOT, "the same address again", XDK_MODULE_DSOUND},
    };

    reset_all();
    CHECK(!xdk_thunk_init(NULL, 4u));
    CHECK(!xdk_thunk_init(SURFACE, 0u));
    CHECK(!xdk_thunk_init(duplicated, sizeof(duplicated) / sizeof(duplicated[0])));
    CHECK(captured_contains("appears twice"));

    /* The previous surface is untouched by every one of those failures. */
    CHECK_EQ_U32(xdk_thunk_count(), SURFACE_COUNT);
    CHECK(recomp_lookup_manual(ADDR_XINPUT_GETSTATE) != NULL);

    /* Adoption resets declared ABIs with the table: an ABI is an assertion about a
     * specific row, and carrying it across a swap would attach it to whatever now
     * lives at that address. */
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 2u, 0u));
    CHECK(xdk_thunk_abi_known(ADDR_D3D_HOT));
    CHECK(xdk_thunk_init(SURFACE, SURFACE_COUNT));
    CHECK(!xdk_thunk_abi_known(ADDR_D3D_HOT));
    CHECK_EQ_U64(xdk_thunk_abi_count(), 0u);

    /* With no surface at all, the lookup answers for nothing rather than faulting. */
    xdk_thunk_shutdown();
    CHECK_EQ_U32(xdk_thunk_count(), 0u);
    CHECK(recomp_lookup_manual(ADDR_D3D_HOT) == NULL);
    CHECK(!xdk_thunk_is_target(ADDR_D3D_HOT));
    CHECK(!xdk_thunk_abi_known(ADDR_D3D_HOT));
    /* Idempotent. */
    xdk_thunk_shutdown();
}

/*
 * THE TRACE IS ONE SEQUENCE COVERING BOTH BOUNDARIES.
 *
 * This is why the trace was extracted out of kernel_thunk.c rather than copied. Two
 * separate arrays, one per boundary, are each internally ordered and unrelated to one
 * another, so "DirectSoundCreate came after RtlEqualString and before NtReadFile" stops
 * being recoverable -- and that sentence is the entire deliverable of a bring-up run.
 *
 * The kernel-kind entries are appended directly here rather than through
 * kernel_thunk.c, which would drag in the guest register file and the whole kernel HLE.
 * What is being tested is the trace, and the trace does not care who appended.
 *
 * BREAKS THIS: a per-kind array; a per-kind counter that the total does not sum;
 * recording the address in the ordinal field or vice versa, which would make the two
 * kinds indistinguishable in a report.
 */
static void test_the_trace_interleaves_both_boundaries_in_one_order(void)
{
    const uint32_t args[1] = {1u};

    reset_all();
    implement_all_three();

    CHECK_EQ_U64(thunk_trace_append(THUNK_KIND_ORDINAL, 279u, 0u, 0x0037C958u, 0u, false),
                 0u);
    prepare_frame(args, 1u);
    CHECK(dispatch(ADDR_DSOUND_CREATE));
    CHECK_EQ_U64(thunk_trace_append(THUNK_KIND_ORDINAL, 219u, 0u, 0x003DF10Bu, 0u, true),
                 2u);

    size_t count = 0;
    const thunk_trace_entry *trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 3u);
    if (count != 3u) {
        return;
    }
    CHECK_EQ_U32(trace[0].kind, THUNK_KIND_ORDINAL);
    CHECK_EQ_U32(trace[0].ordinal, 279u);
    CHECK_EQ_U32(trace[0].address, 0u);

    CHECK_EQ_U32(trace[1].kind, THUNK_KIND_XDK);
    CHECK_EQ_U32(trace[1].address, ADDR_DSOUND_CREATE);
    CHECK_EQ_U32(trace[1].ordinal, 0u);
    CHECK(trace[1].implemented);
    CHECK_EQ_U32(trace[1].result, DSOUND_RESULT);
    CHECK(trace[0].result_known);
    CHECK(trace[1].result_known);
    CHECK(trace[2].result_known);

    CHECK_EQ_U32(trace[2].kind, THUNK_KIND_ORDINAL);
    CHECK_EQ_U32(trace[2].ordinal, 219u);

    CHECK_EQ_U64(thunk_trace_total(), 3u);
    CHECK_EQ_U64(thunk_trace_total_of_kind(THUNK_KIND_ORDINAL), 2u);
    CHECK_EQ_U64(thunk_trace_total_of_kind(THUNK_KIND_XDK), 1u);

    /* Every entry carries a thread id, and they are small and stable rather than a
     * pthread_t. Without them two interleaved call orders are one uninterpretable
     * sequence. */
    CHECK(trace[0].thread != 0u);
    CHECK_EQ_U32(trace[1].thread, trace[0].thread);

    /* A patch addresses OUR slot by index, and an out-of-range slot is ignored rather
     * than writing past the array -- the stop path passes THUNK_TRACE_NO_SLOT straight
     * through. */
    thunk_trace_patch_result(0u, 0xC0000034u);
    thunk_trace_patch_result(THUNK_TRACE_NO_SLOT, 0xBADBAD00u);
    thunk_trace_patch_result(THUNK_TRACE_MAX + 100u, 0xBADBAD00u);
    trace = thunk_trace_entries(&count);
    CHECK_EQ_U32(trace[0].result, 0xC0000034u);
    CHECK_EQ_U32(trace[2].result, 0u);
}

static uint32_t stopping_handler(void *context)
{
    record(&d3d_seen, context);
    host_run_stop(HOST_STOP_UNIMPLEMENTED, ADDR_D3D_HOT, 0u, "handler stopped");
    return EAX_POISON; /* host_run_stop exits through the armed harness. */
}

/* A real registered handler stops before returning through the dispatcher. */
static uint32_t utility_handler(void *context)
{
    uint32_t last = 0u;
    (void)kernel_frame_arg(context, 7u, &last);
    return last ^ 0xAA550000u;
}

static uint32_t stopping_utility_handler(void *context)
{
    (void)context;
    host_run_stop(HOST_STOP_UNIMPLEMENTED, ADDR_XGRPH_CUBE, 0u, "utility stopped");
    return EAX_POISON;
}

static void test_xgrph_routes_and_refuses_without_borrowing_d3d(void)
{
    const uint32_t args[] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    reset_all();
    CHECK(xdk_thunk_declare_abi(ADDR_XGRPH_CUBE, XDK_CC_STDCALL, 8u, 0u));
    prepare_frame(args, 8u);
    CHECK(dispatch(ADDR_XGRPH_CUBE));
    CHECK(stopped);
    CHECK_EQ_U32(stop_record.reason, HOST_STOP_XDK_UNIMPLEMENTED);
    size_t count;
    const thunk_trace_entry *trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 1u);
    CHECK(!trace[0].implemented && !trace[0].result_known);
    CHECK_EQ_U32(g_eax, EAX_POISON);
    CHECK_EQ_U32(g_esp, esp_at_entry);
    CHECK(xgrph_hle_register(ADDR_XGRPH_CUBE, utility_handler));
    thunk_trace_reset();
    prepare_frame(args, 8u);
    CHECK(dispatch(ADDR_XGRPH_CUBE));
    CHECK(!stopped);
    CHECK_EQ_U32(g_eax, 0xAA550008u);
    CHECK_EQ_U32(g_esp, esp_at_entry + 36u);
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_XGRPH), 1u);
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_D3D8), 0u);
    CHECK_EQ_U32(d3d_seen.calls, 0u);
    trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 1u);
    CHECK(trace[0].implemented && trace[0].result_known);
    CHECK_EQ_U32(trace[0].result, 0xAA550008u);
    CHECK_EQ_U32(trace[0].address, ADDR_XGRPH_CUBE);
    CHECK_EQ_U32(trace[0].return_address, RETURN_ADDRESS);
    CHECK(xgrph_hle_register(ADDR_XGRPH, stopping_handler));
    prepare_frame(args, 8u);
    CHECK(dispatch(ADDR_XGRPH));
    CHECK(stopped);
    CHECK_EQ_U32(stop_record.reason, HOST_STOP_XDK_ABI_UNKNOWN);
    trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 2u);
    CHECK(trace[1].implemented && !trace[1].result_known);
    CHECK_EQ_U32(d3d_seen.calls, 0u);
    CHECK(xgrph_hle_register(ADDR_XGRPH_CUBE, stopping_utility_handler));
    prepare_frame(args, 8u);
    CHECK(dispatch(ADDR_XGRPH_CUBE));
    CHECK(stopped);
    CHECK_EQ_U32(stop_record.reason, HOST_STOP_UNIMPLEMENTED);
    CHECK_EQ_U32(stop_record.guest_address, ADDR_XGRPH_CUBE);
    CHECK_EQ_U32(g_eax, EAX_POISON);
    CHECK_EQ_U32(g_esp, esp_at_entry);
    trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 3u);
    CHECK(trace[2].implemented && !trace[2].result_known);
    CHECK_EQ_U32(trace[2].address, ADDR_XGRPH_CUBE);
    CHECK_EQ_U32(trace[2].return_address, RETURN_ADDRESS);
    reset_all();
    CHECK(xdk_thunk_declare_abi(ADDR_XGRPH_CUBE, XDK_CC_STDCALL, 8u, 0u));
    CHECK(xgrph_hle_set_default_return(ADDR_XGRPH_CUBE, 0x12345678u));
    xdk_thunk_set_stop_on_missing(false);
    prepare_frame(args, 8u);
    CHECK(dispatch(ADDR_XGRPH_CUBE));
    CHECK(!stopped);
    CHECK_EQ_U32(g_eax, 0x12345678u);
    CHECK_EQ_U32(g_esp, esp_at_entry + 36u);
    trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 1u);
    CHECK(!trace[0].implemented && trace[0].result_known);
    CHECK_EQ_U32(trace[0].result, 0x12345678u);
}

static void test_registered_handler_stop_is_traced(void)
{
    const uint32_t args[] = {123u};
    reset_all();
    CHECK(d3d8_hle_register(ADDR_D3D_HOT, stopping_handler));
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 1u, 0u));
    prepare_frame(args, 1u);
    g_ecx = 0x11223344u;
    g_edx = 0x55667788u;
    CHECK(dispatch(ADDR_D3D_HOT));
    CHECK(stopped);
    CHECK_EQ_U32(stop_record.reason, HOST_STOP_UNIMPLEMENTED);
    CHECK_EQ_U32(stop_record.guest_address, ADDR_D3D_HOT);
    CHECK_EQ_U32(d3d_seen.calls, 1u);
    CHECK_EQ_U32(g_eax, EAX_POISON);
    CHECK_EQ_U32(g_esp, esp_at_entry);
    CHECK_EQ_U32(g_ecx, 0x11223344u);
    CHECK_EQ_U32(g_edx, 0x55667788u);
    CHECK_EQ_U64(thunk_trace_total_of_kind(THUNK_KIND_XDK), 1u);
    size_t count;
    const thunk_trace_entry *trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 1u);
    if (count == 1u) {
        CHECK_EQ_U32(trace[0].kind, THUNK_KIND_XDK);
        CHECK_EQ_U32(trace[0].address, ADDR_D3D_HOT);
        CHECK_EQ_U32(trace[0].return_address, RETURN_ADDRESS);
        CHECK(trace[0].implemented);
        CHECK(!trace[0].result_known);
    }
}

static pthread_mutex_t return_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t return_cond = PTHREAD_COND_INITIALIZER;
static bool first_entered, release_first;

static uint32_t ordered_return_handler(void *context)
{
    uint32_t token = 0u;
    (void)kernel_frame_arg(context, 0u, &token);
    if (token == 1u) {
        pthread_mutex_lock(&return_mutex);
        first_entered = true;
        pthread_cond_broadcast(&return_cond);
        while (!release_first) pthread_cond_wait(&return_cond, &return_mutex);
        pthread_mutex_unlock(&return_mutex);
    }
    return 0x70000000u + token;
}

typedef struct trace_worker {
    uint32_t stack, token;
    bool okay;
} trace_worker;

static void *dispatch_worker(void *opaque)
{
    trace_worker *worker = opaque;
    g_esp = worker->stack;
    g_eax = EAX_POISON;
    recomp_func_t fn = recomp_lookup_manual(ADDR_D3D_HOT);
    if (fn) fn();
    worker->okay = fn != NULL && g_eax == 0x70000000u + worker->token
                   && g_esp == worker->stack + 8u;
    if (worker->token == 2u) {
        pthread_mutex_lock(&return_mutex);
        release_first = true;
        pthread_cond_broadcast(&return_cond);
        pthread_mutex_unlock(&return_mutex);
    }
    return NULL;
}

/* The first call returns last, so patching the newest slot fails this test. */
static void test_concurrent_returns_patch_their_own_slots(void)
{
    reset_all();
    CHECK(d3d8_hle_register(ADDR_D3D_HOT, ordered_return_handler));
    CHECK(xdk_thunk_declare_abi(ADDR_D3D_HOT, XDK_CC_STDCALL, 1u, 0u));
    trace_worker workers[2] = {{scratch + OFF_STACK, 1u, false},
                               {scratch + OFF_STACK + 64u, 2u, false}};
    for (unsigned i = 0; i < 2u; i++) {
        CHECK(kernel_guest_write_u32(workers[i].stack, RETURN_ADDRESS + i));
        CHECK(kernel_guest_write_u32(workers[i].stack + 4u, workers[i].token));
    }
    first_entered = release_first = false;
    pthread_t threads[2];
    int status = pthread_create(&threads[0], NULL, dispatch_worker, &workers[0]);
    CHECK(status == 0);
    if (status != 0) return;
    pthread_mutex_lock(&return_mutex);
    while (!first_entered) pthread_cond_wait(&return_cond, &return_mutex);
    pthread_mutex_unlock(&return_mutex);
    status = pthread_create(&threads[1], NULL, dispatch_worker, &workers[1]);
    CHECK(status == 0);
    if (status == 0) {
        CHECK(pthread_join(threads[1], NULL) == 0);
    } else {
        pthread_mutex_lock(&return_mutex);
        release_first = true;
        pthread_cond_broadcast(&return_cond);
        pthread_mutex_unlock(&return_mutex);
    }
    CHECK(pthread_join(threads[0], NULL) == 0);
    if (status != 0) return;
    CHECK(workers[0].okay);
    CHECK(workers[1].okay);
    size_t count;
    const thunk_trace_entry *trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 2u);
    CHECK_EQ_U64(thunk_trace_total(), 2u);
    if (count == 2u) {
        for (unsigned i = 0; i < 2u; i++) {
            CHECK(trace[i].result_known);
            CHECK(trace[i].implemented);
            CHECK_EQ_U32(trace[i].address, ADDR_D3D_HOT);
            CHECK_EQ_U32(trace[i].return_address, RETURN_ADDRESS + i);
            CHECK_EQ_U32(trace[i].result, 0x70000001u + i);
        }
        CHECK(trace[0].thread != 0u);
        CHECK(trace[1].thread != 0u);
        CHECK(trace[0].thread != trace[1].thread);
    }
}

/*
 * THE TRACE SURVIVES BEING FULL, AND THE TOTAL STILL COUNTS WHAT IT DROPPED.
 *
 * A bring-up run that reaches gameplay can exceed the fixed trace capacity. The array has to
 * stop appending without writing past itself, and the TOTAL has to keep counting --
 * otherwise a long run silently reports only the stored count and a reader concludes the guest
 * stopped there.
 *
 * BREAKS THIS: an off-by-one in the capacity test; incrementing the total only when
 * there is room.
 */
static void test_a_full_trace_drops_entries_but_not_the_count(void)
{
    thunk_trace_reset();
    for (unsigned i = 0; i < THUNK_TRACE_MAX; i++) {
        CHECK_EQ_U64(thunk_trace_append(THUNK_KIND_XDK, 0u, ADDR_D3D_HOT, 0u, 0u, true),
                     i);
    }
    CHECK_EQ_U64(thunk_trace_append(THUNK_KIND_XDK, 0u, ADDR_D3D_HOT, 0u, 0u, true),
                 THUNK_TRACE_NO_SLOT);

    size_t count = 0;
    (void)thunk_trace_entries(&count);
    CHECK_EQ_U64(count, (uint64_t)THUNK_TRACE_MAX);
    CHECK_EQ_U64(thunk_trace_total(), (uint64_t)THUNK_TRACE_MAX + 1u);

    thunk_trace_reset();
    (void)thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 0u);
    CHECK_EQ_U64(thunk_trace_total(), 0u);
}

/* Overflow must count each production attempt once, including returned calls. */
static void test_full_trace_dispatch_counts_once(void)
{
    const uint32_t args[] = {1u, 2u};
    reset_all();
    implement_all_three();
    for (unsigned i = 0; i < THUNK_TRACE_MAX; i++) {
        (void)thunk_trace_append(THUNK_KIND_ORDINAL, 1u, 0u, 0u, 0u, true);
    }
    prepare_frame(args, 2u);
    CHECK(dispatch(ADDR_D3D_HOT));
    CHECK(!stopped);
    CHECK_EQ_U32(g_eax, D3D_RESULT);
    CHECK_EQ_U64(thunk_trace_total(), THUNK_TRACE_MAX + 1u);
    CHECK_EQ_U64(thunk_trace_total_of_kind(THUNK_KIND_XDK), 1u);
    size_t count;
    const thunk_trace_entry *trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, THUNK_TRACE_MAX);
    CHECK_EQ_U32(trace[count - 1u].kind, THUNK_KIND_ORDINAL);
}

/*
 * THE REPORT SAYS WHAT THIS BOUNDARY CANNOT SEE, UNCONDITIONALLY.
 *
 * The number above it -- "N of 236 addresses routed" -- is the single most misreadable
 * figure this subsystem produces, because `docs/d3d8-usage.md` §9 MEASURED that the
 * hottest GPU operation in the title does not cross a call boundary at all:
 * `D3DDevice_SetRenderState` is INLINED into game `.text` at 0x000211C0 and writes
 * D3D8's dirty mask at 0x3E3AB8 and its deferred shadow array at 0x3E3CC0 directly.
 * A report that printed the count without the caveat would be a plausible
 * overstatement, which is the class of output this project exists not to produce.
 *
 * BREAKS THIS: deleting the caveat; making it conditional on something.
 */
static void test_the_report_states_what_the_boundary_cannot_intercept(void)
{
    reset_all();
    xdk_thunk_report();
    CHECK(captured_contains("INLINED"));
    CHECK(captured_contains("0x000211C0"));
    CHECK(captured_contains("0x3E3AB8"));
    CHECK(captured_contains("0x3E3CC0"));
    CHECK(captured_contains("does NOT mean"));
    /* And it names every module, so a reader can see which sections have nothing. */
    CHECK(captured_contains("no module"));

    /* With no surface adopted it says so, rather than printing an empty table that
     * reads as a boundary with no traffic. */
    xdk_thunk_shutdown();
    capture_clear();
    xdk_thunk_report();
    CHECK(captured_contains("no surface adopted"));
}

static void test_required_missing_policy(const uint32_t *addresses, size_t address_count, unsigned argc)
{
    reset_all();
    xdk_dispatch_entry table[14];
    for (size_t i=0u;i<address_count;i++) {
        table[i]=(xdk_dispatch_entry){addresses[i],"measured required audio boundary",XDK_MODULE_DSOUND};
        CHECK(dsound_hle_requires_implementation(addresses[i]));
    }
    CHECK(!dsound_hle_requires_implementation(ADDR_DSOUND_CREATE));
    CHECK(!dsound_hle_requires_implementation(0u));
    CHECK(!dsound_hle_requires_implementation(0x407B15u));
    CHECK(xdk_thunk_init(table,address_count));
    const uint32_t args[]={0x11223344u,0x55667788u,0x99AABBCCu,0xDDEEFF00u,0x12345678u,0x87654321u,0xABCDEF01u,0x10FEDCBAu};
    /* Only the return word is readable: argument access would hit a real guard. */
    void *guard=kernel_guest_at(scratch+0x3000u,0x1000u);
    CHECK(guard!=NULL);
    CHECK(mprotect(guard,0x1000u,PROT_NONE)==0);
    kernel_guest_probe_cache_flush(); /* T819: a protection change made behind the host's back drops the probe cache */
    uint32_t inaccessible=0xAABBCCDDu;
    CHECK(!kernel_guest_read_u32(scratch+0x3000u,&inaccessible));
    CHECK_EQ_U32(inaccessible,0xAABBCCDDu);
    for (unsigned policy=0u;policy<2u;policy++) {
        xdk_thunk_set_stop_on_missing(policy==0u);
        for (size_t i=0u;i<address_count;i++) {
            g_esp=scratch+0x2FFCu; esp_at_entry=g_esp; g_eax=EAX_POISON;
            CHECK(kernel_guest_write_u32(g_esp,RETURN_ADDRESS));
            g_ecx=0xAABBCCDDu; g_edx=0x01020304u;
            uint8_t before[4],after[4];
            CHECK(kernel_guest_read_bytes(g_esp,before,sizeof(before)));
            thunk_trace_reset();
            CHECK(dispatch(addresses[i])); CHECK(stopped);
            CHECK_EQ_U32(stop_record.reason,HOST_STOP_XDK_UNIMPLEMENTED);
            CHECK_EQ_U32(stop_record.guest_address,addresses[i]);
            CHECK_EQ_U32(stop_record.ordinal,0u);
            CHECK(strcmp(stop_record.detail,"measured required audio boundary")==0);
            CHECK_EQ_U32(g_eax,EAX_POISON); CHECK_EQ_U32(g_esp,esp_at_entry);
            CHECK_EQ_U32(g_ecx,0xAABBCCDDu); CHECK_EQ_U32(g_edx,0x01020304u);
            CHECK(kernel_guest_read_bytes(g_esp,after,sizeof(after)));
            CHECK(memcmp(before,after,sizeof(before))==0);
            CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_DSOUND),0u);
            size_t count; const thunk_trace_entry *trace=thunk_trace_entries(&count);
            CHECK_EQ_U64(count,1u);
            if (count==1u) {
                CHECK(trace[0].address==addresses[i] && trace[0].return_address==RETURN_ADDRESS);
                CHECK(!trace[0].implemented && !trace[0].result_known);
            }
        }
    }
    CHECK(mprotect(guard,0x1000u,PROT_READ|PROT_WRITE)==0);
    kernel_guest_probe_cache_flush();
    /* A real registered implementation is eligible; policy itself registers nothing. */
    CHECK(dsound_hle_register(addresses[address_count-1u],dsound_handler));
    CHECK(xdk_thunk_declare_callee_abi(addresses[address_count-1u],XDK_CC_STDCALL,argc,0u,1u,true));
    for (unsigned policy=0u;policy<2u;policy++) {
        xdk_thunk_set_stop_on_missing(policy==0u);
        thunk_trace_reset();
        prepare_frame(args,argc); CHECK(dispatch(addresses[address_count-1u])); CHECK(!stopped);
        CHECK_EQ_U32(g_eax,DSOUND_RESULT); CHECK_EQ_U32(g_esp,esp_at_entry+4u+argc*4u);
        size_t count; const thunk_trace_entry *trace=thunk_trace_entries(&count);
        CHECK_EQ_U64(count,1u);
        if (count==1u) CHECK(trace[0].implemented && trace[0].result_known);
    }
    CHECK_EQ_U32(dsound_seen.calls,2u);
    CHECK_EQ_U64(xdk_thunk_module_call_count(XDK_MODULE_DSOUND),2u);
    dsound_hle_init();
}

static void test_audio_missing_policy(void)
{
    const uint32_t stream[]={0x407B14u,0x407B19u,0x407B1Eu,0x407B23u,0x407B28u,
        0x4085CFu,0x4085D4u,0x4085D9u,0x4085F1u,0x408609u,0x408632u,0x408637u,
        0x408C2Du,0x40967Cu};
    const uint32_t buffer[]={0x407ABCu,0x407AA4u,0x407AF8u,0x408556u,0x407A64u,
        0x407AD8u,0x408532u,0x40858Bu,0x407A80u,0x4084F2u,0x40850Eu,0x4085AFu,
        0x408C0Du,0x4093C8u};
    test_required_missing_policy(stream,14u,2u);
    test_required_missing_policy(buffer,14u,4u);
    const uint32_t listener[]={0x4093ECu,0x40945Au,0x409410u};
    test_required_missing_policy(listener,3u,8u);
}

static void test_direct_stop_entry(void)
{
    reset_all();
    xdk_thunk_shutdown();
    CHECK_EQ_U64(xdk_thunk_stop_override_count(),0u);
    CHECK_EQ_U64(xdk_thunk_count(),0u);
    xdk_thunk_set_stop_on_missing(false);
    const uint32_t args[]={0xDEADBEEFu,0xFEED1234u};
    prepare_frame(args,2u);
    g_ecx=0x11223344u; g_edx=0x55667788u;
    uint8_t before[32],after[32];
    CHECK(kernel_guest_read_bytes(g_esp,before,sizeof(before)));
    if (sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm();
        xdk_thunk_stop_at(0x00407883u,"stream AddRef adjustor","direct nested hardware stop");
    } else stop_record=*host_run_result();
    host_run_disarm();
    CHECK_EQ_U32(stop_record.reason,HOST_STOP_XDK_UNIMPLEMENTED);
    CHECK_EQ_U32(stop_record.guest_address,0x00407883u);
    CHECK_EQ_U32(stop_record.ordinal,0u);
    CHECK(strcmp(stop_record.detail,"direct nested hardware stop")==0);
    CHECK(captured_contains("stream AddRef adjustor"));
    CHECK_EQ_U32(g_eax,EAX_POISON);
    CHECK_EQ_U32(g_ecx,0x11223344u); CHECK_EQ_U32(g_edx,0x55667788u);
    CHECK_EQ_U32(g_esp,esp_at_entry);
    CHECK(kernel_guest_read_bytes(g_esp,after,sizeof(after)));
    CHECK(memcmp(before,after,sizeof(before))==0);
    CHECK_EQ_U64(xdk_thunk_stop_override_call_count(),1u);
    CHECK_EQ_U64(xdk_thunk_call_count(),0u);
    CHECK_EQ_U32(dsound_seen.calls,0u); CHECK_EQ_U32(d3d_seen.calls,0u);
    size_t count; const thunk_trace_entry *trace=thunk_trace_entries(&count);
    CHECK_EQ_U64(count,1u);
    if (count==1u) {
        CHECK_EQ_U32(trace[0].address,0x00407883u);
        CHECK_EQ_U32(trace[0].return_address,RETURN_ADDRESS);
        CHECK(!trace[0].implemented && !trace[0].result_known);
    }
    /* An unreadable return slot affects only the caller diagnostic. */
    g_esp=0u;
    if (sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm(); xdk_thunk_stop_at(0x0040788Du,NULL,NULL);
    } else stop_record=*host_run_result();
    host_run_disarm();
    CHECK_EQ_U32(g_esp,0u); CHECK_EQ_U32(g_eax,EAX_POISON);
    CHECK_EQ_U32(stop_record.guest_address,0x0040788Du);
    CHECK_EQ_U32(stop_record.ordinal,0u);
    CHECK(strcmp(stop_record.detail,"explicit stop-only policy")==0);
    trace=thunk_trace_entries(&count);
    CHECK_EQ_U64(count,2u);
    if (count==2u) CHECK_EQ_U32(trace[1].return_address,0u);
}

static int discard_stop_log(const char *format, ...)
{ (void)format; return 0; }
typedef struct stop_worker {
    uint32_t address, stack;
    bool okay;
} stop_worker;
static pthread_mutex_t stop_order_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stop_order_cond = PTHREAD_COND_INITIALIZER;
static bool stop_first_lookup, stop_second_finished;
static void *stop_worker_run(void *opaque)
{
    stop_worker *worker = opaque;
    g_eax=EAX_POISON; g_ecx=0x11223344u; g_edx=0x55667788u; g_esp=worker->stack;
    recomp_func_t fn = recomp_lookup_manual(worker->address);
    pthread_mutex_lock(&stop_order_lock);
    if (worker->address == ADDR_NEAR_MISS) {
        stop_first_lookup=true; pthread_cond_broadcast(&stop_order_cond);
        while (!stop_second_finished) pthread_cond_wait(&stop_order_cond,&stop_order_lock);
    }
    pthread_mutex_unlock(&stop_order_lock);
    bool did_stop=false;
    if (fn && sigsetjmp(*host_run_jmp(),1)==0) { host_run_arm(); fn(); }
    else if (fn) {
        const host_stop *result=host_run_result();
        did_stop=result->reason==HOST_STOP_XDK_UNIMPLEMENTED &&
                 result->guest_address==worker->address && strcmp(result->detail,"thread gate")==0;
    }
    host_run_disarm();
    worker->okay=did_stop && g_eax==EAX_POISON && g_ecx==0x11223344u &&
                 g_edx==0x55667788u && g_esp==worker->stack;
    if (worker->address != ADDR_NEAR_MISS) {
        pthread_mutex_lock(&stop_order_lock);
        stop_second_finished=true; pthread_cond_broadcast(&stop_order_cond);
        pthread_mutex_unlock(&stop_order_lock);
    }
    return NULL;
}
static void test_stop_override_tls(void)
{
    reset_all();
    const xdk_stop_override entries[]={{ADDR_NEAR_MISS,"same label","thread gate"},
                                      {ADDR_NEAR_MISS+4u,"same label","thread gate"}};
    CHECK(xdk_thunk_stop_override_adopt(entries,2u));
    xdk_thunk_set_log(discard_stop_log);
    stop_first_lookup=false; stop_second_finished=false;
    stop_worker workers[]={{ADDR_NEAR_MISS,scratch+0x100u,false},
                          {ADDR_NEAR_MISS+4u,scratch+0x200u,false}};
    CHECK(kernel_guest_write_u32(workers[0].stack,RETURN_ADDRESS));
    CHECK(kernel_guest_write_u32(workers[1].stack,RETURN_ADDRESS+4u));
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0],NULL,stop_worker_run,&workers[0])==0);
    pthread_mutex_lock(&stop_order_lock);
    while (!stop_first_lookup) pthread_cond_wait(&stop_order_cond,&stop_order_lock);
    pthread_mutex_unlock(&stop_order_lock);
    CHECK(pthread_create(&threads[1],NULL,stop_worker_run,&workers[1])==0);
    CHECK(pthread_join(threads[1],NULL)==0); CHECK(pthread_join(threads[0],NULL)==0);
    CHECK(workers[0].okay && workers[1].okay);
    CHECK_EQ_U64(xdk_thunk_stop_override_call_count(),2u);
    size_t count; const thunk_trace_entry *trace=thunk_trace_entries(&count);
    CHECK_EQ_U64(count,2u);
    if (count==2u) {
        CHECK_EQ_U32(trace[0].address,workers[1].address);
        CHECK_EQ_U32(trace[1].address,workers[0].address);
        CHECK_EQ_U32(trace[0].return_address,RETURN_ADDRESS+4u);
        CHECK_EQ_U32(trace[1].return_address,RETURN_ADDRESS);
        CHECK(!trace[0].result_known && !trace[1].result_known);
        CHECK(!trace[0].implemented && !trace[1].implemented);
        CHECK(trace[0].thread!=trace[1].thread);
    }
    xdk_thunk_stop_override_reset();
    xdk_thunk_set_log(capture_printer);
}

/* Stop-only overrides are policy gates, not surface/ABI/implementation rows. */
static void test_stop_overrides(void)
{
    reset_all();
    char label[] = "blocked interface method", reason[] = "omitted nested hardware";
    xdk_stop_override entry = {ADDR_NEAR_MISS, label, reason};
    CHECK(xdk_thunk_stop_override_adopt(&entry, 1u));
    label[0] = 'X'; reason[0] = 'X'; /* Registry must own its text. */
    CHECK_EQ_U64(xdk_thunk_stop_override_count(), 1u);
    CHECK(!xdk_thunk_init(NULL, 0u));
    CHECK_EQ_U64(xdk_thunk_stop_override_count(), 1u);
    CHECK(recomp_lookup_manual(ADDR_NEAR_MISS) != NULL);
    xdk_thunk_set_stop_on_missing(false);
    const uint32_t args[] = {0xDEADBEEFu, 0xFEED1234u};
    prepare_frame(args, 2u);
    uint8_t before[32], after[32];
    CHECK(kernel_guest_read_bytes(g_esp, before, sizeof(before)));
    g_ecx = 0x11223344u; g_edx = 0x55667788u;
    CHECK(dispatch(ADDR_NEAR_MISS));
    CHECK(stopped);
    CHECK_EQ_U32(stop_record.reason, HOST_STOP_XDK_UNIMPLEMENTED);
    CHECK_EQ_U32(stop_record.guest_address, ADDR_NEAR_MISS);
    CHECK_EQ_U32(stop_record.ordinal, 0u);
    CHECK(strcmp(stop_record.detail, "omitted nested hardware") == 0);
    CHECK(captured_contains("blocked interface method"));
    CHECK_EQ_U32(g_eax, EAX_POISON);
    CHECK_EQ_U32(g_ecx, 0x11223344u); CHECK_EQ_U32(g_edx, 0x55667788u);
    CHECK_EQ_U32(g_esp, esp_at_entry);
    CHECK(kernel_guest_read_bytes(g_esp, after, sizeof(after)));
    CHECK(memcmp(before, after, sizeof(before)) == 0);
    CHECK_EQ_U64(xdk_thunk_stop_override_call_count(), 1u);
    CHECK_EQ_U64(xdk_thunk_call_count(), 0u);
    CHECK_EQ_U32(d3d_seen.calls, 0u); CHECK_EQ_U32(dsound_seen.calls, 0u);
    size_t count; const thunk_trace_entry *trace = thunk_trace_entries(&count);
    CHECK_EQ_U64(count, 1u);
    if (count == 1u) {
        CHECK_EQ_U32(trace[0].address, ADDR_NEAR_MISS);
        CHECK_EQ_U32(trace[0].return_address, RETURN_ADDRESS);
        CHECK(!trace[0].implemented && !trace[0].result_known);
    }
    const uint32_t bad_addresses[] = {0u, KERNEL_THUNK_VA_BASE,
        KERNEL_THUNK_VA_BASE + KERNEL_THUNK_WINDOW_BYTES - 1u,
        MONITOR_THUNK_NOTIFY_VA, ADDR_D3D_HOT};
    for (size_t i = 0u; i < sizeof(bad_addresses)/sizeof(bad_addresses[0]); i++) {
        xdk_stop_override bad = {bad_addresses[i], "bad", "bad"};
        CHECK(!xdk_thunk_stop_override_adopt(&bad, 1u));
        CHECK_EQ_U64(xdk_thunk_stop_override_count(), 1u);
        CHECK_EQ_U64(xdk_thunk_stop_override_call_count(), 1u);
        CHECK(recomp_lookup_manual(ADDR_NEAR_MISS) != NULL);
    }
    xdk_stop_override duplicate[] = {{ADDR_NEAR_MISS,"one","one"},
                                     {ADDR_NEAR_MISS,"two","two"}};
    CHECK(!xdk_thunk_stop_override_adopt(duplicate, 2u));
    CHECK(!xdk_thunk_stop_override_adopt(NULL, 1u));
    CHECK(!xdk_thunk_stop_override_adopt(&entry, 0u));
    CHECK(!xdk_thunk_stop_override_adopt(&entry, XDK_STOP_MAX_ENTRIES+1u));
    char long_label[XDK_STOP_LABEL_BYTES+1u];
    memset(long_label,'A',sizeof(long_label)); long_label[sizeof(long_label)-1u]=0;
    xdk_stop_override bad_text = {ADDR_NEAR_MISS,long_label,"reason"};
    CHECK(!xdk_thunk_stop_override_adopt(&bad_text,1u));
    bad_text.label=""; CHECK(!xdk_thunk_stop_override_adopt(&bad_text,1u));
    bad_text.label=NULL; CHECK(!xdk_thunk_stop_override_adopt(&bad_text,1u));
    bad_text.label="name"; bad_text.reason="";
    CHECK(!xdk_thunk_stop_override_adopt(&bad_text,1u));
    CHECK_EQ_U64(xdk_thunk_stop_override_call_count(),1u);
    /* A stop registry remains useful with no normal surface adopted. */
    xdk_thunk_shutdown();
    CHECK(xdk_thunk_stop_override_adopt(&entry,1u));
    CHECK_EQ_U64(xdk_thunk_count(),0u);
    prepare_frame(args,2u);
    CHECK(dispatch(ADDR_NEAR_MISS));
    CHECK(stopped && stop_record.reason==HOST_STOP_XDK_UNIMPLEMENTED);
    CHECK_EQ_U32(stop_record.ordinal,0u);
    CHECK_EQ_U32(g_esp,esp_at_entry);
    CHECK(!xdk_thunk_init(NULL,0u));
    CHECK(recomp_lookup_manual(ADDR_NEAR_MISS)!=NULL);
    CHECK(xdk_thunk_init(SURFACE,SURFACE_COUNT));
    CHECK_EQ_U64(xdk_thunk_stop_override_count(),0u);
    xdk_thunk_stop_override_reset();
    CHECK(recomp_lookup_manual(ADDR_NEAR_MISS)==NULL);
    CHECK(recomp_lookup_manual(ADDR_D3D_HOT)!=NULL);
    CHECK(recomp_lookup_manual(MONITOR_THUNK_NOTIFY_VA)!=NULL);
    CHECK_EQ_U64(xdk_thunk_stop_override_call_count(),0u);
    CHECK(xdk_thunk_stop_override_adopt(&entry,1u));
    CHECK(xdk_thunk_init(SURFACE,sizeof(SURFACE)/sizeof(SURFACE[0])));
    CHECK_EQ_U64(xdk_thunk_stop_override_count(),0u);
    const xdk_stop_override verified_methods[] = {
        {0x0040723Fu,"stream AddRef","nested hardware omitted"},
        {0x00407286u,"stream Release","nested hardware omitted"},
        {0x00407424u,"stream Process","nested hardware omitted"},
    };
    CHECK(xdk_thunk_stop_override_adopt(verified_methods,3u));
    CHECK(recomp_lookup_manual(0x00407424u)!=NULL);
    xdk_thunk_stop_override_reset();
}

int main(void)
{
    if (!scratch_init()) {
        printf("FAIL could not allocate scratch guest memory\n");
        return 1;
    }

    CHECK(!xdk_thunk_stream_stops_ready());
    test_audio_missing_policy();
    test_direct_stop_entry();
    test_stop_overrides();
    test_stop_override_tls();
    test_the_lookup_answers_for_measured_addresses_only();
    test_a_call_reaches_the_right_module_and_not_the_others();
    test_all_three_modules_have_a_production_caller();
    test_the_handler_sees_the_frame_the_dispatcher_built();
    test_an_unimplemented_address_stops_rather_than_returning_zero();
    test_continue_on_missing_relaxes_the_stop_but_not_the_cleanup();
    test_an_address_with_no_abi_is_refused_and_the_handler_is_not_reached();
    test_the_measured_arity_quorum_refuses_what_it_should();
    test_an_incoherent_convention_is_refused();
    test_the_cleanup_matches_the_convention();
    test_a_section_with_no_module_stops_whatever_the_policy();
    test_the_callee_path_has_no_quorum_because_sites_are_not_evidence();
    test_the_callee_path_refuses_a_conflict_rather_than_reducing_it();
    test_the_callee_path_still_refuses_what_every_path_refuses();
    test_section_names_map_to_modules_in_one_place();
    test_adoption_refuses_a_table_it_cannot_dispatch_from();
    test_the_trace_interleaves_both_boundaries_in_one_order();
    test_xgrph_routes_and_refuses_without_borrowing_d3d();
    test_registered_handler_stop_is_traced();
    test_concurrent_returns_patch_their_own_slots();
    test_a_full_trace_drops_entries_but_not_the_count();
    test_full_trace_dispatch_counts_once();
    test_the_report_states_what_the_boundary_cannot_intercept();

    printf("%s: %d checks, %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", checks,
           failures);
    return failures == 0 ? 0 : 1;
}
