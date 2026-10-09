/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "test_d3d8_support.h"
#include "xinput_record.h"
#include "xinput_source.h"
#include <unistd.h>
static char path[] = "/tmp/t1079-input-XXXXXX";
static const char *header = "# tsfp-input v1\n# xbe-sha256: aa11\n# flags-sha256: bb22\n# flags: --x 1\n";
static void write_body(const char *body)
{
    FILE *f = fopen(path, "wb"); CHECK(f != NULL);
    CHECK(fputs(header, f) >= 0); CHECK(fputs(body, f) >= 0); CHECK(fclose(f) == 0);
}
static void load_case(const char *body, bool accepted)
{
    char error[2048]; uint64_t total = 777;
    write_body(body);
    CHECK(xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof(error), &total) == accepted);
    CHECK(total == (accepted ? 3u : 777u));
    /* Failed load must retain the prior installed replay, including its chronology. */
    if (!accepted) CHECK(xinput_source_poll());
}
static bool refused_source(uint64_t index, xinput_pad_state *out, void *user)
{ (void)index; (void)user; *out = (xinput_pad_state){.digital_buttons = XINPUT_BUTTON_START}; return true; }
int main(void)
{
    int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
    xinput_hle_init(); CHECK(xinput_hle_attach_synthetic_pad(0));
    load_case("3 START\n# polls: 3\n", true);
    load_case("3 START\n", false);
    load_case("3 START\n# polls: 999\n", false);
    load_case("3 START\n# polls: 3\n# polls: 3\n", false);
    load_case("3 START\n# polls: 3suffix\n", false);
    load_case("3 START\n# polls: -3\n", false);
    load_case("3 START\n# polls: 18446744073709551616\n", false);
    load_case("3 START\n# polls: 3", false);
    load_case("2 START\n# polls: 3\n1 BACK\n", false);
    load_case("3 START\n# polls: 3\n# lossy: historical annotation\n", true);
    char error[2048]; uint64_t total;
    CHECK(!xinput_record_open("/dev/full", "aa11", "bb22", "--x 1", error, sizeof(error)));
    CHECK(xinput_record_open(path, "aa11", "bb22", "--x 1", error, sizeof(error)));
    CHECK(!xinput_record_open(path, "aa11", "bb22", "--x 1", error, sizeof(error)));
    xinput_pad_state held = {.digital_buttons = XINPUT_BUTTON_BACK, .thumb_left_y = -123};
    xinput_record_observe(0, 0, &held);
    xinput_record_observe(1, 0, NULL); xinput_record_observe(2, 0, NULL);
    CHECK(xinput_record_poll_count() == 3); xinput_record_close(); xinput_record_close();
    CHECK(xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof(error), &total)); CHECK(total == 3);
    CHECK(xinput_record_open(path, "aa11", "bb22", "--x 1", error, sizeof(error)));
    /* Crossing the parser's per-run cap must remain valid, never a >1M line. */
    xinput_record_observe(1000000, 0, NULL); xinput_record_close();
    CHECK(xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof(error), &total)); CHECK(total == 1000001);
    CHECK(xinput_record_open(path, "aa11", "bb22", "--x 1", error, sizeof(error)));
    xinput_record_observe(UINT64_MAX, 0, &held); xinput_record_close();
    CHECK(!xinput_replay_load(path, "aa11", "bb22", "--x 1", error, sizeof(error), &total));
    CHECK(xinput_record_open(path, "aa11", "bb22", "--x 1", error, sizeof(error)));
    CHECK(xinput_hle_detach_synthetic_pad(0));
    xinput_source_reset(); xinput_source_install(refused_source, NULL);
    CHECK(!xinput_source_poll()); CHECK(xinput_record_poll_count() == 1);
    xinput_record_close();
    FILE *f = fopen(path, "rb"); char text[1024] = {0}; CHECK(f != NULL);
    CHECK(fread(text, 1, sizeof(text) - 1, f) > 0); fclose(f);
    CHECK(strstr(text, "1 START") == NULL); CHECK(strstr(text, "\n1\n# polls: 1\n") != NULL);
    unlink(path); xinput_source_reset(); printf("%d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}
