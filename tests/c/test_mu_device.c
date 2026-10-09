/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "mu_device.h"
#include "xinput_hle.h"
#include <stdio.h>
#define DRIVE_OUT SCRATCH_DATA
#define MU_SIZE (8u * 1024u * 1024u)

static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init();
    mu_reset();
    mu_set_fatal(catching_fatal);
    map_fixed(0x76E000u, 0x1000u);
    store(MU_MASK_ADDRESS, 0u);
    store(MU_TITLE_DRIVE_ADDRESS, 0u);
    memset(kernel_guest_at(DRIVE_OUT, 4u), 0xAAu, 4u);
}
static uint32_t call_mount(uint32_t port, uint32_t slot, uint32_t out)
{
    const uint32_t args[3] = {port, slot, out};
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, 3u));
    return xinput_hle_call(MU_ENTRY_MOUNT, &frame);
}
static uint32_t call_unmount(uint32_t port, uint32_t slot)
{
    const uint32_t args[2] = {port, slot};
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, 2u));
    return xinput_hle_call(MU_ENTRY_UNMOUNT, &frame);
}
static uint8_t drive_byte(void) { return *(uint8_t *)kernel_guest_at(DRIVE_OUT, 1u); }

static void test_no_device_contract(void)
{
    initialise();
    CHECK_EQ_U32(mu_register(), 2u);
    CHECK_EQ_U32(xinput_hle_entry(MU_ENTRY_MOUNT)->state, XINPUT_ENTRY_IMPLEMENTED);
    CHECK_EQ_U32(call_mount(0u, 0u, DRIVE_OUT), MU_ERROR_DEVICE_NOT_CONNECTED);
    CHECK_EQ_U32(drive_byte(), 0u);   /* cleared at entry, never written */
    CHECK_EQ_U32(call_unmount(0u, 0u), MU_ERROR_INVALID_DRIVE);
    CHECK_EQ_U32(mu_mounted_mask(), 0u);
    CHECK_EQ_U32(call_mount(3u, 1u, 0u), MU_ERROR_DEVICE_NOT_CONNECTED);
    char host_drive = 'Z';   /* host API: the output is cleared first, then left cleared on failure */
    CHECK_EQ_U32(mu_xmount(2u, 1u, true, &host_drive), MU_ERROR_DEVICE_NOT_CONNECTED);
    CHECK_EQ_U32(host_drive, 0u);
    environment_end();
}
static void test_unformatted_is_unrecognized(void)
{
    initialise();
    CHECK(mu_register() == 2u);
    CHECK(mu_attach_blank(0u, 0u, MU_SIZE, false, 0u));
    CHECK_EQ_U32(call_mount(0u, 0u, DRIVE_OUT), MU_ERROR_UNRECOGNIZED_VOLUME);
    CHECK_EQ_U32(drive_byte(), 0u);
    CHECK_EQ_U32(mu_mounted_mask(), 0u);
    CHECK_EQ_U32(load(MU_MASK_ADDRESS), 0u);
    CHECK(mu_volume(0u, 0u) == NULL);
    environment_end();
}
static void test_mount_unmount_states(void)
{
    initialise();
    CHECK(mu_register() == 2u);
    CHECK(mu_attach_blank(1u, 1u, MU_SIZE, true, 0x1234u));
    CHECK(mu_attach_blank(0u, 0u, MU_SIZE, true, 0x1u));
    CHECK_EQ_U32(call_mount(1u, 1u, DRIVE_OUT), 0u);
    CHECK_EQ_U32(drive_byte(), 'I');
    CHECK_EQ_U32(mu_mounted_mask(), 1u << 3);
    CHECK_EQ_U32(load(MU_MASK_ADDRESS), 1u << 3);
    /* Already mounted: 0x55 and the original still writes the drive byte (0046D80B). */
    memset(kernel_guest_at(DRIVE_OUT, 1u), 0, 1u);
    CHECK_EQ_U32(call_mount(1u, 1u, DRIVE_OUT), MU_ERROR_ALREADY_ASSIGNED);
    CHECK_EQ_U32(drive_byte(), 'I');
    CHECK_EQ_U32(call_mount(0u, 0u, 0u), 0u);     /* NULL output is accepted */
    RUN_EXPECTING_FATAL((void)call_mount(0u, 0u, 0u)); CHECK(fatal_seen);
    CHECK_EQ_U32(mu_mounted_mask(), (1u << 3) | 1u);
    CHECK(!mu_detach(1u, 1u));                      /* cannot unplug while mounted */
    CHECK_EQ_U32(call_unmount(1u, 1u), 0u);
    CHECK_EQ_U32(mu_mounted_mask(), 1u);
    CHECK_EQ_U32(load(MU_MASK_ADDRESS), 1u);
    CHECK_EQ_U32(call_unmount(1u, 1u), MU_ERROR_INVALID_DRIVE);
    CHECK_EQ_U32(call_mount(1u, 1u, DRIVE_OUT), 0u);   /* remount after unmount */
    CHECK_EQ_U32(call_unmount(0u, 0u), 0u);
    CHECK_EQ_U32(call_unmount(1u, 1u), 0u);
    CHECK(mu_detach(1u, 1u));
    CHECK_EQ_U32(call_mount(1u, 1u, DRIVE_OUT), MU_ERROR_DEVICE_NOT_CONNECTED);
    environment_end();
}
static void test_original_index_aliasing_and_refusals(void)
{
    initialise();
    CHECK(mu_register() == 2u);
    /* MEASURED arithmetic: slot 2 of port 0 is port 1 slot 0 (same letter, same bit, same table row). */
    CHECK_EQ_U32(mu_drive_letter(0u, 2u), mu_drive_letter(1u, 0u));
    CHECK_EQ_U32(mu_drive_letter(0u, 0u), 'F');
    CHECK_EQ_U32(mu_drive_letter(3u, 1u), 'M');
    CHECK_EQ_U32(mu_drive_letter(4u, 0u), 0u);
    CHECK_EQ_U32(mu_drive_letter(3u, 2u), 0u);
    CHECK(mu_attach_blank(1u, 0u, MU_SIZE, true, 7u));
    CHECK_EQ_U32(call_mount(0u, 2u, DRIVE_OUT), 0u);
    CHECK_EQ_U32(drive_byte(), 'H');
    CHECK_EQ_U32(call_unmount(1u, 0u), 0u);
    /* Beyond the 8-entry table the original reads past it: refused, state untouched. */
    RUN_EXPECTING_FATAL((void)call_mount(4u, 0u, DRIVE_OUT)); CHECK(fatal_seen);
    CHECK_EQ_U32(drive_byte(), 0u); /* Original clears output before unchecked domain access. */
    RUN_EXPECTING_FATAL((void)call_mount(0u, 8u, DRIVE_OUT)); CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)call_mount(0xFFFFFFFFu, 0u, DRIVE_OUT)); CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)call_unmount(4u, 0u)); CHECK(fatal_seen);
    CHECK_EQ_U32(mu_mounted_mask(), 0u);
    /* Unmapped drive output: the original faults. */
    RUN_EXPECTING_FATAL((void)call_mount(1u, 0u, 0x10u)); CHECK(fatal_seen);
    CHECK_EQ_U32(mu_mounted_mask(), 0u);
    environment_end();
}
static void test_title_drive_unmount_refused(void)
{
    initialise();
    CHECK(mu_register() == 2u);
    CHECK(mu_attach_blank(0u, 0u, MU_SIZE, true, 1u));
    CHECK_EQ_U32(call_mount(0u, 0u, 0u), 0u);
    *(uint8_t *)kernel_guest_at(MU_TITLE_DRIVE_ADDRESS, 1u) = 'F';
    RUN_EXPECTING_FATAL((void)call_unmount(0u, 0u)); CHECK(fatal_seen);
    CHECK_EQ_U32(mu_mounted_mask(), 1u);
    environment_end();
}
static void test_round_trip_and_host_file(void)
{
    initialise();
    CHECK(mu_register() == 2u);
    const char *path = "mu_test_image.bin";
    remove(path);
    CHECK(mu_attach_file(0u, 1u, path, MU_SIZE));
    CHECK_EQ_U32(call_mount(0u, 1u, DRIVE_OUT), 0u);
    fatx_volume *volume = mu_volume(0u, 1u);
    CHECK(volume != NULL);
    uint8_t payload[40000];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 7u + 3u);
    CHECK(fatx_write_file(volume, "SAVE.BIN", payload, sizeof(payload)) == FATX_OK);
    CHECK_EQ_U32(call_unmount(0u, 1u), 0u);
    CHECK(mu_volume(0u, 1u) == NULL);
    CHECK(mu_detach(0u, 1u));
    /* A fresh attach of the persisted host file sees the file. */
    CHECK(mu_attach_file(2u, 0u, path, 0u));
    CHECK_EQ_U32(call_mount(2u, 0u, DRIVE_OUT), 0u);
    uint8_t back[40000]; uint32_t length = 0u;
    CHECK(fatx_read_file(mu_volume(2u, 0u), "SAVE.BIN", back, sizeof(back), &length) == FATX_OK);
    CHECK_EQ_U32(length, sizeof(payload));
    CHECK(memcmp(back, payload, sizeof(payload)) == 0);
    CHECK_EQ_U32(call_unmount(2u, 0u), 0u);
    remove(path);
    environment_end();
}
static void test_fatx_volume(void)
{
    static uint8_t image[MU_SIZE];
    fatx_volume v;
    CHECK(fatx_format(image, 1000u, 1u) == FATX_E_GEOMETRY);
    CHECK(fatx_open(&v, image, sizeof(image)) == FATX_E_GEOMETRY || fatx_open(&v, image, sizeof(image)) == FATX_E_BAD_MAGIC);
    memset(image, 0, sizeof(image));
    CHECK(fatx_open(&v, image, sizeof(image)) == FATX_E_BAD_MAGIC);
    CHECK(fatx_format(image, sizeof(image), 0xCAFEu) == FATX_OK);
    CHECK(fatx_open(&v, image, sizeof(image)) == FATX_OK);
    CHECK_EQ_U32(v.volume_id, 0xCAFEu);
    CHECK(!v.fat32);
    CHECK_EQ_U32(fatx_file_count(&v), 0u);
    const uint32_t total = fatx_free_clusters(&v);
    CHECK(total > 400u);
    CHECK(fatx_write_file(&v, "bad name", "x", 1u) == FATX_E_NAME);
    CHECK(fatx_write_file(&v, "", "x", 1u) == FATX_E_NAME);
    CHECK(fatx_write_file(&v, "A", "hello", 5u) == FATX_OK);
    CHECK_EQ_U32(fatx_free_clusters(&v), total - 1u);
    uint8_t buf[8]; uint32_t length;
    CHECK(fatx_read_file(&v, "A", buf, sizeof(buf), &length) == FATX_OK);
    CHECK_EQ_U32(length, 5u); CHECK(memcmp(buf, "hello", 5u) == 0);
    CHECK(fatx_read_file(&v, "B", buf, sizeof(buf), &length) == FATX_E_NOT_FOUND);
    /* Replace with a 2 cluster file frees the old chain. */
    static uint8_t two[FATX_CLUSTER_BYTES + 1u];
    CHECK(fatx_write_file(&v, "A", two, sizeof(two)) == FATX_OK);
    CHECK_EQ_U32(fatx_free_clusters(&v), total - 2u);
    CHECK_EQ_U32(fatx_file_count(&v), 1u);
    /* A failed oversized write leaves the old file intact. */
    static uint8_t huge[MU_SIZE];
    CHECK(fatx_write_file(&v, "A", huge, sizeof(huge)) == FATX_E_NO_SPACE);
    CHECK(fatx_read_file(&v, "A", NULL, 0u, &length) == FATX_OK); CHECK_EQ_U32(length, sizeof(two));
    CHECK(fatx_remove_file(&v, "A") == FATX_OK);
    CHECK_EQ_U32(fatx_free_clusters(&v), total);
    CHECK_EQ_U32(fatx_file_count(&v), 0u);
    CHECK(fatx_remove_file(&v, "A") == FATX_E_NOT_FOUND);
    /* A deleted entry must not end the directory scan: later entries stay reachable. */
    CHECK(fatx_write_file(&v, "X", "1", 1u) == FATX_OK);
    CHECK(fatx_write_file(&v, "Y", "2", 1u) == FATX_OK);
    CHECK(fatx_write_file(&v, "Z", "3", 1u) == FATX_OK);
    CHECK(fatx_remove_file(&v, "Y") == FATX_OK);
    CHECK_EQ_U32(fatx_file_count(&v), 2u);
    CHECK(fatx_read_file(&v, "Z", buf, sizeof(buf), &length) == FATX_OK); CHECK(buf[0] == '3');
    CHECK(fatx_remove_file(&v, "X") == FATX_OK); CHECK(fatx_remove_file(&v, "Z") == FATX_OK);
    /* Replacing a file that fills the whole volume must succeed: its own clusters count as reusable. */
    const uint32_t all = fatx_free_clusters(&v);
    CHECK(fatx_write_file(&v, "FULL", huge, all * FATX_CLUSTER_BYTES) == FATX_OK);
    CHECK_EQ_U32(fatx_free_clusters(&v), 0u);
    CHECK(fatx_write_file(&v, "FULL", huge, all * FATX_CLUSTER_BYTES) == FATX_OK);
    CHECK(fatx_write_file(&v, "MORE", "x", 1u) == FATX_E_NO_SPACE);
    CHECK(fatx_remove_file(&v, "FULL") == FATX_OK);
    /* Directory full: 256 entries. */
    char name[16];
    for (unsigned i = 0; i < 256u; i++) { snprintf(name, sizeof(name), "F%u", i); CHECK(fatx_write_file(&v, name, "", 0u) == FATX_OK); }
    CHECK(fatx_write_file(&v, "OVERFLOW", "", 0u) == FATX_E_DIR_FULL);
    /* Corrupt geometry in the superblock is refused. */
    image[8] = 3u;
    CHECK(fatx_open(&v, image, sizeof(image)) == FATX_E_GEOMETRY);
}
int main(void)
{
    test_no_device_contract();
    test_unformatted_is_unrecognized();
    test_mount_unmount_states();
    test_original_index_aliasing_and_refusals();
    test_title_drive_unmount_refused();
    test_round_trip_and_host_file();
    test_fatx_volume();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
