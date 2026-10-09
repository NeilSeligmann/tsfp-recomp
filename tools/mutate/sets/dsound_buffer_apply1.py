# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the apply=1 SetMinDistance and SetRolloffCurve sites (T1173), src/audio/dsound_buffer.c.
Reached through test_dsound_buffer_apply1_setters (the native half of tests/test_t1173_buffer_apply1_setters.py)."""

BUF = "src/audio/dsound_buffer.c"
T = ["test_dsound_buffer_apply1_setters"]


def mutation(name: str, old: str, new: str, why: str) -> dict:
    return {"id": f"t1173-{name}", "file": BUF, "old": old, "new": new, "targets": T, "why": why}


MUTATIONS: list[dict] = [
    mutation(
        "min-caller-moved",
        "!(entry==MINIMUM && actual==MINIMUM_START_CALLER)",
        "!(entry==MINIMUM && actual==MINIMUM_START_CALLER+1u)",
        "the sound start's SetMinDistance return 0x28390 would be refused again and a neighbour admitted.",
    ),
    mutation(
        "rolloff-caller-moved",
        "!(entry==ROLLOFF && actual==ROLLOFF_START_CALLER)",
        "!(entry==ROLLOFF && actual==ROLLOFF_START_CALLER+1u)",
        "the sound start's SetRolloffCurve return 0x283AE would be refused again and a neighbour admitted.",
    ),
    mutation(
        "caller-apply-pairing-dropped",
        "if((actual==MINIMUM_START_CALLER || actual==ROLLOFF_START_CALLER)!=(apply==1u))",
        "if(false)",
        "each measured caller keeps its own apply value (start sites 1, startup sites 0).",
    ),
    mutation(
        "caller-apply-pairing-inverted",
        "if((actual==MINIMUM_START_CALLER || actual==ROLLOFF_START_CALLER)!=(apply==1u))",
        "if((actual==MINIMUM_START_CALLER || actual==ROLLOFF_START_CALLER)==(apply==1u))",
        "the startup callers would be admitted with apply=1 and the start callers with apply=0.",
    ),
    mutation(
        "apply1-needs-configured-spatial",
        'if(n->value.scope.flags!=0x10u || n->value.cache_mask!=7u || n->value.data_sets==0u) {\n            r->error="only the measured apply=1 update on a configured spatial buffer with recorded data is supported";\n            goto done;\n        }\n        if(r->entry==MINIMUM) {',
        'if(false) {\n            r->error="only the measured apply=1 update on a configured spatial buffer with recorded data is supported";\n            goto done;\n        }\n        if(r->entry==MINIMUM) {',
        "an ordinary, fresh or data-less spatial buffer must be refused (the original faults on a non-spatial buffer).",
    ),
    mutation(
        "apply1-data-not-required",
        'n->value.cache_mask!=7u || n->value.data_sets==0u) {\n            r->error="only the measured apply=1 update',
        'n->value.cache_mask!=7u) {\n            r->error="only the measured apply=1 update',
        "the sound start re-applies after SetBufferData, a buffer without recorded data is outside the measured order.",
    ),
    mutation(
        "min-record-lost",
        "n->value.min_distance_bits=r->argument;n->value.min_distance_sets++;",
        "n->value.min_distance_sets++;",
        "the host record keeps the raw bits the original stored at +0x38.",
    ),
    mutation(
        "min-sets-not-counted",
        "n->value.min_distance_bits=r->argument;n->value.min_distance_sets++;",
        "n->value.min_distance_bits=r->argument;",
        "every admitted call counts once.",
    ),
    mutation(
        "rolloff-record-swapped",
        "n->value.curve_address=r->argument;n->value.curve_count=r->count;n->value.rolloff_sets++;",
        "n->value.curve_address=r->count;n->value.curve_count=r->argument;n->value.rolloff_sets++;",
        "the original stores the curve pointer at +0x50 and the count at +0x54.",
    ),
    mutation(
        "rolloff-sets-not-counted",
        "n->value.curve_address=r->argument;n->value.curve_count=r->count;n->value.rolloff_sets++;",
        "n->value.curve_address=r->argument;n->value.curve_count=r->count;",
        "every admitted call counts once.",
    ),
    mutation(
        "apply1-rewrites-startup-mask",
        "n->value.min_distance_bits=r->argument;n->value.min_distance_sets++;\n        } else {",
        "n->value.min_distance_bits=r->argument;n->value.min_distance_sets++;n->value.cache_mask=3u;\n        } else {",
        "the re-apply must not reopen the startup ordered cache.",
    ),
    mutation(
        "apply1-result",
        "r->announce=!apply_announced;apply_announced=true;\n        *result=0u;goto done;",
        "r->announce=!apply_announced;apply_announced=true;\n        *result=1u;goto done;",
        "the original returns S_OK.",
    ),
]
