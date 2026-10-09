/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1599: the READ-ONLY guest memory dump, src/host/guest_dump.c and guest_dump_ranges.c.
 *
 * Synthetic: a prepared guest region stands in for the title's memory, the dump is compared with it byte for byte.
 * Option-level refusals live in test_host_options.c. Each group asserts something was dumped before comparing.
 */

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
#include <time.h>
#include <unistd.h>

/* the thunk library reads the guest registers, this suite defines them */
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
        const unsigned long long a_ = (unsigned long long)(actual);                    \
        const unsigned long long e_ = (unsigned long long)(expected);                  \
        if (a_ != e_) {                                                                \
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__,      \
                   #actual, a_, e_);                                                   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

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

static kernel_guest_ptr prepared_region(uint8_t seed, kernel_guest_ptr fixed_base)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 0x2000u;
    request.alignment = 0x1000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    request.fixed_base = fixed_base; /* inside the dumpable 0..0x03FFFFFF */
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr base = guest_region_alloc(&request, &status);
    CHECK(base != 0u);
    if (base != 0u) {
        uint8_t bytes[0x2000];
        for (unsigned index = 0u; index < sizeof bytes; index++) {
            bytes[index] = (uint8_t)(seed + index * 7u);
        }
        CHECK(kernel_guest_write_bytes(base, bytes, sizeof bytes));
    }
    return base;
}

static void test_parse_and_bounds(void)
{
    guest_dump_set set;
    memset(&set, 0, sizeof set);
    char error[96] = "";
    CHECK(guest_dump_parse_append(&set, "0x52D3F4:0x30,0x7844A8:0x400", error, sizeof error));
    CHECK_EQ(set.count, 2u);
    CHECK_EQ(set.total_bytes, 0x430u);
    CHECK_EQ(set.ranges[0].address, 0x52D3F4u);
    CHECK_EQ(set.ranges[1].length, 0x400u);
    /* the last byte of the space is valid, one past it is not */
    guest_dump_set edge;
    memset(&edge, 0, sizeof edge);
    CHECK(guest_dump_parse_append(&edge, "0x3FFFFFF:1", NULL, 0u));
    CHECK(guest_dump_parse_append(&edge, "0:0x4000000", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "0x3FFFFFF:2", error, sizeof error));
    CHECK(strstr(error, "leaves") != NULL);
    CHECK(!guest_dump_parse_append(&edge, "0x4000000:1", error, sizeof error));
    CHECK(strstr(error, "outside") != NULL);
    /* 32-bit wraps: address + length overflows uint32_t to a small number */
    CHECK(!guest_dump_parse_append(&edge, "0x10:0xFFFFFFF8", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "0x3000000:0xFFFFFFFF", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "0xFFFFFFFF:2", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "0x100000000:1", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "0x10:0x100000000", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "0:0x4000001", NULL, 0u));
    /* parse errors */
    const char *bad[] = {"",     "0x10",   ":4",     "4:",     "0x10:0",  "0x10:4,", ",0x10:4", "0x10:4,,0x20:4",
                         "-1:4", "+1:4",   " 1:4",   "1: 4",   "zz:4",    "0x10:4x", "0x10:4:8", "99999999999999999999999:4"};
    for (size_t index = 0u; index < sizeof bad / sizeof bad[0]; index++) {
        const guest_dump_set before = set;
        CHECK(!guest_dump_parse_append(&set, bad[index], NULL, 0u));
        CHECK(set.count == before.count && set.total_bytes == before.total_bytes); /* unchanged on failure */
    }
    /* a failing item later in the list leaves the set untouched even though earlier items were fine */
    const unsigned kept = set.count;
    CHECK(!guest_dump_parse_append(&set, "0x20:4,0x30:zz", NULL, 0u));
    CHECK_EQ(set.count, kept);
    /* at most 16 */
    guest_dump_set many;
    memset(&many, 0, sizeof many);
    for (unsigned index = 0u; index < GUEST_DUMP_MAX_RANGES; index++) {
        CHECK(guest_dump_parse_append(&many, "0x100:4", NULL, 0u));
    }
    CHECK(!guest_dump_parse_append(&many, "0x100:4", error, sizeof error));
    CHECK(strstr(error, "too many") != NULL);
    /* the total bound */
    CHECK(guest_dump_check_total(&set, 0x430u + 0x430u));
    CHECK(guest_dump_check_total(&set, set.total_bytes));
    CHECK(!guest_dump_check_total(&set, set.total_bytes - 1u));
    CHECK(!guest_dump_check_total(&set, (uint64_t)GUEST_DUMP_HARD_MAX_BYTES + 1u));
    guest_dump_set empty;
    memset(&empty, 0, sizeof empty);
    CHECK(!guest_dump_check_total(&empty, 100u));
}

