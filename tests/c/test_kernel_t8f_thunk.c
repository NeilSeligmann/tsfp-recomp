/* SPDX-License-Identifier: GPL-3.0-or-later
 * T8f thunk pops. Each ABI row in src/host/kernel_thunk.c is checked through the real
 * dispatcher: the thunk must pop the return address plus exactly the measured argument
 * bytes, and the handler must have run (observed through its own effect).
 *   195 NtDeleteFile(oa)                      ONE stdcall argument, pop 4
 *   228 NtSetSystemTime(&time, NULL)          TWO stdcall arguments, pop 8
 */
#include "guest_mem.h"
#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_event.h"
#include "kernel_file.h"
#include "kernel_hle.h"
#include "kernel_io.h"
#include "kernel_thunk.h"
#include "recomp_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

#define RETURN_ADDRESS 0x004228F0u

/* Push `argument_count` dwords, call the thunk, and report whether esp ended exactly
 * `4 + 4 * argument_count` above where the call started (the pop of a `ret N`). */
static bool call_thunk(unsigned ordinal, uint32_t stack, const uint32_t *arguments,
                       unsigned argument_count)
{
    recomp_func_t fn = recomp_lookup_kernel(KERNEL_THUNK_VA(ordinal));
    if (fn == NULL || !kernel_guest_write_u32(stack, RETURN_ADDRESS)) {
        return false;
    }
    for (unsigned i = 0u; i < argument_count; i++) {
        if (!kernel_guest_write_u32(stack + 4u + 4u * i, arguments[i])) {
            return false;
        }
    }
    g_esp = stack;
    fn();
    return g_esp == stack + 4u + 4u * argument_count;
}

int main(void)
{
    const guest_region_request request = {
        .bytes = 4096u, .alignment = 4096u,
        .protect = PAGE_READWRITE, .state = MEM_COMMIT,
    };
    nt_status status = STATUS_SUCCESS;
    const uint32_t stack = guest_region_alloc(&request, &status);
    if (stack == 0u) {
        return 1;
    }
    kernel_hle_init();
    kernel_file_reset();
    kernel_file_unmount_all();
    bool okay = true;

    /* 195: delete a real host file through the real thunk, with the OBJECT_ATTRIBUTES laid out
     * as the closer builds it (root 0, name, attributes 0x40). One argument is popped and the
     * file is really gone. */
    okay = kernel_file_register() == 7u && okay;
    char directory[] = "tsfp-t8f-thunk-XXXXXX";
    okay = mkdtemp(directory) != NULL && okay;
    okay = kernel_file_mount_host_dir("\\Device\\Harddisk0\\partition1", directory) && okay;
    char file[256];
    (void)snprintf(file, sizeof(file), "%s/LocalCache02.bin", directory);
    FILE *created = fopen(file, "wb");
    okay = created != NULL && okay;
    if (created != NULL) {
        (void)fclose(created);
    }
    static const char name[] = "\\Device\\Harddisk0\\partition1\\LocalCache02.bin";
    const uint32_t text = stack + 0x400u;
    const uint32_t descriptor = stack + 0x380u;
    const uint32_t object_attributes = stack + 0x390u;
    okay = kernel_guest_write_bytes(text, name, sizeof(name) - 1u) && okay;
    const guest_object_string ansi = {(uint16_t)(sizeof(name) - 1u), (uint16_t)sizeof(name), text};
    okay = kernel_guest_write_bytes(descriptor, &ansi, sizeof(ansi)) && okay;
    okay = kernel_guest_write_u32(object_attributes, 0u) && okay;
    okay = kernel_guest_write_u32(object_attributes + 4u, descriptor) && okay;
    okay = kernel_guest_write_u32(object_attributes + 8u, 0x40u) && okay;
    const uint32_t arguments[1] = {object_attributes};
    g_eax = 0xDEADBEEFu;
    okay = call_thunk(195u, stack, arguments, 1u) && okay;
    okay = g_eax == STATUS_SUCCESS && access(file, F_OK) != 0 &&
           kernel_file_deleted_count() == 1u && okay;
    /* The same call again is an absence, and still pops exactly one argument. */
    g_eax = 0xDEADBEEFu;
    okay = call_thunk(195u, stack, arguments, 1u) && okay;
    okay = g_eax == KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND && okay;

    /* 228: the wrapper's shape, OldTime NULL. Two arguments are popped (12 bytes in all), the call
     * succeeds and the guest wall clock really moved to the time passed (read back through the
     * C accessor, so the check does not depend on the host clock to the second). */
    kernel_event_reset();
    okay = kernel_event_register() == 11u && okay;
    const uint64_t new_time = 0x01C5601234567890ull;
    okay = kernel_guest_write_u32(stack + 0x600u, (uint32_t)(new_time & 0xFFFFFFFFu)) && okay;
    okay = kernel_guest_write_u32(stack + 0x604u, (uint32_t)(new_time >> 32)) && okay;
    const uint32_t time_arguments[2] = {stack + 0x600u, 0u};
    g_eax = 0xDEADBEEFu;
    okay = call_thunk(228u, stack, time_arguments, 2u) && okay;
    const uint64_t clock = kernel_event_system_time();
    okay = g_eax == STATUS_SUCCESS && kernel_event_system_time_set_count() == 1u &&
           clock >= new_time && clock - new_time < 50000000ull && okay;
    kernel_event_reset();

    kernel_file_unmount_all();
    kernel_file_reset();
    guest_mem_reset();
    (void)rmdir(directory);
    puts(okay ? "kernel t8f thunk: passed" : "kernel t8f thunk: FAILED");
    return okay ? 0 : 1;
}
