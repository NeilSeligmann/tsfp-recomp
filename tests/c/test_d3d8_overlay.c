/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic overlay checks (T393 part B). Expected values were computed independently of the port
 * from the disassembly of 0x003D9810 and 0x003D9990. tests/test_d3d8_overlay_oracle.py compares
 * the port with the original bytes dword for dword. */
#include "test_d3d8_support.h"
#include "d3d8_overlay.h"
#include "d3d8_overlay_image.h"
#include "d3d8_overlay_key.h"

#define SURFACE (SCRATCH_DATA)
#define SOURCE_RECT (SCRATCH_DATA + 0x40u)
#define DESTINATION_RECT (SCRATCH_DATA + 0x60u)
#define UNMAPPED_ADDRESS 0x50000000u
#define DEV_REGISTER_BASE 0x0934u
#define DEV_OVERLAY_SURFACE 0x1C1Cu
#define DEV_FIELD_COUNTER 0x1DE8u
#define DEV_OVERLAY_COUNTER 0x2410u

typedef struct {
    uint32_t offset;
    uint32_t value;
} expected_write;

static void begin(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_surface_entry rows[sizeof(suite_addresses) / sizeof(suite_addresses[0]) + 3u];
    size_t count = 0u;
    for (; count < sizeof(suite_addresses) / sizeof(suite_addresses[0]); count++) {
        rows[count] = (d3d8_surface_entry){suite_addresses[count], NULL, 1u};
    }
    rows[count++] = (d3d8_surface_entry){D3D8_OVERLAY_ENABLE, NULL, 1u};
    rows[count++] = (d3d8_surface_entry){D3D8_OVERLAY_UPDATE, NULL, 1u};
    rows[count++] = (d3d8_surface_entry){D3D8_OVERLAY_STATUS, NULL, 1u};
    CHECK(d3d8_hle_init(rows, count));
    (void)d3d8_device_register();
    CHECK_EQ_U32(d3d8_overlay_register(), 3u);
    write_title_parameters(SCRATCH_DATA, 0x140u);
    const uint32_t args[6] = {0u, 1u, 0u, 0u, SCRATCH_DATA, SCRATCH_DATA + 0x200u};
    CHECK_EQ_U32(call_stdcall(0x003D9230u, args, 6u), 0u);
    /* The 640x480 linear YUY2 header T250 measured: Format 0x00012429, Size 0x271DF27F. */
    store(SURFACE + 0x00u, 0x01050001u);
    store(SURFACE + 0x04u, 0x81234560u);
    store(SURFACE + 0x08u, 0u);
    store(SURFACE + 0x0Cu, 0x00012429u);
    store(SURFACE + 0x10u, 0x271DF27Fu);
    store(SURFACE + 0x14u, 0u);
    d3d8_device_store32(DEV_FIELD_COUNTER, 5u);
}

static void rectangle(uint32_t address, uint32_t left, uint32_t top, uint32_t right,
                      uint32_t bottom)
{
    store(address, left);
    store(address + 4u, top);
    store(address + 8u, right);
    store(address + 12u, bottom);
}

static uint32_t update(uint32_t enable, uint32_t key)
{
    const uint32_t args[5] = {SURFACE, SOURCE_RECT, DESTINATION_RECT, enable, key};
    return call_stdcall(D3D8_OVERLAY_UPDATE, args, 5u);
}

static void check_log(const expected_write *want, size_t count)
{
    d3d8_overlay_write got[D3D8_OVERLAY_WRITE_LOG_CAPACITY];
    const size_t held = d3d8_overlay_write_log(got, D3D8_OVERLAY_WRITE_LOG_CAPACITY);
    CHECK(count != 0u);
    CHECK_EQ_U32(held, count);
    for (size_t index = 0u; index < count && index < held; index++) {
        CHECK_EQ_U32(got[index].offset, want[index].offset);
        CHECK_EQ_U32(got[index].value, want[index].value);
    }
}

static void check_registers(const expected_write *want, size_t count)
{
    CHECK(count != 0u);
    for (size_t index = 0u; index < count; index++) {
        uint32_t value = 0xDEADBEEFu;
        CHECK(d3d8_overlay_register_value(want[index].offset, &value));
        CHECK_EQ_U32(value, want[index].value);
    }
}

static void test_enable_first_call(void)
{
    begin();
    const uint32_t arg[1] = {0u};
    CHECK_EQ_U32(call_stdcall(D3D8_OVERLAY_ENABLE, arg, 1u), 0xFD000000u);
    static const expected_write writes[] = {{0x8100u, 0x11u},       {0x8920u, 0u},
                                            {0x8928u, 0xFFFFFFFFu}, {0x8930u, 0u},
                                            {0x8900u, 0u},          {0x8910u, 0x1000u},
                                            {0x8918u, 0x1000u}};
    check_log(writes, sizeof(writes) / sizeof(writes[0]));
    check_registers(writes, sizeof(writes) / sizeof(writes[0]));
    CHECK_EQ_U32(d3d8_overlay_state().enables, 1u);
    CHECK_EQ_U32(d3d8_overlay_state().write_count, 7u);
    environment_end();
}

static void test_update_unscaled(void)
{
    begin();
    CHECK(d3d8_overlay_hardware_write(0x8140u, 0x10u));
    rectangle(SOURCE_RECT, 0u, 0u, 640u, 480u);
    rectangle(DESTINATION_RECT, 0u, 0u, 640u, 480u);
    CHECK_EQ_U32(update(0u, 0u), 0x11u);
    static const expected_write writes[] = {
        {0x8704u, 0u},          {0x8B00u, 0u},          {0x8920u, 0x1234540u},
        {0x8930u, 0x100u},      {0x8928u, 0x1E00290u},  {0x8938u, 0x100000u},
        {0x8940u, 0x100000u},   {0x8948u, 0u},          {0x8950u, 0x1E00280u},
        {0x8958u, 0x10A00u},    {0x8908u, 0x7FFFFFFu},  {0x8140u, 0x11u},
        {0x8700u, 1u}};
    check_log(writes, sizeof(writes) / sizeof(writes[0]));
    check_registers(writes, sizeof(writes) / sizeof(writes[0]));
    CHECK_EQ_U32(d3d8_device_load32(DEV_OVERLAY_SURFACE), SURFACE);
    CHECK_EQ_U32(d3d8_device_load32(DEV_OVERLAY_COUNTER), 5u);
    const d3d8_overlay_descriptor state = d3d8_overlay_state();
    CHECK_EQ_U32(state.updates, 1u);
    CHECK_EQ_U32(state.surface, SURFACE);
    CHECK_EQ_U32(state.surface_pitch, 2560u);
    CHECK_EQ_U32(state.surface_data, 0x81234560u);
    CHECK_EQ_U32(state.source[2], 640u);
    CHECK_EQ_U32(state.destination[3], 480u);
    CHECK_EQ_U32(state.source_size_phase, 0x1E00290u);
    CHECK_EQ_U32(state.control, 0x10A00u);
    CHECK_EQ_U32(state.result, 0x11u);
    environment_end();
}

