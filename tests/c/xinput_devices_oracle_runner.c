/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
static const uint32_t declarations[8] = {
    0x46C6E0u,8u,0x46C8A0u,4u,0x46C894u,4u,0x46C75Cu,4u
};
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    uint32_t vector[8];
    if (input == NULL || output == NULL || fread(vector, sizeof(vector), 1u, input) != 1u)
        return 2;
    fclose(input);
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init();
    xinput_devices_reset();
    map_fixed(0x46C000u, 0x1000u);
    map_fixed(0x771000u, 0x1000u);
    memcpy(kernel_guest_at(SCRATCH_DATA, 32u), declarations, 32u);
    /* Nonzero incoming public status inputs are explicitly normalized by EMPTY Init. */
    store(0x771454u, 0xFFFFFFFFu);
    CHECK(kernel_guest_write_u8(0x771370u, 0xFFu));
    uint32_t result = xinput_devices_init_empty(4u, SCRATCH_DATA);
    const uint32_t type = vector[1];
    uint8_t *table = kernel_guest_at(type, 28u);
    if (table == NULL) return 2;
    memset(table, 0xA5u, 28u);
    memcpy(table, &vector[2], 12u);
    const uint32_t first = SCRATCH_DATA + 0x100u, second = SCRATCH_DATA + 0x120u;
    memset(kernel_guest_at(first, 16u), 0xAAu, 16u);
    memset(kernel_guest_at(second, 16u), 0xBBu, 16u);
    if (vector[0] == 1u) result = xinput_devices_get(type);
    else if (vector[0] == 2u)
        result = xinput_devices_peek(type, (vector[5] & 1u) ? first : 0u,
            (vector[5] & 2u) ? second : 0u);
    else if (vector[0] == 3u) result = xinput_devices_changes(type, first, second);
    else if (vector[0] == 4u) {
        store(0x771454u, vector[6]);
        CHECK(kernel_guest_write_u8(0x771370u, (uint8_t)vector[7]));
        result = xinput_devices_enumeration_status();
    }
    if (fwrite(&result, 4u, 1u, output) != 1u || fwrite(table, 28u, 1u, output) != 1u ||
        fwrite(kernel_guest_at(first, 16u), 16u, 1u, output) != 1u ||
        fwrite(kernel_guest_at(second, 16u), 16u, 1u, output) != 1u ||
        fwrite(kernel_guest_at(SCRATCH_DATA, 32u), 32u, 1u, output) != 1u ||
        fwrite(kernel_guest_at(0x771454u, 4u), 4u, 1u, output) != 1u ||
        fwrite(kernel_guest_at(0x771370u, 1u), 1u, 1u, output) != 1u) return 2;
    fclose(output);
    /* Session reset leaves registry/counters and explicit policy untouched. */
    CHECK_EQ_U32(xinput_devices_register(), 5u);
    const uint64_t calls = xinput_hle_entry(0x46DBCDu)->call_count;
    CHECK(xinput_hle_attach_synthetic_pad(2u));
    xinput_devices_reset();
    CHECK(xinput_hle_entry(0x46DBCDu)->call_count == calls);
    CHECK(xinput_hle_entry(0x46DBCDu)->state == XINPUT_ENTRY_IMPLEMENTED);
    CHECK(xinput_hle_port_connected(2u));
    environment_end();
    return failures == 0 ? 0 : 1;
}
