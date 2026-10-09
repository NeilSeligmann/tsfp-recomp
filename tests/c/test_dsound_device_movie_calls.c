/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T421: the device calls the XMV GetNextFrame makes. A second DirectSoundCreate (return 0x44570B) adds a
 * reference to the device the startup made and returns the same interface, device Release (0x406A8A, return
 * 0x445726) takes one such reference back and returns the new count. Both are pinned against the original
 * in tests/test_dsound_device_calls_oracle.py. Everything outside that stops the run. */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#define SLOT 0x412B30u
#define OUT SCRATCH_DATA
#define CREATE 0x409635u
#define RELEASE 0x406A8Au
#define STARTUP_CALLER 0x279D5u
#define MOVIE_CREATE_CALLER 0x44570Bu
#define MOVIE_RELEASE_CALLER 0x445726u
#define STREAM_RELEASE_CALLER 0x386526u
#define READY 6u

static void initialise(bool ready)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    dsound_hle_init();
    dsound_device_reset();
    dsound_device_set_movie_calls(false);
    dsound_device_set_fatal(catching_fatal);
    map_fixed(0x412000u, 0x1000u);
    map_fixed(0x4A1000u, 0x1000u);
    memset(kernel_guest_at(OUT, 16u), 0xAAu, 16u);
    dsound_hle_set_codec_state(ready ? DSOUND_CODEC_READY : DSOUND_CODEC_NOT_READY);
}
static uint32_t device_address(void) { return load(SLOT); }
static uint32_t references(void) { return load(device_address() + 4u); }
static uint32_t call(uint32_t entry, uint32_t caller, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch + 4u, 0x100u, args, count));
    store(frame.stack_ptr, caller);
    return dsound_hle_call(entry, &frame);
}
static uint32_t create_from(uint32_t caller)
{
    const uint32_t args[3] = {0u, OUT, 0u};
    return call(CREATE, caller, args, 3u);
}
static uint32_t release_from(uint32_t caller, uint32_t interface)
{
    const uint32_t args[1] = {interface};
    return call(RELEASE, caller, args, 1u);
}
#define EXPECT_CREATE_FATAL(caller) do { RUN_EXPECTING_FATAL((void)create_from(caller)); CHECK(fatal_seen); } while (0)
#define EXPECT_RELEASE_FATAL(caller, interface) \
    do { RUN_EXPECTING_FATAL((void)release_from(caller, interface)); CHECK(fatal_seen); } while (0)

static void startup_device(void)
{
    CHECK_EQ_U32(dsound_device_register(), 1u);
    CHECK_EQ_U32(create_from(STARTUP_CALLER), 0u);
    CHECK(device_address() != 0u);
    CHECK_EQ_U32(references(), READY);
}

static void test_disabled_by_default(void)
{
    initialise(true);
    startup_device();
    /* Off: the XMV caller of DirectSoundCreate is refused exactly as before, the output stays untouched. */
    CHECK(!dsound_device_movie_calls_enabled());
    memset(kernel_guest_at(OUT, 16u), 0xAAu, 16u);
    EXPECT_CREATE_FATAL(MOVIE_CREATE_CALLER);
    CHECK(strstr(fatal_text, "startup caller") != NULL);
    CHECK_EQ_U32(load(OUT), 0xAAAAAAAAu);
    CHECK_EQ_U32(references(), READY);
    CHECK_EQ_U32(dsound_device_movie_references(), 0u);
    /* Release is not even registered, so it is the unimplemented stub, and a direct call refuses. */
    CHECK(dsound_hle_entry(RELEASE)->state == DSOUND_ENTRY_STUB);
    RUN_EXPECTING_FATAL((void)dsound_device_release_movie(device_address() + 8u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "disabled") != NULL);
    dsound_device_reset();
    environment_end();
}

static void test_registration_is_separate_and_counted(void)
{
    initialise(true);
    CHECK_EQ_U32(dsound_device_register(), 1u);
    const size_t before = dsound_hle_implemented_count();
    CHECK(dsound_hle_entry(RELEASE)->state != DSOUND_ENTRY_IMPLEMENTED);
    CHECK_EQ_U32(dsound_device_register_movie(), 1u);
    CHECK(dsound_hle_entry(RELEASE)->state == DSOUND_ENTRY_IMPLEMENTED);
    CHECK_EQ_U32(dsound_hle_implemented_count(), before + 1u);
    dsound_device_reset();
    environment_end();
}

