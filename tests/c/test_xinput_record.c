/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
/* T1074: pad record and replay. Header identity, run length format, record then replay through the guest bytes. */
#include "test_d3d8_support.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#include "xinput_source.h"
#include "xinput_record.h"
#include "xinput_route_spec.h"
#include <stdlib.h>
#include <unistd.h>
#define TYPE 0x46C75Cu
#define DECL SCRATCH_DATA
#define OUT (SCRATCH_DATA + 0x100u)
#define HANDLE 0x58504430u
static const uint32_t declarations[8] = {0x46C6E0u, 8u, 0x46C8A0u, 4u, 0x46C894u, 4u, 0x46C75Cu, 4u};
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init();
    xinput_devices_reset();
    xinput_source_reset();
    xinput_devices_set_fatal(catching_fatal);
    map_fixed(0x46C000u, 0x1000u);
    map_fixed(0x771000u, 0x1000u);
    memcpy(kernel_guest_at(DECL, 32u), declarations, 32u);
    CHECK(xinput_hle_attach_synthetic_pad(0u));
    xinput_devices_enable_synthetic_pad(true);
    CHECK_EQ_U32(xinput_devices_init_empty(4u, DECL), 0u);
    CHECK_EQ_U32(xinput_pad_open(TYPE, 0u, 0u, 0u), HANDLE);
}
static void finish(void)
{
    xinput_source_reset();
    xinput_devices_enable_synthetic_pad(false);
    environment_end();
}
static void read_state(uint32_t handle, uint32_t *packet, uint16_t *buttons, uint8_t analog[8], int16_t thumbs[4])
{
    CHECK_EQ_U32(xinput_pad_state_read(handle, OUT), 0u);
    const uint8_t *at = kernel_guest_at(OUT, 22u);
    memcpy(packet, at, 4u); memcpy(buttons, at + 4u, 2u); memcpy(analog, at + 6u, 8u); memcpy(thumbs, at + 14u, 8u);
}
static const char XBE[] = "aa11", FLAGS_HASH[] = "bb22", FLAGS[] = "--x 1";
static xinput_pad_state live(uint64_t poll)
{
    xinput_pad_state state = {0};
    if (poll >= 2u && poll < 5u) state.digital_buttons = XINPUT_BUTTON_START;
    if (poll >= 5u && poll <= 7u) { state.analog[0] = 255u; state.thumb_left_x = -300; }
    return state;
}
static bool live_source(uint64_t poll, xinput_pad_state *out, void *user)
{
    (void)user;
    if (poll == 8u) return false; /* a declined poll keeps the previous state */
    *out = live(poll);
    return true;
}
static void test_identity_flags(void)
{
    char *a[] = {"host", "d.xbe", "--hdd", "h1", "--a", "1", "--present", "window", "--record-input", "f", "--b",
                 "--stop-after-calls", "0x1:2", "--disc", "i.iso", "--synthetic-pad"};
    char *b[] = {"host", "other.xbe", "--hdd", "h2", "--a", "1", "--replay-input", "g", "--b", "--disc", "j.iso",
                 "--synthetic-pad"};
    char out_a[256], out_b[256], tiny[4];
    CHECK(xinput_record_identity_flags(16, a, out_a, sizeof(out_a)));
    CHECK(xinput_record_identity_flags(12, b, out_b, sizeof(out_b)));
    CHECK(strcmp(out_a, "--a 1 --b") == 0);
    CHECK(strcmp(out_a, out_b) == 0);
    char *c[] = {"host", "d.xbe", "--a", "2", "--b"};
    CHECK(xinput_record_identity_flags(5, c, out_b, sizeof(out_b)));
    CHECK(strcmp(out_a, out_b) != 0);
    CHECK(!xinput_record_identity_flags(5, c, tiny, sizeof(tiny)));
}
/* T1616: a route recorded by one tool run replays under another: the poke gate, the dumps and the route flags are not identity,
 * a behaviour flag still is. */
