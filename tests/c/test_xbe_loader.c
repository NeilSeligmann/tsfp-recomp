/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the XBE loader. Every fixture is synthesised in memory, so these run
 * without a disc and can run in CI.
 *
 * The mapping tests use the real base address (0x10000) because that is the
 * property under test: that the guest range is reachable at its native addresses
 * in a PIE host. They will fail loudly if someone builds this non-PIE, which is
 * the point.
 */

#include "xbe.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
        }                                                                                \
    } while (0)

#define CHECK_EQ(actual, expected)                                                      \
    do {                                                                                \
        checks++;                                                                        \
        unsigned long long a_ = (unsigned long long)(actual);                            \
        unsigned long long e_ = (unsigned long long)(expected);                          \
        if (a_ != e_) {                                                                  \
            failures++;                                                                  \
            printf("  FAIL %s:%d  %s == %s (got %#llx, want %#llx)\n", __FILE__,          \
                   __LINE__, #actual, #expected, a_, e_);                                 \
        }                                                                                \
    } while (0)

/* ---------------------------------------------------------------------------
 * Fixture: a structurally valid XBE, mirroring tests/conftest.py's build_xbe so
 * the C and Python implementations are checked against the same shapes.
 * ------------------------------------------------------------------------- */

#define FIX_BASE 0x10000u
#define FIX_HEADER_SIZE 0x1000u
#define FIX_SEC_HDR_OFF 0x400u
#define FIX_NAME_POOL_OFF 0x500u
#define FIX_CERT_OFF 0x700u
#define FIX_IMAGE_SIZE 0x20000u

typedef struct {
    const char *name;
    uint32_t flags;
    uint32_t virtual_addr;
    uint32_t virtual_size;
    uint32_t raw_addr;
    uint32_t raw_size;
} fix_section;

static void put_u32(uint8_t *buf, size_t off, uint32_t value)
{
    buf[off] = (uint8_t)(value & 0xFFu);
    buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
    buf[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
    buf[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

/* Builds into `buf` (must be >= total). Returns bytes used. */
static size_t build_xbe(uint8_t *buf, size_t cap, const fix_section *sections, uint32_t count,
                        uint32_t entry_plain, uint32_t entry_xor, uint32_t image_size,
                        uint32_t magic)
{
    size_t total = FIX_HEADER_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        size_t end = (size_t)sections[i].raw_addr + sections[i].raw_size;
        if (end > total) {
            total = end;
        }
    }
    if (total > cap) {
        return 0;
    }
    memset(buf, 0, total);
    put_u32(buf, 0, magic);

    size_t name_cursor = FIX_NAME_POOL_OFF;
    for (uint32_t i = 0; i < count; i++) {
        size_t off = FIX_SEC_HDR_OFF + (size_t)0x38 * i;
        put_u32(buf, off + 0x00, sections[i].flags);
        put_u32(buf, off + 0x04, sections[i].virtual_addr);
        put_u32(buf, off + 0x08, sections[i].virtual_size);
        put_u32(buf, off + 0x0C, sections[i].raw_addr);
        put_u32(buf, off + 0x10, sections[i].raw_size);
        put_u32(buf, off + 0x14, (uint32_t)(FIX_BASE + name_cursor));
        size_t name_len = strlen(sections[i].name);
        memcpy(buf + name_cursor, sections[i].name, name_len + 1);
        name_cursor += name_len + 1;
    }

    put_u32(buf, FIX_CERT_OFF + 0x00, 0x1D4);
    put_u32(buf, FIX_CERT_OFF + 0x08, 0x45410066u); /* title id */

    put_u32(buf, 0x104, FIX_BASE);
    put_u32(buf, 0x108, FIX_HEADER_SIZE);
    put_u32(buf, 0x10C, image_size);
    put_u32(buf, 0x118, FIX_BASE + FIX_CERT_OFF);
    put_u32(buf, 0x11C, count);
    put_u32(buf, 0x120, FIX_BASE + FIX_SEC_HDR_OFF);
    put_u32(buf, 0x128, entry_plain ^ entry_xor);
    put_u32(buf, 0x158, 0x11000u ^ XBE_THUNK_XOR_RETAIL);
    return total;
}

/* ------------------------------------------------------------------------- */

static void test_parse_header(void)
{
    static uint8_t buf[0x8000];
    const fix_section sections[] = {
        {".text", XBE_SECTION_FLAG_PRELOAD | XBE_SECTION_FLAG_EXECUTABLE, 0x12000, 0x1000,
         0x2000, 0x1000},
    };
    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    CHECK(len > 0);

    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);
    CHECK_EQ(image.base_address, FIX_BASE);
    CHECK_EQ(image.size_of_image, FIX_IMAGE_SIZE);
    CHECK_EQ(image.size_of_headers, FIX_HEADER_SIZE);
    CHECK_EQ(image.section_count, 1u);
    CHECK_EQ(image.title_id, 0x45410066u);
    CHECK_EQ(image.entry_point, 0x12345u);
    CHECK_EQ(image.kernel_thunk_addr, 0x11000u);
    CHECK(image.is_retail);
    CHECK(strcmp(image.sections[0].name, ".text") == 0);
}

static void test_parse_rejects_bad_input(void)
{
    static uint8_t buf[0x8000];
    const fix_section sections[] = {{".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000}};

    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, 0x454E4F4Eu /* 'NONE' */);
    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_ERR_BAD_MAGIC);

    len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                    FIX_IMAGE_SIZE, XBE_MAGIC);
    CHECK_EQ(xbe_parse(buf, 0x100, &image), XBE_ERR_TRUNCATED);
    CHECK_EQ(xbe_parse(NULL, len, &image), XBE_ERR_TRUNCATED);
    CHECK_EQ(xbe_parse(buf, len, NULL), XBE_ERR_TRUNCATED);
}

static void test_debug_key_detection(void)
{
    static uint8_t buf[0x8000];
    const fix_section sections[] = {{".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000}};
    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_DEBUG,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);
    CHECK_EQ(image.entry_point, 0x12345u);
    CHECK(!image.is_retail);
}

static void test_find_section_and_contains(void)
{
    static uint8_t buf[0x8000];
    const fix_section sections[] = {
        {".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000},
        {"D3D", 0x07, 0x13000, 0x1000, 0x3000, 0x1000},
    };
    size_t len = build_xbe(buf, sizeof(buf), sections, 2, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);

    const xbe_section *text = xbe_find_section(&image, ".text");
    CHECK(text != NULL);
    CHECK_EQ(text->virtual_addr, 0x12000u);
    CHECK((text->flags & XBE_SECTION_FLAG_EXECUTABLE) != 0);
    CHECK((text->flags & XBE_SECTION_FLAG_WRITABLE) == 0);

    const xbe_section *d3d = xbe_find_section(&image, "D3D");
    CHECK(d3d != NULL);
    CHECK((d3d->flags & XBE_SECTION_FLAG_WRITABLE) != 0);

    CHECK(xbe_find_section(&image, "NOPE") == NULL);
    CHECK(xbe_contains(&image, FIX_BASE));
    CHECK(xbe_contains(&image, FIX_BASE + FIX_IMAGE_SIZE - 1));
    CHECK(!xbe_contains(&image, FIX_BASE + FIX_IMAGE_SIZE));
    CHECK(!xbe_contains(&image, 0));
}

static void test_pie_probe(void)
{
    /* If this fails the binary was built non-PIE and every mapping test below
     * is meaningless, so say so clearly. */
    xbe_status status = xbe_check_pie();
    CHECK_EQ(status, XBE_OK);
    if (status != XBE_OK) {
        printf("  NOTE host is not PIE: %s\n", xbe_status_str(status));
    }
}

static void test_map_and_read_static_data(void)
{
    static uint8_t buf[0x8000];
    const fix_section sections[] = {
        {".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000},
        {".data", 0x07, 0x13000, 0x1000, 0x3000, 0x1000},
    };
    size_t len = build_xbe(buf, sizeof(buf), sections, 2, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    CHECK(len > 0);

    /* Recognisable payloads so we can prove the right bytes land at the right
     * guest addresses rather than merely that something was copied. */
    for (size_t i = 0; i < 0x1000; i++) {
        buf[0x2000 + i] = (uint8_t)(i & 0xFFu);
        buf[0x3000 + i] = (uint8_t)(0xFFu - (i & 0xFFu));
    }

    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);
    xbe_status status = xbe_map(&image, buf, len);
    CHECK_EQ(status, XBE_OK);
    if (status != XBE_OK) {
        printf("  NOTE map failed: %s\n", xbe_status_str(status));
        return;
    }
    CHECK(image.mapped);

    /* A guest address is a host pointer: the whole point of identity mapping. */
    const uint8_t *text = xbe_at(&image, 0x12000u, 0x1000);
    CHECK(text != NULL);
    CHECK_EQ((uintptr_t)text, 0x12000u);
    CHECK_EQ(text[0], 0x00u);
    CHECK_EQ(text[1], 0x01u);
    CHECK_EQ(text[255], 0xFFu);

    const uint8_t *data = xbe_at(&image, 0x13000u, 0x1000);
    CHECK(data != NULL);
    CHECK_EQ(data[0], 0xFFu);
    CHECK_EQ(data[1], 0xFEu);

    /* The headers are mapped too: the guest reads its own certificate. */
    const uint8_t *header = xbe_at(&image, FIX_BASE, 4);
    CHECK(header != NULL);
    CHECK_EQ(*(const uint32_t *)header, XBE_MAGIC);

    /* Out-of-range requests must return NULL, not a plausible pointer. */
    CHECK(xbe_at(&image, 0, 4) == NULL);
    CHECK(xbe_at(&image, FIX_BASE + FIX_IMAGE_SIZE, 4) == NULL);
    /* A length running past the end is a caller bug; do not paper over it. */
    CHECK(xbe_at(&image, FIX_BASE + FIX_IMAGE_SIZE - 2, 4) == NULL);
    /* The bound is half-open: a read ending exactly at the image end fits. */
    CHECK(xbe_at(&image, FIX_BASE + FIX_IMAGE_SIZE - 4, 4) != NULL);
    /* The length is compared against the room left (limit - addr), never added
     * to addr, so no length can wrap the sum. A 32-bit length that would wrap a
     * 32-bit sum must be refused: 0x10010 + 0xFFFFFFF0 is 0x10000 in 32 bits. */
    CHECK(xbe_at(&image, FIX_BASE + 0x10, 0xFFFFFFF0u) == NULL);
    {
        const size_t room = FIX_IMAGE_SIZE - 0x10;
        /* Exact fit from an interior address and one byte over. */
        CHECK(xbe_at(&image, FIX_BASE + 0x10, room) != NULL);
        CHECK(xbe_at(&image, FIX_BASE + 0x10, room + 1) == NULL);
        /* Largest legal length is the whole image from its base. */
        CHECK(xbe_at(&image, FIX_BASE, FIX_IMAGE_SIZE) != NULL);
        CHECK(xbe_at(&image, FIX_BASE, (size_t)FIX_IMAGE_SIZE + 1) == NULL);
        /* Lengths near SIZE_MAX wrap a 64-bit addr + length back into range. */
        CHECK(xbe_at(&image, FIX_BASE, SIZE_MAX) == NULL);
        CHECK(xbe_at(&image, FIX_BASE + 0x10, SIZE_MAX) == NULL);
        CHECK(xbe_at(&image, FIX_BASE + 0x10, SIZE_MAX - 1) == NULL);
        CHECK(xbe_at(&image, FIX_BASE + 0x10, SIZE_MAX - 0x10) == NULL);
        CHECK(xbe_at(&image, FIX_BASE + 0x10, SIZE_MAX - 0x10 + 1) == NULL);
        /* Truncating a 64-bit length to 32 bits would make this a 4-byte read. */
        if (sizeof(size_t) > 4) {
            CHECK(xbe_at(&image, FIX_BASE, ((size_t)1 << 32) + 4) == NULL);
        }
    }

    xbe_unmap(&image);
    CHECK(!image.mapped);
    xbe_unmap(&image); /* idempotent */
}

static void test_parse_accepts_a_full_section_table(void)
{
    /* A hand-rolled minimal header: the shared fixture's name pool and
     * certificate offsets cannot host 64 section headers. All-zero headers are
     * fine, the cap is what is under test, and it is a CAPACITY, so an image
     * with exactly XBE_MAX_SECTIONS sections is valid and one more is not. */
    static uint8_t buf[0x1200];
    memset(buf, 0, sizeof(buf));
    put_u32(buf, 0, XBE_MAGIC);
    put_u32(buf, 0x104, FIX_BASE);
    put_u32(buf, 0x108, 0x200);
    put_u32(buf, 0x10C, FIX_IMAGE_SIZE);
    put_u32(buf, 0x118, FIX_BASE); /* certificate: readable, content unasserted */
    put_u32(buf, 0x11C, XBE_MAX_SECTIONS);
    put_u32(buf, 0x120, FIX_BASE + 0x200);

    xbe_image image;
    CHECK_EQ(xbe_parse(buf, sizeof(buf), &image), XBE_OK);
    CHECK_EQ(image.section_count, XBE_MAX_SECTIONS);

    put_u32(buf, 0x11C, XBE_MAX_SECTIONS + 1);
    CHECK_EQ(xbe_parse(buf, sizeof(buf), &image), XBE_ERR_TOO_MANY_SECTIONS);
}

static void test_parse_rejects_a_certificate_below_base(void)
{
    /* cert_va - base would wrap; the image must be refused as truncated, not
     * parsed "successfully" with a silently absent title id. */
    static uint8_t buf[0x8000];
    const fix_section sections[] = {{".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000}};
    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    put_u32(buf, 0x118, FIX_BASE - 4);
    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_ERR_TRUNCATED);
}

static void test_decode_fallback_is_the_retail_decode(void)
{
    /* An entry that decodes in-range under NEITHER key gets the deterministic
     * answer: the retail decode, flagged retail. A raw 0 entry word guarantees
     * both decodes (the key values themselves) sit far outside the image. */
    static uint8_t buf[0x8000];
    const fix_section sections[] = {{".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000}};
    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0, 0, FIX_IMAGE_SIZE, XBE_MAGIC);
    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);
    CHECK_EQ(image.entry_point, XBE_ENTRY_XOR_RETAIL);
    CHECK(image.is_retail);
}

static void test_kernel_imports_are_read_from_the_thunk_table(void)
{
    static uint8_t buf[0x8000];
    const fix_section sections[] = {{".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000}};
    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    /* The thunk table sits at the section's FIRST byte (a legal layout and the
     * boundary of the locate test), three entries of ordinal | 0x80000000,
     * NUL-terminated, with zeros after the terminator that must not be walked. */
    put_u32(buf, 0x158, 0x12000u ^ XBE_THUNK_XOR_RETAIL);
    put_u32(buf, 0x2000, 0x80000001u);
    put_u32(buf, 0x2004, 0x80000042u);
    put_u32(buf, 0x2008, 0x800000FFu);
    put_u32(buf, 0x200C, 0);

    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);
    CHECK_EQ(image.kernel_thunk_addr, 0x12000u);
    CHECK_EQ(image.kernel_import_count, 3u);
    CHECK_EQ(image.kernel_imports[0], 0x0001u);
    CHECK_EQ(image.kernel_imports[1], 0x0042u);
    CHECK_EQ(image.kernel_imports[2], 0x00FFu);
}

static void test_a_bss_thunk_table_yields_no_imports(void)
{
    /* A thunk VA inside virtual_size but past raw_size points at zero-filled
     * BSS: there are no file bytes behind it, so there is nothing to read. The
     * second section plants real-looking entries at exactly the file offset a
     * raw-window mistake would compute, so guessing there cannot pass. */
    static uint8_t buf[0x8000];
    const fix_section sections[] = {
        {".text", 0x06, 0x12000, 0x2000, 0x2000, 0x1000},
        {".data", 0x07, 0x14000, 0x100, 0x3800, 0x100},
    };
    size_t len = build_xbe(buf, sizeof(buf), sections, 2, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    put_u32(buf, 0x158, 0x13800u ^ XBE_THUNK_XOR_RETAIL);
    put_u32(buf, 0x3800, 0x80000005u);

    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);
    CHECK_EQ(image.kernel_thunk_addr, 0x13800u);
    CHECK_EQ(image.kernel_import_count, 0u);
}

static void test_guest_ptr_is_four_bytes(void)
{
    /* If this ever changes, every guest struct layout silently breaks. */
    CHECK_EQ(sizeof(xbe_guest_ptr), 4u);
}

static void test_map_rejects_section_bytes_past_eof(void)
{
    /* Section raw offsets are untrusted input. A file whose header parses but
     * whose section bytes lie past EOF must fail the MAP, and the failed map
     * must leave the range released (the second map proves it). */
    static uint8_t buf[0x8000];
    const fix_section sections[] = {{".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000}};
    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);

    CHECK_EQ(xbe_map(&image, buf, 0x1800), XBE_ERR_TRUNCATED);
    CHECK(!image.mapped);

    CHECK_EQ(xbe_map(&image, buf, len), XBE_OK);
    xbe_unmap(&image);
}

static void test_map_rejects_a_section_outside_the_image(void)
{
    /* A section VA outside the image would be a write outside the mapping the
     * map itself just created, at whatever host address the guest pointer
     * zero-extends to. Refused as a range error, never copied. */
    static uint8_t buf[0x8000];
    const fix_section sections[] = {{".text", 0x06, 0x40000, 0x1000, 0x2000, 0x1000}};
    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);
    CHECK_EQ(xbe_map(&image, buf, len), XBE_ERR_RANGE);
    CHECK(!image.mapped);
}

static void test_map_refuses_a_double_map(void)
{
    static uint8_t buf[0x8000];
    const fix_section sections[] = {{".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000}};
    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);
    CHECK_EQ(xbe_map(&image, buf, len), XBE_OK);

    /* Mapping an already-mapped image is a caller bug and is refused BEFORE
     * reaching mmap; attempted anyway it would fail EEXIST against its own
     * mapping and be misdiagnosed as a non-PIE host. */
    CHECK_EQ(xbe_map(&image, buf, len), XBE_ERR_RANGE);
    CHECK(image.mapped);
    xbe_unmap(&image);
}

static void test_an_occupied_range_reads_as_not_pie(void)
{
    /* EEXIST on the guest range is reported as the specific non-PIE diagnosis,
     * not as a generic mmap failure: that distinction is what tells an operator
     * to fix the build rather than hunt a phantom kernel limit. */
    static uint8_t buf[0x8000];
    const fix_section sections[] = {{".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000}};
    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    xbe_image first, second;
    CHECK_EQ(xbe_parse(buf, len, &first), XBE_OK);
    CHECK_EQ(xbe_parse(buf, len, &second), XBE_OK);
    CHECK_EQ(xbe_map(&first, buf, len), XBE_OK);

    CHECK_EQ(xbe_map(&second, buf, len), XBE_ERR_NOT_PIE);
    CHECK(!second.mapped);
    xbe_unmap(&first);
}

static void test_unmap_releases_the_range(void)
{
    /* unmap must release exactly what map reserved, or the range stays occupied
     * and the next load of an image at the same base fails as if the host were
     * non-PIE. Mapping again is the proof. */
    static uint8_t buf[0x8000];
    const fix_section sections[] = {{".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000}};
    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);
    CHECK_EQ(xbe_map(&image, buf, len), XBE_OK);
    xbe_unmap(&image);

    CHECK_EQ(xbe_map(&image, buf, len), XBE_OK);
    xbe_unmap(&image);
}

static void test_at_requires_a_mapping(void)
{
    static uint8_t buf[0x8000];
    const fix_section sections[] = {{".text", 0x06, 0x12000, 0x1000, 0x2000, 0x1000}};
    size_t len = build_xbe(buf, sizeof(buf), sections, 1, 0x12345u, XBE_ENTRY_XOR_RETAIL,
                           FIX_IMAGE_SIZE, XBE_MAGIC);
    xbe_image image;
    CHECK_EQ(xbe_parse(buf, len, &image), XBE_OK);
    CHECK(xbe_at(&image, 0x12000u, 4) == NULL);
}

int main(void)
{
    printf("xbe loader tests\n");
    test_parse_header();
    test_parse_rejects_bad_input();
    test_parse_accepts_a_full_section_table();
    test_parse_rejects_a_certificate_below_base();
    test_debug_key_detection();
    test_decode_fallback_is_the_retail_decode();
    test_kernel_imports_are_read_from_the_thunk_table();
    test_a_bss_thunk_table_yields_no_imports();
    test_find_section_and_contains();
    test_guest_ptr_is_four_bytes();
    test_pie_probe();
    test_map_and_read_static_data();
    test_map_rejects_section_bytes_past_eof();
    test_map_rejects_a_section_outside_the_image();
    test_map_refuses_a_double_map();
    test_an_occupied_range_reads_as_not_pie();
    test_unmap_releases_the_range();
    test_at_requires_a_mapping();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
