/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The thread model: stacks, guard bands, the entry frame, the per-thread KPCR, and
 * the watchdog.
 *
 * WHY THIS SUITE EXISTS SEPARATELY, AND WHY IT HAS NO LIFTED CODE IN IT. The
 * interesting part of starting a guest thread is arithmetic over guest addresses
 * plus a few guest-memory writes, and none of that needs the 2.56 M lines of lifted
 * C -- which are derived from the user's own executable and are not committed. A
 * fresh clone has none of them and must still be able to prove the stack is laid
 * out correctly, so `kernel_thread.c` takes the two host-specific operations
 * (resolve a VA, enter the guest) as injected hooks and this suite supplies fakes.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED. The project has been bitten repeatedly by
 * tests that pass against broken code, so each invariant below was confirmed to
 * FAIL when the implementation was deliberately broken in the corresponding way:
 * guard bands removed, esp placed at the wrong end of the stack, esp misaligned,
 * the entry frame's arguments swapped, and the started flag not set. A test that
 * has not been shown to fail has not been shown to test anything.
 */

#include "kernel_thread.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "nt_status.h"

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        uint32_t a_ = (uint32_t)(actual);                                                \
        uint32_t e_ = (uint32_t)(expected);                                              \
        if (a_ != e_) {                                                                   \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__,          \
                   #actual, (unsigned)a_, (unsigned)e_);                                 \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

static int quiet_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    (void)format;
    va_end(args);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* A guest region to lay a stack out in, allocated exactly as the real code does. */
/* ------------------------------------------------------------------------- */

static uint32_t alloc_region(uint32_t bytes, uint32_t alignment)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = bytes;
    request.alignment = alignment;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    kernel_guest_ptr base = guest_region_alloc(&request, &status);
    if (base == 0u) {
        printf("FAIL could not allocate %u guest bytes (status %#x)\n", bytes,
               (unsigned)status);
        failures++;
    }
    return (uint32_t)base;
}

/* ------------------------------------------------------------------------- */
/* Stack sizing. */
/* ------------------------------------------------------------------------- */

static void test_stack_size_honours_the_guest_with_a_floor_and_a_cap(void)
{
    /* The guest's own request is what it gets, when it is sane. This title asks for
     * its XBE header's PeStackCommit, measured at 0x10000. */
    CHECK_EQ_U32(kernel_thread_stack_size_for(0x10000u), 0x10000u);
    CHECK_EQ_U32(kernel_thread_stack_size_for(0x40000u), 0x40000u);

    /* Nobody chose, or chose something unusable: the floor. */
    CHECK_EQ_U32(kernel_thread_stack_size_for(0u), KERNEL_THREAD_STACK_MIN);
    CHECK_EQ_U32(kernel_thread_stack_size_for(16u), KERNEL_THREAD_STACK_MIN);

    /* A garbage 32-bit size must be capped, not allocated. 0xFFFFFFFF would
     * otherwise be a 4 GB request plus two guard bands, which cannot even be
     * expressed in a 32-bit region size. */
    CHECK_EQ_U32(kernel_thread_stack_size_for(0xFFFFFFFFu), KERNEL_THREAD_STACK_MAX);
    CHECK(kernel_thread_stack_size_for(0x1234u) % GUEST_PAGE_SIZE == 0u);
    CHECK(kernel_thread_stack_size_for(0x10001u) % GUEST_PAGE_SIZE == 0u);

    uint32_t bytes = 0u;
    CHECK(kernel_thread_stack_region_bytes(0x10000u, &bytes));
    /* Guard, stack, guard. If the guards were dropped this would be 0x10000. */
    CHECK_EQ_U32(bytes, KERNEL_THREAD_STACK_GUARD + 0x10000u + KERNEL_THREAD_STACK_GUARD);
}

/* ------------------------------------------------------------------------- */
/* Layout: guard bands, and esp at the right end, aligned. */
/* ------------------------------------------------------------------------- */

