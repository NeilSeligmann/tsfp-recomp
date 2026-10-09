/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xmv_original.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#include "recomp_abi.h"
#include "recomp_callback.h"
#include "xmv_seed_patch.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern recomp_func_t recomp_xmv_lookup_original(uint32_t address) __attribute__((weak));
extern const char *recomp_xmv_profile_identity(void) __attribute__((weak));
/* Weak so the unit tests that link this module without the lifted runtime still link (T538). */
extern void recomp_callback_machine_snapshot(recomp_machine_snapshot *out) __attribute__((weak));
/* T611: the lifted codec wrapper, called directly for the seeded run. Weak for the same reason. */
extern void sub_00447E5E(void) __attribute__((weak));

static const uint32_t exports[XMV_ORIGINAL_EXPORT_COUNT] = {
    0x00444A2Du, 0x00444F71u, 0x00445055u, 0x004450C2u,
    0x00445241u, 0x00445252u, 0x0044525Du,
};
/* Argument dwords above the return address, for the trace line only. CloseDecoder takes
 * its first argument in ECX (thiscall), so it shows ECX separately. */
static const uint32_t export_args[XMV_ORIGINAL_EXPORT_COUNT] = {3u, 1u, 2u, 5u, 2u, 1u, 4u};
static const char *const export_names[XMV_ORIGINAL_EXPORT_COUNT] = {
    "CreateDecoderForFile", "CloseDecoder", "GetVideoDescriptor", "EnableAudioStream",
    "SetSynchronizationStream", "TerminatePlayback", "GetNextFrame",
};

static pthread_mutex_t configuration_lock = PTHREAD_MUTEX_INITIALIZER;
static bool configured;
static bool trace;
static bool skip_intro;
static const char *frame_dump_directory;
static unsigned frames_dumped;
static uint32_t frame_dump_maximum;
static xmv_data_resolver data_resolver;
#define FRAME_DUMP_BUFFER_BYTES (2u * 1024u * 1024u)
static uint8_t *frame_dump_buffer; /* allocated once at configuration, before any guest mapping exists */
#define SUBSTITUTE_NAME_BYTES 33u
static char substitute_from[SUBSTITUTE_NAME_BYTES];
static char substitute_to[SUBSTITUTE_NAME_BYTES];
static uint64_t substitutions;
static recomp_func_t functions[XMV_ORIGINAL_EXPORT_COUNT];
static size_t active_calls;

static bool collect(recomp_func_t *out)
{
    if (recomp_xmv_lookup_original == NULL || recomp_xmv_profile_identity == NULL) {
        return false;
    }
    const char *identity = recomp_xmv_profile_identity();
    if (identity == NULL || strcmp(identity, "xmv-original-v1") != 0) {
        return false;
    }
    for (size_t i = 0u; i < XMV_ORIGINAL_EXPORT_COUNT; i++) {
        const recomp_func_t function = recomp_xmv_lookup_original(exports[i]);
        if (function == NULL) {
            return false;
        }
        if (out != NULL) {
            out[i] = function;
        }
    }
    return true;
}

bool xmv_original_ready(void)
{
    return collect(NULL);
}

bool xmv_original_configure(bool enabled)
{
    recomp_func_t selected[XMV_ORIGINAL_EXPORT_COUNT] = {0};
    pthread_mutex_lock(&configuration_lock);
    if (active_calls != 0u || (enabled && !collect(selected))) {
        pthread_mutex_unlock(&configuration_lock);
        return false;
    }
    if (enabled) {
        memcpy(functions, selected, sizeof(functions));
    } else {
        memset(functions, 0, sizeof(functions));
    }
    configured = enabled;
    pthread_mutex_unlock(&configuration_lock);
    return true;
}

void xmv_original_set_trace(bool enabled)
{
    pthread_mutex_lock(&configuration_lock);
    trace = enabled;
    pthread_mutex_unlock(&configuration_lock);
}

bool xmv_original_set_skip_intro(bool enabled)
{
    pthread_mutex_lock(&configuration_lock);
    const bool allowed = active_calls == 0u && (!enabled || configured);
    if (allowed) skip_intro = enabled;
    pthread_mutex_unlock(&configuration_lock);
    return allowed;
}

/* Exact retail movie frame at GetNextFrame's call (0x305C7). Only the
 * language-specific EA logo and Free Radical logo in the startup selector.
 * Never classify by a basename alone: gameplay and attract callers are excluded. */
