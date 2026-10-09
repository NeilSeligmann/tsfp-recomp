/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1613: the guarded guest poke, src/host/guest_poke.c. Synthetic: a fake XBE header and sections at 0x10000, a heap
 * region at 0x02000000, nothing of the title. Each group asserts the thing happened before it compares.
 */

#include "guest_poke.h"

#include "guest_dump.h"
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_memory.h"
#include "nt_status.h"
#include "recomp_abi.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

static int failures;
static int checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                 \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                     \
            failures++;                                                                \
        }                                                                              \
    } while (0)

#define CHECK_EQ(actual, expected)                                                     \
    do {                                                                               \
        checks++;                                                                      \
        const unsigned long long a_ = (unsigned long long)(actual);                   \
        const unsigned long long e_ = (unsigned long long)(expected);                  \
        if (a_ != e_) {                                                                \
            printf("FAIL %s:%d  %s == 0x%llx, expected 0x%llx\n", __FILE__, __LINE__,  \
                   #actual, a_, e_);                                                   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

#define HEAP 0x02000000u
#define IMAGE_SIZE 0x4000u

static char *slurp(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return NULL;
    }
    char *text = calloc(1u, 1u << 16);
    if (text != NULL) {
        (void)fread(text, 1u, (1u << 16) - 1u, file);
    }
    fclose(file);
    return text;
}

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (file != NULL) {
        fputs(text, file);
        fclose(file);
    }
}

static kernel_guest_ptr alloc_region(kernel_guest_ptr fixed_base, uint32_t bytes, uint32_t protect)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = bytes;
    request.alignment = 0x1000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    request.fixed_base = fixed_base;
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr base = guest_region_alloc(&request, &status);
    CHECK(base == fixed_base);
    (void)protect;
    return base;
}

static void put32(uint32_t address, uint32_t value)
{
    CHECK(kernel_guest_write_u32(address, value));
}

static uint32_t get32(uint32_t address)
{
    uint32_t value = 0xDEADBEEFu;
    CHECK(kernel_guest_read_u32(address, &value));
    return value;
}

/* fake XBE: header 0x10000, section table 0x10200 (3 x 0x38), names 0x10400.
 * .text 0x11000+0x800 flags 6 (exec, preload), .data 0x11800+0x800 flags 3 (writable, preload),
 * .rdata 0x12000+0x800 flags 2, image 0x10000..0x14000 so 0x12800..0x14000 is a gap */
static void build_fake_xbe(void)
{
    CHECK(alloc_region(0x10000u, IMAGE_SIZE, PAGE_READWRITE) == 0x10000u);
    CHECK(kernel_guest_write_bytes(0x10000u, "XBEH", 4));
    put32(0x10104u, 0x10000u);
    put32(0x1010Cu, IMAGE_SIZE);
    put32(0x1011Cu, 3u);
    put32(0x10120u, 0x10200u);
    static const struct {
        uint32_t flags, address, size;
        const char *name;
    } sections[3] = {{6u, 0x11000u, 0x800u, ".text"}, {3u, 0x11800u, 0x800u, ".data"}, {2u, 0x12000u, 0x800u, ".rdata"}};
    for (unsigned index = 0u; index < 3u; index++) {
        const uint32_t at = 0x10200u + index * 0x38u;
        put32(at, sections[index].flags);
        put32(at + 4u, sections[index].address);
        put32(at + 8u, sections[index].size);
        put32(at + 0x14u, 0x10400u + index * 16u);
        CHECK(kernel_guest_write_bytes(0x10400u + index * 16u, sections[index].name, strlen(sections[index].name) + 1u));
    }
}

