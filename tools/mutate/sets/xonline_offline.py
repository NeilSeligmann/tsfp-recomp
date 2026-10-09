# ruff: noqa: E501  (the C source anchors and rationale strings are intentionally verbatim)
"""Mutations for T904's opt-in XONLINE offline query model.

The C suite checks the original no-state HRESULT policy, state reference lifetime,
output bounds, and the production XDK dispatcher route. The separate Unicorn tests
measure the original no-state bytes and caller ABI; the model mutations do not claim
to validate the inferred successful-startup/no-account policy against a console.
"""

MODEL = "src/xbox/xonline_offline.c"
HLE = "src/xbox/xonline_hle.c"
THUNK = "src/host/xdk_thunk.c"
SUITES = ["test_xonline_offline"]


def mutation(mutation_id: str, file: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"xonline-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": SUITES,
        "why": why,
    }


MUTATIONS = [
    mutation(
        "nonzero-reserved-accepted",
        MODEL,
        "reserved != 0u || hresult == NULL",
        "(reserved != 0u && reserved != 0x1000u) || hresult == NULL",
        "only the title's measured reserved zero is admitted; accepting a nonzero value silently invents initializer semantics.",
    ),
    mutation(
        "startup-reference-not-incremented",
        MODEL,
        "    state.references++;",
        "    state.references += 2u;",
        "the host-side lifecycle must count one live startup reference per successful call, matching the original state block.",
    ),
    mutation(
        "no-state-query-reports-success",
        MODEL,
        "        *hresult = XONLINE_E_NOT_INITIALIZED;",
        "        *hresult = 0u;",
        "the original no-state GetUsers branch returns 0x80150005 and leaves output words untouched.",
    ),
    mutation(
        "list-does-not-clear-full-area",
        MODEL,
        "if (!kernel_guest_write_bytes(users, zeros, sizeof(zeros)) || !kernel_guest_write_u32(count, 0u)) {",
        "if (!kernel_guest_write_bytes(users, zeros, sizeof(zeros) - 1u) || !kernel_guest_write_u32(count, 0u)) {",
        "a successful no-account answer clears exactly the caller's 0x700-byte user record area; the final byte is guarded by the C suite.",
    ),
    mutation(
        "cleanup-reference-not-dropped",
        MODEL,
        "    state.references--;",
        "    state.references -= 2u;",
        "Cleanup releases one reference, not all shared startup ownership at once.",
    ),
    mutation(
        "cleanup-no-state-success",
        MODEL,
        "        return XONLINE_E_NOT_INITIALIZED;",
        "        return 0u;",
        "Cleanup on a state-less service must preserve the original no-state error rather than claim a successful release.",
    ),
    mutation(
        "xonline-section-unrouted",
        THUNK,
        '    if (strcmp(section, "XONLINE") == 0) {\n        return XDK_MODULE_XONLINE;\n    }',
        '    if (strcmp(section, "XONLINE") == 0) {\n        return XDK_MODULE_NONE;\n    }',
        "the measured XONLINE rows must route to their own module so registered offline handlers are reachable.",
    ),
    mutation(
        "xonline-handler-not-dispatched",
        THUNK,
        "    case XDK_MODULE_XONLINE:\n        return xonline_hle_call(row->address, frame);",
        "    case XDK_MODULE_XONLINE:\n        return 0u;",
        "the production address dispatcher must invoke the registered XONLINE handler and apply its stack ABI.",
    ),
]