static bool startup_frame(void)
{
    uint32_t caller, owner, name, skippable;
    if (!kernel_guest_read_u32(g_esp, &caller) || caller != 0x305CCu ||
        !kernel_guest_read_u32(g_esp + 0x298u, &owner) ||
        !kernel_guest_read_u32(g_esp + 0x29Cu, &name) ||
        !kernel_guest_read_u32(g_esp + 0x2A0u, &skippable) || skippable != 0u) return false;
    return (owner == 0x70606u && (name == 0x47CFC0u || name == 0x47CFB8u ||
            name == 0x47CFB0u || name == 0x47CFA8u)) ||
           ((owner == 0x70614u || owner == 0x7063Cu) && name == 0x479628u) ||
           (owner == 0x70631u && name == 0x47CFA8u);
}

static bool valid_name(const char *text, size_t length)
{
    if (length == 0u || length >= SUBSTITUTE_NAME_BYTES) return false;
    for (size_t i = 0u; i < length; i++) {
        const char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}
bool xmv_original_set_substitute(const char *spec)
{
    char from[SUBSTITUTE_NAME_BYTES] = {0}, to[SUBSTITUTE_NAME_BYTES] = {0};
    if (spec != NULL) {
        const char *equals = strchr(spec, '=');
        if (equals == NULL || !valid_name(spec, (size_t)(equals - spec)) || !valid_name(equals + 1, strlen(equals + 1)))
            return false;
        memcpy(from, spec, (size_t)(equals - spec));
        memcpy(to, equals + 1, strlen(equals + 1));
    }
    pthread_mutex_lock(&configuration_lock);
    memcpy(substitute_from, from, sizeof(from));
    memcpy(substitute_to, to, sizeof(to));
    substitutions = 0u;
    pthread_mutex_unlock(&configuration_lock);
    return true;
}

/* Rewrite the title's own path string "<dir>\<FROM>.xmv" in place to "<dir>\<TO>.xmv". The buffer is the
 * title's 248 byte local, so the new string must fit the old one's room, checked against 240 bytes. */
static void substitute_movie(uint32_t path_pointer)
{
    char text[256];
    size_t length = 0u;
    while (length + 1u < sizeof(text)) {
        uint8_t byte = 0u;
        if (!kernel_guest_read_u8((kernel_guest_ptr)(path_pointer + (uint32_t)length), &byte) || byte == 0u) break;
        text[length++] = (char)byte;
    }
    text[length] = '\0';
    const char *slash = strrchr(text, '\\');
    const char *name = slash != NULL ? slash + 1 : text;
    const size_t from_length = strlen(substitute_from);
    if (strlen(name) != from_length + 4u || strncmp(name, substitute_from, from_length) != 0 ||
        strcmp(name + from_length, ".xmv") != 0)
        return;
    char rewritten[256];
    const int written = snprintf(rewritten, sizeof(rewritten), "%.*s%s.xmv", (int)(name - text), text, substitute_to);
    if (written <= 0 || (size_t)written >= 240u ||
        !kernel_guest_write_bytes((kernel_guest_ptr)path_pointer, rewritten, (size_t)written + 1u)) {
        fprintf(stderr, "xmv-original: substitute of \"%s\" refused, the rewritten path does not fit\n", text);
        return;
    }
    substitutions++;
    fprintf(stderr, "xmv-original: SUBSTITUTED movie \"%s\" -> \"%s\" (substitution %llu)\n", text, rewritten,
            (unsigned long long)substitutions);
}

void xmv_original_set_data_resolver(xmv_data_resolver resolver)
{
    pthread_mutex_lock(&configuration_lock);
    data_resolver = resolver;
    pthread_mutex_unlock(&configuration_lock);
}

void xmv_original_set_frame_dump(const char *directory, uint32_t maximum)
{
    pthread_mutex_lock(&configuration_lock);
    frame_dump_directory = directory;
    frame_dump_maximum = maximum;
    frames_dumped = 0u;
    if (directory != NULL && frame_dump_buffer == NULL) frame_dump_buffer = malloc(FRAME_DUMP_BUFFER_BYTES);
    pthread_mutex_unlock(&configuration_lock);
}

/* T538: entry capture of the codec wrapper (xmv_original.h). Written for tools/diagnostics/replay_xmv_capture.py:
 * "TS538A01", entry and EAX..ESP words (10 dwords, the last reserved 0), MMX (8 qwords), XMM (8 x 16), the
 * virtual x87 stack (8 doubles), top, control word, compare and condition words, MXCSR, the mapping count,
 * then per mapping base, size, permissions (1 read, 2 write) and its bytes. */
#define CAPTURE_MAX_BYTES (96ull << 20)
#define CAPTURE_MAX_MAPPINGS 64u
#define CAPTURE_DECODER_BYTES 0x168u
static const char *entry_capture_directory;
static uint32_t entry_capture_first[XMV_CAPTURE_RANGE_MAX];
static uint32_t entry_capture_last[XMV_CAPTURE_RANGE_MAX];
static size_t entry_capture_ranges;
static uint32_t entry_capture_seen;

bool xmv_original_set_entry_capture(const char *directory, const uint32_t *first, const uint32_t *last,
                                    size_t count)
{
    if (directory != NULL && (first == NULL || last == NULL || count == 0u || count > XMV_CAPTURE_RANGE_MAX)) {
        return false;
    }
    pthread_mutex_lock(&configuration_lock);
    entry_capture_directory = directory;
    entry_capture_ranges = directory != NULL ? count : 0u;
    for (size_t i = 0u; i < entry_capture_ranges; i++) {
        entry_capture_first[i] = first[i];
        entry_capture_last[i] = last[i];
    }
    entry_capture_seen = 0u;
    pthread_mutex_unlock(&configuration_lock);
    return true;
}

typedef struct {
    uint32_t base;
    uint32_t size;
    uint32_t permissions;
} capture_mapping;

/* The run of adjacent readable host mappings below 4 GB that holds `address` (guest memory is the
 * identity mapped low 4 GB). Permissions are the union of read and write over the run. */
static bool capture_run_of(uint32_t address, capture_mapping *out)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps == NULL) return false;
    char line[512];
    uint64_t run_start = 0u, run_end = 0u, previous_end = 0u;
    uint32_t permissions = 0u;
    bool in_run = false, found = false;
    while (fgets(line, sizeof line, maps) != NULL) {
        unsigned long long start = 0u, end = 0u;
        char flags[8] = "";
        if (sscanf(line, "%llx-%llx %7s", &start, &end, flags) != 3) continue;
        const bool readable = flags[0] == 'r' && end <= (1ull << 32);
        if (!readable || (in_run && start != previous_end)) {
            if (in_run && found) break;
            in_run = false;
            permissions = 0u;
        }
        if (!readable) continue;
        if (!in_run) {
            in_run = true;
            run_start = start;
            permissions = 0u;
        }
        permissions |= 1u | (flags[1] == 'w' ? 2u : 0u);
        run_end = end;
        previous_end = end;
        if (address >= start && address < end) found = true;
    }
    fclose(maps);
    if (!found) return false;
    out->base = (uint32_t)run_start;
    out->size = (uint32_t)(run_end - run_start);
    out->permissions = permissions;
    return true;
}

