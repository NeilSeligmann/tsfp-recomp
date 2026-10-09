/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1246: the guest entries that read or rewrite a visibility report slot wait for the report a queued (pipelined) frame still owes
 * that slot. The "presenter" here is a thread that writes the report late, through the production d3d8_visibility_complete_identity,
 * and settles it in the production tracker; the guest side is the production d3d8_visibility_result and d3d8_visibility_slot. */
#define _POSIX_C_SOURCE 200809L
#include "test_d3d8_support.h"
#include "d3d8_visibility.h"
#include "d3d8_gpu.h"
#include "live_report_pending.h"

#include <pthread.h>
#include <time.h>

#define OUTPUT 0xD00000u
#define DELAY_MS 80

static live_report_pending *tracker;

static void hook(uint32_t slot_address)
{
    (void)live_report_pending_wait(tracker, slot_address);
}

typedef struct {
    uint32_t physical, slot;
    uint64_t generation;
    uint32_t samples;
} late_report;

static void sleep_ms(long ms)
{
    struct timespec span = {0, ms * 1000000L};
    nanosleep(&span, NULL);
}

/* The presenter thread: writes the report DELAY_MS after the Swap returned, then settles it. */
static void *late_writer(void *argument)
{
    const late_report *report = argument;
    sleep_ms(DELAY_MS);
    (void)d3d8_visibility_complete_identity(report->physical, report->slot, report->generation, report->samples, UINT64_C(0x0000000500000006));
    live_report_pending_done(tracker, report->slot, 1u);
    return NULL;
}

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

int main(void)
{
    /* the tracker alone */
    tracker = live_report_pending_create(2u);
    CHECK(tracker != NULL);
    CHECK(live_report_pending_add(tracker, 0x1000u, 0u));
    CHECK(live_report_pending_add(tracker, 0x1010u, 0u));
    CHECK(!live_report_pending_add(tracker, 0x1020u, 1u)); /* full: the frame must be drawn serially */
    CHECK(live_report_pending_wait(tracker, 0x2000u) == 0u); /* an unrelated slot never waits */
    live_report_pending_done(tracker, 0x1000u, 1u);        /* another package's settle does not release it */
    CHECK(live_report_pending_count(tracker) == 2u);
    live_report_pending_done(tracker, 0u, 0u);              /* the frame ended: all its reports settle */
    CHECK(live_report_pending_count(tracker) == 0u);
    CHECK(live_report_pending_wait(tracker, 0x1000u) == 0u);
    live_report_pending_destroy(tracker);

    /* through the production visibility entries */
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(OUTPUT, 4096u);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_gpu_create());
    CHECK(d3d8_pushbuffer_create());
    tracker = live_report_pending_create(16u);
    d3d8_visibility_set_drain_hook(hook);
    const uint32_t index = 3u;
    CHECK_EQ_U32(d3d8_visibility_end(index), 0u);
    const uint32_t page = load(D3D8_DEVICE_BASE + 0x7D4u);
    const uint32_t slot = page + (index & 255u) * 16u;
    late_report report = {guest_physical_address(slot), slot, guest_allocation_generation(slot), 15000u};
    CHECK(report.physical != 0u && report.generation != 0u);

    /* frame N was queued with a report owed to `slot`: the guest asks for the result before the presenter wrote it */
    CHECK(live_report_pending_add(tracker, slot, 1u));
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, late_writer, &report) == 0);
    store(OUTPUT, 0xDEADBEEFu);
    const uint64_t start = now_ms();
    const uint32_t status = d3d8_visibility_result(index, OUTPUT, 0u);
    const uint64_t waited = now_ms() - start;
    CHECK_EQ_U32(status, 0u);                 /* the report, not "not ready": the serial renderer had written it at the Swap */
    CHECK_EQ_U32(load(OUTPUT), 15000u);       /* with its samples */
    CHECK(waited >= DELAY_MS - 20);           /* and the guest really waited for the late writer */
    pthread_join(thread, NULL);

    /* rewriting the slot (a new query on it) also waits for an owed report, so the late completion cannot land on the new query */
    store(slot + 12u, 0u);
    CHECK(live_report_pending_add(tracker, slot, 1u));
    CHECK(pthread_create(&thread, NULL, late_writer, &report) == 0);
    const uint64_t start_slot = now_ms();
    CHECK_EQ_U32(d3d8_visibility_slot(index), slot);
    CHECK(now_ms() - start_slot >= DELAY_MS - 20);
    CHECK_EQ_U32(load(slot + 12u), UINT32_MAX); /* the new query's pending mark came AFTER the late completion, so it stands */
    pthread_join(thread, NULL);

    /* a slot nothing owes is not delayed */
    const uint64_t start_free = now_ms();
    CHECK_EQ_U32(d3d8_visibility_result(index, OUTPUT, 0u), 0x88760828u);
    CHECK(now_ms() - start_free < DELAY_MS / 2);

    d3d8_visibility_set_drain_hook(NULL);
    live_report_pending_destroy(tracker);
    environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