static void test_layout_has_a_guard_band_on_both_sides(void)
{
    uint32_t bytes = 0u;
    CHECK(kernel_thread_stack_region_bytes(0x10000u, &bytes));
    const uint32_t base = alloc_region(bytes, GUEST_ALLOCATION_GRANULARITY);
    if (base == 0u) {
        return;
    }

    kernel_thread_stack stack;
    CHECK(kernel_thread_stack_layout(&stack, base, 0x10000u));

    CHECK(stack.guard_bytes > 0u);
    CHECK_EQ_U32(stack.guard_bytes, KERNEL_THREAD_STACK_GUARD);
    /* A band BELOW the usable stack: this is the one an overflow runs into. */
    CHECK_EQ_U32(stack.low, base + stack.guard_bytes);
    CHECK(stack.low > stack.region_base);
    /* And a band ABOVE it, so an underflow is caught too. */
    CHECK_EQ_U32(stack.region_bytes, stack.guard_bytes + (stack.high - stack.low)
                                         + stack.guard_bytes);
    CHECK(stack.region_base + stack.region_bytes > stack.high);
    CHECK_EQ_U32(stack.high - stack.low, 0x10000u);

    /* A single guard page is jumpable by `sub esp, 0x8000`; the band must be big
     * enough that a plausible frame cannot step over it. */
    CHECK(stack.guard_bytes >= 0x10000u);

    (void)guest_region_free(base);
}

static void test_esp_is_at_the_top_of_the_stack_and_aligned(void)
{
    uint32_t bytes = 0u;
    CHECK(kernel_thread_stack_region_bytes(0x10000u, &bytes));
    const uint32_t base = alloc_region(bytes, GUEST_ALLOCATION_GRANULARITY);
    if (base == 0u) {
        return;
    }

    kernel_thread_stack stack;
    CHECK(kernel_thread_stack_layout(&stack, base, 0x10000u));
    /* Before a frame is written there is no esp, and that must be visible rather
     * than being a plausible-looking address. */
    CHECK_EQ_U32(stack.esp, 0u);
    /* The end a downward-growing stack starts from must be 16-byte aligned. */
    CHECK_EQ_U32(stack.high % 16u, 0u);

    const uint32_t frame[3] = {0xDEADF00Du, 0x00112233u, 0x44556677u};
    CHECK(kernel_thread_stack_write_frame(&stack, frame, 3u));

    /* INSIDE the stack, not in either guard band. */
    CHECK(stack.esp >= stack.low);
    CHECK(stack.esp < stack.high);

    /* At the TOP end. A stack grows down, so an esp placed at the bottom has the
     * whole stack above it and nowhere to push into: the first call would walk
     * straight into the low guard. Checked as a position, not just a range, because
     * `low` is also "inside the stack". */
    CHECK(stack.esp > stack.low + (stack.high - stack.low) / 2u);
    CHECK_EQ_U32(stack.high - stack.esp, 3u * 4u);

    /* Four-byte aligned at minimum, since every guest push is a dword. And, given a
     * 16-aligned top and three pushed slots, exactly 4 mod 16 -- which pins the
     * alignment and the frame size in one check. */
    CHECK_EQ_U32(stack.esp % 4u, 0u);
    CHECK_EQ_U32(stack.esp % 16u, 4u);
    CHECK_EQ_U32(stack.frame_slots, 3u);

    (void)guest_region_free(base);
}

static void test_a_frame_too_big_for_the_stack_is_refused(void)
{
    uint32_t bytes = 0u;
    CHECK(kernel_thread_stack_region_bytes(0u, &bytes));
    const uint32_t base = alloc_region(bytes, GUEST_ALLOCATION_GRANULARITY);
    if (base == 0u) {
        return;
    }
    kernel_thread_stack stack;
    CHECK(kernel_thread_stack_layout(&stack, base, 0u));
    /* Deliberately absurd. Refused, rather than writing an esp below `low`. */
    static uint32_t huge[4] = {0u, 0u, 0u, 0u};
    kernel_thread_stack narrow = stack;
    narrow.high = narrow.low + 8u;
    CHECK(!kernel_thread_stack_write_frame(&narrow, huge, 4u));
    CHECK_EQ_U32(narrow.esp, 0u);
    (void)guest_region_free(base);
}