static bool capture_add(capture_mapping *list, size_t *count, uint32_t address)
{
    capture_mapping run;
    if (address < 0x10000u || !capture_run_of(address, &run)) return true; /* not a guest pointer */
    for (size_t i = 0u; i < *count; i++)
        if (list[i].base == run.base) return true;
    if (*count == CAPTURE_MAX_MAPPINGS) return false;
    list[(*count)++] = run;
    return true;
}

static bool capture_write_all(FILE *file, const void *bytes, size_t length)
{
    return fwrite(bytes, 1u, length, file) == length;
}

/* The mappings a capture holds: the image, the stack, the decoder object and everything its 0x168 bytes point at. */
static bool capture_gather(uint32_t *decoder_out, capture_mapping *list, size_t *count_out, uint64_t *total_out)
{
    uint32_t decoder = 0u;
    uint8_t object[CAPTURE_DECODER_BYTES];
    size_t count = 0u;
    bool good = kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 4u), &decoder) && decoder != 0u &&
                kernel_guest_read_bytes((kernel_guest_ptr)decoder, object, sizeof object) &&
                capture_add(list, &count, XMV_CAPTURE_ENTRY) && capture_add(list, &count, g_esp) &&
                capture_add(list, &count, decoder);
    for (size_t offset = 0u; good && offset + 4u <= sizeof object; offset += 4u) {
        uint32_t word;
        memcpy(&word, object + offset, 4u);
        good = capture_add(list, &count, word);
    }
    uint64_t total = 0u;
    for (size_t i = 0u; good && i < count; i++) total += list[i].size;
    *decoder_out = decoder;
    *count_out = count;
    *total_out = total;
    return good;
}

