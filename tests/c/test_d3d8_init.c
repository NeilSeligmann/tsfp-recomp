/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The D3D8 init sequence the title performs in InitD3D (0x00023930): Direct3DCreate8,
 * SetPushBufferSize, CreateDevice, GetBackBuffer2 and the pushbuffer primitive, dispatched
 * through the D3D8 table exactly as src/host/xdk_thunk.c dispatches them.
 *
 * EVERY EXPECTED WORD IS A LITERAL THE ORIGINAL PRODUCED. `tools/d3dscan/oracle.py` runs the
 * retail library bytes under an x86 emulator against a model of the kernel and the NV2A
 * registers, and for the title's own presentation parameters (640x480 A8R8G8B8, D24S8, flags
 * 0x140, 60 Hz) it left these words in D3D8's state: the three surface headers at 0x003E5984,
 * 0x003E599C and 0x003E59CC with Common 0x010D0002 / 0x01050002 / 0x010D0002, Format
 * 0x00011229 / 0x00011229 / 0x00012E29, Size 0x271DF27F; device+0x1A0C 0x128, +0x948
 * 0x4B7FFFFF, +0x94C 0xA00; the display object's mode word 0x88110F01; the dirty mask
 * 0x00FF7F7F. The same run on this port agreed on all of them.
 *
 * THE DISPLAY TABLE HERE IS SYNTHETIC (see test_d3d8_support.h), so a literal that depends on it
 * (the mode word 0x88110F01 is the one real row) was chosen to match what the real table gives.
 */

#include "test_d3d8_support.h"

#include "d3d8_display.h"

#define DEVICE 0x003E3F60u

static void test_registration(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    /* 24 handlers: the 7 init addresses, CreateBuffer, Clear, the ten render-state helpers,
     * the constant mode, the constant setter and the vertical blank callback setter (its
     * surface row joined suite_addresses for the T86 replay fixture), plus BeginPush and
     * EndPush (T854)... counted by the table above. MUTATION: a handler dropped from the table
     * reduces this. */
    /* T990 replaces the selected 003D5670 generic handler after registering
     * it:25 successful registrations,24 unique implemented table entries. */
    CHECK(d3d8_device_register() == 25u);
    CHECK(d3d8_hle_implemented_count() == 24u);

    /* Re-initialising the table drops every handler, which is why the host must register AFTER
     * adopting the surface. */
    d3d8_hle_shutdown();
    environment_end();
}

