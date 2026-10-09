/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#include "kernel_event.h"
#include "kernel_event_handle.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_sync.h"
#include "kernel_thread.h"
#include "nt_status.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
#define EQ(a,b) CHECK((uint32_t)(a) == (uint32_t)(b))
static uint32_t scratch;
static int quiet(const char *format, ...) { (void)format; return 0; }
static void setup(void)
{
    kernel_object_reset();
    CHECK(kernel_thread_reset());
    CHECK(kernel_thread_set_host_ops(NULL));
    guest_mem_reset();
    kernel_hle_init();
    kernel_hle_set_log(quiet);
    kernel_sync_reset();
    kernel_event_reset();
    (void)kernel_object_register();
    (void)kernel_thread_register();
    (void)kernel_sync_register();
    EQ(kernel_event_handle_register(), 2u);
    EQ(kernel_event_handle_register(), 2u);
    guest_region_request request = {.bytes = 0x4000u, .state = MEM_COMMIT,
                                   .protect = PAGE_READWRITE};
    nt_status status = 0u;
    scratch = guest_region_alloc(&request, &status);
    CHECK(scratch != 0u);
}
static uint32_t call(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x100u, 0x100u, args, count));
    return kernel_hle_call(ordinal, &frame);
}
static uint32_t create(uint32_t output, uint32_t attrs, uint32_t type, uint32_t initial)
{
    const uint32_t args[4] = {output, attrs, type, initial};
    return call(189u, args, 4u);
}
static uint32_t read32(uint32_t address)
{
    uint32_t result = 0u;
    CHECK(kernel_guest_read_u32(address, &result));
    return result;
}
static void test_metadata_and_lifetime(void)
{
    setup();
    unsigned char memory[128], after[128];
    memset(memory, 0x59, sizeof(memory));
    CHECK(kernel_guest_write_bytes(scratch + 0x2000u, memory, sizeof(memory)));
    uint64_t clock = kernel_clock_peek();
    unsigned events = kernel_event_tracked_count();
    EQ(create(scratch + 0x400u, 0u, 1u, 0u), STATUS_SUCCESS);
    uint32_t handle = read32(scratch + 0x400u);
    kernel_object_entry entry;
    CHECK(kernel_object_get_copy(handle, &entry));
    EQ(entry.kind, KERNEL_OBJECT_EVENT);
    EQ(entry.event_type, 1u);
    EQ(entry.event_initial_state, 0u);
    EQ(entry.references, 1u);
    EQ(entry.thread_body, 0u);
    CHECK(kernel_clock_peek() == clock);
    EQ(kernel_event_tracked_count(), events);
    CHECK(kernel_guest_read_bytes(scratch + 0x2000u, after, sizeof(after)));
    CHECK(memcmp(memory, after, sizeof(memory)) == 0);
    CHECK(kernel_guest_write_u32(scratch + 0x404u, 0xBAD12345u));
    const uint32_t ref[3] = {handle, 0u, scratch + 0x404u};
    EQ(call(246u, ref, 3u), STATUS_NOT_IMPLEMENTED);
    EQ(read32(scratch + 0x404u), 0xBAD12345u);
    kernel_call_frame fast = {.ecx = handle, .has_registers = true};
    (void)kernel_hle_call(251u, &fast);
    (void)kernel_hle_call(250u, &fast);
    CHECK(kernel_object_get_copy(handle, &entry));
    EQ(entry.references, 1u);
    const uint32_t close[1] = {handle};
    EQ(call(187u, close, 1u), STATUS_SUCCESS);
    EQ(call(187u, close, 1u), STATUS_INVALID_HANDLE);
    CHECK(!kernel_object_get_copy(handle, &entry));
    /* Force FIFO slot recycling, then verify generation rejects the old handle. */
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        EQ(create(scratch + 0x408u, 0u, 1u, 0xFFFF0000u), STATUS_SUCCESS);
        uint32_t next = read32(scratch + 0x408u);
        CHECK(next != handle);
        CHECK(kernel_object_get_copy(next, &entry));
        EQ(entry.event_initial_state, 0u); /* BOOLEAN upper bytes ignored. */
        uint32_t close_next[1] = {next};
        EQ(call(187u, close_next, 1u), STATUS_SUCCESS);
    }
    EQ(call(187u, close, 1u), STATUS_INVALID_HANDLE);
}
static void test_refusals_permissions_and_oom(void)
{
    setup();
    CHECK(kernel_guest_write_u32(scratch + 0x400u, 0x12345678u));
    /* T142: an OBJECT_ATTRIBUTES pointer of 1 is an UNREADABLE block, and the event
     * handler now mirrors the mutant's contract for it (kernel_object.c refuses an
     * unreadable block with STATUS_INVALID_PARAMETER, never a guess). A readable
     * block is a different refusal: see test_kernel_object.c's named-event tests. */
    EQ(create(scratch + 0x400u, 1u, 1u, 0u), STATUS_INVALID_PARAMETER);
    EQ(create(scratch + 0x400u, 1u, 0u, 0u), STATUS_INVALID_PARAMETER);
    /* T270: Type 0 is created now, so the Type refusal is Type 2 and above, and the
     * initially-signaled refusal holds for BOTH measured Types. */
    EQ(create(scratch + 0x400u, 0u, 2u, 0u), STATUS_NOT_IMPLEMENTED);
    EQ(create(scratch + 0x400u, 0u, 3u, 0u), STATUS_NOT_IMPLEMENTED);
    EQ(create(scratch + 0x400u, 0u, 0xFFFFFFFFu, 0u), STATUS_NOT_IMPLEMENTED);
    EQ(create(scratch + 0x400u, 0u, 1u, 1u), STATUS_NOT_IMPLEMENTED);
    EQ(create(scratch + 0x400u, 0u, 0u, 1u), STATUS_NOT_IMPLEMENTED);
    EQ(read32(scratch + 0x400u), 0x12345678u);
    kernel_call_frame fast = {.ecx = 2u, .has_registers = true};
    (void)kernel_hle_call(160u, &fast);
    EQ(create(scratch + 0x400u, 0u, 1u, 0u), STATUS_NOT_IMPLEMENTED);
    EQ(create(scratch + 0x400u, 0u, 0u, 0u), STATUS_NOT_IMPLEMENTED); /* not at PASSIVE either */
    kernel_sync_reset();
    EQ(create(0u, 0u, 1u, 0u), STATUS_INVALID_PARAMETER);
    EQ(create(0u, 0u, 0u, 0u), STATUS_INVALID_PARAMETER);
    EQ(create(0x1000u, 0u, 1u, 0u), STATUS_INVALID_PARAMETER);
    EQ(create(0xFFFFFFFEu, 0u, 1u, 0u), STATUS_INVALID_PARAMETER);
    EQ(kernel_object_live_count(), 0u);
    CHECK(kernel_guest_write_u32(scratch + 0x1000u, 0xABCD5678u));
    CHECK(kernel_guest_write_u32(scratch + 0xFFEu, 0xAABBCCDDu));
    CHECK(mprotect((void *)(uintptr_t)(scratch + 0x1000u), 0x1000u, PROT_READ) == 0);
    EQ(create(scratch + 0x1000u, 0u, 1u, 0u), STATUS_INVALID_PARAMETER);
    EQ(create(scratch + 0xFFEu, 0u, 1u, 0u), STATUS_INVALID_PARAMETER);
    EQ(read32(scratch + 0xFFEu), 0xAABBCCDDu);
    EQ(kernel_object_live_count(), 0u);
    CHECK(mprotect((void *)(uintptr_t)(scratch + 0x1000u), 0x1000u, PROT_NONE) == 0);
    EQ(create(scratch + 0x1000u, 0u, 1u, 0u), STATUS_INVALID_PARAMETER);
    CHECK(mprotect((void *)(uintptr_t)(scratch + 0x1000u), 0x1000u,
                   PROT_READ | PROT_WRITE) == 0);
    kernel_call_frame short_frame = {.stack_ptr = scratch + 0x100u,
                                     .stack_limit = scratch + 0x10Cu};
    EQ(kernel_hle_call(189u, &short_frame), STATUS_INVALID_PARAMETER);
    EQ(kernel_hle_call(189u, NULL), STATUS_INVALID_PARAMETER);
    EQ(kernel_object_live_count(), 0u);
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++)
        EQ(create(scratch + 0x400u, 0u, 1u, 0u), STATUS_SUCCESS);
    CHECK(kernel_guest_write_u32(scratch + 0x400u, 0x12345678u));
    EQ(create(scratch + 0x400u, 0u, 1u, 0u), 0xC000009Au);
    EQ(read32(scratch + 0x400u), 0x12345678u);
    EQ(kernel_object_live_count(), KERNEL_OBJECT_MAX);
}
static void test_owned_thread_body_alias(void)
{
    setup();
    uint32_t thread = kernel_object_create(KERNEL_OBJECT_THREAD, 42u);
    uint32_t body = scratch + 0x2000u;
    CHECK(kernel_object_bind_thread_body(thread, 42u, body));
    unsigned char before[0x124u], after[0x124u];
    CHECK(kernel_guest_read_bytes(body, before, sizeof(before)));
    EQ(create(body + 4u, 0u, 1u, 0u), STATUS_INVALID_PARAMETER);
    CHECK(kernel_guest_read_bytes(body, after, sizeof(after)));
    CHECK(memcmp(before, after, sizeof(before)) == 0);
    EQ(kernel_object_live_count(), 1u);
    CHECK(kernel_object_detach_thread_bodies());
}
static jmp_buf wait_jump;
static uint32_t refused_handle;
static kernel_thread_wait_refusal refused_reason;
static void refuse_wait(uint32_t handle, kernel_thread_wait_refusal reason)
{
    refused_handle = handle;
    refused_reason = reason;
    longjmp(wait_jump, 1);
}
/* T8g: an unsignaled event with an infinite wait would block, so it is still refused, now as
 * WOULD_BLOCK (the full wait contract is tests/c/test_kernel_sync_wait.c). */