static void test_route_flags_not_identity(void)
{
    char *recording[] = {"host", "d.xbe", "--synthetic-pad", "--gpu-live", "--record-input", "r", "--pad-source", "keyboard"};
    char *replay[] = {"host", "d.xbe", "--synthetic-pad", "--gpu-live", "--replay-input", "r", "--replay-handover", "--pad-source",
                      "keyboard", "--forced-state", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d",
                      "--dump-guest-max-bytes", "1024", "--poke-at-poll", "mark3:x", "--route-wait", "mark1:min=3"};
    char a[512], b[512], c[512];
    CHECK(xinput_record_identity_flags(8, recording, a, sizeof(a)));
    CHECK(xinput_record_identity_flags(20, replay, b, sizeof(b)));
    CHECK(strcmp(a, b) == 0);
    CHECK(strcmp(a, "--gpu-live") == 0);
    char *different[] = {"host", "d.xbe", "--synthetic-pad", "--native-xmv", "--replay-input", "r"};
    CHECK(xinput_record_identity_flags(6, different, c, sizeof(c)));
    CHECK(strcmp(a, c) != 0);
    /* a recorded line that names the gate is canonicalised the same way */
    char canonical[512];
    CHECK(xinput_record_canonical_line("--gpu-live --forced-state --dump-guest-range 0x10:4 --dump-guest-dir x", canonical, sizeof(canonical)));
    CHECK(strcmp(canonical, "--gpu-live") == 0);
    CHECK(xinput_record_canonical_line("--gpu-live --voice-log private.log", canonical, sizeof(canonical)));
    CHECK(strcmp(canonical, "--gpu-live") == 0);
    /* T1741: the write watch is a passive observer, not identity */
    CHECK(xinput_record_canonical_line("--gpu-live --watch-write **0x7A3580+0+0x1CC:4 --watch-write-log w.log --watch-write-max 9", canonical, sizeof(canonical)));
    CHECK(strcmp(canonical, "--gpu-live") == 0);
}
/* T1126: output paths that differ per run dir are not identity, behaviour flags still are. */
static void test_path_canonicalisation(void)
{
    char *run1[] = {"host", "d.xbe", "--dump-overlay", "tmp/play/r1/overlay", "--gpu-replay", "tmp/play/modules",
                    "--gpu-live", "--gpu-replay-dump", "d1", "--hdd", "tmp/play/r1/hdd"};
    char *run2[] = {"host", "d.xbe", "--gpu-replay", "/abs/other/modules", "--gpu-live", "--hdd", "x",
                    "--dump-overlay", "tmp/play/r2/overlay"};
    char one[256], two[256];
    CHECK(xinput_record_identity_flags(11, run1, one, sizeof(one)));
    CHECK(xinput_record_identity_flags(9, run2, two, sizeof(two)));
    CHECK(strcmp(one, "--gpu-replay <path> --gpu-live") == 0);
    CHECK(strcmp(one, two) == 0);
    /* MUTATION GUARD: a changed behaviour flag still differs. */
    char *other[] = {"host", "d.xbe", "--gpu-replay", "m", "--gpu-live", "--native-xmv"};
    CHECK(xinput_record_identity_flags(6, other, two, sizeof(two)));
    CHECK(strcmp(one, two) != 0);
    /* A record made before T1126 carries raw paths in its flags line and still canonicalises equal. */
    CHECK(xinput_record_canonical_line("--gpu-live --dump-overlay tmp/play/20261005-173457/overlay --gpu-replay tmp/m", two, sizeof(two)));
    CHECK(strcmp(two, "--gpu-live --gpu-replay <path>") == 0);
    char header[1024], error[600];
    const size_t n = xinput_record_header(header, sizeof(header), XBE, "old-hash",
                                          "--gpu-live --dump-overlay tmp/play/r0/overlay --gpu-replay tmp/m");
    CHECK(n > 0u);
    CHECK(xinput_record_check_header(header, n, XBE, "new-hash", "--gpu-live --gpu-replay <path>", error, sizeof(error)));
    CHECK(!xinput_record_check_header(header, n, XBE, "new-hash", "--gpu-live --native-xmv --gpu-replay <path>", error, sizeof(error)));
    CHECK(strstr(error, "different flag set") != NULL);
}
/* T1126: the budgets header line round trips through a record file into the replay. */
static void test_budgets_round_trip(void)
{
    char path[] = "/tmp/tsfp-input-budget-XXXXXX", error[600], line[256];
    const xinput_record_budgets budgets = {1000u, 100u, 2147483647u, true};
    xinput_record_budgets back = {0};
    const size_t n = xinput_record_format_budgets(line, sizeof(line), &budgets);
    CHECK(n > 0u);
    CHECK(strcmp(line, "# budgets: owner-waits=1000 worker-blanks=100 thread-timeout=2147483647 interactive=1\n") == 0);
    char text[600];
    const size_t head = xinput_record_header(text, sizeof(text), XBE, FLAGS_HASH, FLAGS);
    memcpy(text + head, line, n);
    CHECK(xinput_record_parse_budgets(text, head + n, &back));
    CHECK_EQ_U32(back.owner_waits, 1000u);
    CHECK_EQ_U32(back.worker_blanks, 100u);
    CHECK_EQ_U32(back.thread_timeout_ms, 2147483647u);
    CHECK(back.interactive);
    CHECK(!xinput_record_parse_budgets("# tsfp-input v1\n# budgets: owner-waits=1\n", 41u, &back));
    CHECK(!xinput_record_parse_budgets(text, head, &back)); /* a record without the line */
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) return;
    close(fd);
    initialise();
    xinput_record_set_budgets(&budgets);
    CHECK(xinput_record_open(path, XBE, FLAGS_HASH, FLAGS, error, sizeof(error)));
    xinput_source_install(live_source, NULL);
    uint32_t packet; uint16_t buttons; uint8_t analog[8]; int16_t thumbs[4];
    for (unsigned poll = 0u; poll < 4u; poll++) read_state(HANDLE, &packet, &buttons, analog, thumbs);
    xinput_record_close();
    xinput_source_reset();
    xinput_source_install(NULL, NULL);
    uint64_t total = 0u;
    CHECK(xinput_replay_load(path, XBE, FLAGS_HASH, FLAGS, error, sizeof(error), &total));
    memset(&back, 0, sizeof(back));
    CHECK(xinput_replay_budgets(&back));
    CHECK_EQ_U32(back.owner_waits, 1000u);
    CHECK(back.interactive);
    CHECK_EQ_U32((uint32_t)xinput_replay_total_polls(), 4u);
    for (unsigned poll = 0u; poll < 3u; poll++) read_state(HANDLE, &packet, &buttons, analog, thumbs);
    CHECK_EQ_U32((uint32_t)xinput_source_port_poll_count(0u), 3u); /* polls consumed, the host report's number */
    finish();
    unlink(path);
}
static void test_format_and_header(void)
{
    char line[256], error[512];
    bool lossy = false;
    xinput_pad_state state = {0};
    state.digital_buttons = XINPUT_BUTTON_START | XINPUT_BUTTON_DPAD_UP | 0x1000u;
    state.analog[2] = 7u; state.thumb_right_y = 5;
    CHECK(xinput_record_format_run(line, sizeof(line), 12u, &state, &lossy) > 0u);
    CHECK(strcmp(line, "12 UP START X=7 RY=5\n") == 0);
    CHECK(lossy);
    char header[512];
    const size_t n = xinput_record_header(header, sizeof(header), XBE, FLAGS_HASH, FLAGS);
    CHECK(n > 0u);
    CHECK(xinput_record_check_header(header, n, XBE, FLAGS_HASH, FLAGS, error, sizeof(error)));
    CHECK(!xinput_record_check_header(header, n, "cc33", FLAGS_HASH, FLAGS, error, sizeof(error)));
    CHECK(strstr(error, "different XBE") != NULL);
    CHECK(!xinput_record_check_header(header, n, XBE, "dd44", "--y", error, sizeof(error)));
    CHECK(strstr(error, "different flag set") != NULL && strstr(error, "--x 1") != NULL);
    CHECK(!xinput_record_check_header("1 UP\n", 5u, XBE, FLAGS_HASH, FLAGS, error, sizeof(error)));
    CHECK(strstr(error, "not an input record") != NULL);
    header[14] = '2'; /* "# tsfp-input v1" -> v2 */
    CHECK(!xinput_record_check_header(header, n, XBE, FLAGS_HASH, FLAGS, error, sizeof(error)));
    CHECK(strstr(error, "unsupported record version") != NULL);
}
static void test_record_then_replay_matches(void)
{
    char path[] = "/tmp/tsfp-input-record-XXXXXX", error[600];
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) return;
    close(fd);
    initialise();
    CHECK(xinput_record_open(path, XBE, FLAGS_HASH, FLAGS, error, sizeof(error)));
    xinput_source_install(live_source, NULL);
    uint32_t packet; uint16_t buttons; uint8_t analog[8]; int16_t thumbs[4];
    uint16_t seen_buttons[12]; uint8_t seen_a[12]; int16_t seen_lx[12];
    for (unsigned poll = 0u; poll < 12u; poll++) {
        read_state(HANDLE, &packet, &buttons, analog, thumbs);
        seen_buttons[poll] = buttons; seen_a[poll] = analog[0]; seen_lx[poll] = thumbs[0];
    }
    xinput_record_close();
    CHECK_EQ_U32((uint32_t)xinput_record_poll_count(), 12u);
    /* Poll 8 was declined and the pad stayed as poll 7 left it, so the record has no gap. */
    xinput_source_reset();
    xinput_source_install(NULL, NULL);
    CHECK(!xinput_replay_load(path, "zz", FLAGS_HASH, FLAGS, error, sizeof(error), NULL));
    CHECK(strstr(error, "different XBE") != NULL);
    uint64_t total = 0u;
    CHECK(xinput_replay_load(path, XBE, FLAGS_HASH, FLAGS, error, sizeof(error), &total));
    CHECK_EQ_U32((uint32_t)total, 12u);
    for (unsigned poll = 0u; poll < 12u; poll++) {
        read_state(HANDLE, &packet, &buttons, analog, thumbs);
        CHECK_EQ_U32(buttons, seen_buttons[poll]);
        CHECK_EQ_U32(analog[0], seen_a[poll]);
        CHECK_EQ_U32((uint32_t)(int32_t)thumbs[0], (uint32_t)(int32_t)seen_lx[poll]);
    }
    CHECK_EQ_U32(seen_buttons[3], XINPUT_BUTTON_START);
    CHECK_EQ_U32(seen_buttons[5], 0u);
    CHECK_EQ_U32(seen_a[8], 255u); /* the declined poll 8 kept poll 7's state */
    CHECK_EQ_U32(seen_a[9], 0u);
    finish();
    unlink(path);
}

