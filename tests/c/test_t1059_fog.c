/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_combiner.h"
#include "gpu_fog.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x)                                                               \
  do {                                                                         \
    checks++;                                                                  \
    if (!(x)) {                                                                \
      failures++;                                                              \
      printf("FAIL %u %s\n", __LINE__, #x);                                    \
    }                                                                          \
  } while (0)
static void put(gpu_pgraph_state *s, unsigned index, uint32_t value) {
  s->output[index] = value;
  s->output_written[index] = true;
}
int main(void) {
  gpu_pgraph_state *s = calloc(1, sizeof(*s));
  if (s == NULL)
    return 2;
  gpu_fog_control control;
  char error[256];
  CHECK(!gpu_fog_decode(s, &control, error, sizeof(error)));
  put(s, GPU_PGRAPH_OUT_FOG_ENABLE, 0);
  CHECK(gpu_fog_decode(s, &control, error, sizeof(error)));
  CHECK(gpu_fog_factor(&control, NAN) == 1.0f);
  put(s, GPU_PGRAPH_OUT_FOG_ENABLE, 2);
  CHECK(!gpu_fog_decode(s, &control, error, sizeof(error)));
  put(s, GPU_PGRAPH_OUT_FOG_ENABLE, 1);
  CHECK(!gpu_fog_decode(s, &control, error, sizeof(error)));
  put(s, GPU_PGRAPH_OUT_FOG_PARAM0, 0x3FC00000);
  put(s, GPU_PGRAPH_OUT_FOG_PARAM1, 0xBF800000);
  static const uint32_t modes[] = {0x2601, 0x800, 0x801, 0x804, 0x802, 0x803};
  /* Literal controls: d=0 => linear .5 / exponential 1, d=1 =>
   * linear -.5 (ABS .5), exp2 2^-32, exp 2^-16. */
  for (unsigned i = 0; i < 6; i++) {
    put(s, GPU_PGRAPH_OUT_FOG_MODE, modes[i]);
    CHECK(gpu_fog_decode(s, &control, error, sizeof(error)));
    CHECK(control.mode == (float)i);
    CHECK(gpu_fog_factor(&control, 0.0f) == (i == 0 || i == 3 ? 0.5f : 1.0f));
    const float exceptional = (i == 0 || i == 1 || i == 3) ? 1.0f : 0.0f;
    CHECK(gpu_fog_factor(&control, INFINITY) == exceptional);
    CHECK(gpu_fog_factor(&control, -INFINITY) == exceptional);
    CHECK(gpu_fog_factor(&control, NAN) == exceptional);
  }
  put(s, GPU_PGRAPH_OUT_FOG_MODE, 0x2601);
  CHECK(gpu_fog_decode(s, &control, error, sizeof(error)));
  CHECK(gpu_fog_factor(&control, 1.0f) == -0.5f); /* no premature [0,1] clamp */
  put(s, GPU_PGRAPH_OUT_FOG_MODE, 0x804);
  CHECK(gpu_fog_decode(s, &control, error, sizeof(error)));
  CHECK(gpu_fog_factor(&control, 1.0f) == 0.5f);
  put(s, GPU_PGRAPH_OUT_FOG_MODE, 0xBAD);
  CHECK(!gpu_fog_decode(s, &control, error, sizeof(error)));
  put(s, GPU_PGRAPH_OUT_FOG_MODE, 0x2601);
  put(s, GPU_PGRAPH_OUT_FOG_GEN_MODE, 4);
  CHECK(!gpu_fog_decode(s, &control, error, sizeof(error)));
  put(s, GPU_PGRAPH_OUT_FOG_GEN_MODE, 6);
  CHECK(gpu_fog_decode(s, &control, error, sizeof(error)));
  put(s,GPU_PGRAPH_OUT_FOG_MODE,0x800);put(s,GPU_PGRAPH_OUT_FOG_PARAM1,0xBE000000);
  CHECK(gpu_fog_decode(s,&control,error,sizeof(error)));
  CHECK(gpu_fog_factor(&control,1.0f)==0.25f);
  put(s,GPU_PGRAPH_OUT_FOG_MODE,0x801);CHECK(gpu_fog_decode(s,&control,error,sizeof(error)));
  CHECK(fabsf(gpu_fog_factor(&control,1.0f)-0.70710678f)<0.000001f);
  free(s);
  printf("T1059 CPU fog %u checks %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
