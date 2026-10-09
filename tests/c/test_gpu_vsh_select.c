/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include "gpu_vsh_select.h"

/* Optional: the generator's output for a synthetic corpus, supplied by tests/test_nv2a_vsh_modules.py */
#ifdef VSH_TABLE_INC
#include "gpu_vsh_select.h"
#include VSH_TABLE_INC
#define FIXTURE vsh_table
#else
static const char *const names[] = {"generated_a", "generated_b", "static_c"};
static const struct gpu_vsh_entry key_list[] = {{0x0u, 0u}, {0x2u, 1u}, {0x8010u, 0u}, {0x40000u, 1u}};
static const struct gpu_vsh_entry static_list[] = {{0x30040u, 2u}, {0x30100u, 2u}};
static const struct gpu_vsh_table fixture = {0x48012u, 4u, key_list, 2u, static_list, 3u, names};
#define FIXTURE fixture
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void)
{
    uint32_t module = 99u;
    CHECK(FIXTURE.key_count > 0u);
    CHECK(FIXTURE.static_count > 0u);
    CHECK(gpu_vsh_table_valid(&FIXTURE));
    /* every listed id is found, and maps to its own module */
    for (uint32_t i = 0u; i < FIXTURE.key_count; i++) {
        module = 0xFFFFu;
        CHECK(gpu_vsh_lookup_key(&FIXTURE, FIXTURE.keys[i].id, &module));
        CHECK(module == FIXTURE.keys[i].module);
    }
    for (uint32_t i = 0u; i < FIXTURE.static_count; i++) {
        module = 0xFFFFu;
        CHECK(gpu_vsh_lookup_static(&FIXTURE, FIXTURE.statics[i].id, &module));
        CHECK(module == FIXTURE.statics[i].module);
    }
    /* exactness: every neighbouring id that is not listed misses and leaves module alone */
    for (uint32_t i = 0u; i < FIXTURE.static_count; i++) {
        for (int delta = -1; delta <= 1; delta += 2) {
            const uint32_t probe = FIXTURE.statics[i].id + (uint32_t)delta;
            uint32_t other;
            int listed = 0;
            for (uint32_t j = 0u; j < FIXTURE.static_count; j++) listed |= FIXTURE.statics[j].id == probe;
            module = 77u;
            other = (uint32_t)gpu_vsh_lookup_static(&FIXTURE, probe, &module);
            CHECK((int)other == listed);
            if (!listed) CHECK(module == 77u);
        }
    }
    /* bits outside the mask are ignored, bits inside it are not */
    if (FIXTURE.key_mask != 0xFFFFFFFFu) {
        const uint32_t outside = ~FIXTURE.key_mask & 0x80000000u ? 0x80000000u : ~FIXTURE.key_mask & -(~FIXTURE.key_mask);
        module = 0xFFFFu;
        CHECK(gpu_vsh_lookup_key(&FIXTURE, FIXTURE.keys[0].id | outside, &module));
        CHECK(module == FIXTURE.keys[0].module);
    }
    for (uint32_t bit = 0u; bit < 32u; bit++) {
        const uint32_t key = FIXTURE.keys[0].id ^ (1u << bit);
        int listed = 0;
        for (uint32_t j = 0u; j < FIXTURE.key_count; j++) listed |= FIXTURE.keys[j].id == (key & FIXTURE.key_mask);
        CHECK(gpu_vsh_lookup_key(&FIXTURE, key, &module) == listed);
    }
    /* a module is found by its exact file stem (T84: the replay names a program by its digest) */
    CHECK(FIXTURE.module_count > 0u);
    for (uint32_t i = 0u; i < FIXTURE.module_count; i++) {
        module = 0xFFFFu;
        CHECK(gpu_vsh_lookup_name(&FIXTURE, FIXTURE.module_names[i], &module));
        CHECK(module == i);
    }
    module = 77u;
    CHECK(!gpu_vsh_lookup_name(&FIXTURE, "", &module) && module == 77u);
    CHECK(!gpu_vsh_lookup_name(&FIXTURE, "static_", &module) && module == 77u);
    {
        char longer[256];
        snprintf(longer, sizeof longer, "%sx", FIXTURE.module_names[0]);
        CHECK(!gpu_vsh_lookup_name(&FIXTURE, longer, &module) && module == 77u); /* a prefix is not a hit */
        snprintf(longer, sizeof longer, "%s", FIXTURE.module_names[0] + 1);
        CHECK(!gpu_vsh_lookup_name(&FIXTURE, longer, &module) && module == 77u); /* nor a suffix */
    }
#ifndef VSH_TABLE_INC
    /* the validator rejects unsorted, duplicate, out-of-range and out-of-mask tables */
    {
        static const struct gpu_vsh_entry dup[] = {{2u, 0u}, {2u, 0u}};
        static const struct gpu_vsh_entry range[] = {{2u, 3u}};
        static const struct gpu_vsh_entry mask[] = {{0x4u, 0u}};
        struct gpu_vsh_table bad = FIXTURE;
        bad.keys = dup; bad.key_count = 2u; CHECK(!gpu_vsh_table_valid(&bad));
        bad.keys = range; bad.key_count = 1u; CHECK(!gpu_vsh_table_valid(&bad));
        bad.keys = mask; CHECK(!gpu_vsh_table_valid(&bad));
        /* descending ids: a binary search over them misses entries (T259 mutation survivor) */
        static const struct gpu_vsh_entry descending[] = {{2u, 0u}, {0u, 0u}};
        bad.keys = descending; bad.key_count = 2u; CHECK(!gpu_vsh_table_valid(&bad));
        bad = FIXTURE;
        bad.statics = descending; bad.static_count = 2u; CHECK(!gpu_vsh_table_valid(&bad));
    }
#endif
    if (failures == 0) printf("gpu_vsh_select: ok\n");
    return failures != 0;
}
