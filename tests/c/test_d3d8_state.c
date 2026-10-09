/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The render-state helpers and shader-constant setters the title calls after CreateDevice
 * (src/gpu/d3d8_state.c), dispatched through the D3D8 table.
 *
 * EVERY GLOBAL ADDRESS BELOW IS WHERE THE ORIGINAL STORES: read from each helper's body and
 * confirmed by running the original under the emulated oracle (tools/d3dscan/oracle.py), where the
 * set of D3D8 dwords each call changed was IDENTICAL to this port's for all 18 call shapes tried
 * (every state value the title uses plus the branch-taking neighbours), and for four constant
 * registers. The floats for state 0x95 are what x87 `fild / fchs / fmul` produced.
 */

#include "test_d3d8_support.h"

#include "d3d8_state.h"
#include "d3d8_surface.h"

#define DEVICE 0x003E3F60u

static uint32_t call_state(uint32_t address, uint32_t value)
{
    return call_stdcall(address, &value, 1u);
}

/* Device fixture, built as the original's CreateDevice (0x003D9230, d3d8_create_device) leaves it: the
 * pushbuffer and kickoff sizes at their globals, the ring created (device + 0 put, + 4 limit, + 0x24
 * the ring base, as tests/c/test_d3d8_method_packet.c make_ring) and the device pointer slot at 0x3E3F58
 * holding D3D8_DEVICE_BASE, so the handlers read the device struct at 0x3E3F60. MUTATION: dropping the
 * ring makes state 0x93 the T1172 named fatal, dropping the slot aborts state 0xA3 on an unreadable device. */
static void create_device_fixture(void)
{
    (void)d3d8_device_register();
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    CHECK(load(DEVICE + 0x24u) != 0u);
}

static void test_each_helper_stores_its_state(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    create_device_fixture();

    /* One row per helper: the dispatch address, the value the title passes and the global the
     * original stores it to. MUTATION: any store moved to a neighbour fails its own line. */
    static const struct {
        uint32_t address;
        uint32_t value;
        uint32_t global;
    } rows[] = {
        {0x003D7060u, 0x00000007u, 0x003E3F0Cu}, /* state 0x93 */
        {0x003D7F70u, 0x00000009u, 0x003E3F00u}, /* state 0x90 */
        {0x003D8010u, 0x00001E00u, 0x003E3F04u}, /* state 0x91 */
        {0x003D7150u, 0x12345678u, 0x003E3F10u}, /* state 0x94 */
        {0x003D80B0u, 0x00000001u, 0x003E3F44u}, /* state 0xA1 */
        {0x003D8190u, 0x00000001u, 0x003E3F4Cu}, /* state 0xA3 */
        {0x003D81B0u, 0x00000001u, 0x003E3F50u}, /* state 0xA4 */
        {0x003D7EE0u, 0x00000001u, 0x003E3EFCu}, /* state 0x8F */
        {0x003D81F0u, 0x00000000u, 0x003E3F28u}, /* state 0x9A */
        {0x003D72A0u, 0x00000003u, 0x003E3F14u}, /* state 0x95 */
    };
    /* The render target is not the first back buffer: 0x9A only stores (the recompute is the test below). */
    store(DEVICE + 0x1A04u, 0x003E59A0u);
    for (size_t index = 0u; index < sizeof(rows) / sizeof(rows[0]); index++) {
        store(rows[index].global, 0xCCCCCCCCu);
        (void)call_state(rows[index].address, rows[index].value);
        CHECK_EQ_U32(load(rows[index].global), rows[index].value);
    }
    environment_end();
}

static void test_control_words(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    create_device_fixture();
    store(DEVICE + 0x2428u, 0xFFFFFFFFu);
    store(DEVICE + 0x242Cu, 0xFFFFFFFFu);
    store(0x003E3F54u, 0u);
    store(0x003E3F4Cu, 0u);
    store(0x003E3F50u, 0u);

    /* With all three sources zero the masks clear bit 3 of the first word and bits 20, 27 and
     * the neighbours the original's 0xE7EFFFFF removes from the second. MUTATION: a mask of
     * 0xE7FFFFFF leaves bit 20 set. */
    (void)call_state(0x003D8190u, 0u);
    CHECK_EQ_U32(load(DEVICE + 0x2428u), 0xFFFFFFF7u);
    CHECK_EQ_U32(load(DEVICE + 0x242Cu), 0xE7EFFFFFu);

    /* State 0xA3 set sets bit 20; state 0xA4 set sets bit 27; the first word's bit 3 follows the
     * global at 0x3E3F54. MUTATION: each source wired to the wrong bit fails one. */
    (void)call_state(0x003D8190u, 1u);
    CHECK_EQ_U32(load(DEVICE + 0x242Cu), 0xE7FFFFFFu);
    (void)call_state(0x003D81B0u, 1u);
    CHECK_EQ_U32(load(DEVICE + 0x242Cu), 0xEFFFFFFFu);
    store(0x003E3F54u, 1u);
    d3d8_state_update_control_words();
    CHECK_EQ_U32(load(DEVICE + 0x2428u), 0xFFFFFFFFu);

    /* Clearing a source clears its bit again. */
    (void)call_state(0x003D8190u, 0u);
    CHECK_EQ_U32(load(DEVICE + 0x242Cu), 0xEFEFFFFFu);
    environment_end();
}

