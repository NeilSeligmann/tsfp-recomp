/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * QUADLIST (primitive 8) expansion to a triangle list (src/gpu/d3d8_quadlist.c). Each group of
 * four vertices (a,b,c,d) becomes triangles (a,b,c) and (a,c,d), the NV2A QUADS split. A trailing
 * group of fewer than four vertices draws nothing. */
#include "d3d8_quadlist.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if(!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while(0)

static void test_counts(void)
{
    CHECK(d3d8_quadlist_triangle_index_count(0u) == 0u);
    CHECK(d3d8_quadlist_triangle_index_count(3u) == 0u);
    CHECK(d3d8_quadlist_triangle_index_count(4u) == 6u);
    CHECK(d3d8_quadlist_triangle_index_count(7u) == 6u);
    CHECK(d3d8_quadlist_triangle_index_count(8u) == 12u);
}

static void test_sequential(void)
{
    uint32_t out[12];
    memset(out, 0xEE, sizeof(out));
    CHECK(d3d8_quadlist_expand(NULL, 5u, 8u, out, 12u) == 12u);
    const uint32_t want[12] = {5,6,7, 5,7,8, 9,10,11, 9,11,12};
    CHECK(memcmp(out, want, sizeof(want)) == 0);
}

static void test_indexed(void)
{
    const uint16_t indices[5] = {10, 20, 30, 40, 99};
    uint32_t out[6];
    CHECK(d3d8_quadlist_expand(indices, 0u, 5u, out, 6u) == 6u);
    const uint32_t want[6] = {10,20,30, 10,30,40};
    CHECK(memcmp(out, want, sizeof(want)) == 0);
}

static void test_capacity_and_degenerate(void)
{
    uint32_t out[6] = {0};
    CHECK(d3d8_quadlist_expand(NULL, 0u, 8u, out, 6u) == UINT32_MAX);
    CHECK(d3d8_quadlist_expand(NULL, 0u, 3u, out, 0u) == 0u);
    CHECK(d3d8_quadlist_expand(NULL, 0xFFFFFFFEu, 4u, out, 6u) == UINT32_MAX);
    CHECK(d3d8_quadlist_expand(NULL, 0xFFFFFFFCu, 4u, out, 6u) == 6u && out[5] == 0xFFFFFFFFu);
}

int main(void)
{
    test_counts();
    test_sequential();
    test_indexed();
    test_capacity_and_degenerate();
    if(failures == 0) puts("d3d8_quadlist: all passed");
    return failures != 0;
}
