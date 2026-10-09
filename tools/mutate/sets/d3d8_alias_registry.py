# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the registered allocation alias table (T755, src/gpu/d3d8_resource.c): Register records
each header's binding, and the lock resolves a header only through a recorded and still verifying one.

WHAT THIS SET IS FOR. The table was a fixed 128 rows whose overflow DROPPED a Register silently. The title
registers 490 front end textures before the attract movie builds its two YUY2 texture headers (locals of the
movie player), so the movie's own Register was lost and its `LockRect` stopped as "no verified registered
allocation alias" although the header was registered. The mutants below put that bug and its neighbours
back: no growth, a low limit, a full table that overwrites instead of stopping, reuse of a row that still
verifies, no reuse of a dead row, each check of the shared verification, and the live count.

Every mutation is covered by `tests/c/test_d3d8_lock.c` (the unit cases that register 129, 600 and 16384
headers) and the oracle replay `tests/test_d3dscan_surface_lock.py` is its measured twin.

NOT MUTATED, because the mutant is EQUIVALENT: the `region->requested != record->region_requested` term of the
shared verification. The requested size is written once, when the region is allocated, and nothing resizes a
region, so a different requested size always means a new allocation, whose lifetime generation changes even if physical pages are reused and is
refused by the combined physical/generation guard (`t755-alias-verify-ignores-a-replaced-allocation`).

CONVENTION. `&& false` rather than a bare `false`, because `-Wunused-parameter -Werror` turns the latter
into NOT-A-MUTANT, which reads like evidence.
"""

RESOURCE = "src/gpu/d3d8_resource.c"
SUITE = "test_d3d8_lock"


def mutation(identifier: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"t755-alias-{identifier}",
        "file": RESOURCE,
        "old": old,
        "new": new,
        "targets": [SUITE],
        "why": why,
    }


MUTATIONS: list[dict] = [
    mutation(
        "no-contiguous-region-reason",
        "const alias_rejection_reason reason = ALIAS_REJECTION_NO_CONTIGUOUS_REGION;",
        "const alias_rejection_reason reason = ALIAS_REJECTION_NONE;",
        "a Register over a base without a committed contiguous region loses its named lock refusal.",
    ),
    mutation(
        "region-too-large-reason",
        "const alias_rejection_reason reason = ALIAS_REJECTION_REGION_TOO_LARGE;",
        "const alias_rejection_reason reason = ALIAS_REJECTION_NONE;",
        "a region outside the 28-bit alias-size limit loses its named lock refusal.",
    ),
    mutation(
        "data-outside-region-reason",
        "const alias_rejection_reason reason = ALIAS_REJECTION_DATA_OUTSIDE_REGION;",
        "const alias_rejection_reason reason = ALIAS_REJECTION_NONE;",
        "a Data offset outside the registered region loses its named lock refusal.",
    ),
    mutation(
        "virtual-retained-reason",
        "const alias_rejection_reason reason = ALIAS_REJECTION_VIRTUAL_RETAINED;",
        "const alias_rejection_reason reason = ALIAS_REJECTION_NONE;",
        "a virtual-retained resource loses its named lock refusal.",
    ),
    mutation(
        "table-never-grows-past-initial",
        "    if (alias_count == alias_capacity && alias_capacity < ALIAS_LIMIT) {",
        "    if (alias_count == alias_capacity && alias_capacity < ALIAS_INITIAL_CAPACITY) {",
        "the table stays at its first 128 rows, the fixed table T755 replaced: the 129th Register has no row, so the attract movie's texture is never recorded.",
    ),
    mutation(
        "limit-too-low",
        "#define ALIAS_LIMIT 16384u",
        "#define ALIAS_LIMIT 4096u",
        "the table stops growing at 4096 rows, a title that registers more textures than that loses the later ones.",
    ),
    mutation(
        "full-table-overwrites-instead-of-stopping",
        '    d3d8_hle_fatal(0x003D4D70u,\n                   "registered allocation alias table is full',
        '    if (alias_count > 0u) return &aliases[0];\n    d3d8_hle_fatal(0x003D4D70u,\n                   "registered allocation alias table is full',
        "a full table of rows that all verify silently takes over the first one, so a live texture's binding is replaced by another header's.",
    ),
    mutation(
        "reuse-takes-a-verifying-row",
        "        if (!aliases[i].valid || alias_verify(&aliases[i]) != ALIAS_OK) return &aliases[i];",
        "        if (!aliases[i].valid || alias_verify(&aliases[i]) == ALIAS_OK) return &aliases[i];",
        "the row reused at the limit is one that still verifies, which is a live texture, instead of one that cannot.",
    ),
    mutation(
        "reuse-skips-modified-rows",
        "        if (!aliases[i].valid || alias_verify(&aliases[i]) != ALIAS_OK) return &aliases[i];",
        "        if (!aliases[i].valid) return &aliases[i];",
        "a dead stack header (its words no longer carry the registered identity) is never reclaimed, so the full table stops by name although rows are dead.",
    ),
    mutation(
        "reuse-skips-invalid-rows",
        "        if (!aliases[i].valid || alias_verify(&aliases[i]) != ALIAS_OK) return &aliases[i];",
        "        if (alias_verify(&aliases[i]) != ALIAS_OK) return &aliases[i];",
        "a row a later Register invalidated (header still intact, so it still verifies) is not reclaimed.",
    ),
    mutation(
        "register-stores-data-before-the-table-can-refuse",
        "    record_registered_alias(header, base, offset, common, data);\n    d3d8_guest_store32(header + 4u, data);",
        "    d3d8_guest_store32(header + 4u, data);\n    record_registered_alias(header, base, offset, common, data);",
        "a Register the table refuses has already relocated the header's Data word, so retrying it adds the base twice.",
    ),
    mutation(
        "verify-ignores-the-class",
        "    if ((words[0] & IMMUTABLE_COMMON_MASK) != record->common_identity ||",
        "    if (((words[0] & IMMUTABLE_COMMON_MASK) != record->common_identity && false) ||",
        "a header whose type bits changed (a texture rewritten as a surface) still resolves to the old allocation.",
    ),
    mutation(
        "verify-ignores-data",
        "        words[1] != record->data || words[3] != record->format || words[4] != record->size)",
        "        (words[1] != record->data && false) || words[3] != record->format || words[4] != record->size)",
        "a header whose Data word moved still resolves to the allocation it was registered over.",
    ),
    mutation(
        "verify-ignores-format",
        "        words[1] != record->data || words[3] != record->format || words[4] != record->size)",
        "        words[1] != record->data || (words[3] != record->format && false) || words[4] != record->size)",
        "a header whose format changed (different pitch and extent) still resolves.",
    ),
    mutation(
        "verify-ignores-size",
        "        words[1] != record->data || words[3] != record->format || words[4] != record->size)",
        "        words[1] != record->data || words[3] != record->format || (words[4] != record->size && false))",
        "a header whose Size word changed (a different pitch or height) still resolves.",
    ),
    mutation(
        "verify-ignores-a-replaced-allocation",
        "        region->physical != record->region_physical || region->generation != record->allocation_generation ||",
        "        false ||",
        "an allocation freed and allocated again at the same address (new lifetime, possibly reused physical pages) is taken for the one the header was registered over.",
    ),
    mutation(
        "live-count-counts-retired-rows",
        "    for (size_t i = 0u; i < alias_count; i++) live += aliases[i].valid ? 1u : 0u;",
        "    for (size_t i = 0u; i < alias_count; i++) live += 1u;",
        "the live binding count includes the rows a later Register retired.",
    ),
]