static void test_parse(void)
{
    guest_poke_request request;
    char error[128] = "";
    CHECK(guest_poke_parse(&request, "# comment\n0x52D3FC=6\n\n *0x7844A8+0x1190C=0x38:2\n0x2000000+4=0xFF:1\n", error,
                           sizeof error));
    CHECK_EQ(request.count, 3u);
    if (request.count == 3u) {
        CHECK(!request.items[0].indirect);
        CHECK_EQ(request.items[0].address, 0x52D3FCu);
        CHECK_EQ(request.items[0].width, 4u);
        CHECK_EQ(request.items[0].value, 6u);
        CHECK(request.items[1].indirect);
        CHECK_EQ(request.items[1].address, 0x7844A8u);
        CHECK_EQ(request.items[1].offset, 0x1190Cu);
        CHECK_EQ(request.items[1].width, 2u);
        CHECK_EQ(request.items[1].value, 0x38u);
        CHECK_EQ(request.items[2].offset, 4u);
        CHECK_EQ(request.items[2].width, 1u);
    }
    const char *bad[] = {"",           "\n# only comment\n", "0x10",         "0x10=",         "=5",          "0x10=-1",
                         "0x10=0x100000000", "0x10=1:3",  "0x10=1:0",      "0x10=0x100:1",  "0x10=0x10000:2", "zz=1",
                         "0x10+zz=1",  "*=1",         "0x10=1:",        "0x100000000=1", "0x10 =1 x",   "0x10=1,2"};
    for (size_t index = 0u; index < sizeof bad / sizeof bad[0]; index++) {
        guest_poke_request keep;
        memset(&keep, 0x5A, sizeof keep);
        CHECK(!guest_poke_parse(&keep, bad[index], error, sizeof error));
        CHECK_EQ(((unsigned char *)&keep)[0], 0x5Au); /* untouched on failure */
    }
    CHECK(strstr(error, "line") != NULL);
    /* the limit: 16 fine, 17 refused */
    char many[1024] = "";
    for (unsigned index = 0u; index < GUEST_POKE_MAX; index++) {
        strcat(many, "0x100000=1\n");
    }
    CHECK(guest_poke_parse(&request, many, NULL, 0u));
    CHECK_EQ(request.count, GUEST_POKE_MAX);
    strcat(many, "0x100000=1\n");
    CHECK(!guest_poke_parse(&request, many, error, sizeof error));
    CHECK(strstr(error, "too many") != NULL);
    /* a bad line after good ones fails the whole file */
    CHECK(!guest_poke_parse(&request, "0x100000=1\n0x100004=zz\n", NULL, 0u));
}

static void test_resolve(void)
{
    uint32_t target = 0u;
    guest_poke_item direct = {false, 0x3FFFFFCu, 0u, 4u, 1u};
    CHECK(guest_poke_resolve(&direct, 0u, &target) == NULL);
    CHECK_EQ(target, 0x3FFFFFCu);
    direct.address = 0x3FFFFFDu; /* misaligned, and its end is past the space */
    CHECK(guest_poke_resolve(&direct, 0u, &target) != NULL);
    direct.address = 0x4000000u;
    CHECK(guest_poke_resolve(&direct, 0u, &target) != NULL);
    direct.address = 0x3FFFFFFu; /* one byte at the last address is fine */
    direct.width = 1u;
    CHECK(guest_poke_resolve(&direct, 0u, &target) == NULL);
    direct.width = 2u; /* but two bytes leave the space */
    direct.address = 0x3FFFFFEu;
    CHECK(guest_poke_resolve(&direct, 0u, &target) == NULL);
    direct.address = 0x3FFFFFFu;
    CHECK(guest_poke_resolve(&direct, 0u, &target) != NULL);
    /* offset wrap: address + offset overflows 32 bits */
    guest_poke_item wrap = {false, 0x100u, 0xFFFFFF00u, 4u, 1u};
    CHECK(guest_poke_resolve(&wrap, 0u, &target) != NULL);
    /* alignment */
    guest_poke_item align = {false, 0x100002u, 0u, 4u, 1u};
    CHECK(strstr(guest_poke_resolve(&align, 0u, &target), "aligned") != NULL);
    align.width = 2u;
    CHECK(guest_poke_resolve(&align, 0u, &target) == NULL);
    align.address = 0x100000u;
    align.width = 3u;
    CHECK(guest_poke_resolve(&align, 0u, &target) != NULL);
    align.width = 8u;
    CHECK(guest_poke_resolve(&align, 0u, &target) != NULL);
    /* indirect: pointee above the dump limit is fine (heap), the pointer dword is not */
    guest_poke_item indirect = {true, 0x7844A8u, 0x1190Cu, 4u, 0x38u};
    CHECK(guest_poke_resolve(&indirect, 0x42F9AE20u, &target) == NULL);
    CHECK_EQ(target, 0x42F9AE20u + 0x1190Cu);
    CHECK(guest_poke_resolve(&indirect, 0xFFFFFFFCu, &target) != NULL); /* pointee + offset wraps */
    indirect.offset = 0u;
    CHECK(guest_poke_resolve(&indirect, 0xFFFFFFFCu, &target) == NULL); /* last dword of 32 bits */
    CHECK_EQ(target, 0xFFFFFFFCu);
    indirect.width = 2u;
    CHECK(guest_poke_resolve(&indirect, 0xFFFFFFFFu, &target) != NULL); /* misaligned and wraps */
    indirect.width = 4u;
    indirect.address = 0x3FFFFFCu;
    CHECK(guest_poke_resolve(&indirect, 0x100000u, &target) == NULL);
    indirect.address = 0x4000000u;
    CHECK(guest_poke_resolve(&indirect, 0x100000u, &target) != NULL);
    indirect.address = 0x7844AAu; /* pointer dword misaligned */
    CHECK(guest_poke_resolve(&indirect, 0x100000u, &target) != NULL);
}