static void test_update_scaled_with_key(void)
{
    begin();
    CHECK(d3d8_overlay_hardware_write(0x8140u, 0x10u));
    rectangle(SOURCE_RECT, 0u, 0u, 320u, 240u);
    rectangle(DESTINATION_RECT, 100u, 50u, 740u, 530u);
    CHECK_EQ_U32(update(1u, 0xAABBCCu), 0x11u);
    static const expected_write writes[] = {
        {0x8704u, 0u},          {0x8B00u, 0xAABBCCu},    {0x8920u, 0x1234540u},
        {0x8930u, 0x100u},      {0x8928u, 0xF00150u},    {0x8938u, 0x7FCCBu},
        {0x8940u, 0x7FBB9u},    {0x8948u, 0x320064u},    {0x8950u, 0x1E00280u},
        {0x8958u, 0x110A00u},   {0x8908u, 0x7FFFFFFu},   {0x8140u, 0x11u},
        {0x8700u, 1u}};
    check_log(writes, sizeof(writes) / sizeof(writes[0]));
    check_registers(writes, sizeof(writes) / sizeof(writes[0]));
    environment_end();
}

static void test_update_key_enable_is_any_nonzero_value(void)
{
    begin();
    rectangle(SOURCE_RECT, 0u, 0u, 640u, 480u);
    rectangle(DESTINATION_RECT, 0u, 0u, 640u, 480u);
    (void)update(2u, 0u);
    static const expected_write control[] = {{0x8958u, 0x110A00u}};
    check_registers(control, 1u);
    (void)update(0u, 0u);
    static const expected_write cleared[] = {{0x8958u, 0x10A00u}};
    check_registers(cleared, 1u);
    environment_end();
}

static void test_update_odd_origin_and_one_wide_destination(void)
{
    begin();
    rectangle(SOURCE_RECT, 3u, 5u, 323u, 245u);
    rectangle(DESTINATION_RECT, 10u, 20u, 11u, 21u);
    CHECK_EQ_U32(update(0u, 0x11u), 1u);
    static const expected_write writes[] = {
        {0x8704u, 0u},          {0x8B00u, 0x11u},       {0x8920u, 0x1236D40u},
        {0x8930u, 0x120u},      {0x8928u, 0xF00152u},   {0x8938u, 0x100000u},
        {0x8940u, 0x100000u},   {0x8948u, 0x14000Au},   {0x8950u, 0x10001u},
        {0x8958u, 0x10A00u},    {0x8908u, 0x7FFFFFFu},  {0x8140u, 1u},
        {0x8700u, 1u}};
    check_log(writes, sizeof(writes) / sizeof(writes[0]));
    check_registers(writes, sizeof(writes) / sizeof(writes[0]));
    environment_end();
}

static void test_update_two_wide_destination_divides_by_one(void)
{
    begin();
    rectangle(SOURCE_RECT, 3u, 5u, 323u, 245u);
    rectangle(DESTINATION_RECT, 10u, 20u, 12u, 22u);
    CHECK_EQ_U32(update(1u, 0x11u), 1u);
    static const expected_write writes[] = {{0x8938u, 0x13F00000u},
                                            {0x8940u, 0xEF00000u},
                                            {0x8950u, 0x20002u},
                                            {0x8958u, 0x110A00u}};
    check_registers(writes, sizeof(writes) / sizeof(writes[0]));
    environment_end();
}

static void test_status_after_update(void)
{
    begin();
    rectangle(SOURCE_RECT, 0u, 0u, 640u, 480u);
    rectangle(DESTINATION_RECT, 0u, 0u, 640u, 480u);
    CHECK_EQ_U32(call_stdcall(D3D8_OVERLAY_STATUS, NULL, 0u), 1u);
    (void)update(0u, 0u);
    CHECK_EQ_U32(call_stdcall(D3D8_OVERLAY_STATUS, NULL, 0u), 0u);
    d3d8_device_store32(DEV_FIELD_COUNTER, 6u);
    CHECK_EQ_U32(call_stdcall(D3D8_OVERLAY_STATUS, NULL, 0u), 1u);
    d3d8_device_store32(DEV_FIELD_COUNTER, 5u);
    CHECK_EQ_U32(call_stdcall(D3D8_OVERLAY_STATUS, NULL, 0u), 0u);
    environment_end();
}

