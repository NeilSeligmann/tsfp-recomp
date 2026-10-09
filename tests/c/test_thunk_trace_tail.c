/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1492: the END of the call trace. The ordered trace keeps the first 2048 calls, so a run of 358
 * million calls cannot say what its last calls were. The per-thread tail and the recent
 * non-success status ring can, and the stop report prints them.
 */

#include "thunk_trace.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* the thunk library reads the guest registers, this suite defines them */
#include "recomp_abi.h"
TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static void *other_thread(void *unused)
{
    (void)unused;
    (void)thunk_trace_append(THUNK_KIND_ORDINAL, 99u, 0u, 0x77u, 0xC0000001u, true);
    return NULL;
}

int main(void)
{
    thunk_trace_reset();
    CHECK(thunk_trace_status_is_failure(0xC0000034u));
    CHECK(thunk_trace_status_is_failure(0x80000005u));
    CHECK(!thunk_trace_status_is_failure(0u));
    CHECK(!thunk_trace_status_is_failure(0x103u)); /* STATUS_PENDING is not a failure */
    CHECK(!thunk_trace_status_is_failure(0x40000000u));
    CHECK(!thunk_trace_status_is_failure(0x41C56800u)); /* a guest pointer */

    /* far past THUNK_TRACE_MAX: the tail is the END, not the first calls */
    for (unsigned i = 0u; i < THUNK_TRACE_MAX + 100u; i++) {
        (void)thunk_trace_append(THUNK_KIND_XDK, 0u, 0x1000u + i, 0x2000u + i, 0u, true);
    }
    thunk_trace_entry tail[THUNK_TAIL_MAX];
    unsigned count = thunk_trace_thread_tail(tail, THUNK_TAIL_MAX);
    CHECK(count == THUNK_TAIL_MAX);
    CHECK(tail[THUNK_TAIL_MAX - 1u].address == 0x1000u + THUNK_TRACE_MAX + 99u);
    CHECK(tail[0].address == 0x1000u + THUNK_TRACE_MAX + 100u - THUNK_TAIL_MAX);
    CHECK(thunk_trace_thread_tail(tail, 3u) == 3u);
    CHECK(tail[2].address == 0x1000u + THUNK_TRACE_MAX + 99u);

    /* a pending call is patched even when the first-2048 trace has no slot for it */
    const size_t slot = thunk_trace_append_pending(THUNK_KIND_ORDINAL, 190u, 0u, 0x3000u, true);
    CHECK(slot == THUNK_TRACE_NO_SLOT);
    count = thunk_trace_thread_tail(tail, THUNK_TAIL_MAX);
    CHECK(!tail[count - 1u].result_known);
    thunk_trace_patch_result(slot, 0xC0000034u);
    count = thunk_trace_thread_tail(tail, THUNK_TAIL_MAX);
    CHECK(tail[count - 1u].result_known && tail[count - 1u].result == 0xC0000034u);

    /* nested calls complete LIFO: the inner return patches the inner call */
    const size_t outer = thunk_trace_append_pending(THUNK_KIND_ORDINAL, 1u, 0u, 0u, true);
    const size_t inner = thunk_trace_append_pending(THUNK_KIND_ORDINAL, 2u, 0u, 0u, true);
    thunk_trace_patch_result(inner, 0u);
    thunk_trace_patch_result(outer, 0xC0000005u);
    count = thunk_trace_thread_tail(tail, 2u);
    CHECK(tail[0].ordinal == 1u && tail[0].result == 0xC0000005u);
    CHECK(tail[1].ordinal == 2u && tail[1].result == 0u);

    /* the failure ring: ordinals only, any thread, XDK eax is not a status */
    (void)thunk_trace_append(THUNK_KIND_XDK, 0u, 0x5000u, 0u, 0xC0000099u, true);
    pthread_t thread;
    pthread_create(&thread, NULL, other_thread, NULL);
    pthread_join(thread, NULL);
    thunk_trace_entry bad[THUNK_FAILURE_MAX];
    uint64_t total = 0u;
    unsigned kept = thunk_trace_recent_failures(bad, THUNK_FAILURE_MAX, &total);
    CHECK(total == 3u && kept == 3u);
    CHECK(bad[0].ordinal == 190u && bad[0].result == 0xC0000034u);
    CHECK(bad[1].ordinal == 1u && bad[2].ordinal == 99u && bad[2].return_address == 0x77u);

    /* the ring keeps the LAST ones and the total keeps counting */
    for (unsigned i = 0u; i < 50u; i++) {
        (void)thunk_trace_append(THUNK_KIND_ORDINAL, 500u + i, 0u, 0u, 0xC0000000u + i, true);
    }
    kept = thunk_trace_recent_failures(bad, THUNK_FAILURE_MAX, &total);
    CHECK(total == 53u && kept == THUNK_FAILURE_MAX);
    CHECK(bad[THUNK_FAILURE_MAX - 1u].ordinal == 549u && bad[0].ordinal == 530u);

    if (failures == 0) {
        printf("thunk trace tail: ok\n");
    }
    return failures == 0 ? 0 : 1;
}
