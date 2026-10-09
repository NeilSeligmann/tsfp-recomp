/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The measured XDK ABI table's wiring: src/host/xdk_thunk.c `xdk_thunk_declare_abi_rows`.
 *
 * The generated table itself (src/xbox/xdk_abi.inc) is derived from the user's executable
 * and gitignored, so this suite proves the WIRING with rows it writes itself and checks
 * only invariants of the generated table that hold whether or not it exists.
 *
 * WHAT HAS TO BE TRUE:
 *   1. Each evidence class reaches the entry point that gates it. `ret imm16` rows take no
 *      site quorum, caller-vote rows do. A swap would pass a build and quietly admit a
 *      one-voter row, or refuse a decisive one.
 *   2. A row whose address is not in the adopted surface is REFUSED AND COUNTED, because
 *      that is the only way a table gone stale against its key shows itself.
 *   3. The pop bytes that result are the arithmetic `esp` depends on.
 */

#include "xdk_thunk.h"

#include "recomp_abi.h"

#include <stdarg.h>
#include <stdio.h>

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

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

#define CHECK_POP(address, expected)                                                    \
    do {                                                                                \
        uint32_t pop_ = 0u;                                                             \
        CHECK(xdk_thunk_pop_bytes((address), &pop_));                                   \
        CHECK(pop_ == (uint32_t)(expected));                                            \
    } while (0)

static int quiet_printer(const char *format, ...)
{
    (void)format;
    return 0;
}

enum {
    A_STD = 0x00400000u,
    A_THIS = 0x00400010u,
    A_FAST = 0x00400020u,
    A_VOTED_OK = 0x00400030u,
    A_VOTED_THIN = 0x00400040u,
    A_NO_RET = 0x00400050u,
    A_BAD_SHAPE = 0x00400060u,
    A_ABSENT = 0x00400070u,
};

static const xdk_dispatch_entry SURFACE[] = {
    {A_STD, NULL, XDK_MODULE_NONE},        {A_THIS, NULL, XDK_MODULE_NONE},
    {A_FAST, NULL, XDK_MODULE_NONE},       {A_VOTED_OK, NULL, XDK_MODULE_NONE},
    {A_VOTED_THIN, NULL, XDK_MODULE_NONE}, {A_NO_RET, NULL, XDK_MODULE_NONE},
    {A_BAD_SHAPE, NULL, XDK_MODULE_NONE},
};
#define SURFACE_COUNT (sizeof(SURFACE) / sizeof(SURFACE[0]))

static void fresh(void)
{
    CHECK(xdk_thunk_init(SURFACE, SURFACE_COUNT));
    xdk_thunk_set_log(quiet_printer);
}

