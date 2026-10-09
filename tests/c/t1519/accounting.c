/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#include <assert.h>
#include <sys/mman.h>
extern __thread uint32_t g_ebx;
int main(void)
{
    void *arena = mmap((void *)0xD00000, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(arena == (void *)0xD00000);
    size_t count;
    const game_replacement *entries = game_replacement_table(&count);
    assert(count == 9);
    for (size_t i = 0; i < count; ++i) {
        g_esp = 0xD00100; g_ebx = 0xD00200; g_ecx = 3; g_esi = 5;
        uint64_t before = entries[i].calls;
        entries[i].adapter();
        assert(entries[i].calls == before + 1);
    }
    assert(munmap(arena, 4096) == 0);
    return 0;
}