static void test_an_unaligned_region_is_refused(void)
{
    kernel_thread_stack stack;
    /* mprotect needs a page-aligned base, so an unaligned region would produce a
     * stack that LOOKED guarded and was not. Refused instead. */
    CHECK(!kernel_thread_stack_layout(&stack, 0x40000001u, 0x10000u));
    CHECK(!kernel_thread_stack_layout(&stack, 0u, 0x10000u));
}

/* ------------------------------------------------------------------------- */
/* The guard bands must actually fault. */
/* ------------------------------------------------------------------------- */

static sigjmp_buf guard_jmp;
static volatile sig_atomic_t guard_faulted;

static void guard_handler(int signal_number)
{
    (void)signal_number;
    guard_faulted = 1;
    siglongjmp(guard_jmp, 1);
}

/* Write a word and report whether it faulted. */
static bool write_faults(uint32_t address)
{
    struct sigaction action;
    struct sigaction previous;
    memset(&action, 0, sizeof(action));
    action.sa_handler = guard_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_NODEFER;
    (void)sigaction(SIGSEGV, &action, &previous);

    guard_faulted = 0;
    if (sigsetjmp(guard_jmp, 1) == 0) {
        *(volatile uint32_t *)(uintptr_t)address = 0xA5A5A5A5u;
    }
    (void)sigaction(SIGSEGV, &previous, NULL);
    return guard_faulted != 0;
}

static void test_guard_bands_really_fault(void)
{
    uint32_t bytes = 0u;
    CHECK(kernel_thread_stack_region_bytes(0x10000u, &bytes));
    const uint32_t base = alloc_region(bytes, GUEST_ALLOCATION_GRANULARITY);
    if (base == 0u) {
        return;
    }
    kernel_thread_stack stack;
    CHECK(kernel_thread_stack_layout(&stack, base, 0x10000u));

    /* Before arming, the whole region is ordinary committed guest memory. Proving
     * that first is what makes the post-arm result evidence: otherwise a test that
     * faults everywhere would look like a working guard. */
    CHECK(!write_faults(stack.region_base));
    CHECK(!write_faults(stack.low));

    CHECK(kernel_thread_stack_arm_guards(&stack));

    /* Guarded HLE copies refuse the real stack guards without taking a signal. */
    uint32_t guarded_word = 0xDEADBEEFu;
    uint8_t guarded_byte = 0xEFu;
    CHECK(!kernel_guest_read_u32(stack.low - 4u, &guarded_word));
    CHECK_EQ_U32(guarded_word, 0xDEADBEEFu);
    CHECK(!kernel_guest_read_u8(stack.high, &guarded_byte));
    CHECK_EQ_U32(guarded_byte, 0xEFu);
    CHECK(!kernel_guest_write_u32(stack.low - 4u, 1u));
    CHECK(!kernel_guest_write_u8(stack.high, 1u));
    CHECK(kernel_guest_write_u32(stack.low, 0x12345678u));
    CHECK(kernel_guest_read_u32(stack.low, &guarded_word));
    CHECK_EQ_U32(guarded_word, 0x12345678u);

    /* An overflow runs off the bottom. It must fault. */
    CHECK(write_faults(stack.low - 4u));
    CHECK(write_faults(stack.region_base));
    /* And an underflow off the top. */
    CHECK(write_faults(stack.low + (stack.high - stack.low) + 4u));
    /* While the usable stack itself stays writable -- a guard that protected the
     * stack too would stop the thread rather than catching its overflow. */
    CHECK(!write_faults(stack.low));
    CHECK(!write_faults(stack.high - 4u));

    (void)guest_region_free(base);
}

