/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "live_module_maker.h"

#include "gpu_phase_timing.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_MS 120000
#define MAX_REMEMBERED 256u
#define MESSAGE_BYTES 200u

static unsigned timeout_ms(void)
{
    const char *override = getenv("TSFP_LIVE_MODULE_TIMEOUT_MS");
    if (override != NULL && override[0] != '\0') {
        char *end = NULL;
        const unsigned long value = strtoul(override, &end, 10);
        if (end != override && *end == '\0' && value > 0ul && value <= 3600000ul) {
            return (unsigned)value;
        }
    }
    return TIMEOUT_MS;
}

typedef struct {
    char name[96];
    char message[MESSAGE_BYTES];
} failure;

struct live_module_maker {
  pthread_mutex_t lock; /* T1246: the batch runs make() on several threads; stats and failures are under it, the subprocess is not */
  bool live_raster;
  char directory[512];
  char python[256];
  char glslang[256];
  live_module_maker_stats stats;
  failure failures[MAX_REMEMBERED];
  size_t failure_count;
};

static double now_seconds(void)
{
    struct timespec at;
    clock_gettime(CLOCK_MONOTONIC, &at);
    return (double)at.tv_sec + (double)at.tv_nsec / 1e9;
}

/* generated_<64 lowercase hex> or combiner_<64 lowercase hex>, nothing else may become a file name. */
static bool name_ok(bool fragment, const char *name)
{
    const char *prefix = fragment ? "combiner_" : "generated_";
    const size_t prefix_length = strlen(prefix);
    /* T860: a combiner module may carry the `_alpha` suffix (the variant that runs the alpha test) */
    const bool alpha = fragment && strlen(name) == prefix_length + 64u + 6u && strcmp(name + prefix_length + 64u, "_alpha") == 0;
    if (strncmp(name, prefix, prefix_length) != 0 || (strlen(name) != prefix_length + 64u && !alpha)) {
        return false;
    }
    for (size_t i = prefix_length; i < prefix_length + 64u; i++) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static void say(char *error, size_t error_bytes, const char *format, const char *a, const char *b)
{
    if (error != NULL && error_bytes != 0u) {
        snprintf(error, error_bytes, format, a, b);
    }
}

live_module_maker *live_module_maker_create(const char *directory, const char *python, const char *glslang, char *error,
                                            size_t error_bytes)
{
  if (directory != NULL &&
      strlen(directory) >= sizeof((live_module_maker *)0)->directory) {
    say(error, error_bytes, "module directory path too long%s%s", "", "");
    return NULL;
  }
    struct stat info;
    if (directory == NULL || stat(directory, &info) != 0 || !S_ISDIR(info.st_mode) || access(directory, W_OK) != 0) {
        say(error, error_bytes, "the module directory %s%s is not a writable directory", directory != NULL ? directory : "(none)", "");
        return NULL;
    }
    if (stat("tools/nv2a/live_modules.py", &info) != 0) {
        say(error, error_bytes, "tools/nv2a/live_modules.py is not under the current directory%s%s (run the host from the repository root)",
            "", "");
        return NULL;
    }
    live_module_maker *maker = calloc(1u, sizeof *maker);
    if (maker == NULL) {
        say(error, error_bytes, "out of memory%s%s", "", "");
        return NULL;
    }
    const char *chosen = python != NULL ? python : getenv("TSFP_PYTHON");
    snprintf(maker->directory, sizeof maker->directory, "%s", directory);
    snprintf(maker->python, sizeof maker->python, "%s", chosen != NULL && chosen[0] != '\0' ? chosen : "python3");
    snprintf(maker->glslang, sizeof maker->glslang, "%s", glslang != NULL ? glslang : "glslangValidator");
    pthread_mutex_init(&maker->lock, NULL);
    return maker;
}

void live_module_maker_destroy(live_module_maker *maker)
{
    if (maker != NULL) {
        pthread_mutex_destroy(&maker->lock);
    }
    free(maker);
}

static const failure *find_failure(const live_module_maker *maker, const char *name)
{
    for (size_t i = 0u; i < maker->failure_count; i++) {
        if (strcmp(maker->failures[i].name, name) == 0) {
            return &maker->failures[i];
        }
    }
    return NULL;
}

static void remember_failure(live_module_maker *maker, const char *name, const char *message)
{
    maker->stats.failed++;
    if (maker->failure_count < MAX_REMEMBERED) {
        failure *slot = &maker->failures[maker->failure_count++];
        snprintf(slot->name, sizeof slot->name, "%s", name);
        snprintf(slot->message, sizeof slot->message, "%s", message);
    }
}

static bool exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

/* Run argv with stdout and stderr captured (the tail up to capture_bytes), killed after the deadline. The exit status, or -1. */
static int run_child(char *const argv[], char *capture, size_t capture_bytes);

/* T1289: timed for the guest frame trace (a translation is a subprocess, the slowest stall of a cold module cache). */
static int run(char *const argv[], char *capture, size_t capture_bytes)
{
    const uint64_t phase_start = gpu_phase_now();
    const int status = run_child(argv, capture, capture_bytes);
    gpu_phase_add(GPU_PHASE_MODULE_TRANSLATE, phase_start);
    return status;
}

static int run_child(char *const argv[], char *capture, size_t capture_bytes)
{
    int pipes[2];
    capture[0] = '\0';
    if (pipe(pipes) != 0) {
        return -1;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(pipes[0]);
        close(pipes[1]);
        return -1;
    }
    if (child == 0) {
        dup2(pipes[1], 1);
        dup2(pipes[1], 2);
        close(pipes[0]);
        close(pipes[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipes[1]);
    size_t used = 0u;
    const double deadline = now_seconds() + timeout_ms() / 1000.0;
    bool killed = false;
    bool pipe_open = true;
    int status = 0;
    for (;;) {
        const pid_t reaped = waitpid(child, &status, WNOHANG);
        if (reaped == child) break;
        if (reaped < 0 && errno != EINTR) break;
        int ready = 0;
        if (pipe_open) {
            double remaining = deadline - now_seconds();
            int wait_ms = remaining > 0.0 ? (int)(remaining * 1000.0) : 0;
            if (wait_ms > 200) wait_ms = 200;
            struct pollfd wait = {pipes[0], POLLIN, 0};
            ready = poll(&wait, 1, wait_ms);
        } else {
            const struct timespec pause = {0, 1000000L};
            nanosleep(&pause, NULL);
        }
        if (ready > 0 && pipe_open) {
            char buffer[256];
            const ssize_t got = read(pipes[0], buffer, sizeof buffer);
            if (got <= 0) {
                close(pipes[0]);
                pipe_open = false;
            } else {
                for (ssize_t i = 0; i < got; i++) {
                    if (used + 1u < capture_bytes) capture[used++] = buffer[i];
                }
            }
        } else if (ready < 0 && errno != EINTR) {
            close(pipes[0]);
            pipe_open = false;
        }
        if (now_seconds() > deadline && !killed) {
            kill(child, SIGKILL);
            killed = true;
        }
    }
    capture[used] = '\0';
    if (pipe_open) close(pipes[0]);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (killed) {
        return -2;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* The last non empty line of the captured text, without the newline, as the message. */
static void last_line(char *text, char *out, size_t out_bytes)
{
    size_t end = strlen(text);
    while (end != 0u && (text[end - 1u] == '\n' || text[end - 1u] == '\r' || text[end - 1u] == ' ')) {
        text[--end] = '\0';
    }
    const char *start = strrchr(text, '\n');
    snprintf(out, out_bytes, "%s", start != NULL ? start + 1 : text);
}

bool live_module_maker_make(void *context, bool fragment, const char *name, const uint8_t *bytes, size_t byte_count, char *error,
                            size_t error_bytes)
{
    live_module_maker *maker = context;
    if (maker == NULL || name == NULL || bytes == NULL || byte_count == 0u || !name_ok(fragment, name)) {
        say(error, error_bytes, "refused module request %s%s", name != NULL ? name : "(none)", "");
        return false;
    }
    char target[640];
    snprintf(target, sizeof target, "%s/%s.spv", maker->directory, name);
    pthread_mutex_lock(&maker->lock);
    if (exists(target)) {
        maker->stats.reused++;
        pthread_mutex_unlock(&maker->lock);
        return true;
    }
    const failure *known = find_failure(maker, name);
    if (known != NULL) {
        maker->stats.refused_again++;
        say(error, error_bytes, "%s%s", known->message, "");
        pthread_mutex_unlock(&maker->lock);
        return false;
    }
    pthread_mutex_unlock(&maker->lock);
    char input[700];
    snprintf(input, sizeof input, "%s/.%s.in.%ld", maker->directory, name, (long)getpid());
    FILE *file = fopen(input, "wb");
    if (file == NULL || fwrite(bytes, 1u, byte_count, file) != byte_count || fclose(file) != 0) {
        pthread_mutex_lock(&maker->lock);
        remember_failure(maker, name, "could not write the translator input");
        pthread_mutex_unlock(&maker->lock);
        say(error, error_bytes, "%s%s", "could not write the translator input", "");
        unlink(input);
        return false;
    }
    char expect[96];
    snprintf(expect, sizeof expect, "%s", name);
    char *const argv[] = {maker->python,
                          "-m",
                          "tools.nv2a.live_modules",
                          fragment ? "combiner" : "vertex",
                          input,
                          "--out",
                          maker->directory,
                          "--expect",
                          expect,
                          "--glslang",
                          maker->glslang,
                          maker->live_raster ? "--live-raster" : NULL,
                          NULL};
    char output[1024];
    const double started = now_seconds();
    const int status = run(argv, output, sizeof output);
    const double spent = now_seconds() - started;
    unlink(input);
    pthread_mutex_lock(&maker->lock);
    maker->stats.seconds += spent;
    if (status == 0 && exists(target)) {
        maker->stats.made++;
        pthread_mutex_unlock(&maker->lock);
        return true;
    }
    pthread_mutex_unlock(&maker->lock);
    char message[MESSAGE_BYTES];
    if (status == -2) {
        snprintf(message, sizeof message, "the translator did not finish in %d s", TIMEOUT_MS / 1000);
    } else if (status == 127 || status == -1) {
        snprintf(message, sizeof message, "could not run %s -m tools.nv2a.live_modules", maker->python);
    } else {
        char line[MESSAGE_BYTES];
        last_line(output, line, sizeof line);
        snprintf(message, sizeof message, "%s", line[0] != '\0' ? line : "the translator wrote no module");
    }
    pthread_mutex_lock(&maker->lock);
    remember_failure(maker, name, message);
    pthread_mutex_unlock(&maker->lock);
    say(error, error_bytes, "%s%s", message, "");
    return false;
}

typedef struct {
    live_module_maker *maker;
    const live_module_request *requests;
    size_t count;
    size_t next; /* guarded by maker->lock */
} batch_state;

static void *batch_worker(void *argument)
{
    batch_state *batch = argument;
    for (;;) {
        pthread_mutex_lock(&batch->maker->lock);
        const size_t index = batch->next < batch->count ? batch->next++ : batch->count;
        pthread_mutex_unlock(&batch->maker->lock);
        if (index >= batch->count) {
            return NULL;
        }
        const live_module_request *request = &batch->requests[index];
        char error[MESSAGE_BYTES];
        (void)live_module_maker_make(batch->maker, request->fragment, request->name, request->bytes, request->byte_count, error,
                                     sizeof error);
    }
}

size_t live_module_maker_make_batch(live_module_maker *maker, const live_module_request *requests, size_t count, unsigned parallel)
{
    if (maker == NULL || requests == NULL || count == 0u) {
        return 0u;
    }
    if (parallel == 0u) {
        parallel = 8u;
    }
    if (parallel > 32u) {
        parallel = 32u;
    }
    if (parallel > count) {
        parallel = (unsigned)count;
    }
    const double started = now_seconds();
    batch_state batch = {maker, requests, count, 0u};
    pthread_t threads[32];
    unsigned started_threads = 0u;
    for (; started_threads < parallel; started_threads++) {
        if (pthread_create(&threads[started_threads], NULL, batch_worker, &batch) != 0) {
            break;
        }
    }
    if (started_threads == 0u) {
        batch_worker(&batch); /* no thread could be made: do it here, one after another */
    }
    for (unsigned i = 0u; i < started_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    size_t present = 0u;
    pthread_mutex_lock(&maker->lock);
    for (size_t i = 0u; i < count; i++) {
        char target[640];
        snprintf(target, sizeof target, "%s/%s.spv", maker->directory, requests[i].name);
        present += exists(target) ? 1u : 0u;
    }
    maker->stats.batches++;
    maker->stats.batch_modules += count;
    maker->stats.batch_wall_seconds += now_seconds() - started;
    pthread_mutex_unlock(&maker->lock);
    return present;
}

live_module_maker_stats live_module_maker_get_stats(const live_module_maker *maker)
{
    live_module_maker_stats none;
    memset(&none, 0, sizeof none);
    return maker != NULL ? maker->stats : none;
}

void live_module_maker_print(const live_module_maker *maker, FILE *out)
{
    if (maker == NULL) {
        return;
    }
    const live_module_maker_stats *s = &maker->stats;
    fprintf(out,
            "module maker   ON-DEMAND translation (T847, INFERRED translators): %llu modules made in %.2f s, %llu already in the "
            "directory, %llu names failed (%llu later asks refused without running), directory %s\n",
            s->made, s->seconds, s->reused, s->failed, s->refused_again, maker->directory);
    fprintf(out,
            "module maker   parallel batches (T1247): %llu batches asked for %llu modules in %.2f s of wall time (the guest waits for a batch, not for each module)\n",
            s->batches, s->batch_modules, s->batch_wall_seconds);
    for (size_t i = 0u; i < maker->failure_count; i++) {
        fprintf(out, "module maker   failed %s: %s\n", maker->failures[i].name, maker->failures[i].message);
    }
}

void live_module_maker_set_live_raster(live_module_maker *maker, bool enabled) {
  if (maker != NULL)
    maker->live_raster = enabled;
}

bool live_module_maker_profile_directory(const char *root, char *out,
                                         size_t bytes, char *error,
                                         size_t error_bytes) {
  const int n = snprintf(out, bytes, "%s/%s", root, LIVE_MODULE_RASTER_PROFILE);
  if (n < 0 || (size_t)n >= bytes) {
    say(error, error_bytes, "module profile path too long%s%s", "", "");
    return false;
  }
  if (mkdir(out, 0700) != 0 && errno != EEXIST) {
    say(error, error_bytes, "cannot create live module profile %s: %s", out,
        strerror(errno));
    return false;
  }
  const char *prefixes[2] = {"generated", "combiner"};
  for (size_t i = 0; i < 2; i++) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s_%064u.spv", out, prefixes[i], 0u);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
      if (errno == EEXIST)
        continue;
      say(error, error_bytes, "cannot create module placeholder %s: %s", path,
          strerror(errno));
      return false;
    }
    const uint8_t magic[20] = {3, 2, 35, 7};
    const ssize_t written = write(fd, magic, sizeof magic);
    close(fd);
    if (written != (ssize_t)sizeof magic) {
      say(error, error_bytes, "cannot write module placeholder %s%s", path, "");
      return false;
    }
  }
  return true;
}
