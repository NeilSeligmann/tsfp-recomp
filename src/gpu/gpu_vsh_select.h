/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_VSH_SELECT_H
#define TSFP_GPU_VSH_SELECT_H
#include <stdint.h>

/* Runtime selection of a static vertex-shader module. The table is emitted by
 * tools/nv2a/vsh_modules.py into gitignored output (vsh_table.inc) so no program
 * derived from the user's executable is committed. Lookups are EXACT: a key or address
 * that is not in the table is a miss, never a nearest match. */
struct gpu_vsh_entry {
    uint32_t id; /* key (generated programs) or guest address (static programs) */
    uint32_t module; /* index into module_names */
};

struct gpu_vsh_table {
    uint32_t key_mask; /* the effective bits of the builder key */
    uint32_t key_count;
    const struct gpu_vsh_entry *keys; /* sorted ascending by id, ids unique */
    uint32_t static_count;
    const struct gpu_vsh_entry *statics; /* sorted ascending by id, ids unique */
    uint32_t module_count;
    const char *const *module_names; /* SPIR-V file stem under spv/ */
};

/* The raw builder key is masked to the effective bits first. Returns 1 and sets
 * *module on a hit, 0 on a miss (module untouched). */
int gpu_vsh_lookup_key(const struct gpu_vsh_table *table, uint32_t raw_key, uint32_t *module);
int gpu_vsh_lookup_static(const struct gpu_vsh_table *table, uint32_t guest_address, uint32_t *module);
/* EXACT lookup of a module by its file stem (`static_<sha256>` or `generated_<sha256>`): the
 * corpus names a module after the digest of its program, so this is how a program found by its
 * bytes (the pushbuffer replay, T84) reaches its module. Returns 1 and sets *module, or 0. */
int gpu_vsh_lookup_name(const struct gpu_vsh_table *table, const char *name, uint32_t *module);
/* 1 when both lists are strictly sorted, every module index is in range and every key is
 * inside key_mask. */
int gpu_vsh_table_valid(const struct gpu_vsh_table *table);
#endif