static void test_check_target(void)
{
    guest_poke_image image;
    memset(&image, 0, sizeof image);
    CHECK(guest_poke_check_target(&image, HEAP, 4u) != NULL); /* invalid image fails closed */
    image.valid = true;
    image.base = 0x10000u;
    image.size = IMAGE_SIZE;
    image.count = 4u;
    image.sections[0] = (guest_poke_section){0x11000u, 0x800u, 6u, ".text"};
    image.sections[1] = (guest_poke_section){0x11800u, 0x800u, 3u, ".data"};
    image.sections[2] = (guest_poke_section){0x12000u, 0x800u, 2u, ".rdata"};
    image.sections[3] = (guest_poke_section){0x12800u, 0x800u, 3u, ".rdata"}; /* writable flag but named .rdata */
    CHECK(guest_poke_check_target(&image, HEAP, 4u) == NULL);
    CHECK(guest_poke_check_target(&image, 0x14000u, 4u) == NULL); /* first byte after the image */
    CHECK(guest_poke_check_target(&image, 0x11800u, 4u) == NULL);
    CHECK(guest_poke_check_target(&image, 0x11FFCu, 4u) == NULL);
    CHECK(strstr(guest_poke_check_target(&image, 0x11000u, 4u), ".text") != NULL);
    CHECK(strstr(guest_poke_check_target(&image, 0x12000u, 4u), ".rdata") != NULL);
    CHECK(strstr(guest_poke_check_target(&image, 0x12800u, 4u), ".rdata") != NULL); /* by name, flags are not enough */
    CHECK(guest_poke_check_target(&image, 0x10000u, 4u) != NULL); /* header */
    CHECK(guest_poke_check_target(&image, 0x13000u, 4u) != NULL); /* gap inside the image */
    CHECK(guest_poke_check_target(&image, 0x0FFFCu, 4u) != NULL); /* below the base */
    CHECK(guest_poke_check_target(&image, 0u, 1u) != NULL);
    CHECK(guest_poke_check_target(&image, 0x117FEu, 4u) != NULL); /* straddles .text/.data */
    CHECK(guest_poke_check_target(&image, 0x11FFEu, 4u) != NULL); /* straddles .data end into .rdata */
    image.sections[1].flags = 7u; /* writable and executable */
    CHECK(strstr(guest_poke_check_target(&image, 0x11800u, 4u), "executable") != NULL);
    image.sections[1].flags = 2u; /* preload only */
    CHECK(strstr(guest_poke_check_target(&image, 0x11800u, 4u), "read-only") != NULL);
}

static kernel_guest_ptr heap_base;

static void racing_guest_store(uint32_t target)
{
    CHECK(kernel_guest_write_u8(target, 0x77u));
}