/* T1618: the owner's first live run refused the route because the replay (tools.action_profile) added --skip-intro and the census
 * observers. The observers are read-only and not identity, --skip-intro changes the title and is. */
static void test_observer_flags_not_identity(void)
{
    char *plain[] = {"host", "d.xbe", "--xonline-offline", "--skip-intro", "--gpu-live", "--synthetic-pad", "--replay-input", "r"};
    char *census[] = {"host", "d.xbe", "--xonline-offline", "--skip-intro", "--gpu-live", "--synthetic-pad", "--replay-input", "r",
                      "--census-icalls", "--census-phases", "tmp/owner-profiles/s/census.txt", "--census-window", "3:9",
                      "--cpu-profile", "p.txt", "--cpu-profile-wall", "q.txt", "--profile-watch", "0x10:4"};
    char a[512], b[512];
    CHECK(xinput_record_identity_flags(8, plain, a, sizeof(a)));
    CHECK(strcmp(a, "--xonline-offline --skip-intro --gpu-live") == 0);
    CHECK(xinput_record_identity_flags(19, census, b, sizeof(b)));
    CHECK(strcmp(a, b) == 0);
    /* each observer alone, with its value, vanishes completely (the value is not left behind as a stray token) */
    char *flag_only[] = {"host", "d.xbe", "--gpu-live", "--census-icalls", "--native-xmv"};
    CHECK(xinput_record_identity_flags(5, flag_only, b, sizeof(b)));
    CHECK(strcmp(b, "--gpu-live --native-xmv") == 0);
    static const char *const valued[] = {"--census-phases", "--census-window", "--cpu-profile", "--cpu-profile-wall",
                                         "--hotkey", "--hotkey-dir",
                                         "--shot-dir", "--shot-max", "--shot-max-bytes"}; /* T1627/T1720b: host hotkeys and shots are tooling, not identity */
    for (size_t k = 0u; k < sizeof(valued) / sizeof(valued[0]); k++) {
        char *one[] = {"host", "d.xbe", "--gpu-live", (char *)valued[k], "value", "--native-xmv"};
        CHECK(xinput_record_identity_flags(6, one, b, sizeof(b)));
        CHECK(strcmp(b, "--gpu-live --native-xmv") == 0);
    }
    /* EXACTLY the listed observers: a game affecting flag and look-alike census flags stay in the identity */
    char *without_skip[] = {"host", "d.xbe", "--xonline-offline", "--gpu-live", "--synthetic-pad", "--replay-input", "r"};
    CHECK(xinput_record_identity_flags(7, without_skip, b, sizeof(b)));
    CHECK(strcmp(a, b) != 0);
    CHECK(strcmp(b, "--xonline-offline --gpu-live") == 0);
    /* T1629: the passive button dump observer and every one of its tuning flags (with their values) are not identity either */
    char *button_dump[] = {"host", "d.xbe", "--gpu-live", "--dump-on-button", "--dump-after-frames", "2,30", "--dump-button-threshold", "60",
                           "--dump-button-coalesce", "3", "--dump-button-max-pending", "8", "--dump-button-max-dumps", "2000",
                           "--dump-button-max-bytes", "0x40000000", "--dump-button-start-poll", "9", "--dump-button-after-replay",
                           "--dump-button-idle-every", "600", "--dump-button-max-idle", "40", "--native-xmv"};
    CHECK(xinput_record_identity_flags(24, button_dump, b, sizeof(b)));
    CHECK(strcmp(b, "--gpu-live --native-xmv") == 0);
    char *button_lookalike[] = {"host", "d.xbe", "--dump-button", "--dump-on-buttons", "--dump-after-frame"};
    CHECK(xinput_record_identity_flags(5, button_lookalike, b, sizeof(b)));
    CHECK(strcmp(b, "--dump-button --dump-on-buttons --dump-after-frame") == 0);
    char *lookalike[] = {"host", "d.xbe", "--gpu-live", "--gpu-replay-rt-texture-census", "--census-icalls-all", "--census",
                         "--hotkeys", "--hotkey-directory", "--shot-dirs", "--shot-max-byte", "--shot"};
    CHECK(xinput_record_identity_flags(11, lookalike, b, sizeof(b)));
    CHECK(strcmp(b, "--gpu-live --gpu-replay-rt-texture-census --census-icalls-all --census --hotkeys --hotkey-directory --shot-dirs --shot-max-byte --shot") == 0);
    /* T1720b: the three shot flags with a value, next to the hotkey flags, vanish from a whole command line (a replay with them is the
     * same run as the recording without them) */
    char *with_shots[] = {"host", "d.xbe", "--gpu-live", "--hotkey", "kb=CTRL+SHIFT+S:shot", "--hotkey-dir", "hk", "--shot-dir", "sh",
                          "--shot-max", "9", "--shot-max-bytes", "1000", "--native-xmv"};
    CHECK(xinput_record_identity_flags(14, with_shots, b, sizeof(b)));
    CHECK(strcmp(b, "--gpu-live --native-xmv") == 0);
}
/* T1618: the refusal prints only the flags that differ, units of a flag with its values, plus the re-record advice. */
static void test_flag_diff_message(void)
{
    char only_recorded[200], only_current[200], error[2400];
    CHECK(xinput_record_flags_diff("--a --b 1 --c", "--a --skip-intro --b 1 --c", only_recorded, sizeof(only_recorded),
                                   only_current, sizeof(only_current)));
    CHECK(strcmp(only_recorded, "") == 0);
    CHECK(strcmp(only_current, "--skip-intro") == 0);
    /* a changed value is one unit on each side, not two stray numbers */
    CHECK(xinput_record_flags_diff("--a --vblank-owner-waits 5 --c", "--a --vblank-owner-waits 6 --c", only_recorded,
                                   sizeof(only_recorded), only_current, sizeof(only_current)));
    CHECK(strcmp(only_recorded, "--vblank-owner-waits 5") == 0 && strcmp(only_current, "--vblank-owner-waits 6") == 0);
    /* a repeated flag is matched pairwise */
    CHECK(xinput_record_flags_diff("--m 1 --m 1", "--m 1", only_recorded, sizeof(only_recorded), only_current, sizeof(only_current)));
    CHECK(strcmp(only_recorded, "--m 1") == 0 && strcmp(only_current, "") == 0);
    CHECK(!xinput_record_flags_diff("--a 1 --b", "--a 1 --b", only_recorded, sizeof(only_recorded), only_current, sizeof(only_current)));
    CHECK(!xinput_record_flags_diff("--a --b", "--b --a", only_recorded, sizeof(only_recorded), only_current, sizeof(only_current)));
    /* a list that does not fit ends with "..." and stays a terminated string */
    char tiny[24];
    CHECK(xinput_record_flags_diff("", "--aaaaaaaa --bbbbbbbb --cccccccc --dddddddd", only_recorded, sizeof(only_recorded), tiny,
                                   sizeof(tiny)));
    CHECK(strlen(tiny) < sizeof(tiny) && strlen(tiny) >= 3u && strcmp(tiny + strlen(tiny) - 3u, "...") == 0);
    /* the owner's report: the recording lacks --skip-intro, the replay has it (the observers vanish in the canonical line) */
    char header[1024];
    const char recorded[] = "--xonline-offline --ac97-ready --native-xmv --gpu-live --gpu-replay <path> --gpu-live-translate";
    const size_t n = xinput_record_header(header, sizeof(header), XBE, "old-hash", recorded);
    CHECK(n > 0u);
    CHECK(xinput_record_check_header(header, n, XBE, "new-hash", recorded, error, sizeof(error)));
    CHECK(!xinput_record_check_header(header, n, XBE, "new-hash",
                                      "--xonline-offline --skip-intro --ac97-ready --native-xmv --gpu-live --gpu-replay <path> --gpu-live-translate",
                                      error, sizeof(error)));
    CHECK(strstr(error, "different flag set") != NULL);
    CHECK(strstr(error, "only in this run:      --skip-intro") != NULL);
    CHECK(strstr(error, "only in the recording: (nothing)") != NULL);
    CHECK(strstr(error, "tmp/record_mapmaker_route.fish") != NULL);
    /* ONLY the differences: the flags both sides share are not printed */
    CHECK(strstr(error, "--native-xmv") == NULL && strstr(error, "--ac97-ready") == NULL && strstr(error, "--gpu-live-translate") == NULL);
    /* a record with raw census observers in its flags line (made by an older host) still replays under a canonical line */
    const size_t m = xinput_record_header(header, sizeof(header), XBE, "old-hash",
                                          "--xonline-offline --skip-intro --census-icalls --census-phases tmp/s/census.txt --gpu-live");
    CHECK(m > 0u);
    CHECK(xinput_record_check_header(header, m, XBE, "new-hash", "--xonline-offline --skip-intro --gpu-live", error, sizeof(error)));
    CHECK(!xinput_record_check_header(header, m, XBE, "new-hash", "--xonline-offline --gpu-live", error, sizeof(error)));
    CHECK(strstr(error, "only in the recording: --skip-intro") != NULL);
    /* a record without a flags line says so instead of printing two lines */
    const char no_flags[] = "# tsfp-input v1\n# xbe-sha256: aa11\n# flags-sha256: zz\n1 UP\n# polls: 1\n";
    CHECK(!xinput_record_check_header(no_flags, sizeof(no_flags) - 1u, XBE, "new-hash", "--gpu-live", error, sizeof(error)));
    CHECK(strstr(error, "no usable '# flags:' line") != NULL);
}