static void test_movie_create_and_release_pair(void)
{
    initialise(true);
    startup_device();
    CHECK_EQ_U32(dsound_device_register_movie(), 1u);
    dsound_device_set_movie_calls(true);
    const uint32_t address = device_address();
    uint32_t words[11];
    CHECK(kernel_guest_read_bytes(address, words, sizeof(words)));
    for (unsigned round = 1u; round <= 3u; round++) {
        memset(kernel_guest_at(OUT, 16u), 0xAAu, 16u);
        CHECK_EQ_U32(create_from(MOVIE_CREATE_CALLER), 0u);
        CHECK_EQ_U32(load(OUT), address + 8u);
        CHECK_EQ_U32(load(OUT + 4u), 0xAAAAAAAAu);
        CHECK_EQ_U32(references(), READY + round);
        CHECK_EQ_U32(dsound_device_movie_references(), round);
        CHECK_EQ_U32(device_address(), address);
    }
    for (unsigned round = 3u; round >= 1u; round--) {
        CHECK_EQ_U32(release_from(MOVIE_RELEASE_CALLER, address + 8u), READY + round - 1u);
        CHECK_EQ_U32(references(), READY + round - 1u);
        CHECK_EQ_U32(dsound_device_movie_references(), round - 1u);
    }
    uint32_t after[11];
    CHECK(kernel_guest_read_bytes(address, after, sizeof(after)));
    CHECK(memcmp(words, after, sizeof(words)) == 0);
    /* The startup reference is never released: nothing is outstanding now. */
    EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER, address + 8u);
    CHECK(strstr(fatal_text, "no movie reference") != NULL);
    CHECK_EQ_U32(references(), READY);
    /* The startup caller still works and is not counted as a movie reference. */
    CHECK_EQ_U32(create_from(STARTUP_CALLER), 0u);
    CHECK_EQ_U32(references(), READY + 1u);
    CHECK_EQ_U32(dsound_device_movie_references(), 0u);
    EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER, address + 8u);
    dsound_device_reset();
    CHECK_EQ_U32(dsound_device_movie_references(), 0u);
    environment_end();
}

static void test_movie_create_scope(void)
{
    initialise(true);
    CHECK_EQ_U32(dsound_device_register(), 1u);
    dsound_device_set_movie_calls(true);
    /* No startup device yet: a movie Create must not build the first one. */
    EXPECT_CREATE_FATAL(MOVIE_CREATE_CALLER);
    CHECK(strstr(fatal_text, "startup already created") != NULL);
    CHECK_EQ_U32(load(SLOT), 0u);
    CHECK_EQ_U32(guest_mem_heap_count(), 0u);
    CHECK_EQ_U32(create_from(STARTUP_CALLER), 0u);
    /* Another caller, another return address, and non-NULL GUID or outer all stop. */
    EXPECT_CREATE_FATAL(MOVIE_CREATE_CALLER + 1u);
    EXPECT_CREATE_FATAL(MOVIE_RELEASE_CALLER);
    EXPECT_CREATE_FATAL(0x305A3u);
    const uint32_t guid[3] = {1u, OUT, 0u}, outer[3] = {0u, OUT, 1u};
    RUN_EXPECTING_FATAL((void)call(CREATE, MOVIE_CREATE_CALLER, guid, 3u));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)call(CREATE, MOVIE_CREATE_CALLER, outer, 3u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(references(), READY);
    CHECK_EQ_U32(dsound_device_movie_references(), 0u);
    /* A tampered device header stops the movie Create like the startup one. */
    const uint32_t address = device_address(), saved = load(address + 8u);
    store(address + 8u, saved ^ 1u);
    EXPECT_CREATE_FATAL(MOVIE_CREATE_CALLER);
    store(address + 8u, saved);
    CHECK_EQ_U32(dsound_device_movie_references(), 0u);
    dsound_device_reset();
    environment_end();
}

