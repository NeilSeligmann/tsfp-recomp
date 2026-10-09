/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1153: DMTCP plugin that copies the run directory while every process of the computation is quiesced.
 *
 * A DMTCP image holds memory and the open file descriptors (path and offset), not the files. The title's HDD
 * (files it created through NtCreateFile) lives under the run directory, so the files must be captured at the
 * same instant as the memory: after all user threads of all processes are suspended (global barrier) and
 * before anything resumes. Resume restores this copy over the run directory first (tools/snapshot).
 *
 * Environment (set by tools.snapshot): TSFP_SNAPSHOT_RUN_DIR (absolute path of the directory to copy) and
 * TSFP_SNAPSHOT_DIR (the snapshot directory, the copy goes to <dir>/run-dir). Without both the plugin is inert.
 * Loaded with `dmtcp_launch --with-plugin`. Build: tools/snapshot/dmtcp.py build. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dmtcp.h"

static char g_source[PATH_MAX];
static char g_destination[PATH_MAX];
static size_t g_source_length;

static void log_line(const char *text)
{
    const char *directory = getenv("TSFP_SNAPSHOT_DIR");
    char path[PATH_MAX];
    if (directory == NULL) return;
    snprintf(path, sizeof path, "%s/plugin.log", directory);
    const int descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (descriptor < 0) return;
    (void)!write(descriptor, text, strlen(text));
    close(descriptor);
}

static int remove_entry(const char *path, const struct stat *status, int type, struct FTW *walk)
{
    (void)status; (void)type; (void)walk;
    return remove(path);
}

static int copy_file(const char *from, const char *to, mode_t mode)
{
    const int source = open(from, O_RDONLY);
    if (source < 0) return -1;
    const int target = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode & 07777);
    if (target < 0) { close(source); return -1; }
    char buffer[1 << 16];
    ssize_t count;
    int result = 0;
    while ((count = read(source, buffer, sizeof buffer)) > 0) {
        for (ssize_t done = 0; done < count;) {
            const ssize_t put = write(target, buffer + done, (size_t)(count - done));
            if (put < 0) { result = -1; break; }
            done += put;
        }
        if (result != 0) break;
    }
    if (count < 0) result = -1;
    close(source);
    close(target);
    return result;
}

static int copy_entry(const char *path, const struct stat *status, int type, struct FTW *walk)
{
    (void)walk;
    char target[PATH_MAX];
    snprintf(target, sizeof target, "%s%s", g_destination, path + g_source_length);
    if (type == FTW_D) {
        if (mkdir(target, status->st_mode & 07777) != 0 && errno != EEXIST) return -1;
        return 0;
    }
    if (type == FTW_SL) {
        char link[PATH_MAX];
        const ssize_t length = readlink(path, link, sizeof link - 1u);
        if (length < 0) return -1;
        link[length] = '\0';
        return symlink(link, target) == 0 || errno == EEXIST ? 0 : -1;
    }
    if (type == FTW_F) return copy_file(path, target, status->st_mode);
    return 0;
}

static void copy_run_directory(void)
{
    const char *source = getenv("TSFP_SNAPSHOT_RUN_DIR");
    const char *directory = getenv("TSFP_SNAPSHOT_DIR");
    if (source == NULL || directory == NULL) return;
    snprintf(g_source, sizeof g_source, "%s", source);
    g_source_length = strlen(g_source);
    snprintf(g_destination, sizeof g_destination, "%s/run-dir", directory);
    /* every process runs this hook after the barrier, the first to create the lock does the copy */
    char lock[PATH_MAX];
    snprintf(lock, sizeof lock, "%s/run-dir.lock", directory);
    if (mkdir(lock, 0755) != 0) return;
    (void)nftw(g_destination, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    if (mkdir(g_destination, 0755) != 0 && errno != EEXIST) { log_line("run-dir: cannot create destination\n"); return; }
    if (nftw(g_source, copy_entry, 16, FTW_PHYS) != 0) log_line("run-dir: COPY FAILED\n");
    else log_line("run-dir: copied\n");
    rmdir(lock);
}

void dmtcp_event_hook(DmtcpEvent_t event, DmtcpEventData_t *data)
{
    (void)data;
    if (event == DMTCP_EVENT_PRECHECKPOINT) {
        dmtcp_global_barrier("tsfp-snapshot-all-suspended");
        copy_run_directory();
        dmtcp_global_barrier("tsfp-snapshot-run-dir-copied");
    }
}

DmtcpPluginDescriptor_t tsfp_snapshot_plugin = {
    DMTCP_PLUGIN_API_VERSION, DMTCP_PACKAGE_VERSION, "tsfp-snapshot", "tsfp-recomp T1153",
    "T1153", "copies the run directory at the checkpoint instant", dmtcp_event_hook};

DMTCP_DECL_PLUGIN(tsfp_snapshot_plugin);