/* ------------------------------------------------------------------------- */
/* The per-thread KPCR. */
/* ------------------------------------------------------------------------- */

static void test_control_block_matches_the_measured_fs_offsets(void)
{
    const uint32_t control = alloc_region(KERNEL_THREAD_CONTROL_BYTES, 0u);
    const uint32_t tls = alloc_region(GUEST_PAGE_SIZE, 0u);
    const uint32_t monitor = alloc_region(KERNEL_THREAD_MONITOR_BYTES, 0u);
    if (control == 0u || tls == 0u || monitor == 0u) {
        return;
    }
    CHECK(kernel_thread_control_init(control, tls, monitor,
                                     KERNEL_THREAD_MONITOR_BYTES));

    uint32_t value = 0u;

    /* Every probe below uses the LITERAL fs offset, never the KERNEL_PCR_* macro:
     * the macros are what this test checks, and reading through them moves the
     * assertion together with the code, so a wrong offset would pass (the fs:[4]
     * test survived exactly that mutation until it asserted the literal 4u). The
     * ground truth for each literal is the guest's own instruction. */

    /* fs:[0x00] -- SEH chain head. 0xFFFFFFFF, not 0: an unwind follows the chain
     * until it sees the terminator, so a zero would be dereferenced as a frame.
     * The literal 0xFFFFFFFFu is NT's sentinel, not KERNEL_SEH_CHAIN_END. */
    CHECK(kernel_guest_read_u32(control + 0u, &value));
    CHECK_EQ_U32(value, 0xFFFFFFFFu);

    /* fs:[0x20] -- Prcb, pointing at this PCR's own PrcbData at 0x28. That identity
     * is what makes the 22 measured `fs:[0x20]` sites and the 6 `fs:[0x28]` sites
     * agree with each other. */
    CHECK(kernel_guest_read_u32(control + 0x20u, &value));
    CHECK_EQ_U32(value, control + 0x28u);

    /* fs:[0x24] -- Irql, at PASSIVE. */
    CHECK(kernel_guest_read_u32(control + 0x24u, &value));
    CHECK_EQ_U32(value, 0u);

    /* fs:[0x28] -- PrcbData.CurrentThread. The guest's thread startup shim reads
     * this and immediately dereferences it, so it must not be zero. */
    uint32_t kthread = 0u;
    CHECK(kernel_guest_read_u32(control + 0x28u, &kthread));
    CHECK(kthread != 0u);

    /* KTHREAD+0x28 -- TlsData, the offset the guest's TLS arithmetic reads. The shim
     * writes a self-pointer into it and then copies the title's TLS template over it,
     * so it must be real memory. */
    CHECK(kernel_guest_read_u32(kthread + 0x28u, &value));
    CHECK_EQ_U32(value, tls);

    /* The Prcb must be large enough for every field the guest touches, up to 0x254.
     * The field at +0x250 is NOT zero -- it points at the monitor block, which is
     * what keeps the guest out of the GDT path. test_prcb_monitor.c owns that
     * invariant and its mutations; this is the layout half of it. */
    CHECK(control + KERNEL_PCR_PRCB_DATA + KERNEL_PRCB_BYTES_TOUCHED
          <= control + KERNEL_THREAD_CONTROL_BYTES);
    /* Prcb+0x250 is where the guest's 19 measured read sites look, so the probe is
     * 0x28 + 0x250 literally, not PRCB_DATA + PRCB_MONITOR. */
    CHECK(kernel_guest_read_u32(control + 0x28u + 0x250u, &value));
    CHECK_EQ_U32(value, monitor);

    /* fs:[0x58] is read and compared to zero at 6 sites. */
    CHECK(kernel_guest_read_u32(control + 0x58u, &value));
    CHECK_EQ_U32(value, 0u);

    CHECK(!kernel_thread_control_init(0u, tls, monitor, KERNEL_THREAD_MONITOR_BYTES));
    CHECK(!kernel_thread_control_init(control, 0u, monitor,
                                      KERNEL_THREAD_MONITOR_BYTES));

    (void)guest_region_free(control);
    (void)guest_region_free(tls);
    (void)guest_region_free(monitor);
}