static void test_apply_and_image(void)
{
    build_fake_xbe();
    heap_base = alloc_region(HEAP, 0x2000u, PAGE_READWRITE);
    put32(HEAP + 0x100u, 0x11111111u);
    put32(HEAP + 0x104u, 0x22222222u);
    put32(HEAP + 0x1000u, HEAP + 0x200u); /* pointer dword -> HEAP+0x200 */
    put32(HEAP + 0x204u, 0xAAAAAAAAu);
    put32(0x11800u, 0x33333333u);         /* in the writable .data of the fake image */
    put32(0x11000u, 0x44444444u);         /* in .text */

    guest_poke_image image;
    guest_poke_load_image(&image);
    CHECK(image.valid);
    CHECK_EQ(image.count, 3u);
    CHECK_EQ(image.size, IMAGE_SIZE);
    CHECK(strcmp(image.sections[0].name, ".text") == 0);
    CHECK(strcmp(image.sections[2].name, ".rdata") == 0);
    CHECK_EQ(image.sections[1].address, 0x11800u);
    CHECK_EQ(image.sections[1].flags, 3u);

    guest_poke_request request;
    guest_poke_report report;
    char error[96];
    /* G1: without --forced-state nothing is written */
    CHECK(guest_poke_parse(&request, "0x2000100=0xCAFEBABE\n", error, sizeof error));
    CHECK(!guest_poke_apply(&request, false, &image, &report));
    CHECK_EQ(get32(HEAP + 0x100u), 0x11111111u);
    CHECK_EQ(report.written, 0u);
    CHECK(strstr(report.summary, "forced-state") != NULL);
    /* the happy path: direct dword, direct byte, indirect word, writable image .data */
    CHECK(guest_poke_parse(&request,
                           "0x2000100=0xCAFEBABE\n0x2000104+1=0x5A:1\n*0x2001000+4=0xBEEF:2\n0x11800=0x55555555\n", error,
                           sizeof error));
    CHECK(guest_poke_apply(&request, true, &image, &report));
    CHECK_EQ(report.written, 4u);
    CHECK(report.ok);
    CHECK_EQ(get32(HEAP + 0x100u), 0xCAFEBABEu);
    CHECK_EQ(get32(HEAP + 0x104u), 0x22225A22u);
    CHECK_EQ(get32(HEAP + 0x204u), 0xAAAABEEFu); /* upper half untouched */
    CHECK_EQ(get32(0x11800u), 0x55555555u);
    CHECK_EQ(report.results[0].old_value, 0x11111111u);
    CHECK_EQ(report.results[0].read_back, 0xCAFEBABEu);
    CHECK_EQ(report.results[2].pointer, HEAP + 0x200u);
    CHECK_EQ(report.results[2].target, HEAP + 0x204u);
    CHECK_EQ(report.results[2].old_value, 0xAAAAu);
    CHECK(strcmp(report.results[2].status, "OK") == 0);
    /* G4 + G5: a read-only image section refuses the request and the other pokes do not happen either */
    CHECK(guest_poke_parse(&request, "0x2000100=1\n0x11000=0x90909090\n", error, sizeof error));
    CHECK(!guest_poke_apply(&request, true, &image, &report));
    CHECK_EQ(get32(0x11000u), 0x44444444u);
    CHECK_EQ(get32(HEAP + 0x100u), 0xCAFEBABEu);
    CHECK_EQ(report.written, 0u);
    CHECK(strstr(report.results[1].status, ".text") != NULL);
    CHECK(strstr(report.results[0].status, "not written") != NULL);
    /* the header and .rdata */
    CHECK(guest_poke_parse(&request, "0x10000=0\n", error, sizeof error));
    CHECK(!guest_poke_apply(&request, true, &image, &report));
    CHECK(guest_poke_parse(&request, "0x12000=0\n", error, sizeof error));
    CHECK(!guest_poke_apply(&request, true, &image, &report));
    /* G3: a pointer to a wrapping or unmapped target, an unreadable pointer dword */
    put32(HEAP + 0x1004u, 0xFFFFFFFCu);
    CHECK(guest_poke_parse(&request, "*0x2001004+4=1\n", error, sizeof error));
    CHECK(!guest_poke_apply(&request, true, &image, &report));
    put32(HEAP + 0x1008u, 0x20000000u); /* unmapped pointee */
    CHECK(guest_poke_parse(&request, "*0x2001008+0=1\n", error, sizeof error));
    CHECK(!guest_poke_apply(&request, true, &image, &report));
    CHECK(strstr(report.results[0].status, "unmapped") != NULL);
    CHECK(guest_poke_parse(&request, "*0x2FFFFFC0=1\n", error, sizeof error));
    CHECK(!guest_poke_apply(&request, true, &image, &report)); /* pointer dword unreadable */
    /* a null pointer is below the image base */
    put32(HEAP + 0x100Cu, 0u);
    CHECK(guest_poke_parse(&request, "*0x200100C+0x10=1\n", error, sizeof error));
    CHECK(!guest_poke_apply(&request, true, &image, &report));
    /* an invalid image refuses even a harmless heap poke */
    guest_poke_image none;
    memset(&none, 0, sizeof none);
    CHECK(guest_poke_parse(&request, "0x2000100=1\n", error, sizeof error));
    CHECK(!guest_poke_apply(&request, true, &none, &report));
    CHECK_EQ(get32(HEAP + 0x100u), 0xCAFEBABEu);
    /* G6: read-back mismatch from a racing guest store stops the request, later pokes are skipped */
    CHECK(guest_poke_parse(&request, "0x2000100=0x01020304\n0x2000104=0x05060708\n", error, sizeof error));
    guest_poke_test_after_write = racing_guest_store;
    CHECK(!guest_poke_apply(&request, true, &image, &report));
    guest_poke_test_after_write = NULL;
    CHECK(strcmp(report.results[0].status, "READBACK-MISMATCH") == 0);
    CHECK(strstr(report.results[1].status, "skipped") != NULL);
    CHECK_EQ(report.written, 1u);
    CHECK_EQ(report.results[0].read_back, 0x01020377u);
    CHECK_EQ(get32(HEAP + 0x104u), 0x22225A22u);
    /* the loader refuses a header that is not an XBE */
    CHECK(kernel_guest_write_bytes(0x10000u, "XXXX", 4));
    guest_poke_load_image(&image);
    CHECK(!image.valid);
    CHECK(kernel_guest_write_bytes(0x10000u, "XBEH", 4));
    put32(0x10120u, 0x20000000u); /* section table unmapped */
    guest_poke_load_image(&image);
    CHECK(!image.valid);
    put32(0x10120u, 0x10200u);
    put32(0x10104u, 0x20000u); /* wrong base */
    guest_poke_load_image(&image);
    CHECK(!image.valid);
    put32(0x10104u, 0x10000u);
    put32(0x1011Cu, 0u);
    guest_poke_load_image(&image);
    CHECK(!image.valid);
    put32(0x1011Cu, 3u);
    guest_poke_load_image(&image);
    CHECK(image.valid);
}

