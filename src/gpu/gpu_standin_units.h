/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_STANDIN_UNITS_H
#define TSFP_GPU_STANDIN_UNITS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* T719: the stand-in's sampling unit chosen PER DRAW by the draw's vertex program digest. A rule is a lowercase hex digest prefix
 * (8..64 digits, the form docs print) and the unit its oT0 was MEASURED in: texel (true) or normalised (false). The list is
 * the caller's measured data (docs/data/t719-standin-units.json), never a default. A prefix overlapping another rule is refused,
 * so a draw matches at most one rule. A draw whose program matches none is unclassified and the replay refuses it. */
#define GPU_STANDIN_UNIT_RULES 16u
#define GPU_STANDIN_UNIT_MIN_DIGITS 8u
#define GPU_STANDIN_UNIT_MAX_DIGITS 64u

typedef struct gpu_standin_unit_rule {
    char prefix[GPU_STANDIN_UNIT_MAX_DIGITS + 1u];
    bool texel;
} gpu_standin_unit_rule;

static inline bool gpu_standin_unit_prefix_overlap(const char *left, const char *right)
{
    const size_t shorter = strlen(left) < strlen(right) ? strlen(left) : strlen(right);
    return strncmp(left, right, shorter) == 0;
}

/* Append a rule. False (rules and count unchanged) for a bad prefix, a full list or an overlap with an existing rule. */
static inline bool gpu_standin_unit_rule_add(gpu_standin_unit_rule rules[GPU_STANDIN_UNIT_RULES], uint32_t *count,
                                             const char *prefix, bool texel)
{
    if (rules == NULL || count == NULL || prefix == NULL || *count >= GPU_STANDIN_UNIT_RULES) {
        return false;
    }
    const size_t length = strlen(prefix);
    if (length < GPU_STANDIN_UNIT_MIN_DIGITS || length > GPU_STANDIN_UNIT_MAX_DIGITS) {
        return false;
    }
    for (size_t i = 0u; i < length; i++) {
        if (!((prefix[i] >= '0' && prefix[i] <= '9') || (prefix[i] >= 'a' && prefix[i] <= 'f'))) {
            return false;
        }
    }
    for (uint32_t i = 0u; i < *count; i++) {
        if (gpu_standin_unit_prefix_overlap(rules[i].prefix, prefix)) {
            return false;
        }
    }
    memcpy(rules[*count].prefix, prefix, length + 1u);
    rules[*count].texel = texel;
    (*count)++;
    return true;
}

/* True with *texel set when exactly one rule's prefix starts `digest`. No match (or, impossible after _add, two) is false. */
static inline bool gpu_standin_unit_lookup(const gpu_standin_unit_rule rules[GPU_STANDIN_UNIT_RULES], uint32_t count,
                                           const char *digest, bool *texel)
{
    if (rules == NULL || digest == NULL || texel == NULL) {
        return false;
    }
    uint32_t hits = 0u;
    for (uint32_t i = 0u; i < count && i < GPU_STANDIN_UNIT_RULES; i++) {
        if (strncmp(digest, rules[i].prefix, strlen(rules[i].prefix)) == 0) {
            *texel = rules[i].texel;
            hits++;
        }
    }
    return hits == 1u;
}

#endif