/* T1171 modelled the value-2 cascade (original 0x003D7EE0 -> 0x003D6E50, 0x003D5C50, 0x003D7A50, 0x003D7860), so
 * entering or leaving value 2 now runs it and announces nothing: the old "does not model" announcement this
 * test asserted is superseded (T1211). The cascade reads a declaration at device + 0x794 (flags word zero: no fog
 * program) and the viewport inputs, set up as the 0x9A test does. */
static void test_state_8f_runs_the_value_two_cascade(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    create_device_fixture();
    CHECK(d3d8_hle_unmodelled_count() == 0u);
    seed_recompute_constants();
    static const uint32_t identity[16] = {
        0x3F800000u, 0u, 0u, 0u, 0u, 0x3F800000u, 0u, 0u,
        0u, 0u, 0x3F800000u, 0u, 0u, 0u, 0u, 0x3F800000u,
    };
    for (uint32_t index = 0u; index < 16u; index++) {
        store(DEVICE + 0xCA0u + index * 4u, identity[index]);
    }
    const uint32_t surface = SCRATCH_DATA;
    store(surface + D3D8_SURFACE_COMMON, 0x01050001u);
    store(surface + D3D8_SURFACE_FORMAT, 0x00011229u);
    store(surface + D3D8_SURFACE_SIZE, (479u << 12) | 639u);
    store(DEVICE + 0x794u, surface + 0x100u);
    store(surface + 0x104u, 0u);
    store(DEVICE + 0x1A04u, surface);
    store(DEVICE + 0x1A14u, surface);
    store(DEVICE + 0x1A18u, surface);
    store(DEVICE + 0x96Cu, 0x3F800000u);
    store(DEVICE + 0x970u, 0x3F800000u);
    store(DEVICE + 0x964u, 0x3F800000u);

    /* Neither side is 2: only the two-packet preamble (0x4030C, 0x41D78), 16 bytes. */
    uint32_t before = load(DEVICE);
    CHECK_EQ_U32(call_state(0x003D7EE0u, 1u), 0u);
    CHECK_EQ_U32(load(DEVICE) - before, 16u);
    CHECK_EQ_U32(load(before), 0x0004030Cu);

    /* Entering 2 runs the cascade: more than the preamble reaches the ring, nothing is announced.
     * MUTATION: the cascade dropped leaves exactly 16 bytes. */
    before = load(DEVICE);
    (void)call_state(0x003D7EE0u, 2u);
    CHECK(load(DEVICE) - before > 16u);
    CHECK(d3d8_hle_unmodelled_count() == 0u);
    /* Leaving 2 runs it again. */
    before = load(DEVICE);
    (void)call_state(0x003D7EE0u, 1u);
    CHECK(load(DEVICE) - before > 16u);
    CHECK(d3d8_hle_unmodelled_count() == 0u);
    environment_end();
}

/* T598: the title's own 0x003D81F0 runs 0x003D7B80 and SetViewport(NULL) at the first back buffer, as the library's
 * own call does (T533, compared with the original in tests/test_d3d8_scaled_viewport_oracle.py), and announces
 * nothing. */
