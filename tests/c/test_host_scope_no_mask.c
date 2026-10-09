/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T827: host_run_scope_push/pop and the jump back to a scope make NO signal mask system call (they were four
 * rt_sigprocmask calls per scope, 28 percent of the guest thread CPU in the intro movies). A seccomp filter makes
 * rt_sigprocmask fail with EPERM in a child process: push, pop, nested scopes and a fault caught by the inner scope
 * must all still work. Skips (exit 77) where seccomp cannot be installed.
 */
#define _GNU_SOURCE
#include "host_runtime.h"

#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define STEP(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); _exit(3); } } while (0)

static int install_filter(void)
{
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_rt_sigprocmask, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {(unsigned short)(sizeof filter / sizeof filter[0]), filter};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
}

static int child(void)
{
    if (install_filter() != 0) return 77;
    /* The filter really bites: a mask change now fails. */
    sigset_t probe;
    sigemptyset(&probe);
    STEP(sigprocmask(SIG_BLOCK, &probe, NULL) != 0 && errno == EPERM);

    host_run_arm();
    static volatile host_run_scope outer = HOST_RUN_SCOPE_INITIALIZER;
    static volatile host_run_scope inner = HOST_RUN_SCOPE_INITIALIZER;
    STEP(host_run_scope_init(&outer));
    if (sigsetjmp(*host_run_scope_jmp(&outer), 0) != 0) _exit(4); /* the outer scope must not be the target */
    STEP(host_run_scope_push(&outer));
    STEP(host_run_scope_depth() == 1u);
    STEP(host_run_scope_init(&inner));
    volatile int caught = 0;
    if (sigsetjmp(*host_run_scope_jmp(&inner), 0) == 0) {
        STEP(host_run_scope_push(&inner));
        STEP(host_run_scope_depth() == 2u);
        *(volatile int *)0 = 1; /* a real fault, caught by the INNER scope */
        _exit(5);
    }
    caught = 1;
    STEP(caught);
    STEP(host_run_result()->reason == HOST_STOP_FAULT);
    STEP(host_run_scope_depth() == 2u);
    STEP(host_run_scope_pop(&inner));
    STEP(host_run_scope_depth() == 1u);
    STEP(host_run_scope_pop(&outer));
    STEP(host_run_scope_depth() == 0u);
    /* Push and pop again, many times, still with no mask call. */
    for (unsigned index = 0; index < 1000u; index++) {
        STEP(host_run_scope_init(&inner));
        if (sigsetjmp(*host_run_scope_jmp(&inner), 0) != 0) _exit(6);
        STEP(host_run_scope_push(&inner));
        STEP(host_run_scope_pop(&inner));
    }
    host_run_disarm();
    return 0;
}

int main(void)
{
    const pid_t pid = fork();
    if (pid == 0) _exit(child());
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        fprintf(stderr, "fork or wait failed\n");
        return 1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
        printf("test_host_scope_no_mask: skipped, seccomp unavailable\n");
        return 77;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child failed, status 0x%x\n", status);
        return 1;
    }
    printf("test_host_scope_no_mask: all checks passed\n");
    return 0;
}
