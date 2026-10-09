/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T847: the on-demand module maker. A fake `tools/nv2a/live_modules.py` in a scratch directory stands for the translator (the real one is
 * covered by tests/test_live_modules.py), so this checks what the maker owns: the name rules, the subprocess call, the remembered failures,
 * the files it leaves. Runs in a scratch directory under the system temp, needs python3 on the PATH (SKIP 77 without).
 */
#define _POSIX_C_SOURCE 200809L
#include "live_module_maker.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static int checks;
static int failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static double wall_seconds(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
}

static const char FAKE[] =
    "import os, sys, pathlib, time\n"
    "kind, file = sys.argv[3], None\n"
    "args = sys.argv[1:]\n"
    "data = pathlib.Path(args[1]).read_bytes()\n"
    "out = pathlib.Path(args[args.index('--out') + 1])\n"
    "expect = args[args.index('--expect') + 1]\n"
    "with open(out / 'runs.txt', 'a') as log:\n"
    "    log.write(args[0] + ' ' + expect + '\\n')\n"
    "mode = os.environ.get('T935_FAKE_MODE', '')\n"
    "time.sleep(float(os.environ.get('T1247_FAKE_SLEEP', '0')))\n"
    "if mode == 'eof_sleep':\n"
    "    os.close(1); os.close(2); time.sleep(1.0); sys.exit(0)\n"
    "if mode == 'eof_success':\n"
    "    (out / (expect + '.spv')).write_bytes(bytes.fromhex('03022307') + data)\n"
    "    print('eof success output', flush=True); os.close(1); os.close(2); sys.exit(0)\n"
    "if mode == 'eof_failure':\n"
    "    print('eof failure output', flush=True, file=sys.stderr); os.close(1); os.close(2); sys.exit(1)\n"
    "if data[0] == 0xFF:\n"
    "    print('first line of noise', file=sys.stderr)\n"
    "    print('the translator refuses this program', file=sys.stderr)\n"
    "    sys.exit(1)\n"
    "(out / (expect + '.spv')).write_bytes(bytes.fromhex('03022307') + data)\n"
    "print(out / (expect + '.spv'))\n";

static int count_lines(const char *path, const char *needle)
{
    FILE *file = fopen(path, "r");
    char line[256];
    int count = 0;
    while (file != NULL && fgets(line, sizeof line, file) != NULL) {
        count += needle == NULL || strstr(line, needle) != NULL;
    }
    if (file != NULL) {
        fclose(file);
    }
    return count;
}

static int count_entries(const char *directory)
{
    DIR *handle = opendir(directory);
    int count = 0;
    for (struct dirent *entry = handle != NULL ? readdir(handle) : NULL; entry != NULL; entry = readdir(handle)) {
        count += entry->d_name[0] != '.' || (entry->d_name[1] != '\0' && strcmp(entry->d_name, "..") != 0);
    }
    if (handle != NULL) {
        closedir(handle);
    }
    return count;
}