static void test_dump_content_equals_guest_buffer(void)
{
    const kernel_guest_ptr base = prepared_region(0x11u, 0x02000000u);
    if (base == 0u) {
        return;
    }
    guest_dump_set set;
    memset(&set, 0, sizeof set);
    char text[80];
    snprintf(text, sizeof text, "0x%X:0x30,0x%X:20", (unsigned)base + 4u, (unsigned)base + 0x1000u - 4u);
    CHECK(guest_dump_parse_append(&set, text, NULL, 0u));
    CHECK(guest_dump_write(&set, "gd_content.txt"));
    char *dump = slurp("gd_content.txt");
    CHECK(dump != NULL);
    if (dump == NULL) {
        return;
    }
    /* rebuild the expected hex text from the same pattern the guest holds, independent of the dump code */
    char expect[2048];
    size_t used = 0u;
    const struct {
        uint32_t start;
        uint32_t length;
    } ranges[2] = {{base + 4u, 0x30u}, {base + 0x1000u - 4u, 20u}};
    used += (size_t)snprintf(expect + used, sizeof expect - used, "# guest-dump 1\n# ranges 2 bytes 68\n");
    for (int index = 0; index < 2; index++) {
        used += (size_t)snprintf(expect + used, sizeof expect - used, "range 0x%08X 0x%X\n", (unsigned)ranges[index].start,
                                 (unsigned)ranges[index].length);
        for (uint32_t row = 0u; row < ranges[index].length; row += 16u) {
            used += (size_t)snprintf(expect + used, sizeof expect - used, "%08X:", (unsigned)(ranges[index].start + row));
            for (uint32_t column = row; column < row + 16u && column < ranges[index].length; column++) {
                const uint32_t offset = ranges[index].start + column - base;
                used += (size_t)snprintf(expect + used, sizeof expect - used, " %02x", (unsigned)(uint8_t)(0x11u + offset * 7u));
            }
            used += (size_t)snprintf(expect + used, sizeof expect - used, "\n");
        }
    }
    CHECK(strlen(dump) > 100u);
    CHECK(strcmp(dump, expect) == 0);
    free(dump);
    /* deterministic: a second dump is byte for byte the same */
    CHECK(guest_dump_write(&set, "gd_content2.txt"));
    char *again = slurp("gd_content2.txt");
    char *first = slurp("gd_content.txt");
    CHECK(again != NULL && first != NULL && strcmp(again, first) == 0);
    free(again);
    free(first);
    /* read-only: the guest bytes are unchanged by dumping */
    uint8_t probe = 0u;
    CHECK(kernel_guest_read_u8(base + 4u, &probe));
    CHECK_EQ(probe, (uint8_t)(0x11u + 4u * 7u));
    /* an unreadable range is reported, not fabricated */
    guest_dump_set hole;
    memset(&hole, 0, sizeof hole);
    CHECK(guest_dump_parse_append(&hole, "0x3F00000:16", NULL, 0u));
    CHECK(!guest_dump_write(&hole, "gd_hole.txt"));
    char *hole_text = slurp("gd_hole.txt");
    CHECK(hole_text != NULL && strstr(hole_text, "unreadable 0x03F00000") != NULL);
    free(hole_text);
}

