/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See xbe.h for the memory model and the PIE requirement.
 */

#include "xbe.h"

#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Header field offsets, measured from the retail TSFP XBE. */
#define OFF_BASE_ADDRESS 0x104
#define OFF_SIZE_OF_HEADERS 0x108
#define OFF_SIZE_OF_IMAGE 0x10C
#define OFF_CERTIFICATE_ADDR 0x118
#define OFF_NUMBER_OF_SECTIONS 0x11C
#define OFF_SECTION_HEADERS_ADDR 0x120
#define OFF_ENTRY_POINT 0x128
#define OFF_KERNEL_THUNK_ADDR 0x158
#define HEADER_MIN_SIZE 0x17C

#define SECTION_HEADER_SIZE 0x38
#define CERT_OFF_TITLE_ID 0x08
#define CERT_MIN_SIZE 0xB0

#define PAGE_SIZE_FALLBACK 0x1000u

static _Atomic uint64_t mapped_image_bytes;

uint64_t xbe_mapped_bytes(void)
{
    return atomic_load_explicit(&mapped_image_bytes, memory_order_relaxed);
}

const char *xbe_status_str(xbe_status status)
{
    switch (status) {
    case XBE_OK:
        return "ok";
    case XBE_ERR_TRUNCATED:
        return "truncated: a structure referenced bytes past the end of the file";
    case XBE_ERR_BAD_MAGIC:
        return "bad magic: not an XBE image";
    case XBE_ERR_TOO_MANY_SECTIONS:
        return "too many sections for the fixed table";
    case XBE_ERR_MAP_FAILED:
        return "mmap failed: could not reserve the guest address range";
    case XBE_ERR_NOT_PIE:
        return "host is not position-independent, so low memory is occupied";
    case XBE_ERR_RANGE:
        return "address outside the mapped image";
    }
    return "unknown";
}

