/* SPDX-License-Identifier: GPL-3.0-or-later
 * Measured GetExitCodeThread fields are byte +4 and DWORD +0x120. These
 * tests cover the bounded mapped THREAD identity, not a complete ETHREAD.
 */
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_thread.h"
#include "nt_status.h"

#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
#define EQ(a,b) CHECK((uint32_t)(a) == (uint32_t)(b))
static uint32_t scratch;
static int quiet(const char *fmt, ...) { (void)fmt; return 0; }

static uint32_t call(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x100u, 0x100u, args, count));
    return kernel_hle_call(ordinal, &frame);
}
static uint32_t body_call(unsigned ordinal, uint32_t body)
{
    kernel_call_frame frame = {.ecx = body, .has_registers = true};
    return kernel_hle_call(ordinal, &frame);
}
static uint32_t read32(uint32_t address)
{
    uint32_t value = 0xDEADBEEFu;
    CHECK(kernel_guest_read_u32(address, &value));
    return value;
}
static void setup(void)
{
    kernel_object_reset();
    CHECK(kernel_thread_reset());
    CHECK(kernel_thread_set_host_ops(NULL));
    guest_mem_reset();
    kernel_hle_init();
    kernel_hle_set_log(quiet);
    (void)kernel_object_register();
    (void)kernel_thread_register();
    guest_region_request request = {.bytes = 0x4000u, .state = MEM_COMMIT,
                                   .protect = PAGE_READWRITE};
    nt_status status = 0u;
    scratch = guest_region_alloc(&request, &status);
    CHECK(scratch != 0u);
}
static uint32_t create(bool suspended)
{
    const uint32_t args[10] = {scratch + 0x300u, 0u, 0x4000u, 0u, 0u,
        0x003801D9u, 0u, suspended ? 1u : 0u, 0u, 0x0037FE1Du};
    EQ(call(255u, args, 10u), STATUS_SUCCESS);
    return read32(scratch + 0x300u);
}
static uint32_t reference(uint32_t handle, uint32_t output)
{
    const uint32_t args[3] = {handle, 0u, output};
    return call(246u, args, 3u);
}
static uint32_t exit_view(uint32_t body)
{
    uint8_t signaled = 0u;
    CHECK(kernel_guest_read_u8(body + 4u, &signaled));
    return signaled ? read32(body + 0x120u) : 0x103u;
}
static void test_identity_and_closed_retention(void)
{
    setup();
    uint32_t handle = create(true);
    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    EQ(reference(handle, scratch + 0x400u), STATUS_SUCCESS);
    uint32_t body = read32(scratch + 0x400u);
    EQ(body, record.control_base + KERNEL_THREAD_KTHREAD_OFFSET);
    CHECK(body != handle);
    EQ(exit_view(body), 0x103u);
    kernel_object_entry entry;
    CHECK(kernel_object_get_thread_body_copy(body, &entry));
    EQ(entry.thread_body_references, 1u);
    EQ(entry.references, 2u);
    const uint32_t priority[2] = {body, 7u};
    (void)call(143u, priority, 2u);
    const uint32_t boost[2] = {body, 1u};
    (void)call(144u, boost, 2u);
    CHECK(kernel_thread_get(handle, &record));
    CHECK(record.base_priority_set);
    EQ(record.base_priority_increment, 7u);
    CHECK(record.boost_disabled);
    uint32_t close_args[1] = {handle};
    EQ(call(187u, close_args, 1u), STATUS_SUCCESS);
    EQ(call(187u, close_args, 1u), STATUS_INVALID_HANDLE);
    CHECK(!kernel_object_get_copy(handle, &entry));
    EQ(reference(handle, scratch + 0x404u), STATUS_INVALID_HANDLE);
    CHECK(kernel_object_get_thread_body_copy(body, &entry));
    EQ(entry.references, 1u);
    CHECK(!kernel_thread_reset());
    EQ(exit_view(body), 0x103u);
    (void)body_call(251u, body);
    CHECK(kernel_object_get_thread_body_copy(body, &entry));
    EQ(entry.thread_body_references, 2u);
    (void)body_call(250u, body);
    (void)body_call(250u, body);
    CHECK(!kernel_object_get_thread_body_copy(body, &entry));
    (void)body_call(250u, body); /* Underflow and stale body never resurrect identity. */
    CHECK(kernel_thread_reset());
    CHECK(!kernel_object_get_copy(handle, &entry));
    CHECK(!kernel_guest_read_u32(body + 0x120u, &close_args[0]));
}
static void test_refusal_and_atomic_detach(void)
{
    setup();
    uint32_t handle = create(true);
    kernel_object_entry entry;
    CHECK(kernel_object_get_copy(handle, &entry));
    uint32_t body = entry.thread_body;
    uint32_t initial_signal = read32(body + 4u);
    CHECK(reference(handle, body + 4u) != STATUS_SUCCESS);
    EQ(read32(body + 4u), initial_signal);
    CHECK(kernel_object_get_copy(handle, &entry));
    EQ(entry.thread_body_references, 0u);
    (void)body_call(251u, handle); /* Handle is never a mapped object pointer. */
    (void)body_call(250u, handle);
    CHECK(kernel_object_get_copy(handle, &entry));
    EQ(entry.references, 1u);
    uint32_t control_page = body & ~0xFFFu;
    CHECK(kernel_guest_write_u32(scratch + 0x400u, 0xABCD1234u));
    CHECK(mprotect((void *)(uintptr_t)control_page, 0x1000u, PROT_NONE) == 0);
    CHECK(reference(handle, scratch + 0x400u) != STATUS_SUCCESS);
    EQ(read32(scratch + 0x400u), 0xABCD1234u);
    CHECK(kernel_object_get_copy(handle, &entry));
    EQ(entry.thread_body_references, 0u);
    CHECK(mprotect((void *)(uintptr_t)control_page, 0x1000u,
                   PROT_READ | PROT_WRITE) == 0);
    const uint32_t foreign_priority[2] = {body + 4u, 99u};
    (void)call(143u, foreign_priority, 2u);
    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    CHECK(!record.base_priority_set);
    CHECK(kernel_guest_write_u32(scratch + 0x1000u, 0x12345678u));
    CHECK(mprotect((void *)(uintptr_t)(scratch + 0x1000u), 0x1000u, PROT_READ) == 0);
    EQ(reference(handle, scratch + 0x1000u), STATUS_INVALID_PARAMETER);
    EQ(read32(scratch + 0x1000u), 0x12345678u);
    CHECK(kernel_object_get_copy(handle, &entry));
    EQ(entry.thread_body_references, 0u);
    CHECK(mprotect((void *)(uintptr_t)(scratch + 0x1000u), 0x1000u,
                   PROT_READ | PROT_WRITE) == 0);
    EQ(reference(handle, scratch + 0x400u), STATUS_SUCCESS);
    uint32_t second = create(true);
    CHECK(!kernel_object_detach_thread_bodies());
    CHECK(kernel_object_get_copy(second, &entry)); /* No partial detach. */
    CHECK(kernel_object_get_copy(handle, &entry));
    (void)body_call(250u, body);
    CHECK(kernel_object_detach_thread_bodies());
    CHECK(!kernel_object_get_copy(handle, &entry));
    CHECK(!kernel_object_get_copy(second, &entry));
    CHECK(kernel_thread_reset());
    EQ(reference(handle, scratch + 0x400u), STATUS_INVALID_HANDLE);
}