static void test_consume(void)
{
    mkdir("gp_dir", 0777);
    system("rm -f gp_dir/guestpoke.* gp_dir/FORCED_STATE gp_dir/guestdump.*");
    put32(HEAP + 0x300u, 0x01010101u);
    char header[8192];
    CHECK(!guest_poke_consume("gp_dir", "nothing", true, 7u, header, sizeof header));
    /* without the flag: logged REFUSED, nothing written, no marker, the request is consumed */
    write_file("gp_dir/guestpoke.poke_a", "0x2000300=0x0BADF00D\n");
    CHECK(guest_poke_consume("gp_dir", "poke_a", false, 41u, header, sizeof header));
    CHECK_EQ(get32(HEAP + 0x300u), 0x01010101u);
    CHECK(strstr(header, "REFUSED") != NULL);
    CHECK(access("gp_dir/FORCED_STATE", F_OK) != 0);
    CHECK(access("gp_dir/guestpoke.poke_a", F_OK) != 0);
    CHECK(access("gp_dir/guestpoke.poke_a.done.1", F_OK) == 0);
    CHECK(!guest_poke_consume("gp_dir", "poke_a", true, 42u, header, sizeof header)); /* does not fire twice */
    /* with the flag */
    write_file("gp_dir/guestpoke.poke_b", "# weapon slot\n0x2000300=0x0BADF00D\n");
    CHECK(guest_poke_consume("gp_dir", "poke_b", true, 99u, header, sizeof header));
    CHECK_EQ(get32(HEAP + 0x300u), 0x0BADF00Du);
    CHECK(strstr(header, "# FORCED-STATE") != NULL);
    CHECK(strstr(header, "present=99") != NULL);
    CHECK(strstr(header, "old=0x01010101") != NULL);
    CHECK(strstr(header, "new=0x0BADF00D") != NULL);
    CHECK(strstr(header, "label=poke_b") != NULL);
    CHECK(access("gp_dir/FORCED_STATE", F_OK) == 0);
    char *log = slurp("gp_dir/guestpoke.log");
    CHECK(log != NULL);
    if (log != NULL) {
        CHECK(strstr(log, "label=poke_a") != NULL && strstr(log, "label=poke_b") != NULL); /* refusal also logged */
        CHECK(strstr(log, "status=OK") != NULL);
        CHECK(strstr(log, "REFUSED: host not started") != NULL);
    }
    free(log);
    /* an unparsable request is refused and consumed, a bad label is never read */
    write_file("gp_dir/guestpoke.poke_c", "garbage\n");
    CHECK(guest_poke_consume("gp_dir", "poke_c", true, 1u, header, sizeof header));
    CHECK(strstr(header, "unparsable") != NULL);
    write_file("gp_dir/guestpoke.x", "0x2000300=1\n");
    CHECK(!guest_poke_consume("gp_dir", "../x", true, 1u, header, sizeof header));
    CHECK(!guest_poke_consume("gp_dir", "", true, 1u, header, sizeof header));
    CHECK_EQ(get32(HEAP + 0x300u), 0x0BADF00Du);
    /* end to end through the phase protocol: the poke lands in the dump header and the dump shows the new value */
    guest_dump_set set;
    memset(&set, 0, sizeof set);
    CHECK(guest_dump_parse_append(&set, "0x2000300:0x10", NULL, 0u));
    guest_dump_set_forced_state(true, NULL);
    CHECK(guest_dump_start(&set, GUEST_DUMP_DEFAULT_MAX_BYTES, "gp_dir"));
    write_file("gp_dir/guestpoke.poke_d", "0x2000300=0x600DCAFE\n");
    write_file("gp_dir/guestdump.phase", "poke_d\n");
    kill(getpid(), SIGUSR1);
    for (unsigned wait = 0u; wait < 100u && access("gp_dir/guestdump.ack", F_OK) != 0; wait++) {
        nanosleep(&(struct timespec){0, 50000000L}, NULL);
    }
    guest_dump_stop();
    char *dump = slurp("gp_dir/guestdump.poke_d");
    CHECK(dump != NULL);
    if (dump != NULL) {
        CHECK(strstr(dump, "# FORCED-STATE") != NULL);
        CHECK(strstr(dump, "02000300: fe ca 0d 60") != NULL);
    }
    free(dump);
    char *exit_dump = slurp("gp_dir/guestdump.exit");
    CHECK(exit_dump != NULL && strstr(exit_dump, "FORCED-STATE") == NULL);
    free(exit_dump);
}

