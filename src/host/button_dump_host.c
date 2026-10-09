/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1629: wiring of the passive button dump, see button_dump_host.h.
 */
#include "button_dump_host.h"

#include "button_dump.h"
#include "xinput_source.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static button_dump *g_dump;
static guest_dump_set g_set;
static uint64_t (*g_present)(void);
static bool (*g_armed)(void);
static bool g_stop_started; /* a bounded stop was begun (it may still be running after a timeout) */

static void *host_capture(void *user)
{
    (void)user;
    return guest_dump_capture(&g_set);
}

static bool host_write(void *user, void *snapshot, const char *path, const char *header)
{
    (void)user;
    return guest_dump_snapshot_write(snapshot, path, header);
}

static void host_discard(void *user, void *snapshot)
{
    (void)user;
    guest_dump_snapshot_free(snapshot);
}

static uint64_t host_present(void *user)
{
    (void)user;
    return g_present != NULL ? g_present() : 0u;
}

static bool host_armed(void *user)
{
    (void)user;
    return g_armed != NULL && g_armed();
}

/* Runs inside the xinput device lock on the guest's pad poll: the core only compares masks, captures snapshots and queues. */
static void on_pre_install(uint64_t poll, const xinput_pad_state *state, void *user)
{
    (void)user;
    button_dump_poll(g_dump, poll, state);
}

bool button_dump_host_start(const button_dump_host_config *config, char *error, size_t error_size)
{
    if (g_dump != NULL || config == NULL || config->set == NULL || config->set->count == 0u || config->dir == NULL ||
        (config->after_replay && config->armed == NULL)) {
        snprintf(error, error_size, "button dump: bad configuration or already started");
        return false;
    }
    g_set = *config->set;
    g_present = config->present;
    g_armed = config->armed;
    /* The snapshot memory is fixed by the set (capacity, not readability), so measure it once. */
    guest_dump_snapshot *probe = guest_dump_capture(&g_set);
    const uint64_t snapshot_bytes = guest_dump_snapshot_bytes(probe);
    guest_dump_snapshot_free(probe);
    if (snapshot_bytes == 0u) {
        snprintf(error, error_size, "button dump: cannot size a snapshot");
        return false;
    }
    button_dump_config core;
    memset(&core, 0, sizeof core);
    core.after_count = config->after_count;
    memcpy(core.after, config->after, sizeof core.after);
    core.threshold = config->threshold;
    core.coalesce = config->coalesce;
    core.max_pending = config->max_pending;
    core.max_dumps = config->max_dumps;
    core.max_bytes = config->max_bytes;
    core.start_poll = config->start_poll;
    core.after_replay = config->after_replay;
    core.forced_state = config->forced_state;
    core.idle_every = config->idle_every;
    core.max_idle = config->max_idle;
    core.bytes_per_dump = guest_dump_estimate_text_bytes(&g_set) + 200u; /* + the button-dump header line */
    core.snapshot_bytes = snapshot_bytes;
    core.ranges = g_set.count;
    core.dump_dir = config->dir;
    button_dump_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.capture = host_capture;
    ops.write = host_write;
    ops.discard = host_discard;
    ops.present = host_present;
    ops.armed = host_armed;
    g_stop_started = false;
    g_dump = button_dump_create(&core, &ops, error, error_size);
    if (g_dump == NULL) {
        return false;
    }
    xinput_source_set_pre_install_hook(on_pre_install, NULL);
    return true;
}

void button_dump_host_stop(const char *reason)
{
    if (g_dump == NULL) {
        return;
    }
    /* once this returns no hook call is in flight (the setter is serialized with the poll) */
    xinput_source_set_pre_install_hook(NULL, NULL);
    button_dump_finish(g_dump, reason);
    button_dump_destroy(g_dump);
    g_dump = NULL;
}

/* The bounded form for the exit paths that leave guest threads running: the hook removal takes the device lock, which a wedged
 * guest thread could in principle hold, and the final flush writes files. Both run in a helper thread that this waits for at most
 * `timeout_ms` in total, then gives up (the process is about to _exit). Giving up before the hook is removed skips the flush: closing
 * the dump while a guest thread may still call into it would race. */
typedef struct {
    const char *reason;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool hook_removed;
    bool finished;
} stop_job;

static void *stop_thread(void *argument)
{
    stop_job *job = argument;
    xinput_source_set_pre_install_hook(NULL, NULL);
    pthread_mutex_lock(&job->lock);
    job->hook_removed = true;
    pthread_cond_broadcast(&job->changed);
    pthread_mutex_unlock(&job->lock);
    button_dump_finish(g_dump, job->reason);
    button_dump_destroy(g_dump);
    g_dump = NULL;
    pthread_mutex_lock(&job->lock);
    job->finished = true;
    pthread_cond_broadcast(&job->changed);
    pthread_mutex_unlock(&job->lock);
    return NULL;
}

bool button_dump_host_stop_bounded(const char *reason, unsigned timeout_ms)
{
    if (g_dump == NULL) {
        return true;
    }
    if (g_stop_started) {
        return false; /* an earlier call timed out and its helper is still at it: never two closers */
    }
    g_stop_started = true;
    /* the job outlives this call when it times out, so it is never freed (one small leak at process exit) */
    stop_job *job = calloc(1u, sizeof *job);
    pthread_t thread;
    if (job == NULL) {
        return false;
    }
    job->reason = reason;
    pthread_mutex_init(&job->lock, NULL);
    pthread_cond_init(&job->changed, NULL);
    if (pthread_create(&thread, NULL, stop_thread, job) != 0) {
        return false;
    }
    pthread_detach(thread);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000u);
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&job->lock);
    while (!job->finished) {
        if (pthread_cond_timedwait(&job->changed, &job->lock, &deadline) != 0) {
            break;
        }
    }
    const bool done = job->finished;
    const bool removed = job->hook_removed;
    pthread_mutex_unlock(&job->lock);
    if (!done) {
        fprintf(stderr, "button dump: stop did not finish within %u ms (hook %s), giving up\n", timeout_ms,
                removed ? "removed, flush still running" : "NOT removed, dump left open");
    }
    return done;
}
