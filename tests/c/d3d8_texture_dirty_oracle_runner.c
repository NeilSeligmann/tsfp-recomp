/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_texture_dirty.h"
#include "d3d8_resource.h"
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb");
    FILE *output = fopen(argv[2], "wb");
    if (!input || !output) return 2;
    uint32_t header[4];
    if (fread(header, sizeof(header), 1u, input) != 1u) return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(header[0], header[1]);
    for (uint32_t offset = 0; offset < D3D_REGION_BYTES; offset += 4u) {
        uint32_t value;
        if (fread(&value, sizeof(value), 1u, input) != 1u) return 2;
        store(D3D_REGION_BASE + offset, value);
    }
    uint32_t constants[5];
    if (fread(constants, sizeof(constants), 1u, input) != 1u) return 2;
    map_fixed(0x004A1000u, 0x1000u);
    store(0x00475C78u, constants[0]);
    store(0x004A1BB4u, constants[1]);
    store(0x004A1BB0u, constants[2]);
    store(0x00475CACu, constants[3]);
    store(0x00475CD4u, constants[4]);
    fclose(input);
    const uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    const uint32_t result = header[2] == 0u ? d3d8_emit_texture_stages(header[3])
                               : header[2] == 3u ? d3d8_emit_fog_vertex_program()
                               : header[2] == 2u ? d3d8_draw_vertices(8u,17u,1u)
                                                   : d3d8_emit_fog();
    const uint32_t end = d3d8_device_load32(D3D8_DEV_CURSOR);
    if (fwrite(&result, sizeof(result), 1u, output) != 1u) return 2;
    for (uint32_t address = start; address < end; address += 4u) {
        const uint32_t value = load(address);
        if (fwrite(&value, sizeof(value), 1u, output) != 1u) return 2;
    }
    for (uint32_t offset = 0; offset < D3D_REGION_BYTES; offset += 4u) {
        const uint32_t value = load(D3D_REGION_BASE + offset);
        if (fwrite(&value, sizeof(value), 1u, output) != 1u) return 2;
    }
    fclose(output);
    environment_end();
    return 0;
}
