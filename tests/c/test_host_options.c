/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Command-line option parsing, and every default it establishes.
 *
 * WHY THIS SUITE EXISTS. `parse_options` lived in `src/host/main.c`, which is compiled
 * into `tsfp_host` alone. `tsfp_host` is not a ctest binary, so nothing in it could be
 * covered by any suite or reached by `tools/mutate/c_suites.py`. The task that built
 * `--hdd` wanted to mutate "the no-`--hdd` default flipped to writable", found it
 * unreachable, and mutated a kernel-side guard instead, which is detection by NULL
 * dereference rather than by assertion. This suite is that gap being closed.
 *
 * EVERY DEFAULT HERE IS A SAFETY ARGUMENT, which is why each is pinned individually
 * rather than checked in a loop. A silent change to one would fail no build and no other
 * test, and would change what the title sees in a way that surfaces far from its cause:
 *
 *   hdd_path NULL             nothing is writable unless an operator said where
 *   disc_path NULL            the image is the user's property, never assumed present
 *   disc_device a DEVICE      the title creates the `D:` alias itself at 0x00381301
 *   hdd_device no separator   a trailing one matches the bare device and nothing under it
 *   open_missing_as_empty 0   fabricating a file is a diagnostic, not a behaviour
 *   ac97_ready true           T375 owner decision: a retail console has the codec ready
 *                             (still FABRICATED, announced, --no-ac97-ready opts out)
 *   continue_on_missing false the first unimplemented ordinal stops the run
 *
 * DELIBERATELY FREE OF LIFTED CODE, THE XBE AND ANY DISC. It builds argv arrays and
 * reads back a struct. Nothing is opened, nothing is mapped.
 */

#include "host_options.h"
#include "xinput_hotkey.h"
#include "xmv_seed_patch.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

#define CHECK_STR(actual, expected)                                                     \
    do {                                                                                \
        checks++;                                                                       \
        const char *a_ = (actual);                                                      \
        const char *e_ = (expected);                                                    \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                                        \
            printf("FAIL %s:%d  %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__,   \
                   #actual, a_ ? a_ : "(null)", e_);                                    \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

/* argv is `char **`, and string literals are not writable, so the cases build their
 * arrays from this rather than casting away const on a literal. */
#define ARGV(...)                                                                       \
    (char *[]) { __VA_ARGS__ }

static bool parse(options *out, int argc, char **argv)
{
    /* Poison the struct first. A parser that forgot to set a field would otherwise
     * inherit whatever the previous case left, and every assertion below would pass for
     * the wrong reason. */
    memset(out, 0xA5, sizeof(*out));
    return parse_options(argc, argv, out);
}

/* --- the defaults ---------------------------------------------------------- */

static void test_voice_log_option(void)
{
    options opt;
    CHECK(parse(&opt, 4, ARGV("tsfp_host", "game.xbe", "--voice-log", "requests.log")));
    CHECK_STR(opt.voice_log, "requests.log");
    CHECK(!parse(&opt, 3, ARGV("tsfp_host", "game.xbe", "--voice-log")));
    CHECK(!parse(&opt, 4, ARGV("tsfp_host", "game.xbe", "--voice-log", "")));
}

static void test_the_bare_invocation_sets_every_default(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("tsfp_host", "game.xbe")));

    CHECK_STR(opt.xbe_path, "game.xbe");

    /* THE TWO THAT MATTER MOST. Both NULL means the host fabricates no storage at all,
     * which is the honest state and also the one an operator must opt out of. */
    CHECK(opt.voice_log == NULL);
    CHECK(strstr(host_options_voice_help(), "--voice-log FILE") != NULL);
    CHECK(strstr(host_options_voice_help(), "Return values unavailable.") != NULL);
    CHECK(opt.hdd_path == NULL);
    CHECK(opt.disc_path == NULL);

    /* A DEVICE, not a drive letter. MEASURED: the title calls
     * IoCreateSymbolicLink("\??\D:", "\Device\CdRom0") at 0x00381301, so mounting the
     * device lets its own link resolve instead of competing with it. */
    CHECK_STR(opt.disc_device, "\\Device\\CdRom0");

    /* NO TRAILING SEPARATOR. The mount table needs the character after a matched prefix
     * to be a separator or end-of-string, so a prefix ending in one matches the bare
     * device and nothing beneath it. kernel_file_mount_host_dir refuses such a prefix. */
    CHECK_STR(opt.hdd_device, "\\Device\\Harddisk0\\partition1");
    const size_t hdd_len = strlen(opt.hdd_device);
    CHECK(opt.hdd_device[hdd_len - 1u] != '\\' && opt.hdd_device[hdd_len - 1u] != '/');

    /* 3 is a retail console's cache partitions. Pinned as the LITERAL: 0 would make the title
     * call memmove with length 0xFFFFFFF4, and asserting against the macro would pass for any
     * value, including that one. */
    CHECK(opt.cache_partitions == 3u);
    CHECK(DEFAULT_CACHE_PARTITIONS == 3u);
    CHECK(opt.open_missing_as_empty == false);
    CHECK(opt.ac97_ready == true);
    /* T72a: passive effects CPU views are the default. The acceptance contract is
     * measured (caller 0x000270AF, exact dsstdfx image), so the gate was the one
     * artificial refusal left at the reached call. Streams/buffers/listener remain
     * opt-in: their contracts are not fully recovered. */
    CHECK(opt.headless_effects == true);
    CHECK(opt.headless_streams == false);
    CHECK(opt.headless_buffers == false);
    CHECK(opt.headless_listener == false);
    CHECK(opt.headless_first_vblank == false);
    CHECK(opt.native_shader_assembler == false);
    CHECK(opt.xonline_offline == false);
    CHECK(opt.continue_on_missing == false);
    CHECK(opt.stub_status_set == false);
    CHECK(opt.stub_status == 0u);
    CHECK(opt.mount_count == 0u);
    CHECK(opt.audio_sink == NULL);
    CHECK(opt.audio_output == NULL);
    CHECK(opt.trace_report == DEFAULT_TRACE_REPORT);
    CHECK(opt.thread_timeout_ms == DEFAULT_THREAD_TIMEOUT_MS);
}

static void test_the_defaults_are_not_merely_whatever_the_macros_say(void)
{
    /* Asserting `opt.trace_report == DEFAULT_TRACE_REPORT` above would pass for ANY
     * value of the macro, including 0. These pin the values themselves, because a trace
     * report of 0 would silently hide every call and a thread timeout of 0 would call
     * every run a hang. */
    CHECK(DEFAULT_TRACE_REPORT > 0);
    CHECK(DEFAULT_THREAD_TIMEOUT_MS >= 1000u);
    CHECK(OPTION_MOUNT_MAX >= 1u);
}

static void test_xonline_offline_is_default_off_and_explicitly_opt_in(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("host", "game.xbe")));
    CHECK(opt.xonline_offline == false);
    CHECK(parse(&opt, 3, ARGV("host", "--xonline-offline", "game.xbe")));
    CHECK(opt.xonline_offline == true);
    CHECK(opt.continue_on_missing == false);
    CHECK(opt.present == NULL);
}

/* --- the flags ------------------------------------------------------------- */

static void test_each_boolean_flag_sets_only_itself(void)
{
    options opt;

    CHECK(parse(&opt, 3, ARGV("h", "--ac97-ready", "g.xbe")));
    CHECK(opt.ac97_ready == true);
    CHECK(opt.open_missing_as_empty == false);
    CHECK(opt.continue_on_missing == false);

    CHECK(parse(&opt, 3, ARGV("h", "--open-missing-as-empty", "g.xbe")));
    CHECK(opt.open_missing_as_empty == true);
    CHECK(opt.ac97_ready == true);
    CHECK(opt.headless_effects == true); /* default ON, unaffected by other flags */
    CHECK(opt.headless_streams == false);
    CHECK(opt.headless_buffers == false);
    CHECK(opt.headless_listener == false);
    CHECK(opt.headless_first_vblank == false);
    CHECK(opt.native_shader_assembler == false);

    CHECK(parse(&opt, 3, ARGV("h", "--continue-on-missing", "g.xbe")));
    CHECK(opt.continue_on_missing == true);
    CHECK(opt.ac97_ready == true);
    CHECK(opt.headless_effects == true); /* default ON, unaffected by other flags */
    CHECK(opt.headless_streams == false);
    CHECK(opt.headless_buffers == false);
    CHECK(opt.headless_listener == false);
    CHECK(opt.headless_first_vblank == false);
    CHECK(opt.native_shader_assembler == false);
}

static void test_headless_effects_is_default_and_flag_is_a_noop(void)
{
    options opt;
    /* T72a: the flag stays accepted (operator scripts keep working) and is a no-op. */
    CHECK(parse(&opt, 3, ARGV("h", "--headless-effects", "g.xbe")));
    CHECK(opt.headless_effects);
    CHECK(opt.ac97_ready);
    CHECK(!opt.continue_on_missing);
    CHECK(!opt.open_missing_as_empty);
    CHECK(parse(&opt, 4, ARGV("h", "--ac97-ready", "--headless-effects", "g.xbe")));
    CHECK(opt.headless_effects && opt.ac97_ready);
    CHECK(parse(&opt, 3, ARGV("h", "--ac97-ready", "g.xbe")));
    CHECK(opt.headless_effects && opt.ac97_ready);
    CHECK(!parse(&opt, 2, ARGV("h", "--headless-effects")));
}

static void test_second_vblank_policy(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && !opt.headless_second_vblank);
    const char *flags[]={"--ac97-ready","--headless-effects","--headless-streams",
                         "--headless-buffers","--headless-listener"};
    for(unsigned mask=0u;mask<32u;mask++) {
        char *args[10]={"h","--headless-second-vblank"};int count=2;
        for(unsigned i=0u;i<5u;i++)if(mask&(1u<<i))args[count++]=(char *)flags[i];
        args[count++]="game.xbe";
        /* headless_effects (T72a) and ac97_ready (T375) default true, so their bits are redundant:
         * second-vblank needs the OTHER three flags explicitly. */
        CHECK(parse(&opt,count,args)==((mask|3u)==31u));
        if((mask|3u)==31u)CHECK(opt.headless_second_vblank && !opt.headless_first_vblank);
    }
    CHECK(!parse(&opt,9,ARGV("h","--ac97-ready","--headless-effects","--headless-streams",
          "--headless-buffers","--headless-listener","--headless-first-vblank",
          "--headless-second-vblank","game.xbe")));
    CHECK(!parse(&opt,3,ARGV("h","--headless-second-vblank","game.xbe")));
}

/* T372: the coupling flag is off by default and needs no other flag. */
static void test_model_flips_is_default_off_and_needs_the_coupling(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && !opt.model_flips);
    CHECK(parse(&opt, 4, ARGV("h", "--couple-vblank-effects", "--model-flips", "game.xbe")));
    CHECK(opt.model_flips && opt.couple_vblank_effects);
    CHECK(parse(&opt, 4, ARGV("h", "--model-flips", "--couple-vblank-effects", "game.xbe")));
    CHECK(opt.model_flips && opt.couple_vblank_effects);
    CHECK(!parse(&opt, 3, ARGV("h", "--model-flips", "game.xbe")));
    CHECK(parse(&opt, 3, ARGV("h", "--couple-vblank-effects", "game.xbe")) && !opt.model_flips);
}

static void test_couple_vblank_effects_is_default_off_and_standalone(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && !opt.couple_vblank_effects);
    CHECK(parse(&opt, 3, ARGV("h", "--couple-vblank-effects", "game.xbe")));
    CHECK(opt.couple_vblank_effects && !opt.headless_first_vblank && !opt.headless_second_vblank);
    CHECK(parse(&opt, 4, ARGV("h", "--headless-first-vblank", "--couple-vblank-effects", "game.xbe")));
    CHECK(opt.couple_vblank_effects && opt.headless_first_vblank);
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && !opt.couple_vblank_effects);
}

/* T370: the dispatch-level refinement is default off and only exists on top of a vblank policy. */
static void test_vblank_dispatch_level_requires_a_policy(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && !opt.vblank_dispatch_level);
    CHECK(!parse(&opt, 3, ARGV("h", "--vblank-dispatch-level", "game.xbe")));
    CHECK(parse(&opt, 4, ARGV("h", "--headless-first-vblank", "--vblank-dispatch-level", "game.xbe")));
    CHECK(opt.vblank_dispatch_level && opt.headless_first_vblank);
    CHECK(parse(&opt, 4, ARGV("h", "--vblank-dispatch-level", "--headless-first-vblank", "game.xbe")));
    CHECK(opt.vblank_dispatch_level);
    CHECK(parse(&opt, 10, ARGV("h", "--ac97-ready", "--headless-streams", "--headless-buffers",
          "--headless-listener", "--headless-second-vblank", "--vblank-dispatch-level",
          "--couple-vblank-effects", "--trace-vblank-schedule", "game.xbe")));
    CHECK(opt.vblank_dispatch_level && opt.headless_second_vblank && opt.couple_vblank_effects);
    CHECK(parse(&opt, 3, ARGV("h", "--couple-vblank-effects", "game.xbe")) && !opt.vblank_dispatch_level);
}

/* T371: the reader trace and the quiescence check are default off and need a vblank policy. */
static void test_vblank_quiescence_flags_require_a_policy(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")));
    CHECK(!opt.trace_vblank_readers && !opt.check_vblank_quiescence);
    CHECK(!parse(&opt, 3, ARGV("h", "--trace-vblank-readers", "game.xbe")));
    CHECK(!parse(&opt, 3, ARGV("h", "--check-vblank-quiescence", "game.xbe")));
    CHECK(parse(&opt, 4, ARGV("h", "--headless-first-vblank", "--check-vblank-quiescence", "game.xbe")));
    CHECK(opt.check_vblank_quiescence && !opt.trace_vblank_readers && opt.headless_first_vblank);
    CHECK(parse(&opt, 4, ARGV("h", "--trace-vblank-readers", "--headless-first-vblank", "game.xbe")));
    CHECK(opt.trace_vblank_readers && !opt.check_vblank_quiescence);
    CHECK(parse(&opt, 10, ARGV("h", "--ac97-ready", "--headless-streams", "--headless-buffers",
          "--headless-listener", "--headless-second-vblank", "--check-vblank-quiescence",
          "--trace-vblank-readers", "--trace-vblank-schedule", "game.xbe")));
    CHECK(opt.check_vblank_quiescence && opt.trace_vblank_readers && opt.headless_second_vblank);
}

/* T460: the owner-wait budget is default off, takes a whole number from 1 to 1000000 and exists only
 * on top of the second-event policy with the coupled effects and the quiescence check. */
static void test_vblank_owner_waits_needs_its_prerequisites(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && opt.vblank_owner_waits == 0u);
    const char *audio[] = {"--ac97-ready", "--headless-streams", "--headless-buffers", "--headless-listener"};
    const char *full[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                          "--couple-vblank-effects", "--check-vblank-quiescence",
                          "--vblank-owner-waits", "250", "game.xbe"};
    CHECK(parse(&opt, 11, (char **)full) && opt.vblank_owner_waits == 250u);
    /* dropping any one prerequisite refuses */
    for (int dropped = 5; dropped <= 7; dropped++) {
        const char *partial[11];
        int count = 0;
        for (int index = 0; index < 11; index++) {
            if (index != dropped) partial[count++] = full[index];
        }
        CHECK(!parse(&opt, count, (char **)partial));
    }
    CHECK(!parse(&opt, 4, ARGV("h", "--vblank-owner-waits", "5", "game.xbe")));
    /* the first-event policy has no owner arm, even with the other two prerequisites */
    CHECK(!parse(&opt, 7, ARGV("h", "--headless-first-vblank", "--couple-vblank-effects",
                               "--check-vblank-quiescence", "--vblank-owner-waits", "5", "game.xbe")));
    CHECK(!parse(&opt, 6, ARGV("h", "--headless-first-vblank", "--couple-vblank-effects",
                               "--vblank-owner-waits", "5", "game.xbe")));
    /* the value: positive, whole, capped, nothing after it, and present */
    const char *bad[] = {"0", "-1", "+5", " 5", "5x", "1000001", "", "99999999999999999999", "0x10"};
    for (unsigned index = 0u; index < sizeof bad / sizeof bad[0]; index++) {
        const char *arguments[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                                   "--couple-vblank-effects", "--check-vblank-quiescence",
                                   "--vblank-owner-waits", bad[index], "game.xbe"};
        CHECK(!parse(&opt, 11, (char **)arguments));
    }
    const char *capped[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                            "--couple-vblank-effects", "--check-vblank-quiescence",
                            "--vblank-owner-waits", "1000000", "game.xbe"};
    CHECK(parse(&opt, 11, (char **)capped) && opt.vblank_owner_waits == 1000000u);
    const char *missing[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                             "--couple-vblank-effects", "--check-vblank-quiescence", "--vblank-owner-waits"};
    CHECK(!parse(&opt, 9, (char **)missing));
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && opt.vblank_owner_waits == 0u);
}

