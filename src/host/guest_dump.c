/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1599: the READ-ONLY guest memory dump (see guest_dump.h). Reads only through kernel_guest_read_bytes. The SIGUSR1
 * phase protocol is the one function_census.c and cpu_sampler.c use (handler counts, helper thread works, FILE.ack).
 */
#include "guest_dump.h"

#include "guest_poke.h"

#include "kernel_call.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CHUNK 4096u

/* One walk over the set serves three jobs: the live dump (reader = the guest memory, text out), the snapshot capture
 * (T1629: reader = the guest memory and a log of every read, `file` NULL so nothing is formatted on the calling thread) and
 * the snapshot write (reader = the log replayed, text out). The text is therefore the same bytes by construction. */
typedef bool (*dump_reader)(uint32_t address, void *out, size_t length, void *user);

#define OUT_PRINTF(...)                       \
    do {                                      \
        if (file != NULL) {                   \
            fprintf(file, __VA_ARGS__);       \
        }                                     \
    } while (0)

static bool dump_body(FILE *file, const guest_dump_set *set, const char *header, dump_reader read, void *user)
{
    bool all_read = true;
    OUT_PRINTF("# guest-dump 1\n# ranges %u bytes %llu\n", set->count, (unsigned long long)set->total_bytes);
    if (header != NULL && file != NULL) {
        fputs(header, file);
    }
    uint8_t buffer[CHUNK];
    for (unsigned index = 0u; index < set->count; index++) {
        guest_dump_range range = set->ranges[index];
        if (range.indirect) {
            const guest_dump_range spec = range;
            uint8_t raw[4];
            if (spec.twice) {
                /* T1759: two levels, a null or unreadable pointer at either one is a recorded marker */
                uint32_t first = 0u;
                uint32_t slot = 0u;
                uint32_t second = 0u;
                uint32_t target = 0u;
                const char *why = NULL;
                OUT_PRINTF("indirect2 0x%08X + 0x%X + 0x%X length 0x%X\n", (unsigned)spec.address, (unsigned)spec.offset,
                           (unsigned)spec.offset2, (unsigned)spec.length);
                if (!read(spec.address, raw, sizeof raw, user)) {
                    why = "first pointer dword not readable";
                } else {
                    first = (uint32_t)raw[0] | (uint32_t)raw[1] << 8 | (uint32_t)raw[2] << 16 | (uint32_t)raw[3] << 24;
                    OUT_PRINTF("pointer 0x%08X", (unsigned)first);
                    why = guest_dump_twice_slot(&spec, first, &slot);
                    if (why == NULL && !read(slot, raw, sizeof raw, user)) {
                        why = "second pointer dword not readable";
                    }
                    if (why == NULL) {
                        second = (uint32_t)raw[0] | (uint32_t)raw[1] << 8 | (uint32_t)raw[2] << 16 | (uint32_t)raw[3] << 24;
                        OUT_PRINTF(" pointer2 0x%08X", (unsigned)second);
                        why = guest_dump_twice_target(&spec, second, &target);
                    }
                }
                if (why != NULL) {
                    OUT_PRINTF("\nunreadable pointer at 0x%08X: %s\n", (unsigned)spec.address, why);
                    all_read = false;
                    continue;
                }
                OUT_PRINTF(" final 0x%08X\n", (unsigned)target);
                range.address = target;
            } else {
            OUT_PRINTF("indirect 0x%08X + 0x%X length 0x%X\n", (unsigned)spec.address, (unsigned)spec.offset,
                       (unsigned)spec.length);
            if (!read(spec.address, raw, sizeof raw, user)) {
                OUT_PRINTF("unreadable pointer at 0x%08X: pointer dword not readable\n", (unsigned)spec.address);
                all_read = false;
                continue;
            }
            const uint32_t pointer = (uint32_t)raw[0] | (uint32_t)raw[1] << 8 | (uint32_t)raw[2] << 16 | (uint32_t)raw[3] << 24;
            uint32_t final = 0u;
            const char *reason = guest_dump_indirect_target(&spec, pointer, &final);
            OUT_PRINTF("pointer 0x%08X", (unsigned)pointer);
            if (reason != NULL) {
                OUT_PRINTF("\nunreadable pointer at 0x%08X: %s\n", (unsigned)spec.address, reason);
                all_read = false;
                continue;
            }
            OUT_PRINTF(" final 0x%08X\n", (unsigned)final);
            range.address = final;
            }
        }
        OUT_PRINTF("range 0x%08X 0x%X\n", (unsigned)range.address, (unsigned)range.length);
        uint32_t done = 0u;
        while (done < range.length) {
            const uint32_t want = range.length - done < CHUNK ? range.length - done : CHUNK;
            if (!read(range.address + done, buffer, want, user)) {
                OUT_PRINTF("unreadable 0x%08X\n", (unsigned)(range.address + done));
                all_read = false;
                break;
            }
            if (file != NULL) {
                for (uint32_t row = 0u; row < want; row += 16u) {
                    fprintf(file, "%08X:", (unsigned)(range.address + done + row));
                    for (uint32_t column = row; column < row + 16u && column < want; column++) {
                        fprintf(file, " %02x", buffer[column]);
                    }
                    fputc('\n', file);
                }
            }
            done += want;
        }
    }
    return all_read;
}

