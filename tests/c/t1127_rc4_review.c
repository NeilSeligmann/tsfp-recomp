/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent finite guest-boundary runner. Production is never edited. */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "kernel_call.h"
#include "kernel_crypto.h"
#include "kernel_hle.h"
#define BASE 0x00A00000u
#define BYTES 12288u
static sigjmp_buf escape;
static volatile sig_atomic_t fault;
static void caught(int signal_number)
{
    fault = signal_number;
    siglongjmp(escape, 1);
}
int main(int argc, char **argv)
{
    if (argc != 8) return 2;
    const unsigned mode = (unsigned)strtoul(argv[1], NULL, 0);
    const uint32_t state = (uint32_t)strtoul(argv[2], NULL, 0);
    const uint32_t length = (uint32_t)strtoul(argv[3], NULL, 0);
    const uint32_t data = (uint32_t)strtoul(argv[4], NULL, 0);
    const int page = atoi(argv[5]), protection = atoi(argv[6]);
    const uint32_t limit = (uint32_t)strtoul(argv[7], NULL, 0);
    unsigned char *memory = mmap((void *)(uintptr_t)BASE, BYTES, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (memory != (void *)(uintptr_t)BASE) return 3;
    if (fread(memory, 1, BYTES, stdin) != BYTES) return 3;
    kernel_call_frame frame = {0};
    frame.stack_ptr = BASE + BYTES - 256u;
    frame.stack_limit = limit == 0u ? 0u : frame.stack_ptr + limit;
    frame.ecx = 0x12345678u; frame.edx = 0x87654321u; frame.has_registers = true;
    frame.result_high = 0xAABBCCDDu; frame.has_result_high = true;
    const kernel_call_frame before = frame;
    if (mode >= 2u) {
        kernel_hle_init();
        if (kernel_crypto_register() != 7u) return 3;
        uint32_t words[] = {0x00345678u, state, length, data};
        memcpy(memory + BYTES - 256u, words, sizeof(words));
    }
    struct sigaction action = {0};
    action.sa_handler = caught;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL) != 0 || sigaction(SIGBUS, &action, NULL) != 0)
        return 3;
    if (page >= 0) {
        kernel_guest_probe_change_begin();
        if (page > 2 || mprotect(memory + (unsigned)page * 4096u, 4096u, protection) != 0)
            return 3;
        kernel_guest_probe_cache_flush();
    }
    volatile uint32_t status = 0xDEADBEEFu;
    if (sigsetjmp(escape, 1) == 0) {
        if (mode == 0u) status = kernel_crypto_rc4_key_guest(state, length, data);
        else if (mode == 1u) status = kernel_crypto_rc4_crypt_guest(state, length, data);
        else status = kernel_hle_call(mode == 2u ? 338u : 339u,
                                     mode == 4u ? NULL : &frame);
    }
    kernel_guest_probe_change_begin();
    if (mprotect(memory, BYTES, PROT_READ | PROT_WRITE) != 0) return 3;
    kernel_guest_probe_cache_flush();
    uint32_t header[] = {status, (uint32_t)fault, memcmp(&before, &frame, sizeof(frame)) == 0u};
    if (fwrite(header, sizeof(header), 1, stdout) != 1 ||
        fwrite(memory, 1, BYTES, stdout) != BYTES) return 3;
    if (munmap(memory, BYTES) != 0) return 3;
    return 0;
}