static void test_indirect_parse_and_resolve(void)
{
    guest_dump_set set;
    memset(&set, 0, sizeof set);
    char error[96] = "";
    CHECK(guest_dump_parse_append(&set, "*0x7844A8+0:0x400,*0x7844A8:4,*0x10+0x20:8,0x100:4", error, sizeof error));
    CHECK_EQ(set.count, 4u);
    CHECK(set.ranges[0].indirect && set.ranges[0].address == 0x7844A8u && set.ranges[0].offset == 0u && set.ranges[0].length == 0x400u);
    CHECK(set.ranges[1].indirect && set.ranges[1].offset == 0u && set.ranges[1].length == 4u);
    CHECK(set.ranges[2].indirect && set.ranges[2].address == 0x10u && set.ranges[2].offset == 0x20u);
    CHECK(!set.ranges[3].indirect);
    CHECK_EQ(set.total_bytes, 0x400u + 4u + 8u + 4u);
    /* edges of the static bounds: pointer dword fully inside, offset+length exactly the space */
    guest_dump_set edge;
    memset(&edge, 0, sizeof edge);
    CHECK(guest_dump_parse_append(&edge, "*0x3FFFFFC:4", NULL, 0u));
    CHECK(guest_dump_parse_append(&edge, "*0x10+0x3FFFFFF:1", NULL, 0u));
    CHECK(guest_dump_parse_append(&edge, "*0x10+0:0x4000000", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "*0x3FFFFFD:4", error, sizeof error)); /* dword straddles the end */
    CHECK(strstr(error, "pointer dword") != NULL);
    CHECK(!guest_dump_parse_append(&edge, "*0x3FFFFFF:4", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "*0x4000000:4", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "*0x10+0x4000000:1", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "*0x10+0x3FFFFFF:2", error, sizeof error));
    CHECK(strstr(error, "offset+length") != NULL);
    CHECK(!guest_dump_parse_append(&edge, "*0x10+0xFFFFFFF8:0x10", NULL, 0u)); /* 32-bit wrap of OFF+LEN */
    CHECK(!guest_dump_parse_append(&edge, "*0x10+0x100000000:4", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "*0x10+0xFFFFFFFFFFFFFFFF:1", NULL, 0u)); /* 64-bit OFF+LEN wraps to 0 */
    CHECK(!guest_dump_parse_append(&edge, "*0x10+2:0xFFFFFFFFFFFFFFFF", NULL, 0u)); /* 64-bit OFF+LEN wraps to 1 */
    CHECK(!guest_dump_parse_append(&edge, "*0x10+0:0x4000001", NULL, 0u));
    CHECK(!guest_dump_parse_append(&edge, "*0x10+0:0", NULL, 0u));
    CHECK_EQ(edge.count, 3u);
    const char *bad[] = {"*",        "*:4",       "*0x10",    "*0x10+:4", "*+4:4",   "**0x10:4",  "*0x10+4+4:4", "*-1:4",
                         "* 0x10:4", "*0x10 +4:4", "*0x10+-4:4", "*0x10+4x:4", "*0x10:4x", "0x10+4:4", "*zz:4", "*0x10+zz:4"};
    for (size_t index = 0u; index < sizeof bad / sizeof bad[0]; index++) {
        const guest_dump_set before = set;
        CHECK(!guest_dump_parse_append(&set, bad[index], NULL, 0u));
        CHECK(set.count == before.count && set.total_bytes == before.total_bytes);
    }
    /* 16 range limit counts indirect items too */
    guest_dump_set many;
    memset(&many, 0, sizeof many);
    for (unsigned index = 0u; index < GUEST_DUMP_MAX_RANGES; index++) {
        CHECK(guest_dump_parse_append(&many, "*0x100+4:4", NULL, 0u));
    }
    CHECK(!guest_dump_parse_append(&many, "*0x100+4:4", error, sizeof error));
    CHECK(strstr(error, "too many") != NULL);
    /* resolution: pure bounds on the runtime pointer */
    guest_dump_range range;
    memset(&range, 0, sizeof range);
    range.indirect = true;
    range.address = 0x100u;
    range.offset = 0x10u;
    range.length = 0x20u;
    uint32_t final = 0u;
    CHECK(guest_dump_indirect_target(&range, 0x1000u, &final) == NULL);
    CHECK_EQ(final, 0x1010u);
    CHECK(guest_dump_indirect_target(&range, 0x42F9AE20u, &final) == NULL); /* a measured heap pointer, above 64 MiB */
    CHECK_EQ(final, 0x42F9AE30u);
    CHECK(guest_dump_indirect_target(&range, 0xFFFFFFD0u, &final) == NULL); /* ends exactly at 2^32 */
    CHECK_EQ(final, 0xFFFFFFE0u);
    CHECK(guest_dump_indirect_target(&range, 0xFFFFFFD1u, &final) != NULL); /* one byte past */
    CHECK(guest_dump_indirect_target(&range, 0xFFFFFFF0u, &final) != NULL); /* wraps to 0x20 in 32 bits */
    CHECK(guest_dump_indirect_target(&range, 0xFFFFFFFFu, &final) != NULL);
    CHECK(guest_dump_indirect_target(&range, 0u, &final) == NULL); /* null is just an address here, the read decides */
    range.offset = 0xFFFFFFF0u; /* hand-built, the parser would refuse it: the 32-bit sum with pointer 0x20 is 0x30 */
    range.length = 0x20u;
    CHECK(guest_dump_indirect_target(&range, 0x20u, &final) != NULL);
    range.offset = 0x10u;
    range.indirect = false;
    CHECK(guest_dump_indirect_target(&range, 0x1000u, &final) != NULL);
}

