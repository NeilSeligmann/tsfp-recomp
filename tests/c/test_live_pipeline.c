/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T791: live NV2A state to Vulkan pipeline description, cache key, cache and refusal census (src/gpu/live_pipeline.c).
 *
 * Layers: 1. ORACLE, device free: the description of a live snapshot is the replay's own resolvers' answer (program,
 * output, topology, every refusal message). 2. KEY: each static field changes the key, no dynamic field does.
 * 3. CACHE: hits, misses, LRU eviction with destroy, a failing device. 4. CENSUS: every refused draw is named in order.
 * 5. REPLAY PARITY on a real device when one exists (skipped with a stated note otherwise, the Vulkan half could not be
 * run in the authoring container).
 */
#include "gpu_device.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"
#include "gpu_pgraph_test_support.h"
#include "gpu_pgraph_vertex_words.h"
#include "live_pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(condition)) {                                                                   \
            failures++;                                                                       \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);                             \
        }                                                                                     \
    } while (0)

#define WIDTH 64u
#define HEIGHT 64u
#define MEMORY_BASE 0x00600000u
#define ALL_GROUPS (GPU_PGRAPH_OUTPUT_SCISSOR | GPU_PGRAPH_OUTPUT_CULL | GPU_PGRAPH_OUTPUT_BLEND | \
                    GPU_PGRAPH_OUTPUT_ALPHA_TEST | GPU_PGRAPH_OUTPUT_DEPTH_STENCIL)

static const char *const module_names[] = {SYNTHETIC_PROGRAM_NAME};
static const struct gpu_vsh_table table = {0u, 0u, NULL, 0u, NULL, 1u, module_names};
static uint8_t memory[0x400];

static bool load_module(void *context, uint32_t module, const uint32_t **words, size_t *word_count)
{
    (void)context;
    if (module != 0u) {
        return false;
    }
    *words = draw_vertex_words;
    *word_count = sizeof draw_vertex_words / sizeof(uint32_t);
    return true;
}

typedef struct {
    float position[3];
    float v2[4];
} float_vertex;

static void fill_memory(void)
{
    static const float corners[3][3] = {{-0.9f, -0.9f, 0.5f}, {0.9f, -0.9f, 0.5f}, {-0.9f, 0.9f, 0.5f}};
    memset(memory, 0, sizeof memory);
    for (uint32_t i = 0u; i < 3u; i++) {
        const float_vertex vertex = {{corners[i][0], corners[i][1], corners[i][2]}, {0.0f, 0.0f, 1.0f, 1.0f}};
        memcpy(memory + i * sizeof vertex, &vertex, sizeof vertex);
    }
}