static void capture_entry(uint32_t ordinal)
{
    char path[512], partial[520];
    snprintf(path, sizeof path, "%s/entry_%05u.bin", entry_capture_directory, ordinal);
    snprintf(partial, sizeof partial, "%s.part", path);
    uint32_t decoder = 0u;
    capture_mapping list[CAPTURE_MAX_MAPPINGS];
    size_t count = 0u;
    uint64_t total = 0u;
    bool good = capture_gather(&decoder, list, &count, &total);
    if (!good || total > CAPTURE_MAX_BYTES) {
        fprintf(stderr, "xmv-original: entry capture %u skipped, %s (%llu bytes over %zu mappings)\n", ordinal,
                good ? "the mappings exceed the byte bound" : "the decoder object or mappings are unreadable",
                (unsigned long long)total, count);
        return;
    }
    FILE *file = fopen(partial, "wb");
    if (file == NULL) {
        fprintf(stderr, "xmv-original: entry capture cannot open %s\n", partial);
        return;
    }
    uint8_t header[328];
    memset(header, 0, sizeof header);
    memcpy(header, "TS538A01", 8u);
    if (recomp_callback_machine_snapshot == NULL) {
        (void)fclose(file);
        (void)remove(partial);
        fprintf(stderr, "xmv-original: entry capture %u skipped, no lifted runtime to read the registers from\n", ordinal);
        return;
    }
    recomp_machine_snapshot machine;
    recomp_callback_machine_snapshot(&machine);
    const uint32_t registers[10] = {XMV_CAPTURE_ENTRY, machine.registers[0], machine.registers[1],
                                    machine.registers[2], machine.registers[3], machine.registers[4],
                                    machine.registers[5], machine.registers[6], machine.registers[7], 0u};
    memcpy(header + 8, registers, sizeof registers);
    memcpy(header + 48, machine.mmx, sizeof machine.mmx);
    memcpy(header + 112, machine.xmm, sizeof machine.xmm);
    memcpy(header + 240, machine.x87, sizeof machine.x87);
    const uint32_t floating[4] = {machine.x87_top, machine.x87_control, machine.x87_compare,
                                  machine.x87_condition};
    memcpy(header + 304, floating, sizeof floating);
    const uint32_t mxcsr = 0x1F80u, mappings = (uint32_t)count;
    memcpy(header + 320, &mxcsr, 4u);
    memcpy(header + 324, &mappings, 4u);
    good = capture_write_all(file, header, sizeof header);
    uint8_t *chunk = malloc(1u << 20);
    good = good && chunk != NULL;
    for (size_t i = 0u; good && i < count; i++) {
        good = capture_write_all(file, &list[i], sizeof list[i]);
        for (uint32_t done = 0u; good && done < list[i].size; done += 1u << 20) {
            const uint32_t length = list[i].size - done < (1u << 20) ? list[i].size - done : 1u << 20;
            good = kernel_guest_read_bytes((kernel_guest_ptr)(list[i].base + done), chunk, length) &&
                   capture_write_all(file, chunk, length);
        }
    }
    free(chunk);
    good = (fclose(file) == 0) && good && rename(partial, path) == 0;
    if (!good) {
        (void)remove(partial);
        fprintf(stderr, "xmv-original: entry capture %u failed (a mapping was unreadable or %s is unwritable)\n",
                ordinal, path);
        return;
    }
    fprintf(stderr, "xmv-original: entry capture %u written %s mappings=%zu bytes=%llu decoder=0x%08X esp=0x%08X\n",
            ordinal, path, count, (unsigned long long)total, decoder, g_esp);
}

/* T611: the seeded native run, see xmv_seed_patch.h and tools/diagnostics/replay_xmv_seeded.py. */
static bool seed_enabled;
static uint32_t seed_entry;
static const char *seed_result;
static xmv_seed_patch seed_patches[XMV_SEED_PATCH_MAX];
static size_t seed_patch_count;
static uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, size_t length);

bool xmv_original_set_seed(bool enabled, uint32_t entry, const char *result, const char *const *patches,
                           size_t count)
{
    if (enabled && (result == NULL || count > XMV_SEED_PATCH_MAX || (count != 0u && patches == NULL))) return false;
    xmv_seed_patch parsed[XMV_SEED_PATCH_MAX];
    for (size_t i = 0u; enabled && i < count; i++)
        if (!xmv_seed_patch_parse(patches[i], &parsed[i])) return false;
    pthread_mutex_lock(&configuration_lock);
    seed_enabled = enabled;
    seed_entry = entry;
    seed_result = enabled ? result : NULL;
    seed_patch_count = enabled ? count : 0u;
    if (enabled) memcpy(seed_patches, parsed, count * sizeof parsed[0]);
    pthread_mutex_unlock(&configuration_lock);
    return true;
}

static void seed_fail(const char *what)
{
    fprintf(stderr, "xmv-original: seeded run failed, %s\n", what);
    fflush(NULL);
    _exit(3);
}