static void test_enable_refuses_a_started_buffer(void)
{
    begin();
    rectangle(SOURCE_RECT, 0u, 0u, 640u, 480u);
    rectangle(DESTINATION_RECT, 0u, 0u, 640u, 480u);
    (void)update(0u, 0u);
    const uint64_t writes = d3d8_overlay_state().write_count;
    const uint32_t arg[1] = {0u};
    RUN_EXPECTING_FATAL((void)call_stdcall(D3D8_OVERLAY_ENABLE, arg, 1u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, D3D8_OVERLAY_ENABLE);
    CHECK(strstr(fatal_text, "0x8700") != NULL);
    CHECK(strstr(fatal_text, "completed modeled vblank") != NULL);
    CHECK_EQ_U32(d3d8_overlay_state().write_count, writes);
    CHECK_EQ_U32(d3d8_overlay_state().enables, 0u);
    uint32_t value = 0u;
    CHECK(d3d8_overlay_register_value(0x8704u, &value));
    CHECK_EQ_U32(value, 0u);
    CHECK(d3d8_overlay_register_value(0x8100u, &value));
    CHECK_EQ_U32(value, 0u);
    /* Once the hardware side retires the buffer the same call runs. */
    CHECK(d3d8_overlay_hardware_write(0x8700u, 0u));
    CHECK_EQ_U32(call_stdcall(D3D8_OVERLAY_ENABLE, arg, 1u), 0xFD000000u);
    CHECK_EQ_U32(d3d8_overlay_state().write_count, writes + 7u);
    environment_end();
}

/* T540: the default-off policy retires a started buffer only at a completed modeled vblank.
 * EnableOverlay does not fabricate elapsed time when the next blank has not run. */
static void test_consume_at_vblank(void)
{
    begin();
    rectangle(SOURCE_RECT, 0u, 0u, 640u, 480u);
    rectangle(DESTINATION_RECT, 0u, 0u, 640u, 480u);
    const uint32_t arg[1] = {0u};
    d3d8_overlay_set_consume_policy(true);
    CHECK_EQ_U32(call_stdcall(D3D8_OVERLAY_ENABLE, arg, 1u), 0xFD000000u);
    CHECK_EQ_U32(d3d8_overlay_consumed_count(), 0u);
    d3d8_overlay_consume_vblank();
    CHECK_EQ_U32(d3d8_overlay_consumed_count(), 0u);
    (void)update(0u, 0u);
    uint32_t value = 0u;
    CHECK(d3d8_overlay_register_value(0x8700u, &value));
    CHECK_EQ_U32(value, 1u);
    const uint64_t writes = d3d8_overlay_state().write_count;
    RUN_EXPECTING_FATAL((void)call_stdcall(D3D8_OVERLAY_ENABLE, arg, 1u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(d3d8_overlay_state().write_count, writes);
    CHECK_EQ_U32(d3d8_overlay_state().enables, 1u);
    CHECK_EQ_U32(d3d8_overlay_consumed_count(), 0u);
    d3d8_overlay_consume_vblank();
    CHECK_EQ_U32(d3d8_overlay_consumed_count(), 1u);
    d3d8_overlay_consume_vblank();
    CHECK_EQ_U32(d3d8_overlay_consumed_count(), 1u);
    CHECK(d3d8_overlay_register_value(0x8700u, &value));
    CHECK_EQ_U32(value, 0u);
    CHECK_EQ_U32(call_stdcall(D3D8_OVERLAY_ENABLE, arg, 1u), 0xFD000000u);
    CHECK_EQ_U32(d3d8_overlay_state().write_count, writes + 7u);
    CHECK_EQ_U32(d3d8_overlay_state().enables, 2u);
    d3d8_overlay_reset();
    CHECK_EQ_U32(d3d8_overlay_consumed_count(), 0u);
    (void)update(0u, 0u);
    d3d8_overlay_consume_vblank();
    CHECK_EQ_U32(call_stdcall(D3D8_OVERLAY_ENABLE, arg, 1u), 0xFD000000u);
    CHECK_EQ_U32(d3d8_overlay_consumed_count(), 1u);
    d3d8_overlay_set_consume_policy(false);
    (void)update(0u, 0u);
    /* T665: a completed vblank with the policy off retires nothing, the buffer stays pending. */
    d3d8_overlay_consume_vblank();
    CHECK_EQ_U32(d3d8_overlay_consumed_count(), 1u);
    CHECK(d3d8_overlay_register_value(0x8700u, &value));
    CHECK_EQ_U32(value, 1u);
    const uint64_t before = d3d8_overlay_state().write_count;
    RUN_EXPECTING_FATAL((void)call_stdcall(D3D8_OVERLAY_ENABLE, arg, 1u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(d3d8_overlay_state().write_count, before);
    CHECK_EQ_U32(d3d8_overlay_consumed_count(), 1u);
    environment_end();
}

static void refuse_update(uint32_t surface, uint32_t source, uint32_t destination,
                          const char *needle)
{
    const uint64_t writes = d3d8_overlay_state().write_count;
    const uint32_t surface_before = d3d8_device_load32(DEV_OVERLAY_SURFACE);
    const uint32_t counter_before = d3d8_device_load32(DEV_OVERLAY_COUNTER);
    const uint32_t args[5] = {surface, source, destination, 0u, 0u};
    RUN_EXPECTING_FATAL((void)call_stdcall(D3D8_OVERLAY_UPDATE, args, 5u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, D3D8_OVERLAY_UPDATE);
    CHECK(strstr(fatal_text, needle) != NULL);
    CHECK_EQ_U32(d3d8_overlay_state().write_count, writes);
    CHECK_EQ_U32(d3d8_device_load32(DEV_OVERLAY_SURFACE), surface_before);
    CHECK_EQ_U32(d3d8_device_load32(DEV_OVERLAY_COUNTER), counter_before);
}

static void test_update_refusals(void)
{
    begin();
    rectangle(SOURCE_RECT, 0u, 0u, 640u, 480u);
    rectangle(DESTINATION_RECT, 0u, 0u, 640u, 480u);
    refuse_update(SURFACE, 0u, DESTINATION_RECT, "null source rectangle");
    refuse_update(SURFACE, SOURCE_RECT, 0u, "null destination rectangle");
    refuse_update(SURFACE, UNMAPPED_ADDRESS, DESTINATION_RECT, "source rectangle at 0x50000000");
    refuse_update(SURFACE, SOURCE_RECT, UNMAPPED_ADDRESS, "destination rectangle at 0x50000000");
    refuse_update(0u, SOURCE_RECT, DESTINATION_RECT, "null surface header");
    refuse_update(UNMAPPED_ADDRESS, SOURCE_RECT, DESTINATION_RECT, "surface header at 0x50000000");
    /* A window other than the literal CreateDevice stores is refused by name. */
    d3d8_device_store32(DEV_REGISTER_BASE, 0xFE000000u);
    refuse_update(SURFACE, SOURCE_RECT, DESTINATION_RECT, "0xFD000000");
    const uint32_t arg[1] = {0u};
    RUN_EXPECTING_FATAL((void)call_stdcall(D3D8_OVERLAY_ENABLE, arg, 1u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "0xFD000000") != NULL);
    CHECK_EQ_U32(d3d8_overlay_state().write_count, 0u);
    d3d8_device_store32(DEV_REGISTER_BASE, 0xFD000000u);
    environment_end();
}

/* --- T537: the opt-in picture dump ------------------------------------------------------- */

#define PICTURE_PITCH 2560u
#define PICTURE_DATA 0x81234560u
static uint32_t picture_base;

static uint32_t resolve_picture(uint32_t data)
{
    return data == PICTURE_DATA ? picture_base : 0u;
}

static uint32_t resolve_nothing(uint32_t data)
{
    (void)data;
    return 0u;
}

static uint32_t resolve_unmapped(uint32_t data)
{
    (void)data;
    return UNMAPPED_ADDRESS;
}

/* A readable guest picture, 8 rows of the title's 640x480 pitch, each pixel a distinct YUY2 pair. */
static void map_picture(void)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 0x6000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    picture_base = guest_region_alloc(&request, &status);
    CHECK(picture_base != 0u);
    for (uint32_t row = 0u; row < 8u; row++) {
        for (uint32_t byte = 0u; byte < 40u; byte++) {
            store_byte(picture_base + row * PICTURE_PITCH + byte, (uint8_t)(16u + row * 29u + byte * 7u));
        }
    }
}

static uint8_t *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    *length = (size_t)ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = malloc(*length + 1u);
    CHECK(bytes != NULL && fread(bytes, 1u, *length, file) == *length);
    fclose(file);
    return bytes;
}

static bool files_equal(const char *left, const char *right)
{
    size_t left_length = 0u, right_length = 0u;
    uint8_t *left_bytes = read_file(left, &left_length);
    uint8_t *right_bytes = read_file(right, &right_length);
    const bool equal = left_bytes != NULL && right_bytes != NULL && left_length == right_length &&
                       memcmp(left_bytes, right_bytes, left_length) == 0;
    free(left_bytes);
    free(right_bytes);
    return equal;
}

static bool file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        fclose(file);
    }
    return file != NULL;
}

static void remove_dump_files(void)
{
    remove("./overlay_00000.png");
    remove("./overlay_00000.yuv422p");
    remove("./overlay_00001.png");
    remove("./overlay_00001.yuv422p");
    remove("./overlay_00002.png");
    remove("./overlay_00002.yuv422p");
    remove("./overlay_index.txt");
    remove("./expected_00000.png");
}

/* Everything the title or a later model can see of the overlay, to compare two runs. */
typedef struct {
    d3d8_overlay_descriptor state;
    uint32_t registers[(D3D8_OVERLAY_LAST_REGISTER - D3D8_OVERLAY_FIRST_REGISTER) / 4u + 1u];
    d3d8_overlay_write log[D3D8_OVERLAY_WRITE_LOG_CAPACITY];
    size_t logged;
    uint32_t device_surface, device_counter;
} observable;

static void observe(observable *out)
{
    memset(out, 0, sizeof(*out));
    out->state = d3d8_overlay_state();
    for (uint32_t offset = D3D8_OVERLAY_FIRST_REGISTER; offset <= D3D8_OVERLAY_LAST_REGISTER; offset += 4u) {
        CHECK(d3d8_overlay_register_value(offset, &out->registers[(offset - D3D8_OVERLAY_FIRST_REGISTER) / 4u]));
    }
    out->logged = d3d8_overlay_write_log(out->log, D3D8_OVERLAY_WRITE_LOG_CAPACITY);
    out->device_surface = d3d8_device_load32(DEV_OVERLAY_SURFACE);
    out->device_counter = d3d8_device_load32(DEV_OVERLAY_COUNTER);
}

static bool same_observable(const observable *left, const observable *right)
{
    return memcmp(&left->state, &right->state, sizeof(left->state)) == 0 &&
           memcmp(left->registers, right->registers, sizeof(left->registers)) == 0 &&
           left->logged == right->logged &&
           memcmp(left->log, right->log, left->logged * sizeof(left->log[0])) == 0 &&
           left->device_surface == right->device_surface && left->device_counter == right->device_counter;
}

static void test_dump_writes_the_picture_the_update_asked_for(void)
{
    remove_dump_files();
    begin();
    map_picture();
    /* Odd width 5 from an odd left and top: the driver rounds both down to even (2, 2). Destination 10 x 4. */
    rectangle(SOURCE_RECT, 3u, 3u, 8u, 7u);
    rectangle(DESTINATION_RECT, 20u, 30u, 30u, 34u);
    CHECK(d3d8_overlay_set_dump(".", 0u, resolve_picture));
    CHECK_EQ_U32(update(0u, 0u), 1u);
    CHECK_EQ_U32(d3d8_overlay_dump_written(), 1u);
    CHECK_EQ_U32(d3d8_overlay_dump_failed(), 0u);
    const d3d8_overlay_descriptor state = d3d8_overlay_state();
    CHECK_EQ_U32(state.horizontal_step, ((5u - 1u) << 20) / 9u);
    CHECK_EQ_U32(state.vertical_step, ((4u - 1u) << 20) / 3u);

    /* The expected picture, built here from the guest rows the update names: left 2, top 2, width 5
     * (pairs 1 to 3, the last pair's second half is read too), height 4. */
    uint8_t rows[4][12];
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t byte = 0u; byte < 12u; byte++) {
            rows[row][byte] = (uint8_t)(16u + (row + 2u) * 29u + (4u + byte) * 7u);
        }
    }
    uint8_t source_rgb[5 * 4 * 3];
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t column = 0u; column < 5u; column++) {
            d3d8_overlay_image_yuv_to_rgb(rows[row][2u * column], rows[row][4u * (column / 2u) + 1u],
                                          rows[row][4u * (column / 2u) + 3u],
                                          source_rgb + (row * 5u + column) * 3u);
        }
    }
    uint8_t scaled[10 * 4 * 3];
    CHECK(d3d8_overlay_image_scale(source_rgb, 5u, 4u, state.horizontal_step, state.vertical_step, scaled, 10u, 4u));
    CHECK(d3d8_overlay_image_write_png("./expected_00000.png", scaled, 10u, 4u));
    CHECK(files_equal("./overlay_00000.png", "./expected_00000.png"));

    /* The raw planes: Y 5 x 4, then U and V 3 x 4. */
    size_t length = 0u;
    uint8_t *planes = read_file("./overlay_00000.yuv422p", &length);
    CHECK(planes != NULL && length == 5u * 4u + 2u * 3u * 4u);
    if (planes != NULL && length == 5u * 4u + 2u * 3u * 4u) {
        for (uint32_t row = 0u; row < 4u; row++) {
            for (uint32_t column = 0u; column < 5u; column++) {
                CHECK(planes[row * 5u + column] == rows[row][2u * column]);
            }
            for (uint32_t pair = 0u; pair < 3u; pair++) {
                CHECK(planes[20u + row * 3u + pair] == rows[row][4u * pair + 1u]);
                CHECK(planes[32u + row * 3u + pair] == rows[row][4u * pair + 3u]);
            }
        }
    }
    free(planes);

    /* One index line, naming the rectangles, the steps and the matrix status. */
    uint8_t *index = read_file("./overlay_index.txt", &length);
    CHECK(index != NULL);
    if (index != NULL) {
        index[length] = 0;
        CHECK(strstr((char *)index, "update 0 ") != NULL);
        CHECK(strstr((char *)index, "source 3,3,8,7 destination 20,30,30,34") != NULL);
        CHECK(strstr((char *)index, "pitch 2560") != NULL);
        CHECK(strstr((char *)index, "control 0x10a00") != NULL); /* bit 24, the BT.709 select, is clear */
        CHECK(strstr((char *)index, "UNMEASURED") != NULL);
    }
    free(index);
    d3d8_overlay_set_dump(NULL, 0u, NULL);
    remove_dump_files();
    environment_end();
}