static _Thread_local jmp_buf terminate_jump;
static _Thread_local bool confirmed;
static unsigned mode;
static void terminate_hook(uint32_t status)
{
    (void)status;
    if (mode != 2u) longjmp(terminate_jump, 1);
}
static void enter_hook(const kernel_thread_launch *launch)
{
    confirmed = false;
    if (mode == 3u) return; /* Diagnostic host return, not guest termination. */
    if (setjmp(terminate_jump) == 0) {
        uint32_t args[1] = {0x87654321u};
        kernel_call_frame frame = {0};
        /* Worker uses its own guest stack; no shared CHECK counters. */
        if (!kernel_frame_build(&frame, launch->stack_low + 0x100u, 0x100u, args, 1u))
            return;
        (void)kernel_hle_call(258u, &frame);
    } else {
        confirmed = mode == 0u;
    }
}
static bool confirm_hook(const kernel_thread_launch *launch)
{
    (void)launch;
    return confirmed;
}
static void test_confirmed_termination_only(void)
{
    for (mode = 0u; mode < 4u; mode++) {
        setup();
        kernel_thread_host_ops ops = {.enter = enter_hook, .terminate = terminate_hook,
                                     .termination_confirmed = confirm_hook};
        CHECK(kernel_thread_set_host_ops(&ops));
        uint32_t handle = create(false);
        CHECK(kernel_thread_join_all(5000u) == 0u);
        EQ(reference(handle, scratch + 0x400u), STATUS_SUCCESS);
        uint32_t body = read32(scratch + 0x400u);
        EQ(exit_view(body), mode == 0u ? 0x87654321u : 0x103u);
        kernel_thread_record record;
        CHECK(kernel_thread_get(handle, &record));
        CHECK(record.finished);
        CHECK(record.terminated == (mode == 0u));
        (void)body_call(250u, body);
        CHECK(kernel_thread_reset());
    }
}