/* Bounds-checked little-endian reads. Nothing in the file is trusted. */
static bool read_u32(const uint8_t *data, size_t len, size_t offset, uint32_t *out)
{
    if (offset + 4 > len) {
        return false;
    }
    *out = (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |
           ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
    return true;
}

static void copy_cstring(char *dest, size_t dest_size, const uint8_t *data, size_t len,
                         size_t offset)
{
    size_t i = 0;
    while (i + 1 < dest_size && offset + i < len && data[offset + i] != 0) {
        dest[i] = (char)data[offset + i];
        i++;
    }
    dest[i] = '\0';
}

static uint32_t decode_xored(uint32_t raw, uint32_t retail_key, uint32_t debug_key,
                             uint32_t base, uint32_t size, bool *is_retail)
{
    uint32_t retail = raw ^ retail_key;
    if (retail >= base && retail < base + size) {
        if (is_retail) {
            *is_retail = true;
        }
        return retail;
    }
    uint32_t debug = raw ^ debug_key;
    if (debug >= base && debug < base + size) {
        if (is_retail) {
            *is_retail = false;
        }
        return debug;
    }
    /* Neither fits. Return the retail decode so callers get a deterministic
     * answer for a malformed image rather than undefined behaviour. */
    if (is_retail) {
        *is_retail = true;
    }
    return retail;
}

/* Read the NUL-terminated kernel thunk table. Each entry is ordinal | 0x80000000,
 * and the table is addressed virtually, so it must be located through a section. */
static void read_kernel_imports(const uint8_t *data, size_t len, xbe_image *out)
{
    out->kernel_import_count = 0;
    uint32_t va = out->kernel_thunk_addr;
    size_t offset = 0;
    bool located = false;
    for (uint32_t i = 0; i < out->section_count; i++) {
        const xbe_section *section = &out->sections[i];
        if (va >= section->virtual_addr &&
            va < section->virtual_addr + section->virtual_size) {
            uint32_t delta = va - section->virtual_addr;
            if (delta < section->raw_size) {
                offset = (size_t)section->raw_addr + delta;
                located = true;
            }
            break;
        }
    }
    if (!located) {
        return;
    }
    while (out->kernel_import_count < XBE_MAX_KERNEL_IMPORTS) {
        uint32_t value = 0;
        if (!read_u32(data, len, offset, &value) || value == 0) {
            break;
        }
        out->kernel_imports[out->kernel_import_count++] = (uint16_t)(value & 0xFFFFu);
        offset += 4;
    }
}

xbe_status xbe_parse(const uint8_t *data, size_t len, xbe_image *out)
{
    if (!data || !out) {
        return XBE_ERR_TRUNCATED;
    }
    memset(out, 0, sizeof(*out));

    if (len < HEADER_MIN_SIZE) {
        return XBE_ERR_TRUNCATED;
    }
    uint32_t magic = 0;
    if (!read_u32(data, len, 0, &magic) || magic != XBE_MAGIC) {
        return XBE_ERR_BAD_MAGIC;
    }

    uint32_t base = 0, headers = 0, image_size = 0, count = 0, sec_va = 0, cert_va = 0;
    if (!read_u32(data, len, OFF_BASE_ADDRESS, &base) ||
        !read_u32(data, len, OFF_SIZE_OF_HEADERS, &headers) ||
        !read_u32(data, len, OFF_SIZE_OF_IMAGE, &image_size) ||
        !read_u32(data, len, OFF_NUMBER_OF_SECTIONS, &count) ||
        !read_u32(data, len, OFF_SECTION_HEADERS_ADDR, &sec_va) ||
        !read_u32(data, len, OFF_CERTIFICATE_ADDR, &cert_va)) {
        return XBE_ERR_TRUNCATED;
    }
    if (count > XBE_MAX_SECTIONS) {
        return XBE_ERR_TOO_MANY_SECTIONS;
    }

    out->base_address = base;
    out->size_of_headers = headers;
    out->size_of_image = image_size;
    out->section_count = count;

    /* Header-resident structures are addressed virtually but live at
     * (virtual address - base address) inside the header region. */
    if (sec_va < base || cert_va < base) {
        return XBE_ERR_TRUNCATED;
    }
    size_t sec_off = (size_t)(sec_va - base);
    if (sec_off + (size_t)SECTION_HEADER_SIZE * count > len) {
        return XBE_ERR_TRUNCATED;
    }

    for (uint32_t i = 0; i < count; i++) {
        size_t off = sec_off + (size_t)SECTION_HEADER_SIZE * i;
        uint32_t flags = 0, va = 0, vsize = 0, raw = 0, rsize = 0, name_va = 0;
        if (!read_u32(data, len, off + 0x00, &flags) ||
            !read_u32(data, len, off + 0x04, &va) ||
            !read_u32(data, len, off + 0x08, &vsize) ||
            !read_u32(data, len, off + 0x0C, &raw) ||
            !read_u32(data, len, off + 0x10, &rsize) ||
            !read_u32(data, len, off + 0x14, &name_va)) {
            return XBE_ERR_TRUNCATED;
        }
        xbe_section *section = &out->sections[i];
        section->flags = flags;
        section->virtual_addr = va;
        section->virtual_size = vsize;
        section->raw_addr = raw;
        section->raw_size = rsize;
        section->name[0] = '\0';
        if (name_va >= base) {
            copy_cstring(section->name, sizeof(section->name), data, len,
                         (size_t)(name_va - base));
        }
    }

    size_t cert_off = (size_t)(cert_va - base);
    if (cert_off + CERT_MIN_SIZE <= len) {
        (void)read_u32(data, len, cert_off + CERT_OFF_TITLE_ID, &out->title_id);
    }

    uint32_t raw_entry = 0, raw_thunk = 0;
    (void)read_u32(data, len, OFF_ENTRY_POINT, &raw_entry);
    (void)read_u32(data, len, OFF_KERNEL_THUNK_ADDR, &raw_thunk);
    out->entry_point = decode_xored(raw_entry, XBE_ENTRY_XOR_RETAIL, XBE_ENTRY_XOR_DEBUG,
                                    base, image_size, &out->is_retail);
    out->kernel_thunk_addr = decode_xored(raw_thunk, XBE_THUNK_XOR_RETAIL,
                                          XBE_THUNK_XOR_DEBUG, base, image_size, NULL);
    read_kernel_imports(data, len, out);
    return XBE_OK;
}

xbe_status xbe_check_pie(void)
{
    /* The question is not really "am I PIE" but "is the low guest range free".
     * Probe it directly: that is the property we actually depend on, and it
     * cannot be wrong the way an inference from the ELF type could be. */
    long page = sysconf(_SC_PAGESIZE);
    size_t probe_len = (page > 0) ? (size_t)page : PAGE_SIZE_FALLBACK;
    void *want = (void *)(uintptr_t)0x10000u;
    void *got = mmap(want, probe_len, PROT_READ,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == MAP_FAILED) {
        return XBE_ERR_NOT_PIE;
    }
    munmap(got, probe_len);
    return XBE_OK;
}

xbe_status xbe_map(xbe_image *image, const uint8_t *data, size_t len)
{
    if (!image || !data || image->mapped) {
        return XBE_ERR_RANGE;
    }

    long page = sysconf(_SC_PAGESIZE);
    size_t page_size = (page > 0) ? (size_t)page : PAGE_SIZE_FALLBACK;

    /* Round the mapping down to a page boundary and up to cover the image. */
    uintptr_t start = (uintptr_t)image->base_address & ~(uintptr_t)(page_size - 1);
    uintptr_t end = (uintptr_t)image->base_address + image->size_of_image;
    end = (end + page_size - 1) & ~(uintptr_t)(page_size - 1);
    size_t length = (size_t)(end - start);

    void *got = mmap((void *)start, length, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == MAP_FAILED) {
        /* EEXIST here almost always means a non-PIE host: its own image sits at
         * 0x400000, inside the guest range. Report that specifically. */
        return (errno == EEXIST) ? XBE_ERR_NOT_PIE : XBE_ERR_MAP_FAILED;
    }

    /* Headers first: the guest reads its own certificate and section table. */
    size_t header_bytes = image->size_of_headers;
    if (header_bytes > len) {
        header_bytes = len;
    }
    memcpy((void *)(uintptr_t)image->base_address, data, header_bytes);

    for (uint32_t i = 0; i < image->section_count; i++) {
        const xbe_section *section = &image->sections[i];
        if (section->raw_size == 0) {
            continue;
        }
        if ((size_t)section->raw_addr + section->raw_size > len) {
            munmap(got, length);
            return XBE_ERR_TRUNCATED;
        }
        if (!xbe_contains(image, section->virtual_addr)) {
            munmap(got, length);
            return XBE_ERR_RANGE;
        }
        memcpy((void *)(uintptr_t)section->virtual_addr, data + section->raw_addr,
               section->raw_size);
    }

    image->mapped = true;
    image->mapped_length = length;
    atomic_fetch_add_explicit(&mapped_image_bytes, length, memory_order_relaxed);
    return XBE_OK;
}

void xbe_unmap(xbe_image *image)
{
    if (!image || !image->mapped) {
        return;
    }
    long page = sysconf(_SC_PAGESIZE);
    size_t page_size = (page > 0) ? (size_t)page : PAGE_SIZE_FALLBACK;
    uintptr_t start = (uintptr_t)image->base_address & ~(uintptr_t)(page_size - 1);
    munmap((void *)start, image->mapped_length);
    image->mapped = false;
    atomic_fetch_sub_explicit(&mapped_image_bytes, image->mapped_length, memory_order_relaxed);
    image->mapped_length = 0;
}

const xbe_section *xbe_find_section(const xbe_image *image, const char *name)
{
    if (!image || !name) {
        return NULL;
    }
    for (uint32_t i = 0; i < image->section_count; i++) {
        if (strcmp(image->sections[i].name, name) == 0) {
            return &image->sections[i];
        }
    }
    return NULL;
}

bool xbe_contains(const xbe_image *image, xbe_guest_ptr addr)
{
    if (!image) {
        return false;
    }
    return addr >= image->base_address &&
           addr < image->base_address + image->size_of_image;
}

void *xbe_at(const xbe_image *image, xbe_guest_ptr addr, size_t length)
{
    if (!image || !image->mapped) {
        return NULL;
    }
    if (!xbe_contains(image, addr)) {
        return NULL;
    }
    /* Guard the end too: a length that runs past the image is a bug in the
     * caller, and returning a pointer anyway would hide it.
     * Overflow constraint: never form addr + length, since a length near
     * SIZE_MAX wraps that sum (even in 64 bits) back below the limit and the
     * guard would bless it. xbe_contains guarantees addr < limit, so
     * limit - addr is positive and cannot underflow: subtract on the known
     * larger side and compare the length against the room that remains. */
    uint64_t limit = (uint64_t)image->base_address + image->size_of_image;
    if ((uint64_t)length > limit - (uint64_t)addr) {
        return NULL;
    }
    return (void *)(uintptr_t)addr;
}
