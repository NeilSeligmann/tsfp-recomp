/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_vertex_program.h"
#include <sys/mman.h>

#define STREAM 0x00A10000u
#define STREAM_BYTES 8192u
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    if (input == NULL || output == NULL) return 2;
    uint32_t args[4];
    uint8_t device[D3D_REGION_BYTES], stream[STREAM_BYTES], source[4u + 136u * 16u];
    if (fread(args, sizeof(args), 1u, input) != 1u || args[2] > sizeof(source) ||
        fread(device, sizeof(device), 1u, input) != 1u ||
        fread(stream, sizeof(stream), 1u, input) != 1u ||
        fread(source, args[2], 1u, input) != 1u) return 2;
    fclose(input);
    guest_mem_reset();
    d3d8_guest_reset();
    d3d8_pushbuffer_reset();
    map_fixed(D3D_REGION_BASE, D3D_REGION_BYTES);
    map_fixed(0x00A00000u, 0x2000u);
    map_fixed(STREAM, STREAM_BYTES);
    map_fixed(0x004B0000u, 0x10000u);
    memcpy(kernel_guest_at(D3D_REGION_BASE, sizeof(device)), device, sizeof(device));
    memcpy(kernel_guest_at(STREAM, sizeof(stream)), stream, sizeof(stream));
    memcpy(kernel_guest_at(args[0], args[2]), source, args[2]);
    if (args[3] == 2u) {
        if (mprotect((void *)(uintptr_t)(args[0] & ~4095u), 4096u, PROT_NONE) != 0) return 2;
    } else if (args[3] != 0u) {
        const uint32_t page = args[0] & ~4095u;
        const uint32_t bytes = (args[0] + args[2] - page + 4095u) & ~4095u;
        if (mprotect((void *)(uintptr_t)page, bytes, PROT_READ) != 0) return 2;
    }
    const uint32_t result = d3d8_upload_vertex_program(args[0], args[1]);
    if (args[3] == 2u &&
        mprotect((void *)(uintptr_t)(args[0] & ~4095u), 4096u, PROT_READ | PROT_WRITE) != 0) return 2;
    const uint64_t written = d3d8_pushbuffer_dwords_written();
    if (fwrite(&result, sizeof(result), 1u, output) != 1u ||
        fwrite(kernel_guest_at(D3D_REGION_BASE, sizeof(device)), sizeof(device), 1u, output) != 1u ||
        fwrite(kernel_guest_at(STREAM, sizeof(stream)), sizeof(stream), 1u, output) != 1u ||
        fwrite(kernel_guest_at(args[0], args[2]), args[2], 1u, output) != 1u ||
        fwrite(&written, sizeof(written), 1u, output) != 1u) return 2;
    fclose(output);
    guest_mem_reset();
    return 0;
}