/* ------------------------------------------------------------------------- */
/* Starting a thread, through the real ordinal, with fake host ops. */
/* ------------------------------------------------------------------------- */

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_cond = PTHREAD_COND_INITIALIZER;
static kernel_thread_launch fake_launch;
static unsigned fake_entered;
static bool fake_block;
static bool fake_release;

static bool fake_has_code(uint32_t guest_va)
{
    /* Anything in a plausible code range. A real host asks the lift. */
    return guest_va >= 0x10000u;
}

static void fake_enter(const kernel_thread_launch *launch)
{
    pthread_mutex_lock(&fake_lock);
    fake_launch = *launch;
    fake_entered++;
    pthread_cond_broadcast(&fake_cond);
    while (fake_block && !fake_release) {
        (void)pthread_cond_wait(&fake_cond, &fake_lock);
    }
    pthread_mutex_unlock(&fake_lock);
}

static void fake_reset(bool block)
{
    pthread_mutex_lock(&fake_lock);
    memset(&fake_launch, 0, sizeof(fake_launch));
    fake_entered = 0u;
    fake_block = block;
    fake_release = false;
    pthread_mutex_unlock(&fake_lock);
}

static const kernel_thread_host_ops FAKE_OPS = {
    .has_code = fake_has_code,
    .enter = fake_enter,
    .terminate = NULL,
};

/* Call ordinal 255 with a full 10-argument frame, returning the handle written. */
static uint32_t create_thread(uint32_t stack_size, uint32_t tls_size,
                              uint32_t start_routine, uint32_t start_context,
                              uint32_t system_routine, uint32_t suspended,
                              uint32_t *out_status)
{
    const uint32_t scratch = alloc_region(0x4000u, 0u);
    if (scratch == 0u) {
        return 0u;
    }
    const uint32_t handle_slot = scratch;
    const uint32_t args[10] = {
        handle_slot, 0u,            stack_size,     tls_size,  0u,
        start_routine, start_context, suspended,    0u,        system_routine,
    };
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    CHECK(kernel_frame_build(&frame, scratch + 0x100u, 0x100u, args, 10u));
    const uint32_t status = kernel_hle_call(255u, &frame);
    if (out_status) {
        *out_status = status;
    }
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(handle_slot, &handle));
    return handle;
}

/* Wait until `count` threads have entered, or give up. */
static bool wait_for_entries(unsigned count)
{
    struct timespec deadline;
    if (timespec_get(&deadline, TIME_UTC) != TIME_UTC) {
        return false;
    }
    deadline.tv_sec += 5;
    bool reached = false;
    pthread_mutex_lock(&fake_lock);
    while (fake_entered < count) {
        if (pthread_cond_timedwait(&fake_cond, &fake_lock, &deadline) != 0) {
            break;
        }
    }
    reached = fake_entered >= count;
    pthread_mutex_unlock(&fake_lock);
    return reached;
}

static void begin_case(void)
{
    kernel_hle_init();
    kernel_hle_set_log(quiet_log);
    kernel_object_reset();
    CHECK(kernel_thread_reset());
    CHECK_EQ_U32(kernel_thread_register(), 10u);
}

static void end_case(void)
{
    CHECK(kernel_thread_set_host_ops(NULL));
    CHECK(kernel_thread_reset());
    kernel_hle_set_log(NULL);
}

