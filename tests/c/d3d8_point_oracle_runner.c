/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_dirty.h"
int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) return 2;
    FILE *input = fopen(argv[1], "rb");
    FILE *output = fopen(argv[2], "wb");
    if (!input || !output) return 2;
    uint32_t header[2];
    if (fread(header, sizeof(header), 1u, input) != 1u) return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(header[0], header[1]);
    for (uint32_t offset = 0; offset < D3D_REGION_BYTES; offset += 4u) {
        uint32_t value;
        if (fread(&value, sizeof(value), 1u, input) != 1u) return 2;
        store(D3D_REGION_BASE + offset, value);
    }
    fclose(input);
    const uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    const uint32_t result = argc == 3 ? d3d8_emit_point_state()
        : strcmp(argv[3], "lighting") == 0
          ? d3d8_emit_default_lighting_state(load(D3D8_GLOBAL_DIRTY_MASK))
          : d3d8_emit_shader_stage_program();
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