static void test_dump_is_a_pure_observer(void)
{
    remove_dump_files();
    begin();
    map_picture();
    rectangle(SOURCE_RECT, 0u, 0u, 16u, 8u);
    rectangle(DESTINATION_RECT, 0u, 0u, 32u, 16u);
    CHECK(d3d8_overlay_set_dump(".", 0u, resolve_picture));
    (void)update(1u, 0x123456u);
    observable with_dump;
    observe(&with_dump);
    CHECK_EQ_U32(d3d8_overlay_dump_written(), 1u);
    /* The same call with the dump off leaves the same state, registers, log and device words. */
    d3d8_overlay_set_dump(NULL, 0u, NULL);
    d3d8_overlay_reset();
    d3d8_device_store32(DEV_OVERLAY_SURFACE, 0u);
    d3d8_device_store32(DEV_OVERLAY_COUNTER, 0u);
    (void)update(1u, 0x123456u);
    observable without_dump;
    observe(&without_dump);
    CHECK(same_observable(&with_dump, &without_dump));
    CHECK_EQ_U32(d3d8_overlay_dump_written(), 0u);
    remove_dump_files();
    environment_end();
}

static void test_dump_failures_are_counted_and_never_change_the_update(void)
{
    remove_dump_files();
    begin();
    map_picture();
    rectangle(SOURCE_RECT, 0u, 0u, 16u, 8u);
    rectangle(DESTINATION_RECT, 0u, 0u, 16u, 8u);
    observable reference;
    (void)update(0u, 0u);
    observe(&reference);
    struct {
        const char *what;
        d3d8_overlay_data_resolver resolver;
        uint32_t right;
    } cases[] = {
        {"unresolved Data word", resolve_nothing, 16u},
        {"unreadable allocation", resolve_unmapped, 16u},
        {"rectangle larger than the buffers", resolve_picture, 3000u},
        {"source row wider than the surface pitch", resolve_picture, 1500u},
    };
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); index++) {
        d3d8_overlay_reset();
        d3d8_device_store32(DEV_OVERLAY_SURFACE, 0u);
        d3d8_device_store32(DEV_OVERLAY_COUNTER, 0u);
        rectangle(SOURCE_RECT, 0u, 0u, cases[index].right, 8u);
        CHECK(d3d8_overlay_set_dump(".", 0u, cases[index].resolver));
        CHECK_EQ_U32(update(0u, 0u), 1u);
        CHECK_EQ_U32(d3d8_overlay_dump_written(), 0u);
        CHECK_EQ_U32(d3d8_overlay_dump_failed(), 1u);
        CHECK(!file_exists("./overlay_00000.png"));
        CHECK(!file_exists("./overlay_00000.yuv422p"));
        CHECK_EQ_U32(d3d8_overlay_state().updates, 1u);
        CHECK_EQ_U32(d3d8_overlay_state().write_count, reference.state.write_count);
    }
    /* A directory that does not exist is a counted failure too, not a silent skip. */
    d3d8_overlay_reset();
    rectangle(SOURCE_RECT, 0u, 0u, 16u, 8u);
    CHECK(d3d8_overlay_set_dump("./no-such-directory-t537", 0u, resolve_picture));
    (void)update(0u, 0u);
    CHECK_EQ_U32(d3d8_overlay_dump_failed(), 1u);
    CHECK_EQ_U32(d3d8_overlay_dump_written(), 0u);
    d3d8_overlay_set_dump(NULL, 0u, NULL);
    environment_end();
}