static void test_each_evidence_class_reaches_its_own_gate(void)
{
    fresh();
    /* ONE terminator is enough for a `ret imm16` row. A site quorum applied to it would
     * refuse this, which is the 165-of-199 failure xdk_thunk_declare_callee_abi exists
     * to avoid. */
    const xdk_abi_row rows[] = {
        {A_STD, XDK_CC_STDCALL, 3u, 0u, XDK_ABI_FROM_CALLEE_RET, 1u},
        {A_THIS, XDK_CC_THISCALL, 2u, 1u, XDK_ABI_FROM_CALLEE_RET, 2u},
        {A_FAST, XDK_CC_FASTCALL, 1u, 2u, XDK_ABI_FROM_CALLEE_RET, 1u},
        /* Exactly the quorum accepts, one below it does not. */
        {A_VOTED_OK, XDK_CC_STDCALL, 0u, 0u, XDK_ABI_FROM_CALLER_VOTES,
         XDK_MEASURED_ARITY_MIN_SITES},
        {A_VOTED_THIN, XDK_CC_STDCALL, 0u, 0u, XDK_ABI_FROM_CALLER_VOTES,
         XDK_MEASURED_ARITY_MIN_SITES - 1u},
        /* Nothing was read, so nothing was measured. */
        {A_NO_RET, XDK_CC_STDCALL, 1u, 0u, XDK_ABI_FROM_CALLEE_RET, 0u},
        /* THISCALL with no register argument is not a shape that convention has. */
        {A_BAD_SHAPE, XDK_CC_THISCALL, 1u, 0u, XDK_ABI_FROM_CALLEE_RET, 1u},
        /* Not in the adopted surface. */
        {A_ABSENT, XDK_CC_STDCALL, 1u, 0u, XDK_ABI_FROM_CALLEE_RET, 1u},
    };
    size_t refused = 0u;
    const size_t accepted = xdk_thunk_declare_abi_rows(rows, 8u, &refused);

    CHECK(accepted == 4u);
    CHECK(refused == 4u);
    CHECK(xdk_thunk_abi_count() == 4u);
    CHECK(xdk_thunk_abi_known(A_STD));
    CHECK(xdk_thunk_abi_known(A_THIS));
    CHECK(xdk_thunk_abi_known(A_FAST));
    CHECK(xdk_thunk_abi_known(A_VOTED_OK));
    CHECK(!xdk_thunk_abi_known(A_VOTED_THIN));
    CHECK(!xdk_thunk_abi_known(A_NO_RET));
    CHECK(!xdk_thunk_abi_known(A_BAD_SHAPE));
    CHECK(!xdk_thunk_abi_known(A_ABSENT));

    /* Callee pops its stack arguments plus the return address, registers excluded. */
    CHECK_POP(A_STD, 4u + 4u * 3u);
    CHECK_POP(A_THIS, 4u + 4u * 2u);
    CHECK_POP(A_FAST, 4u + 4u * 1u);
    CHECK_POP(A_VOTED_OK, 4u);
}

static void test_an_unknown_evidence_class_is_refused_not_defaulted(void)
{
    fresh();
    const xdk_abi_row rows[] = {
        {A_STD, XDK_CC_STDCALL, 1u, 0u, (xdk_abi_evidence)99, 5u},
    };
    size_t refused = 0u;
    CHECK(xdk_thunk_declare_abi_rows(rows, 1u, &refused) == 0u);
    CHECK(refused == 1u);
    CHECK(!xdk_thunk_abi_known(A_STD));
}

static void test_a_null_refused_pointer_is_allowed(void)
{
    fresh();
    const xdk_abi_row rows[] = {
        {A_STD, XDK_CC_STDCALL, 1u, 0u, XDK_ABI_FROM_CALLEE_RET, 1u},
    };
    CHECK(xdk_thunk_declare_abi_rows(rows, 1u, NULL) == 1u);
}

static void test_reinit_drops_every_declared_abi(void)
{
    fresh();
    const xdk_abi_row rows[] = {
        {A_STD, XDK_CC_STDCALL, 1u, 0u, XDK_ABI_FROM_CALLEE_RET, 1u},
    };
    CHECK(xdk_thunk_declare_abi_rows(rows, 1u, NULL) == 1u);
    CHECK(xdk_thunk_abi_known(A_STD));
    /* This is why main.c declares AFTER init and never before. */
    CHECK(xdk_thunk_init(SURFACE, SURFACE_COUNT));
    CHECK(!xdk_thunk_abi_known(A_STD));
}

static void test_the_generated_table_accounts_for_every_row(void)
{
    fresh();
    size_t refused = 0u;
    const size_t accepted = xdk_thunk_declare_generated_abis(&refused);
    /* Holds with or without a generated table, and is the check that a row is never
     * silently dropped: accepted plus refused is the table. Against this suite's own
     * synthetic surface every real address is outside it, so none can be accepted. */
    CHECK(accepted + refused == xdk_thunk_generated_abi_count());
    CHECK(accepted == 0u);
    CHECK(xdk_thunk_abi_count() == 0u);
}

int main(void)
{
    test_each_evidence_class_reaches_its_own_gate();
    test_an_unknown_evidence_class_is_refused_not_defaulted();
    test_a_null_refused_pointer_is_allowed();
    test_reinit_drops_every_declared_abi();
    test_the_generated_table_accounts_for_every_row();

    xdk_thunk_shutdown();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
