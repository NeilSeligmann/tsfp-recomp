/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_lock.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    if (input == NULL || output == NULL) return 2;
    uint32_t args[9];
    uint8_t header[32], metadata[64], output_initial[16], device[64];
    if (fread(args, sizeof(args), 1u, input) != 1u ||
        fread(header, sizeof(header), 1u, input) != 1u ||
        fread(metadata, sizeof(metadata), 1u, input) != 1u ||
        fread(output_initial, sizeof(output_initial), 1u, input) != 1u ||
        fread(device, sizeof(device), 1u, input) != 1u) return 2;
    uint8_t *payload = malloc(args[5]);
    if (payload == NULL || fread(payload, args[5], 1u, input) != 1u) return 2;
    fclose(input);
    guest_mem_reset();
    d3d8_resource_reset();
    map_fixed(D3D_REGION_BASE, D3D_REGION_BYTES);
    map_fixed(0x00665000u, 0x1000u);
    map_fixed(0x00A00000u, 0x2000u);
    map_fixed(0x00B00000u, 0x1000u);
    guest_region_request request = {.bytes=args[5], .fixed_base=args[6], .contiguous=true,
                                    .state=MEM_COMMIT, .protect=PAGE_READWRITE};
    nt_status status;
    if (guest_region_alloc(&request, &status) != args[6]) return 2;
    memcpy(kernel_guest_at(args[0], sizeof(header)), header, sizeof(header));
    memcpy(kernel_guest_at(0x3E1828u, sizeof(metadata)), metadata, sizeof(metadata));
    memcpy(kernel_guest_at(D3D8_DEVICE_BASE, sizeof(device)), device, sizeof(device));
    memcpy(kernel_guest_at(args[2], sizeof(output_initial)), output_initial, sizeof(output_initial));
    memcpy(kernel_guest_at(args[6], args[5]), payload, args[5]);
    free(payload);
    (void)d3d8_register_resource(args[0], args[6]);
    store(0x3E3F58u, args[7]);
    /* Dispatch through the new actual native handler, not only the typed helper. */
    const d3d8_surface_entry row = {0x003D4E60u, NULL, 1u};
    if (!d3d8_hle_init(&row, 1u) || d3d8_lock_register() != 1u) return 2;
    kernel_call_frame frame;
    if (!kernel_frame_build(&frame, 0xB00000u, 0x100u, args, 5u)) return 2;
    const uint32_t result = d3d8_hle_call(0x003D4E60u, &frame);
    if (fwrite(&result, sizeof(result), 1u, output) != 1u ||
        fwrite(kernel_guest_at(args[2], 16u), 16u, 1u, output) != 1u ||
        fwrite(kernel_guest_at(args[0], 32u), 32u, 1u, output) != 1u ||
        fwrite(kernel_guest_at(D3D8_DEVICE_BASE, 64u), 64u, 1u, output) != 1u ||
        fwrite(kernel_guest_at(args[6], args[5]), args[5], 1u, output) != 1u) return 2;
    fclose(output);
    d3d8_hle_shutdown();
    d3d8_resource_reset();
    guest_mem_reset();
    return 0;
}