static void test_dump_maximum_and_reset(void)
{
    remove_dump_files();
    begin();
    map_picture();
    rectangle(SOURCE_RECT, 0u, 0u, 16u, 8u);
    rectangle(DESTINATION_RECT, 0u, 0u, 16u, 8u);
    CHECK(d3d8_overlay_set_dump(".", 1u, resolve_picture));
    (void)update(0u, 0u);
    (void)update(0u, 0u);
    CHECK_EQ_U32(d3d8_overlay_state().updates, 2u);
    CHECK_EQ_U32(d3d8_overlay_dump_written(), 1u);
    CHECK_EQ_U32(d3d8_overlay_dump_failed(), 0u);
    CHECK(file_exists("./overlay_00000.png"));
    CHECK(!file_exists("./overlay_00001.png")); /* past the limit, nothing written */
    /* The setting survives a reset, the counters do not, and the picture numbering restarts. */
    d3d8_overlay_reset();
    CHECK_EQ_U32(d3d8_overlay_dump_written(), 0u);
    (void)update(0u, 0u);
    CHECK_EQ_U32(d3d8_overlay_dump_written(), 1u);
    /* A limit of 0 is no limit, and a new setting restarts the counters (the update numbers go on: 1, 2). */
    CHECK(d3d8_overlay_set_dump(".", 0u, resolve_picture));
    (void)update(0u, 0u);
    (void)update(0u, 0u);
    CHECK_EQ_U32(d3d8_overlay_dump_written(), 2u);
    CHECK(file_exists("./overlay_00001.png") && file_exists("./overlay_00001.yuv422p"));
    /* The index of the new setting starts afresh and then appends: updates 1 and 2, one line each. */
    size_t index_length = 0u;
    uint8_t *index_text = read_file("./overlay_index.txt", &index_length);
    CHECK(index_text != NULL);
    if (index_text != NULL) {
        index_text[index_length] = 0;
        size_t lines = 0u;
        for (const char *at = (const char *)index_text; (at = strstr(at, "update ")) != NULL; at++) {
            lines++;
        }
        CHECK_EQ_U32(lines, 2u);
        CHECK(strstr((char *)index_text, "update 1 ") == (char *)index_text);
        CHECK(strstr((char *)index_text, "update 2 ") != NULL);
        CHECK(strstr((char *)index_text, "update 0 ") == NULL);
    }
    free(index_text);
    d3d8_overlay_set_dump(NULL, 0u, NULL);
    remove_dump_files();
    environment_end();
}

/* T760: the present hook. Records every call so the test can assert the sequence, never the aggregate. */
typedef struct {
    uint32_t pictures;
    uint32_t vblanks;
    uint32_t without_picture;
    uint64_t numbers[8];
    uint32_t width, height;
    uint8_t rgb[16 * 8 * 3];
    bool rgb_copied;
    uint32_t order[16]; /* 1 picture, 2 vblank, in call order */
    uint32_t order_count;
} hook_log;

static void hook_picture(const d3d8_overlay_picture *picture, void *context)
{
    hook_log *log = context;
    if (log->pictures < 8u) {
        log->numbers[log->pictures] = picture->number;
    }
    if (picture->rgb == NULL) {
        log->without_picture++;
        CHECK(picture->width == 0u && picture->height == 0u);
    } else if (picture->width == 16u && picture->height == 8u && !log->rgb_copied) {
        memcpy(log->rgb, picture->rgb, sizeof(log->rgb));
        log->width = picture->width;
        log->height = picture->height;
        log->rgb_copied = true;
    }
    if (log->order_count < 16u) {
        log->order[log->order_count++] = 1u;
    }
    log->pictures++;
}

static void hook_vblank(void *context)
{
    hook_log *log = context;
    if (log->order_count < 16u) {
        log->order[log->order_count++] = 2u;
    }
    log->vblanks++;
}

