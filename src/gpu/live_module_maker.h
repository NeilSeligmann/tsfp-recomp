/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T847 (M9): shader modules made ON DEMAND for the live renderer. The T95 tables hold the programs the title's own assembler can emit
 * and the static ones of the executable, and the combiner modules cover the definitions the corpus lists. A program the title binds
 * that is in none of them (found by the retail run: movie player and loading screen programs) used to refuse every draw that bound it.
 * With a maker installed (gpu_pgraph_backend.make_module) the replay hands the program's bytes (or a combiner definition block) to
 * live_module_maker_make, which runs the repository's own translator as a subprocess
 *
 *     <python> -m tools.nv2a.live_modules vertex|combiner --out DIR --expect NAME FILE
 *
 * (tools/nv2a/live_modules.py: the vertex translator of tools/nv2a/translate.py and glslang, the combiner translator of
 * tools/nv2a_combiner) and the module appears in DIR as `<name>.spv`, named by the digest of its own input. DIR is the `--gpu-replay`
 * module directory, so a module made once is found by the next run at start (a persistent cache keyed by digest, GITIGNORED: derived
 * from the user's own data, never committed). A failure is remembered by name (a draw is refused with the translator's own message,
 * the subprocess does not run again for the same program). INFERRED: the translators are INFERRED (docs/vertex-translator.md), the
 * viewport is the raw default of the corpus modules. Runs on the thread that resolves the draw, the guest is blocked meanwhile.
 *
 * The command runs in the current directory (the repository root: `tools` is imported from there).
 */
#ifndef TSFP_GPU_LIVE_MODULE_MAKER_H
#define TSFP_GPU_LIVE_MODULE_MAKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LIVE_MODULE_RASTER_PROFILE "live-raster-v5"
bool live_module_maker_profile_directory(const char *root, char *out,
                                         size_t bytes, char *error,
                                         size_t error_bytes);

typedef struct live_module_maker live_module_maker;
void live_module_maker_set_live_raster(live_module_maker *maker, bool enabled);

typedef struct {
    unsigned long long made;     /* modules the translator wrote */
    unsigned long long reused;   /* asked for a name whose file already existed (another run or process wrote it) */
    unsigned long long failed;   /* distinct names the translator refused or that failed to run */
    unsigned long long refused_again; /* later asks for a name that failed before (the subprocess did not run) */
    double seconds;              /* wall time spent in subprocesses (summed, so a batch counts every subprocess) */
    unsigned long long batches, batch_modules; /* T1247: live_module_maker_make_batch calls and modules asked for */
    double batch_wall_seconds;   /* T1247: wall time the batches took */
} live_module_maker_stats;

/* `directory` must exist. `python` NULL: the TSFP_PYTHON environment variable, else "python3". `glslang` NULL: glslangValidator. NULL with
 * `error` filled when tools/nv2a/live_modules.py is not under the current directory or the directory is not writable. */
live_module_maker *live_module_maker_create(const char *directory, const char *python, const char *glslang, char *error,
                                            size_t error_bytes);
void live_module_maker_destroy(live_module_maker *maker);
/* The gpu_pgraph_module_make_fn: `fragment` picks combiner or vertex, `name` is the module stem (`generated_<64 hex>` or
 * `combiner_<64 hex>`, anything else is refused: the name becomes a file name). */
bool live_module_maker_make(void *maker, bool fragment, const char *name, const uint8_t *bytes, size_t byte_count, char *error,
                            size_t error_bytes);
/* T1247: translate several modules at once (at most `parallel` subprocesses, 0 = 8, cap 32): a cold cache that needs 20 modules in one
 * frame costs the slowest translation, not their sum. Same name rules and failure memory as live_module_maker_make, which it calls
 * on each request from worker threads. Returns how many of the requested files exist afterwards. */
typedef struct {
    bool fragment;
    char name[96];
    const uint8_t *bytes;
    size_t byte_count;
} live_module_request;
size_t live_module_maker_make_batch(live_module_maker *maker, const live_module_request *requests, size_t count, unsigned parallel);
live_module_maker_stats live_module_maker_get_stats(const live_module_maker *maker);
void live_module_maker_print(const live_module_maker *maker, FILE *out);

#endif
