/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the xbe_dump cross-validation tool.
 *
 * WHY THIS SUITE EXISTS. xbe_dump is the C half of the loader's cross-validation
 * oracle (tests/test_integration.py diffs it field-for-field against the Python
 * parser on the real XBE), but that pytest is gated on the user's gitignored
 * default.xbe, so until this suite existed NO ctest reached xbe_dump.c and no
 * mutation could be killed in it (T383 finding; the same reachability trap as
 * src/host/main.c, recorded in tools/mutate/c_suites.py). A dump tool that prints
 * the wrong field is worse than one that crashes: the cross-validation would then
 * compare a wrong pair and bless it.
 *
 * The tool's main() is compiled into this binary under the name xbe_dump_main via
 * the include below, so the mutation harness's fingerprint check sees xbe_dump.c
 * changes in a ctest binary. The fixtures are synthesised in memory (the same
 * shapes as test_xbe_loader.c) and written to mkstemp files, so this runs with no
 * disc, no XBE and no fixed paths.
 */

#define main xbe_dump_main
#include "../../src/loader/xbe_dump.c"
#undef main

#include <fcntl.h>
#include <unistd.h>

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
        long long a_ = (long long)(actual);                                              \
        long long e_ = (long long)(expected);                                            \
        if (a_ != e_) {                                                                  \
            failures++;                                                                  \
            printf("  FAIL %s:%d  %s == %s (got %lld, want %lld)\n", __FILE__,            \
                   __LINE__, #actual, #expected, a_, e_);                                 \
        }                                                                                \
    } while (0)

/* ---------------------------------------------------------------------------
 * Fixture: the same structurally valid XBE as test_xbe_loader.c, with the
 * recognisable per-section payloads, so the dumped lines have known values.
 * ------------------------------------------------------------------------- */

#define FIX_BASE 0x10000u
#define FIX_HEADER_SIZE 0x1000u
#define FIX_SEC_HDR_OFF 0x400u
#define FIX_NAME_POOL_OFF 0x500u
#define FIX_CERT_OFF 0x700u
#define FIX_IMAGE_SIZE 0x20000u

