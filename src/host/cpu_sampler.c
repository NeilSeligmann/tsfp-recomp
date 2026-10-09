/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE 1
#include "cpu_sampler.h"

#include <dirent.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#define SAMPLER_CAPACITY (1u << 20)
#define SAMPLER_DEPTH 8u

typedef struct {
    int32_t tid;
    uint32_t depth;
    uint64_t clock_ms; /* T1250 gaps: CLOCK_MONOTONIC ms, written to the FILE.ms sidecar (the clock of the call profile slow log) */
    uint64_t rip[SAMPLER_DEPTH];
} sampler_entry;

static sampler_entry g_samples[SAMPLER_CAPACITY];
static atomic_uint g_count;
static bool g_running;
static bool g_wall_mode;
static char g_path[512];

static uint64_t sample_clock_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void on_profile_signal(int signal_number, siginfo_t *info, void *context)
{
    (void)signal_number;
    (void)info;
    const ucontext_t *ucontext = context;
    const unsigned slot = atomic_fetch_add(&g_count, 1u);
    if (slot >= SAMPLER_CAPACITY) {
        return;
    }
    g_samples[slot].tid = (int32_t)syscall(SYS_gettid);
    g_samples[slot].clock_ms = sample_clock_ms();
    g_samples[slot].depth = 1u;
#if defined(__x86_64__)
    g_samples[slot].rip[0] = (uint64_t)ucontext->uc_mcontext.gregs[REG_RIP];
#else
    (void)ucontext;
    g_samples[slot].rip[0] = 0u;
#endif
}

/* Wall mode: a sampler thread signals every thread, the handler stores the call chain (glibc backtrace, .eh_frame
 * unwinding) so a thread blocked in a wait is attributed to the function that waits. */
static void on_wall_signal(int signal_number, siginfo_t *info, void *context)
{
    (void)signal_number;
    (void)info;
    (void)context;
    const unsigned slot = atomic_fetch_add(&g_count, 1u);
    if (slot >= SAMPLER_CAPACITY) {
        return;
    }
    void *frames[SAMPLER_DEPTH + 2u];
    const int found = backtrace(frames, (int)(SAMPLER_DEPTH + 2u));
    g_samples[slot].tid = (int32_t)syscall(SYS_gettid);
    g_samples[slot].clock_ms = sample_clock_ms();
    unsigned kept = 0u;
    /* skip the handler and the signal trampoline frames */
    for (int index = 2; index < found && kept < SAMPLER_DEPTH; index++) {
        g_samples[slot].rip[kept++] = (uint64_t)(uintptr_t)frames[index];
    }
    g_samples[slot].depth = kept;
}

static pthread_t g_wall_thread;
static atomic_bool g_wall_run;
static unsigned g_wall_hz;

static void *wall_main(void *argument)
{
    (void)argument;
    const pid_t self = getpid();
    const pid_t me = (pid_t)syscall(SYS_gettid);
    while (atomic_load(&g_wall_run)) {
        struct timespec nap = {0, (long)(1000000000u / g_wall_hz)};
        nanosleep(&nap, NULL);
        DIR *dir = opendir("/proc/self/task");
        if (dir == NULL) {
            continue;
        }
        struct dirent *item;
        while ((item = readdir(dir)) != NULL) {
            const long tid = strtol(item->d_name, NULL, 10);
            if (tid > 0 && tid != me) {
                syscall(SYS_tgkill, self, (pid_t)tid, SIGRTMIN + 3);
            }
        }
        closedir(dir);
    }
    return NULL;
}

/* T1495 phase marking: SIGUSR1 only counts a request (async-signal-safe). A helper thread does the file work, so a
 * long interactive session splits into per-action profiles without a restart. Each request writes the samples since the
 * previous dump to FILE.<phase> and resets the counter; <phase> is the first word of the control file FILE.phase when
 * the tool wrote one, else an incrementing number. The exit dump to FILE keeps the samples since the last phase. */
static pthread_t g_dump_thread;
static atomic_bool g_dump_run;
static atomic_uint g_dump_requests;
static unsigned g_dump_served;
static unsigned g_dump_number;

static void on_dump_signal(int signal_number)
{
    (void)signal_number;
    atomic_fetch_add(&g_dump_requests, 1u);
}

/* Write the samples [0, count) to `path` (+ `.ms`). With reset, loop until the counter is reset to 0 with no sample
 * lost in between (a sample being stored at that instant can land in the wrong phase, one sample at most). */