static void test_an_unsignaled_event_wait_is_refused_as_would_block(void)
{
    setup();
    kernel_thread_host_ops ops = {.wait_refused = refuse_wait};
    CHECK(kernel_thread_set_host_ops(&ops));
    EQ(create(scratch + 0x400u, 0u, 1u, 0u), STATUS_SUCCESS);
    uint32_t handle = read32(scratch + 0x400u);
    if (setjmp(wait_jump) == 0) {
        const uint32_t args[4] = {handle, 1u, 0u, 0u};
        (void)call(234u, args, 4u);
        CHECK(false);
    }
    EQ(refused_handle, handle);
    EQ(refused_reason, KERNEL_THREAD_WAIT_WOULD_BLOCK);
    EQ(kernel_thread_active_wait_count(), 0u);
    kernel_object_entry entry;
    CHECK(kernel_object_get_copy(handle, &entry));
    EQ(entry.references, 1u);
    CHECK(kernel_thread_set_host_ops(NULL)); /* Refusal released the thread lock. */
    /* 225 NtSetEvent left this list when T8c implemented it. */
    const unsigned missing[] = {186u, 205u, 235u};
    for (unsigned i = 0u; i < sizeof(missing) / sizeof(missing[0]); i++) {
        const kernel_entry *row = kernel_hle_entry(missing[i]);
        CHECK(row == NULL || row->state != KERNEL_ENTRY_IMPLEMENTED);
    }
}
/* ---- 225 NtSetEvent(HANDLE, PLONG PreviousState OPTIONAL), T8c. Two stdcall arguments;
 * the four measured sites all push the literal 0 for PreviousState. */
