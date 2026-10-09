/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The D3D8 init handlers against the user's REAL executable: its display-mode table, its
 * pitch and format tables, its floats, all at their retail addresses.
 *
 * SKIPS (exit 77) when the executable is not there, which is the normal state of a fresh clone:
 * the XBE is never committed. The synthetic suites (test_d3d8_init and friends) cover the logic in
 * every clone; this one covers the one thing they cannot, that the logic is right over the real
 * data.
 *
 * EVERY EXPECTED VALUE IS A LITERAL THE ORIGINAL LIBRARY CODE PRODUCED, run as machine code under
 * an x86 emulator against a model of the kernel and the NV2A registers (tools/d3dscan/oracle.py),
 * not a number this port computed. The mode lists are checked by a digest (FNV-1a over every
 * dword of every mode) plus the first rows spelled out, because spelling out 36 modes five times
 * would bury the three that matter.
 */

#include "test_d3d8_support.h"

#include <sys/stat.h>

#include "xbe.h"

static uint32_t fnv_dword(uint32_t hash, uint32_t value)
{
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        hash ^= (value >> shift) & 0xFFu;
        hash *= 0x01000193u;
    }
    return hash;
}

static uint8_t *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (data && fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *length = (size_t)size;
    return data;
}

/* The kernel side and the D3D table for one case, with the real image already mapped. */
static void real_environment(kernel_av_pack pack, uint32_t region)
{
    guest_mem_reset();
    d3d8_guest_reset();
    d3d8_resource_reset();
    d3d8_pushbuffer_reset();
    kernel_hle_init();
    kernel_av_reset();
    capture_clear();
    kernel_hle_set_log(capture_printer);
    d3d8_hle_set_log(capture_printer);
    d3d8_hle_set_fatal(catching_fatal);
    (void)kernel_register_all();
    if (!kernel_av_configure(pack, region)) {
        printf("FATAL AV configure failed\n");
        exit(EXIT_FAILURE);
    }

    /* The image is mapped for the whole process, so D3D8's BSS keeps what the previous case left:
     * clear what the cases read. */
    store(D3D8_GLOBAL_AV_CAPABILITIES, 0u);
    for (uint32_t offset = 0u; offset < 0x24A8u; offset += 4u) {
        store(D3D8_DEVICE_BASE + offset, 0u);
    }
    store(D3D8_DEVICE_POINTER_SLOT, 0u);

    d3d8_surface_entry rows[sizeof(suite_addresses) / sizeof(suite_addresses[0])];
    for (size_t index = 0u; index < sizeof(rows) / sizeof(rows[0]); index++) {
        rows[index].address = suite_addresses[index];
        rows[index].name = NULL;
        rows[index].sites = 1u;
    }
    if (!d3d8_hle_init(rows, sizeof(rows) / sizeof(rows[0]))) {
        printf("FATAL d3d8_hle_init failed\n");
        exit(EXIT_FAILURE);
    }
    (void)d3d8_device_register();

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 0x2000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    call_scratch = guest_region_alloc(&request, &status);
}

static uint32_t mode_digest(uint32_t count)
{
    uint32_t hash = 0x811C9DC5u;
    const uint32_t out = SCRATCH_DATA;
    for (uint32_t mode = 0u; mode < count; mode++) {
        uint32_t args[3] = {0u, mode, out};
        CHECK_EQ_U32(call_stdcall(0x003D90B0u, args, 3u), 0u);
        for (uint32_t word = 0u; word < 5u; word++) {
            hash = fnv_dword(hash, load(out + word * 4u));
        }
    }
    return hash;
}

static void check_mode_list(kernel_av_pack pack, uint32_t region, uint32_t expected_count,
                            uint32_t expected_digest)
{
    real_environment(pack, region);
    uint32_t adapter = 0u;
    const uint32_t count = call_stdcall(0x003D9010u, &adapter, 1u);
    CHECK_EQ_U32(count, expected_count);
    CHECK_EQ_U32(mode_digest(count), expected_digest);
}

static void test_mode_lists(void)
{
    /* HDTV NA: 36 modes. The first eight: 640x480 and 720x480 at 60 Hz, progressive (0x40), in
     * the four formats 0x1E, 0x11, 0x1C, 0x12. */
    check_mode_list(KERNEL_AV_PACK_HDTV, KERNEL_AV_REGION_NA, 36u, 0x493408A0u);
    {
        const uint32_t out = SCRATCH_DATA;
        static const uint32_t formats[4] = {0x1E, 0x11, 0x1C, 0x12};
        for (uint32_t mode = 0u; mode < 8u; mode++) {
            uint32_t args[3] = {0u, mode, out};
            CHECK_EQ_U32(call_stdcall(0x003D90B0u, args, 3u), 0u);
            CHECK_EQ_U32(load(out + 0u), mode < 4u ? 0x280u : 0x2D0u);
            CHECK_EQ_U32(load(out + 4u), 0x1E0u);
            CHECK_EQ_U32(load(out + 8u), 0x3Cu);
            CHECK_EQ_U32(load(out + 12u), 0x40u);
            CHECK_EQ_U32(load(out + 16u), formats[mode & 3u]);
        }
    }

    /* Composite and S-Video NA: 24 modes, interlaced (0x20), and the S-Video list is the
     * composite list. */
    check_mode_list(KERNEL_AV_PACK_COMPOSITE, KERNEL_AV_REGION_NA, 24u, 0x60B61689u);
    check_mode_list(KERNEL_AV_PACK_SVIDEO, KERNEL_AV_REGION_NA, 24u, 0x60B61689u);

    /* PAL: 36 modes at 50 Hz. NTSC-J has the same list as NTSC-M. */
    check_mode_list(KERNEL_AV_PACK_HDTV, KERNEL_AV_REGION_REST_OF_WORLD, 36u, 0x12D4F1D0u);
    check_mode_list(KERNEL_AV_PACK_HDTV, KERNEL_AV_REGION_JAPAN, 36u, 0x493408A0u);
}

