/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Dump a parsed XBE in a stable, diffable form.
 *
 * This exists primarily as a cross-validation oracle: the Python parser in
 * tools/xbe and this C loader are independent implementations of the same
 * format, and they must agree field-for-field on a real executable. Two
 * implementations agreeing is far stronger evidence than either one passing its
 * own tests, because a misread of the format would have to be made twice.
 */

#include "xbe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (size < 0) {
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

int main(int argc, char **argv)
{
    bool do_map = false;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--map") == 0) {
            do_map = true;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        fprintf(stderr, "usage: xbe_dump [--map] <file.xbe>\n");
        return 2;
    }

    size_t len = 0;
    uint8_t *data = read_file(path, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    xbe_image image;
    xbe_status status = xbe_parse(data, len, &image);
    if (status != XBE_OK) {
        fprintf(stderr, "parse failed: %s\n", xbe_status_str(status));
        free(data);
        return 1;
    }

    printf("file_size %zu\n", len);
    printf("base_address %#x\n", image.base_address);
    printf("size_of_image %#x\n", image.size_of_image);
    printf("size_of_headers %#x\n", image.size_of_headers);
    printf("entry_point %#x\n", image.entry_point);
    printf("kernel_thunk_addr %#x\n", image.kernel_thunk_addr);
    printf("is_retail %d\n", image.is_retail ? 1 : 0);
    printf("title_id %#x\n", image.title_id);
    printf("section_count %u\n", image.section_count);
    for (uint32_t i = 0; i < image.section_count; i++) {
        const xbe_section *s = &image.sections[i];
        printf("section %s %#x %#x %#x %#x %#x\n", s->name, s->flags, s->virtual_addr,
               s->virtual_size, s->raw_addr, s->raw_size);
    }

    if (do_map) {
        status = xbe_map(&image, data, len);
        if (status != XBE_OK) {
            fprintf(stderr, "map failed: %s\n", xbe_status_str(status));
            free(data);
            return 1;
        }
        printf("mapped 1 length %#zx\n", image.mapped_length);
        /* Read the magic back through the mapping to prove it is live. */
        const uint32_t *magic = xbe_at(&image, image.base_address, 4);
        printf("mapped_magic %#x\n", magic ? *magic : 0u);
        /* And a byte from the entry point, which must be real code. */
        const uint8_t *entry = xbe_at(&image, image.entry_point, 1);
        printf("entry_first_byte %#x\n", entry ? *entry : 0u);
        xbe_unmap(&image);
    }

    free(data);
    return 0;
}