static bool live_reader(uint32_t address, void *out, size_t length, void *user)
{
    (void)user;
    return kernel_guest_read_bytes(address, out, length);
}

/* The text goes to `path.tmp` and is renamed over `path`, so a reader never sees a partial dump. */
static bool write_atomic(const char *path, const guest_dump_set *set, const char *header, dump_reader read, void *user)
{
    char temporary[600];
    if (snprintf(temporary, sizeof temporary, "%s.tmp", path) >= (int)sizeof temporary) {
        return false;
    }
    FILE *file = fopen(temporary, "w");
    if (file == NULL) {
        return false;
    }
    const bool all_read = dump_body(file, set, header, read, user);
    const bool closed = fclose(file) == 0;
    if (!closed || rename(temporary, path) != 0) {
        remove(temporary);
        return false;
    }
    return all_read;
}

bool guest_dump_write(const guest_dump_set *set, const char *path)
{
    return guest_dump_write_with_header(set, path, NULL);
}

bool guest_dump_write_with_header(const guest_dump_set *set, const char *path, const char *header)
{
    if (set == NULL || path == NULL || set->count == 0u) {
        return false;
    }
    return write_atomic(path, set, header, live_reader, NULL);
}

/* ---- T1629: binary snapshot of a dump, captured on the calling thread, formatted later on another ---- */

typedef struct {
    uint32_t address;
    uint32_t length;
    uint32_t data_offset;
    bool ok;
} snapshot_read;

struct guest_dump_snapshot {
    guest_dump_set set;
    snapshot_read *reads;
    size_t read_count;
    size_t read_capacity;
    uint8_t *data;
    size_t data_used;
    size_t data_capacity;
    bool overflow;
    bool all_read;
};

typedef struct {
    guest_dump_snapshot *snapshot;
    size_t cursor; /* replay position */
} snapshot_cursor;

static bool recording_reader(uint32_t address, void *out, size_t length, void *user)
{
    guest_dump_snapshot *snapshot = user;
    const bool ok = kernel_guest_read_bytes(address, out, length);
    if (snapshot->read_count >= snapshot->read_capacity ||
        (ok && snapshot->data_used + length > snapshot->data_capacity)) {
        snapshot->overflow = true;
        return false;
    }
    snapshot_read *entry = &snapshot->reads[snapshot->read_count++];
    entry->address = address;
    entry->length = (uint32_t)length;
    entry->ok = ok;
    entry->data_offset = (uint32_t)snapshot->data_used;
    if (ok) {
        memcpy(snapshot->data + snapshot->data_used, out, length);
        snapshot->data_used += length;
    }
    return ok;
}