static void test_create_device_over_the_real_table(void)
{
    real_environment(KERNEL_AV_PACK_HDTV, KERNEL_AV_REGION_NA);
    uint32_t size_args[2] = {0x100000u, 0x10000u};
    (void)call_stdcall(0x003D9210u, size_args, 2u);
    const uint32_t parameters = SCRATCH_DATA;
    write_title_parameters(parameters, 0x140u);
    const uint32_t out = SCRATCH_DATA + 0x200u;
    const uint32_t args[6] = {0u, 1u, 0u, 0u, parameters, out};

    CHECK_EQ_U32(call_stdcall(0x003D9230u, args, 6u), 0u);
    CHECK_EQ_U32(load(out), 0x003E3F60u);
    /* The original CreateDevice records its default execution mode before its fence packet. */
    const uint32_t ring = load(0x003E3F60u + 0x24u);
    CHECK(ring != 0u);
    CHECK_EQ_U32(load(ring), 0x00081E94u);
    CHECK_EQ_U32(load(ring + 4u), 6u);
    CHECK_EQ_U32(load(ring + 8u), 0u); /* initial declaration flags & 1 */
    CHECK_EQ_U32(load(0x003E3AB8u), 0x00FF7F7Fu);
    /* The surface headers, the render target words and the display object, as the original left
     * them for these parameters. */
    CHECK_EQ_U32(load(0x003E5984u), 0x010D0002u);
    CHECK_EQ_U32(load(0x003E5990u), 0x00011229u);
    CHECK_EQ_U32(load(0x003E5994u), 0x271DF27Fu);
    CHECK_EQ_U32(load(0x003E599Cu), 0x01050002u);
    CHECK_EQ_U32(load(0x003E59CCu), 0x010D0002u);
    CHECK_EQ_U32(load(0x003E59D8u), 0x00012E29u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x1A0Cu), 0x128u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x948u), 0x4B7FFFFFu);
    CHECK_EQ_U32(load(0x003E3F60u + 0x94Cu), 0xA00u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x1C30u), 0x88110F01u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x1C34u), 0x12u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x1DDCu), 0x02480104u);
    CHECK_EQ_U32(load(0x003E3F60u + 0x1DE0u), 1u);

    /* Composite NA with the title's progressive flag fails, and without it succeeds: the same
     * answers the original gave for both. */
    real_environment(KERNEL_AV_PACK_COMPOSITE, KERNEL_AV_REGION_NA);
    (void)call_stdcall(0x003D9210u, size_args, 2u);
    const uint32_t composite_parameters = SCRATCH_DATA;
    write_title_parameters(composite_parameters, 0x140u);
    const uint32_t composite_out = SCRATCH_DATA + 0x200u;
    const uint32_t composite_args[6] = {0u, 1u, 0u, 0u, composite_parameters, composite_out};
    CHECK_EQ_U32(call_stdcall(0x003D9230u, composite_args, 6u), 0x80004005u);

    real_environment(KERNEL_AV_PACK_COMPOSITE, KERNEL_AV_REGION_NA);
    (void)call_stdcall(0x003D9210u, size_args, 2u);
    const uint32_t interlaced_parameters = SCRATCH_DATA;
    write_title_parameters(interlaced_parameters, 0x100u);
    const uint32_t interlaced_out = SCRATCH_DATA + 0x200u;
    const uint32_t interlaced_args[6] = {0u, 1u, 0u, 0u, interlaced_parameters, interlaced_out};
    CHECK_EQ_U32(call_stdcall(0x003D9230u, interlaced_args, 6u), 0u);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tmp/oxm-extract/retail/default.xbe";
    size_t length = 0u;
    uint8_t *data = read_file(path, &length);
    if (data == NULL) {
        printf("SKIP: %s is not present (the retail executable is never committed)\n", path);
        return 77;
    }
    xbe_image image;
    memset(&image, 0, sizeof(image));
    if (xbe_parse(data, length, &image) != XBE_OK || xbe_map(&image, data, length) != XBE_OK) {
        printf("FATAL %s could not be parsed or mapped\n", path);
        return EXIT_FAILURE;
    }

    test_mode_lists();
    test_create_device_over_the_real_table();

    xbe_unmap(&image);
    free(data);
    d3d8_hle_set_fatal(NULL);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
