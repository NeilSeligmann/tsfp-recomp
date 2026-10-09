/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Report what a guest executable's kernel imports still need.
 *
 * Point this at a user-supplied XBE and it prints the HLE backlog: every ordinal
 * the guest imports that has no implementation, named, busiest first. That is the
 * work queue for the kernel layer, derived from the binary rather than guessed.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel_hle.h"
#include "kernel_register_all.h"
#include "xbe.h"

/* The HLE logs diagnostics to stderr, which is right at runtime. For this tool
 * the report IS the output, so send it to stdout where it can be piped. */
static int stdout_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vfprintf(stdout, format, args);
    va_end(args);
    return written;
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *handle = fopen(path, "rb");
    if (!handle) {
        return NULL;
    }
    if (fseek(handle, 0, SEEK_END) != 0) {
        fclose(handle);
        return NULL;
    }
    long size = ftell(handle);
    if (size <= 0) {
        fclose(handle);
        return NULL;
    }
    rewind(handle);
    uint8_t *buffer = malloc((size_t)size);
    if (!buffer) {
        fclose(handle);
        return NULL;
    }
    size_t got = fread(buffer, 1, (size_t)size, handle);
    fclose(handle);
    if (got != (size_t)size) {
        free(buffer);
        return NULL;
    }
    *out_len = got;
    return buffer;
}

/*
 * Print the registered ordinals as a comma-separated list, nothing else.
 *
 * This is the SOURCE OF TRUTH for "which ordinals are implemented". The call-site queue
 * (`ordinal_callsites.json`) used to take that from a hand-typed `--implemented` argument,
 * which was wrong for ordinal 49 (bound in kernel_hal.c) and let three different figures
 * circulate on the same day: 43, 47 and 54. Deriving it from the registered handlers makes
 * the flag a measurement. Feed it straight in:
 *
 *     tools.lift.callsites --implemented "$(hle_report --implemented-ordinals)"
 */
static int print_implemented_ordinals(void)
{
    kernel_hle_init();
    (void)kernel_register_all();
    const char *separator = "";
    for (unsigned ordinal = 1u; ordinal <= XBOX_KERNEL_ORDINAL_MAX; ordinal++) {
        const kernel_entry *entry = kernel_hle_entry(ordinal);
        if (entry && entry->state == KERNEL_ENTRY_IMPLEMENTED) {
            printf("%s%u", separator, ordinal);
            separator = ",";
        }
    }
    printf("\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--implemented-ordinals") == 0) {
        return print_implemented_ordinals();
    }
    if (argc < 2) {
        fprintf(stderr, "usage: hle_report <file.xbe> | hle_report --implemented-ordinals\n");
        return 2;
    }

    size_t len = 0;
    uint8_t *data = read_file(argv[1], &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    xbe_image image;
    xbe_status status = xbe_parse(data, len, &image);
    if (status != XBE_OK) {
        fprintf(stderr, "parse failed: %s\n", xbe_status_str(status));
        free(data);
        return 1;
    }

    printf("image          %s\n", argv[1]);
    printf("kernel imports %u\n", image.kernel_import_count);
    printf("known ordinals %zu\n\n", xbox_kernel_ordinal_count());

    unsigned ordinals[XBE_MAX_KERNEL_IMPORTS];
    for (uint32_t i = 0; i < image.kernel_import_count; i++) {
        ordinals[i] = image.kernel_imports[i];
    }

    kernel_hle_init();
    /*
     * REGISTER EVERY MODULE FIRST, through the one shared list. Without registration the
     * tool reported the ENTIRE import list as backlog, so its figure was a constant dressed
     * as a measurement. See kernel_register_all.h for why the list is not duplicated here.
     */
    const size_t registered = kernel_register_all();

    kernel_hle_set_log(stdout_printer);
    printf("hle registered %zu ordinals\n", registered);
    kernel_hle_report_missing(ordinals, image.kernel_import_count);

    free(data);
    return 0;
}