static void test_present_hook_once_per_update_and_per_vblank(void)
{
    remove_dump_files();
    begin();
    map_picture();
    rectangle(SOURCE_RECT, 0u, 0u, 16u, 8u);
    rectangle(DESTINATION_RECT, 0u, 0u, 16u, 8u);
    observable reference;
    (void)update(0u, 0u);
    observe(&reference);
    d3d8_overlay_reset();
    d3d8_device_store32(DEV_OVERLAY_SURFACE, 0u);
    d3d8_device_store32(DEV_OVERLAY_COUNTER, 0u);

    /* A pure observer: one update with the hook leaves the state, registers, log and device words of one without. */
    hook_log scratch;
    memset(&scratch, 0, sizeof(scratch));
    CHECK(d3d8_overlay_set_present_hook(hook_picture, hook_vblank, &scratch, resolve_picture));
    (void)update(0u, 0u);
    observable with_hook;
    observe(&with_hook);
    CHECK_EQ_U32(scratch.pictures, 1u);
    CHECK(same_observable(&reference, &with_hook));
    d3d8_overlay_set_present_hook(NULL, NULL, NULL, NULL);
    d3d8_overlay_reset();
    d3d8_device_store32(DEV_OVERLAY_SURFACE, 0u);
    d3d8_device_store32(DEV_OVERLAY_COUNTER, 0u);

    hook_log log;
    memset(&log, 0, sizeof(log));
    /* No dump directory: the hook alone allocates the buffers. */
    CHECK(d3d8_overlay_set_present_hook(hook_picture, hook_vblank, &log, resolve_picture));
    CHECK_EQ_U32(log.pictures, 0u); /* nothing before the first update */
    (void)update(0u, 0u);
    d3d8_overlay_consume_vblank();
    (void)update(0u, 0u);
    (void)update(0u, 0u);
    d3d8_overlay_consume_vblank();
    CHECK_EQ_U32(log.pictures, 3u);
    CHECK_EQ_U32(log.vblanks, 2u);
    CHECK_EQ_U32(log.without_picture, 0u);
    CHECK_EQ_U32(d3d8_overlay_state().updates, 3u);
    CHECK(log.numbers[0] == 0u && log.numbers[1] == 1u && log.numbers[2] == 2u);
    const uint32_t want_order[5] = {1u, 2u, 1u, 1u, 2u};
    CHECK_EQ_U32(log.order_count, 5u);
    for (uint32_t index = 0u; index < 5u && index < log.order_count; index++) {
        CHECK_EQ_U32(log.order[index], want_order[index]);
    }
    /* The picture is the one the dump would write: guest rows, BT.601, identity scale. */
    uint8_t rows[8 * 32];
    for (uint32_t row = 0u; row < 8u; row++) {
        for (uint32_t byte = 0u; byte < 32u; byte++) {
            rows[row * 32u + byte] = (uint8_t)(16u + row * 29u + byte * 7u);
        }
    }
    uint8_t source_rgb[16 * 8 * 3];
    uint8_t expected[16 * 8 * 3];
    CHECK(d3d8_overlay_image_yuy2_to_rgb(rows, 32u, 16u, 8u, source_rgb));
    const d3d8_overlay_descriptor state = d3d8_overlay_state();
    CHECK(d3d8_overlay_image_scale(source_rgb, 16u, 8u, state.horizontal_step, state.vertical_step, expected,
                                   16u, 8u));
    CHECK(log.rgb_copied && log.width == 16u && log.height == 8u);
    CHECK(memcmp(log.rgb, expected, sizeof(expected)) == 0);
    CHECK(log.rgb[0] != 0u || log.rgb[1] != 0u || log.rgb[2] != 0u); /* not an empty buffer */
    d3d8_overlay_set_present_hook(NULL, NULL, NULL, NULL);
    /* Removed: no more calls. */
    CHECK_EQ_U32(log.pictures, 3u);
    d3d8_overlay_consume_vblank();
    CHECK_EQ_U32(log.vblanks, 2u);
    environment_end();
}

static void test_present_hook_reports_unbuildable_and_limited_pictures(void)
{
    remove_dump_files();
    begin();
    map_picture();
    rectangle(SOURCE_RECT, 0u, 0u, 16u, 8u);
    rectangle(DESTINATION_RECT, 0u, 0u, 16u, 8u);
    hook_log log;
    memset(&log, 0, sizeof(log));
    /* A resolver that finds nothing: every update still reaches the hook, with no picture. */
    CHECK(d3d8_overlay_set_present_hook(hook_picture, NULL, &log, resolve_nothing));
    (void)update(0u, 0u);
    (void)update(0u, 0u);
    CHECK_EQ_U32(log.pictures, 2u);
    CHECK_EQ_U32(log.without_picture, 2u);
    CHECK_EQ_U32(d3d8_overlay_dump_failed(), 2u);
    /* With a dump limit of 1 the dump stops writing, the hook does not stop. */
    memset(&log, 0, sizeof(log));
    d3d8_overlay_reset();
    CHECK(d3d8_overlay_set_dump(".", 1u, resolve_picture));
    CHECK(d3d8_overlay_set_present_hook(hook_picture, NULL, &log, resolve_picture));
    (void)update(0u, 0u);
    (void)update(0u, 0u);
    (void)update(0u, 0u);
    CHECK_EQ_U32(d3d8_overlay_dump_written(), 1u);
    CHECK_EQ_U32(log.pictures, 3u);
    CHECK_EQ_U32(log.without_picture, 0u);
    CHECK(file_exists("./overlay_00000.png") && !file_exists("./overlay_00001.png"));
    /* Switching the dump off keeps the hook working (it owns the buffers it needs). */
    CHECK(d3d8_overlay_set_dump(NULL, 0u, NULL));
    (void)update(0u, 0u);
    CHECK_EQ_U32(log.pictures, 4u);
    CHECK_EQ_U32(log.without_picture, 0u);
    d3d8_overlay_set_present_hook(NULL, NULL, NULL, NULL);
    remove_dump_files();
    environment_end();
}

/* --- T831: the xemu-level picture and the key composition, both opt-in ---------------------------------- */

/* The picture of the update `rectangle(SOURCE_RECT, 3, 3, 8, 7)` names (left 2, top 2, width 5, height 4) built here
 * from the guest rows: the default (library matrix, nearest scale) or the xemu alternative (box one larger). */
static void expected_picture(bool xemu, uint32_t destination_width, uint32_t destination_height, uint8_t *out)
{
    uint8_t rows[4][12];
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t byte = 0u; byte < 12u; byte++) {
            rows[row][byte] = (uint8_t)(16u + (row + 2u) * 29u + (4u + byte) * 7u);
        }
    }
    uint8_t source_rgb[5 * 4 * 3];
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t column = 0u; column < 5u; column++) {
            uint8_t *pixel = source_rgb + (row * 5u + column) * 3u;
            const uint8_t luma = rows[row][2u * column];
            const uint8_t blue = rows[row][4u * (column / 2u) + 1u];
            const uint8_t red = rows[row][4u * (column / 2u) + 3u];
            if (xemu) {
                d3d8_overlay_image_yuv_to_rgb_xemu(luma, blue, red, pixel);
            } else {
                d3d8_overlay_image_yuv_to_rgb(luma, blue, red, pixel);
            }
        }
    }
    const d3d8_overlay_descriptor state = d3d8_overlay_state();
    if (xemu) {
        CHECK(d3d8_overlay_image_scale_xemu(source_rgb, 5u, 4u, out, destination_width, destination_height));
    } else {
        CHECK(d3d8_overlay_image_scale(source_rgb, 5u, 4u, state.horizontal_step, state.vertical_step, out,
                                       destination_width, destination_height));
    }
}