/* T696: the poll blank is default off and needs --vblank-owner-waits (so every option that needs). */
static void test_vblank_poll_blank_needs_the_owner_waits(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && !opt.vblank_poll_blank);
    const char *audio[] = {"--ac97-ready", "--headless-streams", "--headless-buffers", "--headless-listener"};
    const char *full[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                          "--couple-vblank-effects", "--check-vblank-quiescence",
                          "--vblank-owner-waits", "250", "--vblank-poll-blank", "game.xbe"};
    CHECK(parse(&opt, 12, (char **)full) && opt.vblank_poll_blank && opt.vblank_owner_waits == 250u);
    const char *no_owner[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                              "--couple-vblank-effects", "--check-vblank-quiescence",
                              "--vblank-poll-blank", "game.xbe"};
    CHECK(!parse(&opt, 10, (char **)no_owner));
    CHECK(!parse(&opt, 3, ARGV("h", "--vblank-poll-blank", "game.xbe")));
    const char *owner_only[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                                "--couple-vblank-effects", "--check-vblank-quiescence",
                                "--vblank-owner-waits", "250", "game.xbe"};
    /* T762: with the owner waits the poll blank is a DEFAULT, --no-vblank-poll-blank is the opt-out */
    CHECK(parse(&opt, 11, (char **)owner_only) && opt.vblank_poll_blank && opt.vblank_poll_blank_by_default);
    CHECK(parse(&opt, 12, (char **)full) && opt.vblank_poll_blank && !opt.vblank_poll_blank_by_default);
    const char *owner_no[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                              "--couple-vblank-effects", "--check-vblank-quiescence",
                              "--vblank-owner-waits", "250", "--no-vblank-poll-blank", "game.xbe"};
    CHECK(parse(&opt, 12, (char **)owner_no) && !opt.vblank_poll_blank && !opt.vblank_poll_blank_by_default);
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && !opt.vblank_poll_blank);
}

/* T592: the worker's blanks are default off, take the same number as the owner waits and exist only on top of
 * --vblank-owner-waits (so on every option that needs). */
static void test_vblank_worker_blanks_needs_the_owner_waits(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && opt.vblank_worker_blanks == 0u);
    const char *audio[] = {"--ac97-ready", "--headless-streams", "--headless-buffers", "--headless-listener"};
    const char *full[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                          "--couple-vblank-effects", "--check-vblank-quiescence",
                          "--vblank-owner-waits", "250", "--vblank-worker-blanks", "61", "game.xbe"};
    CHECK(parse(&opt, 13, (char **)full) && opt.vblank_worker_blanks == 61u && opt.vblank_owner_waits == 250u);
    /* without the owner waits (the other prerequisites present) it refuses */
    const char *no_owner[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                              "--couple-vblank-effects", "--check-vblank-quiescence",
                              "--vblank-worker-blanks", "61", "game.xbe"};
    CHECK(!parse(&opt, 11, (char **)no_owner));
    CHECK(!parse(&opt, 4, ARGV("h", "--vblank-worker-blanks", "5", "game.xbe")));
    /* the owner waits alone leave the worker off */
    const char *owner_only[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                                "--couple-vblank-effects", "--check-vblank-quiescence",
                                "--vblank-owner-waits", "250", "game.xbe"};
    CHECK(parse(&opt, 11, (char **)owner_only) && opt.vblank_worker_blanks == 0u);
    /* the value: positive, whole, capped, nothing after it, and present */
    const char *bad[] = {"0", "-1", "+5", " 5", "5x", "1000001", "", "99999999999999999999", "0x10"};
    for (unsigned index = 0u; index < sizeof bad / sizeof bad[0]; index++) {
        const char *arguments[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                                   "--couple-vblank-effects", "--check-vblank-quiescence",
                                   "--vblank-owner-waits", "250", "--vblank-worker-blanks", bad[index], "game.xbe"};
        CHECK(!parse(&opt, 13, (char **)arguments));
    }
    const char *capped[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                            "--couple-vblank-effects", "--check-vblank-quiescence",
                            "--vblank-owner-waits", "250", "--vblank-worker-blanks", "1000000", "game.xbe"};
    CHECK(parse(&opt, 13, (char **)capped) && opt.vblank_worker_blanks == 1000000u);
    const char *missing[] = {"h", audio[0], audio[1], audio[2], audio[3], "--headless-second-vblank",
                             "--couple-vblank-effects", "--check-vblank-quiescence",
                             "--vblank-owner-waits", "250", "--vblank-worker-blanks"};
    CHECK(!parse(&opt, 11, (char **)missing));
    /* a failed parse leaves nothing half set for the next one */
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && opt.vblank_worker_blanks == 0u);
}

static void test_trace_vblank_schedule_requires_a_policy(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("h", "game.xbe")) && !opt.trace_vblank_schedule);
    CHECK(!parse(&opt, 3, ARGV("h", "--trace-vblank-schedule", "game.xbe")));
    CHECK(parse(&opt, 4, ARGV("h", "--headless-first-vblank", "--trace-vblank-schedule", "game.xbe")));
    CHECK(opt.trace_vblank_schedule && opt.headless_first_vblank && !opt.headless_second_vblank);
    CHECK(parse(&opt, 4, ARGV("h", "--trace-vblank-schedule", "--headless-first-vblank", "game.xbe")));
    CHECK(opt.trace_vblank_schedule && opt.headless_first_vblank);
    CHECK(parse(&opt, 8, ARGV("h", "--ac97-ready", "--headless-streams", "--headless-buffers",
          "--headless-listener", "--headless-second-vblank", "--trace-vblank-schedule", "game.xbe")));
    CHECK(opt.trace_vblank_schedule && opt.headless_second_vblank && !opt.headless_first_vblank);
    CHECK(parse(&opt, 3, ARGV("h", "--headless-first-vblank", "game.xbe")) && !opt.trace_vblank_schedule);
}

static void test_headless_audio_policies_are_independent(void)
{
    for (unsigned mask = 0u; mask < 128u; mask++) {
        char *arguments[10] = {"host"};
        int count = 1;
        if ((mask & 1u) != 0u) arguments[count++] = "--ac97-ready";
        if ((mask & 2u) != 0u) arguments[count++] = "--headless-effects";
        if ((mask & 4u) != 0u) arguments[count++] = "--headless-streams";
        if ((mask & 8u) != 0u) arguments[count++] = "--headless-buffers";
        if ((mask & 16u) != 0u) arguments[count++] = "--headless-listener";
        if ((mask & 32u) != 0u) arguments[count++] = "--headless-first-vblank";
        if ((mask & 64u) != 0u) arguments[count++] = "--native-shader-assembler";
        arguments[count++] = "game.xbe";
        options opt;
        CHECK(parse(&opt, count, arguments));
        CHECK(opt.ac97_ready == true); /* default ON (T375): the flag is a deprecated alias */
        CHECK(opt.ac97_ready_flag_given == ((mask & 1u) != 0u));
        CHECK(opt.headless_effects == true); /* default ON (T72a); the flag is a no-op */
        CHECK(opt.headless_streams == ((mask & 4u) != 0u));
        CHECK(opt.headless_buffers == ((mask & 8u) != 0u));
        CHECK(opt.headless_listener == ((mask & 16u) != 0u));
        CHECK(opt.headless_first_vblank == ((mask & 32u) != 0u));
        CHECK(opt.native_shader_assembler == ((mask & 64u) != 0u));
        CHECK(!opt.continue_on_missing && !opt.open_missing_as_empty);
    }
    options opt;
    CHECK(!parse(&opt, 2, ARGV("host", "--native-shader-assembler")));
    CHECK(parse(&opt, 4, ARGV("host", "--native-shader-assembler", "--native-shader-assembler", "game.xbe")));
    CHECK(opt.native_shader_assembler && !opt.headless_first_vblank && opt.ac97_ready);
    CHECK(!parse(&opt, 2, ARGV("host", "--headless-streams")));
    CHECK(parse(&opt, 4, ARGV("host", "--headless-streams", "--headless-streams", "game.xbe")));
    CHECK(opt.headless_streams && opt.ac97_ready && opt.headless_effects && !opt.headless_buffers);
    CHECK(!parse(&opt, 2, ARGV("host", "--headless-buffers")));
    CHECK(parse(&opt, 4, ARGV("host", "--headless-buffers", "--headless-buffers", "game.xbe")));
    CHECK(opt.headless_buffers && opt.ac97_ready && opt.headless_effects && !opt.headless_streams && !opt.headless_listener);
    CHECK(!parse(&opt, 2, ARGV("host", "--headless-first-vblank")));
    CHECK(parse(&opt, 4, ARGV("host", "--headless-first-vblank", "--headless-first-vblank", "game.xbe")));
    CHECK(opt.headless_first_vblank && opt.ac97_ready && opt.headless_effects &&
          !opt.headless_streams && !opt.headless_buffers && !opt.headless_listener);
    CHECK(!parse(&opt, 2, ARGV("host", "--headless-listener")));
    CHECK(parse(&opt, 4, ARGV("host", "--headless-listener", "--headless-listener", "game.xbe")));
    CHECK(opt.headless_listener && opt.ac97_ready && opt.headless_effects && !opt.headless_streams && !opt.headless_buffers);
}

/* T92/T234: --native-xmv runs the title's own XMV decoder over the user's disc. Default off,
 * needs a mounted disc (the movie is read from d:\xmv), and --trace-xmv needs it. */
static void test_native_xmv_defaults_with_a_disc(void)
{
    options defaults, enabled, traced;
    CHECK(parse(&defaults, 2, ARGV("host", "game.xbe")));
    CHECK(defaults.native_xmv == false && defaults.trace_xmv == false);
    CHECK(parse(&enabled, 5, ARGV("host", "--native-xmv", "--disc", "img.iso", "game.xbe")));
    CHECK(enabled.native_xmv && !enabled.trace_xmv && enabled.disc_path != NULL);
    /* Independent of the shader compiler profile and of every audio policy. */
    CHECK(!enabled.native_shader_assembler && enabled.ac97_ready && !enabled.headless_streams);
    CHECK(!parse(&enabled, 3, ARGV("host", "--native-xmv", "game.xbe")));
    CHECK(parse(&traced, 6, ARGV("host", "--native-xmv", "--trace-xmv", "--disc", "img.iso", "game.xbe")));
    CHECK(traced.native_xmv && traced.trace_xmv);
    CHECK(!parse(&traced, 4, ARGV("host", "--trace-xmv", "--disc", "game.xbe")));
    CHECK(!parse(&traced, 3, ARGV("host", "--trace-xmv", "game.xbe")));
    /* T1093: a disc alone turns the retained XMV route on (not the trace), --no-native-xmv opts out. */
    CHECK(parse(&traced, 4, ARGV("host", "--disc", "img.iso", "game.xbe")));
    CHECK(traced.native_xmv && !traced.native_xmv_explicit && !traced.trace_xmv);
    CHECK(parse(&traced, 5, ARGV("host", "--no-native-xmv", "--disc", "img.iso", "game.xbe")));
    CHECK(!traced.native_xmv && traced.native_xmv_off);
    CHECK(!parse(&traced, 6, ARGV("host", "--no-native-xmv", "--trace-xmv", "--disc", "img.iso", "game.xbe")));
    CHECK(!parse(&traced, 6, ARGV("host", "--no-native-xmv", "--native-xmv", "--disc", "img.iso", "game.xbe")));
}

/* T394: the movie test aids are default off and each needs --native-xmv, the frame files and the
 * movie substitution also need --trace-xmv or at least the retained exports. Malformed values fail. */
/* T538: the codec entry capture names its directory and its entries together, rides the trace and the
 * cooperative seam of a headless vblank policy, and takes up to 32 ascending, disjoint ranges. */
static void test_xmv_entry_capture_options(void)
{
    options on, defaults;
    CHECK(parse(&defaults, 2, ARGV("host", "game.xbe")));
    CHECK(defaults.capture_xmv_entries == NULL && defaults.capture_xmv_range_count == 0u);
    CHECK(parse(&on, 11, ARGV("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank",
                              "--capture-xmv-entries", "dir", "--capture-xmv-entry-at", "2,5-9,11",
                              "--disc", "img.iso", "game.xbe")));
    CHECK(on.capture_xmv_entries != NULL && strcmp(on.capture_xmv_entries, "dir") == 0);
    CHECK(on.capture_xmv_range_count == 3u);
    CHECK(on.capture_xmv_range_first[0] == 2u && on.capture_xmv_range_last[0] == 2u);
    CHECK(on.capture_xmv_range_first[1] == 5u && on.capture_xmv_range_last[1] == 9u);
    CHECK(on.capture_xmv_range_first[2] == 11u && on.capture_xmv_range_last[2] == 11u);
    /* A later parse starts from nothing: the ranges of an earlier one never leak in. */
    CHECK(parse(&on, 11, ARGV("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank",
                              "--capture-xmv-entries", "dir", "--capture-xmv-entry-at", "0",
                              "--disc", "img.iso", "game.xbe")));
    CHECK(on.capture_xmv_range_count == 1u && on.capture_xmv_range_first[0] == 0u);
    /* Each half needs the other, and the capture needs the trace and a headless vblank policy. */
    CHECK(!parse(&on, 9, ARGV("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank",
                              "--capture-xmv-entries", "dir", "--disc", "img.iso", "game.xbe")));
    CHECK(!parse(&on, 9, ARGV("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank",
                              "--capture-xmv-entry-at", "2", "--disc", "img.iso", "game.xbe")));
    CHECK(!parse(&on, 10, ARGV("host", "--native-xmv", "--headless-first-vblank", "--capture-xmv-entries",
                               "dir", "--capture-xmv-entry-at", "2", "--disc", "img.iso", "game.xbe")));
    CHECK(!parse(&on, 10, ARGV("host", "--native-xmv", "--trace-xmv", "--capture-xmv-entries", "dir",
                               "--capture-xmv-entry-at", "2", "--disc", "img.iso", "game.xbe")));
    /* Malformed, descending, overlapping, empty or out of range lists fail. */
    char *const bad[] = {"", "x", "2,", ",2", "2-", "-2", "5-3", "3,3", "5,2", "2,1-4", "2x",
                               "99999999", "2--3", "2 ", "+2"};
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!parse(&on, 11, ARGV("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank",
                                   "--capture-xmv-entries", "dir", "--capture-xmv-entry-at", bad[i],
                                   "--disc", "img.iso", "game.xbe")));
    }
    /* 32 ranges are accepted, the 33rd is not. */
    char many[256];
    size_t used = 0u;
    for (unsigned n = 0u; n < 33u; n++) {
        used += (size_t)snprintf(many + used, sizeof many - used, "%s%u", n ? "," : "", n * 2u);
        if (n == 31u) {
            CHECK(parse(&on, 11, ARGV("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank",
                                      "--capture-xmv-entries", "dir", "--capture-xmv-entry-at", many,
                                      "--disc", "img.iso", "game.xbe")));
            CHECK(on.capture_xmv_range_count == 32u);
        }
    }
    CHECK(!parse(&on, 11, ARGV("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank",
                               "--capture-xmv-entries", "dir", "--capture-xmv-entry-at", many,
                               "--disc", "img.iso", "game.xbe")));
}

/* T611: the seeded wrapper run. The patch grammar is shared with tests/test_xmv_seeded_replay.py, which holds the
 * same valid and invalid vectors, so the C and the Python reader cannot drift apart unseen. */
/* Counts its own arguments, so a miscounted argc cannot read past the array. */
#define SEED_PARSE(out, list) parse((out), (int)(sizeof((char *[]){SEED_UNWRAP list}) / sizeof(char *)), (char *[]){SEED_UNWRAP list})
#define SEED_UNWRAP(...) __VA_ARGS__

static void test_xmv_seed_options(void)
{
    xmv_seed_patch patch;
    CHECK(xmv_seed_patch_parse("dec+0xA0=01", &patch) && patch.base == XMV_SEED_DEC && patch.offset == 0xA0u &&
          patch.length == 1u && patch.bytes[0] == 1u);
    CHECK(xmv_seed_patch_parse("esp+4=DEADbeef", &patch) && patch.base == XMV_SEED_ESP && patch.offset == 4u &&
          patch.length == 4u && patch.bytes[0] == 0xDEu && patch.bytes[3] == 0xEFu);
    CHECK(xmv_seed_patch_parse("dec@0x8+16=00ff", &patch) && patch.base == XMV_SEED_DEC_POINTER &&
          patch.pointer_offset == 8u && patch.offset == 16u && patch.length == 2u);
    CHECK(xmv_seed_patch_parse("dec+010=00", &patch) && patch.offset == 10u); /* decimal, not octal */
    char longest[8 + 2 * XMV_SEED_PATCH_BYTES + 1] = "dec+0=";
    memset(longest + 6, 'a', 2u * XMV_SEED_PATCH_BYTES);
    longest[6 + 2 * XMV_SEED_PATCH_BYTES] = '\0';
    CHECK(xmv_seed_patch_parse(longest, &patch) && patch.length == XMV_SEED_PATCH_BYTES);
    longest[6 + 2 * XMV_SEED_PATCH_BYTES] = 'a';
    longest[7 + 2 * XMV_SEED_PATCH_BYTES] = 'a';
    longest[8 + 2 * XMV_SEED_PATCH_BYTES] = '\0';
    CHECK(!xmv_seed_patch_parse(longest, &patch));
    char *const bad[] = {"", "dec+=01", "dec+0xA0", "dec+0xA0=", "dec+0xA0=0", "dec+0xA0=zz", "foo+1=00",
                         "dec@8=00", "dec@8+=00", "dec+0x10000000=00", "dec+-1=00", "dec+0x=00", "dec+ 1=00",
                         "dec+1=00 ", "esp=00", "DEC+1=00", "dec+1x=00", "dec@0x8+1x=00"};
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!xmv_seed_patch_parse(bad[i], &patch));
    }
    options on, defaults;
    CHECK(SEED_PARSE(&defaults, ("host", "game.xbe")));
    CHECK(!defaults.seed_xmv_entry_set && defaults.seed_xmv_result == NULL && defaults.seed_xmv_patch_count == 0u);
    CHECK(SEED_PARSE(&on, ("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank", "--seed-xmv-entry",
                              "2", "--seed-xmv-result", "out.bin", "--seed-xmv-patch", "dec+0xA1=01",
                              "--seed-xmv-patch", "esp+4=00", "--disc", "img.iso", "game.xbe")));
    CHECK(on.seed_xmv_entry_set && on.seed_xmv_entry == 2u && on.seed_xmv_patch_count == 2u);
    CHECK_STR(on.seed_xmv_result, "out.bin");
    CHECK_STR(on.seed_xmv_patch[1], "esp+4=00");
    /* No patch is a valid control run, and a later parse starts from nothing. */
    CHECK(SEED_PARSE(&on, ("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank", "--seed-xmv-entry",
                              "0", "--seed-xmv-result", "out.bin", "--disc", "img.iso", "game.xbe")));
    CHECK(on.seed_xmv_entry == 0u && on.seed_xmv_patch_count == 0u);
    /* The entry and the result file come together, patches need the entry, the run needs the trace and the seam. */
    CHECK(!SEED_PARSE(&on, ("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank", "--seed-xmv-entry",
                              "2", "--disc", "img.iso", "game.xbe")));
    CHECK(!SEED_PARSE(&on, ("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank",
                              "--seed-xmv-result", "out.bin", "--disc", "img.iso", "game.xbe")));
    CHECK(!SEED_PARSE(&on, ("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank",
                               "--seed-xmv-patch", "dec+0=01", "--disc", "img.iso", "game.xbe")));
    CHECK(!SEED_PARSE(&on, ("host", "--native-xmv", "--headless-first-vblank", "--seed-xmv-entry", "2",
                               "--seed-xmv-result", "out.bin", "--disc", "img.iso", "game.xbe")));
    CHECK(!SEED_PARSE(&on, ("host", "--native-xmv", "--trace-xmv", "--seed-xmv-entry", "2",
                               "--seed-xmv-result", "out.bin", "--disc", "img.iso", "game.xbe")));
    /* A malformed entry, patch or a missing value fails. */
    CHECK(!SEED_PARSE(&on, ("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank", "--seed-xmv-entry",
                               "x", "--seed-xmv-result", "out.bin", "--disc", "img.iso", "game.xbe")));
    CHECK(!SEED_PARSE(&on, ("host", "--native-xmv", "--trace-xmv", "--headless-first-vblank", "--seed-xmv-entry",
                               "2", "--seed-xmv-result", "out.bin", "--seed-xmv-patch", "dec+0=0", "--disc",
                               "img.iso", "game.xbe")));
    /* 32 patches are accepted, the 33rd is not. */
    char *args[100] = {"host", "--native-xmv", "--trace-xmv", "--headless-first-vblank", "--seed-xmv-entry", "2",
                      "--seed-xmv-result", "out.bin"};
    int count = 8;
    for (int n = 0; n < 33; n++) {
        args[count++] = "--seed-xmv-patch";
        args[count++] = "dec+0=01";
        if (n == 31) {
            char *tail[] = {"--disc", "img.iso", "game.xbe"};
            char *held[100];
            memcpy(held, args, sizeof args);
            memcpy(held + count, tail, sizeof tail);
            CHECK(parse(&on, count + 3, held));
            CHECK(on.seed_xmv_patch_count == 32u);
        }
    }
    args[count++] = "--disc";
    args[count++] = "img.iso";
    args[count++] = "game.xbe";
    CHECK(!parse(&on, count, args));
}

