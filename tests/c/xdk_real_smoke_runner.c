/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T86: the always-running real-image frame over EVERY registered GPU handler.
 *
 * test_d3d8_init_real.c proves a handful of init handlers over the user's real
 * executable. This runner is the frame that extends at least a smoke-tier check
 * to every D3D and XGRPH address with a registered C handler, with no per-address
 * list anywhere: the set is enumerated from the adopted measured surface plus the
 * live registries, so a newly ported address joins the sweep the moment its
 * register call lands, without anyone editing a test.
 *
 * Registration mirrors the HOST (src/host/main.c), not src/xbox/xdk_report.c,
 * for the two GPU registries: adopt the measured surface (src/xbox/xdk_surface.c,
 * generated and gitignored), then d3d8_device_register, d3d8_target_query_register
 * and the three XGRPH registrars. xdk_report.c predates the target-query and
 * shader-query registrars and under-reports by three addresses, which this harness
 * is exactly the frame to catch. The real image is mapped at its retail addresses
 * first, exactly as in test_d3d8_init_real.c, so every handler that reads the
 * library's tables reads the REAL bytes.
 *
 * Modes:
 *   runner <default.xbe> list
 *       Print one REGISTERED line per implemented D3D/XGRPH surface address.
 *       This is the dynamic enumeration axis the pytest wrapper compares against
 *       an independent static extraction of the register calls in the C sources.
 *   runner <default.xbe> smoke 0xADDRESS
 *       Dispatch the address once through the real dispatcher with a zeroed
 *       8-argument stdcall frame and zeroed fastcall registers over the freshly
 *       mapped image. The smoke contract is NOT that the call succeeds. It is
 *       that dispatch lands on the registered C handler and the handler either
 *       returns a value or refuses through the guarded fatal hook. A crash, an
 *       abort outside the hook, a dispatch that misses the registry, or an
 *       unknown-address fallthrough is the failure the frame exists to catch.
 *
 * Exits 77 with a SKIP line naming the missing file when the executable is not
 * present, which is the normal state of a fresh clone.
 */

#include "test_d3d8_support.h"

#include "d3d8_surface_adapter.h"
#include "d3d8_target_query.h"
#include "xbe.h"
#include "xdk_surface.h"
#include "xgrph_hle.h"
#include "xgrph_shader_query.h"
#include "xgrph_swizzle.h"
#include "xgrph_texture.h"

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

/* The kernel side, the adopted measured surface and BOTH GPU registries, with the
 * real image already mapped. The BSS the init suites clear is cleared here too, so
 * a handler that reads device state sees the zeroed pre-init state rather than
 * whatever the loader left. */
static void real_environment(void)
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
    xgrph_hle_set_log(capture_printer);
    d3d8_hle_set_fatal(catching_fatal);
    xgrph_hle_set_fatal(catching_fatal);
    (void)kernel_register_all();
    if (!kernel_av_configure(KERNEL_AV_PACK_HDTV, KERNEL_AV_REGION_NA)) {
        printf("FATAL AV configure failed\n");
        exit(EXIT_FAILURE);
    }

    store(D3D8_GLOBAL_AV_CAPABILITIES, 0u);
    for (uint32_t offset = 0u; offset < 0x24A8u; offset += 4u) {
        store(D3D8_DEVICE_BASE + offset, 0u);
    }
    store(D3D8_DEVICE_POINTER_SLOT, 0u);

    d3d8_xdk_row rows[XDK_SURFACE_COUNT];
    for (size_t index = 0u; index < XDK_SURFACE_COUNT; index++) {
        rows[index].address = xdk_surface[index].address;
        rows[index].section = xdk_surface[index].section;
        rows[index].name = xdk_surface[index].name;
        rows[index].sites = xdk_surface[index].sites;
    }
    if (!d3d8_surface_adopt(rows, XDK_SURFACE_COUNT, D3D8_SECTION_D3D)) {
        printf("FATAL could not adopt the measured D3D surface\n");
        exit(EXIT_FAILURE);
    }
    (void)d3d8_device_register();
    (void)d3d8_target_query_register();
    if (!xgrph_hle_adopt((const xgrph_xdk_row *)xdk_surface, XDK_SURFACE_COUNT)) {
        printf("FATAL could not adopt the measured XGRPH surface\n");
        exit(EXIT_FAILURE);
    }
    (void)xgrph_texture_register();
    (void)xgrph_swizzle_register();
    (void)xgrph_shader_query_register();

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 0x2000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    call_scratch = guest_region_alloc(&request, &status);
    if (call_scratch == 0u) {
        printf("FATAL no scratch\n");
        exit(EXIT_FAILURE);
    }
}

