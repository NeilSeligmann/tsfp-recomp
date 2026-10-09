# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the passive IDirectSoundStream_SetPosition 0x408609 (T1182), src/audio/dsound_stream.c.
Reached through test_t1182_stream_set_position (tests/test_t1182_stream_set_position.py is its original half)."""

STR = "src/audio/dsound_stream.c"
T = ["test_t1182_stream_set_position"]


def mutation(name: str, old: str, new: str, why: str) -> dict:
    return {"id": f"t1182-{name}", "file": STR, "old": old, "new": new, "targets": T, "why": why}


MUTATIONS: list[dict] = [
    mutation(
        "caller-1-moved",
        "(actual!=0x29ACAu && actual!=0x29E2Bu && actual!=0x29FADu)",
        "(actual!=0x29ACBu && actual!=0x29E2Bu && actual!=0x29FADu)",
        "the setup return 0x29ACA would be refused and a neighbour admitted.",
    ),
    mutation(
        "caller-2-moved",
        "(actual!=0x29ACAu && actual!=0x29E2Bu && actual!=0x29FADu)",
        "(actual!=0x29ACAu && actual!=0x29E2Cu && actual!=0x29FADu)",
        "the update return 0x29E2B would be refused and a neighbour admitted.",
    ),
    mutation(
        "caller-3-moved",
        "(actual!=0x29ACAu && actual!=0x29E2Bu && actual!=0x29FADu)",
        "(actual!=0x29ACAu && actual!=0x29E2Bu && actual!=0x29FACu)",
        "the update return 0x29FAD would be refused and a neighbour admitted.",
    ),
    mutation(
        "any-caller",
        "(actual!=0x29ACAu && actual!=0x29E2Bu && actual!=0x29FADu))",
        "false)",
        "only the three measured callers pass.",
    ),
    mutation(
        "apply-zero-admitted",
        'if(r->apply!=1u) {\n            r->error="SetPosition apply=0 enters the unmodeled settings commit helper";goto done;\n        }',
        "",
        "apply=0 enters the commit helper 0x406E90 and the APU chain, which the host record does not model.",
    ),
    mutation(
        "scope-not-spatial",
        'if(n->value.scope.flags!=0x10u || n->value.cache_mask!=7u) {\n            r->error="only a fully configured spatial startup stream may record SetPosition";goto done;\n        }',
        "",
        "an ordinary or unconfigured stream is refused (the original faults on a stream without settings).",
    ),
    mutation(
        "mask-not-required",
        'if(n->value.scope.flags!=0x10u || n->value.cache_mask!=7u) {\n            r->error="only a fully configured spatial startup stream may record SetPosition";goto done;\n        }',
        'if(n->value.scope.flags!=0x10u) {\n            r->error="only a fully configured spatial startup stream may record SetPosition";goto done;\n        }',
        "the three startup setters must be recorded first.",
    ),
    mutation(
        "x-y-swapped",
        "n->value.position_bits[0]=r->argument;n->value.position_bits[1]=r->count;n->value.position_bits[2]=r->position_z;",
        "n->value.position_bits[0]=r->count;n->value.position_bits[1]=r->argument;n->value.position_bits[2]=r->position_z;",
        "the original stores x at +8 and y at +0xC.",
    ),
    mutation(
        "z-lost",
        "n->value.position_bits[0]=r->argument;n->value.position_bits[1]=r->count;n->value.position_bits[2]=r->position_z;",
        "n->value.position_bits[0]=r->argument;n->value.position_bits[1]=r->count;n->value.position_bits[2]=0u;",
        "the original stores z at +0x10.",
    ),
    mutation(
        "sets-not-counted",
        "n->value.position_sets++;\n        *result=0u;goto done;",
        "*result=0u;goto done;",
        "every admitted call counts once.",
    ),
    mutation(
        "result",
        "n->value.position_sets++;\n        *result=0u;goto done;",
        "n->value.position_sets++;\n        *result=1u;goto done;",
        "the original returns S_OK.",
    ),
    mutation(
        "argument-order",
        "return dsound_stream_set_position(args[0],args[1],args[2],args[3],args[4]);",
        "return dsound_stream_set_position(args[0],args[2],args[1],args[3],args[4]);",
        "the stdcall order is stream, x, y, z, apply.",
    ),
    mutation(
        "apply-order",
        "return dsound_stream_set_position(args[0],args[1],args[2],args[3],args[4]);",
        "return dsound_stream_set_position(args[0],args[1],args[2],args[4],args[3]);",
        "apply is the fifth argument.",
    ),
    mutation(
        "register-gate",
        "if(policy)count+=dsound_hle_register(SET_POSITION,set_position_handler)?1u:0u;",
        "if(!policy)count+=dsound_hle_register(SET_POSITION,set_position_handler)?1u:0u;",
        "a flags-off boot keeps its registry, a flags-on boot registers the route.",
    ),
]