static void test_movie_test_aids_are_opt_in(void)
{
    options defaults, on;
    CHECK(parse(&defaults, 2, ARGV("host", "game.xbe")));
    CHECK(!defaults.overlay_consume && defaults.dump_xmv_frames == NULL && defaults.dump_xmv_frames_max == 0u &&
          defaults.xmv_substitute == NULL);
    CHECK(parse(&on, 7, ARGV("host", "--native-xmv", "--couple-vblank-effects", "--overlay-consume",
                            "--disc", "img.iso", "game.xbe")));
    CHECK(on.overlay_consume && on.native_xmv);
    CHECK(!parse(&on, 6, ARGV("host", "--native-xmv", "--overlay-consume", "--disc", "img.iso", "game.xbe")));
    CHECK(!parse(&on, 3, ARGV("host", "--overlay-consume", "game.xbe")));
    /* T537: the overlay picture dump, default off, a directory and an optional limit. */
    CHECK(defaults.dump_overlay == NULL && defaults.dump_overlay_max == 0u);
    CHECK(parse(&on, 4, ARGV("host", "--dump-overlay", "dir", "game.xbe")));
    CHECK(on.dump_overlay != NULL && strcmp(on.dump_overlay, "dir") == 0 && on.dump_overlay_max == 0u);
    CHECK(parse(&on, 6, ARGV("host", "--dump-overlay", "dir", "--dump-overlay-max", "8", "game.xbe")));
    CHECK(on.dump_overlay_max == 8u);
    CHECK(!parse(&on, 4, ARGV("host", "--dump-overlay-max", "8", "game.xbe")));
    CHECK(!parse(&on, 3, ARGV("host", "--dump-overlay", "game.xbe"))); /* the xbe is taken as the directory */
    CHECK(!parse(&on, 3, ARGV("host", "game.xbe", "--dump-overlay")));
    CHECK(!parse(&on, 6, ARGV("host", "--dump-overlay", "dir", "--dump-overlay-max", "8x", "game.xbe")));
    CHECK(!parse(&on, 6, ARGV("host", "--dump-overlay", "dir", "--dump-overlay-max", "-1", "game.xbe")));
    CHECK(!parse(&on, 6, ARGV("host", "--dump-overlay", "dir", "--dump-overlay-max", "1000001", "game.xbe")));
    /* T831: the xemu-level overlay options, each default off, independent of each other and of the dump. */
    CHECK(!defaults.overlay_xemu_image && !defaults.overlay_xemu_key);
    CHECK(parse(&on, 3, ARGV("host", "--overlay-xemu-image", "game.xbe")));
    CHECK(on.overlay_xemu_image && !on.overlay_xemu_key);
    CHECK(parse(&on, 3, ARGV("host", "--overlay-xemu-key", "game.xbe")));
    CHECK(!on.overlay_xemu_image && on.overlay_xemu_key);
    CHECK(parse(&on, 5, ARGV("host", "--overlay-xemu-key", "--dump-overlay", "dir", "game.xbe")));
    CHECK(on.overlay_xemu_key && on.dump_overlay != NULL && !on.overlay_xemu_image);
    CHECK(parse(&on, 4, ARGV("host", "--overlay-xemu-image", "--overlay-xemu-key", "game.xbe")));
    CHECK(on.overlay_xemu_image && on.overlay_xemu_key);
    CHECK(parse(&on, 10, ARGV("host", "--native-xmv", "--trace-xmv", "--dump-xmv-frames", "dir",
                              "--dump-xmv-frames-max", "12", "--disc", "img.iso", "game.xbe")));
    CHECK(on.dump_xmv_frames != NULL && strcmp(on.dump_xmv_frames, "dir") == 0 && on.dump_xmv_frames_max == 12u);
    CHECK(!parse(&on, 7, ARGV("host", "--native-xmv", "--dump-xmv-frames", "dir", "--disc", "img.iso", "game.xbe")));
    CHECK(!parse(&on, 4, ARGV("host", "--dump-xmv-frames", "dir", "game.xbe")));
    CHECK(!parse(&on, 4, ARGV("host", "--native-xmv", "--dump-xmv-frames", "--disc")));
    CHECK(!parse(&on, 10, ARGV("host", "--native-xmv", "--trace-xmv", "--dump-xmv-frames", "dir",
                               "--dump-xmv-frames-max", "12x", "--disc", "img.iso", "game.xbe")));
    CHECK(!parse(&on, 10, ARGV("host", "--native-xmv", "--trace-xmv", "--dump-xmv-frames", "dir",
                               "--dump-xmv-frames-max", "-1", "--disc", "img.iso", "game.xbe")));
    CHECK(parse(&on, 7, ARGV("host", "--native-xmv", "--xmv-substitute", "eag_e=frd", "--disc", "img.iso", "game.xbe")));
    CHECK(on.xmv_substitute != NULL && strcmp(on.xmv_substitute, "eag_e=frd") == 0);
    CHECK(!parse(&on, 4, ARGV("host", "--xmv-substitute", "eag_e=frd", "game.xbe")));
    /* T630: the console language setting, 1 to 9, default 0 (not given, the fabricated zero). */
    CHECK(parse(&on, 2, ARGV("host", "game.xbe")) && on.eeprom_language == 0u);
    CHECK(parse(&on, 4, ARGV("host", "--eeprom-language", "4", "game.xbe")) && on.eeprom_language == 4u);
    CHECK(parse(&on, 4, ARGV("host", "--eeprom-language", "9", "game.xbe")) && on.eeprom_language == 9u);
    CHECK(!parse(&on, 4, ARGV("host", "--eeprom-language", "0", "game.xbe")));
    CHECK(!parse(&on, 4, ARGV("host", "--eeprom-language", "10", "game.xbe")));
    CHECK(!parse(&on, 4, ARGV("host", "--eeprom-language", "x", "game.xbe")));
    CHECK(!parse(&on, 3, ARGV("host", "game.xbe", "--eeprom-language")));
}

/* T392: --headless-movie-audio is the bounded CPU model of the XMV movie stream. Default off, and
 * it needs both the retained XMV exports (which need a disc) and the startup stream policy. */
static void test_headless_movie_audio_is_opt_in_and_needs_xmv_and_streams(void)
{
    options defaults, enabled, other;
    CHECK(parse(&defaults, 2, ARGV("host", "game.xbe")));
    CHECK(defaults.headless_movie_audio == false);
    CHECK(parse(&enabled, 7, ARGV("host", "--headless-movie-audio", "--native-xmv", "--headless-streams",
                                  "--disc", "img.iso", "game.xbe")));
    CHECK(enabled.headless_movie_audio && enabled.native_xmv && enabled.headless_streams);
    /* It sets no other policy. */
    CHECK(enabled.ac97_ready && !enabled.headless_buffers && !enabled.headless_listener &&
          !enabled.trace_xmv && !enabled.headless_first_vblank && !enabled.headless_second_vblank);
    /* Each prerequisite is needed on its own: XMV, the stream policy, and (through XMV) a disc. */
    CHECK(!parse(&other, 3, ARGV("host", "--headless-movie-audio", "game.xbe")));
    CHECK(!parse(&other, 6, ARGV("host", "--headless-movie-audio", "--native-xmv", "--disc", "img.iso",
                                 "game.xbe")));
    CHECK(!parse(&other, 4, ARGV("host", "--headless-movie-audio", "--headless-streams", "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--headless-movie-audio", "--native-xmv", "--headless-streams",
                                 "game.xbe")));
    /* The prerequisites alone do not turn it on. */
    CHECK(parse(&other, 6, ARGV("host", "--native-xmv", "--headless-streams", "--disc", "img.iso", "game.xbe")));
    CHECK(other.native_xmv && other.headless_streams && !other.headless_movie_audio);
}

/* T681: --passive-audio-completion is the opt-in passive completion model. Default off, sets no other policy and
 * needs the three startup policies (streams, buffers, listener) it extends. */
static void test_passive_audio_completion_is_opt_in_and_needs_the_startup_policies(void)
{
    options defaults, enabled, other;
    CHECK(parse(&defaults, 2, ARGV("host", "game.xbe")));
    CHECK(defaults.passive_audio_completion == false && !defaults.passive_audio_completion_by_default);
    CHECK(parse(&enabled, 6, ARGV("host", "--passive-audio-completion", "--headless-streams", "--headless-buffers",
                                  "--headless-listener", "game.xbe")));
    CHECK(enabled.passive_audio_completion && enabled.headless_streams && enabled.headless_buffers &&
          enabled.headless_listener);
    CHECK(enabled.ac97_ready && !enabled.headless_movie_audio && !enabled.native_xmv);
    CHECK(!parse(&other, 3, ARGV("host", "--passive-audio-completion", "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--passive-audio-completion", "--headless-buffers", "--headless-listener",
                                 "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--passive-audio-completion", "--headless-streams", "--headless-listener",
                                 "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--passive-audio-completion", "--headless-streams", "--headless-buffers",
                                 "game.xbe")));
    /* T762: with the three policies it is a DEFAULT, --no-passive-audio-completion is the opt-out */
    CHECK(parse(&other, 5, ARGV("host", "--headless-streams", "--headless-buffers", "--headless-listener", "game.xbe")));
    CHECK(other.passive_audio_completion && other.passive_audio_completion_by_default);
    CHECK(enabled.passive_audio_completion && !enabled.passive_audio_completion_by_default);
    CHECK(parse(&other, 6, ARGV("host", "--no-passive-audio-completion", "--headless-streams", "--headless-buffers",
                                "--headless-listener", "game.xbe")));
    CHECK(!other.passive_audio_completion && !other.passive_audio_completion_by_default);
    /* T855 + T871: DoWork delivery is a mode of the model, DEFAULT on with it (xemu-level), never a policy of its own (with
     * the model off it is refused), --no-passive-audio-completion-at-dowork opts out. */
    CHECK(!defaults.passive_audio_completion && !defaults.passive_audio_completion_at_dowork);
    CHECK(enabled.passive_audio_completion && enabled.passive_audio_completion_at_dowork &&
          enabled.passive_audio_completion_at_dowork_by_default);
    CHECK(parse(&other, 6, ARGV("host", "--no-passive-audio-completion-at-dowork", "--headless-streams", "--headless-buffers",
                                "--headless-listener", "game.xbe")));
    CHECK(other.passive_audio_completion && !other.passive_audio_completion_at_dowork &&
          !other.passive_audio_completion_at_dowork_by_default);
    CHECK(parse(&other, 5, ARGV("host", "--headless-streams", "--headless-buffers", "--headless-listener", "game.xbe")));
    CHECK(other.passive_audio_completion_at_dowork && other.passive_audio_completion_at_dowork_by_default);
    CHECK(parse(&other, 7, ARGV("host", "--passive-audio-completion", "--passive-audio-completion-at-dowork", "--headless-streams",
                                "--headless-buffers", "--headless-listener", "game.xbe")));
    CHECK(other.passive_audio_completion && other.passive_audio_completion_at_dowork &&
          !other.passive_audio_completion_at_dowork_by_default);
    CHECK(parse(&other, 6, ARGV("host", "--passive-audio-completion-at-dowork", "--headless-streams", "--headless-buffers",
                                "--headless-listener", "game.xbe")));
    CHECK(other.passive_audio_completion && other.passive_audio_completion_at_dowork);
    CHECK(!parse(&other, 7, ARGV("host", "--no-passive-audio-completion", "--passive-audio-completion-at-dowork", "--headless-streams",
                                 "--headless-buffers", "--headless-listener", "game.xbe")));
    CHECK(!parse(&other, 3, ARGV("host", "--passive-audio-completion-at-dowork", "game.xbe")));
}

/* T743: --async-file-io is the opt-in overlapped read completion (XEMU-LEVEL timing). Default off, standalone, sets no other
 * policy. */
static void test_async_file_io_is_opt_in_and_standalone(void)
{
    options defaults, enabled;
    CHECK(parse(&defaults, 2, ARGV("host", "game.xbe")));
    /* T762: DEFAULT ON, --no-async-file-io is the opt-out */
    CHECK(defaults.async_file_io && defaults.async_file_io_by_default);
    CHECK(parse(&enabled, 3, ARGV("host", "--no-async-file-io", "game.xbe")));
    CHECK(!enabled.async_file_io && !enabled.async_file_io_by_default);
    CHECK(parse(&enabled, 3, ARGV("host", "--async-file-io", "game.xbe")));
    CHECK(enabled.async_file_io && !enabled.async_file_io_by_default);
    CHECK(!enabled.passive_audio_completion && !enabled.headless_streams && !enabled.headless_buffers &&
          !enabled.vblank_poll_blank && !enabled.synthetic_pad);
    /* T764: the file object reads are a second, separate opt-in. */
    CHECK(defaults.async_file_io_file_object == false);
    CHECK(!enabled.async_file_io_file_object);
    options file_object;
    CHECK(parse(&file_object, 4, ARGV("host", "--async-file-io", "--async-file-io-file-object", "game.xbe")));
    CHECK(file_object.async_file_io && file_object.async_file_io_file_object);
    options alone;
    /* T762: async reads are a default, so the file object reads stand alone, but not against the opt-out */
    CHECK(parse(&alone, 3, ARGV("host", "--async-file-io-file-object", "game.xbe")) && alone.async_file_io &&
          alone.async_file_io_file_object);
    CHECK(!parse(&alone, 4, ARGV("host", "--no-async-file-io", "--async-file-io-file-object", "game.xbe")));
}