static void test_create_and_push_buffer_size(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();

    /* 0x003D9000 is `mov eax, 1; ret 4`: eax is 1 whatever the argument. MUTATION: returning the
     * argument or 0 fails. */
    uint32_t args[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    CHECK_EQ_U32(call_stdcall(0x003D9000u, args, 1u), 1u);
    args[0] = 0x12345678u;
    CHECK_EQ_U32(call_stdcall(0x003D9000u, args, 1u), 1u);

    /* 0x003D9210 stores arg0 at 0x3E6404 and arg1 at 0x3E6400 and leaves arg0 in eax. */
    args[0] = 0x100000u;
    args[1] = 0x10000u;
    CHECK_EQ_U32(call_stdcall(0x003D9210u, args, 2u), 0x100000u);
    CHECK_EQ_U32(load(0x003E6404u), 0x100000u);
    CHECK_EQ_U32(load(0x003E6400u), 0x10000u);
    environment_end();
}

static void run_create_device(uint32_t flags, uint32_t behavior, uint32_t *hr, uint32_t *out)
{
    const uint32_t parameters = SCRATCH_DATA;
    write_title_parameters(parameters, flags);
    *out = SCRATCH_DATA + 0x200u;
    store(*out, 0xDEADBEEFu);
    const uint32_t args[6] = {0u, 1u, 0u, behavior, parameters, *out};
    *hr = call_stdcall(0x003D9230u, args, 6u);
}

static void test_create_device_success(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    const uint32_t size_args[2] = {0x100000u, 0x10000u};
    (void)call_stdcall(0x003D9210u, size_args, 2u);
    store(0x475CCCu, 0x4F800000u);
    store(0x475CD4u, 0x3F000000u);
    store(0x4760C4u, 0xBF000000u);
    store(0x475C78u, 0x3F800000u);

    uint32_t hr = 0xFFFFFFFFu;
    uint32_t out = 0u;
    run_create_device(0x140u, 0u, &hr, &out);

    /* The return value and the out-pointer. The title never reads the out-pointer, but the
     * original writes the device address through it. MUTATION: returning failure or not writing
     * fails. */
    CHECK_EQ_U32(hr, 0u);
    CHECK_EQ_U32(load(out), DEVICE);
    CHECK_EQ_U32(load(D3D8_DEVICE_POINTER_SLOT), DEVICE);
    CHECK_EQ_U32(load(0x003E4898u), 1u);

    /* The dirty mask, which the game ORs into. */
    CHECK_EQ_U32(load(0x003E3AB8u), 0x00FF7F7Fu);

    /* The three surface headers, word for word. */
    CHECK_EQ_U32(load(DEVICE + 0x1A14u), 0x003E5984u);
    CHECK_EQ_U32(load(DEVICE + 0x1A18u), 0x003E599Cu);
    CHECK_EQ_U32(load(DEVICE + 0x1A20u), 0x003E59CCu);
    CHECK_EQ_U32(load(DEVICE + 0x1A10u), 2u);
    CHECK_EQ_U32(load(0x003E5984u + 0x00u), 0x010D0002u);
    CHECK_EQ_U32(load(0x003E5984u + 0x08u), 0u);
    CHECK_EQ_U32(load(0x003E5984u + 0x0Cu), 0x00011229u);
    CHECK_EQ_U32(load(0x003E5984u + 0x10u), 0x271DF27Fu);
    CHECK_EQ_U32(load(0x003E5984u + 0x14u), 0u);
    CHECK_EQ_U32(load(0x003E599Cu + 0x00u), 0x01050002u);
    CHECK_EQ_U32(load(0x003E599Cu + 0x0Cu), 0x00011229u);
    CHECK_EQ_U32(load(0x003E599Cu + 0x10u), 0x271DF27Fu);
    CHECK_EQ_U32(load(0x003E59CCu + 0x00u), 0x010D0002u);
    CHECK_EQ_U32(load(0x003E59CCu + 0x0Cu), 0x00012E29u);
    CHECK_EQ_U32(load(0x003E59CCu + 0x10u), 0x271DF27Fu);

    /* Data is the physical address of each allocation (the kernel module's own answer), and the
     * three allocations are distinct, each 0x12C000 bytes (480 * 2560). MUTATION: a shared
     * allocation or the wrong size makes two overlap. */
    const uint32_t back = load(DEVICE + 0x1A84u);
    const uint32_t front = load(DEVICE + 0x1A88u);
    const uint32_t depth = load(DEVICE + 0x1A8Cu);
    CHECK(back != 0u && front != 0u && depth != 0u);
    CHECK(back != front && front != depth && back != depth);
    CHECK_EQ_U32(back & 0x3FFFu, 0u);
    CHECK_EQ_U32(load(0x003E5984u + 0x04u), guest_physical_address(back) & 0x0FFFFFFFu);
    CHECK_EQ_U32(load(0x003E599Cu + 0x04u), guest_physical_address(front) & 0x0FFFFFFFu);
    CHECK_EQ_U32(load(0x003E59CCu + 0x04u), guest_physical_address(depth) & 0x0FFFFFFFu);
    const guest_region *region = guest_region_at(back);
    CHECK(region != NULL && region->size == 0x12C000u);

    /* The render target binding and its derived words. */
    CHECK_EQ_U32(load(DEVICE + 0x1A04u), 0x003E5984u);
    CHECK_EQ_U32(load(DEVICE + 0x1A08u), 0x003E59CCu);
    CHECK_EQ_U32(load(DEVICE + 0x1A0Cu), 0x128u);
    CHECK_EQ_U32(load(DEVICE + 0x948u), 0x4B7FFFFFu);
    CHECK_EQ_U32(load(DEVICE + 0x94Cu), 0xA00u);
    /* SetRenderTarget rebuilds the viewport matrix from CreateDevice's identity composite. */
    CHECK_EQ_U32(load(DEVICE + 0x980u), 0x43A00000u);
    CHECK_EQ_U32(load(DEVICE + 0x994u), 0xC3700000u);
    CHECK_EQ_U32(load(DEVICE + 0x9B0u), 0x43A00000u);
    CHECK_EQ_U32(load(DEVICE + 0x9B4u), 0x43700000u);
    CHECK_EQ_U32(load(DEVICE + 0xCA0u), 0x3F800000u);
    CHECK_EQ_U32(load(DEVICE + 0xCA0u + 0x14u), 0x3F800000u);
    CHECK_EQ_U32(load(DEVICE + 0xCA0u + 0x28u), 0x3F800000u);
    CHECK_EQ_U32(load(DEVICE + 0xCA0u + 0x3Cu), 0x3F800000u);

    /* T885: retail CreateDevice's default-state table seeds render-state shadows 0x8B and 0x8C
     * to FILL. These are later restored by copy composition, so an omitted init emits invalid 0. */
    CHECK_EQ_U32(load(0x003E3EECu), 0x1B02u);
    CHECK_EQ_U32(load(0x003E3EF0u), 0x1B02u);

    /* Presentation-derived fields. */
    CHECK_EQ_U32(load(DEVICE + 0x196Cu), 0x11u);
    CHECK_EQ_U32(load(DEVICE + 0x1970u), 3u);
    CHECK_EQ_U32(load(DEVICE + 0x96Cu), 0x3F800000u);
    CHECK_EQ_U32(load(DEVICE + 0x970u), 0x3F800000u);
    CHECK_EQ_U32(load(0x003E3EB8u), 2u);
    CHECK_EQ_U32(load(0x003E3EBCu), 1u);
    CHECK_EQ_U32(load(0x003E3F28u), 0u);
    /* Original CreateDevice leaves render-target and projection flags set. */
    CHECK_EQ_U32(load(DEVICE + 8u), 0x4003u);

    /* The display object: register window, pitch, mode word, row flags and marker. */
    CHECK_EQ_U32(load(DEVICE + 0x1C28u), 0xFD000000u);
    CHECK_EQ_U32(load(DEVICE + 0x934u), 0xFD000000u);
    CHECK_EQ_U32(load(0x003E3AACu), 0xFD000000u);
    CHECK_EQ_U32(load(DEVICE + 0x1C2Cu), 0xA00u);
    CHECK_EQ_U32(load(DEVICE + 0x1C30u), 0x88110F01u);
    CHECK_EQ_U32(load(DEVICE + 0x1C34u), 0x12u);
    CHECK_EQ_U32(load(DEVICE + 0x1DDCu), 0x02480104u);
    CHECK_EQ_U32(load(DEVICE + 0x1DE0u), 1u);
    CHECK_EQ_U32(load(DEVICE + 0x2404u), 1u);

    /* The two filter calls the original ends with, and their caches. MUTATION: dropping either
     * call leaves its cache word zero and its log line absent. */
    CHECK_EQ_U32(load(0x003E2B98u), 5u);
    CHECK_EQ_U32(load(0x003E2BA0u), 1u);
    CHECK_EQ_U32(load(0x003E2B94u), 0u);
    CHECK_EQ_U32(load(0x003E2B9Cu), 1u);
    CHECK(captured_has("option 0xB, param 5"));
    CHECK(captured_has("option 0xE, param 0"));

    /* CreateDevice starts the ring with the measured transform execution mode packet, then its
     * own fence insertion (0x003D67B0 from 0x003DB087) writes eight dwords. */
    const uint32_t ring = load(DEVICE + 0x24u);
    CHECK(ring != 0u);
    CHECK_EQ_U32(load(DEVICE + 0x00u), ring + 0x178u);
    CHECK_EQ_U32(load(ring), 0x00081E94u);
    CHECK_EQ_U32(load(ring + 4u), 6u);
    CHECK_EQ_U32(load(ring + 8u), 0u);
    CHECK_EQ_U32(load(ring + 12u), 0x0004A310u);
    CHECK_EQ_U32(load(ring + 20u), 0x00041D70u);
    /* Host init subset now includes original fog default before the two fill
     * packets. These offsets are the port's subset, not the full original ring. */
    CHECK_EQ_U32(load(ring + 0x158u), 0x000402A8u);
    CHECK_EQ_U32(load(ring + 0x15Cu), 0u);
    CHECK_EQ_U32(load(ring + 0x160u), 0x0008038Cu);
    CHECK_EQ_U32(load(ring + 0x164u), 0x1B02u);
    CHECK_EQ_U32(load(ring + 0x168u), 0x1B02u);
    CHECK_EQ_U32(load(ring + 0x16Cu), 0x0008038Cu);
    CHECK_EQ_U32(load(ring + 0x170u), 0x1B02u);
    CHECK_EQ_U32(load(ring + 0x174u), 0x1B02u);
    CHECK_EQ_U32(load(DEVICE + 0x28u), ring + 0x100000u);
    CHECK_EQ_U32(load(DEVICE + 0x04u), ring + 0x10000u - 0x204u);
    environment_end();
}

static void test_create_device_defaults_the_sizes_and_keeps_dirty_bits(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    /* SetPushBufferSize was never called: both globals are zero and CreateDevice defaults them to
     * 0x80000 and 0x8000 (0x003D9230), and the ring follows. The game's own bits in the dirty
     * mask survive the OR. MUTATION: other defaults, or a plain store of 0xFF7F7F, fail. */
    store(0x003E3AB8u, 0x01000000u);
    uint32_t hr = 0xFFFFFFFFu;
    uint32_t out = 0u;
    run_create_device(0x140u, 0u, &hr, &out);
    CHECK_EQ_U32(hr, 0u);
    CHECK_EQ_U32(load(0x003E6404u), 0x80000u);
    CHECK_EQ_U32(load(0x003E6400u), 0x8000u);
    const uint32_t ring = load(DEVICE + 0x24u);
    CHECK_EQ_U32(load(DEVICE + 0x28u), ring + 0x80000u);
    CHECK_EQ_U32(load(DEVICE + 0x04u), ring + 0x8000u - 0x204u);
    CHECK_EQ_U32(load(0x003E3AB8u), 0x01FF7F7Fu);
    environment_end();
}

static void test_filters_send_only_on_change(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    uint32_t hr = 0xFFFFFFFFu;
    uint32_t out = 0u;
    run_create_device(0x140u, 0u, &hr, &out);
    capture_clear();

    /* CreateDevice left the flicker filter at 5 and the soft filter at 0 (as a boolean). The same
     * value again sends nothing. MUTATION: an unconditional send logs a line. */
    d3d8_set_flicker_filter(5u);
    d3d8_set_soft_display_filter(0u);
    CHECK(!captured_has("param"));

    /* A new flicker value is sent and cached. */
    d3d8_set_flicker_filter(3u);
    CHECK(captured_has("option 0xB, param 3"));
    CHECK_EQ_U32(load(0x003E2B98u), 3u);

    /* The soft filter caches a BOOLEAN: any non-zero value is 1. Sending 2 is a change from 0, and
     * sending 3 afterwards is not a change from 1, so it is not sent. MUTATION: caching the raw
     * value makes the 3 a change and logs it. */
    d3d8_set_soft_display_filter(2u);
    CHECK(captured_has("option 0xE, param 2"));
    CHECK_EQ_U32(load(0x003E2B94u), 1u);
    capture_clear();
    d3d8_set_soft_display_filter(3u);
    CHECK(!captured_has("param"));
    environment_end();
}

static void test_create_device_behavior_flag(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    uint32_t hr = 0xFFFFFFFFu;
    uint32_t out = 0u;
    /* Behaviour flag 0x10 (pure device) is copied into the device flags and nothing else of the
     * behaviour word is. MUTATION: a mask of 0x1F carries 0x0F too. */
    run_create_device(0x140u, 0x1Fu, &hr, &out);
    CHECK_EQ_U32(hr, 0u);
    CHECK_EQ_U32(load(DEVICE + 8u), 0x4013u);
    environment_end();
}

static void test_create_device_failure_clears_the_device(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    uint32_t hr = 0u;
    uint32_t out = 0u;
    /* Widescreen requested of a non-widescreen capability word: no display mode serves it, the
     * original returns E_FAIL, clears the out-pointer, zeroes 0x2490 bytes of the device and
     * clears the device slot. MUTATION: returning success, or leaving any of them, fails. */
    run_create_device(0x110u, 0u, &hr, &out);
    CHECK_EQ_U32(hr, 0x80004005u);
    CHECK_EQ_U32(load(out), 0u);
    CHECK_EQ_U32(load(D3D8_DEVICE_POINTER_SLOT), 0u);
    /* The polygon-mode defaults are applied only after a supported mode succeeds; failed
     * CreateDevice must not seed state that the retail path never reached. */
    CHECK_EQ_U32(load(0x003E3EECu), 0u);
    CHECK_EQ_U32(load(0x003E3EF0u), 0u);
    for (uint32_t offset = 0u; offset < 0x24A0u; offset += 4u) {
        CHECK_EQ_U32(load(DEVICE + offset), 0u);
    }
    environment_end();

    /* The same request under the composite pack fails the other way round: that pack's rows do
     * not carry the progressive bit the title asks for with 0x140. */
    environment_begin(KERNEL_AV_PACK_COMPOSITE);
    (void)d3d8_device_register();
    run_create_device(0x140u, 0u, &hr, &out);
    CHECK_EQ_U32(hr, 0x80004005u);
    environment_end();
}

static void test_create_device_unsupported_requests_are_fatal(void)
{
    static const struct {
        unsigned index;
        uint32_t value;
    } cases[] = {{3u, 2u}, {4u, 0x12u}, {5u, 1u}, {13u, 0x1000u}};
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); index++) {
        environment_begin(KERNEL_AV_PACK_HDTV);
        (void)d3d8_device_register();
        const uint32_t parameters = SCRATCH_DATA;
        write_title_parameters(parameters, 0x140u);
        store(parameters + cases[index].index * 4u, cases[index].value);
        /* BackBufferCount 2, MultiSampleType 0x12, SwapEffect 1 and a caller-supplied surface:
         * each a path the original takes and this port does not. MUTATION: running them as the
         * title's configuration returns success. */
        RUN_EXPECTING_FATAL((void)d3d8_create_device(0u, parameters, 0u));
        CHECK(fatal_seen);
        CHECK(fatal_address == 0x003DA700u);
        environment_end();
    }
}