static void write_u32(kernel_guest_ptr address, uint32_t value)
{
    CHECK(kernel_guest_write_bytes(address, (const uint8_t *)&value, 4u));
}

/* T1759: the two level form **ADDR+OFF1+OFF2:LEN */
static void test_twice(void)
{
    guest_dump_set set;
    memset(&set, 0, sizeof set);
    char error[96] = "";
    CHECK(guest_dump_parse_append(&set, "**0x6F32F0+0x134+0x220:0x440,*0x10+4:4,0x20:4", error, sizeof error));
    CHECK_EQ(set.count, 3u);
    CHECK(set.ranges[0].twice && set.ranges[0].indirect && set.ranges[0].address == 0x6F32F0u && set.ranges[0].offset == 0x134u &&
          set.ranges[0].offset2 == 0x220u && set.ranges[0].length == 0x440u);
    CHECK(!set.ranges[1].twice && !set.ranges[2].twice && set.ranges[1].offset2 == 0u);
    const char *bad[] = {"**0x10:4",           "**0x10+4:4",         "**0x10+4+4+4:4",  "**0x10++4:4",       "**0x10+4+:4",
                         "**+4+4:4",           "***0x10+4+4:4",      "**0x10+4+-4:4",   "**0x10+4+zz:4",     "**0x4000000+4+4:4",
                         "**0x3FFFFFD+4+4:4",  "**0x10+0x4000001+4:4", "**0x10+4+0x3FFFFFF:2", "**0x10+4+0xFFFFFFFFFFFFFFFF:1", "**0x10+4+4:0",
                         "**0x10 +4+4:4",      "**0x10+4+4"};
    for (unsigned index = 0u; index < sizeof bad / sizeof bad[0]; index++) {
        guest_dump_set none;
        memset(&none, 0, sizeof none);
        CHECK(!guest_dump_parse_append(&none, bad[index], error, sizeof error));
        CHECK_EQ(none.count, 0u);
    }
    guest_dump_set edge;
    memset(&edge, 0, sizeof edge);
    CHECK(guest_dump_parse_append(&edge, "**0x3FFFFFC+0x4000000+0x3FFFFFF:1,**0x10+0+0:0x4000000", NULL, 0u));
    /* pure resolution */
    guest_dump_range range = set.ranges[0];
    uint32_t value = 0u;
    CHECK(guest_dump_twice_slot(&range, 0x1000u, &value) == NULL && value == 0x1134u);
    CHECK(guest_dump_twice_slot(&range, 0u, &value) != NULL);
    CHECK(guest_dump_twice_slot(&range, 0xFFFFFECCu, &value) != NULL); /* slot dword would reach past 2^32 */
    CHECK(guest_dump_twice_slot(&range, 0xFFFFFEC8u, &value) == NULL);
    CHECK(guest_dump_twice_target(&range, 0x2000u, &value) == NULL && value == 0x2220u);
    CHECK(guest_dump_twice_target(&range, 0u, &value) != NULL);
    CHECK(guest_dump_twice_target(&range, 0xFFFFFB80u, &value) != NULL); /* 0x220+0x440 wraps */
    CHECK(guest_dump_twice_target(&range, 0xFFFFF000u, &value) == NULL);
    range.twice = false;
    CHECK(guest_dump_twice_slot(&range, 0x1000u, &value) != NULL && guest_dump_twice_target(&range, 0x1000u, &value) != NULL);

    const kernel_guest_ptr base = prepared_region(0x23u, 0x02600000u);
    if (base == 0u) {
        return;
    }
    const kernel_guest_ptr cell = base + 0x1F00u;
    write_u32(cell, base + 0x200u);            /* first pointer */
    write_u32(base + 0x200u + 0x10u, base + 0x400u); /* second pointer at first+0x10 */
    write_u32(cell + 4u, 0u);                  /* null first */
    write_u32(cell + 8u, base + 0x200u);       /* first ok ... */
    write_u32(cell + 12u, 0u);                 /* null first (reused below) */
    write_u32(base + 0x200u + 0x20u, 0u);      /* ... null second at first+0x20 */
    write_u32(cell + 16u, 0x3F00000u);         /* first pointing at unmapped */
    write_u32(base + 0x200u + 0x30u, 0xFFFFFFF8u); /* wrapping second at first+0x30 */
    write_u32(cell + 20u, base + 0x200u);
    write_u32(cell + 24u, base + 0x200u);
    write_u32(base + 0x200u + 0x40u, 0x3F00000u); /* second pointing at unmapped */
    char text[512];
    snprintf(text, sizeof text,
             "**0x%X+0x10+4:0x20,**0x%X+0x10+4:8,**0x%X+0x20+4:8,**0x%X+0x10+4:8,**0x%X+0x30+0:0x10,**0x3F00000+0+0:4,**0x%X+0x40+0:4,0x%X:4",
             (unsigned)cell, (unsigned)cell + 4u, (unsigned)cell + 8u, (unsigned)cell + 16u, (unsigned)cell + 20u,
             (unsigned)cell + 24u, (unsigned)base);
    guest_dump_set live;
    memset(&live, 0, sizeof live);
    CHECK(guest_dump_parse_append(&live, text, NULL, 0u));
    CHECK_EQ(live.count, 8u);
    CHECK(!guest_dump_write(&live, "gd_twice.txt"));
    char *dump = slurp("gd_twice.txt");
    CHECK(dump != NULL);
    if (dump == NULL) {
        return;
    }
    char line[200];
    snprintf(line, sizeof line, "indirect2 0x%08X + 0x10 + 0x4 length 0x20\npointer 0x%08X pointer2 0x%08X final 0x%08X\nrange 0x%08X 0x20\n",
             (unsigned)cell, (unsigned)(base + 0x200u), (unsigned)(base + 0x400u), (unsigned)(base + 0x404u), (unsigned)(base + 0x404u));
    CHECK(strstr(dump, line) != NULL);
    snprintf(line, sizeof line, "%08X: %02x ", (unsigned)(base + 0x404u), (unsigned)(uint8_t)(0x23u + 0x404u * 7u));
    CHECK(strstr(dump, line) != NULL);
    CHECK(strstr(dump, "pointer 0x00000000\nunreadable pointer at") != NULL && strstr(dump, "first pointer is null") != NULL);
    CHECK(strstr(dump, "pointer2 0x00000000\nunreadable pointer at") != NULL && strstr(dump, "second pointer is null") != NULL);
    CHECK(strstr(dump, "pointer 0x03F00000\nunreadable pointer at") != NULL && strstr(dump, "second pointer dword not readable") != NULL);
    CHECK(strstr(dump, "pointer2 0xFFFFFFF8\nunreadable pointer at") != NULL && strstr(dump, "second pointer+offset2+length wraps") != NULL);
    CHECK(strstr(dump, "unreadable pointer at 0x03F00000: first pointer dword not readable") != NULL);
    CHECK(strstr(dump, "pointer2 0x03F00000 final 0x03F00000\nrange 0x03F00000 0x4\nunreadable 0x03F00000\n") != NULL);
    snprintf(line, sizeof line, "range 0x%08X 0x4\n%08X: 23 ", (unsigned)base, (unsigned)base); /* later direct range survives */
    CHECK(strstr(dump, line) != NULL);
    free(dump);
}