static void test_xemu_image_dump_and_hook(void)
{
    remove_dump_files();
    begin();
    map_picture();
    rectangle(SOURCE_RECT, 3u, 3u, 8u, 7u);
    rectangle(DESTINATION_RECT, 20u, 30u, 30u, 34u);
    CHECK(!d3d8_overlay_xemu_image()); /* default off */
    /* the same update, default and xemu: the title-visible state is identical (a pure observer either way) */
    CHECK(d3d8_overlay_set_dump(".", 0u, resolve_picture));
    (void)update(0u, 0u);
    observable library;
    observe(&library);
    CHECK(files_equal("./overlay_00000.png", "./overlay_00000.png"));
    size_t default_length = 0u;
    uint8_t *default_png = read_file("./overlay_00000.png", &default_length);
    d3d8_overlay_reset();
    d3d8_device_store32(DEV_OVERLAY_SURFACE, 0u);
    d3d8_device_store32(DEV_OVERLAY_COUNTER, 0u);

    d3d8_overlay_set_xemu_image(true);
    CHECK(d3d8_overlay_xemu_image());
    hook_log log;
    memset(&log, 0, sizeof(log));
    CHECK(d3d8_overlay_set_present_hook(hook_picture, NULL, &log, resolve_picture));
    (void)update(0u, 0u);
    observable xemu;
    observe(&xemu);
    CHECK(same_observable(&library, &xemu));
    CHECK_EQ_U32(d3d8_overlay_dump_written(), 1u);

    /* the dump file is the xemu box, 11 x 5, and differs from the default picture */
    uint8_t box[11 * 5 * 3];
    expected_picture(true, 10u, 4u, box);
    CHECK(d3d8_overlay_image_write_png("./expected_00000.png", box, 11u, 5u));
    CHECK(files_equal("./overlay_00000.png", "./expected_00000.png"));
    size_t xemu_length = 0u;
    uint8_t *xemu_png = read_file("./overlay_00000.png", &xemu_length);
    CHECK(default_png != NULL && xemu_png != NULL &&
          (default_length != xemu_length || memcmp(default_png, xemu_png, xemu_length) != 0));
    free(default_png);
    free(xemu_png);

    /* the present sink gets the declared rectangle, the top left 10 x 4 of the box */
    CHECK_EQ_U32(log.pictures, 1u);
    CHECK(log.without_picture == 0u);
    d3d8_overlay_set_present_hook(NULL, NULL, NULL, NULL);
    size_t length = 0u;
    uint8_t *index = read_file("./overlay_index.txt", &length);
    CHECK(index != NULL);
    if (index != NULL) {
        index[length] = 0;
        CHECK(strstr((char *)index, "xemu-level") != NULL && strstr((char *)index, "UNMEASURED") == NULL);
    }
    free(index);
    d3d8_overlay_set_dump(NULL, 0u, NULL);
    d3d8_overlay_set_xemu_image(false);
    remove_dump_files();
    environment_end();
}

typedef struct {
    uint32_t width, height;
    uint8_t rgb[16 * 8 * 3];
    bool seen;
} crop_log;

static void crop_picture(const d3d8_overlay_picture *picture, void *context)
{
    crop_log *log = context;
    if (picture->rgb != NULL && picture->width <= 16u && picture->height <= 8u) {
        log->width = picture->width;
        log->height = picture->height;
        memcpy(log->rgb, picture->rgb, (size_t)picture->width * picture->height * 3u);
        log->seen = true;
    }
}

static void test_xemu_image_hook_gets_the_declared_rectangle(void)
{
    begin();
    map_picture();
    rectangle(SOURCE_RECT, 3u, 3u, 8u, 7u);
    rectangle(DESTINATION_RECT, 20u, 30u, 30u, 34u);
    d3d8_overlay_set_xemu_image(true);
    crop_log log;
    memset(&log, 0, sizeof(log));
    CHECK(d3d8_overlay_set_present_hook(crop_picture, NULL, &log, resolve_picture));
    (void)update(0u, 0u);
    uint8_t box[11 * 5 * 3];
    expected_picture(true, 10u, 4u, box);
    CHECK(log.seen && log.width == 10u && log.height == 4u);
    for (uint32_t row = 0u; row < 4u; row++) {
        CHECK(memcmp(log.rgb + (size_t)row * 10u * 3u, box + (size_t)row * 11u * 3u, 10u * 3u) == 0);
    }
    /* default again: the hook gets the library picture of the same size */
    memset(&log, 0, sizeof(log));
    d3d8_overlay_set_xemu_image(false);
    d3d8_overlay_reset();
    (void)update(0u, 0u);
    uint8_t library[10 * 4 * 3];
    expected_picture(false, 10u, 4u, library);
    CHECK(log.seen && log.width == 10u && log.height == 4u && memcmp(log.rgb, library, sizeof(library)) == 0);
    CHECK(memcmp(library, box, 3u) != 0 || memcmp(library + 3u, box + 3u, 3u) != 0);
    d3d8_overlay_set_present_hook(NULL, NULL, NULL, NULL);
    environment_end();
}

static void fill_frame(gpu_image *frame, uint32_t width, uint32_t height, uint32_t argb)
{
    frame->pixels = malloc((size_t)width * height * 4u);
    frame->width = width;
    frame->height = height;
    frame->stride_bytes = width * 4u;
    for (size_t pixel = 0u; pixel < (size_t)width * height; pixel++) {
        frame->pixels[pixel * 4u] = (uint8_t)(argb >> 16);
        frame->pixels[pixel * 4u + 1u] = (uint8_t)(argb >> 8);
        frame->pixels[pixel * 4u + 2u] = (uint8_t)argb;
        frame->pixels[pixel * 4u + 3u] = (uint8_t)(argb >> 24);
    }
}