static void seed_write_u32(FILE *file, uint32_t value)
{
    if (fwrite(&value, 4u, 1u, file) != 1u) seed_fail("the result file is unwritable");
}

/* Apply the edits, run the lifted wrapper on the state they make, write every 4 KiB page of the captured
 * mappings the run changed ("TS611R01": the entry number, the CRC-32 of the edited entry state, EAX..ESP after
 * the return, then per mapping its base, size and changed pages), and end the process: the state is spent. */
static void seed_run(uint32_t ordinal)
{
    uint32_t decoder = 0u;
    capture_mapping list[CAPTURE_MAX_MAPPINGS];
    size_t count = 0u;
    uint64_t total = 0u;
    if (!capture_gather(&decoder, list, &count, &total) || total > CAPTURE_MAX_BYTES || count == 0u)
        seed_fail("the decoder object or its mappings are unreadable or too large");
    if (sub_00447E5E == NULL || recomp_callback_machine_snapshot == NULL) seed_fail("no lifted runtime");
    const uint32_t entry_esp = g_esp;
    for (size_t i = 0u; i < seed_patch_count; i++) {
        const xmv_seed_patch *patch = &seed_patches[i];
        uint32_t base = patch->base == XMV_SEED_ESP ? entry_esp : decoder;
        if (patch->base == XMV_SEED_DEC_POINTER &&
            !kernel_guest_read_u32((kernel_guest_ptr)(decoder + patch->pointer_offset), &base))
            seed_fail("a patch pointer is unreadable");
        if (!kernel_guest_write_bytes((kernel_guest_ptr)(base + patch->offset), patch->bytes, patch->length))
            seed_fail("a patch target is unwritable");
    }
    uint8_t *before = malloc((size_t)total);
    if (before == NULL) seed_fail("out of memory");
    size_t at = 0u;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0u; i < count; at += list[i].size, i++) {
        if (!kernel_guest_read_bytes((kernel_guest_ptr)list[i].base, before + at, list[i].size))
            seed_fail("a mapping is unreadable");
        crc = crc32_update(crc, before + at, list[i].size);
    }
    crc ^= 0xFFFFFFFFu;
    fprintf(stderr, "xmv-original: seeded run of entry %u, %zu patches, %zu mappings, %llu bytes, crc %08X\n",
            ordinal, seed_patch_count, count, (unsigned long long)total, crc);
    /* sub_00447E5E ends in `ret 4` and so leaves ESP at entry + 8. */
    sub_00447E5E();
    recomp_machine_snapshot after;
    recomp_callback_machine_snapshot(&after);
    FILE *file = fopen(seed_result, "wb");
    if (file == NULL) seed_fail("the result file cannot be opened");
    if (fwrite("TS611R01", 8u, 1u, file) != 1u) seed_fail("the result file is unwritable");
    seed_write_u32(file, ordinal);
    seed_write_u32(file, crc);
    for (unsigned r = 0u; r < 8u; r++) seed_write_u32(file, after.registers[r]);
    seed_write_u32(file, (uint32_t)count);
    for (size_t i = 0u, offset = 0u; i < count; offset += list[i].size, i++) {
        const uint32_t pages = list[i].size / 4096u;
        uint32_t changed = 0u;
        /* Two passes over the mapping: count the changed pages, then write them. */
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                seed_write_u32(file, list[i].base);
                seed_write_u32(file, list[i].size);
                seed_write_u32(file, changed);
            }
            for (uint32_t page = 0u; page < pages; page++) {
                uint8_t now[4096];
                if (!kernel_guest_read_bytes((kernel_guest_ptr)(list[i].base + page * 4096u), now, sizeof now))
                    seed_fail("a mapping is unreadable after the run");
                if (memcmp(now, before + offset + (size_t)page * 4096u, sizeof now) == 0) continue;
                if (pass == 0) {
                    changed++;
                } else {
                    seed_write_u32(file, page);
                    if (fwrite(now, sizeof now, 1u, file) != 1u) seed_fail("the result file is unwritable");
                }
            }
        }
    }
    if (fclose(file) != 0) seed_fail("the result file did not close");
    free(before);
    fprintf(stderr, "xmv-original: seeded run of entry %u done, wrote %s\n", ordinal, seed_result);
    fflush(NULL);
    _exit(0);
}