static bool replay_reader(uint32_t address, void *out, size_t length, void *user)
{
    snapshot_cursor *cursor = user;
    const guest_dump_snapshot *snapshot = cursor->snapshot;
    if (cursor->cursor >= snapshot->read_count) {
        return false;
    }
    const snapshot_read *entry = &snapshot->reads[cursor->cursor++];
    if (entry->address != address || entry->length != length) {
        return false; /* the walk diverged from the capture: cannot happen, refuse rather than invent bytes */
    }
    if (entry->ok) {
        memcpy(out, snapshot->data + entry->data_offset, length);
    }
    return entry->ok;
}

guest_dump_snapshot *guest_dump_capture(const guest_dump_set *set)
{
    if (set == NULL || set->count == 0u) {
        return NULL;
    }
    guest_dump_snapshot *snapshot = calloc(1u, sizeof *snapshot);
    if (snapshot == NULL) {
        return NULL;
    }
    snapshot->set = *set;
    size_t reads = 0u;
    size_t bytes = 0u;
    for (unsigned index = 0u; index < set->count; index++) {
        reads += 1u + ((size_t)set->ranges[index].length + CHUNK - 1u) / CHUNK;
        bytes += (size_t)set->ranges[index].length + 4u;
    }
    snapshot->reads = malloc(reads * sizeof *snapshot->reads);
    snapshot->data = malloc(bytes);
    if (snapshot->reads == NULL || snapshot->data == NULL) {
        guest_dump_snapshot_free(snapshot);
        return NULL;
    }
    snapshot->read_capacity = reads;
    snapshot->data_capacity = bytes;
    snapshot->all_read = dump_body(NULL, &snapshot->set, NULL, recording_reader, snapshot);
    if (snapshot->overflow) {
        guest_dump_snapshot_free(snapshot);
        return NULL;
    }
    return snapshot;
}

bool guest_dump_snapshot_write(const guest_dump_snapshot *snapshot, const char *path, const char *header)
{
    if (snapshot == NULL || path == NULL) {
        return false;
    }
    snapshot_cursor cursor = {(guest_dump_snapshot *)snapshot, 0u};
    return write_atomic(path, &snapshot->set, header, replay_reader, &cursor);
}

bool guest_dump_snapshot_complete(const guest_dump_snapshot *snapshot)
{
    return snapshot != NULL && snapshot->all_read;
}

size_t guest_dump_snapshot_bytes(const guest_dump_snapshot *snapshot)
{
    return snapshot == NULL ? 0u
                            : sizeof *snapshot + snapshot->read_capacity * sizeof *snapshot->reads + snapshot->data_capacity;
}

void guest_dump_snapshot_free(guest_dump_snapshot *snapshot)
{
    if (snapshot != NULL) {
        free(snapshot->reads);
        free(snapshot->data);
        free(snapshot);
    }
}

uint64_t guest_dump_estimate_text_bytes(const guest_dump_set *set)
{
    /* the file header is ~50 bytes, the caller's extra header lines are not counted (see button_dump_host.c) */
    uint64_t total = 64u;
    for (unsigned index = 0u; set != NULL && index < set->count; index++) {
        const uint64_t length = set->ranges[index].length;
        total += 96u + (set->ranges[index].twice ? 64u : 0u) + ((length + 15u) / 16u) * 10u + length * 3u; /* indirect, pointer, range lines + rows */
    }
    return total;
}

static bool make_directories(const char *path)
{
    char copy[512];
    if (strlen(path) >= sizeof copy) {
        return false;
    }
    strcpy(copy, path);
    for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(copy, 0777) != 0 && access(copy, F_OK) != 0) {
                return false;
            }
            *cursor = '/';
        }
    }
    return (mkdir(copy, 0777) == 0) || access(copy, F_OK) == 0;
}