static void test_overflow_and_creation_rollback(void)
{
    setup();
    uint32_t handle = create(true);
    /* Deliberate quiescent synthetic boundary injection through the legacy
     * pointer API. Production callers must use the locked copy APIs. */
    kernel_object_entry *entry = (kernel_object_entry *)(uintptr_t)kernel_object_find(handle);
    CHECK(entry != NULL);
    if (!entry) return;
    uint32_t body = entry->thread_body;
    entry->thread_body_references = UINT32_MAX - 1u;
    entry->references = UINT32_MAX;
    CHECK(kernel_guest_write_u32(scratch + 0x400u, 0x76543210u));
    CHECK(reference(handle, scratch + 0x400u) != STATUS_SUCCESS);
    EQ(read32(scratch + 0x400u), 0x76543210u);
    (void)body_call(251u, body);
    EQ(entry->thread_body_references, UINT32_MAX - 1u);
    EQ(entry->references, UINT32_MAX);
    entry->thread_body_references = 0u;
    entry->references = 1u;
    CHECK(kernel_thread_reset());
    unsigned before = kernel_object_live_count();
    const uint32_t args[10] = {0x1000u, 0u, 0x4000u, 0u, 0u,
        0x003801D9u, 0u, 1u, 0u, 0x0037FE1Du};
    CHECK(call(255u, args, 10u) != STATUS_SUCCESS);
    EQ(kernel_object_live_count(), before);
    CHECK(kernel_thread_reset());
}

struct race_state {
    pthread_barrier_t barrier;
    kernel_call_frame frame;
    uint32_t status;
    bool detached;
};
static void *race_reference(void *opaque)
{
    struct race_state *state = opaque;
    (void)pthread_barrier_wait(&state->barrier);
    state->status = kernel_hle_call(246u, &state->frame);
    return NULL;
}
static void *race_detach(void *opaque)
{
    struct race_state *state = opaque;
    (void)pthread_barrier_wait(&state->barrier);
    state->detached = kernel_object_detach_thread_bodies();
    return NULL;
}
static void test_reference_detach_race(void)
{
    setup();
    for (unsigned i = 0u; i < 100u; i++) {
        kernel_object_reset();
        uint32_t handle = kernel_object_create(KERNEL_OBJECT_THREAD, 42u);
        uint32_t body = scratch + 0x2000u;
        CHECK(kernel_object_bind_thread_body(handle, 42u, body));
        struct race_state state = {0};
        const uint32_t args[3] = {handle, 0u, scratch + 0x400u};
        CHECK(kernel_frame_build(&state.frame, scratch + 0x100u, 0x100u, args, 3u));
        CHECK(pthread_barrier_init(&state.barrier, NULL, 2u) == 0);
        pthread_t first, second;
        int first_rc = pthread_create(&first, NULL, race_reference, &state);
        int second_rc = pthread_create(&second, NULL, race_detach, &state);
        /* No shared test-counter accesses overlap either worker. */
        if (first_rc != 0 || second_rc != 0) return;
        int first_join = pthread_join(first, NULL);
        int second_join = pthread_join(second, NULL);
        CHECK(first_join == 0 && second_join == 0);
        CHECK(pthread_barrier_destroy(&state.barrier) == 0);
        CHECK((state.status == STATUS_SUCCESS && !state.detached) ||
              (state.status == STATUS_INVALID_HANDLE && state.detached));
        if (state.status == STATUS_SUCCESS) {
            EQ(read32(scratch + 0x400u), body);
            (void)body_call(250u, body);
            CHECK(kernel_object_detach_thread_bodies());
        }
    }
}

static void test_optional_thread_id_cannot_corrupt_another_body(void)
{
    setup();
    uint32_t first = create(true);
    kernel_object_entry before, after;
    CHECK(kernel_object_get_copy(first, &before));
    unsigned char bytes_before[0x124u], bytes_after[0x124u];
    CHECK(kernel_guest_read_bytes(before.thread_body, bytes_before, sizeof(bytes_before)));
    const uint32_t args[10] = {scratch + 0x304u, 0u, 0x4000u, 0u,
        before.thread_body + 4u, 0x003801D9u, 0u, 1u, 0u, 0x0037FE1Du};
    EQ(call(255u, args, 10u), STATUS_SUCCESS);
    uint32_t second = read32(scratch + 0x304u);
    CHECK(second != first);
    CHECK(kernel_object_get_copy(second, &after));
    CHECK(kernel_object_get_copy(first, &after));
    EQ(after.references, before.references);
    EQ(after.thread_body_references, before.thread_body_references);
    CHECK(kernel_guest_read_bytes(before.thread_body, bytes_after, sizeof(bytes_after)));
    CHECK(memcmp(bytes_before, bytes_after, sizeof(bytes_before)) == 0);
    CHECK(kernel_thread_reset());
}

int main(void)
{
    test_identity_and_closed_retention();
    test_refusal_and_atomic_detach();
    test_confirmed_termination_only();
    test_overflow_and_creation_rollback();
    test_reference_detach_race();
    test_optional_thread_id_cannot_corrupt_another_body();
    kernel_object_reset();
    CHECK(kernel_thread_reset());
    guest_mem_reset();
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
