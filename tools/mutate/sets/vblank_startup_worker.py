# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1245 compiled startup credit defects; real owner/producer wait-cycle controls."""


def mutation(name: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"t1245-startup-{name}",
        "file": "src/host/recomp_second_vblank.c",
        "old": old,
        "new": new,
        "targets": ["test_recomp_second_vblank_startup_worker"],
        "why": why,
    }


MUTATIONS = [
    mutation(
        "credit-never-delivered",
        "const bool candidate = interactive && owner_budget != 0u",
        "const bool candidate = false && owner_budget != 0u",
        "The terminal worker poll must consume the earned credit so the owner can join it.",
    ),
    mutation(
        "evidence-delivery-changed",
        "const bool candidate = interactive && owner_budget != 0u",
        "const bool candidate = true && owner_budget != 0u",
        "Default/evidence epochs must retain the owner-FRAME delivery policy.",
    ),
    mutation(
        "credit-not-consumed",
        "if (!raced) { state.inflight = true; state.second_attempted = true; state.credits = 0u; }",
        "if (!raced) { state.inflight = true; state.second_attempted = true; state.credits = 1u; }",
        "One completed wait supplies one credit, never a retained backlog after delivery.",
    ),
    mutation(
        "startup-recounted",
        "deliver_blank(handle,fs,true,true);",
        "(void)d3d8_gpu_model_blank(); deliver_blank(handle,fs,true,true);",
        "The credited wait already advanced time and device effects; no extra blank is owed.",
    ),
    mutation(
        "terminal-producer-refused",
        "(handle != state.producer_handle || state.second_via_worker)",
        "(handle != state.producer_handle)",
        "The live startup producer completes its final title step/wait before testing stop.",
    ),
]
