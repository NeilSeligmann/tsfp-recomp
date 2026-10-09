# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1245 compiled defects: guest ABI, candidate atomicity and real source time."""


def mutation(
    name: str, file: str, old: str, new: str, why: str, target: str = "test_dsound_completion"
) -> dict:
    return {
        "id": f"t1245-{name}",
        "file": f"src/audio/{file}.c",
        "old": old,
        "new": new,
        "targets": [target],
        "why": why,
    }


MUTATIONS = [
    mutation(
        "runtime-pause-missing-transport",
        "dsound_stream",
        "(!completion || pause_note == NULL || !n->value.discontinuity_seen || r->argument != 1u)",
        "(!completion || !n->value.discontinuity_seen || r->argument != 1u)",
        "A runtime pause cannot report success without an installed transport consumer.",
        "test_dsound_stream_start_caller",
    ),
    mutation(
        "runtime-pause-caller-refused",
        "dsound_stream",
        "third_admitted && entry==PAUSE && actual==0x29ED1u",
        "third_admitted && entry==PAUSE && actual==0x29ED2u",
        "The original runtime pause return29ED1 must reach the owned transport.",
        "test_dsound_stream_start_caller",
    ),
    mutation(
        "runtime-pause-inactive-admitted",
        "dsound_stream",
        "(!completion || pause_note == NULL || !n->value.discontinuity_seen || r->argument != 1u)",
        "(!completion || pause_note == NULL || r->argument != 1u)",
        "A runtime caller cannot pause an unstarted lease.",
        "test_dsound_stream_start_caller",
    ),
    mutation(
        "runtime-pause-mode-unchecked",
        "dsound_stream",
        "(!completion || pause_note == NULL || !n->value.discontinuity_seen || r->argument != 1u)",
        "(!completion || pause_note == NULL || !n->value.discontinuity_seen)",
        "The original runtime caller requests mode1, never resume or synchronous mode2.",
        "test_dsound_stream_start_caller",
    ),
    mutation(
        "boundary-frequency-uses-unrendered-point",
        "dsound_mixer",
        "const uint64_t committed=mixer->rendered_frames!=0u?mixer->rendered_frames-1u:0u;",
        "const uint64_t committed=mixer->rendered_frames;",
        "A rate event between emitted sample30 and future sample32 must admit tick31.",
        "test_t1245_frequency_mixer",
    ),
    mutation(
        "boundary-packet-uses-unrendered-point",
        "dsound_mixer",
        "mixer->started ? mul_div_floor(mixer->rendered_frames!=0u?mixer->rendered_frames-1u:0u,",
        "mixer->started ? mul_div_floor(mixer->rendered_frames,",
        "A new packet after between-sample SetFormat must affect only future PCM.",
        "test_t1245_frequency_mixer",
    ),
    mutation(
        "spatial-global-reports-success",
        "dsound_stream",
        "if(failure!=0u)return failure;\n    }\n    access_request r={0};",
        "if(failure!=0u)return 0u;\n    }\n    access_request r={0};",
        "Original spatial global failure returns E_FAIL before invalid object access.",
        "test_t1182_stream_set_position",
    ),
    mutation(
        "spatial-global-preflight-skipped",
        "dsound_stream",
        "if(entry==SET_POSITION || entry==MAX_DISTANCE) {",
        "if(false) {",
        "Spatial global failure must precede identifying the stream and settings.",
        "test_t1182_stream_set_position",
    ),
    mutation(
        "call-address-not-return",
        "dsound_stream",
        "actual!=0x29B35u",
        "actual!=0x29B30u",
        "The measured CALL address is not its return address.",
    ),
    mutation(
        "zero-keeps-effective-rate",
        "dsound_stream",
        "hertz=n->value.scope.sample_rate;",
        "hertz=n->value.source_rate_hz;",
        "Zero restores the format rate rather than the previous effective rate.",
    ),
    mutation(
        "unsupported-cw-admitted",
        "dsound_stream",
        "dsound_stream_frequency_pitch(hertz,(uint16_t)r->count,&pitch)",
        "dsound_stream_frequency_pitch(hertz,0x027Fu,&pitch)",
        "An unsupported guest CW must refuse without committing.",
    ),
    mutation(
        "parent-header-check-dropped",
        "dsound_stream",
        "if(!valid_node(n,&r->identity,internal))",
        "if(n==NULL)",
        "Tampered stream headers cannot enter the rate transaction.",
    ),
    mutation(
        "commit-refused-candidate",
        "dsound_completion",
        "dsound_audio_runtime_frequency_stream(stream,serial,ticks,base,old_rate_hz,new_rate_hz);\n        if(accepted)stream_publish_advance(state,&candidate);",
        "dsound_audio_runtime_frequency_stream(stream,serial,ticks,base,old_rate_hz,new_rate_hz);\n        stream_publish_advance(state,&candidate);",
        "PCM rejection must not publish completion words or counters.",
    ),
    mutation(
        "no-live-rate-change",
        "dsound_completion",
        "(uint64_t)new_rate_hz<<32u,candidate.active_ticks)",
        "(uint64_t)old_rate_hz<<32u,candidate.active_ticks)",
        "Already queued packet durations must follow the new source clock.",
    ),
    mutation(
        "ignore-pcm-consumer",
        "dsound_completion",
        "if(accepted && pcm)accepted=",
        "if(accepted && pcm && false)accepted=",
        "A missing renderer cannot acknowledge a real PCM frequency change.",
    ),
    mutation(
        "no-live-pcm-rate",
        "dsound_mixer",
        "(uint64_t)source_rate_hz<<32u,retired)",
        "(uint64_t)candidate.base_hertz<<32u,retired)",
        "PCM samples must accelerate at the event, not only future packets.",
        "test_t1245_frequency_mixer",
    ),
    mutation(
        "drop-submission-fraction",
        "dsound_mixer",
        ".start_fraction = start.fraction,",
        ".start_fraction = 0u,",
        "A packet submitted between source ticks retains its exact source origin.",
        "test_t1245_frequency_mixer",
    ),
    mutation(
        "format-keeps-prior-pitch",
        "dsound_completion",
        "candidate.active_ticks,(uint64_t)new_hz<<32u,candidate.active_ticks);",
        "candidate.active_ticks,(uint64_t)old_hz<<32u,candidate.active_ticks);",
        "Format reset applies the new format pitch to source time.",
    ),
    mutation(
        "format-ignores-pcm-reset",
        "dsound_completion",
        "if(accepted)accepted=dsound_audio_runtime_format_stream(stream,serial,ticks,old_hz,new_hz);",
        "if(accepted && false)accepted=dsound_audio_runtime_format_stream(stream,serial,ticks,old_hz,new_hz);",
        "PCM format refusal must preserve completion and the stream snapshot.",
    ),
    mutation(
        "spatial-frequency-refused",
        "dsound_stream",
        "const bool spatial=n->value.scope.flags==0x10u && n->value.scope.channels==1u && n->value.cache_mask==7u;",
        "const bool spatial=false;",
        "Fully configured mono streams use the measured frequency transaction and actual PCM decoder.",
    ),
    mutation(
        "format-reports-success-on-abort",
        "dsound_completion",
        "kernel_guest_write_u32(pending->status,0x80004004u)",
        "kernel_guest_write_u32(pending->status,0u)",
        "Original SetFormat marks each queued packet E_ABORT rather than successful completion.",
    ),
    mutation(
        "format-packet-base-unscaled",
        "dsound_completion",
        "remaining_q32=(remaining_q32*nominal_hz)/base;",
        "remaining_q32=(remaining_q32*base)/base; (void)nominal_hz;",
        "Future packets after format reset retain physical duration in the historical source clock.",
    ),
]