void xmv_original_note_call(uint32_t callee)
{
    if (callee != XMV_CAPTURE_ENTRY) return;
    const uint32_t ordinal = entry_capture_seen++;
    /* T630: the codec entry count, the number of frames the decoder body actually ran for. */
    if (trace) fprintf(stderr, "xmv-original: codec entry %u\n", ordinal);
    if (entry_capture_directory == NULL && !seed_enabled) return;
    for (size_t i = 0u; entry_capture_directory != NULL && i < entry_capture_ranges; i++)
        if (ordinal >= entry_capture_first[i] && ordinal <= entry_capture_last[i]) {
            capture_entry(ordinal);
            break;
        }
    if (seed_enabled && ordinal == seed_entry) seed_run(ordinal);
}

/* CRC-32 (IEEE, the zlib one) so the independent decoder side can match it with zlib.crc32. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, size_t length)
{
    static uint32_t table[256];
    static bool ready;
    if (!ready) {
        for (uint32_t n = 0u; n < 256u; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = (c & 1u) != 0u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    for (size_t i = 0u; i < length; i++) crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

/* Decoder object fields (docs/xmv-contracts.md section 12.1): macroblock columns and rows and the
 * three planes, Y 16*cols by 16*rows, U and V 8*cols by 8*rows. */