static void test_indirect_dump(void)
{
    const kernel_guest_ptr base = prepared_region(0x23u, 0x02200000u);
    if (base == 0u) {
        return;
    }
    const kernel_guest_ptr cell0 = base + 0x1F00u; /* pointer dwords live in the region too */
    write_u32(cell0, base + 0x100u);                /* valid: 0x20 bytes at base+0x100+8 */
    write_u32(cell0 + 4u, 0x3F00000u);              /* valid address, unmapped memory */
    write_u32(cell0 + 8u, 0xFFFFFFF0u);             /* +8:0x20 wraps past 2^32 */
    write_u32(cell0 + 12u, 0x42F9AE20u);            /* a heap pointer like the owner's, not mapped here */
    guest_dump_set set;
    memset(&set, 0, sizeof set);
    char text[256];
    snprintf(text, sizeof text, "*0x%X+8:0x20,*0x%X+0:16,*0x%X+8:0x20,*0x%X+0x20:0x20,*0x3F00000+0:4,0x%X:4", (unsigned)cell0,
             (unsigned)cell0 + 4u, (unsigned)cell0 + 8u, (unsigned)cell0 + 12u, (unsigned)base);
    CHECK(guest_dump_parse_append(&set, text, NULL, 0u));
    CHECK_EQ(set.count, 6u);
    CHECK(!guest_dump_write(&set, "gd_indirect.txt")); /* three ranges are unreadable */
    char *dump = slurp("gd_indirect.txt");
    CHECK(dump != NULL);
    if (dump == NULL) {
        return;
    }
    char line[160];
    /* range 1: header lines then content equal to the prepared pattern at base+0x108 */
    snprintf(line, sizeof line, "indirect 0x%08X + 0x8 length 0x20\npointer 0x%08X final 0x%08X\nrange 0x%08X 0x20\n",
             (unsigned)cell0, (unsigned)(base + 0x100u), (unsigned)(base + 0x108u), (unsigned)(base + 0x108u));
    CHECK(strstr(dump, line) != NULL);
    char row[96];
    snprintf(row, sizeof row, "%08X: %02x %02x %02x", (unsigned)(base + 0x108u), (unsigned)(uint8_t)(0x23u + 0x108u * 7u),
             (unsigned)(uint8_t)(0x23u + 0x109u * 7u), (unsigned)(uint8_t)(0x23u + 0x10Au * 7u));
    CHECK(strstr(dump, row) != NULL);
    snprintf(row, sizeof row, "%08X: ", (unsigned)(base + 0x118u)); /* second row of that range */
    CHECK(strstr(dump, row) != NULL);
    /* range 2: pointer to unmapped memory is the ordinary unreadable line after a good header */
    snprintf(line, sizeof line, "pointer 0x03F00000 final 0x03F00000\nrange 0x03F00000 0x10\nunreadable 0x03F00000\n");
    CHECK(strstr(dump, line) != NULL);
    /* range 3 wrapping pointer: reason written, no range block. range 4: heap pointer, unmapped here */
    snprintf(line, sizeof line, "pointer 0xFFFFFFF0\nunreadable pointer at 0x%08X: pointer+offset+length wraps", (unsigned)(cell0 + 8u));
    CHECK(strstr(dump, line) != NULL);
    CHECK(strstr(dump, "pointer 0x42F9AE20 final 0x42F9AE40\nrange 0x42F9AE40 0x20\nunreadable 0x42F9AE40\n") != NULL);
    /* range 5: the pointer dword itself is unmapped */
    CHECK(strstr(dump, "unreadable pointer at 0x03F00000: pointer dword not readable") != NULL);
    /* range 6 (direct) after the failures is still dumped: the other ranges are not aborted */
    snprintf(line, sizeof line, "range 0x%08X 0x4\n%08X: 23 ", (unsigned)base, (unsigned)base);
    CHECK(strstr(dump, line) != NULL);
    /* the pointee moves between runs: same spec, new pointer, new content */
    write_u32(cell0, base + 0x400u);
    CHECK(!guest_dump_write(&set, "gd_indirect2.txt"));
    char *moved = slurp("gd_indirect2.txt");
    CHECK(moved != NULL && strcmp(moved, dump) != 0);
    if (moved != NULL) {
        snprintf(line, sizeof line, "pointer 0x%08X final 0x%08X\n", (unsigned)(base + 0x400u), (unsigned)(base + 0x408u));
        CHECK(strstr(moved, line) != NULL);
    }
    free(moved);
    free(dump);
    /* a pointee ending exactly at 2^32 resolves, the unmapped read is the ordinary unreadable line */
    write_u32(cell0, 0xFFFFFFD8u);
    guest_dump_set near_end;
    memset(&near_end, 0, sizeof near_end);
    snprintf(text, sizeof text, "*0x%X+8:0x20", (unsigned)cell0);
    CHECK(guest_dump_parse_append(&near_end, text, NULL, 0u));
    (void)guest_dump_write(&near_end, "gd_indirect3.txt");
    char *tail = slurp("gd_indirect3.txt");
    CHECK(tail != NULL && strstr(tail, "pointer 0xFFFFFFD8 final 0xFFFFFFE0\nrange 0xFFFFFFE0 0x20\nunreadable 0xFFFFFFE0\n") != NULL);
    free(tail);
    /* read-only: nothing in the pointer cell changed */
    uint32_t after = 0u;
    CHECK(kernel_guest_read_u32(cell0, &after));
    CHECK_EQ(after, 0xFFFFFFD8u);
}