static void test_state_9a_runs_the_scaled_viewport_recompute(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    (void)d3d8_device_register();
    store(0x003E3F58u, DEVICE);
    seed_recompute_constants();
    static const uint32_t identity[16] = {
        0x3F800000u, 0u, 0u, 0u, 0u, 0x3F800000u, 0u, 0u,
        0u, 0u, 0x3F800000u, 0u, 0u, 0u, 0u, 0x3F800000u,
    };
    for (uint32_t index = 0u; index < 16u; index++) {
        store(DEVICE + 0xCA0u + index * 4u, identity[index]);
    }
    /* The back buffer and the front buffer are linear 640 by 480 headers (the Size word holds width - 1 and
     * height - 1), the sample scales are 1.0. */
    const uint32_t surface = SCRATCH_DATA;
    store(surface + D3D8_SURFACE_COMMON, 0x01050001u);
    store(surface + D3D8_SURFACE_FORMAT, 0x00011229u);
    store(surface + D3D8_SURFACE_SIZE, (479u << 12) | 639u);
    store(DEVICE + 0x794u, surface + 0x100u); /* a fixed function declaration: flags word zero */
    store(surface + 0x104u, 0u);
    store(DEVICE + 0x1A04u, surface);
    store(DEVICE + 0x1A14u, surface);
    store(DEVICE + 0x1A18u, surface);
    store(DEVICE + 0x96Cu, 0x3F800000u);
    store(DEVICE + 0x970u, 0x3F800000u);
    store(DEVICE + 0x964u, 0x3F800000u);

    /* The render target is the first back buffer: the clip size is the front buffer's, and the 0x40208 pair, the
     * 0x41D7C pair and the viewport packets reach the ring (the cursor is the put pointer at device + 0).
     * MUTATION: the handler reverted to a store and an announcement writes nothing and announces. */
    const uint32_t before = load(DEVICE);
    (void)call_state(0x003D81F0u, 2u);
    CHECK_EQ_U32(load(0x003E3F28u), 2u);
    CHECK(d3d8_hle_unmodelled_count() == 0u);
    CHECK_EQ_U32(load(DEVICE + 0x954u), 320u);
    CHECK_EQ_U32(load(DEVICE + 0x958u), 240u);
    CHECK(load(DEVICE) - before >= 8u + 8u + 36u + 12u);
    bool saw_surface_format = false;
    bool saw_multisample = false;
    for (uint32_t cursor = before; cursor < load(DEVICE); cursor += 4u) {
        saw_surface_format = saw_surface_format || load(cursor) == 0x00040208u;
        saw_multisample = saw_multisample || load(cursor) == 0x00041D7Cu;
    }
    CHECK(saw_surface_format);
    CHECK(saw_multisample);

    /* Away from the first back buffer only the store: no packet, no change. MUTATION: an inverted compare runs
     * the recompute on the other side. */
    store(DEVICE + 0x1A04u, surface + 0x20u);
    const uint32_t after = load(DEVICE);
    (void)call_state(0x003D81F0u, 3u);
    CHECK_EQ_U32(load(0x003E3F28u), 3u);
    CHECK_EQ_U32(load(DEVICE), after);
    environment_end();
}

static void test_state_95_floats_and_shadows(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    create_device_fixture();

    /* Zero: -0.0 for both floats (0x80000000), and the three flags are 0. Render states 0x4D to
     * 0x51 shadow at 0x3E3CC0 + 4 * state. MUTATION: a missing fchs gives +0.0. */
    (void)call_state(0x003D72A0u, 0u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x4Du * 4u), 0x80000000u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x4Eu * 4u), 0x80000000u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x4Fu * 4u), 0u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x50u * 4u), 0u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x51u * 4u), 0u);

    /* 5: -5.0 is 0xC0A00000 and the scaled value, times 0.25, is -1.25 = 0xBFA00000. State 0x4D
     * takes the SCALED float and 0x4E the unscaled one (the order of the two pushes). The three
     * flags are 1. MUTATION: swapping them or a scale of 0.5 fails. */
    (void)call_state(0x003D72A0u, 5u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x4Du * 4u), 0xBFA00000u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x4Eu * 4u), 0xC0A00000u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x4Fu * 4u), 1u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x50u * 4u), 1u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x51u * 4u), 1u);

    /* A value with the sign bit set is widened by 2^32 first, so it is read as unsigned: the
     * original adds the float at 0x475CCC when `fild` comes out negative. 0xFFFFFFFE is
     * 4294967294.0, which rounds to -4294967296.0 (0xCF800000) as a float, and a quarter of it
     * is -1073741823.5, which rounds to -1073741824.0 (0xCE800000). MUTATION: reading it as a
     * signed -2 gives 0x40000000 and 0xBF000000... */
    (void)call_state(0x003D72A0u, 0xFFFFFFFEu);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x4Du * 4u), 0xCE800000u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x4Eu * 4u), 0xCF800000u);
    CHECK_EQ_U32(load(0x003E3CC0u + 0x4Fu * 4u), 1u);
    environment_end();
}

