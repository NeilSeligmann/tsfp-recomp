/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1246: present_video_sink_post, the queue the pipelined live renderer draws a frame through. On SDL's dummy video driver (no display).
 * A posted job returns at once, runs on the presenter thread, and every later synchronous call (the vblank, the front observer, a
 * report-slot drain) runs after it: any synchronous run is a drain of the queue, so jobs execute in the order they were issued.
 */
#define _POSIX_C_SOURCE 200809L
#include "present_sink.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int failures;
static int checks;
#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void sleep_ms(long ms)
{
    struct timespec span = {0, ms * 1000000L};
    nanosleep(&span, NULL);
}

static int order[16];
static int order_count;
static pthread_t job_thread;
static pthread_t main_thread;
static bool ran_off_main;

static void slow_job(void *context)
{
    sleep_ms(60);
    order[order_count++] = (int)(intptr_t)context;
    job_thread = pthread_self();
    ran_off_main = !pthread_equal(job_thread, main_thread);
}

static void quick_job(void *context)
{
    order[order_count++] = (int)(intptr_t)context;
}

int main(void)
{
    if (!present_window_available()) {
        printf("SKIPPED present_post: this build has no SDL3\n");
        return 77;
    }
    const char *error = NULL;
    present_video_sink *sink = present_video_sink_open(PRESENT_VIDEO_WINDOW, "tsfp post test", &error);
    if (sink == NULL) {
        printf("FAIL window could not open on the dummy driver: %s\n", error != NULL ? error : "(no text)");
        return 1;
    }
    main_thread = pthread_self();

    /* a posted job returns before it ran, runs on the presenter thread, and a drain waits for it */
    int64_t start = now_ms();
    CHECK(present_video_sink_post(sink, slow_job, (void *)1));
    CHECK(now_ms() - start < 40);
    CHECK(order_count == 0);
    present_video_sink_drain(sink);
    CHECK(order_count == 1 && order[0] == 1 && ran_off_main);

    /* a second post waits for the first (one in flight), jobs run in issue order */
    order_count = 0;
    CHECK(present_video_sink_post(sink, slow_job, (void *)1));
    CHECK(present_video_sink_post(sink, quick_job, (void *)2));
    CHECK(present_video_sink_post(sink, quick_job, (void *)3));
    present_video_sink_drain(sink);
    CHECK(order_count == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3);

    /* a synchronous run issued after a post runs after it (the vblank and observer of the next Swap see the frame drawn) */
    order_count = 0;
    CHECK(present_video_sink_post(sink, slow_job, (void *)1));
    CHECK(present_video_sink_run(sink, quick_job, (void *)2));
    CHECK(order_count == 2 && order[0] == 1 && order[1] == 2);

    /* the wait is accounted: the synchronous run above waited for the 60 ms job */
    uint64_t calls = 0u;
    double total_ms = 0.0, worst_ms = 0.0;
    present_video_sink_blocked(sink, 0u, &calls, &total_ms, &worst_ms);
    CHECK(calls >= 1u && worst_ms >= 30.0);

    /* T1262: the same wait is split by job kind: the 60 ms job in flight is waiting time (the job slot) of the observer, not its execution */
    order_count = 0;
    CHECK(present_video_sink_post(sink, slow_job, (void *)1));
    CHECK(present_video_sink_run_kind(sink, PRESENT_JOB_OBSERVER, quick_job, (void *)2));
    CHECK(present_video_sink_run_kind(sink, PRESENT_JOB_SERIAL_FRAME, slow_job, (void *)3));
    present_job_kind_stats observer, serial, vblank;
    present_video_sink_job_kind_stats(sink, PRESENT_JOB_OBSERVER, &observer);
    present_video_sink_job_kind_stats(sink, PRESENT_JOB_SERIAL_FRAME, &serial);
    present_video_sink_job_kind_stats(sink, PRESENT_JOB_VBLANK, &vblank);
    CHECK(observer.calls == 1u && observer.wait_ms >= 30.0 && observer.exec_ms < 30.0);
    CHECK(serial.calls == 1u && serial.wait_ms < 30.0 && serial.exec_ms >= 30.0);
    CHECK(vblank.calls == 0u);
    CHECK(order_count == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3);

    /* nothing queued: a drain returns at once */
    start = now_ms();
    present_video_sink_drain(sink);
    CHECK(now_ms() - start < 30);

    present_video_sink_close(sink);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