static void dump_frame(uint32_t decoder, uint32_t surface)
{
    uint32_t cols = 0u, rows = 0u, plane[3] = {0u, 0u, 0u};
    if (!kernel_guest_read_u32((kernel_guest_ptr)(decoder + 0xDCu), &cols) ||
        !kernel_guest_read_u32((kernel_guest_ptr)(decoder + 0xE0u), &rows) ||
        !kernel_guest_read_u32((kernel_guest_ptr)(decoder + 0xECu), &plane[0]) ||
        !kernel_guest_read_u32((kernel_guest_ptr)(decoder + 0xF0u), &plane[1]) ||
        !kernel_guest_read_u32((kernel_guest_ptr)(decoder + 0xF4u), &plane[2]) || cols == 0u ||
        rows == 0u || cols > 256u || rows > 256u) {
        fprintf(stderr, "xmv-original: frame dump skipped, decoder planes unreadable\n");
        return;
    }
    const uint32_t size[3] = {256u * cols * rows, 64u * cols * rows, 64u * cols * rows};
    if (frame_dump_buffer == NULL || size[0] > FRAME_DUMP_BUFFER_BYTES) {
        fprintf(stderr, "xmv-original: frame dump skipped, the frame does not fit the dump buffer\n");
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%05u.yuv", frame_dump_directory, frames_dumped);
    const bool write_file = frame_dump_maximum == 0u || frames_dumped < frame_dump_maximum;
    FILE *file = write_file ? fopen(path, "wb") : NULL;
    if (write_file && file == NULL) {
        fprintf(stderr, "xmv-original: frame dump cannot open %s\n", path);
        return;
    }
    uint32_t crc = 0xFFFFFFFFu;
    bool ok = true;
    for (unsigned index = 0u; index < 3u && ok; index++) {
        uint8_t *bytes = frame_dump_buffer;
        ok = kernel_guest_read_bytes((kernel_guest_ptr)plane[index], bytes, size[index]) &&
             (file == NULL || fwrite(bytes, 1u, size[index], file) == size[index]);
        if (ok) crc = crc32_update(crc, bytes, size[index]);
    }
    if (file != NULL) ok = fclose(file) == 0 && ok;
    /* The converted picture the title hands to UpdateOverlay: the surface's Data word and the pitch
     * from its Size word (the same arithmetic as the overlay port), width * 2 bytes of each row. */
    uint32_t data = 0u, size_word = 0u, width = 0u, height = 0u;
    (void)kernel_guest_read_u32((kernel_guest_ptr)(surface + 4u), &data);
    (void)kernel_guest_read_u32((kernel_guest_ptr)(surface + 0x10u), &size_word);
    (void)kernel_guest_read_u32((kernel_guest_ptr)(decoder + 0x40u), &width);
    (void)kernel_guest_read_u32((kernel_guest_ptr)(decoder + 0x44u), &height);
    const uint32_t pitch = (((size_word >> 24) & 0xFFu) + 1u) << 6;
    const kernel_guest_ptr virtual_data = data_resolver != NULL ? data_resolver(data) : 0u;
    if (write_file && virtual_data != 0u && size_word != 0u && width != 0u && width <= 2048u && height <= 2048u &&
        2u * width <= pitch &&
        2u * width * height <= FRAME_DUMP_BUFFER_BYTES) {
        snprintf(path, sizeof(path), "%s/frame_%05u.yuy2", frame_dump_directory, frames_dumped);
        FILE *picture = fopen(path, "wb");
        bool good = picture != NULL;
        for (uint32_t row = 0u; good && row < height; row++)
            good = kernel_guest_read_bytes((kernel_guest_ptr)(virtual_data + row * pitch), frame_dump_buffer, 2u * width) &&
                   fwrite(frame_dump_buffer, 1u, 2u * width, picture) == 2u * width;
        if (picture != NULL) good = fclose(picture) == 0 && good;
        if (!good) fprintf(stderr, "xmv-original: surface picture of frame %u could not be written\n", frames_dumped);
    }
    fprintf(stderr, "xmv-original: frame %u %ux%u macroblocks crc32=%08X%s\n", frames_dumped, cols, rows,
            ~crc, ok ? "" : " WRITE FAILED");
    frames_dumped++;
}

uint32_t xmv_original_address(size_t index)
{
    return index < XMV_ORIGINAL_EXPORT_COUNT ? exports[index] : 0u;
}

static void release_call(void)
{
    pthread_mutex_lock(&configuration_lock);
    if (active_calls == 0u) {
        abort();
    }
    active_calls--;
    pthread_mutex_unlock(&configuration_lock);
}

static void trace_entry(size_t index)
{
    uint32_t caller = 0u;
    (void)kernel_guest_read_u32((kernel_guest_ptr)g_esp, &caller);
    fprintf(stderr, "xmv-original: %s enter caller=0x%08X ecx=0x%08X args=", export_names[index],
            caller, g_ecx);
    for (uint32_t i = 0u; i < export_args[index]; i++) {
        uint32_t word = 0u;
        (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 4u + 4u * i), &word);
        fprintf(stderr, "%s0x%X", i ? "," : "", word);
    }
    fputc('\n', stderr);
    if (index == 0u) {
        /* CreateDecoderForFile: the movie path the title built, up to 127 characters. */
        uint32_t name = 0u;
        char text[128];
        size_t length = 0u;
        (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 8u), &name);
        while (length + 1u < sizeof(text)) {
            uint8_t byte = 0u;
            if (!kernel_guest_read_u8((kernel_guest_ptr)(name + (uint32_t)length), &byte) ||
                byte == 0u) {
                break;
            }
            text[length++] = (char)byte;
        }
        text[length] = '\0';
        fprintf(stderr, "xmv-original: movie path \"%s\"\n", text);
        if (caller == 0x000302F4u) {
            /* T636, trace only: the title's movie routine sub_00030280 owns this call. Its own frame (0x274 bytes of
             * locals, three saved registers, the three pushed arguments and this return address) puts ITS return
             * address 0x290 above the call's, then the two arguments (name, skippable). The title's front end state
             * [0x78AD2C] and the two display aspect words the title tests with sub_0003C420 (16:9 only for 16 and 9)
             * [0x4C0458] and [0x4C045C] are read, never written. */
            uint32_t routine_caller = 0u, routine_name = 0u, routine_skippable = 0u, state = 0u, aspect_width = 0u, aspect_height = 0u;
            (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 0x290u), &routine_caller);
            (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 0x294u), &routine_name);
            (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 0x298u), &routine_skippable);
            (void)kernel_guest_read_u32((kernel_guest_ptr)0x0078AD2Cu, &state);
            (void)kernel_guest_read_u32((kernel_guest_ptr)0x004C0458u, &aspect_width);
            (void)kernel_guest_read_u32((kernel_guest_ptr)0x004C045Cu, &aspect_height);
            fprintf(stderr,
                    "xmv-original: title movie routine 0x30280 called from 0x%08X name=0x%08X skippable=%u front end state [0x78AD2C]=%u display aspect [0x4C0458]:[0x4C045C]=%u:%u\n",
                    routine_caller, routine_name, routine_skippable, state, aspect_width, aspect_height);
        }
    }
    if (index == 3u) {
        /* EnableAudioStream: the 20-byte audio stream descriptor it is about to read. */
        uint32_t decoder = 0u, stream_index = 0u, table = 0u;
        (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 4u), &decoder);
        (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 8u), &stream_index);
        (void)kernel_guest_read_u32((kernel_guest_ptr)(decoder + 0x4Cu), &table);
        fprintf(stderr, "xmv-original: audio descriptor[%u] =", stream_index);
        for (uint32_t i = 0u; i < 5u; i++) {
            uint32_t word = 0u;
            (void)kernel_guest_read_u32((kernel_guest_ptr)(table + stream_index * 20u + 4u * i), &word);
            fprintf(stderr, " %08X", word);
        }
        fputc('\n', stderr);
    }
}

