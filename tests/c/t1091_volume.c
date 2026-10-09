/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_file.h"
#include "kernel_hle.h"
#include "kernel_io.h"
#include "kernel_object.h"
#include "nt_status.h"
#include "xnet_volume.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define BASE 0x20000000u
static int quiet(const char *format, ...) { (void)format; return 0; }
static bool error(void *context, uint32_t status, bool open_phase)
{
    (void)context;
    printf("error %u %u\n", status, (unsigned)open_phase);
    return true; /* Declared NT-error boundary; no fabricated TLS state. */
}
static void dump(const char *name, uint32_t address, uint32_t bytes)
{
    printf("%s ", name);
    const unsigned char *data = kernel_guest_at(address, bytes);
    for (uint32_t i = 0u; i < bytes; ++i) printf("%02x", data[i]);
    putchar('\n');
}
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    const unsigned mode = (unsigned)strtoul(argv[1], NULL, 0);
    const unsigned mask = (unsigned)strtoul(argv[2], NULL, 0);
    const bool alias = strtoul(argv[3], NULL, 0) != 0u;
    kernel_hle_init(); kernel_hle_set_log(quiet);
    kernel_object_reset(); kernel_file_reset(); kernel_io_reset();
    (void)kernel_file_register(); (void)kernel_io_register();
    kernel_file_set_missing_policy(KERNEL_FILE_MISSING_EMPTY); /* Strict query must override per request. */
    if (mode != 3u) (void)kernel_object_register();
    const guest_region_request request = {.bytes=0x10000u,.protect=PAGE_READWRITE,
        .state=MEM_COMMIT,.fixed_base=BASE};
    nt_status status;
    if (guest_region_alloc(&request, &status) != BASE) return 3;
    memset(kernel_guest_at(BASE,0x10000u),0xa5,0x10000u);
    if (!kernel_guest_write_bytes(BASE+0x8000u,"T:",3u)) return 4;
    char directory[]="/tmp/tsfp-t1091-volume-oracle-XXXXXX";
    if (!mkdtemp(directory)) return 5;
    if (mode == 0u) {
        if (!kernel_file_mount_host_dir("\\Device\\Harddisk0\\Partition1",directory) ||
            !kernel_file_add_symlink("\\??\\T:","\\Device\\Harddisk0\\Partition1")) return 6;
    } else if (mode == 2u) {
        if (!kernel_file_mount_host_device("\\Device\\Harddisk0\\Partition1",directory,"raw",4096u) ||
            !kernel_file_add_symlink("\\??\\T:","\\Device\\Harddisk0\\Partition1")) return 7;
    }
    uint32_t output[3];
    for (unsigned i=0u;i<3u;++i)
        output[i]=(mask&(1u<<i)) ? BASE+0x9000u+(alias?0u:i*16u) : 0u;
    xnet_volume_kernel source={BASE+0x1000u,BASE+0x2000u,32u,error,NULL};
    uint32_t result=0xeeeeeeeeu;
    const bool executed=xnet_volume_space_kernel(&source,BASE+0x8000u,output,&result);
    printf("executed %u\nresult %u\n",(unsigned)executed,result);
    dump("output",BASE+0x9000u,48u);
    dump("information",BASE+0x1020u,24u);
    uint32_t handle=0u;
    (void)kernel_guest_read_u32(BASE+0x1014u,&handle);
    printf("closed %u\nfabricated %u %u\n",(unsigned)(kernel_object_find(handle)==NULL),
           kernel_file_fabricated_count(),kernel_io_fabricated_geometry_count());
    kernel_file_reset(); kernel_object_reset(); guest_mem_reset();
    if (mode==2u) {
        char file[256];(void)snprintf(file,sizeof(file),"%s/raw",directory);
        (void)unlink(file);
    }
    if (rmdir(directory)!=0) return 8;
    return 0;
}