static void test_a_started_thread_is_marked_started_and_entered_correctly(void)
{
    begin_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(false);

    uint32_t status = 0xFFFFFFFFu;
    const uint32_t handle = create_thread(0x10000u, 0x14u, 0x003801D9u, 0xCAFEu,
                                          0x0037FE1Du, 0u, &status);
    CHECK_EQ_U32(status, STATUS_SUCCESS);
    CHECK(handle != 0u);
    CHECK(wait_for_entries(1u));
    CHECK_EQ_U32(kernel_thread_join_all(5000u), 0u);

    /* THE flag. Without it, "requested" and "running" are indistinguishable and the
     * outstanding-thread count that explains a hang becomes a lie. */
    CHECK_EQ_U32(kernel_thread_started_count(), 1u);
    CHECK_EQ_U32(kernel_thread_unstarted_count(), 0u);
    CHECK_EQ_U32(kernel_thread_running_count(), 0u);

    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    CHECK(record.started);
    CHECK(record.finished);
    CHECK_EQ_U32(record.handle, handle);
    CHECK_EQ_U32(record.start_routine, 0x003801D9u);
    CHECK_EQ_U32(record.start_context, 0xCAFEu);
    CHECK_EQ_U32(record.system_routine, 0x0037FE1Du);
    /* Entered at the SystemRoutine, which is the whole derivation in the header:
     * the routine the guest supplies is the one the kernel calls, and it in turn
     * calls the start routine. */
    CHECK_EQ_U32(record.entry_va, 0x0037FE1Du);
    CHECK(record.guards_armed);
    CHECK(record.control_base != 0u);
    CHECK(record.tls_base != 0u);
    CHECK_EQ_U32(record.stack_high - record.stack_low, 0x10000u);

    /*
     * fs:[4] IS THE END OF THE REQUESTED TLS BLOCK, published on the PRODUCTION creation
     * path. The title's XapiSetLastError reads [fs:[4] + index*4] with a negative index, so
     * a zero here faults at 0xFFFFFFEC on the first SetLastError of the boot -- which was
     * misattributed to DirectSound until gdb showed the TLS arithmetic. Asserted through
     * the real path rather than by calling the publisher directly, because the publisher
     * being correct and never called is exactly the "built but unreachable" failure.
     * Size is asserted non-zero so the equality cannot hold vacuously with a size of 0.
     */
    CHECK(record.tls_data_size > 0u);
    uint32_t tls_end = 0u;
    /* The LITERAL 4u, not KERNEL_PCR_TLS_END: reading through the macro under test moves
     * the assertion together with the code, so a wrong offset would pass. The guest's
     * instruction is `mov eax, fs:[4]`, and that is the independent fact. */
    CHECK(kernel_guest_read_u32(record.control_base + 4u, &tls_end));
    CHECK_EQ_U32(tls_end, record.tls_base + record.tls_data_size);

    pthread_mutex_lock(&fake_lock);
    const kernel_thread_launch launch = fake_launch;
    pthread_mutex_unlock(&fake_lock);

    CHECK_EQ_U32(launch.handle, handle);
    CHECK_EQ_U32(launch.entry_va, 0x0037FE1Du);
    CHECK_EQ_U32(launch.esp, record.initial_esp);
    /* A thread with no `fs` base faults on its tenth instruction, so this is not
     * optional state. */
    CHECK_EQ_U32(launch.fs_base, record.control_base);
    CHECK(launch.esp >= launch.stack_low);
    CHECK(launch.esp < launch.stack_high);

    end_case();
}

static void test_the_entry_frame_matches_the_derived_convention(void)
{
    begin_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(false);

    const uint32_t handle = create_thread(0x10000u, 0x14u, 0xAAAA1111u, 0xBBBB2222u,
                                          0xCCCC3333u, 0u, NULL);
    CHECK(wait_for_entries(1u));
    CHECK_EQ_U32(kernel_thread_join_all(5000u), 0u);

    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    const uint32_t esp = record.initial_esp;
    CHECK(esp != 0u);

    uint32_t value = 0u;
    /* [esp+0] return address. A recognisable sentinel, because the measured exit
     * path is PsTerminateSystemThread and never a return -- so this value showing
     * up anywhere means the convention is wrong. */
    CHECK(kernel_guest_read_u32(esp, &value));
    CHECK(value != 0xAAAA1111u);
    CHECK(value != 0xBBBB2222u);
    CHECK(value != 0xCCCC3333u);

    /* [esp+4] = argument 0 = StartRoutine, because the supplied routine does
     * `call [ebp+8]`. Distinct sentinels, so a swap with StartContext is caught
     * rather than both being plausible pointers. */
    CHECK(kernel_guest_read_u32(esp + 4u, &value));
    CHECK_EQ_U32(value, 0xAAAA1111u);

    /* [esp+8] = argument 1 = StartContext, because it does `push [ebp+0x0C]`. */
    CHECK(kernel_guest_read_u32(esp + 8u, &value));
    CHECK_EQ_U32(value, 0xBBBB2222u);

    end_case();
}