static void stream_setup(stream_builder *stream)
{
    static const float offset[4] = {32.0f, 32.0f, 0.0f, 0.0f};
    static const float scale[4] = {32.0f, -32.0f, 1.0f, 0.0f};
    stream_viewport(stream, offset, scale);
    stream_pair(stream, GPU_PGRAPH_EXECUTION_MODE, 6u);
    stream_program(stream, 0u, synthetic_program, 1u);
    stream_pair(stream, GPU_PGRAPH_PROGRAM_START, 0u);
    static const float c3_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    stream_constants(stream, 3u, c3_zero, 4u);
    stream_array(stream, 1u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    stream_array(stream, 2u, MEMORY_BASE + 12u, array_format(28u, 4u, GPU_PGRAPH_TYPE_F));
}

static void stream_blend(stream_builder *stream, uint32_t enable, uint32_t source, uint32_t destination,
                         uint32_t equation)
{
    stream_pair(stream, 0x0304u, enable);
    stream_pair(stream, 0x0348u, destination);
    stream_pair(stream, 0x0344u, source);
    stream_pair(stream, 0x0350u, equation);
}

static void stream_depth_state(stream_builder *stream, uint32_t enable, uint32_t function, uint32_t mask)
{
    stream_pair(stream, 0x030Cu, enable);
    stream_pair(stream, 0x0354u, function);
    stream_pair(stream, 0x035Cu, mask);
}

static void stream_stencil_state(stream_builder *stream, uint32_t reference, uint32_t function_mask,
                                 uint32_t write_mask)
{
    stream_pair(stream, 0x032Cu, 1u);
    stream_pair(stream, 0x0364u, 0x202u);
    stream_pair(stream, 0x0368u, reference);
    stream_pair(stream, 0x036Cu, function_mask);
    stream_pair(stream, 0x0360u, write_mask);
    stream_pair(stream, 0x0370u, 0x1E00u);
    stream_pair(stream, 0x0374u, 0x1E00u);
    stream_pair(stream, 0x0378u, 0x1E00u);
}

static void stream_cull_mode(stream_builder *stream, uint32_t mode, uint32_t stored_front)
{
    stream_pair(stream, 0x0308u, mode != 0u ? 1u : 0u);
    if (mode != 0u) {
        stream_pair(stream, 0x039Cu, 0x404u + (mode != stored_front ? 1u : 0u));
    }
}

typedef struct {
    gpu_pgraph *model;
    fake_guest guest;
} scene;

/* Decode setup, then `extra`, then one draw of `op` over three vertices. */
static scene make_scene(void (*extra)(stream_builder *), uint32_t op)
{
    scene result;
    result.guest = (fake_guest){MEMORY_BASE, memory, sizeof memory};
    result.model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(result.model, ALL_GROUPS);
    stream_builder stream = {0};
    stream_setup(&stream);
    if (extra != NULL) {
        extra(&stream);
    }
    stream_draw_arrays(&stream, op, 0u, 3u);
    CHECK(gpu_pgraph_decode(result.model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(result.model) == 1u);
    stream_free(&stream);
    return result;
}

static const gpu_pgraph_state *scene_state(const scene *s)
{
    return gpu_pgraph_snapshot(s->model, gpu_pgraph_draw_at(s->model, 0u)->snapshot);
}

static gpu_pgraph_backend make_backend(fake_guest *guest, uint32_t inferences)
{
    gpu_pgraph_backend backend = {0};
    backend.table = &table;
    backend.read_guest = fake_guest_read;
    backend.load_module = load_module;
    backend.context = guest;
    backend.allowed_inferences = inferences;
    backend.output_groups = ALL_GROUPS;
    return backend;
}

#define EVERYTHING (GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_OUTPUT_ALL)

static void blend_extra(stream_builder *stream)
{
    stream_blend(stream, 1u, 0x302u, 0x303u, 0x8006u);
}

static void depth_extra(stream_builder *stream)
{
    stream_depth_state(stream, 1u, 0x203u, 1u);
}

static void cull_extra(stream_builder *stream)
{
    stream_pair(stream, 0x03A0u, 0x900u);
    stream_cull_mode(stream, 0x901u, 0x900u);
}

static void cull_no_front_extra(stream_builder *stream)
{
    stream_cull_mode(stream, 0x901u, 0x900u);
}

static void stencil_extra(stream_builder *stream)
{
    stream_depth_state(stream, 1u, 0x203u, 1u);
    stream_stencil_state(stream, 7u, 0xFFu, 0xFFu);
}

/* --- 1. oracle: the description is the replay's resolvers' answer ------------------------- */

static void check_oracle(void (*extra)(stream_builder *), uint32_t op, const char *name)
{
    printf("oracle %s\n", name);
    scene s = make_scene(extra, op);
    gpu_pgraph_backend backend = make_backend(&s.guest, EVERYTHING);
    const gpu_pgraph_state *state = scene_state(&s);
    live_pipeline_description description;
    live_pipeline_stage stage;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    CHECK(live_pipeline_resolve(state, op, 0u, &backend, WIDTH, HEIGHT, &description, &stage, &report) ==
          GPU_PGRAPH_OK);
    if (stage != LIVE_STAGE_NONE) {
        printf("    refused: %s\n", report.error);
    }
    CHECK(stage == LIVE_STAGE_NONE);

    gpu_pgraph_program program;
    gpu_pgraph_output output;
    CHECK(gpu_pgraph_resolve_program(state, &backend, &program, &report) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_resolve_output(state, &backend, WIDTH, HEIGHT, &output, &report) == GPU_PGRAPH_OK);
    CHECK(description.program.module == program.module && strcmp(description.program.digest, program.digest) == 0);
    CHECK(description.program.instructions == 1u);
    CHECK(description.output.active == output.active);
    CHECK(memcmp(&description.output.output.cull_mode, &output.output.cull_mode, sizeof output.output.cull_mode) == 0);
    CHECK(description.output.output.blend == output.output.blend);
    CHECK(description.output.output.blend_source == output.output.blend_source);
    CHECK(description.output.output.depth_test == output.output.depth_test);
    CHECK(description.output.output.depth_func == output.output.depth_func);
    CHECK(description.output.output.stencil_test == output.output.stencil_test);
    CHECK(description.output.used_inferences == output.used_inferences);
    CHECK((description.used_inferences & output.used_inferences) == output.used_inferences);
    CHECK((description.used_inferences & GPU_PGRAPH_INFER_PROGRAM_HEADER) != 0u);

    /* the topology the replay's own assembly decides */
    gpu_pgraph_assembled assembled;
    CHECK(gpu_pgraph_assemble_draw(s.model, 0u, &backend, &assembled, &report) == GPU_PGRAPH_OK);
    CHECK(description.topology == assembled.topology);
    gpu_pgraph_assembled_free(&assembled);
    gpu_pgraph_destroy(s.model);
}

static void test_oracle(void)
{
    check_oracle(NULL, GPU_PGRAPH_OP_TRIANGLES, "plain triangles");
    check_oracle(blend_extra, GPU_PGRAPH_OP_TRIANGLES, "blend");
    check_oracle(depth_extra, GPU_PGRAPH_OP_TRIANGLES, "depth");
    check_oracle(cull_extra, GPU_PGRAPH_OP_TRIANGLES, "cull");
    check_oracle(stencil_extra, GPU_PGRAPH_OP_TRIANGLES, "stencil");
    check_oracle(NULL, GPU_PGRAPH_OP_LINES, "lines");
    check_oracle(NULL, GPU_PGRAPH_OP_POINTS, "points");
    /* the exported mapping itself, one line per class */
    CHECK(gpu_pgraph_topology_of(GPU_PGRAPH_OP_POINTS) == GPU_VSH_TOPOLOGY_POINT_LIST);
    CHECK(gpu_pgraph_topology_of(GPU_PGRAPH_OP_LINES) == GPU_VSH_TOPOLOGY_LINE_LIST);
    CHECK(gpu_pgraph_topology_of(GPU_PGRAPH_OP_LINE_STRIP) == GPU_VSH_TOPOLOGY_LINE_LIST);
    CHECK(gpu_pgraph_topology_of(GPU_PGRAPH_OP_TRIANGLES) == GPU_VSH_TOPOLOGY_TRIANGLE_LIST);
    CHECK(gpu_pgraph_topology_of(GPU_PGRAPH_OP_QUADS) == GPU_VSH_TOPOLOGY_TRIANGLE_LIST);
}

/* --- 2. key ------------------------------------------------------------------------------- */

static live_pipeline_description full_description(void)
{
    live_pipeline_description description;
    memset(&description, 0, sizeof description);
    description.output.active = true;
    description.output.output.blend = true;
    description.output.output.depth_test = true;
    description.output.output.stencil_test = true;
    description.output.output.alpha_test = true;
    description.output.output.depth_bias = true;
    description.has_fragment = true;
    return description;
}

static bool key_differs(const live_pipeline_description *a, const live_pipeline_description *b)
{
    live_pipeline_key key_a, key_b;
    live_pipeline_key_build(a, &key_a);
    live_pipeline_key_build(b, &key_b);
    return memcmp(&key_a, &key_b, sizeof key_a) != 0;
}

#define STATIC_FIELD_DIFFERS(field, value)                                  \
    do {                                                                    \
        live_pipeline_description changed = base;                           \
        changed.field = (value);                                            \
        mutants++;                                                          \
        CHECK(key_differs(&base, &changed));                                \
    } while (0)

#define DYNAMIC_FIELD_SAME(field, value)                                    \
    do {                                                                    \
        live_pipeline_description changed = base;                           \
        changed.field = (value);                                            \
        dynamics++;                                                         \
        CHECK(!key_differs(&base, &changed));                               \
    } while (0)

static void test_key(void)
{
    printf("key\n");
    live_pipeline_description base = full_description();
    base.output.output.blend_source = 4u;
    base.output.output.blend_destination = 5u;
    base.output.output.stencil_func = 3u;
    int mutants = 0;
    int dynamics = 0;
    STATIC_FIELD_DIFFERS(program.module, 1u);
    STATIC_FIELD_DIFFERS(fragment.module, 3u);
    STATIC_FIELD_DIFFERS(fragment.plan.texture_stages, 1u);
    STATIC_FIELD_DIFFERS(has_fragment, false);
    STATIC_FIELD_DIFFERS(topology, GPU_VSH_TOPOLOGY_LINE_LIST);
    STATIC_FIELD_DIFFERS(output.active, false);
    STATIC_FIELD_DIFFERS(output.output.cull_mode, GPU_VSH_CULL_BACK);
    STATIC_FIELD_DIFFERS(output.output.front_clockwise, true);
    STATIC_FIELD_DIFFERS(output.output.blend, false);
    STATIC_FIELD_DIFFERS(output.output.blend_source, 6u);
    STATIC_FIELD_DIFFERS(output.output.blend_destination, 7u);
    STATIC_FIELD_DIFFERS(output.output.blend_equation, GPU_VSH_BLEND_OP_MAX);
    STATIC_FIELD_DIFFERS(output.output.color_write_disable, GPU_VSH_CHANNEL_A);
    STATIC_FIELD_DIFFERS(output.output.alpha_test, false);
    STATIC_FIELD_DIFFERS(output.output.alpha_func, GPU_VSH_COMPARE_GREATER);
    STATIC_FIELD_DIFFERS(output.output.alpha_ref, 77u);
    STATIC_FIELD_DIFFERS(output.output.depth_test, false);
    STATIC_FIELD_DIFFERS(output.output.depth_write, true);
    STATIC_FIELD_DIFFERS(output.output.depth_func, GPU_VSH_COMPARE_LESS);
    STATIC_FIELD_DIFFERS(output.output.stencil_test, false);
    STATIC_FIELD_DIFFERS(output.output.stencil_func, GPU_VSH_COMPARE_LESS);
    STATIC_FIELD_DIFFERS(output.output.stencil_compare_mask, 0x0Fu);
    STATIC_FIELD_DIFFERS(output.output.stencil_write_mask, 0x0Fu);
    STATIC_FIELD_DIFFERS(output.output.stencil_fail_op, GPU_VSH_STENCIL_OP_ZERO);
    STATIC_FIELD_DIFFERS(output.output.stencil_zfail_op, GPU_VSH_STENCIL_OP_ZERO);
    STATIC_FIELD_DIFFERS(output.output.stencil_zpass_op, GPU_VSH_STENCIL_OP_ZERO);
    STATIC_FIELD_DIFFERS(output.output.depth_bias, false);
    STATIC_FIELD_DIFFERS(output.output.polygon_mode, GPU_VSH_POLYGON_LINE); /* T860 */
    CHECK(mutants == 28);
    DYNAMIC_FIELD_SAME(output.output.scissor, true);
    DYNAMIC_FIELD_SAME(output.output.scissor_x, 9u);
    DYNAMIC_FIELD_SAME(output.output.scissor_width, 9u);
    DYNAMIC_FIELD_SAME(output.output.blend_constant[2], 0.5f);
    DYNAMIC_FIELD_SAME(output.output.stencil_ref, 77u);
    DYNAMIC_FIELD_SAME(output.output.depth_bias_constant, 2.0f);
    DYNAMIC_FIELD_SAME(output.output.depth_bias_slope, 2.0f);
    DYNAMIC_FIELD_SAME(used_inferences, 0xFFu);
    CHECK(dynamics == 8);
    /* blend factors are not baked while blending is off, stencil ops not while the stencil test is off */
    live_pipeline_description off = base;
    off.output.output.blend = false;
    off.output.output.stencil_test = false;
    live_pipeline_description off_other = off;
    off_other.output.output.blend_source = 9u;
    off_other.output.output.stencil_zpass_op = GPU_VSH_STENCIL_OP_INVERT;
    CHECK(!key_differs(&off, &off_other));
    /* the texel coordinate rewrite is baked (stages and sizes), only for the stages it applies to and only with a combiner */
    live_pipeline_description texel = full_description();
    texel.fragment.plan.texture_stages = 3u;
    texel.texel_stages = 1u;
    texel.texel_size[0][0] = 8u;
    texel.texel_size[0][1] = 4u;
    live_pipeline_description texel_changed = texel;
    CHECK(!key_differs(&texel, &texel_changed));
    texel_changed.texel_size[0][1] = 16u;
    CHECK(key_differs(&texel, &texel_changed));
    texel_changed = texel;
    texel_changed.texel_size[0][0] = 16u;
    CHECK(key_differs(&texel, &texel_changed));
    texel_changed = texel;
    texel_changed.texel_stages = 3u;
    texel_changed.texel_size[1][0] = 2u;
    CHECK(key_differs(&texel, &texel_changed));
    texel_changed = texel;
    texel_changed.texel_stages = 0u;
    CHECK(key_differs(&texel, &texel_changed));
    texel_changed = texel;
    texel_changed.texel_stages = 2u; /* another stage, both sizes zero: only the mask tells it from no rewrite */
    texel_changed.texel_size[0][0] = 0u;
    texel_changed.texel_size[0][1] = 0u;
    live_pipeline_description texel_none = texel_changed;
    texel_none.texel_stages = 0u;
    CHECK(key_differs(&texel_changed, &texel_none));
    texel_changed = texel;
    texel_changed.texel_size[2][0] = 99u; /* a stage that is not a texel stage */
    CHECK(!key_differs(&texel, &texel_changed));
    texel_changed = texel;
    texel_changed.has_fragment = false; /* no combiner, no rewrite */
    texel.has_fragment = false;
    texel.texel_stages = 0u;
    CHECK(!key_differs(&texel, &texel_changed));
    /* the dynamic struct carries what the key leaves out */
    live_pipeline_description dyn = full_description();
    dyn.output.output.scissor = true;
    dyn.output.output.scissor_x = 3u;
    dyn.output.output.scissor_y = 4u;
    dyn.output.output.scissor_width = 5u;
    dyn.output.output.scissor_height = 6u;
    dyn.output.output.blend_constant[3] = 0.25f;
    dyn.output.output.stencil_ref = 9u;
    dyn.output.output.alpha_ref = 10u;
    dyn.output.output.depth_bias_constant = 1.5f;
    dyn.output.output.depth_bias_slope = 2.5f;
    live_pipeline_dynamic dynamic;
    live_pipeline_dynamic_build(&dyn, NULL, WIDTH, HEIGHT, &dynamic);
    CHECK(dynamic.viewport_width == WIDTH && dynamic.viewport_height == HEIGHT);
    CHECK(dynamic.scissor && dynamic.scissor_x == 3u && dynamic.scissor_y == 4u);
    CHECK(dynamic.scissor_width == 5u && dynamic.scissor_height == 6u);
    CHECK(dynamic.blend_constant[3] == 0.25f && dynamic.stencil_ref == 9u && dynamic.alpha_ref == 10u);
    CHECK(dynamic.depth_bias_constant == 1.5f && dynamic.depth_bias_slope == 2.5f);
    /* each enable word is its own: enabled with every other field zero must differ from disabled */
    live_pipeline_description zero;
    memset(&zero, 0, sizeof zero);
    live_pipeline_description zero_active = zero;
    zero_active.output.active = true;
    CHECK(key_differs(&zero, &zero_active));
    live_pipeline_description zero_blend = zero_active;
    zero_blend.output.output.blend = true;
    CHECK(key_differs(&zero_active, &zero_blend));
    live_pipeline_description zero_stencil = zero_active;
    zero_stencil.output.output.stencil_test = true;
    CHECK(key_differs(&zero_active, &zero_stencil));
    /* the hash reads every byte of every word */
    live_pipeline_key low, high;
    memset(&low, 0, sizeof low);
    for (size_t word = 0u; word < LIVE_PIPELINE_KEY_WORDS; word += 7u) {
        for (uint32_t shift = 0u; shift < 32u; shift += 8u) {
            high = low;
            high.words[word] = 0xA5u << shift;
            CHECK(live_pipeline_key_hash(&low) != live_pipeline_key_hash(&high));
        }
    }
    /* equal keys hash equal, different keys hash differently */
    live_pipeline_key key_a, key_b;
    live_pipeline_key_build(&base, &key_a);
    live_pipeline_key_build(&base, &key_b);
    CHECK(live_pipeline_key_hash(&key_a) == live_pipeline_key_hash(&key_b));
    live_pipeline_description other = base;
    other.output.output.depth_func = GPU_VSH_COMPARE_LESS;
    live_pipeline_key_build(&other, &key_b);
    CHECK(live_pipeline_key_hash(&key_a) != live_pipeline_key_hash(&key_b));
}

/* --- 3. cache ----------------------------------------------------------------------------- */

typedef struct {
    int creates;
    int destroys;
    bool fail;
    uint64_t next_handle;
    uint64_t destroyed_last;
} fake_device;

static bool fake_create(void *context, const live_pipeline_description *description,
                        const live_pipeline_key *key, void **handle, char *error, size_t error_bytes)
{
    fake_device *device = context;
    (void)description;
    (void)key;
    if (device->fail) {
        snprintf(error, error_bytes, "fake device out of memory");
        return false;
    }
    device->creates++;
    *handle = (void *)(uintptr_t)(++device->next_handle);
    return true;
}

static void fake_destroy(void *context, void *handle)
{
    fake_device *device = context;
    device->destroys++;
    device->destroyed_last = (uint64_t)(uintptr_t)handle;
}

static gpu_pgraph_state stencil_state_with(uint32_t function_mask, uint32_t write_mask)
{
    gpu_pgraph *model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(model, ALL_GROUPS);
    stream_builder stream = {0};
    stream_setup(&stream);
    stream_depth_state(&stream, 1u, 0x203u, 1u);
    stream_stencil_state(&stream, 1u, function_mask, write_mask);
    CHECK(gpu_pgraph_decode(model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    stream_free(&stream);
    const gpu_pgraph_state state = *gpu_pgraph_state_now(model);
    gpu_pgraph_destroy(model);
    return state;
}

static uint64_t constant_hash(const live_pipeline_key *key)
{
    (void)key;
    return 42u;
}

/* two different keys with the SAME hash are two pipelines: the cache compares the key, not just the hash */
static void test_collision(void)
{
    printf("collision\n");
    fill_memory();
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = make_backend(&guest, EVERYTHING);
    fake_device device = {0};
    live_pipeline_cache cache;
    const live_pipeline_ops ops = {fake_create, fake_destroy, &device};
    live_pipeline_cache_init(&cache, &ops);
    cache.hash = constant_hash;
    gpu_pgraph_state first = stencil_state_with(0xFFu, 0xFFu);
    gpu_pgraph_state other = stencil_state_with(0x0Fu, 0xFFu);
    live_pipeline_selection one, two, again;
    CHECK(live_pipeline_select(&cache, NULL, &first, GPU_PGRAPH_OP_TRIANGLES, 0u, &backend, WIDTH, HEIGHT, &one) == GPU_PGRAPH_OK);
    CHECK(live_pipeline_select(&cache, NULL, &other, GPU_PGRAPH_OP_TRIANGLES, 1u, &backend, WIDTH, HEIGHT, &two) == GPU_PGRAPH_OK);
    CHECK(!two.cache_hit && two.handle != one.handle && device.creates == 2);
    CHECK(live_pipeline_select(&cache, NULL, &first, GPU_PGRAPH_OP_TRIANGLES, 2u, &backend, WIDTH, HEIGHT, &again) == GPU_PGRAPH_OK);
    CHECK(again.cache_hit && again.handle == one.handle);
    live_pipeline_cache_destroy(&cache);
}

static void test_cache(void)
{
    printf("cache\n");
    fill_memory();
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    gpu_pgraph_backend backend = make_backend(&guest, EVERYTHING);
    fake_device device = {0};
    live_pipeline_cache cache;
    live_pipeline_census census;
    const live_pipeline_ops ops = {fake_create, fake_destroy, &device};
    live_pipeline_cache_init(&cache, &ops);
    live_pipeline_census_init(&census);

    gpu_pgraph_state first = stencil_state_with(0xFFu, 0xFFu);
    live_pipeline_selection one, two, three;
    CHECK(live_pipeline_select(&cache, &census, &first, GPU_PGRAPH_OP_TRIANGLES, 0u, &backend, WIDTH, HEIGHT, &one) ==
          GPU_PGRAPH_OK);
    CHECK(!one.cache_hit && one.handle != NULL && device.creates == 1);
    CHECK(live_pipeline_select(&cache, &census, &first, GPU_PGRAPH_OP_TRIANGLES, 1u, &backend, WIDTH, HEIGHT, &two) ==
          GPU_PGRAPH_OK);
    CHECK(two.cache_hit && two.handle == one.handle && device.creates == 1);
    CHECK(cache.hits == 1u && cache.misses == 1u);
    /* the same state at another size reuses the pipeline: the viewport is dynamic */
    CHECK(live_pipeline_select(&cache, &census, &first, GPU_PGRAPH_OP_TRIANGLES, 2u, &backend, 128u, 32u, &three) ==
          GPU_PGRAPH_OK);
    CHECK(three.cache_hit && three.dynamic.viewport_width == 128u && three.dynamic.viewport_height == 32u);
    /* another topology and another state are other pipelines */
    CHECK(live_pipeline_select(&cache, &census, &first, GPU_PGRAPH_OP_POINTS, 3u, &backend, WIDTH, HEIGHT, &two) ==
          GPU_PGRAPH_OK);
    CHECK(!two.cache_hit && two.handle != one.handle && device.creates == 2);
    gpu_pgraph_state other = stencil_state_with(0x0Fu, 0xFFu);
    CHECK(live_pipeline_select(&cache, &census, &other, GPU_PGRAPH_OP_TRIANGLES, 4u, &backend, WIDTH, HEIGHT, &two) ==
          GPU_PGRAPH_OK);
    CHECK(!two.cache_hit && device.creates == 3 && device.destroys == 0);
    CHECK(census.selected == 5u && census.count == 0u);

    /* fill to the capacity, refresh `first` with a hit, then overflow by one: the victim is the least recently USED
     * pipeline, which is the points one (handle 2) only because the hit moved `first` (handle 1) out of the way */
    uint32_t distinct = 3u;
    uint32_t mask = 1u;
    for (; distinct < LIVE_PIPELINE_CACHE_CAPACITY && mask < 0x100u; mask++) {
        gpu_pgraph_state variant = stencil_state_with(0xFFu, mask == 0xFFu ? 0u : mask);
        live_pipeline_select(&cache, &census, &variant, GPU_PGRAPH_OP_TRIANGLES, 100u + mask, &backend, WIDTH, HEIGHT,
                             &three);
        distinct = (uint32_t)device.creates;
    }
    CHECK(distinct == LIVE_PIPELINE_CACHE_CAPACITY);
    CHECK(cache.evictions == 0u && device.destroys == 0);
    CHECK(live_pipeline_select(&cache, &census, &first, GPU_PGRAPH_OP_TRIANGLES, 899u, &backend, WIDTH, HEIGHT, &two) ==
              GPU_PGRAPH_OK &&
          two.cache_hit && two.handle == (void *)(uintptr_t)1u);
    gpu_pgraph_state overflow = stencil_state_with(0xFFu, 0u);
    CHECK(live_pipeline_select(&cache, &census, &overflow, GPU_PGRAPH_OP_TRIANGLES, 898u, &backend, WIDTH, HEIGHT,
                               &three) == GPU_PGRAPH_OK);
    CHECK(!three.cache_hit && device.creates == (int)LIVE_PIPELINE_CACHE_CAPACITY + 1);
    CHECK(cache.evictions == 1u && device.destroys == 1);
    CHECK(device.destroyed_last == 2u);
    CHECK(live_pipeline_select(&cache, &census, &first, GPU_PGRAPH_OP_TRIANGLES, 900u, &backend, WIDTH, HEIGHT, &two) ==
              GPU_PGRAPH_OK &&
          two.cache_hit);
    CHECK(live_pipeline_select(&cache, &census, &first, GPU_PGRAPH_OP_POINTS, 901u, &backend, WIDTH, HEIGHT, &two) ==
              GPU_PGRAPH_OK &&
          !two.cache_hit && cache.evictions == 2u);
    const int destroys_before = device.destroys;
    live_pipeline_cache_destroy(&cache);
    CHECK(device.destroys - destroys_before == (int)LIVE_PIPELINE_CACHE_CAPACITY);

    /* a device that cannot build the pipeline refuses the draw, names it in the census, caches nothing */
    live_pipeline_cache_init(&cache, &ops);
    device.fail = true;
    const size_t refused_before = census.count;
    CHECK(live_pipeline_select(&cache, &census, &first, GPU_PGRAPH_OP_TRIANGLES, 950u, &backend, WIDTH, HEIGHT, &one) ==
          GPU_PGRAPH_ERR_DEVICE);
    CHECK(one.handle == NULL && cache.creates_failed == 1u);
    CHECK(census.count == refused_before + 1u);
    CHECK(census.refusals[census.count - 1u].stage == LIVE_STAGE_DEVICE && census.refusals[census.count - 1u].draw == 950u);
    CHECK(strstr(census.refusals[census.count - 1u].reason, "fake device out of memory") != NULL);
    device.fail = false;
    CHECK(live_pipeline_select(&cache, &census, &first, GPU_PGRAPH_OP_TRIANGLES, 951u, &backend, WIDTH, HEIGHT, &one) ==
              GPU_PGRAPH_OK &&
          !one.cache_hit);
    live_pipeline_cache_destroy(&cache);
    live_pipeline_census_free(&census);
}

/* --- 4. census ---------------------------------------------------------------------------- */

static void test_census_helpers(void)
{
    printf("census helpers\n");
    live_pipeline_census census;
    live_pipeline_census_init(&census);
    census.selected = 3u;
    live_pipeline_census_refuse_selected(&census, 7u, LIVE_STAGE_VERTEX, GPU_PGRAPH_ERR_UNMEASURED, "no vertex data");
    CHECK(census.selected == 2u && census.count == 1u && census.by_stage[LIVE_STAGE_VERTEX] == 1u);
    CHECK(census.refusals[0].draw == 7u && census.refusals[0].stage == LIVE_STAGE_VERTEX);
    CHECK(strcmp(census.refusals[0].reason, "no vertex data") == 0);
    live_pipeline_census_refuse(&census, 9u, LIVE_STAGE_DEVICE, GPU_PGRAPH_ERR_DEVICE, "arena full");
    CHECK(census.selected == 2u && census.count == 2u && census.by_stage[LIVE_STAGE_DEVICE] == 1u);
    CHECK(strcmp(live_pipeline_stage_name(LIVE_STAGE_VERTEX), "vertex") == 0);
    live_pipeline_census_refuse(NULL, 1u, LIVE_STAGE_DEVICE, GPU_PGRAPH_ERR_DEVICE, "ignored");
    live_pipeline_census_refuse_selected(NULL, 1u, LIVE_STAGE_DEVICE, GPU_PGRAPH_ERR_DEVICE, "ignored");
    census.selected = 0u;
    live_pipeline_census_refuse_selected(&census, 11u, LIVE_STAGE_VERTEX, GPU_PGRAPH_ERR_UNMEASURED, "again"); /* never below 0 */
    CHECK(census.selected == 0u && census.count == 3u);
    FILE *sink = tmpfile();
    CHECK(sink != NULL);
    if (sink != NULL) {
        live_pipeline_census_print(&census, sink);
        rewind(sink);
        char text[2048] = "";
        const size_t length = fread(text, 1u, sizeof text - 1u, sink);
        text[length] = '\0';
        fclose(sink);
        CHECK(strstr(text, "[vertex] no vertex data") != NULL && strstr(text, "vertex 2 device 1)") != NULL);
    }
    live_pipeline_census_free(&census);
}

static void test_census(void)
{
    printf("census\n");
    fill_memory();
    fake_guest guest = {MEMORY_BASE, memory, sizeof memory};
    fake_device device = {0};
    live_pipeline_cache cache;
    live_pipeline_census census;
    const live_pipeline_ops ops = {fake_create, fake_destroy, &device};
    live_pipeline_cache_init(&cache, &ops);
    live_pipeline_census_init(&census);
    live_pipeline_selection selection;

    scene s = make_scene(NULL, GPU_PGRAPH_OP_TRIANGLES);
    const gpu_pgraph_state *state = scene_state(&s);
    gpu_pgraph_backend open = make_backend(&guest, EVERYTHING);
    /* 0 drawn */
    CHECK(live_pipeline_select(&cache, &census, state, GPU_PGRAPH_OP_TRIANGLES, 0u, &open, WIDTH, HEIGHT, &selection) ==
          GPU_PGRAPH_OK);
    /* 1 no program inference allowed: the replay's own message */
    gpu_pgraph_backend closed = make_backend(&guest, 0u);
    gpu_pgraph_report oracle;
    memset(&oracle, 0, sizeof oracle);
    gpu_pgraph_program program;
    CHECK(gpu_pgraph_resolve_program(state, &closed, &program, &oracle) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(live_pipeline_select(&cache, &census, state, GPU_PGRAPH_OP_TRIANGLES, 1u, &closed, WIDTH, HEIGHT,
                               &selection) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(selection.handle == NULL);
    /* 2 points without the point size inference: the primitive stage */
    gpu_pgraph_backend no_points = make_backend(&guest, EVERYTHING & ~GPU_PGRAPH_INFER_POINT_SIZE);
    CHECK(live_pipeline_select(&cache, &census, state, GPU_PGRAPH_OP_POINTS, 2u, &no_points, WIDTH, HEIGHT,
                               &selection) == GPU_PGRAPH_ERR_UNMEASURED);
    /* 3 no program written at all: the program stage again */
    gpu_pgraph_state empty;
    memset(&empty, 0, sizeof empty);
    CHECK(live_pipeline_select(&cache, &census, &empty, GPU_PGRAPH_OP_TRIANGLES, 3u, &open, WIDTH, HEIGHT,
                               &selection) != GPU_PGRAPH_OK);
    /* 4 an output inference not allowed (blend model) */
    scene blended = make_scene(blend_extra, GPU_PGRAPH_OP_TRIANGLES);
    gpu_pgraph_backend no_blend = make_backend(&guest, EVERYTHING & ~GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL);
    CHECK(live_pipeline_select(&cache, &census, scene_state(&blended), GPU_PGRAPH_OP_TRIANGLES, 4u, &no_blend, WIDTH,
                               HEIGHT, &selection) == GPU_PGRAPH_ERR_UNMEASURED);
    /* 5 culling with the front face never written: the output stage, the replay's own refusal */
    scene culled = make_scene(cull_no_front_extra, GPU_PGRAPH_OP_TRIANGLES);
    CHECK(live_pipeline_select(&cache, &census, scene_state(&culled), GPU_PGRAPH_OP_TRIANGLES, 5u, &open, WIDTH, HEIGHT,
                               &selection) == GPU_PGRAPH_ERR_UNMEASURED);
    /* 6 NULL state */
    CHECK(live_pipeline_select(&cache, &census, NULL, GPU_PGRAPH_OP_TRIANGLES, 6u, &open, WIDTH, HEIGHT, &selection) ==
          GPU_PGRAPH_ERR_ARGUMENT);

    CHECK(census.selected == 1u);
    CHECK(census.count == 6u);
    const size_t expected_draws[6] = {1u, 2u, 3u, 4u, 5u, 6u};
    const live_pipeline_stage expected_stages[6] = {LIVE_STAGE_PROGRAM, LIVE_STAGE_PRIMITIVE, LIVE_STAGE_PROGRAM,
                                                    LIVE_STAGE_OUTPUT, LIVE_STAGE_OUTPUT, LIVE_STAGE_PROGRAM};
    for (size_t index = 0u; index < census.count && index < 6u; index++) {
        CHECK(census.refusals[index].draw == expected_draws[index]);
        CHECK(census.refusals[index].stage == expected_stages[index]);
        CHECK(census.refusals[index].reason[0] != '\0');
    }
    CHECK(strcmp(census.refusals[0].reason, oracle.error) == 0); /* the resolver's own text, not a paraphrase */
    CHECK(strstr(census.refusals[1].reason, "point") != NULL);
    CHECK(census.by_stage[LIVE_STAGE_PROGRAM] == 3u && census.by_stage[LIVE_STAGE_PRIMITIVE] == 1u &&
          census.by_stage[LIVE_STAGE_OUTPUT] == 2u && census.by_stage[LIVE_STAGE_FRAGMENT] == 0u);
    CHECK((census.used_inferences & GPU_PGRAPH_INFER_PROGRAM_HEADER) != 0u);
    /* many refusals are all kept, not just the first few */
    for (size_t draw = 10u; draw < 210u; draw++) {
        live_pipeline_select(&cache, &census, state, GPU_PGRAPH_OP_TRIANGLES, draw, &closed, WIDTH, HEIGHT, &selection);
    }
    CHECK(census.count == 206u && census.refusals[205].draw == 209u);
    /* the printed census names every refused draw */
    FILE *sink = tmpfile();
    CHECK(sink != NULL);
    if (sink != NULL) {
        live_pipeline_census_print(&census, sink);
        rewind(sink);
        char line[512];
        size_t lines = 0u;
        bool names_209 = false;
        bool summary = false;
        while (fgets(line, sizeof line, sink) != NULL) {
            lines++;
            names_209 |= strstr(line, "refused draw 209 [program]") != NULL;
            summary |= strstr(line, "census: selected 1 refused 206 (program 203 fragment 0 output 2 primitive 1 vertex 0 device 0)") != NULL;
        }
        CHECK(lines == 207u && names_209 && summary);
        fclose(sink);
    }
    gpu_pgraph_destroy(s.model);
    gpu_pgraph_destroy(blended.model);
    gpu_pgraph_destroy(culled.model);
    live_pipeline_cache_destroy(&cache);
    live_pipeline_census_free(&census);
}

/* --- 5. parity with the replay on a real device ------------------------------------------- */

static void test_replay_parity(void)
{
    printf("replay parity\n");
    if (!gpu_vulkan_available()) {
        printf("  SKIP: no Vulkan loader, the replay cannot run here\n");
        return;
    }
    gpu_device *device = NULL;
    if (gpu_device_create_selected("software", &device) != GPU_OK) {
        printf("  SKIP: no software Vulkan device\n");
        return;
    }
    fill_memory();
    scene s = make_scene(blend_extra, GPU_PGRAPH_OP_TRIANGLES);
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int closed = 0; closed < 2; closed++) {
        gpu_pgraph_backend backend = make_backend(&s.guest, closed ? 0u : EVERYTHING);
        gpu_image image = {0};
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        const gpu_pgraph_result replay = gpu_pgraph_replay(s.model, device, &backend, WIDTH, HEIGHT, clear, &image, &report);
        live_pipeline_description description;
        live_pipeline_stage stage;
        gpu_pgraph_report mine;
        memset(&mine, 0, sizeof mine);
        const gpu_pgraph_result live = live_pipeline_resolve(scene_state(&s), GPU_PGRAPH_OP_TRIANGLES, 0u, &backend,
                                                             WIDTH, HEIGHT, &description, &stage, &mine);
        CHECK((replay == GPU_PGRAPH_OK) == (live == GPU_PGRAPH_OK));
        CHECK(closed ? live != GPU_PGRAPH_OK : live == GPU_PGRAPH_OK);
        if (closed) {
            CHECK(strcmp(report.error, mine.error) == 0);
        }
        gpu_image_free(&image);
    }
    gpu_pgraph_destroy(s.model);
    gpu_device_destroy(device);
}

/* --- 6. T870: a fixed-mode draw is refused by name, the T846 inference is deleted ---------------------------------------- */

typedef struct {
    uint32_t mode;
    bool slot0;
} fixed_case;

static scene make_fixed_scene(const fixed_case *spec)
{
    scene result;
    result.guest = (fake_guest){MEMORY_BASE, memory, sizeof memory};
    result.model = gpu_pgraph_create();
    gpu_pgraph_set_output_groups(result.model, ALL_GROUPS | GPU_PGRAPH_OUTPUT_FIXED);
    stream_builder stream = {0};
    stream_setup(&stream);
    stream_pair(&stream, GPU_PGRAPH_EXECUTION_MODE, spec->mode);
    if (spec->slot0) {
        stream_array(&stream, 0u, MEMORY_BASE, array_format(28u, 3u, GPU_PGRAPH_TYPE_F));
    }
    stream_draw_arrays(&stream, GPU_PGRAPH_OP_TRIANGLES, 0u, 3u);
    CHECK(gpu_pgraph_decode(result.model, stream.pairs, stream.count) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_draw_count(result.model) == 1u);
    stream_free(&stream);
    return result;
}

static gpu_pgraph_result resolve_fixed_case(const fixed_case *spec, live_pipeline_description *description,
                                            live_pipeline_stage *stage, char *message, size_t message_bytes)
{
    scene s = make_fixed_scene(spec);
    gpu_pgraph_backend backend = make_backend(&s.guest, EVERYTHING);
    backend.output_groups = ALL_GROUPS | GPU_PGRAPH_OUTPUT_FIXED;
    gpu_pgraph_report report;
    memset(&report, 0, sizeof report);
    const gpu_pgraph_result result =
        live_pipeline_resolve(scene_state(&s), GPU_PGRAPH_OP_TRIANGLES, 0u, &backend, WIDTH, HEIGHT, description, stage, &report);
    snprintf(message, message_bytes, "%s", report.error);
    gpu_pgraph_destroy(s.model);
    return result;
}

static void test_target_viewport_backend(void)
{
    const fixed_case spec = {0};
    scene s = make_fixed_scene(&spec);
    gpu_pgraph_backend backend = make_backend(&s.guest, 0u);
    const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(s.model, 0u);
    const gpu_pgraph_backend default_draw = live_pipeline_draw_backend(&backend, draw, 320u, 240u);
    CHECK(default_draw.viewport_width == 320u && default_draw.viewport_height == 240u);
    CHECK(!default_draw.viewport_from_target);
    backend.allowed_inferences |= GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET;
    const gpu_pgraph_backend inferred_draw = live_pipeline_draw_backend(&backend, draw, 640u, 480u);
    CHECK(inferred_draw.viewport_width == 640u && inferred_draw.viewport_height == 480u);
    CHECK(inferred_draw.viewport_from_target);
    CHECK((inferred_draw.allowed_inferences & GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET) != 0u);
    gpu_pgraph_destroy(s.model);
}

static void test_fixed_mode_refused(void)
{
    printf("fixed mode refused by name (T870)\n");
    live_pipeline_description description;
    live_pipeline_stage stage;
    char message[320];
    /* the old T846 bit is gone: 0x200 is in no mask and a mode word with low bits 0 is refused at the program stage, with or
     * without a slot 0 array, a started program or not */
    CHECK((GPU_PGRAPH_INFER_ALL & 0x200u) == 0u && (GPU_PGRAPH_INFER_OUTPUT_ALL & 0x200u) == 0u);
    const fixed_case intro_shape = {4u, false};
    CHECK(resolve_fixed_case(&intro_shape, &description, &stage, message, sizeof message) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(stage == LIVE_STAGE_PROGRAM && strstr(message, "not the program mode") != NULL);
    const fixed_case with_array = {4u, true};
    CHECK(resolve_fixed_case(&with_array, &description, &stage, message, sizeof message) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(stage == LIVE_STAGE_PROGRAM && strstr(message, "not the program mode") != NULL);
    const fixed_case zero = {0u, false};
    CHECK(resolve_fixed_case(&zero, &description, &stage, message, sizeof message) == GPU_PGRAPH_ERR_UNMEASURED);
    /* the same program in the program mode resolves */
    const fixed_case program_mode = {6u, false};
    CHECK(resolve_fixed_case(&program_mode, &description, &stage, message, sizeof message) == GPU_PGRAPH_OK);
    CHECK(stage == LIVE_STAGE_NONE && !description.program.mode_inferred);
}

int main(void)
{
    fill_memory();
    test_oracle();
    test_key();
    test_cache();
    test_collision();
    test_census();
    test_census_helpers();
    test_target_viewport_backend();
    test_fixed_mode_refused();
    test_replay_parity();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
