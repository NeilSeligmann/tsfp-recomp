/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A host process for `tools/d3dscan/port_diff.py`: it maps the retail XBE at its guest addresses (so
 * the library's own tables are in memory at their real addresses, exactly as under tsfp_host), sets
 * up the kernel HLE and the AV module the way the host does, registers the D3D8 init handlers, and
 * then does what stdin tells it. One command per line, every reply one line:
 *
 *     w32 ADDR VALUE            write a guest dword
 *     r32 ADDR                  -> ok VALUE
 *     call ADDR ECX EDX N A0..  dispatch a handler through the D3D8 table -> ok EAX | fatal TEXT
 *     dump LO HI PATH           write guest bytes [LO, HI) to a file
 *     load PATH ADDR            read a file into guest memory
 *     quit
 *
 * This is a measurement harness, not part of the host. It deliberately does not link the lifted
 * code or the dispatcher: a handler is called with a synthetic stdcall frame, which is all the
 * handlers see in production too.
 *
 * It is a PIE executable because the guest image is mapped at 0x10000, which a non-PIE host would
 * collide with, exactly as src/loader/xbe.c explains.
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_device.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "guest_mem.h"
#include "kernel_av.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_register_all.h"
#include "nt_status.h"
#include "xbe.h"

#define MAX_ARGUMENTS 16u
#define FRAME_BYTES 0x100u

static jmp_buf fatal_jump;
static int fatal_armed;
static char fatal_text[512];

static void on_fatal(uint32_t address, const char *message)
{
    snprintf(fatal_text, sizeof(fatal_text), "0x%08x: %s", (unsigned)address, message);
    if (fatal_armed) {
        longjmp(fatal_jump, 1);
    }
}

static int quiet(const char *format, ...)
{
    (void)format;
    return 0;
}

static uint32_t next_number(void)
{
    const char *token = strtok(NULL, " \t\r\n");
    return token ? (uint32_t)strtoul(token, NULL, 0) : 0u;
}

/* Every address the init handlers register under, so d3d8_hle_register accepts them. */
static const uint32_t surface_addresses[] = {
    0x3D9000, 0x3D9010, 0x3D90B0, 0x3D9210, 0x3D9230, 0x3D6C90, 0x3D3A80, 0x3D4EE0, 0x3D5EB0,
    0x3D7060, 0x3D7F70, 0x3D8010, 0x3D7150, 0x3D80B0, 0x3D8190, 0x3D81B0, 0x3D7EE0, 0x3D81F0,
    0x3D72A0, 0x3D5AF0, 0x3D5670,
};

static int load_image(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return 0;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        return 0;
    }
    fclose(file);
    static xbe_image image;
    memset(&image, 0, sizeof(image));
    xbe_status status = xbe_parse(data, (size_t)size, &image);
    if (status == XBE_OK) {
        status = xbe_map(&image, data, (size_t)size);
    }
    if (status != XBE_OK) {
        fprintf(stderr, "xbe: %s\n", xbe_status_str(status));
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: port_run XBE AV_PACK CERT_REGION\n");
        return 2;
    }
    if (!load_image(argv[1])) {
        return 2;
    }

    kernel_hle_init();
    kernel_hle_set_log(quiet);
    (void)kernel_register_all();
    d3d8_hle_set_log(quiet);

    static d3d8_surface_entry rows[sizeof(surface_addresses) / sizeof(surface_addresses[0])];
    for (size_t index = 0; index < sizeof(rows) / sizeof(rows[0]); index++) {
        rows[index].address = surface_addresses[index];
        rows[index].name = NULL;
        rows[index].sites = 1u;
    }
    if (!d3d8_hle_init(rows, sizeof(rows) / sizeof(rows[0]))) {
        return 2;
    }
    d3d8_hle_set_fatal(on_fatal);
    if (!kernel_av_configure((kernel_av_pack)atoi(argv[2]), (uint32_t)strtoul(argv[3], NULL, 0))) {
        fprintf(stderr, "kernel_av_configure refused the pack and region\n");
        return 2;
    }
    printf("registered %zu\n", d3d8_device_register());

    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 0x2000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr scratch = guest_region_alloc(&request, &status);
    printf("scratch %u\n", (unsigned)(scratch + 0x1000u));
    fflush(stdout);

    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        const char *command = strtok(line, " \t\r\n");
        if (!command) {
            continue;
        }
        if (!strcmp(command, "quit")) {
            break;
        }
        if (!strcmp(command, "w32")) {
            const uint32_t address = next_number();
            *(volatile uint32_t *)(uintptr_t)address = next_number();
            printf("ok\n");
        } else if (!strcmp(command, "r32")) {
            const uint32_t address = next_number();
            printf("ok %u\n", *(volatile uint32_t *)(uintptr_t)address);
        } else if (!strcmp(command, "dump")) {
            const uint32_t low = next_number();
            const uint32_t high = next_number();
            FILE *out = fopen(strtok(NULL, " \t\r\n"), "wb");
            fwrite((void *)(uintptr_t)low, 1, high - low, out);
            fclose(out);
            printf("ok\n");
        } else if (!strcmp(command, "load")) {
            const char *path = strtok(NULL, " \t\r\n");
            const uint32_t address = next_number();
            FILE *in = fopen(path, "rb");
            fseek(in, 0, SEEK_END);
            const long size = ftell(in);
            fseek(in, 0, SEEK_SET);
            const size_t got = fread((void *)(uintptr_t)address, 1, (size_t)size, in);
            fclose(in);
            printf(got == (size_t)size ? "ok\n" : "fatal short read\n");
        } else if (!strcmp(command, "call")) {
            const uint32_t address = next_number();
            const uint32_t ecx = next_number();
            const uint32_t edx = next_number();
            const unsigned count = next_number();
            uint32_t args[MAX_ARGUMENTS];
            for (unsigned index = 0; index < count && index < MAX_ARGUMENTS; index++) {
                args[index] = next_number();
            }
            kernel_call_frame frame;
            memset(&frame, 0, sizeof(frame));
            if (!kernel_frame_build(&frame, scratch, FRAME_BYTES, args, count)) {
                printf("fatal could not build a frame\n");
                continue;
            }
            kernel_frame_set_registers(&frame, ecx, edx);
            fatal_text[0] = '\0';
            fatal_armed = 1;
            if (setjmp(fatal_jump) != 0) {
                fatal_armed = 0;
                printf("fatal %s\n", fatal_text);
                fflush(stdout);
                continue;
            }
            const uint32_t eax = d3d8_hle_call(address, &frame);
            fatal_armed = 0;
            printf("ok %u\n", eax);
        } else {
            printf("fatal unknown command %s\n", command);
        }
        fflush(stdout);
    }
    return 0;
}
