/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_fog.h"
#include <float.h>
#include <math.h>
float gpu_fog_factor(const gpu_fog_control *c, float distance) {
  if (c->enabled == 0.0f)
    return 1.0f;
  const unsigned mode = (unsigned)c->mode;
  const float exceptional = (mode <= 1u || mode == 3u) ? 1.0f : 0.0f;
  if (isinf(distance))
    return exceptional;
  float value;
  if (mode == 0u || mode == 3u)
    value = (c->param0 + distance * c->param1) - 1.0f;
  else if (mode == 1u || mode == 4u)
    value = (c->param0 + exp2f(distance * c->param1 * 16.0f)) - 1.5f;
  else
    value = (c->param0 +
             exp2f(-distance * distance * c->param1 * c->param1 * 32.0f)) -
            1.5f;
  if (mode >= 3u)
    value = fabsf(value);
  if (isnan(value))
    value = exceptional;
  if (value > FLT_MAX)
    value = FLT_MAX;
  if (value < -FLT_MAX)
    value = -FLT_MAX;
  return value; /* Clamp [0,1] is in fragment, after interpolation. */
}