static void test_key_composition_follows_the_update(void)
{
    for (int xemu = 0; xemu <= 1; xemu++) {
        begin();
        map_picture();
        rectangle(SOURCE_RECT, 3u, 3u, 8u, 7u);
        rectangle(DESTINATION_RECT, 20u, 30u, 30u, 34u);
        d3d8_overlay_set_xemu_image(xemu != 0);
        const uint32_t box_width = xemu ? 11u : 10u;
        const uint32_t box_height = xemu ? 5u : 4u;
        uint8_t picture[11 * 5 * 3];
        gpu_image frame;
        fill_frame(&frame, 64u, 48u, 0x00112233u);

        /* off by default: an update changes nothing displayed and builds no layer */
        CHECK(!d3d8_overlay_key_enabled());
        (void)update(1u, 0x00112233u);
        CHECK(d3d8_overlay_key_displayed(&frame) == &frame);
        CHECK_EQ_U32(d3d8_overlay_dump_failed(), 0u);

        CHECK(d3d8_overlay_set_dump(NULL, 0u, NULL)); /* frees the picture buffers: the composition must allocate its own */
        CHECK(d3d8_overlay_set_key_composition(true, resolve_picture));
        CHECK(d3d8_overlay_key_enabled());
        CHECK(d3d8_overlay_key_displayed(&frame) == &frame); /* nothing latched yet */
        (void)update(1u, 0x00112233u);                      /* key bit set, key = the frame colour */
        CHECK_EQ_U32(d3d8_overlay_state().control & 0x100000u, 0x100000u);
        expected_picture(xemu != 0, 10u, 4u, picture); /* the library scale reads the steps this update wrote */
        const gpu_image *shown = d3d8_overlay_key_displayed(&frame);
        CHECK(shown != &frame && shown->width == 64u && shown->height == 48u);
        for (uint32_t y = 0u; y < 48u; y++) {
            for (uint32_t x = 0u; x < 64u; x++) {
                const uint8_t *pixel = shown->pixels + ((size_t)y * 64u + x) * 4u;
                if (x >= 20u && x < 20u + box_width && y >= 30u && y < 30u + box_height) {
                    const uint8_t *want = picture + ((size_t)(y - 30u) * box_width + (x - 20u)) * 3u;
                    CHECK(pixel[0] == want[0] && pixel[1] == want[1] && pixel[2] == want[2] && pixel[3] == 0xFFu);
                } else {
                    CHECK(pixel[0] == 0x11u && pixel[1] == 0x22u && pixel[2] == 0x33u && pixel[3] == 0x00u);
                }
            }
        }

        /* a key that is not the frame colour (and alpha only differing) leaves the frame whole */
        (void)update(1u, 0x00112234u);
        CHECK(memcmp(d3d8_overlay_key_displayed(&frame)->pixels, frame.pixels, (size_t)64u * 48u * 4u) == 0);
        (void)update(1u, 0xFF112233u); /* alpha ignored: keys again */
        CHECK(d3d8_overlay_key_displayed(&frame)->pixels[(30u * 64u + 20u) * 4u + 3u] == 0xFFu);
        /* key bit clear: every pixel of the rectangle, whatever the key */
        (void)update(0u, 0x00998877u);
        CHECK_EQ_U32(d3d8_overlay_state().control & 0x100000u, 0u);
        shown = d3d8_overlay_key_displayed(&frame);
        CHECK(shown->pixels[(30u * 64u + 20u) * 4u + 3u] == 0xFFu &&
              shown->pixels[((30u + box_height - 1u) * 64u + 20u + box_width - 1u) * 4u + 3u] == 0xFFu);
        uint8_t other[4] = {1, 2, 3, 4};
        memcpy(other, frame.pixels, 4u);
        CHECK(memcmp(shown->pixels, other, 4u) == 0); /* outside the rectangle the frame */

        /* reset drops the layer, the setting stays; switching off stops composing */
        d3d8_overlay_reset();
        CHECK(d3d8_overlay_key_displayed(&frame) == &frame && d3d8_overlay_key_enabled());
        (void)update(0u, 0u);
        CHECK(d3d8_overlay_key_displayed(&frame) != &frame);
        CHECK(d3d8_overlay_set_key_composition(false, NULL));
        CHECK(!d3d8_overlay_key_enabled() && d3d8_overlay_key_displayed(&frame) == &frame);
        free(frame.pixels);
        d3d8_overlay_set_xemu_image(false);
        environment_end();
    }
}

static void test_key_composition_failures_never_show_a_stale_layer(void)
{
    begin();
    map_picture();
    rectangle(SOURCE_RECT, 3u, 3u, 8u, 7u);
    rectangle(DESTINATION_RECT, 20u, 30u, 30u, 34u);
    gpu_image frame;
    fill_frame(&frame, 64u, 48u, 0x00112233u);
    CHECK(d3d8_overlay_set_key_composition(true, resolve_picture));
    (void)update(0u, 0u);
    CHECK(d3d8_overlay_key_displayed(&frame) != &frame);
    /* an update whose picture cannot be built: counted, the earlier layer is not left on screen */
    CHECK(d3d8_overlay_set_key_composition(true, resolve_nothing));
    (void)update(0u, 0u);
    CHECK_EQ_U32(d3d8_overlay_dump_failed(), 1u);
    CHECK(d3d8_overlay_key_displayed(&frame) == &frame);
    CHECK_EQ_U32(d3d8_overlay_state().updates, 2u); /* and the update itself ran */
    d3d8_overlay_set_key_composition(false, NULL);
    free(frame.pixels);
    environment_end();
}

/* T839 (xemu-level): the retail EnableOverlay, with either argument, takes the overlay off the screen, the next update puts it back. */
static void test_enable_overlay_hides_the_layer(void)
{
    begin();
    map_picture();
    rectangle(SOURCE_RECT, 3u, 3u, 8u, 7u);
    rectangle(DESTINATION_RECT, 20u, 30u, 30u, 34u);
    gpu_image frame;
    fill_frame(&frame, 64u, 48u, 0x00112233u);
    CHECK(d3d8_overlay_set_key_composition(true, resolve_picture));
    (void)update(0u, 0u);
    CHECK(d3d8_overlay_key_displayed(&frame) != &frame);
    /* a refused EnableOverlay (a buffer still pending) wrote nothing and so changes nothing displayed */
    const uint32_t off[1] = {0u};
    const uint32_t on[1] = {1u};
    RUN_EXPECTING_FATAL((void)call_stdcall(D3D8_OVERLAY_ENABLE, off, 1u));
    CHECK(fatal_seen);
    CHECK(d3d8_overlay_key_displayed(&frame) != &frame && d3d8_overlay_key_disabled_layers() == 0u);
    for (int argument = 1; argument >= 0; argument--) {
        CHECK(d3d8_overlay_hardware_write(0x8700u, 0u));
        CHECK_EQ_U32(call_stdcall(D3D8_OVERLAY_ENABLE, argument != 0 ? on : off, 1u), 0xFD000000u);
        CHECK(d3d8_overlay_key_displayed(&frame) == &frame);
        CHECK(!d3d8_overlay_key_active());
        (void)update(0u, 0u);
        CHECK(d3d8_overlay_key_displayed(&frame) != &frame);
    }
    CHECK_EQ_U32(d3d8_overlay_key_disabled_layers(), 2u);
    d3d8_overlay_set_key_composition(false, NULL);
    free(frame.pixels);
    environment_end();
}

int main(void)
{
    test_enable_first_call();
    test_update_unscaled();
    test_update_scaled_with_key();
    test_update_key_enable_is_any_nonzero_value();
    test_update_odd_origin_and_one_wide_destination();
    test_update_two_wide_destination_divides_by_one();
    test_status_after_update();
    test_enable_refuses_a_started_buffer();
    test_consume_at_vblank();
    test_update_refusals();
    test_dump_writes_the_picture_the_update_asked_for();
    test_dump_is_a_pure_observer();
    test_dump_failures_are_counted_and_never_change_the_update();
    test_dump_maximum_and_reset();
    test_present_hook_once_per_update_and_per_vblank();
    test_present_hook_reports_unbuildable_and_limited_pictures();
    test_xemu_image_dump_and_hook();
    test_xemu_image_hook_gets_the_declared_rectangle();
    test_key_composition_follows_the_update();
    test_key_composition_failures_never_show_a_stale_layer();
    test_enable_overlay_hides_the_layer();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
