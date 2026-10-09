/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_vsh_select.h"

#include <string.h>

static int find(const struct gpu_vsh_entry *list, uint32_t count, uint32_t id, uint32_t *module)
{
    uint32_t low = 0u;
    uint32_t high = count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2u;
        if (list[middle].id == id) {
            *module = list[middle].module;
            return 1;
        }
        if (list[middle].id < id) low = middle + 1u;
        else high = middle;
    }
    return 0;
}

int gpu_vsh_lookup_key(const struct gpu_vsh_table *table, uint32_t raw_key, uint32_t *module)
{
    return find(table->keys, table->key_count, raw_key & table->key_mask, module);
}

int gpu_vsh_lookup_static(const struct gpu_vsh_table *table, uint32_t guest_address, uint32_t *module)
{
    return find(table->statics, table->static_count, guest_address, module);
}

int gpu_vsh_lookup_name(const struct gpu_vsh_table *table, const char *name, uint32_t *module)
{
    for (uint32_t i = 0u; i < table->module_count; i++) {
        if (strcmp(table->module_names[i], name) == 0) {
            *module = i;
            return 1;
        }
    }
    return 0;
}

static int list_valid(const struct gpu_vsh_entry *list, uint32_t count, uint32_t modules, uint32_t mask)
{
    for (uint32_t i = 0u; i < count; i++) {
        if (list[i].module >= modules) return 0;
        if (i > 0u && list[i - 1u].id >= list[i].id) return 0;
        if ((list[i].id & ~mask) != 0u) return 0;
    }
    return 1;
}

int gpu_vsh_table_valid(const struct gpu_vsh_table *table)
{
    return list_valid(table->keys, table->key_count, table->module_count, table->key_mask) &&
           list_valid(table->statics, table->static_count, table->module_count, 0xFFFFFFFFu);
}