static bool wait_for_file(const char *path)
{
    for (unsigned wait = 0u; wait < 100u && access(path, F_OK) != 0; wait++) {
        nanosleep(&(struct timespec){0, 50000000L}, NULL);
    }
    return access(path, F_OK) == 0;
}

/* T1614: the poke trigger SIGUSR2 is separate from the SIGUSR1 phase boundary (census and dump share SIGUSR1). */
static void test_sigusr2_trigger(void)
{
    system("rm -f gp_dir/guestpoke.* gp_dir/guestdump.* gp_dir/FORCED_STATE");
    put32(HEAP + 0x400u, 0x11111111u);
    guest_dump_set set;
    memset(&set, 0, sizeof set);
    CHECK(guest_dump_parse_append(&set, "0x2000400:0x10", NULL, 0u));
    guest_dump_set_forced_state(true, NULL);
    CHECK(guest_dump_start(&set, GUEST_DUMP_DEFAULT_MAX_BYTES, "gp_dir"));
    /* a request is applied on SIGUSR2 with the label from guestpoke.phase, no SIGUSR1 phase is counted or acked */
    write_file("gp_dir/guestpoke.slot_a", "0x2000400=0x22222222\n");
    write_file("gp_dir/guestpoke.phase", "slot_a\n");
    kill(getpid(), SIGUSR2);
    CHECK(wait_for_file("gp_dir/guestpoke.ack"));
    CHECK_EQ(get32(HEAP + 0x400u), 0x22222222u);
    CHECK(access("gp_dir/guestdump.ack", F_OK) != 0);
    char *ack = slurp("gp_dir/guestpoke.ack");
    CHECK(ack != NULL && strcmp(ack, "1 slot_a\n") == 0);
    free(ack);
    char *dump = slurp("gp_dir/guestdump.slot_a");
    CHECK(dump != NULL && strstr(dump, "# FORCED-STATE") != NULL && strstr(dump, "02000400: 22 22 22 22") != NULL);
    free(dump);
    CHECK(system("ls gp_dir/guestpoke.slot_a.done.* >/dev/null 2>&1") == 0);
    /* SIGUSR1 keeps serving guestpoke.<label> exactly as before and does not touch the SIGUSR2 ack */
    write_file("gp_dir/guestpoke.slot_b", "0x2000400=0x33333333\n");
    write_file("gp_dir/guestdump.phase", "slot_b\n");
    kill(getpid(), SIGUSR1);
    CHECK(wait_for_file("gp_dir/guestdump.ack"));
    CHECK_EQ(get32(HEAP + 0x400u), 0x33333333u);
    ack = slurp("gp_dir/guestpoke.ack");
    CHECK(ack != NULL && strcmp(ack, "1 slot_a\n") == 0);
    free(ack);
    /* a SIGUSR2 without a request file for its label writes nothing and still acks */
    write_file("gp_dir/guestpoke.phase", "slot_none\n");
    remove("gp_dir/guestpoke.ack");
    kill(getpid(), SIGUSR2);
    CHECK(wait_for_file("gp_dir/guestpoke.ack"));
    CHECK_EQ(get32(HEAP + 0x400u), 0x33333333u);
    guest_dump_stop();
    /* without --forced-state the SIGUSR2 request is REFUSED, logged and consumed, memory untouched */
    guest_dump_set_forced_state(false, NULL);
    CHECK(guest_dump_start(&set, GUEST_DUMP_DEFAULT_MAX_BYTES, "gp_dir"));
    write_file("gp_dir/guestpoke.slot_c", "0x2000400=0x44444444\n");
    write_file("gp_dir/guestpoke.phase", "slot_c\n");
    remove("gp_dir/guestpoke.ack");
    kill(getpid(), SIGUSR2);
    CHECK(wait_for_file("gp_dir/guestpoke.ack"));
    guest_dump_stop();
    CHECK_EQ(get32(HEAP + 0x400u), 0x33333333u);
    char *log = slurp("gp_dir/guestpoke.log");
    CHECK(log != NULL && strstr(log, "label=slot_c") != NULL && strstr(log, "REFUSED: host not started") != NULL);
    free(log);
    guest_dump_set_forced_state(true, NULL);
}