static bool wait_for_ack(unsigned want)
{
    for (int attempt = 0; attempt < 250; attempt++) {
        char *text = slurp("gd_dir/guestdump.ack");
        if (text != NULL) {
            const unsigned got = (unsigned)strtoul(text, NULL, 10);
            free(text);
            if (got >= want) {
                return true;
            }
        }
        struct timespec nap = {0, 20000000L};
        nanosleep(&nap, NULL);
    }
    return false;
}

static void write_phase_word(const char *word)
{
    FILE *control = fopen("gd_dir/guestdump.phase", "w");
    CHECK(control != NULL);
    if (control != NULL) {
        fputs(word, control);
        fclose(control);
    }
}

static void test_phase_files(void)
{
    const kernel_guest_ptr base = prepared_region(0x40u, 0x02100000u);
    if (base == 0u) {
        return;
    }
    guest_dump_set set;
    memset(&set, 0, sizeof set);
    char text[48];
    snprintf(text, sizeof text, "0x%X:8", (unsigned)base);
    CHECK(guest_dump_parse_append(&set, text, NULL, 0u));
    CHECK(!guest_dump_start(&set, 4u, "gd_dir")); /* over the byte bound */
    CHECK(guest_dump_start(&set, 64u, "gd_dir"));
    CHECK(!guest_dump_start(&set, 64u, "gd_dir")); /* only once */
    char *pid_text = slurp("gd_dir/guestdump.pid");
    CHECK(pid_text != NULL && (pid_t)strtol(pid_text, NULL, 10) == getpid());
    free(pid_text);
    write_phase_word("menu-idle\n");
    kill(getpid(), SIGUSR1);
    CHECK(wait_for_ack(1u));
    CHECK(kernel_guest_write_u8(base, 0xEEu)); /* the TEST changes the guest between phases, not the dump */
    write_phase_word("fire/../x\n"); /* sanitized: separators and dots are dropped */
    kill(getpid(), SIGUSR1);
    CHECK(wait_for_ack(2u));
    remove("gd_dir/guestdump.phase");
    kill(getpid(), SIGUSR1);
    CHECK(wait_for_ack(3u));
    guest_dump_stop();
    char *idle = slurp("gd_dir/guestdump.menu-idle");
    char *fire = slurp("gd_dir/guestdump.firex");
    char *third = slurp("gd_dir/guestdump.3");
    char *exit_dump = slurp("gd_dir/guestdump.exit");
    CHECK(idle != NULL && fire != NULL && third != NULL && exit_dump != NULL);
    if (idle != NULL && fire != NULL && third != NULL && exit_dump != NULL) {
        char line[48];
        snprintf(line, sizeof line, "%08X: 40 ", (unsigned)base);
        CHECK(strstr(idle, line) != NULL);
        snprintf(line, sizeof line, "%08X: ee ", (unsigned)base);
        CHECK(strstr(idle, line) == NULL);
        CHECK(strstr(fire, line) != NULL);
        CHECK(strstr(third, line) != NULL);
        CHECK(strstr(exit_dump, line) != NULL);
    }
    free(idle);
    free(fire);
    free(third);
    free(exit_dump);
    guest_dump_stop(); /* a second stop is harmless */
}