/* T717: --synthetic-pad is FABRICATED and opt-in. Default off, sets no other policy. */
static void test_synthetic_pad_is_opt_in_and_standalone(void)
{
    options defaults, enabled;
    CHECK(parse(&defaults, 2, ARGV("host", "game.xbe")));
    CHECK(defaults.synthetic_pad == false);
    CHECK(parse(&enabled, 3, ARGV("host", "--synthetic-pad", "game.xbe")));
    CHECK(enabled.synthetic_pad);
    CHECK(!enabled.passive_audio_completion && !enabled.headless_streams && !enabled.headless_buffers);
    /* T731: the removal source defaults to never, needs the pad and a positive whole number. */
    CHECK(defaults.synthetic_pad_remove_after_polls == 0u);
    CHECK(parse(&enabled, 5, ARGV("host", "--synthetic-pad", "--synthetic-pad-remove-after-polls", "7", "game.xbe")));
    CHECK(enabled.synthetic_pad_remove_after_polls == 7u);
    CHECK(!parse(&enabled, 4, ARGV("host", "--synthetic-pad-remove-after-polls", "7", "game.xbe")));
    /* T707: the scripted source defaults to none, needs the pad and a path. */
    CHECK(defaults.pad_script == NULL);
    CHECK(parse(&enabled, 5, ARGV("host", "--synthetic-pad", "--pad-script", "in.txt", "game.xbe")));
    CHECK(enabled.pad_script != NULL && strcmp(enabled.pad_script, "in.txt") == 0);
    CHECK(!parse(&enabled, 4, ARGV("host", "--pad-script", "in.txt", "game.xbe")));
    /* T1222: --pad-script-live names a live pad file, needs the pad like --pad-script, and a later --pad-script turns it off. */
    CHECK(parse(&enabled, 5, ARGV("host", "--synthetic-pad", "--pad-script-live", "live.txt", "game.xbe")));
    CHECK(enabled.pad_script != NULL && strcmp(enabled.pad_script, "live.txt") == 0 && enabled.pad_script_live);
    CHECK(!parse(&enabled, 4, ARGV("host", "--pad-script-live", "live.txt", "game.xbe")));
    CHECK(parse(&enabled, 7, ARGV("host", "--synthetic-pad", "--pad-script-live", "live.txt", "--pad-script", "in.txt", "game.xbe")));
    CHECK(!enabled.pad_script_live);
    CHECK(parse(&enabled, 5, ARGV("host", "--synthetic-pad", "--pad-script", "in.txt", "game.xbe")));
    CHECK(!enabled.pad_script_live);
    CHECK(!parse(&enabled, 4, ARGV("host", "--synthetic-pad", "--pad-script", "game.xbe")));
    /* T751: --pad-source needs the pad, a known name, a feed for keyboard and gamepad, and excludes --pad-script. */
    CHECK(defaults.pad_source == NULL && defaults.pad_feed == NULL);
    CHECK(parse(&enabled, 7, ARGV("host", "--synthetic-pad", "--pad-source", "keyboard", "--pad-feed", "in.txt", "game.xbe")));
    CHECK(strcmp(enabled.pad_source, "keyboard") == 0 && strcmp(enabled.pad_feed, "in.txt") == 0);
    CHECK(parse(&enabled, 7, ARGV("host", "--synthetic-pad", "--pad-source", "gamepad", "--pad-feed", "in.txt", "game.xbe")));
    CHECK(parse(&enabled, 5, ARGV("host", "--synthetic-pad", "--pad-source", "keyboard", "game.xbe")));
    CHECK(parse(&enabled, 7, ARGV("host", "--synthetic-pad", "--pad-source", "script", "--pad-script", "in.txt", "game.xbe")));
    CHECK(!parse(&enabled, 5, ARGV("host", "--synthetic-pad", "--pad-source", "script", "game.xbe")));
    CHECK(!parse(&enabled, 5, ARGV("host", "--synthetic-pad", "--pad-source", "mouse", "game.xbe")));
    CHECK(!parse(&enabled, 4, ARGV("host", "--pad-source", "keyboard", "game.xbe")));
    CHECK(!parse(&enabled, 5, ARGV("host", "--synthetic-pad", "--pad-feed", "in.txt", "game.xbe")));
    CHECK(!parse(&enabled, 9, ARGV("host", "--synthetic-pad", "--pad-source", "keyboard", "--pad-script", "a.txt", "--pad-feed", "in.txt", "game.xbe")));
    CHECK(!parse(&enabled, 9, ARGV("host", "--synthetic-pad", "--pad-source", "script", "--pad-script", "a.txt", "--pad-feed", "in.txt", "game.xbe")));
    CHECK(!parse(&enabled, 4, ARGV("host", "--synthetic-pad", "--pad-source", "game.xbe")));
    /* T751: the window is the other provider, one provider only (window and feed together is refused). */
    CHECK(parse(&enabled, 7, ARGV("host", "--synthetic-pad", "--pad-source", "keyboard", "--present", "window", "game.xbe")));
    CHECK(parse(&enabled, 7, ARGV("host", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "game.xbe")));
    CHECK(!parse(&enabled, 9, ARGV("host", "--synthetic-pad", "--pad-source", "keyboard", "--present", "window", "--pad-feed", "in.txt", "game.xbe")));
    CHECK(parse(&enabled, 9, ARGV("host", "--synthetic-pad", "--pad-source", "keyboard", "--present", "null", "--pad-feed", "in.txt", "game.xbe")));
    CHECK(!parse(&enabled, 5, ARGV("host", "--synthetic-pad", "--synthetic-pad-remove-after-polls", "0", "game.xbe")));
    CHECK(!parse(&enabled, 5, ARGV("host", "--synthetic-pad", "--synthetic-pad-remove-after-polls", "-1", "game.xbe")));
    CHECK(!parse(&enabled, 5, ARGV("host", "--synthetic-pad", "--synthetic-pad-remove-after-polls", "4x", "game.xbe")));
    CHECK(!parse(&enabled, 4, ARGV("host", "--synthetic-pad", "--synthetic-pad-remove-after-polls", "game.xbe")));
}

/* T422: --profile-calls and its refinements. Default off, observation only. The cut and the watch
 * count through the profile, so each is refused without it. */
static void test_profile_calls_is_opt_in_and_its_refinements_need_it(void)
{
    options defaults, enabled, other;
    CHECK(parse(&defaults, 2, ARGV("host", "game.xbe")));
    CHECK(!defaults.profile_calls && !defaults.stop_after_set && defaults.profile_watch_count == 0u);
    CHECK(defaults.profile_calls_top == DEFAULT_PROFILE_TOP);
    CHECK(DEFAULT_PROFILE_TOP > 0u);
    CHECK(parse(&enabled, 9,
                ARGV("host", "--profile-calls", "--profile-calls-top", "7", "--stop-after-calls",
                     "0x3D8E50:2000", "--profile-watch", "0x563918", "game.xbe")));
    CHECK(enabled.profile_calls && enabled.profile_calls_top == 7u);
    CHECK(enabled.stop_after_set && !enabled.stop_after_ordinal);
    CHECK(enabled.stop_after_id == 0x3D8E50u && enabled.stop_after_count == 2000u);
    CHECK(enabled.profile_watch_count == 1u && enabled.profile_watch[0] == 0x563918u);
    /* It changes no policy. */
    CHECK(enabled.ac97_ready && !enabled.headless_first_vblank && !enabled.native_xmv);
    /* A kernel ordinal is spelled ord:K:N. */
    CHECK(parse(&other, 5, ARGV("host", "--profile-calls", "--stop-after-calls", "ord:21:5", "game.xbe")));
    CHECK(other.stop_after_set && other.stop_after_ordinal && other.stop_after_id == 21u &&
          other.stop_after_count == 5u);
    /* The cut and the watch are refused without the profile. */
    CHECK(!parse(&other, 4, ARGV("host", "--stop-after-calls", "0x3D8E50:2", "game.xbe")));
    CHECK(!parse(&other, 4, ARGV("host", "--profile-watch", "0x10", "game.xbe")));
    /* Malformed cuts and watches. */
    CHECK(!parse(&other, 5, ARGV("host", "--profile-calls", "--stop-after-calls", "0x3D8E50", "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--profile-calls", "--stop-after-calls", "0x3D8E50:0", "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--profile-calls", "--stop-after-calls", "zz:5", "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--profile-calls", "--stop-after-calls", "0x3D8E50:5x", "game.xbe")));
    CHECK(!parse(&other, 4, ARGV("host", "--profile-calls", "--stop-after-calls", "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--profile-calls", "--profile-watch", "nope", "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--profile-calls", "--profile-calls-top", "0", "game.xbe")));
    /* The watch list is bounded at eight, and the ninth is refused. */
    char *many[2 + 2 * 9 + 1];
    int argc = 0;
    many[argc++] = "host";
    many[argc++] = "--profile-calls";
    for (int index = 0; index < 8; index++) {
        many[argc++] = "--profile-watch";
        many[argc++] = "0x10";
    }
    many[argc++] = "game.xbe";
    CHECK(parse(&other, argc, many));
    CHECK(other.profile_watch_count == 8u);
    argc--;
    many[argc++] = "--profile-watch";
    many[argc++] = "0x10";
    many[argc++] = "game.xbe";
    CHECK(!parse(&other, argc, many));
}

/* argc counted from the list itself, so a miscount cannot read past the array */
#define PARSEV(out, ...) parse((out), (int)(sizeof((char *[]){__VA_ARGS__}) / sizeof(char *)), (char *[]){__VA_ARGS__})

/* T1616: the route replay options. A poke trigger needs the guarded poke, the route flags need a replay, and a
 * handover replay takes a live pad source while a plain replay still refuses one. */
static void test_route_options(void)
{
    options defaults, ok, other;
    CHECK(PARSEV(&defaults, "host", "game.xbe"));
    CHECK(!defaults.replay_handover && defaults.route_wait_count == 0u && defaults.poke_at_count == 0u);
    CHECK(PARSEV(&ok, "host", "--synthetic-pad", "--replay-input", "r", "--replay-handover", "--pad-source", "keyboard",
                 "--present", "window", "--route-wait", "mark2:mem=0x52D3FC==6,max=900", "--route-wait", "mark1:min=30",
                 "--forced-state", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "--poke-at-poll", "mark2:forced_b1",
                 "game.xbe"));
    CHECK(ok.replay_handover && ok.route_wait_count == 2u && ok.poke_at_count == 1u);
    CHECK(strcmp(ok.poke_at[0], "mark2:forced_b1") == 0 && strcmp(ok.route_waits[1], "mark1:min=30") == 0);
    /* a plain replay still refuses a pad source, a handover replay requires one */
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--pad-source", "keyboard", "--present", "window", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--replay-handover", "game.xbe"));
    CHECK(PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--replay-handover", "--pad-source", "gamepad", "--present", "window", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--replay-handover", "--pad-source", "keyboard", "--record-input", "o", "--present", "window", "game.xbe"));
    /* route flags without a replay are refused */
    CHECK(!PARSEV(&other, "host", "--replay-handover", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--route-wait", "mark1:min=3", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--forced-state", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "--poke-at-poll", "5:x", "game.xbe"));
    /* a poke trigger without --forced-state is refused (the guard stays) */
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "--poke-at-poll", "5:x", "game.xbe"));
    /* a plain replay may carry waits and a poke trigger */
    CHECK(PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--forced-state", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "--poke-at-poll", "replay-end:x", "game.xbe"));
    /* malformed specs and over the limits */
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--route-wait", "mark1", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--route-wait", "mark1:max=5", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--forced-state", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "--poke-at-poll", "0:x", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--route-wait"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--forced-state", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d",
                  "--poke-at-poll", "1:a", "--poke-at-poll", "2:b", "--poke-at-poll", "3:c", "--poke-at-poll", "4:d", "--poke-at-poll", "5:e", "game.xbe"));
    /* recording and the guarded poke do not mix: SIGUSR2 is the mark in a recording and the poke elsewhere */
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--record-input", "o", "--forced-state", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(PARSEV(&other, "host", "--synthetic-pad", "--record-input", "o", "game.xbe"));
}

/* T1627: host hotkeys. Default off, each spec validated at parse (the same grammar the host uses), the directory is required,
 * they ride the window pad feed only, at most four. The help text documents them with the markers the owner scripts grep. */