/* T1616: guest_dump_poke_now serves the request on the calling thread, same guards and logging, no signal. */
static void test_poke_now(void)
{
    system("rm -f gp_dir/guestpoke.* gp_dir/guestdump.* gp_dir/FORCED_STATE");
    put32(HEAP + 0x400u, 0x11111111u);
    guest_dump_set set;
    memset(&set, 0, sizeof set);
    CHECK(guest_dump_parse_append(&set, "0x2000400:0x10", NULL, 0u));
    CHECK(!guest_dump_poke_now("route_a")); /* not started */
    guest_dump_set_forced_state(true, NULL);
    CHECK(guest_dump_start(&set, GUEST_DUMP_DEFAULT_MAX_BYTES, "gp_dir"));
    write_file("gp_dir/guestpoke.route_a", "0x2000400=0x55555555\n");
    CHECK(guest_dump_poke_now("route_a"));
    CHECK_EQ(get32(HEAP + 0x400u), 0x55555555u); /* synchronous: written when the call returns */
    char *dump = slurp("gp_dir/guestdump.route_a");
    CHECK(dump != NULL && strstr(dump, "# FORCED-STATE") != NULL && strstr(dump, "02000400: 55 55 55 55") != NULL);
    free(dump);
    CHECK(system("ls gp_dir/guestpoke.route_a.done.* >/dev/null 2>&1") == 0); /* fires once */
    put32(HEAP + 0x400u, 0x11111111u);
    CHECK(guest_dump_poke_now("route_a"));
    CHECK_EQ(get32(HEAP + 0x400u), 0x11111111u);
    CHECK(!guest_dump_poke_now(""));
    guest_dump_stop();
    /* without --forced-state it is REFUSED and memory untouched */
    guest_dump_set_forced_state(false, NULL);
    CHECK(guest_dump_start(&set, GUEST_DUMP_DEFAULT_MAX_BYTES, "gp_dir"));
    write_file("gp_dir/guestpoke.route_b", "0x2000400=0x66666666\n");
    CHECK(guest_dump_poke_now("route_b"));
    guest_dump_stop();
    CHECK_EQ(get32(HEAP + 0x400u), 0x11111111u);
    char *log = slurp("gp_dir/guestpoke.log");
    CHECK(log != NULL && strstr(log, "label=route_b") != NULL && strstr(log, "REFUSED: host not started") != NULL);
    free(log);
    guest_dump_set_forced_state(true, NULL);
}

int main(void)
{
    test_parse();
    test_resolve();
    test_check_target();
    test_apply_and_image();
    test_consume();
    test_sigusr2_trigger();
    test_poke_now();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