static void test_no_system_routine_falls_back_to_the_start_routine(void)
{
    begin_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(false);

    const uint32_t handle = create_thread(0x10000u, 0u, 0xAAAA1111u, 0xBBBB2222u,
                                          0u /* no SystemRoutine */, 0u, NULL);
    CHECK(wait_for_entries(1u));
    CHECK_EQ_U32(kernel_thread_join_all(5000u), 0u);

    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    CHECK_EQ_U32(record.entry_va, 0xAAAA1111u);

    uint32_t value = 0u;
    /* Two slots, not three: StartRoutine is now the thing being entered, so its own
     * single argument is StartContext. */
    CHECK(kernel_guest_read_u32(record.initial_esp + 4u, &value));
    CHECK_EQ_U32(value, 0xBBBB2222u);
    CHECK_EQ_U32(record.stack_high - record.initial_esp, 2u * 4u);

    end_case();
}

static void test_a_suspended_thread_is_recorded_and_not_started(void)
{
    begin_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(false);

    uint32_t status = 0xFFFFFFFFu;
    const uint32_t handle = create_thread(0x10000u, 0u, 0x003801D9u, 0u, 0x0037FE1Du,
                                          1u /* CreateSuspended */, &status);
    /* The ordinal still SUCCEEDS and the handle is still valid -- the guest
     * dereferences it immediately either way. */
    CHECK_EQ_U32(status, STATUS_SUCCESS);
    CHECK(handle != 0u);
    CHECK_EQ_U32(kernel_thread_started_count(), 0u);
    CHECK_EQ_U32(kernel_thread_unstarted_count(), 1u);

    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    CHECK(record.created_suspended);
    CHECK(!record.started);
    /* Provisioned anyway, so a future NtResumeThread has something to resume. */
    CHECK(record.initial_esp != 0u);

    end_case();
}

static void test_without_host_ops_nothing_starts(void)
{
    /* The documented fallback, and the reason this file builds in a fresh clone:
     * with no way to run guest code, the request is recorded and loudly not
     * started, exactly as before a thread model existed. */
    begin_case();
    CHECK(kernel_thread_set_host_ops(NULL));

    uint32_t status = 0xFFFFFFFFu;
    const uint32_t handle = create_thread(0x10000u, 0u, 0x003801D9u, 0u, 0x0037FE1Du,
                                          0u, &status);
    CHECK_EQ_U32(status, STATUS_SUCCESS);
    CHECK(handle != 0u);
    CHECK_EQ_U32(kernel_thread_started_count(), 0u);
    CHECK_EQ_U32(kernel_thread_unstarted_count(), 1u);
    CHECK_EQ_U32(kernel_thread_running_count(), 0u);

    end_case();
}

static void test_an_entry_with_no_code_is_refused_before_starting(void)
{
    begin_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(false);

    /* fake_has_code rejects anything below 0x10000, standing in for "the lift
     * produced nothing there". Screened on the CREATING thread, so the address is
     * named rather than vanishing into a thread nobody is watching. */
    const uint32_t handle = create_thread(0x10000u, 0u, 0x1234u, 0u, 0u, 0u, NULL);
    CHECK(handle != 0u);
    CHECK_EQ_U32(kernel_thread_started_count(), 0u);
    CHECK_EQ_U32(kernel_thread_unstarted_count(), 1u);

    end_case();
}