static guest_dump_set dump_set;
static char dump_dir[400];
static pthread_t dump_thread;
static atomic_bool dump_run;
static atomic_uint dump_requests;
static unsigned dump_served;
static unsigned dump_number;
static struct sigaction dump_previous;
static bool dump_started;
/* T1614: the poke trigger SIGUSR2, separate from the SIGUSR1 phase boundary that the census and the dump share */
static atomic_uint poke_requests;
static unsigned poke_served;
static struct sigaction poke_previous;
static bool poke_handler_installed;

static void on_dump_signal(int signal_number)
{
    atomic_fetch_add(&dump_requests, 1u);
    if (dump_previous.sa_handler != SIG_DFL && dump_previous.sa_handler != SIG_IGN && dump_previous.sa_handler != NULL &&
        (dump_previous.sa_flags & SA_SIGINFO) == 0) {
        dump_previous.sa_handler(signal_number);
    }
}

static void on_poke_signal(int signal_number)
{
    atomic_fetch_add(&poke_requests, 1u);
    if (poke_previous.sa_handler != SIG_DFL && poke_previous.sa_handler != SIG_IGN && poke_previous.sa_handler != NULL &&
        (poke_previous.sa_flags & SA_SIGINFO) == 0) {
        poke_previous.sa_handler(signal_number);
    }
}

/* the label is the first word of DIR/<control_name> (guestdump.phase for SIGUSR1, guestpoke.phase for SIGUSR2) */
static void read_phase_word(const char *control_name, char *phase, size_t size)
{
    char control_path[sizeof dump_dir + 32u];
    snprintf(control_path, sizeof control_path, "%s/%s", dump_dir, control_name);
    FILE *control = fopen(control_path, "r");
    if (control == NULL) {
        return;
    }
    char word[64] = "";
    if (fscanf(control, "%63s", word) == 1) {
        size_t kept = 0u;
        char clean[64];
        for (size_t index = 0u; word[index] != '\0'; index++) {
            const char c = word[index];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
                clean[kept++] = c;
            }
        }
        if (kept > 0u) {
            clean[kept] = '\0';
            snprintf(phase, size, "%s", clean);
        }
    }
    fclose(control);
}

static bool dump_forced_state;
static uint64_t (*dump_present)(void);

void guest_dump_set_forced_state(bool forced_state, uint64_t (*present)(void))
{
    dump_forced_state = forced_state;
    dump_present = present;
}

static void dump_named(const char *phase, bool allow_poke)
{
    char path[sizeof dump_dir + 96u];
    snprintf(path, sizeof path, "%s/guestdump.%s", dump_dir, phase);
    char header[8192];
    header[0] = '\0';
    if (allow_poke) {
        /* T1613: a request guestpoke.<phase> is served here, before the dump, so the dump shows the result */
        guest_poke_consume(dump_dir, phase, dump_forced_state, dump_present != NULL ? dump_present() : 0u, header,
                           sizeof header);
    }
    const bool ok = guest_dump_write_with_header(&dump_set, path, header);
    fprintf(stderr, "guest dump (T1599, read-only): %u range(s), %llu byte(s) -> %s%s\n", dump_set.count,
            (unsigned long long)dump_set.total_bytes, path, ok ? "" : " (SOME RANGE UNREADABLE)");
}