int main(void)
{
    if (system("python3 -c pass") != 0) {
        printf("SKIP: no python3\n");
        return 77;
    }
    char root[] = "/tmp/tsfp_maker_XXXXXX";
    if (mkdtemp(root) == NULL) {
        return 1;
    }
    char original[512];
    if (getcwd(original, sizeof original) == NULL || chdir(root) != 0) {
        return 1;
    }
    char error[256];
    mkdir("modules", 0700);

    /* the repository root is not here yet */
    CHECK(live_module_maker_create("modules", "python3", NULL, error, sizeof error) == NULL);
    CHECK(strstr(error, "live_modules.py") != NULL);
    mkdir("tools", 0700);
    mkdir("tools/nv2a", 0700);
    FILE *script = fopen("tools/nv2a/live_modules.py", "w");
    CHECK(script != NULL && fputs(FAKE, script) >= 0 && fclose(script) == 0);
    FILE *init = fopen("tools/__init__.py", "w");
    CHECK(init != NULL && fclose(init) == 0);
    init = fopen("tools/nv2a/__init__.py", "w");
    CHECK(init != NULL && fclose(init) == 0);
    CHECK(live_module_maker_create("nowhere", "python3", NULL, error, sizeof error) == NULL);
    CHECK(strstr(error, "writable directory") != NULL);

    live_module_maker *maker = live_module_maker_create("modules", "python3", NULL, error, sizeof error);
    CHECK(maker != NULL);
    char vertex[96], bad[96], combiner[96];
    snprintf(vertex, sizeof vertex, "generated_%064x", 0xABCDEFu);
    snprintf(bad, sizeof bad, "generated_%064x", 0x123u);
    snprintf(combiner, sizeof combiner, "combiner_%064x", 0x77u);
    const uint8_t good_bytes[4] = {0x78, 0x20, 1, 0};
    const uint8_t bad_bytes[4] = {0xFF, 0x20, 1, 0};

    /* names become file names: refuse anything but the two digest forms */
    CHECK(!live_module_maker_make(maker, false, "../evil", good_bytes, 4u, error, sizeof error));
    CHECK(!live_module_maker_make(maker, false, "generated_xyz", good_bytes, 4u, error, sizeof error));
    CHECK(!live_module_maker_make(maker, true, vertex, good_bytes, 4u, error, sizeof error)); /* vertex name asked as fragment */
    CHECK(!live_module_maker_make(maker, false, combiner, good_bytes, 4u, error, sizeof error));
    CHECK(!live_module_maker_make(maker, false, vertex, good_bytes, 0u, error, sizeof error));
    CHECK(!live_module_maker_make(maker, false, vertex, NULL, 4u, error, sizeof error));
    char upper[96];
    snprintf(upper, sizeof upper, "generated_%064X", 0xABCDEFu);
    CHECK(!live_module_maker_make(maker, false, upper, good_bytes, 4u, error, sizeof error));
    char longer[96], not_hex[96], shorter[96];
    snprintf(longer, sizeof longer, "%s0", vertex); /* 65 digits */
    snprintf(not_hex, sizeof not_hex, "generated_%063xg", 0x5u);
    snprintf(shorter, sizeof shorter, "generated_%063x", 0x5u);
    char odd_prefix[96], odd_combiner[96];
    snprintf(odd_prefix, sizeof odd_prefix, "generatedX%064x", 0x5u); /* the right length, the wrong prefix */
    snprintf(odd_combiner, sizeof odd_combiner, "combinerX%064x", 0x5u);
    CHECK(!live_module_maker_make(maker, false, odd_prefix, good_bytes, 4u, error, sizeof error));
    CHECK(!live_module_maker_make(maker, true, odd_combiner, good_bytes, 4u, error, sizeof error));
    CHECK(!live_module_maker_make(maker, false, longer, good_bytes, 4u, error, sizeof error));
    CHECK(!live_module_maker_make(maker, false, not_hex, good_bytes, 4u, error, sizeof error));
    CHECK(!live_module_maker_make(maker, false, shorter, good_bytes, 4u, error, sizeof error));
    CHECK(count_entries("modules") == 0);
    CHECK(live_module_maker_get_stats(maker).made == 0u);

    /* a vertex module is made: the file is there, the input is gone, the translator ran once with the kind */
    CHECK(live_module_maker_make(maker, false, vertex, good_bytes, 4u, error, sizeof error));
    char path[700];
    snprintf(path, sizeof path, "modules/%s.spv", vertex);
    CHECK(access(path, R_OK) == 0);
    CHECK(count_lines("modules/runs.txt", "vertex") == 1);
    CHECK(count_entries("modules") == 2); /* the module and the fake's log, no input left behind */
    CHECK(live_module_maker_get_stats(maker).made == 1u);

    /* EOF is not child exit: the deadline still terminates and reaps a silent child. */
    char eof_sleep[96];
    snprintf(eof_sleep, sizeof eof_sleep, "generated_%064x", 0xE0Fu);
    CHECK(setenv("TSFP_LIVE_MODULE_TIMEOUT_MS", "100", 1) == 0);
    CHECK(setenv("T935_FAKE_MODE", "eof_sleep", 1) == 0);
    const double overdue_started = wall_seconds();
    CHECK(!live_module_maker_make(maker, false, eof_sleep, good_bytes, 4u, error, sizeof error));
    const double overdue_elapsed = wall_seconds() - overdue_started;
    CHECK(strstr(error, "did not finish") != NULL);
    CHECK(overdue_elapsed < 0.8);
    CHECK(unsetenv("T935_FAKE_MODE") == 0);
    CHECK(unsetenv("TSFP_LIVE_MODULE_TIMEOUT_MS") == 0);

    /* Output remains captured across EOF, for both an ordinary success and failure. */
    char eof_success[96], eof_failure[96];
    snprintf(eof_success, sizeof eof_success, "generated_%064x", 0xE01u);
    snprintf(eof_failure, sizeof eof_failure, "generated_%064x", 0xE02u);
    CHECK(setenv("T935_FAKE_MODE", "eof_success", 1) == 0);
    CHECK(live_module_maker_make(maker, false, eof_success, good_bytes, 4u, error, sizeof error));
    snprintf(path, sizeof path, "modules/%s.spv", eof_success);
    CHECK(access(path, R_OK) == 0);
    CHECK(setenv("T935_FAKE_MODE", "eof_failure", 1) == 0);
    CHECK(!live_module_maker_make(maker, false, eof_failure, bad_bytes, 4u, error, sizeof error));
    CHECK(strcmp(error, "eof failure output") == 0);
    CHECK(unsetenv("T935_FAKE_MODE") == 0);

    /* asking again finds the file: the translator does not run */
    CHECK(live_module_maker_make(maker, false, vertex, good_bytes, 4u, error, sizeof error));
    CHECK(count_lines("modules/runs.txt", NULL) == 4);
    CHECK(live_module_maker_get_stats(maker).reused == 1u);

    /* a combiner goes to the combiner command */
    CHECK(live_module_maker_make(maker, true, combiner, good_bytes, 4u, error, sizeof error));
    CHECK(count_lines("modules/runs.txt", "combiner combiner_") == 1);

    /* a refusal: false with the translator's last line, remembered, the subprocess does not run again */
    CHECK(!live_module_maker_make(maker, false, bad, bad_bytes, 4u, error, sizeof error));
    CHECK(strcmp(error, "the translator refuses this program") == 0);
    CHECK(count_lines("modules/runs.txt", NULL) == 6);
    error[0] = '\0';
    CHECK(!live_module_maker_make(maker, false, bad, bad_bytes, 4u, error, sizeof error));
    CHECK(strcmp(error, "the translator refuses this program") == 0);
    CHECK(count_lines("modules/runs.txt", NULL) == 6);
    live_module_maker_stats stats = live_module_maker_get_stats(maker);
    CHECK(stats.made == 3u && stats.failed == 3u && stats.refused_again == 1u && stats.reused == 1u && stats.seconds > 0.0);
    snprintf(path, sizeof path, "modules/%s.spv", bad);
    CHECK(access(path, F_OK) != 0);
    live_module_maker_destroy(maker);

    /* an interpreter that does not exist is a named failure, not a crash */
    maker = live_module_maker_create("modules", "/nonexistent/python", NULL, error, sizeof error);
    CHECK(maker != NULL);
    CHECK(!live_module_maker_make(maker, false, bad, good_bytes, 4u, error, sizeof error));
    CHECK(strstr(error, "could not run /nonexistent/python") != NULL);
    live_module_maker_destroy(maker);

    char long_directory[513];
    memset(long_directory, 'x', sizeof long_directory - 1u);
    long_directory[sizeof long_directory - 1u] = '\0';
    CHECK(live_module_maker_create(long_directory, NULL, NULL, error, sizeof error) == NULL);
    CHECK(strstr(error, "path too long") != NULL);
    char profile[512];
    CHECK(live_module_maker_profile_directory(
        "modules", profile, sizeof profile, error, sizeof error));
    CHECK(strstr(profile, "modules/live-raster-v5") != NULL);
    /* Same digest in v4 must not satisfy the new UBO/fog profile. */
    CHECK(mkdir("modules/live-raster-v4",0700)==0);
    snprintf(path,sizeof path,"modules/live-raster-v4/%s.spv",vertex);
    FILE *old_profile=fopen(path,"wb");
    const uint8_t stale[]={3,2,35,7};
    CHECK(old_profile!=NULL&&fwrite(stale,1,sizeof stale,old_profile)==sizeof stale&&fclose(old_profile)==0);
    maker=live_module_maker_create(profile,"python3",NULL,error,sizeof error);
    CHECK(maker!=NULL);
    live_module_maker_set_live_raster(maker,true);
    CHECK(live_module_maker_make(maker,false,vertex,good_bytes,4u,error,sizeof error));
    stats=live_module_maker_get_stats(maker);
    CHECK(stats.made==1u&&stats.reused==0u);
    snprintf(path,sizeof path,"%s/%s.spv",profile,vertex);
    CHECK(access(path,F_OK)==0);
    live_module_maker_destroy(maker);

    /* T1247: a batch runs its translations at the same time. Eight modules of 0.4 s each take about 0.4 s, not 3.2 s, and each file is made
     * exactly once with the same input a single make would give it; a failing request does not stop the others. */
    {
        CHECK(mkdir("batch", 0700) == 0);
        live_module_maker *batch_maker = live_module_maker_create("batch", "python3", NULL, error, sizeof error);
        CHECK(batch_maker != NULL);
        setenv("T1247_FAKE_SLEEP", "0.4", 1);
        live_module_request requests[9];
        uint8_t inputs[9][4];
        for (unsigned i = 0u; i < 9u; i++) {
            memset(&requests[i], 0, sizeof requests[i]);
            snprintf(requests[i].name, sizeof requests[i].name, "generated_%064x", 0x9000u + i);
            inputs[i][0] = i == 8u ? 0xFFu : 0x78u; /* the last one the fake translator refuses */
            inputs[i][1] = 0x20u;
            inputs[i][2] = 1u;
            inputs[i][3] = (uint8_t)i;
            requests[i].bytes = inputs[i];
            requests[i].byte_count = 4u;
        }
        const double batch_start = wall_seconds();
        const size_t present = live_module_maker_make_batch(batch_maker, requests, 9u, 9u);
        const double batch_seconds = wall_seconds() - batch_start;
        CHECK(present == 8u);
        CHECK(batch_seconds < 1.6);   /* serial would be 9 x 0.4 s */
        CHECK(batch_seconds >= 0.35); /* and it really ran the translator */
        const live_module_maker_stats batch_stats = live_module_maker_get_stats(batch_maker);
        CHECK(batch_stats.made == 8u && batch_stats.failed == 1u && batch_stats.batches == 1u && batch_stats.batch_modules == 9u);
        CHECK(count_lines("batch/runs.txt", NULL) == 9);
        char made_path[700];
        snprintf(made_path, sizeof made_path, "batch/%s.spv", requests[3].name);
        CHECK(access(made_path, F_OK) == 0);
        /* the swap replay's own ask for one of them afterwards finds the file and runs nothing */
        CHECK(live_module_maker_make(batch_maker, false, requests[3].name, inputs[3], 4u, error, sizeof error));
        CHECK(count_lines("batch/runs.txt", NULL) == 9);
        CHECK(live_module_maker_get_stats(batch_maker).reused == 1u);
        CHECK(live_module_maker_make_batch(batch_maker, requests, 0u, 4u) == 0u);
        CHECK(live_module_maker_make_batch(NULL, requests, 9u, 4u) == 0u);
        unsetenv("T1247_FAKE_SLEEP");
        live_module_maker_destroy(batch_maker);
    }

    CHECK(live_module_maker_profile_directory(
        "modules", profile, sizeof profile, error, sizeof error));
    CHECK(!live_module_maker_profile_directory("modules", profile, 2u, error,
                                               sizeof error));
    CHECK(strstr(error, "path too long") != NULL);
    CHECK(!live_module_maker_profile_directory(
        "/nonexistent/root", profile, sizeof profile, error, sizeof error));
    CHECK(chdir(original) == 0);
    char cleanup[600];
    snprintf(cleanup, sizeof cleanup, "rm -rf %s", root);
    CHECK(system(cleanup) == 0);
    printf("live_module_maker: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
