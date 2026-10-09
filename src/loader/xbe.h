/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XBE image loading: parse a user-supplied Xbox executable and map it at its
 * native virtual addresses.
 *
 * MEMORY MODEL. The host is a 64-bit process with the guest address space
 * identity-mapped into the low 4 GB. A 32-bit guest address therefore
 * zero-extends to a valid host address and translation costs nothing, while we
 * keep the 64-bit toolchain and unbounded allocation above 4 GB.
 *
 * This REQUIRES the host binary to be position-independent. A non-PIE
 * executable loads at 0x400000, which sits inside the guest range, and the
 * mapping fails with EEXIST. Verified both ways; the build enforces PIE and
 * xbe_check_pie() re-checks at runtime.
 *
 * DISCIPLINE. Pointer fields inside guest structs are four bytes. Declare them
 * as xbe_guest_ptr, never as a native pointer, or every guest struct layout
 * silently breaks on a 64-bit host. Storing a host pointer into guest-visible
 * storage truncates it and produces corruption far from its cause, so host state
 * attaches sidecar-style instead.
 */

#ifndef TSFP_LOADER_XBE_H
#define TSFP_LOADER_XBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A pointer as the guest sees it: four bytes, zero-extended to reach host memory. */
typedef uint32_t xbe_guest_ptr;

#define XBE_MAGIC 0x48454258u /* 'XBEH' little-endian */

#define XBE_SECTION_FLAG_WRITABLE 0x01u
#define XBE_SECTION_FLAG_PRELOAD 0x02u
#define XBE_SECTION_FLAG_EXECUTABLE 0x04u
#define XBE_SECTION_FLAG_INSERTED_FILE 0x08u

/* Entry point and kernel thunk are XOR-encoded; which key applies identifies
 * the build. Try retail first, then debug, and accept whichever decodes to an
 * address inside the image. */
#define XBE_ENTRY_XOR_RETAIL 0xA8FC57ABu
#define XBE_ENTRY_XOR_DEBUG 0x94859D4Bu
#define XBE_THUNK_XOR_RETAIL 0x5B6D40B6u
#define XBE_THUNK_XOR_DEBUG 0xEFB1F152u

#define XBE_MAX_SECTIONS 64
#define XBE_SECTION_NAME_MAX 32
/* TSFP imports 151; the real kernel exports 371. A generous cap avoids a
 * truncation that would look like a shorter import list rather than an error. */
#define XBE_MAX_KERNEL_IMPORTS 512

typedef enum {
    XBE_OK = 0,
    XBE_ERR_TRUNCATED,
    XBE_ERR_BAD_MAGIC,
    XBE_ERR_TOO_MANY_SECTIONS,
    XBE_ERR_MAP_FAILED,
    XBE_ERR_NOT_PIE,
    XBE_ERR_RANGE,
} xbe_status;

typedef struct {
    char name[XBE_SECTION_NAME_MAX];
    uint32_t flags;
    xbe_guest_ptr virtual_addr;
    uint32_t virtual_size;
    uint32_t raw_addr;
    uint32_t raw_size;
} xbe_section;

typedef struct {
    xbe_guest_ptr base_address;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    xbe_guest_ptr entry_point;
    xbe_guest_ptr kernel_thunk_addr;
    bool is_retail;
    uint32_t title_id;
    uint32_t section_count;
    xbe_section sections[XBE_MAX_SECTIONS];
    /* Kernel imports, by ordinal. The thunk table holds no names, so an ordinal
     * is all the guest tells us; resolve via xbox_kernel_ordinal_name(). */
    uint32_t kernel_import_count;
    uint16_t kernel_imports[XBE_MAX_KERNEL_IMPORTS];
    /* Set once mapped; the mapping is at base_address by construction. */
    bool mapped;
    size_t mapped_length;
} xbe_image;

/** Human-readable form of a status code. Never NULL. */
const char *xbe_status_str(xbe_status status);

/**
 * Parse an XBE from its raw bytes. Does not map anything.
 *
 * Every offset is bounds-checked against `len`, so a malformed or truncated
 * image yields a status rather than a wrong answer or a crash.
 */
xbe_status xbe_parse(const uint8_t *data, size_t len, xbe_image *out);

/**
 * Confirm this process can host an identity mapping, i.e. that it is PIE.
 *
 * Returns XBE_OK or XBE_ERR_NOT_PIE. Called by xbe_map, but exposed so a host
 * can fail at startup with a clear message rather than mid-load.
 */
xbe_status xbe_check_pie(void);

/**
 * Map the image at its native virtual addresses and copy section contents in.
 *
 * Sections are mapped writable because the guest writes to its own data; page
 * protection is deliberately not modelled yet.
 */
xbe_status xbe_map(xbe_image *image, const uint8_t *data, size_t len);

/** Release a mapping created by xbe_map. Safe to call on an unmapped image. */
void xbe_unmap(xbe_image *image);

/** Actual live image mappings, including page-rounded headers and holes. */
uint64_t xbe_mapped_bytes(void);

/** Find a section by exact name, or NULL. */
const xbe_section *xbe_find_section(const xbe_image *image, const char *name);

/**
 * Translate a guest address to a host pointer.
 *
 * Under identity mapping this is a zero-extension, but go through this function
 * anyway: it bounds-checks against the mapped range, and it is the single place
 * to change if the memory model ever moves to translated addressing. Returns
 * NULL when the address is outside the mapping or the image is not mapped.
 */
void *xbe_at(const xbe_image *image, xbe_guest_ptr addr, size_t length);

/** True when `addr` lies inside the image's virtual address range. */
bool xbe_contains(const xbe_image *image, xbe_guest_ptr addr);

#endif /* TSFP_LOADER_XBE_H */