static void *dump_main(void *argument)
{
    (void)argument;
    while (atomic_load(&dump_run)) {
        if (atomic_load(&poke_requests) != poke_served) {
            /* T1614: SIGUSR2 = serve DIR/guestpoke.<label> (label from DIR/guestpoke.phase), dump after it as
             * guestdump.<label>, ack in guestpoke.ack. A SIGUSR1 phase counter and ack are not touched. */
            poke_served++;
            char label[64];
            snprintf(label, sizeof label, "poke%u", poke_served);
            read_phase_word("guestpoke.phase", label, sizeof label);
            dump_named(label, true);
            char poke_ack_path[sizeof dump_dir + 32u];
            snprintf(poke_ack_path, sizeof poke_ack_path, "%s/guestpoke.ack", dump_dir);
            FILE *poke_ack = fopen(poke_ack_path, "w");
            if (poke_ack != NULL) {
                fprintf(poke_ack, "%u %s\n", poke_served, label);
                fclose(poke_ack);
            }
            continue;
        }
        if (atomic_load(&dump_requests) == dump_served) {
            struct timespec nap = {0, 20000000L};
            nanosleep(&nap, NULL);
            continue;
        }
        dump_served++;
        char phase[64];
        snprintf(phase, sizeof phase, "%u", ++dump_number);
        read_phase_word("guestdump.phase", phase, sizeof phase);
        dump_named(phase, true);
        char ack_path[sizeof dump_dir + 32u];
        snprintf(ack_path, sizeof ack_path, "%s/guestdump.ack", dump_dir);
        FILE *ack = fopen(ack_path, "w");
        if (ack != NULL) {
            fprintf(ack, "%u %s\n", dump_served, phase);
            fclose(ack);
        }
    }
    return NULL;
}

bool guest_dump_poke_now(const char *label)
{
    if (!dump_started || label == NULL || label[0] == '\0') {
        return false;
    }
    dump_named(label, true);
    char poke_ack_path[sizeof dump_dir + 32u];
    snprintf(poke_ack_path, sizeof poke_ack_path, "%s/guestpoke.ack", dump_dir);
    FILE *poke_ack = fopen(poke_ack_path, "w");
    if (poke_ack != NULL) {
        fprintf(poke_ack, "route %s\n", label);
        fclose(poke_ack);
    }
    return true;
}

bool guest_dump_start(const guest_dump_set *set, uint64_t max_bytes, const char *dir)
{
    if (dump_started || dir == NULL || strlen(dir) >= sizeof dump_dir || !guest_dump_check_total(set, max_bytes)) {
        return false;
    }
    strcpy(dump_dir, dir);
    if (!make_directories(dump_dir)) {
        return false;
    }
    dump_set = *set;
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_dump_signal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    memset(&dump_previous, 0, sizeof dump_previous);
    if (sigaction(SIGUSR1, &action, &dump_previous) != 0) {
        return false;
    }
    memset(&action, 0, sizeof action);
    action.sa_handler = on_poke_signal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    memset(&poke_previous, 0, sizeof poke_previous);
    poke_handler_installed = sigaction(SIGUSR2, &action, &poke_previous) == 0;
    atomic_store(&poke_requests, 0u);
    poke_served = 0u;
    atomic_store(&dump_run, true);
    atomic_store(&dump_requests, 0u);
    dump_served = 0u;
    dump_number = 0u;
    if (pthread_create(&dump_thread, NULL, dump_main, NULL) != 0) {
        sigaction(SIGUSR1, &dump_previous, NULL);
        if (poke_handler_installed) {
            sigaction(SIGUSR2, &poke_previous, NULL);
            poke_handler_installed = false;
        }
        return false;
    }
    dump_started = true;
    char pid_path[sizeof dump_dir + 32u];
    snprintf(pid_path, sizeof pid_path, "%s/guestdump.pid", dump_dir);
    FILE *pid_file = fopen(pid_path, "w");
    if (pid_file != NULL) {
        fprintf(pid_file, "%d\n", (int)getpid());
        fclose(pid_file);
    }
    return true;
}

void guest_dump_stop(void)
{
    if (!dump_started) {
        return;
    }
    atomic_store(&dump_run, false);
    pthread_join(dump_thread, NULL);
    sigaction(SIGUSR1, &dump_previous, NULL);
    if (poke_handler_installed) {
        sigaction(SIGUSR2, &poke_previous, NULL);
        poke_handler_installed = false;
    }
    dump_started = false;
    dump_named("exit", false);
}