/* ------------------------------------------------------------------------- */
/* The watchdog. */
/* ------------------------------------------------------------------------- */

static void test_the_join_watchdog_gives_up_and_says_so(void)
{
    begin_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    /* A thread that does not finish -- which is exactly what a hung guest thread
     * looks like from here. */
    fake_reset(true);

    const uint32_t handle = create_thread(0x10000u, 0u, 0x003801D9u, 0u, 0x0037FE1Du,
                                          0u, NULL);
    CHECK(handle != 0u);
    CHECK(wait_for_entries(1u));
    CHECK_EQ_U32(kernel_thread_started_count(), 1u);

    /* The whole point: the wait is BOUNDED. A hang that is reported is
     * diagnosable; a hang that is waited on forever is the worst outcome
     * available. */
    CHECK_EQ_U32(kernel_thread_join_all(50u), 1u);
    CHECK_EQ_U32(kernel_thread_running_count(), 1u);
    /* And the table must refuse to be torn down under a running thread, because
     * freeing a live thread's stack is a use-after-free with a baffling crash. */
    CHECK(!kernel_thread_reset());
    CHECK(!kernel_thread_set_host_ops(NULL));

    pthread_mutex_lock(&fake_lock);
    fake_release = true;
    pthread_cond_broadcast(&fake_cond);
    pthread_mutex_unlock(&fake_lock);

    CHECK_EQ_U32(kernel_thread_join_all(5000u), 0u);
    CHECK_EQ_U32(kernel_thread_running_count(), 0u);

    end_case();
}

static void test_several_threads_get_distinct_stacks(void)
{
    begin_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(true);

    uint32_t handles[4] = {0u, 0u, 0u, 0u};
    for (unsigned i = 0u; i < 4u; i++) {
        handles[i] = create_thread(0x10000u, 0u, 0x003801D9u + i, 0u, 0x0037FE1Du, 0u,
                                   NULL);
        CHECK(handles[i] != 0u);
    }
    CHECK(wait_for_entries(4u));
    CHECK_EQ_U32(kernel_thread_started_count(), 4u);

    kernel_thread_record records[4];
    for (unsigned i = 0u; i < 4u; i++) {
        CHECK(kernel_thread_get(handles[i], &records[i]));
    }
    for (unsigned i = 0u; i < 4u; i++) {
        for (unsigned j = i + 1u; j < 4u; j++) {
            CHECK(handles[i] != handles[j]);
            CHECK(records[i].control_base != records[j].control_base);
            /* Non-overlapping stacks. Two threads sharing one would corrupt each
             * other's frames with no sign of where it came from. */
            CHECK(records[i].stack_high <= records[j].stack_low
                  || records[j].stack_high <= records[i].stack_low);
        }
    }

    pthread_mutex_lock(&fake_lock);
    fake_release = true;
    pthread_cond_broadcast(&fake_cond);
    pthread_mutex_unlock(&fake_lock);
    CHECK_EQ_U32(kernel_thread_join_all(5000u), 0u);

    end_case();
}

int main(void)
{
    test_stack_size_honours_the_guest_with_a_floor_and_a_cap();
    test_layout_has_a_guard_band_on_both_sides();
    test_esp_is_at_the_top_of_the_stack_and_aligned();
    test_a_frame_too_big_for_the_stack_is_refused();
    test_an_unaligned_region_is_refused();
    test_guard_bands_really_fault();
    test_control_block_matches_the_measured_fs_offsets();
    test_a_started_thread_is_marked_started_and_entered_correctly();
    test_the_entry_frame_matches_the_derived_convention();
    test_no_system_routine_falls_back_to_the_start_routine();
    test_a_suspended_thread_is_recorded_and_not_started();
    test_without_host_ops_nothing_starts();
    test_an_entry_with_no_code_is_refused_before_starting();
    test_the_join_watchdog_gives_up_and_says_so();
    test_several_threads_get_distinct_stacks();

    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("guest_thread: all checks passed\n");
    return 0;
}