/* Trace only: the decoder object CreateDecoderForFile published, as raw dwords. */
static void trace_decoder(uint32_t out_pointer)
{
    uint32_t decoder = 0u;
    if (!kernel_guest_read_u32((kernel_guest_ptr)out_pointer, &decoder) || decoder == 0u) {
        fprintf(stderr, "xmv-original: decoder output is %s\n", decoder == 0u ? "NULL" : "unreadable");
        return;
    }
    fprintf(stderr, "xmv-original: decoder=0x%08X", decoder);
    for (uint32_t offset = 0u; offset < 0x168u; offset += 4u) {
        uint32_t word = 0u;
        (void)kernel_guest_read_u32((kernel_guest_ptr)(decoder + offset), &word);
        fprintf(stderr, "%s%03X:%08X", (offset % 32u) == 0u ? "\n  " : " ", offset, word);
    }
    fputc('\n', stderr);
}

bool xmv_original_dispatch(uint32_t address)
{
    size_t index = 0u;
    while (index < XMV_ORIGINAL_EXPORT_COUNT && exports[index] != address) {
        index++;
    }
    if (index == XMV_ORIGINAL_EXPORT_COUNT) {
        return false;
    }
    pthread_mutex_lock(&configuration_lock);
    if (!configured) {
        pthread_mutex_unlock(&configuration_lock);
        return false;
    }
    if (active_calls == SIZE_MAX) {
        pthread_mutex_unlock(&configuration_lock);
        abort();
    }
    const recomp_func_t function = functions[index];
    const bool tracing = trace;
    const bool terminate_intro = skip_intro && index == 6u;
    active_calls++;
    pthread_mutex_unlock(&configuration_lock);

    if (!host_run_armed()) {
        release_call();
        abort();
    }
    volatile host_run_scope scope = HOST_RUN_SCOPE_INITIALIZER;
    if (!host_run_scope_init(&scope)) {
        release_call();
        abort();
    }
    if (sigsetjmp(*host_run_scope_jmp(&scope), 0) == 0) {
        if (!host_run_scope_push(&scope)) {
            release_call();
            host_run_stop(HOST_STOP_UNIMPLEMENTED, address, 0u,
                          "original XMV stop scope unavailable");
            abort();
        }
        if (index == 0u && substitute_from[0] != '\0') {
            uint32_t name_pointer = 0u;
            if (kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 8u), &name_pointer)) substitute_movie(name_pointer);
        }
        if (tracing) {
            trace_entry(index);
        }
        uint32_t first_argument = 0u, output_argument = 0u;
        if (tracing && index == 0u) {
            (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 12u), &output_argument);
        }
        /* GetNextFrame: the result word the caller reads (1 frame, 2 end, 3 error), T394. */
        uint32_t result_pointer = 0u, decoder_argument = 0u, surface_argument = 0u;
        if (tracing && index == 6u) {
            (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 4u), &decoder_argument);
            (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 8u), &surface_argument);
            (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 12u), &result_pointer);
        }
        (void)first_argument;
        if (terminate_intro && startup_frame()) {
            /* The original leaf consumes the same decoder argument as GetNextFrame.
             * Execute real TerminatePlayback, then restore its scratch EAX/ESP so
             * the original GetNextFrame and title cleanup run on the existing frame.
             * No synthetic result, pad state, clock or decoder-success publication. */
            const uint32_t saved_esp = g_esp, saved_eax = g_eax;
            functions[5]();
            g_esp = saved_esp;
            g_eax = saved_eax;
            fprintf(stderr, "xmv-original: FABRICATED --skip-intro requested original startup termination\n");
        }
        function();
        if (tracing) {
            fprintf(stderr, "xmv-original: %s return eax=0x%08X", export_names[index], g_eax);
            uint32_t result = 0u;
            if (index == 6u) {
                /* GetNextFrame only, the other lines stay as they were (tests compare them). */
                fprintf(stderr, " tick=%llu", (unsigned long long)kernel_clock_peek());
                if (kernel_guest_read_u32((kernel_guest_ptr)result_pointer, &result)) {
                    fprintf(stderr, " result=%u", result);
                }
            }
            fputc('\n', stderr);
            if (index == 6u && result == 1u && frame_dump_directory != NULL) {
                dump_frame(decoder_argument, surface_argument);
            }
            if (index == 0u) {
                trace_decoder(output_argument);
            }
        }
        if (!host_run_scope_pop(&scope)) {
            abort();
        }
        release_call();
        return true;
    }
    if (!host_run_scope_pop(&scope)) {
        abort();
    }
    release_call();
    host_run_rethrow(host_run_result());
}
