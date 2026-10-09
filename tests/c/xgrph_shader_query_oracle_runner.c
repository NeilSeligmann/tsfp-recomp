/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xgrph_shader_query.h"
#include "xgrph_hle.h"
#include "kernel_call.h"
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>

static uint32_t mapped;
uint32_t shader_query_prepare(void)
{
    if (mapped != 0u) { return 0u; }
    void *memory = mmap(NULL, 4096u, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (memory == MAP_FAILED) { return 0; }
    const uintptr_t address = (uintptr_t)memory;
    if (address == 0u || address > UINT32_MAX - 4096u) {
        (void)munmap(memory, 4096u);
        return 0u;
    }
    mapped = (uint32_t)address;
    const xgrph_surface_entry entry = {0x3E9546u, "XGSUCode_GetVertexShaderLength", 1u};
    if (!xgrph_hle_init(&entry, 1u) || xgrph_shader_query_register() != 1u) {
        xgrph_hle_shutdown();
        (void)munmap(memory, 4096u);
        mapped = 0u;
        return 0u;
    }
    return mapped;
}
int shader_query_run(uint32_t registers[8], uint8_t state[4096])
{
    if (registers[7] != mapped) { return 0; }
    memcpy((void *)(uintptr_t)mapped, state, 4096u);
    kernel_call_frame frame = {0};
    frame.stack_ptr = registers[7];
    frame.stack_limit = mapped + 4096u;
    const uint32_t result = xgrph_hle_call(0x3E9546u, &frame);
    registers[0] = result;
    registers[7] += 8u; /* Standard stdcall1 adapter, actual thunk tested separately. */
    memcpy(state, (const void *)(uintptr_t)mapped, 4096u);
    return 1;
}
void shader_query_finish(void)
{
    xgrph_hle_shutdown();
    if (mapped != 0u) { (void)munmap((void *)(uintptr_t)mapped, 4096u); mapped = 0u; }
}