static void test_get_back_buffer(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    uint32_t hr = 0u;
    uint32_t out = 0u;
    run_create_device(0x140u, 0u, &hr, &out);
    CHECK_EQ_U32(hr, 0u);

    /* -1 is the front buffer, 0 the first back buffer; each gets a reference. MUTATION: the
     * indices swapped returns the other header. */
    uint32_t arg = 0xFFFFFFFFu;
    CHECK_EQ_U32(call_stdcall(0x003D3A80u, &arg, 1u), 0x003E599Cu);
    CHECK_EQ_U32(load(0x003E599Cu), 0x01050003u);
    arg = 0u;
    CHECK_EQ_U32(call_stdcall(0x003D3A80u, &arg, 1u), 0x003E5984u);
    CHECK_EQ_U32(load(0x003E5984u), 0x010D0003u);

    /* Any other index reads the third slot, which a single-buffer device leaves empty: fatal
     * rather than a null pointer for the title to copy. */
    arg = 1u;
    RUN_EXPECTING_FATAL((void)call_stdcall(0x003D3A80u, &arg, 1u));
    CHECK(fatal_seen);
    environment_end();
}

static void test_pushbuffer_primitive_through_the_dispatcher(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    uint32_t hr = 0u;
    uint32_t out = 0u;
    run_create_device(0x140u, 0u, &hr, &out);
    const uint32_t ring = load(DEVICE + 0x24u);

    /* A fastcall: header in ecx, value in edx. The result is the advanced cursor, as in eax at
     * the original's return. MUTATION: ecx and edx swapped writes the value first. */
    const uint32_t first = ring + 0x178u; /* mode, fence, target, fog and two FILL packets */
    CHECK_EQ_U32(call_fastcall(0x003D6C90u, 0x00040308u, 0u), first + 8u);
    CHECK_EQ_U32(load(first), 0x00040308u);
    CHECK_EQ_U32(load(first + 4u), 0u);
    CHECK_EQ_U32(call_fastcall(0x003D6C90u, 0x00040A60u, 0x11223344u), first + 16u);
    CHECK_EQ_U32(load(first + 8u), 0x00040A60u);
    CHECK_EQ_U32(load(first + 12u), 0x11223344u);
    environment_end();
}

static void test_wrong_arity_is_refused_not_guessed(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    /* A frame bounded to 3 argument slots for a function with 6: the handler cannot read its
     * fourth argument and must stop, not substitute zero. */
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    const uint32_t args[3] = {0u, 1u, 0u};
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, 3u));
    frame.stack_limit = call_scratch + 4u * 4u;
    RUN_EXPECTING_FATAL((void)d3d8_hle_call(0x003D9230u, &frame));
    CHECK(fatal_seen);
    CHECK(fatal_address == 0x003D9230u);
    environment_end();
}

int main(void)
{
    test_registration();
    test_create_and_push_buffer_size();
    test_create_device_success();
    test_create_device_defaults_the_sizes_and_keeps_dirty_bits();
    test_filters_send_only_on_change();
    test_create_device_behavior_flag();
    test_create_device_failure_clears_the_device();
    test_create_device_unsupported_requests_are_fatal();
    test_get_back_buffer();
    test_pushbuffer_primitive_through_the_dispatcher();
    test_wrong_arity_is_refused_not_guessed();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
