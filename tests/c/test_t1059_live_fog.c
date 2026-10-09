/* SPDX-License-Identifier: GPL-3.0-or-later
 * Synthetic literal point coverage through the real live translator and
 * renderer. No retail bytes; fixed state must override both missing and
 * explicit oPts. */
#define VK_NO_PROTOTYPES
#include "gpu_fog.h"
#include "live_vk_pipeline.h"
#include "names.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks, failures;
#define CHECK(c)                                                               \
  do {                                                                         \
    checks++;                                                                  \
    if (!(c)) {                                                                \
      failures++;                                                              \
      printf("FAIL %d: %s\n", __LINE__, #c);                                   \
    }                                                                          \
  } while (0)
/* Include order supplies WIDTH/HEIGHT and CHECK to the dependent rig. */
#include "live_vk_scenes.h"
#include "live_vk_rig.h"

static uint32_t *point_modules[4];
static size_t point_counts[4];
static const char *const vertex_names[] = {FOG_VERTEX_NAME, FOG_EXPLICIT_NAME};
static const char *const fog_names[] = {FOG_FRAGMENT_NAME, FOG_ALPHA_NAME};
static const struct gpu_vsh_table vertex_table = {0u,   0u, NULL,        0u,
                                                  NULL, 2u, vertex_names};
static const struct gpu_vsh_table fog_table = {0u,   0u, NULL,     0u,
                                               NULL, 2u, fog_names};
static const uint32_t point_program[] = {0, 0x0020021B, 0x0836186C, 0x0000F800,
                                         0, 0x0020041B, 0x0836186C, 0x0000F818,
                                         0, 0x0020041B, 0x0836186C, 0x0000F831};
static bool point_load(void *context, uint32_t index, const uint32_t **words,
                       size_t *count) {
  (void)context;
  if (index > 1u)
    return false;
  const unsigned selected=index==0u?0u:2u;
  *words = point_modules[selected];
  *count = point_counts[selected];
  return *words != NULL;
}
static bool fragment_load(void *context, uint32_t index, const uint32_t **words,
                          size_t *count) {
  (void)context;
  if (index>1u) return false;
  const unsigned slot=index==0u?1u:3u;
  *words=point_modules[slot];*count=point_counts[slot];
  return *words != NULL;
}
static bool read_shader(const char *directory, unsigned index) {
  char name[1024];
  if (snprintf(name, sizeof name, "%s/%s.spv", directory,
               index == 3u ? "alpha.frag" : (index == 2u ? "explicit.vert" : (index ? "fog.frag" : "fog.vert"))) >= (int)sizeof name)
    return false;
  FILE *file = fopen(name, "rb");
  if (file == NULL)
    return false;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false;
  }
  long bytes = ftell(file);
  if (bytes <= 0 || bytes % 4 != 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return false;
  }
  point_modules[index] = malloc((size_t)bytes);
  const bool okay =
      point_modules[index] != NULL &&
      fread(point_modules[index], 1u, (size_t)bytes, file) == (size_t)bytes;
  fclose(file);
  point_counts[index] = (size_t)bytes / 4u;
  return okay;
}
static bool explicit_fog;
static bool multi_draw;
static bool alpha_only,omit_color;
static gpu_pgraph *fog_model(uint32_t enable, uint32_t mode, float p0, float p1,
                             uint32_t colour) {
  stream_builder stream = {0};
  const float offset[4] = {0, 0, 0, 0}, scale[4] = {1, 1, 1, 1};
  stream_viewport(&stream, offset, scale);
  stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
  uint32_t words[12];
  memcpy(words, point_program, sizeof words);
  if (explicit_fog) { words[9]=0x0020061Bu;words[11]=0x0000F829u; }
  else words[7] |= 1u;
  stream_program(&stream, 0u, words, explicit_fog?3u:2u);
  stream_pair(&stream, GPU_PGRAPH_PROGRAM_START, 0u);
  stream_pair(&stream, 0x0318u, 0u);
  stream_pair(&stream, 0x031Cu, 0u);
  stream_pair(&stream, 0x043Cu, 8u);
  stream_pair(&stream, 0x02A4u, enable);
  stream_pair(&stream, 0x029Cu, mode);
  stream_pair(&stream, 0x02A0u, 6u);
  uint32_t u0, u1;
  memcpy(&u0, &p0, 4);
  memcpy(&u1, &p1, 4);
  stream_pair(&stream, 0x09C0u, u0);
  stream_pair(&stream, 0x09C4u, u1);
  stream_pair(&stream, 0x09C8u, 0u);
  if(!omit_color) stream_pair(&stream, 0x02A8u, colour);
  uint32_t combiner[COMBINER_WORDS] = {0};
  combiner[8] = alpha_only ? 0x00000013u : 0x13040300u;
  combiner[9] = 0x00001400u;
  combiner[53] = 1u;
  stream_pixel_shader(&stream, combiner);
  stream_array(&stream, 1u, MEMORY_BASE,
               array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
  stream_array(&stream, 2u, MEMORY_BASE + 12u,
               array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
  if(explicit_fog) stream_array(&stream,3u,MEMORY_BASE+24u,array_format(28u,1u,GPU_PGRAPH_TYPE_F));
  stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
  if(multi_draw){
    stream_pair(&stream,0x02A4u,1u);
    stream_array(&stream,1u,MEMORY_BASE+TRIANGLE_BYTES,array_format(28u,3u,GPU_PGRAPH_TYPE_F));
    stream_array(&stream,2u,MEMORY_BASE+TRIANGLE_BYTES+12u,array_format(28u,4u,GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream,GPU_PGRAPH_OP_TRIANGLES,0u,3u);
  }
  gpu_pgraph *model = gpu_pgraph_create();
  CHECK(model != NULL);
  gpu_pgraph_set_combiner(model, true);
  gpu_pgraph_set_output_groups(model,
                               ALL_GROUPS | GPU_PGRAPH_OUTPUT_POLYGON_OFFSET);
  CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
  stream_free(&stream);
  return model;
}
static void one_case(rig *r, uint32_t enable, uint32_t mode, float p0, float p1,
                     uint32_t colour, bool profile, uint32_t inferences,
                     const unsigned rgb[3], const char *refusal) {
  fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
  gpu_pgraph_backend backend = make_backend(&guest, inferences);
  backend.table = &vertex_table;
  backend.load_module = point_load;
  backend.fragment_table = &fog_table;
  backend.load_fragment_module = fragment_load;
  backend.combiner = true;
  backend.live_raster_modules = true;
  backend.live_fog_modules = profile;
  char error[256] = "";
  live_vk_renderer *renderer = live_vk_renderer_create(
      &r->device, r->colour_pass, &backend, error, sizeof error);
  CHECK(renderer != NULL);
  if (renderer == NULL)
    return;
  gpu_pgraph *model = fog_model(enable, mode, p0, p1, colour);
  const float clear[4] = {0, 0, 0, 1};
  frame_result result;
  render_live(r, renderer, LIVE_VK_PASS_WINDOW, model, clear, &result);
  if (refusal == NULL) {
    CHECK(result.drawn == (multi_draw?2u:1u) && result.refused == 0u);
    const uint8_t *pixel =
        result.image.pixels + 16u * result.image.stride_bytes + 16u * 4u;
    for (unsigned i = 0; i < 3; i++)
      CHECK(abs((int)pixel[i] - (int)rgb[i]) <= 1);
    if(multi_draw){
      const uint8_t *second=result.image.pixels+40u*result.image.stride_bytes+40u*4u;
      CHECK(second[0]==255u&&second[1]==0u&&second[2]==0u);
      printf("same-batch disabled/enabled fog pixels green/red\n");
    }
    printf("fog enable=%u mode=%x pixel=%u,%u,%u\n", enable, mode, pixel[0],
           pixel[1], pixel[2]);
  } else {
    CHECK(result.drawn == 0u && result.refused == 1u);
    CHECK(strstr(result.first_refusal, refusal) != NULL);
    printf("fog refusal %s\n", result.first_refusal);
  }
  gpu_image_free(&result.image);
  gpu_pgraph_destroy(model);
  live_vk_renderer_destroy(renderer);
}
int main(int argc, char **argv) {
  if (argc != 2 || !read_shader(argv[1], 0u) || !read_shader(argv[1], 1u) || !read_shader(argv[1],2u) || !read_shader(argv[1],3u))
    return 2;
  gpu_device *device = NULL;
  if (gpu_device_create_selected("software", &device) != GPU_OK)
    return 77;
  rig r;
  CHECK(rig_init(&r, device));
  static const float corners[3][3] = {
      {4, 4, 0.5f}, {60, 4, 0.5f}, {4, 60, 0.5f}};
  static const float green_colour[4] = {0, 1, 0, 1};
  put_triangle(0, corners, green_colour);
  static const unsigned green[] = {0, 255, 0}, red[] = {255, 0, 0},
                        half[] = {128, 128, 0}, blue[] = {0, 0, 255};
  /* Four literal xemu reference pixels, raw ABGR red has alphaZERO. */
  one_case(&r, 0, 0x2601, 1, -1, 0x000000FF, true, EVERYTHING, green, NULL);
  one_case(&r, 1, 0x2601, 1, -1, 0x000000FF, true, EVERYTHING, red, NULL);
  one_case(&r, 1, 0x2601, 1.5f, -1, 0x000000FF, true, EVERYTHING, half, NULL);
  one_case(&r, 1, 0x800, 1.5f, -1, 0x000000FF, true, EVERYTHING, green, NULL);
  one_case(&r, 1, 0x2601, 1, -1, 0x00FF0000, true, EVERYTHING, blue, NULL);
  one_case(&r, 1, 0x2601, 1, -1, 0x000000FF, false, EVERYTHING, red, "fog");
  one_case(&r, 1, 0xBAD, 1, -1, 0x000000FF, true, EVERYTHING, red, "fog mode");
  one_case(&r, 1, 0x2601, 1, -1, 0x000000FF, true,
           EVERYTHING & ~GPU_COMBINER_INFER_CONSTANT_BYTES, red, "fog");
  explicit_fog=true;
  static const unsigned quarter[]={191,64,0};
  one_case(&r,1,0x800,1.5f,-0.125f,0x000000FF,true,EVERYTHING,quarter,NULL);
  /* Factors [-1,1,1] interpolate negative at16.5,16.5; early vertex
   * clamp would yield green114 instead of zero. */
  ((float_vertex *)memory)[0].colour[3]=2.0f;
  ((float_vertex *)memory)[1].colour[3]=0.0f;
  ((float_vertex *)memory)[2].colour[3]=0.0f;
  one_case(&r,1,0x2601,2.0f,-1.0f,0x000000FF,true,EVERYTHING,red,NULL);
  explicit_fog=false;multi_draw=true;
  static const float left[3][3]={{4,4,.5f},{30,4,.5f},{4,30,.5f}};
  static const float right[3][3]={{34,34,.5f},{62,34,.5f},{34,62,.5f}};
  put_triangle(0,left,green_colour);put_triangle(1,right,green_colour);
  one_case(&r,0,0x2601,1,-1,0x000000FF,true,EVERYTHING,green,NULL);
  multi_draw=false;omit_color=true;alpha_only=true;
  static const unsigned white[]={255,255,255},grey[]={128,128,128},black[]={0,0,0};
  one_case(&r,0,0x2601,1,-1,0,true,EVERYTHING,white,NULL);
  one_case(&r,1,0x2601,1.5f,-1,0,true,EVERYTHING,grey,NULL);
  one_case(&r,1,0x2601,1,-1,0,true,EVERYTHING,black,NULL);
  alpha_only=false;
  one_case(&r,0,0x2601,1,-1,0,true,EVERYTHING,green,"fog colour");
  r.fn.vkDestroyRenderPass(r.native.device, r.colour_pass, NULL);
  gpu_device_destroy(device);
  free(point_modules[0]);
  free(point_modules[1]);
  free(point_modules[2]);
  free(point_modules[3]);
  printf("T1059 live fog %d checks %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
