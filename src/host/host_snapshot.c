/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "host_snapshot.h"
#include "xinput_source.h"
#include <dirent.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The libdmtcp.so symbol, weak so a host started without dmtcp_launch still links and runs. */
extern int dmtcp_checkpoint(void) __attribute__((weak));

static host_snapshot_checkpoint_fn g_checkpoint;
static _Atomic uint64_t g_target;
static _Atomic uint64_t g_taken_at;
static _Atomic int g_fired;
static _Atomic uint64_t g_stop_target;
static host_snapshot_stop_fn g_stop_function;
static _Atomic int g_stop_fired;

static int default_checkpoint(void)
{
    return dmtcp_checkpoint != NULL ? dmtcp_checkpoint() : -1;
}

void host_snapshot_set_checkpoint_function(host_snapshot_checkpoint_fn function)
{
    g_checkpoint = function;
}

bool host_snapshot_find_device_fd(char *path, size_t path_size)
{
    static const char *const prefixes[] = {"/dev/dri/", "/dev/nvidia", "/dev/kfd", "/dev/accel"};
    DIR *directory = opendir("/proc/self/fd");
    if (directory == NULL) return false;
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(directory)) != NULL) {
        char link[64], target[PATH_MAX];
        snprintf(link, sizeof link, "/proc/self/fd/%s", entry->d_name);
        const ssize_t length = readlink(link, target, sizeof target - 1u);
        if (length <= 0) continue;
        target[length] = '\0';
        for (size_t i = 0u; i < sizeof prefixes / sizeof prefixes[0]; i++) {
            if (strncmp(target, prefixes[i], strlen(prefixes[i])) == 0) {
                snprintf(path, path_size, "%s", target);
                found = true;
                break;
            }
        }
    }
    closedir(directory);
    return found;
}

int host_snapshot_write_mapped_files(const char *out_path)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    FILE *out = fopen(out_path, "w");
    if (maps == NULL || out == NULL) {
        if (maps != NULL) fclose(maps);
        if (out != NULL) fclose(out);
        return -1;
    }
    char (*seen)[PATH_MAX] = calloc(4096u, sizeof *seen);
    size_t seen_count = 0u;
    char line[PATH_MAX + 128];
    int written = 0;
    while (seen != NULL && fgets(line, sizeof line, maps) != NULL) {
        char *path = strchr(line, '/');
        if (path == NULL || strstr(line, "(deleted)") != NULL || strncmp(path, "/memfd:", 7) == 0 ||
            strncmp(path, "/dev/", 5) == 0 || strncmp(path, "/proc/", 6) == 0) continue;
        path[strcspn(path, "\n")] = '\0';
        bool known = false;
        for (size_t i = 0u; i < seen_count && !known; i++) known = strcmp(seen[i], path) == 0;
        if (known || seen_count >= 4096u) continue;
        snprintf(seen[seen_count++], PATH_MAX, "%s", path);
        fprintf(out, "%s\n", path);
        written++;
    }
    free(seen);
    fclose(maps);
    fclose(out);
    return written;
}

static void fail_loudly(const char *why)
{
    fprintf(stderr, "snapshot: REFUSED: %s\n", why);
    fflush(stderr);
    _exit(97);
}

static void take_snapshot(uint64_t polls)
{
    char device[PATH_MAX];
    if (host_snapshot_find_device_fd(device, sizeof device)) {
        char why[PATH_MAX + 200];
        snprintf(why, sizeof why,
                 "the process holds the device file %s, which a process snapshot cannot carry. Run with the "
                 "software Vulkan device (lavapipe) so no GPU file is open (docs/state-snapshot.md, GPU row)",
                 device);
        fail_loudly(why);
    }
    const char *directory = getenv("TSFP_SNAPSHOT_DIR");
    if (directory != NULL && directory[0] != '\0') {
        char list[PATH_MAX];
        snprintf(list, sizeof list, "%s/mapped-files.txt", directory);
        if (host_snapshot_write_mapped_files(list) < 0) fail_loudly("cannot write the mapped-file list");
    }
    host_snapshot_checkpoint_fn function = g_checkpoint != NULL ? g_checkpoint : default_checkpoint;
    fprintf(stderr, "snapshot: taking the snapshot at poll %llu of port 0\n", (unsigned long long)polls);
    fflush(stderr);
    const int result = function();
    atomic_store(&g_taken_at, polls);
    if (result == HOST_SNAPSHOT_AFTER_CHECKPOINT) {
        fprintf(stderr, "snapshot: checkpoint complete at poll %llu, this run continues\n", (unsigned long long)polls);
    } else if (result == HOST_SNAPSHOT_AFTER_RESTART) {
        fprintf(stderr, "snapshot: RESUMED from the snapshot taken at poll %llu\n", (unsigned long long)polls);
    } else {
        fail_loudly("the DMTCP layer is not loaded (start the run with python -m tools.snapshot or "
                    "tools.play --snapshot-at-poll) or the checkpoint failed");
    }
    fflush(stderr);
}

static void on_poll(unsigned port, uint64_t port_polls, void *user)
{
    (void)user;
    if (port != 0u) return;
    const uint64_t target = atomic_load(&g_target);
    int expected = 0;
    if (target != 0u && port_polls == target && atomic_compare_exchange_strong(&g_fired, &expected, 1))
        take_snapshot(port_polls);
    const uint64_t stop_target = atomic_load(&g_stop_target);
    expected = 0;
    if (stop_target != 0u && port_polls == stop_target && g_stop_function != NULL &&
        atomic_compare_exchange_strong(&g_stop_fired, &expected, 1))
        g_stop_function(port_polls);
}

bool host_snapshot_arm(uint64_t at_poll, char *error, size_t error_size)
{
    if (at_poll == 0u) {
        if (error != NULL && error_size != 0u) snprintf(error, error_size, "--snapshot-at-poll needs a poll index of at least 1");
        return false;
    }
    atomic_store(&g_target, at_poll);
    atomic_store(&g_fired, 0);
    xinput_source_set_poll_observer(on_poll, NULL);
    return true;
}

bool host_snapshot_arm_stop(uint64_t at_poll, host_snapshot_stop_fn stop, char *error, size_t error_size)
{
    if (at_poll == 0u || stop == NULL) {
        if (error != NULL && error_size != 0u) snprintf(error, error_size, "--stop-at-poll needs a poll index of at least 1");
        return false;
    }
    g_stop_function = stop;
    atomic_store(&g_stop_fired, 0);
    atomic_store(&g_stop_target, at_poll);
    xinput_source_set_poll_observer(on_poll, NULL);
    return true;
}

uint64_t host_snapshot_taken_at(void)
{
    return atomic_load(&g_taken_at);
}

void host_snapshot_reset(void)
{
    atomic_store(&g_target, 0u);
    atomic_store(&g_taken_at, 0u);
    atomic_store(&g_fired, 0);
    atomic_store(&g_stop_target, 0u);
    atomic_store(&g_stop_fired, 0);
    g_stop_function = NULL;
    g_checkpoint = NULL;
    xinput_source_set_poll_observer(NULL, NULL);
}
