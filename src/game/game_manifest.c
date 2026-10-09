/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Prints the registry of the replacements linked into THIS executable, one record per
 * line. It is the only source the coverage metric accepts for "what is replaced": a
 * source scan can be fooled by a commented-out registration or an `#if 0`, a linked
 * registry cannot.
 *
 * Format, tab separated, one function per line, no header:
 *   va  name  convention  stack_args  returns  scratch  source
 * `scratch` is a comma list of the registers the replacement does not promise to match.
 * `tools/replace manifest` runs this and turns it into manifest.json.
 */
#include <stdio.h>
#include <string.h>

#include "game_replace.h"

/* The adapters name the lifter's registers and memory-model offset. This executable runs
 * no guest code, so it supplies its own copies instead of linking the host runtime. */
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebp, g_ebx, g_esi, g_edi;
/* Registry-only runtime supplies the guest direction state used by exact copies. */
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
/* The x87 model the float helpers use; see ftol_helper.c. */
__thread double g_fp_stack[8];
__thread int g_fp_top;
__thread uint16_t g_fp_control_word;
/* The flag bridge words the 0x003CCE80 replacement publishes (T554). */
__thread uint32_t g_flag_bridge_eflags, g_flag_bridge_mask;

static const char *const CONVENTION_NAMES[] = {"cdecl", "stdcall", "thiscall", "fastcall"};

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

int main(void)
{
    size_t count = 0;
    const game_replacement *table = game_replacement_table(&count);
    for (size_t index = 0; index < count; index++) {
        const game_replacement *entry = &table[index];
        if (!game_replacement_valid(entry)) {
            fprintf(stderr, "invalid replacement ABI at index %zu\n", index);
            return 2;
        }
        unsigned mask = game_replacement_scratch(entry);
        static const char *const scratches[] = {
            "", "eax", "ecx", "eax,ecx", "edx", "eax,edx", "ecx,edx", "eax,ecx,edx"
        };
        const char *scratch = scratches[mask];
        printf("0x%08X\t%s\t%s\t%u\t%s\t%s\t%s", entry->va, entry->name,
               CONVENTION_NAMES[entry->convention], (unsigned)entry->stack_args,
               entry->returns_value ? "eax" : "void", scratch, base_name(entry->source));
        if (entry->input_abi != 0u) printf("\tinputs=%s", game_replacement_input_names(entry));
        putchar('\n');
    }
    return 0;
}