static bool dump_profile(const char *path, bool reset)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    char side_path[sizeof g_path + 64u];
    snprintf(side_path, sizeof side_path, "%s.ms", path);
    FILE *side = fopen(side_path, "w");
    /* The first mapping of the executable is its load address (PIE). */
    char exe[512] = "";
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (length > 0) {
        exe[length] = '\0';
    }
    unsigned long base = 0u;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps != NULL) {
        char line[1024];
        while (fgets(line, sizeof line, maps) != NULL) {
            if (strstr(line, exe) != NULL) {
                base = strtoul(line, NULL, 16);
                break;
            }
        }
        fclose(maps);
    }
    /* fixed width count, patched after the body is written */
    fprintf(file, "# base %lx %s\n", base, exe);
    const long count_at = ftell(file);
    fprintf(file, "# samples %010u\n# mode %s\n", 0u, g_wall_mode ? "wall" : "cpu");
    /* T1289: the executable mappings of the libraries (`# map START END FILE_OFFSET PATH`), so a sample outside the executable can be named. */
    maps = fopen("/proc/self/maps", "r");
    if (maps != NULL) {
        char line[1024];
        while (fgets(line, sizeof line, maps) != NULL) {
            unsigned long start = 0u, end = 0u, offset = 0u;
            char perms[8] = "";
            char mapped[900] = "";
            if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %899[^\n]", &start, &end, perms, &offset, mapped) >= 4 && perms[2] == 'x' &&
                mapped[0] == '/' && strstr(mapped, exe) == NULL) {
                fprintf(file, "# map %lx %lx %lx %s\n", start, end, offset, mapped);
            }
        }
        fclose(maps);
    }
    unsigned written = 0u;
    for (unsigned pass = 0u; pass < 1000u; pass++) {
        unsigned raw = atomic_load(&g_count);
        unsigned count = raw > SAMPLER_CAPACITY ? SAMPLER_CAPACITY : raw;
        for (; written < count; written++) {
            fprintf(file, "%d", g_samples[written].tid);
            for (uint32_t depth = 0u; depth < g_samples[written].depth; depth++) {
                fprintf(file, " %llx", (unsigned long long)g_samples[written].rip[depth]);
            }
            fputc('\n', file);
            if (side != NULL) {
                fprintf(side, "%llu\n", (unsigned long long)g_samples[written].clock_ms);
            }
        }
        if (!reset || atomic_compare_exchange_strong(&g_count, &raw, 0u)) {
            reset = false;
            break;
        }
    }
    if (reset) {
        atomic_store(&g_count, 0u); /* the counter never settled in 1000 passes: reset anyway */
    }
    fseek(file, count_at, SEEK_SET);
    fprintf(file, "# samples %010u", written);
    fclose(file);
    if (side != NULL) {
        fclose(side);
    }
    return true;
}

static void *dump_main(void *argument)
{
    (void)argument;
    while (atomic_load(&g_dump_run)) {
        const unsigned requested = atomic_load(&g_dump_requests);
        if (requested == g_dump_served) {
            struct timespec nap = {0, 20000000L};
            nanosleep(&nap, NULL);
            continue;
        }
        g_dump_served++;
        char phase[64];
        snprintf(phase, sizeof phase, "%u", ++g_dump_number);
        char control_path[sizeof g_path + 8u];
        snprintf(control_path, sizeof control_path, "%s.phase", g_path);
        FILE *control = fopen(control_path, "r");
        if (control != NULL) {
            char word[64] = "";
            if (fscanf(control, "%63s", word) == 1) {
                size_t kept = 0u;
                for (size_t index = 0u; word[index] != '\0'; index++) {
                    const char c = word[index];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
                        phase[kept++] = c;
                    }
                }
                if (kept > 0u) {
                    phase[kept] = '\0';
                }
            }
            fclose(control);
        }
        char out_path[sizeof g_path + 80u];
        snprintf(out_path, sizeof out_path, "%s.%s", g_path, phase);
        (void)dump_profile(out_path, true);
        /* acknowledge: FILE.ack holds the number of phases written, the tool waits on it */
        char ack_path[sizeof g_path + 8u];
        snprintf(ack_path, sizeof ack_path, "%s.ack", g_path);
        FILE *ack = fopen(ack_path, "w");
        if (ack != NULL) {
            fprintf(ack, "%u %s\n", g_dump_served, phase);
            fclose(ack);
        }
    }
    return NULL;
}

static bool start_dump_thread(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_dump_signal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0) {
        return false;
    }
    atomic_store(&g_dump_run, true);
    if (pthread_create(&g_dump_thread, NULL, dump_main, NULL) != 0) {
        return false;
    }
    char pid_path[sizeof g_path + 8u];
    snprintf(pid_path, sizeof pid_path, "%s.pid", g_path);
    FILE *pid_file = fopen(pid_path, "w");
    if (pid_file != NULL) {
        fprintf(pid_file, "%d\n", (int)getpid());
        fclose(pid_file);
    }
    return true;
}

bool cpu_sampler_start(const char *path, unsigned hz, bool wall)
{
    if (g_running || path == NULL || hz == 0u || hz > 10000u || strlen(path) >= sizeof g_path) {
        return false;
    }
    strcpy(g_path, path);
    if (wall) {
        void *warm[2];
        (void)backtrace(warm, 2); /* load libgcc's unwinder before a handler needs it */
        struct sigaction wall_action;
        memset(&wall_action, 0, sizeof wall_action);
        wall_action.sa_sigaction = on_wall_signal;
        wall_action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&wall_action.sa_mask);
        if (sigaction(SIGRTMIN + 3, &wall_action, NULL) != 0) {
            return false;
        }
        g_wall_hz = hz;
        atomic_store(&g_wall_run, true);
        if (pthread_create(&g_wall_thread, NULL, wall_main, NULL) != 0) {
            return false;
        }
        g_running = true;
        g_wall_mode = true;
        return start_dump_thread();
    }
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_profile_signal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPROF, &action, NULL) != 0) {
        return false;
    }
    const struct itimerval timer = {{0, (suseconds_t)(1000000u / hz)}, {0, (suseconds_t)(1000000u / hz)}};
    if (setitimer(ITIMER_PROF, &timer, NULL) != 0) {
        return false;
    }
    g_running = true;
    return start_dump_thread();
}

void cpu_sampler_stop(void)
{
    if (!g_running) {
        return;
    }
    if (g_wall_mode) {
        atomic_store(&g_wall_run, false);
        pthread_join(g_wall_thread, NULL);
        struct timespec settle = {0, 50000000L};
        nanosleep(&settle, NULL);
    } else {
        const struct itimerval off = {{0, 0}, {0, 0}};
        setitimer(ITIMER_PROF, &off, NULL);
    }
    g_running = false;
    if (atomic_exchange(&g_dump_run, false)) {
        pthread_join(g_dump_thread, NULL);
    }
    (void)dump_profile(g_path, false);
}