static uint32_t set_event(uint32_t handle, uint32_t previous_out)
{
    const uint32_t args[2] = {handle, previous_out};
    return call(225u, args, 2u);
}
static uint32_t new_event(void)
{
    EQ(create(scratch + 0x500u, 0u, 1u, 0u), STATUS_SUCCESS);
    return read32(scratch + 0x500u);
}
static bool signaled(uint32_t handle)
{
    bool state = false;
    CHECK(kernel_object_event_signaled(handle, &state));
    return state;
}
static void test_set_event_signals_only_that_handle(void)
{
    setup();
    const uint32_t first = new_event();
    const uint32_t second = new_event();
    CHECK(first != second);
    CHECK(!signaled(first));
    CHECK(!signaled(second));
    const uint64_t clock = kernel_clock_peek();
    const unsigned tracked = kernel_event_tracked_count();
    EQ(set_event(first, 0u), STATUS_SUCCESS);
    CHECK(signaled(first));
    CHECK(!signaled(second));
    /* A second set of a signaled event is still success, still signaled. */
    EQ(set_event(first, 0u), STATUS_SUCCESS);
    CHECK(signaled(first));
    EQ(set_event(second, 0u), STATUS_SUCCESS);
    CHECK(signaled(second));
    /* Creation metadata is untouched, and nothing else moved. */
    kernel_object_entry entry;
    CHECK(kernel_object_get_copy(first, &entry));
    EQ(entry.kind, KERNEL_OBJECT_EVENT);
    EQ(entry.event_type, 1u);
    EQ(entry.event_initial_state, 0u);
    EQ(entry.references, 1u);
    CHECK(kernel_clock_peek() == clock);
    EQ(kernel_event_tracked_count(), tracked);
    EQ(kernel_object_live_count(), 2u);
}
/* The object half reports the state it replaced, for the day a measured site passes a
 * non-NULL PreviousState (the handler refuses one today, so only this path reaches it). */
