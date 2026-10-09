/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "guest_mem.h"
#include "kernel_call.h"
#include "xgrph_hle.h"
#include "xgrph_object_lifetime.h"

static jmp_buf escape;
static void refuse(uint32_t address, const char *message)
{
    (void)message;
    if (address != 0x003EF236u) exit(3);
    longjmp(escape, 1);
}
int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    const uint32_t base = (uint32_t)strtoul(argv[1], NULL, 0);
    const uint32_t object = (uint32_t)strtoul(argv[2], NULL, 0);
    const uint32_t protection = (uint32_t)strtoul(argv[3], NULL, 0);
    guest_region_request request = {0};
    request.bytes = 4096u;
    request.fixed_base = base;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status;
    if (guest_region_alloc(&request, &status) != base) return 3;
    unsigned char page[4096];
    if (fread(page, 1, sizeof(page), stdin) != sizeof(page)) return 3;
    if (!kernel_guest_write_bytes(base, page, sizeof(page))) return 3;
    if (!guest_region_set_protect(base, 4096u, protection)) return 3;
    const xgrph_surface_entry row = {0x003EF236u, "review fixture", 1u};
    if (!xgrph_hle_init(&row, 1u) || xgrph_object_lifetime_register() != 1u) return 3;
    xgrph_hle_set_fatal(refuse);
    kernel_call_frame frame = {0};
    frame.stack_ptr = base + 2048u;
    frame.stack_limit = base + 4096u;
    if (atoi(argv[4])) kernel_frame_set_registers(&frame, object, 0xDEADBEEFu);
    kernel_call_frame before = frame;
    volatile uint32_t result = 0xFFFFFFFFu;
    int fault = setjmp(escape);
    if (!fault) result = xgrph_hle_call(0x003EF236u, &frame);
    if (!guest_region_set_protect(base, 4096u, PAGE_READWRITE)) return 3;
    if (!kernel_guest_read_bytes(base, page, sizeof(page))) return 3;
    uint32_t header[] = {(uint32_t)fault, result, memcmp(&before, &frame, sizeof(frame)) == 0};
    if (fwrite(header, sizeof(header), 1, stdout) != 1 ||
        fwrite(page, 1, sizeof(page), stdout) != sizeof(page)) return 3;
    xgrph_hle_shutdown();
    guest_mem_reset();
    return 0;
}
