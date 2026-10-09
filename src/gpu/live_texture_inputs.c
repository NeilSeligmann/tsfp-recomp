/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_texture_inputs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 512u
#define REFUSAL_BYTES 160u

typedef struct {
    live_texture_binding binding;
    uint32_t bytes;
    bool resolved;      /* the resolver accepted */
    uint32_t address;   /* resolved read address */
    uint64_t identity;
    char refusal[REFUSAL_BYTES];
    bool has_bytes;     /* the reader succeeded */
    size_t offset;
} entry;

struct live_texture_inputs {
    entry entries[MAX_ENTRIES];
    size_t count;
    uint8_t *arena;
    size_t used, budget;
    uint64_t misses;
};

live_texture_inputs *live_texture_inputs_create(size_t budget_bytes)
{
    live_texture_inputs *inputs = calloc(1u, sizeof *inputs);
    if (inputs == NULL) {
        return NULL;
    }
    inputs->arena = malloc(budget_bytes != 0u ? budget_bytes : 1u);
    if (inputs->arena == NULL) {
        free(inputs);
        return NULL;
    }
    inputs->budget = budget_bytes;
    return inputs;
}

void live_texture_inputs_destroy(live_texture_inputs *inputs)
{
    if (inputs != NULL) {
        free(inputs->arena);
        free(inputs);
    }
}

void live_texture_inputs_reset(live_texture_inputs *inputs)
{
    inputs->count = 0u;
    inputs->used = 0u;
}

static bool same_binding(const live_texture_binding *left, const live_texture_binding *right)
{
    return left->header == right->header && left->format == right->format && left->size_word == right->size_word &&
           left->data == right->data && left->mip_limit == right->mip_limit;
}

static const entry *find_binding(const live_texture_inputs *inputs, const live_texture_binding *binding, uint32_t bytes)
{
    for (size_t index = 0u; index < inputs->count; index++) {
        if (inputs->entries[index].bytes == bytes && same_binding(&inputs->entries[index].binding, binding)) {
            return &inputs->entries[index];
        }
    }
    return NULL;
}

bool live_texture_inputs_names_target(const live_texture_cache *cache, const live_texture_binding *binding, uint32_t source_bytes)
{
    for (size_t index = 0u; index < LIVE_TEXTURE_TARGETS; index++) {
        const live_texture_target *target = &cache->targets[index];
        if (!target->used) {
            continue;
        }
        if (target->data == binding->data ||
            ((uint64_t)binding->data < (uint64_t)target->data + (uint64_t)target->pitch * target->height &&
             (uint64_t)target->data < (uint64_t)binding->data + source_bytes)) {
            return true;
        }
    }
    return false;
}

bool live_texture_inputs_capture(live_texture_inputs *inputs, const live_texture_cache *cache, const live_texture_binding *binding,
                                 live_texture_resolver resolver, void *resolver_context, live_texture_reader reader,
                                 void *reader_context)
{
    live_texture_plan plan;
    if (!live_texture_plan_binding(binding, cache->allow_inferred, &plan)) {
        return true; /* the lookup refuses before any read */
    }
    if (live_texture_inputs_names_target(cache, binding, plan.source_bytes)) {
        return true; /* a render target or an alias of one: the lookup reads no guest bytes */
    }
    if (find_binding(inputs, binding, plan.source_bytes) != NULL) {
        return true;
    }
    if (inputs->count == MAX_ENTRIES) {
        return false;
    }
    entry *slot = &inputs->entries[inputs->count];
    memset(slot, 0, sizeof *slot);
    slot->binding = *binding;
    slot->bytes = plan.source_bytes;
    const char *refusal = NULL;
    uint32_t address = binding->data;
    uint64_t identity = 0u;
    slot->resolved = resolver == NULL || resolver(resolver_context, binding, plan.source_bytes, &address, &identity, &refusal);
    if (!slot->resolved) {
        snprintf(slot->refusal, sizeof slot->refusal, "%s", refusal != NULL ? refusal : "resource resolution failed");
        inputs->count++;
        return true;
    }
    slot->address = address;
    slot->identity = identity;
    if (inputs->used + plan.source_bytes > inputs->budget) {
        return false;
    }
    slot->offset = inputs->used;
    slot->has_bytes = reader != NULL && reader(reader_context, address, inputs->arena + slot->offset, plan.source_bytes);
    if (slot->has_bytes) {
        inputs->used += plan.source_bytes;
    }
    inputs->count++;
    return true;
}

bool live_texture_inputs_resolver(void *context, const live_texture_binding *binding, uint32_t bytes, uint32_t *address,
                                  uint64_t *identity, const char **refusal)
{
    live_texture_inputs *inputs = context;
    const entry *found = find_binding(inputs, binding, bytes);
    if (found == NULL) {
        inputs->misses++;
        *refusal = "pipelined frame: the texture binding was not captured at the Swap";
        return false;
    }
    if (!found->resolved) {
        *refusal = found->refusal;
        return false;
    }
    *address = found->address;
    *identity = found->identity;
    return true;
}

bool live_texture_inputs_reader(void *context, uint32_t address, void *out, size_t bytes)
{
    live_texture_inputs *inputs = context;
    for (size_t index = 0u; index < inputs->count; index++) {
        const entry *candidate = &inputs->entries[index];
        if (candidate->resolved && candidate->has_bytes && candidate->address == address && candidate->bytes == bytes) {
            memcpy(out, inputs->arena + candidate->offset, bytes);
            return true;
        }
    }
    inputs->misses++;
    return false;
}

uint64_t live_texture_inputs_misses(const live_texture_inputs *inputs)
{
    return inputs->misses;
}

size_t live_texture_inputs_entries(const live_texture_inputs *inputs)
{
    return inputs->count;
}

size_t live_texture_inputs_bytes(const live_texture_inputs *inputs)
{
    return inputs->used;
}
