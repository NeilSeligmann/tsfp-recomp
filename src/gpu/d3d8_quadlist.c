/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_quadlist.h"
uint32_t d3d8_quadlist_triangle_index_count(uint32_t vertex_count)
{
    return (vertex_count / 4u) * 6u;
}
uint32_t d3d8_quadlist_expand(const uint16_t *indices, uint32_t first, uint32_t vertex_count,
                              uint32_t *out, uint32_t capacity)
{
    const uint32_t quads = vertex_count / 4u;
    const uint32_t needed = quads * 6u;
    if(needed > capacity) return UINT32_MAX;
    if(indices == NULL && quads != 0u && (uint64_t)first + quads * 4u - 1u > UINT32_MAX)
        return UINT32_MAX;
    for(uint32_t quad = 0u; quad < quads; quad++) {
        uint32_t v[4];
        for(uint32_t i = 0u; i < 4u; i++)
            v[i] = indices != NULL ? indices[quad * 4u + i] : first + quad * 4u + i;
        uint32_t *dst = out + quad * 6u;
        dst[0] = v[0]; dst[1] = v[1]; dst[2] = v[2];
        dst[3] = v[0]; dst[4] = v[2]; dst[5] = v[3];
    }
    return needed;
}
