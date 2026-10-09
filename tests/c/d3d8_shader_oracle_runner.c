/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Private test runner: replay oracle guest state through the C vertex setter. */
#include "test_d3d8_support.h"
#include "d3d8_shader.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb");
    FILE *output = fopen(argv[2], "wb");
    if (!input || !output) return 2;
    uint32_t header[4];
    if (fread(header, sizeof(header), 1u, input) != 1u) return 2;
    const bool pixel = header[2] == 0xFFFFFFFFu;
    const bool draw = header[2] == 0xFFFFFFFEu;
    uint32_t draw_start = 0u;
    uint32_t count = 0u;
    if (pixel && fread(&count, sizeof(count), 1u, input) != 1u) return 2;
    if (draw && (fread(&draw_start, sizeof(draw_start), 1u, input) != 1u ||
                 fread(&count, sizeof(count), 1u, input) != 1u)) return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(header[0], header[1]);
    map_fixed(0x00A00000u, 0x1000u);
    for (uint32_t i = 0u; i < D3D_REGION_BYTES / 4u; i++) {
        uint32_t value;
        if (fread(&value, sizeof(value), 1u, input) != 1u) return 2;
        store(D3D_REGION_BASE + i * 4u, value);
    }
    for (uint32_t i = 0u; i < 64u; i++) {
        uint32_t value;
        if (fread(&value, sizeof(value), 1u, input) != 1u) return 2;
        store(0x00A00000u + i * 4u, value);
    }
    if (pixel || draw) {
        map_fixed(0x00540000u, 0x10000u);
        for (uint32_t i = 0u; i < 60u; i++) {
            uint32_t value;
            if (fread(&value, sizeof(value), 1u, input) != 1u) return 2;
            store(0x00A00100u + i * 4u, value);
        }
        for (uint32_t i = 0u; i < 16u; i++) {
            uint32_t value;
            if (fread(&value, sizeof(value), 1u, input) != 1u) return 2;
            store(0x005496E0u + i * 4u, value);
        }
    }
    if (draw) {
        for (uint32_t i = 0u; i < 8u; i++) {
            uint32_t value;
            if (fread(&value, sizeof(value), 1u, input) != 1u) return 2;
            store(0x005496A0u + i * 4u, value);
        }
    }
    fclose(input);
    const uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    const uint32_t result = draw ? d3d8_draw_vertices(header[3], draw_start, count) : pixel ? d3d8_set_pixel_shader_constants(header[3], 0x00A00000u, count)
                                  : d3d8_set_vertex_shader(header[2], header[3]);
    const uint32_t end = d3d8_device_load32(D3D8_DEV_CURSOR);
    if (fwrite(&result, sizeof(result), 1u, output) != 1u) return 2;
    if (pixel && fwrite(&end, sizeof(end), 1u, output) != 1u) return 2;
    for (uint32_t address = start; address < end; address += 4u) {
        const uint32_t value = load(address);
        if (fwrite(&value, sizeof(value), 1u, output) != 1u) return 2;
    }
    for (uint32_t i = 0u; i < D3D_REGION_BYTES / 4u; i++) {
        const uint32_t value = load(D3D_REGION_BASE + i * 4u);
        if (fwrite(&value, sizeof(value), 1u, output) != 1u) return 2;
    }
    fclose(output);
    environment_end();
    return 0;
}