static void test_hotkey_options(void)
{
    options defaults, ok, other;
    CHECK(PARSEV(&defaults, "host", "game.xbe"));
    CHECK(defaults.hotkey_count == 0u && defaults.hotkey_dir == NULL);
    CHECK(PARSEV(&ok, "host", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "--hotkey",
                 "pad=BACK+START+LB+RB@500:advance", "--hotkey", "kb=CTRL+SHIFT+N:advance", "--hotkey-dir", "d/hk", "game.xbe"));
    CHECK(ok.hotkey_count == 2u && strcmp(ok.hotkeys[0], "pad=BACK+START+LB+RB@500:advance") == 0 &&
          strcmp(ok.hotkeys[1], "kb=CTRL+SHIFT+N:advance") == 0 && strcmp(ok.hotkey_dir, "d/hk") == 0);
    CHECK(PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "keyboard", "--present", "window", "--hotkey",
                 "kb=CTRL+SHIFT+N:advance", "--hotkey-dir", "d", "game.xbe"));
    /* the route handover run of the forced weapons script: replay, handover to the live gamepad, hotkeys */
    CHECK(PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--replay-handover", "--pad-source", "gamepad",
                 "--present", "window", "--hotkey", "pad=BACK+START:advance", "--hotkey-dir", "d", "game.xbe"));
    /* a hotkey needs its directory, and a directory alone is refused */
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "--hotkey", "pad=BACK+START:advance", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "--hotkey-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--hotkey-dir", "d", "game.xbe"));
    /* a hotkey needs a keyboard or gamepad window source: none, a script, a fake feed file, a plain replay are refused */
    CHECK(!PARSEV(&other, "host", "--hotkey", "pad=BACK+START:advance", "--hotkey-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "gamepad", "--pad-feed", "f", "--hotkey", "pad=BACK+START:advance", "--hotkey-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-script", "s", "--hotkey", "pad=BACK+START:advance", "--hotkey-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--hotkey", "pad=BACK+START:advance", "--hotkey-dir", "d", "game.xbe"));
    /* a malformed spec, a missing value, a fifth hotkey */
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "--hotkey", "pad=BACK:advance", "--hotkey-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "--hotkey", "pad=BACK+NOPE:advance", "--hotkey-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "--hotkey-dir", "d", "--hotkey"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "--hotkey-dir"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "--hotkey-dir", "", "game.xbe"));
    /* T1632: eight hotkeys (the four pad and four keyboard default chords) fit, a ninth is refused */
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "--hotkey-dir", "d",
                  "--hotkey", "pad=A+B:a", "--hotkey", "pad=A+X:b", "--hotkey", "pad=A+Y:c", "--hotkey", "pad=B+X:d", "--hotkey", "pad=B+Y:e",
                  "--hotkey", "pad=X+Y:f", "--hotkey", "pad=LB+RB:g", "--hotkey", "pad=LB+A:h", "--hotkey", "pad=RB+A:i", "game.xbe"));
    CHECK(PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "--hotkey-dir", "d",
                 "--hotkey", XINPUT_HOTKEY_DEFAULT_PAD_ADVANCE, "--hotkey", XINPUT_HOTKEY_DEFAULT_PAD_MARK,
                 "--hotkey", XINPUT_HOTKEY_DEFAULT_PAD_DUMP, "--hotkey", XINPUT_HOTKEY_DEFAULT_PAD_STOP,
                 "--hotkey", XINPUT_HOTKEY_DEFAULT_KB_ADVANCE, "--hotkey", XINPUT_HOTKEY_DEFAULT_KB_MARK,
                 "--hotkey", XINPUT_HOTKEY_DEFAULT_KB_DUMP, "--hotkey", XINPUT_HOTKEY_DEFAULT_KB_STOP, "--record-input", "r.txt", "game.xbe"));
    CHECK(other.hotkey_count == 8u);
    /* T1720b: --shot-dir, --shot-max, --shot-max-bytes. Defaults, parse, bounds, they need --hotkey */
    CHECK(defaults.shot_dir == NULL && defaults.shot_max == 200u && defaults.shot_max_bytes == 536870912ull);
    CHECK(PARSEV(&ok, "host", "--synthetic-pad", "--pad-source", "keyboard", "--present", "window", "--hotkey", "kb=CTRL+SHIFT+S:shot",
                 "--hotkey-dir", "d/hk", "--shot-dir", "d/shots", "--shot-max", "7", "--shot-max-bytes", "123456", "game.xbe"));
    CHECK(strcmp(ok.shot_dir, "d/shots") == 0 && strcmp(ok.hotkey_dir, "d/hk") == 0 && ok.shot_max == 7u && ok.shot_max_bytes == 123456ull);
    CHECK(PARSEV(&ok, "host", "--synthetic-pad", "--pad-source", "keyboard", "--present", "window", "--hotkey", "kb=CTRL+SHIFT+S:shot",
                 "--hotkey-dir", "d/hk", "--shot-max", "100000", "game.xbe") && ok.shot_max == 100000u && ok.shot_dir == NULL);
    CHECK(PARSEV(&ok, "host", "--synthetic-pad", "--pad-source", "keyboard", "--present", "window", "--hotkey", "kb=CTRL+SHIFT+S:shot",
                 "--hotkey-dir", "d/hk", "--shot-max", "1", "--shot-max-bytes", "1", "game.xbe") && ok.shot_max == 1u && ok.shot_max_bytes == 1ull);
    char *bad_values[][2] = {{"--shot-max", "0"},       {"--shot-max", "100001"}, {"--shot-max", "-1"},  {"--shot-max", "x"},
                                   {"--shot-max", ""},        {"--shot-max", "3x"},     {"--shot-max-bytes", "0"}, {"--shot-max-bytes", "-5"},
                                   {"--shot-max-bytes", "y"}, {"--shot-max-bytes", ""}, {"--shot-dir", ""}};
    for (unsigned i = 0u; i < sizeof bad_values / sizeof bad_values[0]; i++)
        CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "keyboard", "--present", "window", "--hotkey", "kb=CTRL+SHIFT+S:shot",
                      "--hotkey-dir", "d/hk", bad_values[i][0], bad_values[i][1], "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "keyboard", "--present", "window", "--hotkey", "kb=CTRL+SHIFT+S:shot",
                  "--hotkey-dir", "d/hk", "--shot-dir"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "keyboard", "--present", "window", "--hotkey", "kb=CTRL+SHIFT+S:shot",
                  "--hotkey-dir", "d/hk", "--shot-max"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--pad-source", "keyboard", "--present", "window", "--hotkey", "kb=CTRL+SHIFT+S:shot",
                  "--hotkey-dir", "d/hk", "--shot-max-bytes"));
    /* the three flags without any --hotkey are refused */
    CHECK(!PARSEV(&other, "host", "--shot-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--shot-max", "5", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--shot-max-bytes", "5", "game.xbe"));
}

static void test_hotkey_help_text(void)
{
    const char *help = host_options_hotkey_help();
    CHECK(help != NULL);
    if (help == NULL) return;
    CHECK(strstr(help, "T1627") != NULL);
    CHECK(strstr(help, "--hotkey SPEC") != NULL);
    CHECK(strstr(help, "--hotkey-dir DIR") != NULL);
    CHECK(strstr(help, "T1720") != NULL && strstr(help, "shots.manifest") != NULL); /* the marker for the shot label */
    CHECK(strstr(help, "T1720b") != NULL && strstr(help, "--shot-dir DIR") != NULL && strstr(help, "--shot-max N") != NULL &&
          strstr(help, "--shot-max-bytes N") != NULL && strstr(help, "unix=EPOCH bytes=PNGBYTES") != NULL); /* the marker for the shot bounds */
    CHECK(strstr(help, "T1632") != NULL); /* the marker the owner scripts grep for (a host built before T1632 lacks it) */
    CHECK(strstr(help, "LEAD") != NULL && strstr(help, "swallowed") != NULL);
    CHECK(strstr(help, "'mark'") != NULL && strstr(help, "'stop'") != NULL && strstr(help, "SIGUSR2") != NULL);
    CHECK(strstr(help, "taken out of the recorded route") != NULL);
    CHECK(strstr(help, "most 8") != NULL);
    CHECK(strstr(help, "game window has focus") != NULL);
    /* the two example specs in the text are accepted by the parser they document */
    CHECK(strstr(help, "--hotkey pad=BACK+START+LB+RB@500:advance --hotkey kb=CTRL+SHIFT+N:advance") != NULL);
    const char *example[] = {"prog", "--synthetic-pad", "--pad-source", "gamepad", "--present", "window", "--hotkey",
                             "pad=BACK+START+LB+RB@500:advance", "--hotkey", "kb=CTRL+SHIFT+N:advance", "--hotkey-dir", "d", "game.xbe"};
    options parsed;
    CHECK(parse(&parsed, 13, (char **)example));
    CHECK(parsed.hotkey_count == 2u);
    /* the default chords the help names are the ones in the header (labels and keys in the order the text lists them) */
    CHECK(strstr(help, "BACK+START+LB+RB 0.5 s advance") != NULL && strstr(help, "+X 0.5 s mark") != NULL);
    CHECK(strstr(help, "+B 0.5 s dump") != NULL && strstr(help, "+Y 1.5 s stop") != NULL);
    CHECK(strstr(XINPUT_HOTKEY_DEFAULT_PAD_MARK, "BACK+START+LB+X@500:mark") != NULL);
    CHECK(strstr(XINPUT_HOTKEY_DEFAULT_PAD_STOP, "BACK+START+LB+Y@1500:stop") != NULL);
}

/* T1599: the read-only guest dump options. Default off, bounded, a directory is required, malformed and wrapping ranges refused. */
static void test_dump_guest_range_options(void)
{
    options defaults, ok, other;
    CHECK(PARSEV(&defaults, "host", "game.xbe"));
    CHECK(defaults.dump_guest_set.count == 0u && defaults.dump_guest_dir == NULL);
    CHECK(defaults.dump_guest_max_bytes == GUEST_DUMP_DEFAULT_MAX_BYTES);
    CHECK(PARSEV(&ok, "host", "--dump-guest-range", "0x52D3F4:0x30,0x7844A8:0x400", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(ok.dump_guest_set.count == 2u && ok.dump_guest_set.total_bytes == 0x430u);
    CHECK(ok.dump_guest_set.ranges[0].address == 0x52D3F4u && ok.dump_guest_set.ranges[0].length == 0x30u);
    CHECK(ok.dump_guest_set.ranges[1].address == 0x7844A8u && ok.dump_guest_set.ranges[1].length == 0x400u);
    /* repeating the flag accumulates */
    CHECK(PARSEV(&ok, "host", "--dump-guest-range", "0x10:4", "--dump-guest-range", "20:4", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(ok.dump_guest_set.count == 2u && ok.dump_guest_set.ranges[1].address == 20u);
    /* the last byte of the address space is accepted, one more is not */
    CHECK(PARSEV(&ok, "host", "--dump-guest-range", "0x3FFFFFF:1", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x3FFFFFF:2", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x4000000:1", "--dump-guest-dir", "d", "game.xbe"));
    /* a 32-bit wrapping sum is refused */
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x10:0xFFFFFFF8", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0xFFFFFFF0:0x20", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x10:0x100000000", "--dump-guest-dir", "d", "game.xbe"));
    /* malformed */
    CHECK(PARSEV(&ok, "host", "--dump-guest-range", "*0x7844A8+0:0x400,0x7844A8:4", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(ok.dump_guest_set.count == 2u && ok.dump_guest_set.ranges[0].indirect && ok.dump_guest_set.total_bytes == 0x404u);
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "*0x10+0x3FFFFFF:2", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "*0x10", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x10", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x10:0", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x10:4,", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "-1:4", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "zz:4", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x10:4x", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-dir", "d", "game.xbe", "--dump-guest-range"));
    /* T1613: --forced-state defaults off, is accepted only with a dump range, and sets the flag */
    CHECK(!defaults.forced_state);
    CHECK(!ok.forced_state);
    CHECK(PARSEV(&ok, "host", "--forced-state", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(ok.forced_state);
    CHECK(!PARSEV(&other, "host", "--forced-state", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--forced-state", "--dump-guest-dir", "d", "game.xbe"));
    /* a dump needs its directory, a directory alone is refused */
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x10:4", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-dir", "d", "game.xbe"));
    /* the total is bounded by --dump-guest-max-bytes, in either flag order */
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x10:0x100001", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(PARSEV(&ok, "host", "--dump-guest-range", "0x10:0x100000", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(PARSEV(&ok, "host", "--dump-guest-range", "0x10:0x200000", "--dump-guest-dir", "d", "--dump-guest-max-bytes", "0x200000", "game.xbe"));
    CHECK(PARSEV(&ok, "host", "--dump-guest-max-bytes", "0x200000", "--dump-guest-range", "0x10:0x200000", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x10:0x200001", "--dump-guest-dir", "d", "--dump-guest-max-bytes", "0x200000", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-max-bytes", "0", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-max-bytes", "0x4000001", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "game.xbe"));
    /* 17 ranges are refused (the sixteenth is the last) */
    CHECK(PARSEV(&ok, "host", "--dump-guest-range", "1:1,2:1,3:1,4:1,5:1,6:1,7:1,8:1,9:1,10:1,11:1,12:1,13:1,14:1,15:1,16:1", "--dump-guest-dir", "d", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "1:1,2:1,3:1,4:1,5:1,6:1,7:1,8:1,9:1,10:1,11:1,12:1,13:1,14:1,15:1,16:1,17:1", "--dump-guest-dir", "d", "game.xbe"));
}

/* T821: the read-only indirect call census. Default off, a window of ordered calls needs it, malformed windows are refused. */
static void test_census_icalls_is_opt_in_and_its_window_needs_it(void)
{
    options defaults, enabled, windowed, other;
    CHECK(parse(&defaults, 2, ARGV("host", "game.xbe")));
    CHECK(!defaults.census_icalls && !defaults.census_window_set);
    CHECK(parse(&enabled, 3, ARGV("host", "--census-icalls", "game.xbe")));
    CHECK(enabled.census_icalls && !enabled.census_window_set);
    CHECK(parse(&windowed, 5, ARGV("host", "--census-icalls", "--census-window", "3070:3074", "game.xbe")));
    CHECK(windowed.census_icalls && windowed.census_window_set);
    CHECK(windowed.census_window_first == 3070u && windowed.census_window_last == 3074u);
    /* a one present window and a hex bound */
    CHECK(parse(&other, 5, ARGV("host", "--census-icalls", "--census-window", "0x10:0x10", "game.xbe")));
    CHECK(other.census_window_first == 16u && other.census_window_last == 16u);
    /* it needs no --profile-calls and turns nothing else on */
    CHECK(!windowed.profile_calls && windowed.profile_watch_count == 0u);
    CHECK(!parse(&other, 4, ARGV("host", "--census-window", "1:2", "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--census-icalls", "--census-window", "5", "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--census-icalls", "--census-window", "5:4", "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--census-icalls", "--census-window", "a:4", "game.xbe")));
    CHECK(!parse(&other, 5, ARGV("host", "--census-icalls", "--census-window", "4:", "game.xbe")));
    CHECK(!parse(&other, 4, ARGV("host", "--census-icalls", "--census-window", "game.xbe")));
    /* T1502: --census-phases FILE needs --census-icalls and a value */
    CHECK(parse(&other, 5, ARGV("host", "--census-icalls", "--census-phases", "c.txt", "game.xbe")));
    CHECK(other.census_phases != NULL && strcmp(other.census_phases, "c.txt") == 0);
    CHECK(!parse(&other, 4, ARGV("host", "--census-phases", "c.txt", "game.xbe")));
    CHECK(!defaults.census_phases);
}

/* T821: --async-file-io-spin-complete N (INFERRED, opt-in). Needs the async model (default on) and a headless vblank policy. */
static void test_async_file_io_spin_complete_is_opt_in_and_validated(void)
{
    options defaults, enabled, other;
    CHECK(parse(&defaults, 2, ARGV("host", "game.xbe")));
    CHECK(defaults.async_file_io_spin == 0u);
    CHECK(parse(&enabled, 5,
                ARGV("host", "--headless-first-vblank", "--async-file-io-spin-complete", "2048", "game.xbe")));
    CHECK(enabled.async_file_io_spin == 2048u && enabled.async_file_io);
    CHECK(!parse(&other, 6,
                 ARGV("host", "--headless-first-vblank", "--no-async-file-io", "--async-file-io-spin-complete", "2048",
                      "game.xbe")));
    /* no cooperative provider without a headless vblank policy, no async model with --no-async-file-io */
    CHECK(!parse(&other, 4, ARGV("host", "--async-file-io-spin-complete", "2048", "game.xbe")));
    /* malformed or zero thresholds */
    CHECK(!parse(&other, 4, ARGV("host", "--async-file-io-spin-complete", "0", "game.xbe")));
    CHECK(!parse(&other, 4, ARGV("host", "--async-file-io-spin-complete", "x", "game.xbe")));
    CHECK(!parse(&other, 3, ARGV("host", "--async-file-io-spin-complete")));
}

/* T156: --headless-audio is a pure spelling of the four audio policies. It must set exactly
 * the fields the individual flags set, nothing timing related, and change no default. */
static void test_headless_audio_umbrella_is_a_pure_convenience(void)
{
    options umbrella, explicit_flags, defaults;
    CHECK(parse(&defaults, 2, ARGV("host", "game.xbe")));
    CHECK(parse(&umbrella, 3, ARGV("host", "--headless-audio", "game.xbe")));
    CHECK(parse(&explicit_flags, 7,
                ARGV("host", "--ac97-ready", "--headless-effects", "--headless-streams",
                     "--headless-buffers", "--headless-listener", "game.xbe")));
    /* Non-vacuous first: the explicit spelling really turned the four policies on. */
    CHECK(explicit_flags.ac97_ready && explicit_flags.headless_effects);
    CHECK(explicit_flags.headless_streams && explicit_flags.headless_buffers);
    CHECK(explicit_flags.headless_listener);
    CHECK(umbrella.ac97_ready == explicit_flags.ac97_ready);
    CHECK(umbrella.headless_effects == explicit_flags.headless_effects);
    CHECK(umbrella.headless_streams == explicit_flags.headless_streams);
    CHECK(umbrella.headless_buffers == explicit_flags.headless_buffers);
    CHECK(umbrella.headless_listener == explicit_flags.headless_listener);
    /* Not timing, not the shader compiler, not any other default. */
    CHECK(!umbrella.headless_first_vblank && !umbrella.headless_second_vblank);
    CHECK(!umbrella.native_shader_assembler && !umbrella.continue_on_missing);
    CHECK(umbrella.open_missing_as_empty == defaults.open_missing_as_empty);
    CHECK(umbrella.hdd_path == defaults.hdd_path && umbrella.disc_path == defaults.disc_path);
    CHECK(umbrella.cache_partitions == defaults.cache_partitions);
    /* Default unchanged: only the flag, never the absence of it, turns anything on. */
    CHECK(defaults.ac97_ready && !defaults.headless_streams);
    CHECK(!defaults.headless_buffers && !defaults.headless_listener);
    /* Position independent, repeatable, and needs the xbe path like any option. */
    options late;
    CHECK(parse(&late, 4, ARGV("host", "game.xbe", "--headless-audio", "--headless-audio")));
    CHECK(late.ac97_ready && late.headless_streams && late.headless_buffers && late.headless_listener);
    CHECK(!parse(&late, 2, ARGV("host", "--headless-audio")));
    /* The reason it exists: second vblank demands all five policies. The vblank policies
     * stay explicit, so the pair is accepted but neither is implied. */
    options vblank;
    CHECK(parse(&vblank, 4, ARGV("host", "--headless-audio", "--headless-second-vblank", "game.xbe")));
    CHECK(vblank.headless_second_vblank && !vblank.headless_first_vblank);
    CHECK(parse(&vblank, 4, ARGV("host", "--headless-audio", "--headless-first-vblank", "game.xbe")));
    CHECK(vblank.headless_first_vblank && !vblank.headless_second_vblank);
    CHECK(!parse(&vblank, 5, ARGV("host", "--headless-audio", "--headless-first-vblank",
                                  "--headless-second-vblank", "game.xbe")));
}

static void test_each_valued_flag_takes_the_following_argv_slot(void)
{
    options opt;

    CHECK(parse(&opt, 4, ARGV("h", "--hdd", "/tmp/hdd", "g.xbe")));
    CHECK_STR(opt.hdd_path, "/tmp/hdd");
    /* And the device default SURVIVES: --hdd alone must not also change where it mounts. */
    CHECK_STR(opt.hdd_device, "\\Device\\Harddisk0\\partition1");

    CHECK(parse(&opt, 4, ARGV("h", "--disc", "a.iso", "g.xbe")));
    CHECK_STR(opt.disc_path, "a.iso");
    CHECK_STR(opt.disc_device, "\\Device\\CdRom0");

    CHECK(parse(&opt, 4, ARGV("h", "--disc-device", "\\Device\\Cdrom1", "g.xbe")));
    CHECK_STR(opt.disc_device, "\\Device\\Cdrom1");

    CHECK(parse(&opt, 4, ARGV("h", "--hdd-device", "\\Device\\Harddisk0\\partition2",
                              "g.xbe")));
    CHECK_STR(opt.hdd_device, "\\Device\\Harddisk0\\partition2");

    CHECK(parse(&opt, 4, ARGV("h", "--trace", "7", "g.xbe")));
    CHECK(opt.trace_report == 7u);

    CHECK(parse(&opt, 4, ARGV("h", "--thread-timeout", "250", "g.xbe")));
    CHECK(opt.thread_timeout_ms == 250u);

    /* --stub-status sets a FLAG as well as a value, because 0 is a legitimate status
     * and "not given" must stay distinguishable from "given as zero". */
    CHECK(parse(&opt, 4, ARGV("h", "--stub-status", "0", "g.xbe")));
    CHECK(opt.stub_status == 0u);
    CHECK(opt.stub_status_set == true);

    /* strtoul with base 0, so 0x... is hex. */
    CHECK(parse(&opt, 4, ARGV("h", "--stub-status", "0xC0000034", "g.xbe")));
    CHECK(opt.stub_status == 0xC0000034u);
}

static void test_audio_output_path_is_opt_in(void)
{
    options opt;
    CHECK(parse(&opt, 6, ARGV("host", "--audio-sink", "wav-file", "--audio-output", "capture.wav", "game.xbe")));
    CHECK_STR(opt.audio_sink, "wav-file");
    CHECK_STR(opt.audio_output, "capture.wav");
    CHECK(!parse(&opt, 4, ARGV("host", "--audio-output", "capture.wav", "game.xbe")));
    CHECK(!parse(&opt, 6, ARGV("host", "--audio-sink", "sdl", "--audio-output", "capture.wav", "game.xbe")));
}

static void test_audio_mute_needs_the_sdl_sink(void)
{
    options opt;
    CHECK(parse(&opt, 5, ARGV("host", "--audio-sink", "sdl", "--audio-mute", "game.xbe")));
    CHECK(opt.audio_mute);
    CHECK(parse(&opt, 4, ARGV("host", "--audio-sink", "sdl", "game.xbe")) && !opt.audio_mute);
    CHECK(!parse(&opt, 3, ARGV("host", "--audio-mute", "game.xbe")));
    CHECK(!parse(&opt, 5, ARGV("host", "--audio-sink", "null", "--audio-mute", "game.xbe")));
}

static void test_cache_partitions_parses_and_is_bounded(void)
{
    options opt;
    CHECK(parse(&opt, 4, ARGV("h", "--cache-partitions", "1", "g.xbe")));
    CHECK(opt.cache_partitions == 1u);
    /* 0 is ALLOWED (an operator may want to reproduce the memmove fault), and is distinct
     * from "not given", which is 3. */
    CHECK(parse(&opt, 4, ARGV("h", "--cache-partitions", "0", "g.xbe")));
    CHECK(opt.cache_partitions == 0u);
    /* The bound itself parses; one past it and a negative are refused, so a corrupt value can
     * never become a plausible one. */
    CHECK(parse(&opt, 4, ARGV("h", "--cache-partitions", "8", "g.xbe")));
    CHECK(opt.cache_partitions == 8u);
    CHECK(!parse(&opt, 4, ARGV("h", "--cache-partitions", "9", "g.xbe")));
    CHECK(!parse(&opt, 4, ARGV("h", "--cache-partitions", "-1", "g.xbe")));
    /* With the path already given, so only the flag arm can produce the refusal. */
    CHECK(!parse(&opt, 3, ARGV("h", "g.xbe", "--cache-partitions")));
}

static void test_the_xbe_path_is_the_one_positional_argument(void)
{
    options opt;
    /* Order must not matter: flags before or after the path. */
    CHECK(parse(&opt, 4, ARGV("h", "g.xbe", "--hdd", "/tmp/x")));
    CHECK_STR(opt.xbe_path, "g.xbe");
    CHECK_STR(opt.hdd_path, "/tmp/x");

    /* A SECOND positional is refused rather than silently ignored or overwriting the
     * first. Two paths means the operator meant something we cannot guess. */
    CHECK(!parse(&opt, 3, ARGV("h", "a.xbe", "b.xbe")));

    /* No path at all is refused even when flags parse cleanly. */
    CHECK(!parse(&opt, 2, ARGV("h", "--ac97-ready")));
    CHECK(!parse(&opt, 1, ARGV("h")));
}

/* --- the refusals ---------------------------------------------------------- */

static void test_a_flag_whose_argument_is_missing_is_refused(void)
{
    options opt;
    /* Each of these ends argv on a flag that needs a value. The parser must REFUSE
     * rather than read past the end of argv or fall back to a default the operator did
     * not ask for -- the second is worse, because the run proceeds and looks fine. */
    CHECK(!parse(&opt, 2, ARGV("h", "--hdd")));
    CHECK(!parse(&opt, 2, ARGV("h", "--hdd-device")));
    CHECK(!parse(&opt, 2, ARGV("h", "--disc")));
    CHECK(!parse(&opt, 2, ARGV("h", "--disc-device")));
    CHECK(!parse(&opt, 2, ARGV("h", "--mount")));
    CHECK(!parse(&opt, 2, ARGV("h", "--stub-status")));
    CHECK(!parse(&opt, 2, ARGV("h", "--thread-timeout")));
    CHECK(!parse(&opt, 2, ARGV("h", "--trace")));

    /*
     * AND THE SAME CASES WITH A VALID XBE PATH ALREADY GIVEN.
     *
     * The block above passed for the WRONG REASON and a mutation proved it. With only
     * `--disc` in argv there is no positional either, so `parse_options` returns false on
     * the final `xbe_path != NULL` check no matter what the flag arm did. Replacing the
     * refusal with `continue` left every assertion above still passing.
     *
     * Putting the path FIRST removes that second reason to fail, so these assertions can
     * only be satisfied by the flag arm actually refusing. Same trap as asserting two
     * empty lists are equal: a negative that would hold anyway tests nothing.
     */
    CHECK(!parse(&opt, 3, ARGV("h", "g.xbe", "--hdd")));
    CHECK(!parse(&opt, 3, ARGV("h", "g.xbe", "--hdd-device")));
    CHECK(!parse(&opt, 3, ARGV("h", "g.xbe", "--disc")));
    CHECK(!parse(&opt, 3, ARGV("h", "g.xbe", "--disc-device")));
    CHECK(!parse(&opt, 3, ARGV("h", "g.xbe", "--mount")));
    CHECK(!parse(&opt, 3, ARGV("h", "g.xbe", "--stub-status")));
    CHECK(!parse(&opt, 3, ARGV("h", "g.xbe", "--thread-timeout")));
    CHECK(!parse(&opt, 3, ARGV("h", "g.xbe", "--trace")));

    /* And the control: the same shape WITH the argument present must parse, so the
     * refusals above are about the missing argument and not about argument order. */
    CHECK(parse(&opt, 4, ARGV("h", "g.xbe", "--disc", "a.iso")));
    CHECK_STR(opt.disc_path, "a.iso");
}

static void test_an_unknown_flag_is_refused(void)
{
    options opt;
    CHECK(!parse(&opt, 3, ARGV("h", "--not-a-flag", "g.xbe")));
    /* A near-miss on a real flag must not be silently accepted as the real one. */
    CHECK(!parse(&opt, 4, ARGV("h", "--hdd-dev", "/tmp/x", "g.xbe")));
    /* A lone dash is a flag shape, so it is refused rather than taken as a path. */
    CHECK(!parse(&opt, 3, ARGV("h", "-", "g.xbe")));
}

static void test_out_of_range_numbers_are_refused(void)
{
    options opt;
    /* A NEGATIVE timeout would wrap to an enormous unsigned and turn the watchdog off,
     * so a hang would be waited on forever instead of reported. */
    CHECK(!parse(&opt, 4, ARGV("h", "--thread-timeout", "-1", "g.xbe")));
    /* Zero is ALLOWED and means do not wait at all, per the help text. */
    CHECK(parse(&opt, 4, ARGV("h", "--thread-timeout", "0", "g.xbe")));
    CHECK(opt.thread_timeout_ms == 0u);

    /* A trace of 0 or less would print nothing, which is indistinguishable from a run
     * that made no calls. Refused. */
    CHECK(!parse(&opt, 4, ARGV("h", "--trace", "0", "g.xbe")));
    CHECK(!parse(&opt, 4, ARGV("h", "--trace", "-5", "g.xbe")));
}

static void test_mounts_accumulate_in_order_and_the_bound_is_refused(void)
{
    options opt;
    CHECK(parse(&opt, 6, ARGV("h", "--mount", "A:", "--mount", "B:", "g.xbe")));
    CHECK(opt.mount_count == 2u);
    /* ORDER IS PRESERVED. The mount table is longest-prefix matched, so two names where
     * one is a prefix of the other resolve differently depending on order. */
    CHECK_STR(opt.mounts[0], "A:");
    CHECK_STR(opt.mounts[1], "B:");

    /* One past the bound is REFUSED, not truncated. A dropped mount would present as a
     * missing file, which reads as a content problem rather than an argument one. */
    char *over[2 + 2 * (OPTION_MOUNT_MAX + 1u)];
    int argc = 0;
    over[argc++] = (char *)"h";
    for (unsigned i = 0u; i < OPTION_MOUNT_MAX + 1u; i++) {
        over[argc++] = (char *)"--mount";
        over[argc++] = (char *)"X:";
    }
    over[argc++] = (char *)"g.xbe";
    CHECK(!parse_options(argc, over, &opt));

    /* Exactly at the bound still parses, so the refusal above is about the bound and not
     * about mounts in general. Asserting the positive is what makes the negative mean
     * something. */
    int ok_argc = 0;
    char *ok[2 + 2 * OPTION_MOUNT_MAX];
    ok[ok_argc++] = (char *)"h";
    for (unsigned i = 0u; i < OPTION_MOUNT_MAX; i++) {
        ok[ok_argc++] = (char *)"--mount";
        ok[ok_argc++] = (char *)"X:";
    }
    ok[ok_argc++] = (char *)"g.xbe";
    CHECK(parse_options(ok_argc, ok, &opt));
    CHECK(opt.mount_count == OPTION_MOUNT_MAX);
}

static void test_a_refused_parse_still_leaves_a_fully_initialised_struct(void)
{
    /* The struct is poisoned before every parse. If a caller inspected `out` after a
     * false return, it must see the defaults rather than poison -- `parse_options` sets
     * every field before reading any argument, and this pins that. */
    options opt;
    CHECK(!parse(&opt, 3, ARGV("h", "--not-a-flag", "g.xbe")));
    CHECK(opt.hdd_path == NULL);
    CHECK(opt.disc_path == NULL);
    CHECK(opt.mount_count == 0u);
    CHECK(opt.open_missing_as_empty == false);
    CHECK_STR(opt.disc_device, "\\Device\\CdRom0");
}

/* T84a: the Swap replay is OFF unless --gpu-replay names the module directory, so a boot trace
 * does not change by default, and its refinements are refused without it. */
static void test_ac97_ready_is_the_default_with_an_opt_out(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("h", "g.xbe")));
    CHECK(opt.ac97_ready == true && opt.ac97_ready_flag_given == false);
    CHECK(parse(&opt, 3, ARGV("h", "--no-ac97-ready", "g.xbe")));
    CHECK(opt.ac97_ready == false && opt.ac97_ready_flag_given == false);
    CHECK(parse(&opt, 3, ARGV("h", "--ac97-ready", "g.xbe")));
    CHECK(opt.ac97_ready == true && opt.ac97_ready_flag_given == true);
    CHECK(parse(&opt, 4, ARGV("h", "--ac97-ready", "--no-ac97-ready", "g.xbe")));
    CHECK(opt.ac97_ready == false);
    CHECK(parse(&opt, 4, ARGV("h", "--no-ac97-ready", "--ac97-ready", "g.xbe")));
    CHECK(opt.ac97_ready == true);
}

static void test_gpu_replay_is_opt_in(void)
{
    options opt;
    CHECK(parse(&opt, 2, ARGV("h", "g.xbe")));
    CHECK(opt.gpu_replay_dir == NULL);
    CHECK(opt.gpu_replay_dump == NULL);
    CHECK(opt.gpu_replay_width == 0u && opt.gpu_replay_height == 0u);
    CHECK(opt.gpu_replay_lenient == false);
    /* T84a3: both undecided/optional behaviours are off by default */
    CHECK(opt.gpu_replay_flip_y == false && opt.gpu_replay_undo_viewport == false);

    CHECK(parse(&opt, 4, ARGV("h", "--gpu-replay", "spv", "g.xbe")));
    CHECK(opt.gpu_replay_flip_y == false && opt.gpu_replay_undo_viewport == false);
    CHECK_STR(opt.gpu_replay_dir, "spv");
    CHECK(opt.gpu_replay_dump == NULL && opt.gpu_replay_lenient == false);
    CHECK(opt.gpu_replay_width == 0u && opt.gpu_replay_height == 0u);

    CHECK(parse(&opt, 9, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-dump", "frames",
                              "--gpu-replay-size", "640x480", "--gpu-replay-lenient", "g.xbe")));
    CHECK_STR(opt.gpu_replay_dump, "frames");
    CHECK(opt.gpu_replay_width == 640u && opt.gpu_replay_height == 480u);
    CHECK(opt.gpu_replay_lenient == true);

    /* T84a3: each of the two flags sets only its own field, and a stale struct is reset first */
    CHECK(parse(&opt, 5, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-flip-y", "g.xbe")));
    CHECK(opt.gpu_replay_flip_y == true && opt.gpu_replay_undo_viewport == false);
    CHECK(parse(&opt, 5, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-undo-viewport", "g.xbe")));
    CHECK(opt.gpu_replay_flip_y == false && opt.gpu_replay_undo_viewport == true);
    CHECK(opt.gpu_replay_lenient == false && opt.gpu_replay_dump == NULL);
    CHECK(parse(&opt, 6, ARGV("h", "--gpu-replay-undo-viewport", "--gpu-replay-flip-y", "--gpu-replay",
                              "spv", "g.xbe")));
    CHECK(opt.gpu_replay_flip_y == true && opt.gpu_replay_undo_viewport == true);
    CHECK_STR(opt.gpu_replay_dir, "spv"); /* the order of the flags does not matter */
    CHECK(parse(&opt, 2, ARGV("h", "g.xbe")));
    CHECK(opt.gpu_replay_flip_y == false && opt.gpu_replay_undo_viewport == false);
    /* T441: the two opt-ins are off by default, set only their own field, and are refused without --gpu-replay */
    CHECK(opt.gpu_replay_assume_program_mode == false && opt.gpu_replay_output_state == false);
    CHECK(opt.gpu_replay_viewport_from_target == false);
    CHECK(parse(&opt, 5, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-assume-program-mode", "g.xbe")));
    CHECK(opt.gpu_replay_assume_program_mode == true && opt.gpu_replay_output_state == false);
    CHECK(parse(&opt, 5, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-output-state", "g.xbe")));
    CHECK(opt.gpu_replay_assume_program_mode == false && opt.gpu_replay_output_state == true);
    CHECK(parse(&opt, 5, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-viewport-from-target", "g.xbe")));
    CHECK(opt.gpu_replay_viewport_from_target == true);
    CHECK(parse(&opt, 2, ARGV("h", "g.xbe")));
    CHECK(opt.gpu_replay_assume_program_mode == false && opt.gpu_replay_output_state == false);
    CHECK(!parse(&opt, 3, ARGV("h", "--gpu-replay-assume-program-mode", "g.xbe")));
    CHECK(!parse(&opt, 3, ARGV("h", "--gpu-replay-output-state", "g.xbe")));
    CHECK(!parse(&opt, 3, ARGV("h", "--gpu-replay-viewport-from-target", "g.xbe")));
    /* T560: the window-to-clip statement is off by default, sets only its own field, is refused alone */
    CHECK(opt.gpu_replay_window_to_clip == false);
    CHECK(parse(&opt, 5, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-window-to-clip", "g.xbe")));
    CHECK(opt.gpu_replay_window_to_clip == true && opt.gpu_replay_undo_viewport == false);
    CHECK(opt.gpu_replay_flip_y == false && opt.gpu_replay_viewport_from_target == false);
    CHECK(parse(&opt, 2, ARGV("h", "g.xbe")));
    CHECK(opt.gpu_replay_window_to_clip == false);
    CHECK(!parse(&opt, 3, ARGV("h", "--gpu-replay-window-to-clip", "g.xbe")));
    /* each refinement alone is refused: it would silently do nothing */
    CHECK(!parse(&opt, 3, ARGV("h", "--gpu-replay-flip-y", "g.xbe")));
    CHECK(!parse(&opt, 3, ARGV("h", "--gpu-replay-undo-viewport", "g.xbe")));
    CHECK(!parse(&opt, 4, ARGV("h", "--gpu-replay-dump", "frames", "g.xbe")));
    CHECK(!parse(&opt, 4, ARGV("h", "--gpu-replay-size", "64x64", "g.xbe")));
    CHECK(!parse(&opt, 3, ARGV("h", "--gpu-replay-lenient", "g.xbe")));
    /* a missing argument, and sizes that are not WxH or are out of range */
    CHECK(!parse(&opt, 3, ARGV("h", "g.xbe", "--gpu-replay")));
    CHECK(!parse(&opt, 6, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-size", "640", "g.xbe")));
    CHECK(!parse(&opt, 6, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-size", "640x", "g.xbe")));
    CHECK(!parse(&opt, 6, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-size", "0x480", "g.xbe")));
    test_second_vblank_policy();
    test_couple_vblank_effects_is_default_off_and_standalone();
    test_model_flips_is_default_off_and_needs_the_coupling();
    test_vblank_dispatch_level_requires_a_policy();
    test_vblank_quiescence_flags_require_a_policy();
    test_vblank_owner_waits_needs_its_prerequisites();
    test_vblank_worker_blanks_needs_the_owner_waits();
    test_vblank_poll_blank_needs_the_owner_waits();
    test_trace_vblank_schedule_requires_a_policy();
    CHECK(!parse(&opt, 6, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-size", "5000x480", "g.xbe")));
    CHECK(!parse(&opt, 6, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-size", "640x480y", "g.xbe")));
    CHECK(parse(&opt, 6, ARGV("h", "--gpu-replay", "spv", "--gpu-replay-size", "4096x1", "g.xbe")));
    CHECK(opt.gpu_replay_width == 4096u && opt.gpu_replay_height == 1u);
}

/* argc counted by the compiler: a hand-counted argc that is one off would make a refusal test pass for the wrong reason. */
#define PARSE(out, ...) parse((out), (int)(sizeof((char *[]){__VA_ARGS__}) / sizeof(char *)), ARGV(__VA_ARGS__))

/* T478 and T484: the combiner opt-in and the dump cadence are off by default, set only their own field and are
 * refused without the option they refine, so none of them can silently do nothing. */
static void test_gpu_replay_combiner_and_dump_cadence(void)
{
    options opt;
    CHECK(PARSE(&opt, "h", "g.xbe"));
    CHECK(opt.gpu_replay_combiner == false);
    CHECK(opt.gpu_replay_dump_every == 0u && opt.gpu_replay_dump_last == false);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "g.xbe"));
    CHECK(opt.gpu_replay_combiner == false); /* a replay without the flag keeps the fixed fragment stage */
    CHECK(opt.gpu_replay_dump_every == 0u && opt.gpu_replay_dump_last == false);

    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "g.xbe"));
    CHECK(opt.gpu_replay_combiner == true);
    CHECK(opt.gpu_replay_output_state == false && opt.gpu_replay_assume_program_mode == false);
    CHECK(opt.gpu_replay_dump_every == 0u && opt.gpu_replay_dump_last == false);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-output-state", "g.xbe"));
    CHECK(opt.gpu_replay_combiner == false && opt.gpu_replay_output_state == true);
    CHECK(!PARSE(&opt, "h", "--gpu-replay-combiner", "g.xbe"));

    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "frames",
                              "--gpu-replay-dump-every", "100", "g.xbe"));
    CHECK(opt.gpu_replay_dump_every == 100u && opt.gpu_replay_dump_last == false);
    CHECK(opt.gpu_replay_combiner == false);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "frames",
                              "--gpu-replay-dump-last", "g.xbe"));
    CHECK(opt.gpu_replay_dump_every == 0u && opt.gpu_replay_dump_last == true);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "frames",
                              "--gpu-replay-dump-last", "--gpu-replay-dump-every", "7", "g.xbe"));
    CHECK(opt.gpu_replay_dump_every == 7u && opt.gpu_replay_dump_last == true); /* they compose */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "frames",
                              "--gpu-replay-dump-every", "1", "g.xbe"));
    CHECK(opt.gpu_replay_dump_every == 1u); /* 1 is every frame: the default cadence, spelled out */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "g.xbe"));
    CHECK(opt.gpu_replay_dump_every == 0u && opt.gpu_replay_dump_last == false); /* a stale struct is reset */

    /* each cadence flag without a dump directory would do nothing, and without --gpu-replay as well */
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump-every", "5", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump-last", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay-dump-last", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay-dump-every", "5", "g.xbe"));
    /* a count must be a positive whole number, with nothing after it, and be present */
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "f",
                               "--gpu-replay-dump-every", "0", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "f",
                               "--gpu-replay-dump-every", "-3", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "f",
                               "--gpu-replay-dump-every", "5x", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "f",
                               "--gpu-replay-dump-every", "", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "f",
                               "--gpu-replay-dump-every", " 5", "g.xbe")); /* strtoull skips spaces and takes a sign */
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "f",
                               "--gpu-replay-dump-every", "+5", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "f",
                               "--gpu-replay-dump-every", "4294967296", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "f", "g.xbe",
                               "--gpu-replay-dump-every"));
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "f",
                              "--gpu-replay-dump-every", "4294967295", "g.xbe"));
    CHECK(opt.gpu_replay_dump_every == 4294967295u);
}

/* T497: --gpu-replay-standin-texture WxH:RRGGBBAA. Off by default, sets only its own fields, needs --gpu-replay-combiner
 * (which needs --gpu-replay), and a malformed value is refused rather than read as a different texture. */
static void test_gpu_replay_standin_texture(void)
{
    options opt;
    CHECK(PARSE(&opt, "h", "g.xbe"));
    CHECK(opt.gpu_replay_standin == false);
    CHECK(opt.gpu_replay_standin_width == 0u && opt.gpu_replay_standin_height == 0u);
    CHECK(opt.gpu_replay_standin_rgba[0] == 0u && opt.gpu_replay_standin_rgba[1] == 0u &&
          opt.gpu_replay_standin_rgba[2] == 0u && opt.gpu_replay_standin_rgba[3] == 0u);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "g.xbe"));
    CHECK(opt.gpu_replay_standin == false); /* the combiner without the flag keeps refusing a texture read */

    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-standin-texture",
                "2x3:336699FF", "g.xbe"));
    CHECK(opt.gpu_replay_standin == true);
    CHECK(opt.gpu_replay_standin_width == 2u && opt.gpu_replay_standin_height == 3u);
    CHECK(opt.gpu_replay_standin_rgba[0] == 0x33u && opt.gpu_replay_standin_rgba[1] == 0x66u &&
          opt.gpu_replay_standin_rgba[2] == 0x99u && opt.gpu_replay_standin_rgba[3] == 0xFFu);
    CHECK(opt.gpu_replay_combiner == true && opt.gpu_replay_output_state == false);
    /* the flag order does not matter, lowercase hex is hex, the edges are accepted */
    CHECK(PARSE(&opt, "h", "--gpu-replay-standin-texture", "4096x1:0a0B0c0D", "--gpu-replay-combiner",
                "--gpu-replay", "spv", "g.xbe"));
    CHECK(opt.gpu_replay_standin_width == 4096u && opt.gpu_replay_standin_height == 1u);
    CHECK(opt.gpu_replay_standin_rgba[0] == 0x0Au && opt.gpu_replay_standin_rgba[1] == 0x0Bu &&
          opt.gpu_replay_standin_rgba[2] == 0x0Cu && opt.gpu_replay_standin_rgba[3] == 0x0Du);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-standin-texture",
                "1x4096:00000000", "g.xbe"));
    CHECK(opt.gpu_replay_standin_width == 1u && opt.gpu_replay_standin_height == 4096u);
    CHECK(opt.gpu_replay_standin_rgba[0] == 0u && opt.gpu_replay_standin_rgba[3] == 0u);
    /* a later parse of a smaller command line resets it */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "g.xbe"));
    CHECK(opt.gpu_replay_standin == false && opt.gpu_replay_standin_width == 0u &&
          opt.gpu_replay_standin_rgba[0] == 0u);

    /* refused without the combiner and without the replay: it would do nothing and say nothing */
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-standin-texture", "1x1:FFFFFFFF", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay-standin-texture", "1x1:FFFFFFFF", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay-combiner", "--gpu-replay-standin-texture", "1x1:FFFFFFFF", "g.xbe"));
    /* a missing value, and every malformed one */
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "g.xbe",
                 "--gpu-replay-standin-texture"));
    static char *const malformed[] = {
        "",              "1x1",          "1x1:",        "1x1:FFFFFF",    "1x1:FFFFFFFFF", "1x1:GGGGGGGG",
        "1x1:FFFFFFFF ", "1x1: FFFFFFF", "1x1:+FFFFFFF", "0x1:FFFFFFFF",  "1x0:FFFFFFFF",  "4097x1:FFFFFFFF",
        "1x4097:FFFFFFFF", "-1x1:FFFFFFFF", "+1x1:FFFFFFFF", " 1x1:FFFFFFFF", "1x-1:FFFFFFFF", "1x+1:FFFFFFFF",
        "1X1:FFFFFFFF",  "1:FFFFFFFF",   "x1:FFFFFFFF", "1x:FFFFFFFF",   "1x1FFFFFFFF",   "1x1:0xFFFFFF",
        "1x1x1:FFFFFFFF", "99999999999x1:FFFFFFFF", "1x1:FFFFFFFF:",
    };
    for (size_t index = 0u; index < sizeof malformed / sizeof malformed[0]; index++) {
        const bool accepted = PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner",
                                    "--gpu-replay-standin-texture", malformed[index], "g.xbe");
        if (accepted) {
            printf("accepted malformed stand-in value \"%s\"\n", malformed[index]);
        }
        CHECK(!accepted);
    }
    CHECK(sizeof malformed / sizeof malformed[0] >= 20u);
}

