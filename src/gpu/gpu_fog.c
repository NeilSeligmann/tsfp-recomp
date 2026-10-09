/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_fog.h"
#include <stdio.h>
#include <string.h>
static bool refused(char *error, size_t bytes, const char *message) {
  if (error != NULL && bytes != 0u)
    snprintf(error, bytes, "%s", message);
  return false;
}
bool gpu_fog_decode(const gpu_pgraph_state *state, gpu_fog_control *out,
                    char *error, size_t bytes) {
  if (state == NULL || out == NULL)
    return refused(error, bytes, "missing fog state");
  memset(out, 0, sizeof(*out));
  if (!state->output_written[GPU_PGRAPH_OUT_FOG_ENABLE])
    return refused(error, bytes, "fog enable was never written");
  const uint32_t enabled = state->output[GPU_PGRAPH_OUT_FOG_ENABLE];
  if (enabled > 1u)
    return refused(error, bytes, "fog enable is not 0 or 1");
  if (enabled == 0u)
    return true; /* Disabled xemu fog factor is1, parameters unused. */
  if (!state->output_written[GPU_PGRAPH_OUT_FOG_MODE] ||
      !state->output_written[GPU_PGRAPH_OUT_FOG_PARAM0] ||
      !state->output_written[GPU_PGRAPH_OUT_FOG_PARAM1])
    return refused(error, bytes,
                   "enabled fog mode/parameters were never written");
  switch (state->output[GPU_PGRAPH_OUT_FOG_MODE]) {
  case 0x2601u:
    out->mode = 0.0f;
    break;
  case 0x800u:
    out->mode = 1.0f;
    break;
  case 0x801u:
    out->mode = 2.0f;
    break;
  case 0x804u:
    out->mode = 3.0f;
    break;
  case 0x802u:
    out->mode = 4.0f;
    break;
  case 0x803u:
    out->mode = 5.0f;
    break;
  default:
    return refused(error, bytes, "unsupported fog mode");
  }
  if (state->output_written[GPU_PGRAPH_OUT_FOG_GEN_MODE]) {
    const uint32_t gen = state->output[GPU_PGRAPH_OUT_FOG_GEN_MODE];
    if (gen > 3u && gen != 6u)
      return refused(error, bytes, "unsupported fog generation mode");
  }
  /* Programmable xemu ignores foggen, takes oFog.x. Nonfinite parameters are
   * deliberate inputs: its NaN/final-clamp policy handles them. */
  memcpy(&out->param0, &state->output[GPU_PGRAPH_OUT_FOG_PARAM0],
         sizeof(float));
  memcpy(&out->param1, &state->output[GPU_PGRAPH_OUT_FOG_PARAM1],
         sizeof(float));
  out->enabled = 1.0f;
  return true;
}