static void test_constant_mode(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    /* Explicit synthetic device identity for this standalone fixture. */
    store(D3D8_DEVICE_POINTER_SLOT, DEVICE);
    const uint32_t start = 0x00A00000u;
    map_fixed(start, 0x1000u);
    store(DEVICE, start);
    store(DEVICE + 4u, start + 0x800u);
    store(DEVICE + 8u, 0x4000u);
    store(0x003E3AB8u, 0u);
    for (uint32_t index = 0u; index < 28u; ++index) {
        store(0x003E1898u + index * 4u, 0xA0000000u + index);
    }
    CHECK_EQ_U32(call_state(0x003D5AF0u, 0x11u), 1u);
    CHECK_EQ_U32(load(DEVICE + 8u), 0x4200u);
    CHECK_EQ_U32(load(DEVICE + 0x1928u), 1u);
    CHECK_EQ_U32(load(0x003E3AB8u), 0u);
    CHECK_EQ_U32(load(DEVICE), start);
    CHECK(d3d8_hle_unmodelled_count() == 0u);

    CHECK_EQ_U32(call_state(0x003D5AF0u, 0x10u), 0u);
    CHECK_EQ_U32(load(DEVICE + 0x1928u), 0u);
    CHECK_EQ_U32(load(0x003E3AB8u), 0x1600u);
    CHECK_EQ_U32(load(DEVICE + 8u), 0x4200u);
    CHECK_EQ_U32(load(DEVICE), start + 372u);
    CHECK_EQ_U32(load(start), 0x41EA4u);
    CHECK_EQ_U32(load(start + 4u), 0x3Cu);
    CHECK_EQ_U32(load(start + 8u), 0x300B80u);
    CHECK_EQ_U32(load(start + 12u), 0xA0000010u);
    CHECK_EQ_U32(load(start + 60u), 0x400840u);
    CHECK_EQ_U32(load(start + 64u), 0xA0000000u);
    CHECK_EQ_U32(load(start + 68u), 0xA0000004u);
    CHECK_EQ_U32(load(start + 332u), 0x1009D0u);
    CHECK_EQ_U32(load(start + 344u), 0x3F800000u);
    CHECK_EQ_U32(load(start + 352u), 0x100A50u);
    CHECK_EQ_U32(load(start + 368u), 0x3F800000u);
    CHECK(d3d8_hle_unmodelled_count() == 0u);
    CHECK(d3d8_pushbuffer_dwords_written() == 93u);

    CHECK_EQ_U32(call_state(0x003D5AF0u, 0u), 0u);
    CHECK_EQ_U32(load(DEVICE + 8u), 0x4000u);
    CHECK_EQ_U32(load(DEVICE), start + 744u);
    environment_end();
}

static void test_vertex_shader_constant(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    create_device_fixture();
    const uint32_t source = SCRATCH_DATA;
    store(source + 0u, 0x11111111u);
    store(source + 4u, 0x22222222u);
    store(source + 8u, 0x33333333u);
    store(source + 12u, 0x44444444u);

    /* Register 0xB lands at 0x3E2EA0 + 0xB * 16, four dwords, from the pointer in edx. MUTATION:
     * a shift of 3 or ecx and edx swapped puts it elsewhere. */
    (void)call_fastcall(0x003D5670u, 0xBu, source);
    CHECK_EQ_U32(load(0x003E2EA0u + 0xBu * 16u + 0u), 0x11111111u);
    CHECK_EQ_U32(load(0x003E2EA0u + 0xBu * 16u + 4u), 0x22222222u);
    CHECK_EQ_U32(load(0x003E2EA0u + 0xBu * 16u + 8u), 0x33333333u);
    CHECK_EQ_U32(load(0x003E2EA0u + 0xBu * 16u + 12u), 0x44444444u);
    CHECK_EQ_U32(load(0x003E2EA0u + 0xBu * 16u + 16u), 0u);

    /* The last register of the 192-entry shadow works, the next is fatal (the original would
     * write past the shadow). MUTATION: a bound of 193 accepts it. */
    (void)call_fastcall(0x003D5670u, 191u, source);
    CHECK_EQ_U32(load(0x003E2EA0u + 191u * 16u), 0x11111111u);
    RUN_EXPECTING_FATAL((void)call_fastcall(0x003D5670u, 192u, source));
    CHECK(fatal_seen);
    CHECK(fatal_address == 0x003D5670u);
    environment_end();
}

int main(void)
{
    test_each_helper_stores_its_state();
    test_control_words();
    test_state_8f_runs_the_value_two_cascade();
    test_state_9a_runs_the_scaled_viewport_recompute();
    test_state_95_floats_and_shadows();
    test_constant_mode();
    test_vertex_shader_constant();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