/* T510: --gpu-replay-rt-texture and its census. Off by default, set only their own fields (the census implies the texture), and are
 * refused without everything the bridge needs, so neither can silently do nothing or stand in for a refused binding. */
static void test_gpu_replay_rt_texture(void)
{
    options opt;
    CHECK(PARSE(&opt, "h", "g.xbe"));
    CHECK(opt.gpu_replay_rt_texture == false && opt.gpu_replay_rt_texture_census == false);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state", "g.xbe"));
    CHECK(opt.gpu_replay_rt_texture == false && opt.gpu_replay_rt_texture_census == false);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state",
                "--gpu-replay-rt-texture", "g.xbe"));
    CHECK(opt.gpu_replay_rt_texture == true && opt.gpu_replay_rt_texture_census == false);
    CHECK(opt.gpu_replay_combiner && opt.gpu_replay_output_state && !opt.gpu_replay_standin && !opt.gpu_replay_flip_y);
    CHECK(PARSE(&opt, "h", "--gpu-replay-rt-texture-census", "--gpu-replay-output-state", "--gpu-replay-combiner",
                "--gpu-replay", "spv", "g.xbe"));
    CHECK(opt.gpu_replay_rt_texture == true && opt.gpu_replay_rt_texture_census == true);
    /* each missing requirement refuses */
    CHECK(!PARSE(&opt, "h", "--gpu-replay-rt-texture", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-output-state", "--gpu-replay-rt-texture", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-rt-texture", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state",
                 "--gpu-replay-rt-texture", "--gpu-replay-standin-texture", "1x1:FFFFFFFF", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state",
                 "--gpu-replay-rt-texture", "--gpu-replay-flip-y", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-rt-texture-census", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay-rt-texture-census", "g.xbe"));
}