static void test_event_set_reports_the_previous_state(void)
{
    setup();
    const uint32_t handle = new_event();
    bool previous = true;
    EQ(kernel_object_event_set(handle, &previous), STATUS_SUCCESS);
    CHECK(!previous);
    EQ(kernel_object_event_set(handle, &previous), STATUS_SUCCESS);
    CHECK(previous);
    EQ(kernel_object_event_set(handle, NULL), STATUS_SUCCESS);
    previous = true;
    EQ(kernel_object_event_set(0u, &previous), STATUS_INVALID_HANDLE);
    CHECK(previous);
}
static void test_set_event_state_dies_with_the_handle(void)
{
    setup();
    const uint32_t handle = new_event();
    EQ(set_event(handle, 0u), STATUS_SUCCESS);
    const uint32_t close[1] = {handle};
    EQ(call(187u, close, 1u), STATUS_SUCCESS);
    EQ(set_event(handle, 0u), STATUS_INVALID_HANDLE);
    bool state = true;
    CHECK(!kernel_object_event_signaled(handle, &state));
    CHECK(state);
    /* Force slot recycling: every later event starts clear, whatever the slot held. */
    for (unsigned i = 0u; i < KERNEL_OBJECT_MAX; i++) {
        const uint32_t next = new_event();
        CHECK(next != handle);
        CHECK(!signaled(next));
        EQ(set_event(next, 0u), STATUS_SUCCESS);
        const uint32_t close_next[1] = {next};
        EQ(call(187u, close_next, 1u), STATUS_SUCCESS);
    }
    EQ(set_event(handle, 0u), STATUS_INVALID_HANDLE);
}
static void test_set_event_refuses_unmeasured_previous_state(void)
{
    setup();
    const uint32_t handle = new_event();
    CHECK(kernel_guest_write_u32(scratch + 0x600u, 0x5A5A5A5Au));
    EQ(set_event(handle, scratch + 0x600u), STATUS_NOT_IMPLEMENTED);
    EQ(read32(scratch + 0x600u), 0x5A5A5A5Au);
    CHECK(!signaled(handle));
    /* The refusal names the unmeasured argument. */
    EQ(set_event(handle, 0u), STATUS_SUCCESS);
    CHECK(signaled(handle));
}
static void test_set_event_rejects_bad_handles_and_kinds(void)
{
    setup();
    EQ(set_event(0u, 0u), STATUS_INVALID_HANDLE);
    EQ(set_event(0x12345u, 0u), STATUS_INVALID_HANDLE);
    EQ(set_event(0xFFFFFFFFu, 0u), STATUS_INVALID_HANDLE);
    const uint32_t file = kernel_object_create(KERNEL_OBJECT_FILE, 7u);
    const uint32_t mutant = kernel_object_create(KERNEL_OBJECT_MUTANT, 7u);
    CHECK(file != 0u && mutant != 0u);
    EQ(set_event(file, 0u), STATUS_OBJECT_TYPE_MISMATCH);
    EQ(STATUS_OBJECT_TYPE_MISMATCH, 0xC0000024u); /* the NT value, not just the macro */
    EQ(set_event(mutant, 0u), STATUS_OBJECT_TYPE_MISMATCH);
    bool state = true;
    CHECK(!kernel_object_event_signaled(file, &state));
    CHECK(state);
    kernel_call_frame short_frame = {.stack_ptr = scratch + 0x100u,
                                     .stack_limit = scratch + 0x108u};
    EQ(kernel_hle_call(225u, &short_frame), STATUS_INVALID_PARAMETER);
    EQ(kernel_hle_call(225u, NULL), STATUS_INVALID_PARAMETER);
    EQ(kernel_object_live_count(), 2u);
}
/* ---- T270: Type 0 (NotificationEvent, manual reset) creation, the XNET 0x431D5F shape:
 * NULL ObjectAttributes, Type 0, initially clear. */