/* One line per implemented GPU address, straight from the live registries. */
static int list_registered(void)
{
    size_t printed = 0u;
    for (size_t index = 0u; index < XDK_SURFACE_COUNT; index++) {
        const xdk_surface_entry *row = &xdk_surface[index];
        if (strcmp(row->section, "D3D") == 0) {
            const d3d8_entry *entry = d3d8_hle_entry(row->address);
            if (entry != NULL && entry->state == D3D8_ENTRY_IMPLEMENTED) {
                printf("REGISTERED 0x%08X D3D8 %s\n", row->address,
                       row->name != NULL ? row->name : "?");
                printed++;
            }
        } else if (strcmp(row->section, "XGRPH") == 0) {
            const xgrph_entry *entry = xgrph_hle_entry(row->address);
            if (entry != NULL && entry->state == XGRPH_ENTRY_IMPLEMENTED) {
                printf("REGISTERED 0x%08X XGRPH %s\n", row->address,
                       row->name != NULL ? row->name : "?");
                printed++;
            }
        }
    }
    printf("REGISTERED-COUNT %zu\n", printed);
    return 0;
}

static int smoke(uint32_t address)
{
    const xdk_surface_entry *row = xdk_surface_find(address);
    if (row == NULL) {
        printf("VERDICT 0x%08X NOT-IN-SURFACE\n", address);
        return 3;
    }
    const bool is_d3d = strcmp(row->section, "D3D") == 0;
    const bool is_xgrph = strcmp(row->section, "XGRPH") == 0;
    if (!is_d3d && !is_xgrph) {
        printf("VERDICT 0x%08X NOT-A-GPU-SECTION %s\n", address, row->section);
        return 3;
    }
    if (is_d3d) {
        const d3d8_entry *entry = d3d8_hle_entry(address);
        if (entry == NULL || entry->state != D3D8_ENTRY_IMPLEMENTED) {
            printf("VERDICT 0x%08X NOT-REGISTERED\n", address);
            return 4;
        }
    } else {
        const xgrph_entry *entry = xgrph_hle_entry(address);
        if (entry == NULL || entry->state != XGRPH_ENTRY_IMPLEMENTED) {
            printf("VERDICT 0x%08X NOT-REGISTERED\n", address);
            return 4;
        }
    }

    /* A zeroed frame: eight zero stack arguments and zero fastcall registers.
     * Zero is a null device, a null pointer and a zero handle everywhere this
     * boundary looks, so a guarded handler refuses it and an unguarded one that
     * dereferences it crashes, which is exactly the distinction worth testing. */
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    uint32_t args[8] = {0u};
    if (!kernel_frame_build(&frame, call_scratch, 0x100u, args, 8u)) {
        printf("FATAL frame\n");
        exit(EXIT_FAILURE);
    }
    kernel_frame_set_registers(&frame, 0u, 0u);

    uint32_t result = 0u;
    if (is_d3d) {
        RUN_EXPECTING_FATAL(result = d3d8_hle_call(address, &frame));
    } else {
        RUN_EXPECTING_FATAL(result = xgrph_hle_call(address, &frame));
    }
    if (fatal_seen) {
        printf("VERDICT 0x%08X REFUSED %s\n", address, fatal_text);
        fputs(captured, stderr);
        return 0;
    }
    const uint64_t unknown = is_d3d ? d3d8_hle_unknown_count() : xgrph_hle_unknown_count();
    const uint64_t called = is_d3d ? d3d8_hle_entry(address)->call_count
                                   : xgrph_hle_entry(address)->call_count;
    if (unknown != 0u || called != 1u) {
        printf("VERDICT 0x%08X MISDISPATCHED unknown=%llu call_count=%llu\n", address,
               (unsigned long long)unknown, (unsigned long long)called);
        fputs(captured, stderr);
        return 5;
    }
    printf("VERDICT 0x%08X RETURNED 0x%08X\n", address, result);
    fputs(captured, stderr);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <default.xbe> list | <default.xbe> smoke 0xADDRESS\n", argv[0]);
        return 2;
    }
    size_t length = 0u;
    uint8_t *data = read_file(argv[1], &length);
    if (data == NULL) {
        printf("SKIP: %s is not present (the retail executable is never committed)\n", argv[1]);
        return 77;
    }
    xbe_image image;
    memset(&image, 0, sizeof(image));
    if (xbe_parse(data, length, &image) != XBE_OK || xbe_map(&image, data, length) != XBE_OK) {
        printf("FATAL %s could not be parsed or mapped\n", argv[1]);
        return EXIT_FAILURE;
    }

    real_environment();

    int code = 2;
    if (strcmp(argv[2], "list") == 0) {
        code = list_registered();
    } else if (strcmp(argv[2], "smoke") == 0 && argc == 4) {
        code = smoke((uint32_t)strtoul(argv[3], NULL, 16));
    } else {
        fprintf(stderr, "unknown mode %s\n", argv[2]);
    }

    xbe_unmap(&image);
    free(data);
    d3d8_hle_set_fatal(NULL);
    xgrph_hle_set_fatal(NULL);
    return code;
}