/* T838: --gpu-live (the M9 live renderer) and --gpu-live-inferred. Off by default, implies the output state and the combiner, and is refused
 * without everything it needs: the replay's directory, the window sink, and not with the replay-only image paths. */
static void test_gpu_live(void)
{
    options opt;
    CHECK(PARSE(&opt, "h", "g.xbe"));
    CHECK(!opt.gpu_live && !opt.gpu_live_inferred);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "g.xbe"));
    CHECK(opt.gpu_live && !opt.gpu_live_inferred && opt.gpu_replay_output_state && opt.gpu_replay_combiner);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--gpu-live-inferred", "g.xbe"));
    CHECK(opt.gpu_live && opt.gpu_live_inferred);
    /* the replay's own flags set no live field */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state", "g.xbe"));
    CHECK(!opt.gpu_live && !opt.gpu_live_inferred);
    /* each missing requirement refuses */
    CHECK(!PARSE(&opt, "h", "--present", "window", "--gpu-live", "g.xbe"));                           /* no --gpu-replay */
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-live", "g.xbe"));                           /* no window */
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "null", "--gpu-live", "g.xbe"));      /* another sink */
    CHECK(!PARSE(&opt, "h", "--gpu-live-inferred", "g.xbe"));                                        /* needs --gpu-live */
    /* T847: on-demand module translation, off by default, needs --gpu-live */
    CHECK(PARSE(&opt, "h", "g.xbe") && !opt.gpu_live_translate);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--gpu-live-translate", "g.xbe"));
    CHECK(opt.gpu_live && opt.gpu_live_translate && !opt.gpu_live_inferred);
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live-translate", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-live-translate", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live-inferred", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--gpu-replay-rt-texture", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--gpu-replay-flip-y", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--gpu-replay-standin-texture", "1x1:FFFFFFFF", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-dump", "d", "--present", "window", "--gpu-live", "g.xbe"));
    /* T870: --gpu-live-fixed-program is gone (the intro's mode 4 draws were a CreateDevice state divergence), an unknown option */
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--gpu-live-fixed-program", "g.xbe"));
    /* T849: --gpu-live-blit, off by default, needs --gpu-live, sets nothing else */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "g.xbe") && !opt.gpu_live_blit);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--gpu-live-blit", "g.xbe"));
    CHECK(opt.gpu_live && opt.gpu_live_blit && !opt.gpu_live_inferred);
    CHECK(!PARSE(&opt, "h", "--gpu-live-blit", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live-blit", "g.xbe"));
    /* T1246: --live-pipeline N and --live-frame-hash FILE need --gpu-live, depth 0 or 1, unset means the default depth */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "g.xbe"));
    CHECK(!opt.live_pipeline_set && opt.live_pipeline == 0u && opt.live_frame_hash == NULL && LIVE_PIPELINE_DEFAULT_DEPTH == 1u);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--live-pipeline", "0", "g.xbe"));
    CHECK(opt.live_pipeline_set && opt.live_pipeline == 0u);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--live-pipeline", "1", "--live-frame-hash", "f.txt", "g.xbe"));
    CHECK(opt.live_pipeline_set && opt.live_pipeline == 1u && opt.live_frame_hash != NULL);
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--live-pipeline", "2", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--live-pipeline", "x", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--live-pipeline", "1", "g.xbe"));
    /* T1267: --live-readback and --live-blit-verify N need --gpu-live, verify excludes the readback route */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "g.xbe") && !opt.live_readback && opt.live_blit_verify == 0u);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--live-readback", "g.xbe") && opt.live_readback);
    CHECK(!PARSE(&opt, "h", "--live-readback", "g.xbe"));
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--live-blit-verify", "60", "g.xbe") && opt.live_blit_verify == 60u);
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--live-blit-verify", "0", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--live-readback", "--live-blit-verify", "60", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--live-blit-verify", "60", "g.xbe"));
    /* T1262: --live-present-sync needs --gpu-live and is off by default */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "g.xbe"));
    CHECK(!opt.live_present_sync);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--present", "window", "--gpu-live", "--live-present-sync", "g.xbe"));
    CHECK(opt.live_present_sync);
    CHECK(!PARSE(&opt, "h", "--live-present-sync", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--live-frame-hash", "f.txt", "g.xbe"));
}

static void test_interactive(void)
{
    options opt;
    CHECK(PARSE(&opt, "h", "--headless-streams", "--headless-buffers", "--headless-listener", "--headless-second-vblank", "--couple-vblank-effects", "--check-vblank-quiescence",
                "--vblank-owner-waits", "1", "--vblank-worker-blanks", "1", "--present", "window", "--interactive", "g.xbe"));
    CHECK(opt.interactive);
    CHECK(!PARSE(&opt, "h", "--interactive", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--headless-second-vblank", "--couple-vblank-effects", "--check-vblank-quiescence",
                 "--vblank-owner-waits", "1", "--vblank-worker-blanks", "1", "--present", "window", "--present-no-pace", "--interactive", "g.xbe"));
    CHECK(PARSE(&opt, "h", "g.xbe"));
    CHECK(!opt.interactive);
}

/* T633, T596: --gpu-replay-surface-source and --gpu-replay-target-persist. Both INFERRED, off by default, each sets only its own field and
 * is refused without what it needs: the surface source feeds the render target texture bridge (so the combiner, the output state, the replay
 * and no stand-in or flip-y), persistence needs the replay and cannot be mirrored a second time by flip-y. */
static void test_gpu_replay_surface_model(void)
{
    options opt;
    CHECK(PARSE(&opt, "h", "g.xbe"));
    CHECK(opt.gpu_replay_surface_source == false && opt.gpu_replay_target_persist == false);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state", "--gpu-replay-rt-texture", "g.xbe"));
    CHECK(opt.gpu_replay_surface_source == false && opt.gpu_replay_target_persist == false);
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "g.xbe"));
    CHECK(opt.gpu_replay_surface_source == false && opt.gpu_replay_target_persist == false);

    /* the surface source: with everything the bridge needs, in any argument order, and only its own field */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state", "--gpu-replay-rt-texture",
                "--gpu-replay-surface-source", "g.xbe"));
    CHECK(opt.gpu_replay_surface_source == true && opt.gpu_replay_target_persist == false && opt.gpu_replay_rt_texture == true &&
          opt.gpu_replay_rt_texture_census == false);
    CHECK(PARSE(&opt, "h", "--gpu-replay-surface-source", "--gpu-replay-rt-texture", "--gpu-replay-output-state", "--gpu-replay-combiner",
                "--gpu-replay", "spv", "g.xbe"));
    CHECK(opt.gpu_replay_surface_source == true && opt.gpu_replay_target_persist == false);
    /* the census implies the texture, so it satisfies the requirement and sets the census field, not the surface source */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state", "--gpu-replay-rt-texture-census",
                "--gpu-replay-surface-source", "g.xbe"));
    CHECK(opt.gpu_replay_surface_source == true && opt.gpu_replay_rt_texture_census == true);
    /* each missing requirement refuses, so it can never silently do nothing */
    CHECK(!PARSE(&opt, "h", "--gpu-replay-surface-source", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-surface-source", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state", "--gpu-replay-surface-source", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay-combiner", "--gpu-replay-output-state", "--gpu-replay-rt-texture", "--gpu-replay-surface-source", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-output-state", "--gpu-replay-rt-texture", "--gpu-replay-surface-source", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-rt-texture", "--gpu-replay-surface-source", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state", "--gpu-replay-rt-texture",
                 "--gpu-replay-surface-source", "--gpu-replay-flip-y", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state", "--gpu-replay-rt-texture",
                 "--gpu-replay-surface-source", "--gpu-replay-standin-texture", "1x1:FFFFFFFF", "g.xbe"));

    /* target persistence: only the replay is needed, and only its own field is set */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-target-persist", "g.xbe"));
    CHECK(opt.gpu_replay_target_persist == true && opt.gpu_replay_surface_source == false && opt.gpu_replay_rt_texture == false &&
          opt.gpu_replay_combiner == false);
    CHECK(PARSE(&opt, "h", "--gpu-replay-target-persist", "--gpu-replay", "spv", "g.xbe"));
    CHECK(opt.gpu_replay_target_persist == true);
    CHECK(!PARSE(&opt, "h", "--gpu-replay-target-persist", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-target-persist", "--gpu-replay-flip-y", "g.xbe"));
    CHECK(!PARSE(&opt, "h", "--gpu-replay-flip-y", "--gpu-replay-target-persist", "--gpu-replay", "spv", "g.xbe"));
    /* flip-y alone is still fine: only the pair is refused */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-flip-y", "g.xbe"));
    CHECK(opt.gpu_replay_flip_y == true && opt.gpu_replay_target_persist == false);

    /* both together, with the whole bridge: each is set, neither implies the other */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "--gpu-replay-combiner", "--gpu-replay-output-state", "--gpu-replay-rt-texture",
                "--gpu-replay-surface-source", "--gpu-replay-target-persist", "g.xbe"));
    CHECK(opt.gpu_replay_surface_source == true && opt.gpu_replay_target_persist == true);
    /* a later parse of a smaller command line resets both */
    CHECK(PARSE(&opt, "h", "--gpu-replay", "spv", "g.xbe"));
    CHECK(opt.gpu_replay_surface_source == false && opt.gpu_replay_target_persist == false);
}

/* T1250: the slowed audio playback is ON by default (250 per mille, pitch preserving), the options override it. */
static void test_audio_slowdown_options(void)
{
    options opt;
    char *bare[]={"host","game.xbe"};
    CHECK(parse(&opt,2,bare));
    CHECK(opt.audio_min_rate_permille == 250u && !opt.audio_stretch_resample);
    char *resample[]={"host","--audio-sink","sdl","--audio-stretch","resample","--audio-min-rate","300","game.xbe"};
    CHECK(parse(&opt,8,resample));
    CHECK(opt.audio_stretch_resample && opt.audio_min_rate_permille == 300u);
    char *wsola[]={"host","--audio-stretch","wsola","--audio-min-rate","0","game.xbe"};
    CHECK(parse(&opt,6,wsola));
    CHECK(!opt.audio_stretch_resample && opt.audio_min_rate_permille == 0u);
    char *bad[]={"host","--audio-stretch","bogus","game.xbe"};
    CHECK(!parse(&opt,4,bad));
    char *missing[]={"host","game.xbe","--audio-stretch"};
    CHECK(!parse(&opt,3,missing));
    /* T1250 gaps: --audio-hold-ms, default 400 */
    CHECK(parse(&opt,2,bare) && opt.audio_hold_ms == 400u);
    char *hold[]={"host","--audio-hold-ms","250","game.xbe"};
    CHECK(parse(&opt,4,hold) && opt.audio_hold_ms == 250u);
    char *hold_off[]={"host","--audio-hold-ms","0","game.xbe"};
    CHECK(parse(&opt,4,hold_off) && opt.audio_hold_ms == 0u);
    char *hold_bad[]={"host","--audio-hold-ms","2001","game.xbe"};
    CHECK(!parse(&opt,4,hold_bad));
    char *hold_text[]={"host","--audio-hold-ms","x","game.xbe"};
    CHECK(!parse(&opt,4,hold_text));
    char *pump_off[]={"host","--no-audio-pump","game.xbe"};
    CHECK(parse(&opt,2,bare) && opt.audio_pump);
    CHECK(parse(&opt,3,pump_off) && !opt.audio_pump);
    char *hold_missing[]={"host","game.xbe","--audio-hold-ms"};
    CHECK(!parse(&opt,3,hold_missing));
    /* T1487: governor default 80 ms, --av-sync-offset-ms signed -500..500 default 0, --audio-stretch-legacy */
    CHECK(parse(&opt,2,bare) && opt.audio_latency_ms == 80u && opt.av_sync_offset_ms == 0 && !opt.audio_stretch_legacy);
    char *av_pos[]={"host","--av-sync-offset-ms","21","game.xbe"};
    CHECK(parse(&opt,4,av_pos) && opt.av_sync_offset_ms == 21);
    char *av_neg[]={"host","--av-sync-offset-ms","-40","game.xbe"};
    CHECK(parse(&opt,4,av_neg) && opt.av_sync_offset_ms == -40);
    char *av_big[]={"host","--av-sync-offset-ms","501","game.xbe"};
    CHECK(!parse(&opt,4,av_big));
    char *av_text[]={"host","--av-sync-offset-ms","x","game.xbe"};
    CHECK(!parse(&opt,4,av_text));
    char *av_missing[]={"host","game.xbe","--av-sync-offset-ms"};
    CHECK(!parse(&opt,3,av_missing));
    char *legacy[]={"host","--audio-stretch-legacy","game.xbe"};
    CHECK(parse(&opt,3,legacy) && opt.audio_stretch_legacy);
    CHECK(parse(&opt,2,bare) && !opt.audio_stretch_legacy && opt.av_sync_offset_ms == 0); /* a later parse resets them */
}