static uint32_t new_notification_event(void)
{
    EQ(create(scratch + 0x520u, 0u, 0u, 0u), STATUS_SUCCESS);
    return read32(scratch + 0x520u);
}
static uint32_t wait_zero(uint32_t handle)
{
    const int64_t zero = 0;
    CHECK(kernel_guest_write_bytes(scratch + 0x700u, &zero, sizeof(zero)));
    const uint32_t args[4] = {handle, 1u, 0u, scratch + 0x700u};
    return call(234u, args, 4u);
}
static void test_type0_creation_publishes_a_clear_manual_reset_event(void)
{
    setup();
    const uint64_t clock = kernel_clock_peek();
    const unsigned tracked = kernel_event_tracked_count();
    const uint32_t handle = new_notification_event();
    kernel_object_entry entry;
    CHECK(kernel_object_get_copy(handle, &entry));
    EQ(entry.kind, KERNEL_OBJECT_EVENT);
    EQ(entry.event_type, 0u);
    EQ(entry.event_initial_state, 0u);
    EQ(entry.references, 1u);
    EQ(entry.thread_body, 0u);
    CHECK(!signaled(handle));
    CHECK(kernel_clock_peek() == clock);
    EQ(kernel_event_tracked_count(), tracked);
    EQ(kernel_object_live_count(), 1u);
    /* Type 0 and Type 1 are distinct objects with their own recorded type. */
    const uint32_t synchronization = new_event();
    CHECK(synchronization != handle);
    CHECK(kernel_object_get_copy(synchronization, &entry));
    EQ(entry.event_type, 1u);
    CHECK(kernel_object_get_copy(handle, &entry));
    EQ(entry.event_type, 0u);
    const uint32_t close[1] = {handle};
    EQ(call(187u, close, 1u), STATUS_SUCCESS);
    EQ(call(187u, close, 1u), STATUS_INVALID_HANDLE);
    EQ(kernel_object_live_count(), 1u);
}
/* An unsignaled zero-timeout wait is STATUS_TIMEOUT, a set event satisfies the wait, and a
 * wait on a manual-reset event does NOT clear it (a Type 1 event IS cleared, the control). */
static void test_type0_wait_semantics(void)
{
    setup();
    const uint32_t handle = new_notification_event();
    EQ(wait_zero(handle), 0x00000102u);
    CHECK(!signaled(handle));
    EQ(set_event(handle, 0u), STATUS_SUCCESS);
    CHECK(signaled(handle));
    EQ(wait_zero(handle), STATUS_SUCCESS);
    CHECK(signaled(handle));
    EQ(wait_zero(handle), STATUS_SUCCESS);
    CHECK(signaled(handle));
    const uint32_t synchronization = new_event();
    EQ(set_event(synchronization, 0u), STATUS_SUCCESS);
    EQ(wait_zero(synchronization), STATUS_SUCCESS);
    CHECK(!signaled(synchronization));
    EQ(wait_zero(synchronization), 0x00000102u);
    /* NtClearEvent (186) and NtPulseEvent (205) have no handler: nothing resets a Type 0 event. */
    const unsigned missing[] = {186u, 205u};
    for (unsigned i = 0u; i < sizeof(missing) / sizeof(missing[0]); i++) {
        const kernel_entry *row = kernel_hle_entry(missing[i]);
        CHECK(row == NULL || row->state != KERNEL_ENTRY_IMPLEMENTED);
    }
    CHECK(signaled(handle));
}
int main(void)
{
    test_metadata_and_lifetime();
    test_refusals_permissions_and_oom();
    test_owned_thread_body_alias();
    test_an_unsignaled_event_wait_is_refused_as_would_block();
    test_set_event_signals_only_that_handle();
    test_event_set_reports_the_previous_state();
    test_set_event_state_dies_with_the_handle();
    test_set_event_refuses_unmeasured_previous_state();
    test_set_event_rejects_bad_handles_and_kinds();
    test_type0_creation_publishes_a_clear_manual_reset_event();
    test_type0_wait_semantics();
    kernel_object_reset();
    CHECK(kernel_thread_reset());
    guest_mem_reset();
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