static void test_release_scope(void)
{
    initialise(true);
    startup_device();
    CHECK_EQ_U32(dsound_device_register_movie(), 1u);
    dsound_device_set_movie_calls(true);
    const uint32_t address = device_address(), interface = address + 8u;
    CHECK_EQ_U32(create_from(MOVIE_CREATE_CALLER), 0u);
    CHECK_EQ_U32(references(), READY + 1u);
    /* Callers: only the XMV return address, not the stream destructors or the startup. */
    EXPECT_RELEASE_FATAL(STREAM_RELEASE_CALLER, interface);
    EXPECT_RELEASE_FATAL(STARTUP_CALLER, interface);
    EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER + 1u, interface);
    /* Arguments: only the owned interface, never NULL, the internal address or a neighbour. */
    EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER, 0u);
    EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER, address);
    EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER, interface + 4u);
    EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER, interface - 4u);
    CHECK_EQ_U32(references(), READY + 1u);
    CHECK_EQ_U32(dsound_device_movie_references(), 1u);
    /* A changed header, reference word or singleton is foreign state. */
    for (unsigned i = 0u; i < 11u; i++) {
        const uint32_t saved = load(address + i * 4u);
        store(address + i * 4u, saved ^ 1u);
        EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER, interface);
        store(address + i * 4u, saved);
    }
    store(SLOT, 0u);
    EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER, interface);
    store(SLOT, address);
    CHECK_EQ_U32(references(), READY + 1u);
    CHECK_EQ_U32(release_from(MOVIE_RELEASE_CALLER, interface), READY);
    dsound_device_reset();
    environment_end();
}

static void test_release_never_reaches_the_original_destructor(void)
{
    /* A device made without a ready codec has no children, so its count is 1, not the measured 6. The
     * original destroys the device at a count of 5, which this facade never models, so the Release stops. */
    initialise(false);
    CHECK_EQ_U32(dsound_device_register(), 1u);
    CHECK_EQ_U32(dsound_device_register_movie(), 1u);
    dsound_device_set_movie_calls(true);
    CHECK_EQ_U32(create_from(STARTUP_CALLER), 0x88780078u);
    CHECK_EQ_U32(references(), 1u);
    CHECK_EQ_U32(create_from(MOVIE_CREATE_CALLER), 0u);
    CHECK_EQ_U32(references(), 2u);
    EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER, device_address() + 8u);
    CHECK(strstr(fatal_text, "destructor") != NULL);
    CHECK_EQ_U32(references(), 2u);
    CHECK_EQ_U32(dsound_device_movie_references(), 1u);
    /* The boundary itself: a count of 7 may drop to 6, a count of 6 may not drop to 5. Startup creates
     * raise the count to 5, the movie Create makes it 6 and its Release would leave the measured 5. */
    for (unsigned i = 0u; i < 3u; i++) CHECK_EQ_U32(create_from(STARTUP_CALLER), 0u);
    CHECK_EQ_U32(references(), 5u);
    CHECK_EQ_U32(create_from(MOVIE_CREATE_CALLER), 0u);
    CHECK_EQ_U32(references(), 6u);
    EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER, device_address() + 8u);
    CHECK_EQ_U32(references(), 6u);
    CHECK_EQ_U32(create_from(STARTUP_CALLER), 0u);
    CHECK_EQ_U32(references(), 7u);
    CHECK_EQ_U32(release_from(MOVIE_RELEASE_CALLER, device_address() + 8u), 6u);
    CHECK_EQ_U32(references(), 6u);
    dsound_device_reset();
    environment_end();
}

static void test_reset_clears_the_movie_reference_count(void)
{
    initialise(true);
    startup_device();
    CHECK_EQ_U32(dsound_device_register_movie(), 1u);
    dsound_device_set_movie_calls(true);
    CHECK_EQ_U32(create_from(MOVIE_CREATE_CALLER), 0u);
    CHECK_EQ_U32(dsound_device_movie_references(), 1u);
    dsound_device_reset();  /* the run stopped between Create and Release */
    CHECK_EQ_U32(dsound_device_movie_references(), 0u);
    /* A fresh device starts with no movie reference, so its Release has nothing to give back. */
    CHECK_EQ_U32(create_from(STARTUP_CALLER), 0u);
    EXPECT_RELEASE_FATAL(MOVIE_RELEASE_CALLER, device_address() + 8u);
    CHECK(strstr(fatal_text, "no movie reference") != NULL);
    dsound_device_reset();
    environment_end();
}

int main(void)
{
    test_disabled_by_default();
    test_registration_is_separate_and_counted();
    test_movie_create_and_release_pair();
    test_movie_create_scope();
    test_release_scope();
    test_release_never_reaches_the_original_destructor();
    test_reset_clears_the_movie_reference_count();
    CHECK(checks > 100);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