static void test_gp_effects_explicit_opt_in(void)
{
    options opt;
    char *bare[]={"host","game.xbe"};
    CHECK(parse(&opt,2,bare));CHECK(!opt.gp_effects);
    char *gp[]={"host","game.xbe","--gp-effects"};
    CHECK(parse(&opt,3,gp));CHECK(opt.gp_effects);CHECK(opt.headless_effects);
}


/* T1629: --dump-on-button and its tuning flags: defaults, every bound at both ends, the requirements (ranges + dir, a pad source,
 * the replay handover for after-replay), the tuning flags refused alone. */
#define BD_BASE "host", "--synthetic-pad", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d"
static void test_dump_on_button_options(void)
{
    options defaults, ok, other;
    CHECK(PARSEV(&defaults, "host", "game.xbe"));
    CHECK(!defaults.dump_on_button && !defaults.dump_button_tuned && !defaults.dump_button_after_replay);
    CHECK(defaults.dump_after_count == 2u && defaults.dump_after[0] == 2u && defaults.dump_after[1] == 30u);
    CHECK(defaults.dump_button_threshold == 60u && defaults.dump_button_coalesce == 3u);
    CHECK(defaults.dump_button_max_pending == 8u && defaults.dump_button_max_dumps == 2000u);
    CHECK(defaults.dump_button_max_bytes == 0x40000000ull && defaults.dump_button_start_poll == 0u);
    CHECK(defaults.dump_button_idle_every == 0u && defaults.dump_button_max_idle == 40u);
    /* the plain enable keeps every default */
    CHECK(PARSEV(&ok, BD_BASE, "--dump-on-button", "game.xbe"));
    CHECK(ok.dump_on_button && !ok.dump_button_tuned && ok.dump_after_count == 2u && ok.dump_button_threshold == 60u);
    /* every flag set to a non default value */
    CHECK(PARSEV(&ok, "host", "--synthetic-pad", "--replay-input", "r", "--replay-handover", "--pad-source", "keyboard", "--present", "window",
                 "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "--dump-on-button", "--dump-after-frames", "1,5,600,3600",
                 "--dump-button-threshold", "100", "--dump-button-coalesce", "0", "--dump-button-max-pending", "64",
                 "--dump-button-max-dumps", "100000", "--dump-button-max-bytes", "0x1000", "--dump-button-start-poll", "99",
                 "--dump-button-after-replay", "--dump-button-idle-every", "600", "--dump-button-max-idle", "0", "game.xbe"));
    CHECK(ok.dump_on_button && ok.dump_button_tuned && ok.dump_button_after_replay);
    CHECK(ok.dump_after_count == 4u && ok.dump_after[0] == 1u && ok.dump_after[1] == 5u && ok.dump_after[2] == 600u &&
          ok.dump_after[3] == 3600u);
    CHECK(ok.dump_button_threshold == 100u && ok.dump_button_coalesce == 0u && ok.dump_button_max_pending == 64u);
    CHECK(ok.dump_button_max_dumps == 100000u && ok.dump_button_max_bytes == 0x1000u && ok.dump_button_start_poll == 99u);
    CHECK(ok.dump_button_idle_every == 600u && ok.dump_button_max_idle == 0u);
    /* a single after value, and a shorter list replaces the default fully */
    CHECK(PARSEV(&ok, BD_BASE, "--dump-on-button", "--dump-after-frames", "7", "game.xbe"));
    CHECK(ok.dump_after_count == 1u && ok.dump_after[0] == 7u && ok.dump_after[1] == 0u);
    /* --controllers is a pad source too (needs the window present) */
    CHECK(PARSEV(&ok, "host", "--controllers", "--present", "window", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "--dump-on-button",
                 "game.xbe"));
    /* requirements: ranges and dir, and a pad source */
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--dump-on-button", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--dump-guest-dir", "d", "--dump-on-button", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "--dump-on-button", "game.xbe"));
    /* the tuning flags are refused without the enable, each one alone */
    CHECK(!PARSEV(&other, BD_BASE, "--dump-after-frames", "2", "game.xbe"));
    CHECK(!PARSEV(&other, BD_BASE, "--dump-button-threshold", "60", "game.xbe"));
    CHECK(!PARSEV(&other, BD_BASE, "--dump-button-coalesce", "3", "game.xbe"));
    CHECK(!PARSEV(&other, BD_BASE, "--dump-button-max-pending", "8", "game.xbe"));
    CHECK(!PARSEV(&other, BD_BASE, "--dump-button-max-dumps", "20", "game.xbe"));
    CHECK(!PARSEV(&other, BD_BASE, "--dump-button-max-bytes", "5", "game.xbe"));
    CHECK(!PARSEV(&other, BD_BASE, "--dump-button-start-poll", "5", "game.xbe"));
    CHECK(!PARSEV(&other, BD_BASE, "--dump-button-after-replay", "game.xbe"));
    CHECK(!PARSEV(&other, BD_BASE, "--dump-button-idle-every", "5", "game.xbe"));
    CHECK(!PARSEV(&other, BD_BASE, "--dump-button-max-idle", "5", "game.xbe"));
    /* after-replay needs the replay and the handover */
    CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", "--dump-button-after-replay", "game.xbe"));
    CHECK(!PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d",
                  "--dump-on-button", "--dump-button-after-replay", "game.xbe"));
    CHECK(PARSEV(&other, "host", "--synthetic-pad", "--replay-input", "r", "--replay-handover", "--pad-source", "gamepad", "--present", "window",
                 "--dump-guest-range", "0x10:4", "--dump-guest-dir", "d", "--dump-on-button", "--dump-button-after-replay", "game.xbe"));
    /* --dump-after-frames: 1..4 strictly increasing integers in 1..3600, digits and commas only */
    const char *bad_lists[] = {"", "0", "3601", "5,5", "5,4", "1,2,3,4,5", "1,", ",1", "1,,2", "a", "1;2", "-1", "+1", " 1", "1 ", "2,30x", "99999999999999999999"};
    for (size_t index = 0u; index < sizeof bad_lists / sizeof bad_lists[0]; index++) {
        CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", "--dump-after-frames", (char *)bad_lists[index], "game.xbe"));
    }
    CHECK(PARSEV(&other, BD_BASE, "--dump-on-button", "--dump-after-frames", "3600", "game.xbe"));
    CHECK(PARSEV(&other, BD_BASE, "--dump-on-button", "--dump-after-frames", "1", "game.xbe"));
    CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", "--dump-after-frames"));
    /* numeric bounds, both ends: [flag, lowest ok, below it, highest ok, above it] */
    const struct {
        const char *flag;
        const char *low_ok;
        const char *below;
        const char *high_ok;
        const char *above;
    } bounds[] = {
        {"--dump-button-threshold", "1", "0", "254", "255"},
        {"--dump-button-coalesce", "0", "-1", "60", "61"},
        {"--dump-button-max-pending", "1", "0", "64", "65"},
        {"--dump-button-max-dumps", "3", "2", "100000", "100001"},
        {"--dump-button-max-bytes", "1", "0", "0xFFFFFFFFFFFFFFFF", "0x10000000000000000"},
        {"--dump-button-start-poll", "0", "-1", "4294967295", "4294967296"},
        {"--dump-button-idle-every", "0", "-1", "1000000", "1000001"},
        {"--dump-button-max-idle", "0", "-1", "100000", "100001"},
    };
    for (size_t index = 0u; index < sizeof bounds / sizeof bounds[0]; index++) {
        CHECK(PARSEV(&other, BD_BASE, "--dump-on-button", (char *)bounds[index].flag, (char *)bounds[index].low_ok, "game.xbe"));
        CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", (char *)bounds[index].flag, (char *)bounds[index].below, "game.xbe"));
        CHECK(PARSEV(&other, BD_BASE, "--dump-on-button", (char *)bounds[index].flag, (char *)bounds[index].high_ok, "game.xbe"));
        CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", (char *)bounds[index].flag, (char *)bounds[index].above, "game.xbe"));
        CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", (char *)bounds[index].flag, "x", "game.xbe"));
        CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", (char *)bounds[index].flag, "5x", "game.xbe"));
        CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", (char *)bounds[index].flag, "", "game.xbe"));
        CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", (char *)bounds[index].flag, " 5", "game.xbe"));
        CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", (char *)bounds[index].flag, "+5", "game.xbe"));
        CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", (char *)bounds[index].flag));
    }
    /* max-bytes takes C integer syntax (hex), the counts are decimal */
    CHECK(PARSEV(&ok, BD_BASE, "--dump-on-button", "--dump-button-max-bytes", "0x40000000", "game.xbe"));
    CHECK(ok.dump_button_max_bytes == 0x40000000ull);
    CHECK(PARSEV(&ok, BD_BASE, "--dump-on-button", "--dump-button-threshold", "010", "game.xbe"));
    CHECK(ok.dump_button_threshold == 10u); /* decimal, not octal */
    /* every event must fit the dump cap: 1 + the number of after dumps */
    CHECK(PARSEV(&ok, BD_BASE, "--dump-on-button", "--dump-button-max-dumps", "3", "game.xbe")); /* default 2 afters */
    CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", "--dump-button-max-dumps", "2", "game.xbe"));
    CHECK(PARSEV(&ok, BD_BASE, "--dump-on-button", "--dump-after-frames", "1,2,3,4", "--dump-button-max-dumps", "5", "game.xbe"));
    CHECK(!PARSEV(&other, BD_BASE, "--dump-on-button", "--dump-after-frames", "1,2,3,4", "--dump-button-max-dumps", "4", "game.xbe"));
    CHECK(PARSEV(&ok, BD_BASE, "--dump-on-button", "--dump-button-max-dumps", "2", "--dump-after-frames", "9", "game.xbe")); /* order free */
    /* it is passive: it does not need --forced-state and does not forbid it */
    CHECK(PARSEV(&ok, BD_BASE, "--forced-state", "--dump-on-button", "game.xbe"));
}

/* T1629: --help carries the marker the owner scripts grep for and names every flag. */
static void test_dump_on_button_help_text(void)
{
    const char *help = host_options_button_dump_help();
    CHECK(help != NULL);
    if (help == NULL) return;
    CHECK(strstr(help, "T1629") != NULL);
    static const char *const flags[] = {"--dump-on-button", "--dump-after-frames", "--dump-button-threshold", "--dump-button-coalesce",
                                        "--dump-button-max-pending", "--dump-button-max-dumps", "--dump-button-max-bytes",
                                        "--dump-button-start-poll", "--dump-button-after-replay", "--dump-button-idle-every",
                                        "--dump-button-max-idle"};
    for (size_t index = 0u; index < sizeof flags / sizeof flags[0]; index++) {
        CHECK(strstr(help, flags[index]) != NULL);
    }
    CHECK(strstr(help, "DIR/buttons.jsonl") != NULL);
    CHECK(strstr(help, "start line says forced_state:true") != NULL);
}

/* T1617: the owner scripts grep `--help` for "T1616" and "--route-wait" to tell a host with route replay from one without. */
static void test_route_replay_help_text(void)
{
    const char *help = host_options_route_replay_help();
    CHECK(help != NULL);
    if (help == NULL) return;
    CHECK(strstr(help, "T1616") != NULL);
    CHECK(strstr(help, "--route-wait") != NULL);
    CHECK(strstr(help, "--poke-at-poll") != NULL);
    CHECK(strstr(help, "--replay-handover") != NULL);
    CHECK(strstr(help, "--replay-input") != NULL);
    CHECK(strstr(help, "--forced-state") != NULL);
    CHECK(strstr(help, "--dump-guest-range") != NULL);
    CHECK(strstr(help, "SIGUSR2") != NULL);
    CHECK(strstr(help, "# mark: at=N") != NULL);
    /* T1618: the marker the owner scripts grep for, and the identity rule it states */
    CHECK(strstr(help, "T1618") != NULL);
    CHECK(strstr(help, "--census-icalls, --census-phases and --census-window") != NULL);
    CHECK(strstr(help, "--skip-intro is") != NULL);
    /* The two example specs in the text are accepted by the parser they document. */
    const char *route_wait[] = {"prog", "--synthetic-pad", "--replay-input", "r.txt", "--route-wait", "mark3:mem=0x52D3FC==6,max=36000", "game.xbe"};
    const char *poke_at[] = {"prog", "--synthetic-pad", "--replay-input", "r.txt", "--forced-state", "--dump-guest-range", "0x10:4",
                             "--dump-guest-dir", "d", "--poke-at-poll", "mark3:batch1", "game.xbe"};
    CHECK(strstr(help, "--route-wait mark3:mem=0x52D3FC==6,max=36000") != NULL);
    CHECK(strstr(help, "--poke-at-poll mark3:batch1") != NULL);
    options parsed;
    CHECK(parse(&parsed, 7, (char **)route_wait));
    CHECK(parsed.route_wait_count == 1u);
    CHECK(parse(&parsed, 12, (char **)poke_at));
    CHECK(parsed.poke_at_count == 1u);
}

/* T1640: the owner scripts grep `--help` for "T1640" and "--route-nav" to tell a host with the closed loop menu navigation. */
static void test_route_nav_help_text(void)
{
    const char *help = host_options_route_nav_help();
    CHECK(help != NULL);
    if (help == NULL) return;
    CHECK(strstr(help, "T1640") != NULL);
    CHECK(strstr(help, "--route-nav ") != NULL);
    CHECK(strstr(help, "--route-nav-menus") != NULL);
    CHECK(strstr(help, "--route-nav-timing") != NULL);
    CHECK(strstr(help, "# nav: at=P to=Q menu=ID select=index:N|name:TEXT") != NULL);
    CHECK(strstr(help, "strict") != NULL && strstr(help, "falls back") != NULL);
}

int main(void)
{
    test_route_replay_help_text();
    test_route_nav_help_text();
    test_audio_slowdown_options();
    test_gp_effects_explicit_opt_in();
    test_interactive();
    printf("host option parsing tests\n");

    test_the_bare_invocation_sets_every_default();
    test_voice_log_option();
    test_the_defaults_are_not_merely_whatever_the_macros_say();
    test_xonline_offline_is_default_off_and_explicitly_opt_in();
    test_each_boolean_flag_sets_only_itself();
    test_headless_effects_is_default_and_flag_is_a_noop();
    test_second_vblank_policy();
    test_headless_audio_policies_are_independent();
    test_headless_audio_umbrella_is_a_pure_convenience();
    test_native_xmv_defaults_with_a_disc();
    test_movie_test_aids_are_opt_in();
    test_xmv_entry_capture_options();
    test_xmv_seed_options();
    test_headless_movie_audio_is_opt_in_and_needs_xmv_and_streams();
    test_passive_audio_completion_is_opt_in_and_needs_the_startup_policies();
    test_async_file_io_is_opt_in_and_standalone();
    test_synthetic_pad_is_opt_in_and_standalone();
    test_profile_calls_is_opt_in_and_its_refinements_need_it();
    test_census_icalls_is_opt_in_and_its_window_needs_it();
    test_dump_guest_range_options();
    test_route_options();
    test_hotkey_options();
    test_hotkey_help_text();
    test_dump_on_button_options();
    test_dump_on_button_help_text();
    test_async_file_io_spin_complete_is_opt_in_and_validated();
    test_each_valued_flag_takes_the_following_argv_slot();
    test_audio_output_path_is_opt_in();
    test_audio_mute_needs_the_sdl_sink();
    test_cache_partitions_parses_and_is_bounded();
    test_the_xbe_path_is_the_one_positional_argument();
    test_a_flag_whose_argument_is_missing_is_refused();
    test_an_unknown_flag_is_refused();
    test_out_of_range_numbers_are_refused();
    test_mounts_accumulate_in_order_and_the_bound_is_refused();
    test_a_refused_parse_still_leaves_a_fully_initialised_struct();
    test_ac97_ready_is_the_default_with_an_opt_out();
    test_gpu_replay_is_opt_in();
    test_gpu_replay_combiner_and_dump_cadence();
    test_gpu_replay_standin_texture();
    test_gpu_replay_rt_texture();
    test_gpu_live();
    test_gpu_replay_surface_model();

    printf("%s: %d checks, %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", checks,
           failures);
    return failures == 0 ? 0 : 1;
}