static void put_u32(uint8_t *buf, size_t off, uint32_t value)
{
    buf[off] = (uint8_t)(value & 0xFFu);
    buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
    buf[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
    buf[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

/* A two-section image: .text (payload i & 0xFF) and .data (payload 0xFF - i).
 * Returns the byte count used. */
static size_t build_fixture(uint8_t *buf, uint32_t magic)
{
    const size_t total = 0x4000;
    memset(buf, 0, total);
    put_u32(buf, 0, magic);

    /* .text: VA 0x12000, raw 0x2000, 0x1000 bytes. */
    put_u32(buf, FIX_SEC_HDR_OFF + 0x00, 0x06);
    put_u32(buf, FIX_SEC_HDR_OFF + 0x04, 0x12000);
    put_u32(buf, FIX_SEC_HDR_OFF + 0x08, 0x1000);
    put_u32(buf, FIX_SEC_HDR_OFF + 0x0C, 0x2000);
    put_u32(buf, FIX_SEC_HDR_OFF + 0x10, 0x1000);
    put_u32(buf, FIX_SEC_HDR_OFF + 0x14, FIX_BASE + FIX_NAME_POOL_OFF);
    memcpy(buf + FIX_NAME_POOL_OFF, ".text", 6);
    /* .data: VA 0x13000, raw 0x3000, 0x1000 bytes. */
    put_u32(buf, FIX_SEC_HDR_OFF + 0x38 + 0x00, 0x07);
    put_u32(buf, FIX_SEC_HDR_OFF + 0x38 + 0x04, 0x13000);
    put_u32(buf, FIX_SEC_HDR_OFF + 0x38 + 0x08, 0x1000);
    put_u32(buf, FIX_SEC_HDR_OFF + 0x38 + 0x0C, 0x3000);
    put_u32(buf, FIX_SEC_HDR_OFF + 0x38 + 0x10, 0x1000);
    put_u32(buf, FIX_SEC_HDR_OFF + 0x38 + 0x14, FIX_BASE + FIX_NAME_POOL_OFF + 6);
    memcpy(buf + FIX_NAME_POOL_OFF + 6, ".data", 6);

    for (size_t i = 0; i < 0x1000; i++) {
        buf[0x2000 + i] = (uint8_t)(i & 0xFFu);
        buf[0x3000 + i] = (uint8_t)(0xFFu - (i & 0xFFu));
    }

    put_u32(buf, FIX_CERT_OFF + 0x00, 0x1D4);
    put_u32(buf, FIX_CERT_OFF + 0x08, 0x45410066u); /* title id */

    put_u32(buf, 0x104, FIX_BASE);
    put_u32(buf, 0x108, FIX_HEADER_SIZE);
    put_u32(buf, 0x10C, FIX_IMAGE_SIZE);
    put_u32(buf, 0x118, FIX_BASE + FIX_CERT_OFF);
    put_u32(buf, 0x11C, 2);
    put_u32(buf, 0x120, FIX_BASE + FIX_SEC_HDR_OFF);
    put_u32(buf, 0x128, 0x12345u ^ XBE_ENTRY_XOR_RETAIL);
    put_u32(buf, 0x158, 0x11000u ^ XBE_THUNK_XOR_RETAIL);
    return total;
}

/* Write `len` bytes to a fresh temp file and return its path (static storage,
 * valid until the next call). Unlinked by the caller via cleanup(). */
static char fixture_path[64];

static const char *write_fixture(const uint8_t *data, size_t len)
{
    snprintf(fixture_path, sizeof(fixture_path), "/tmp/tsfp_xbe_dump_fix_XXXXXX");
    int fd = mkstemp(fixture_path);
    if (fd < 0) {
        printf("  FATAL: mkstemp failed\n");
        exit(EXIT_FAILURE);
    }
    size_t done = 0;
    while (done < len) {
        ssize_t wrote = write(fd, data + done, len - done);
        if (wrote <= 0) {
            printf("  FATAL: writing the fixture failed\n");
            exit(EXIT_FAILURE);
        }
        done += (size_t)wrote;
    }
    close(fd);
    return fixture_path;
}

/* Run the tool in-process with stdout redirected into a capture buffer. The
 * tool returns rather than exits on every path, which is what makes this safe. */
#define CAPTURE_MAX 8192

static char captured[CAPTURE_MAX];

static int run_tool(int argc, char **argv)
{
    char capture_path[] = "/tmp/tsfp_xbe_dump_out_XXXXXX";
    int fd = mkstemp(capture_path);
    if (fd < 0) {
        printf("  FATAL: mkstemp for capture failed\n");
        exit(EXIT_FAILURE);
    }
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);
    int rc = xbe_dump_main(argc, argv);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    memset(captured, 0, sizeof(captured));
    lseek(fd, 0, SEEK_SET);
    ssize_t got = read(fd, captured, sizeof(captured) - 1);
    if (got < 0) {
        got = 0;
    }
    captured[got] = '\0';
    close(fd);
    unlink(capture_path);
    return rc;
}

/* True when `line` occurs in the capture bounded by newlines (or the ends), so
 * "entry_point 0x12345" cannot be satisfied by "entry_point 0x123456". */
static bool has_line(const char *line)
{
    const char *at = captured;
    size_t len = strlen(line);
    while ((at = strstr(at, line)) != NULL) {
        bool starts = (at == captured) || (at[-1] == '\n');
        bool ends = (at[len] == '\n') || (at[len] == '\0');
        if (starts && ends) {
            return true;
        }
        at += 1;
    }
    return false;
}

/* ------------------------------------------------------------------------- */

static uint8_t fixture[0x4000];

static void test_usage_without_a_path(void)
{
    char *argv[] = {(char *)"xbe_dump", NULL};
    CHECK_EQ(run_tool(1, argv), 2);
    CHECK_EQ(captured[0], '\0'); /* usage goes to stderr, stdout stays empty */
}

static void test_missing_file_fails(void)
{
    char *argv[] = {(char *)"xbe_dump", (char *)"/nonexistent/no.xbe", NULL};
    CHECK_EQ(run_tool(2, argv), 1);
}

static void test_bad_magic_fails(void)
{
    size_t len = build_fixture(fixture, 0x454E4F4Eu /* 'NONE' */);
    const char *path = write_fixture(fixture, len);
    char *argv[] = {(char *)"xbe_dump", (char *)path, NULL};
    CHECK_EQ(run_tool(2, argv), 1);
    CHECK_EQ(captured[0], '\0');
    unlink(path);
}

static void test_dump_prints_the_parsed_fields(void)
{
    size_t len = build_fixture(fixture, XBE_MAGIC);
    const char *path = write_fixture(fixture, len);
    char *argv[] = {(char *)"xbe_dump", (char *)path, NULL};
    CHECK_EQ(run_tool(2, argv), 0);

    CHECK(has_line("file_size 16384"));
    CHECK(has_line("base_address 0x10000"));
    CHECK(has_line("size_of_image 0x20000"));
    CHECK(has_line("size_of_headers 0x1000"));
    CHECK(has_line("entry_point 0x12345"));
    CHECK(has_line("kernel_thunk_addr 0x11000"));
    CHECK(has_line("is_retail 1"));
    CHECK(has_line("title_id 0x45410066"));
    CHECK(has_line("section_count 2"));
    /* The section line order is name flags va vsize raw rsize; the two
     * sections differ in every address field so a swapped pair cannot hide. */
    CHECK(has_line("section .text 0x6 0x12000 0x1000 0x2000 0x1000"));
    CHECK(has_line("section .data 0x7 0x13000 0x1000 0x3000 0x1000"));
    /* No --map, so nothing may be mapped (and nothing printed about it). */
    CHECK(strstr(captured, "mapped") == NULL);
    unlink(path);
}

static void test_map_prints_the_live_mapping(void)
{
    size_t len = build_fixture(fixture, XBE_MAGIC);
    const char *path = write_fixture(fixture, len);
    char *argv[] = {(char *)"xbe_dump", (char *)"--map", (char *)path, NULL};
    CHECK_EQ(run_tool(3, argv), 0);

    /* 0x10000..0x30000, both page-aligned. */
    CHECK(has_line("mapped 1 length 0x20000"));
    CHECK(has_line("mapped_magic 0x48454258"));
    /* Entry 0x12345 lands in .text at payload offset 0x345, pattern i & 0xFF. */
    CHECK(has_line("entry_first_byte 0x45"));

    /* And the tool unmapped on its way out: the same mapping works again. */
    CHECK_EQ(run_tool(3, argv), 0);
    unlink(path);
}

static void test_map_failure_is_an_error_exit(void)
{
    /* Headers only: parse succeeds, but .text's raw bytes lie past EOF so the
     * map must fail, and the failure must be the exit code, not a shrug. */
    size_t len = build_fixture(fixture, XBE_MAGIC);
    (void)len;
    const char *path = write_fixture(fixture, FIX_HEADER_SIZE);
    char *plain[] = {(char *)"xbe_dump", (char *)path, NULL};
    CHECK_EQ(run_tool(2, plain), 0); /* without --map this still dumps */
    char *mapped[] = {(char *)"xbe_dump", (char *)"--map", (char *)path, NULL};
    CHECK_EQ(run_tool(3, mapped), 1);
    unlink(path);
}

int main(void)
{
    printf("xbe_dump tool tests\n");
    test_usage_without_a_path();
    test_missing_file_fails();
    test_bad_magic_fails();
    test_dump_prints_the_parsed_fields();
    test_map_prints_the_live_mapping();
    test_map_failure_is_an_error_exit();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