/* ---- T1632: taking a hotkey chord key out of the record ---- */
#define TRIM_BACK XINPUT_BUTTON_BACK
static const xinput_record_effect effect_back = {XINPUT_BUTTON_BACK, -1, -1};
static const xinput_record_effect effect_a = {0u, 0, -1};   /* analog button A */
static const xinput_record_effect effect_lx = {0u, -1, 0};  /* left stick X */

static char *slurp(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    char *text = calloc(1u, 1u << 20);
    if (text != NULL) { size_t got = fread(text, 1u, (1u << 20) - 1u, file); text[got] = '\0'; }
    fclose(file);
    return text;
}
/* the file without its three header lines (identity and budgets differ in nothing here, but the body is what is compared) */
static const char *body_of(const char *text)
{
    const char *at = text;
    while (*at == '#') { const char *eol = strchr(at, '\n'); if (eol == NULL) return at + strlen(at); at = eol + 1; }
    return at;
}
typedef struct {
    bool press_back;       /* the lead BACK is held polls 3..9 in the live pad and is taken out by a trim */
    bool with_trim;
    bool back_again;      /* a genuine BACK tap at polls 14..15 after the gesture */
    unsigned mark_at;     /* 0 = none, else a mark is requested before this poll */
    unsigned polls;
} trim_case;
static bool open_record(const char *path)
{
    char error[200];
    return xinput_record_open(path, XBE, FLAGS_HASH, FLAGS, error, sizeof error);
}
static void run_trim_case(const char *path, const trim_case *c)
{
    CHECK(open_record(path));
    for (unsigned poll = 0u; poll < c->polls; poll++) {
        xinput_pad_state state = {0};
        if (poll >= 5u && poll <= 6u) state.analog[0] = 255u;             /* a genuine A press inside the gesture window */
        if (poll == 2u) state.analog[1] = 255u;                           /* a B tap whose run ends exactly where the trim starts */
        if (c->press_back && poll >= 3u && poll <= 9u) state.digital_buttons |= TRIM_BACK;
        if (c->back_again && poll >= 14u && poll <= 15u) state.digital_buttons |= TRIM_BACK;
        if (c->with_trim && poll == 7u) xinput_record_suppress_begin(&effect_back, 3u); /* the chord fired at poll 7 */
        if (c->with_trim && poll == 10u) xinput_record_suppress_end(&effect_back, 10u);  /* the lead came up at poll 10 */
        if (c->mark_at != 0u && poll == c->mark_at) CHECK(xinput_record_request_mark());
        xinput_record_observe(poll, 0u, &state);
    }
    xinput_record_close();
}
static void test_trim_removes_the_gesture(void)
{
    const char *dirty_path = "/tmp/t1632_dirty.rec", *clean_path = "/tmp/t1632_clean.rec";
    trim_case dirty = {true, true, true, 0u, 20u};
    trim_case clean = {false, false, true, 0u, 20u};
    run_trim_case(dirty_path, &dirty);
    run_trim_case(clean_path, &clean);
    char *a = slurp(dirty_path), *b = slurp(clean_path);
    CHECK(a != NULL && b != NULL);
    if (a != NULL && b != NULL) {
        CHECK(strcmp(a, b) == 0);                       /* byte identical to a recording where BACK was never held in the gesture */
        CHECK(strstr(body_of(a), "BACK") != NULL);      /* the genuine tap at polls 14..15 is kept ... */
        CHECK(strstr(a, "3 BACK") == NULL && strstr(a, "7 BACK") == NULL); /* ... the 7 poll hold is gone */
        CHECK(strstr(a, "2 A=255") != NULL);            /* the A press inside the window is kept */
        CHECK(strstr(a, "# polls: 20") != NULL);        /* the length is unchanged: polls are never removed */
    }
    CHECK(xinput_record_trims() == 0u);                 /* counters are per recording (the clean one reset them) */
    free(a);
    free(b);
    /* and it replays: the dirty file loads and its pad at the gesture polls is at rest */
    initialise();
    char error[300];
    uint64_t total = 0u;
    CHECK(xinput_replay_load(dirty_path, XBE, FLAGS_HASH, FLAGS, error, sizeof error, &total));
    CHECK(total == 20u);
    const xinput_script *script = xinput_replay_script();
    CHECK(script != NULL);
    if (script != NULL) {
        for (uint64_t poll = 0u; poll < 14u; poll++) CHECK(xinput_script_state_at(script, poll).digital_buttons == 0u);
        CHECK(xinput_script_state_at(script, 14u).digital_buttons == TRIM_BACK);
        CHECK(xinput_script_state_at(script, 5u).analog[0] == 255u);
    }
    finish();
    remove(dirty_path);
    remove(clean_path);
}
static void test_trim_keeps_marks_and_position(void)
{
    const char *dirty_path = "/tmp/t1632_dirty_mark.rec", *clean_path = "/tmp/t1632_clean_mark.rec";
    trim_case dirty = {true, true, false, 8u, 16u};
    trim_case clean = {false, false, false, 8u, 16u};
    run_trim_case(dirty_path, &dirty);
    const unsigned trims = xinput_record_trims();
    run_trim_case(clean_path, &clean);
    char *a = slurp(dirty_path), *b = slurp(clean_path);
    CHECK(a != NULL && b != NULL);
    if (a != NULL && b != NULL) {
        CHECK(strcmp(a, b) == 0);
        CHECK(strstr(a, "# mark: at=8\n") != NULL); /* the mark sits where it was asked for, in the middle of the trimmed span */
        CHECK(strstr(a, "BACK") == NULL);
    }
    CHECK(trims == 1u);
    free(a);
    free(b);
    remove(dirty_path);
    remove(clean_path);
}
static void test_trim_without_end_runs_to_the_close(void)
{
    /* the stop chord: the lead is still held when the run ends, everything from its start is cut */
    const char *path = "/tmp/t1632_open.rec";
    CHECK(open_record(path));
    for (unsigned poll = 0u; poll < 12u; poll++) {
        xinput_pad_state state = {0};
        if (poll >= 4u) state.digital_buttons = TRIM_BACK;
        if (poll == 8u) xinput_record_suppress_begin(&effect_back, 4u);
        xinput_record_observe(poll, 0u, &state);
    }
    xinput_record_close();
    char *text = slurp(path);
    CHECK(text != NULL && strstr(text, "BACK") == NULL && strstr(text, "12\n") != NULL && strstr(text, "# polls: 12") != NULL);
    free(text);
    remove(path);
}
static void test_trim_effects_and_references(void)
{
    const char *path = "/tmp/t1632_effects.rec";
    CHECK(open_record(path));
    for (unsigned poll = 0u; poll < 10u; poll++) {
        xinput_pad_state state = {0};
        state.analog[0] = 255u;
        state.analog[1] = 255u;
        state.thumb_left_x = 32767;
        state.thumb_left_y = 5;
        if (poll == 2u) { xinput_record_suppress_begin(&effect_a, 0u); xinput_record_suppress_begin(&effect_lx, 0u); }
        if (poll == 3u) xinput_record_suppress_begin(&effect_a, 1u);   /* a second key with the same effect */
        if (poll == 5u) xinput_record_suppress_end(&effect_a, 5u);     /* one of the two came up: still cut */
        if (poll == 7u) xinput_record_suppress_end(&effect_a, 7u);     /* both up: recorded again from here */
        if (poll == 8u) xinput_record_suppress_end(&effect_lx, 8u);
        xinput_record_observe(poll, 0u, &state);
    }
    xinput_record_close();
    char *text = slurp(path);
    CHECK(text != NULL);
    if (text != NULL) {
        /* polls 0..6: no A, no LX, B and LY stay; poll 7: A back; poll 8..9: LX back */
        CHECK(strstr(text, "7 B=255 LY=5\n1 A=255 B=255 LY=5\n2 A=255 B=255 LX=32767 LY=5\n") != NULL);
        CHECK(strstr(text, "# polls: 10") != NULL);
    }
    free(text);
    remove(path);
}
static void test_trim_older_than_the_tail_is_clipped(void)
{
    const char *path = "/tmp/t1632_clipped.rec";
    CHECK(open_record(path));
    for (unsigned poll = 0u; poll < 9000u; poll++) {
        xinput_pad_state state = {0};
        if (poll < 8990u) state.digital_buttons = poll % 2u != 0u ? TRIM_BACK : 0u; /* many runs, old ones are already written */
        else state.digital_buttons = TRIM_BACK;
        if (poll == 8995u) xinput_record_suppress_begin(&effect_back, 10u);
        xinput_record_observe(poll, 0u, &state);
    }
    xinput_record_close();
    CHECK(xinput_record_trims_clipped() == 1u);
    char *text = slurp(path);
    CHECK(text != NULL && strstr(text, "1 BACK\n") != NULL); /* the old part stays, it was already on disk */
    free(text);
    remove(path);
}
static void test_trim_no_recording_is_a_noop(void)
{
    xinput_record_suppress_begin(&effect_back, 0u);
    xinput_record_suppress_end(&effect_back, 1u);
    CHECK(!xinput_record_request_mark());
    xinput_record_suppress_begin(NULL, 0u);
}
static void test_mark_cap_and_request(void)
{
    const char *path = "/tmp/t1632_marks.rec";
    CHECK(open_record(path));
    xinput_pad_state rest = {0};
    for (unsigned poll = 0u; poll < 20u; poll++) {
        if (poll != 0u) CHECK(xinput_record_request_mark()); /* 19 requests */
        xinput_record_observe(poll, 0u, &rest);
    }
    xinput_record_close();
    CHECK(xinput_record_marks_written() == XINPUT_ROUTE_MAX_MARKS); /* 16 kept, the rest refused loudly */
    char *text = slurp(path);
    CHECK(text != NULL);
    unsigned count = 0u;
    for (const char *at = text; text != NULL && (at = strstr(at, "# mark: at=")) != NULL; at++) count++;
    CHECK(count == XINPUT_ROUTE_MAX_MARKS);
    free(text);
    /* and the file is a valid route: the replay accepts all 16 */
    initialise();
    char error[300];
    uint64_t total = 0u;
    CHECK(xinput_replay_load(path, XBE, FLAGS_HASH, FLAGS, error, sizeof error, &total));
    const uint64_t *marks = NULL;
    CHECK(xinput_replay_marks(&marks) == XINPUT_ROUTE_MAX_MARKS);
    finish();
    remove(path);
}


int main(void)
{
    test_identity_flags();
    test_path_canonicalisation();
    test_route_flags_not_identity();
    test_observer_flags_not_identity();
    test_flag_diff_message();
    test_budgets_round_trip();
    test_format_and_header();
    test_record_then_replay_matches();
    test_trim_removes_the_gesture();
    test_trim_keeps_marks_and_position();
    test_trim_without_end_runs_to_the_close();
    test_trim_effects_and_references();
    test_trim_older_than_the_tail_is_clipped();
    test_trim_no_recording_is_a_noop();
    test_mark_cap_and_request();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
