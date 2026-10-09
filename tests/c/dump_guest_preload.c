/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LD_PRELOAD shim for the title-state differential (T443): at process exit it writes every readable
 * mapping below 4 GB (the guest's address space, the host maps the guest at its own addresses) to the
 * file named by TSFP_DUMP_GUEST, so a test can transplant the state the title reached at a
 * `--stop-after-calls` cut into the Unicorn oracle. Test tooling only, nothing in the host links it.
 *
 * Format: u32 count, then per mapping {u32 start, u32 size}, then the raw bytes of each in order.
 * The dump holds the user's title state (and so possibly disc-derived data), it belongs in tmp/ and
 * is never committed.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdarg.h>

#define MAX_MAPPINGS 4096u

static int dumped;

/* The dump runs when the host reports the stop, BEFORE its own teardown unmaps the guest, which an
 * exit handler or a destructor would run after. */
static void dump_guest(void)
{
    const char *path = getenv("TSFP_DUMP_GUEST");
    if (path == NULL || dumped) {
        return;
    }
    dumped = 1;
    static uint32_t starts[MAX_MAPPINGS];
    static uint32_t sizes[MAX_MAPPINGS];
    uint32_t count = 0u;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps == NULL) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), maps) != NULL && count < MAX_MAPPINGS) {
        unsigned long long low = 0u;
        unsigned long long high = 0u;
        char permissions[8] = {0};
        if (sscanf(line, "%llx-%llx %7s", &low, &high, permissions) != 3 || permissions[0] != 'r' ||
            high > 0x100000000ull) {
            continue;
        }
        starts[count] = (uint32_t)low;
        sizes[count] = (uint32_t)(high - low);
        count++;
    }
    fclose(maps);
    FILE *out = fopen(path, "wb");
    if (out == NULL) {
        return;
    }
    fwrite(&count, sizeof(count), 1u, out);
    for (uint32_t index = 0u; index < count; index++) {
        fwrite(&starts[index], sizeof(uint32_t), 1u, out);
        fwrite(&sizes[index], sizeof(uint32_t), 1u, out);
    }
    for (uint32_t index = 0u; index < count; index++) {
        fwrite((const void *)(uintptr_t)starts[index], 1u, sizes[index], out);
    }
    fclose(out);
}

/* The host prints its stop report before it tears the guest down. The first line of that report is
 * the trigger: it is printed with printf or compiled to puts, so both are interposed. */
#define TRIGGER "where each host thread stopped"

int puts(const char *text)
{
    if (strstr(text, TRIGGER) != NULL) {
        dump_guest();
    }
    return fputs(text, stdout) < 0 ? -1 : putchar('\n');
}

int printf(const char *format, ...)
{
    if (strstr(format, TRIGGER) != NULL) {
        dump_guest();
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = vprintf(format, arguments);
    va_end(arguments);
    return written;
}