/* T1629: a binary snapshot captured on one thread and formatted later must be the same TEXT the live dump writes, for
 * every shape of set the live dump handles: direct, indirect, multi chunk, unreadable pointer, wrapping pointer, a range
 * that runs off the mapped memory in a later chunk. The shared walk makes this true by construction, this proves it. */
static void test_snapshot_text_equals_live_dump(void)
{
    const kernel_guest_ptr base = prepared_region(0x5Bu, 0x02400000u);
    if (base == 0u) {
        return;
    }
    const kernel_guest_ptr cell = base + 0x1F00u;
    write_u32(cell, base + 0x100u);
    write_u32(cell + 4u, 0x3F00000u);
    write_u32(cell + 8u, 0xFFFFFFF0u);
    guest_dump_set set;
    memset(&set, 0, sizeof set);
    char text[512];
    /* 0x1800 bytes = two chunks, the last range starts mapped and ends past the region (second chunk unreadable) */
    snprintf(text, sizeof text, "0x%X:0x1800,*0x%X+8:0x20,*0x%X+0:16,*0x%X+8:0x20,*0x3F00000+0:4,0x%X:0x30,0x%X:0x2000,0x3F00000:8",
             (unsigned)base, (unsigned)cell, (unsigned)cell + 4u, (unsigned)cell + 8u, (unsigned)base + 4u, (unsigned)base + 0x1000u);
    CHECK(guest_dump_parse_append(&set, text, NULL, 0u));
    CHECK_EQ(set.count, 8u);
    const char *header = "# button-dump event=1 kind=before label=x poll=3 present=4 offset=0\n";
    CHECK(!guest_dump_write_with_header(&set, "gd_snap_live.txt", header)); /* some ranges are unreadable */
    guest_dump_snapshot *snapshot = guest_dump_capture(&set);
    CHECK(snapshot != NULL);
    if (snapshot == NULL) {
        return;
    }
    CHECK(!guest_dump_snapshot_complete(snapshot));
    CHECK(guest_dump_snapshot_bytes(snapshot) > 0x1800u + 0x30u + 0x2000u);
    /* the guest memory moving on after the capture does not change the snapshot text */
    uint8_t scribble[0x1800];
    memset(scribble, 0xEE, sizeof scribble);
    CHECK(kernel_guest_write_bytes(base, scribble, sizeof scribble));
    write_u32(cell, base + 0x800u);
    CHECK(!guest_dump_snapshot_write(snapshot, "gd_snap_snap.txt", header));
    char *live = slurp("gd_snap_live.txt");
    char *snap = slurp("gd_snap_snap.txt");
    CHECK(live != NULL && snap != NULL);
    if (live != NULL && snap != NULL) {
        CHECK(strlen(live) > 1000u);
        CHECK(strcmp(live, snap) == 0);
        CHECK(strstr(snap, header) != NULL);
        CHECK(strstr(snap, "unreadable pointer at 0x03F00000: pointer dword not readable") != NULL);
        CHECK(strstr(snap, "pointer+offset+length wraps") != NULL);
        CHECK(strstr(snap, "range 0x03F00000 0x8\nunreadable 0x03F00000\n") != NULL);
    }
    /* the same snapshot formats twice to the same bytes and leaves no temporary file */
    CHECK(!guest_dump_snapshot_write(snapshot, "gd_snap_again.txt", header));
    char *again = slurp("gd_snap_again.txt");
    CHECK(again != NULL && snap != NULL && strcmp(again, snap) == 0);
    CHECK(access("gd_snap_snap.txt.tmp", F_OK) != 0 && access("gd_snap_again.txt.tmp", F_OK) != 0);
    free(again);
    free(live);
    free(snap);
    guest_dump_snapshot_free(snapshot);
    /* a fully readable set: write returns true, the estimate bounds the real size from above and lies within 250 bytes of it */
    guest_dump_set clean;
    memset(&clean, 0, sizeof clean);
    snprintf(text, sizeof text, "0x%X:0x1800,*0x%X+8:0x20,0x%X:0x30", (unsigned)base, (unsigned)cell, (unsigned)base + 4u);
    write_u32(cell, base + 0x100u);
    CHECK(guest_dump_parse_append(&clean, text, NULL, 0u));
    guest_dump_snapshot *whole = guest_dump_capture(&clean);
    CHECK(whole != NULL && guest_dump_snapshot_complete(whole));
    CHECK(guest_dump_snapshot_write(whole, "gd_snap_clean.txt", NULL));
    CHECK(guest_dump_write(&clean, "gd_snap_clean_live.txt"));
    char *clean_snap = slurp("gd_snap_clean.txt");
    char *clean_live = slurp("gd_snap_clean_live.txt");
    CHECK(clean_snap != NULL && clean_live != NULL && strcmp(clean_snap, clean_live) == 0);
    if (clean_snap != NULL) {
        const uint64_t actual = strlen(clean_snap);
        const uint64_t estimate = guest_dump_estimate_text_bytes(&clean);
        CHECK(actual > 5000u);
        CHECK(estimate >= actual);
        CHECK(estimate <= actual + 250u); /* measured 22914 for 22741: the fixed overhead is estimated, the rows are exact */
    }
    free(clean_snap);
    free(clean_live);
    guest_dump_snapshot_free(whole);
    /* refusals */
    guest_dump_set empty;
    memset(&empty, 0, sizeof empty);
    CHECK(guest_dump_capture(&empty) == NULL && guest_dump_capture(NULL) == NULL);
    CHECK(!guest_dump_snapshot_write(NULL, "gd_snap_none.txt", NULL));
    CHECK(access("gd_snap_none.txt", F_OK) != 0);
    CHECK(!guest_dump_snapshot_complete(NULL) && guest_dump_snapshot_bytes(NULL) == 0u);
    guest_dump_snapshot_free(NULL);
}

int main(void)
{
    test_parse_and_bounds();
    test_dump_content_equals_guest_buffer();
    test_indirect_parse_and_resolve();
    test_indirect_dump();
    test_twice();
    test_snapshot_text_equals_live_dump();
    test_phase_files();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
