/* tests/unit/run_unit.c
 *
 * C 단위·계약 테스트 러너. 테스트마다 fork한 자식에서 실행하고 pending.txt로 판정한다.
 * 판정 규칙과 출력 형식: docs/TESTING.md
 *
 * 사용법: run_unit [--pending FILE] [--only ID접두사] [--list] [-v]
 */
#include "unit/testlib.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PENDING_MAX 1024
#define OUT_MAX 4096

static char *g_pending[PENDING_MAX];
static size_t g_npending;

static void load_pending(const char *path)
{
    FILE *f;
    char line[256];

    if (path == NULL) {
        return;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        return; /* 파일이 없으면 pending 없음 */
    }
    while (fgets(line, sizeof(line), f) != NULL && g_npending < PENDING_MAX) {
        char *p = line;
        char *hash = strchr(p, '#');
        size_t n;

        if (hash != NULL) {
            *hash = '\0';
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        n = strcspn(p, " \t\r\n");
        if (n == 0) {
            continue;
        }
        p[n] = '\0';
        g_pending[g_npending] = strdup(p);
        if (g_pending[g_npending] != NULL) {
            g_npending++;
        }
    }
    fclose(f);
}

static int is_pending(const char *id)
{
    for (size_t i = 0; i < g_npending; i++) {
        if (strcmp(g_pending[i], id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int cmp_entry(const void *a, const void *b)
{
    const test_entry_t *x = a;
    const test_entry_t *y = b;
    return strcmp(x->id, y->id);
}

/* 자식에서 테스트 하나를 실행하고 결과를 돌려준다. 0 통과, 1 실패, 2 건너뜀. reason에 사유. */
static int run_one(const test_entry_t *t, char *reason, size_t cap)
{
    int fds[2];
    pid_t pid;
    char out[OUT_MAX];
    size_t used = 0;
    int status;

    reason[0] = '\0';
    if (pipe(fds) != 0) {
        snprintf(reason, cap, "pipe: errno %d", errno);
        return 1;
    }
    fflush(NULL);
    pid = fork();
    if (pid < 0) {
        snprintf(reason, cap, "fork: errno %d", errno);
        close(fds[0]);
        close(fds[1]);
        return 1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        signal(SIGPIPE, SIG_DFL);
        alarm(5);
        t->fn();
        fflush(NULL);
        exit(testlib_failed() ? 1 : 0);
    }
    close(fds[1]);
    for (;;) {
        char tmp[1024];
        ssize_t r = read(fds[0], tmp, sizeof(tmp));
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        if (used < sizeof(out) - 1) {
            size_t take = (size_t)r;
            if (take > sizeof(out) - 1 - used) {
                take = sizeof(out) - 1 - used;
            }
            memcpy(out + used, tmp, take);
            used += take;
        }
    }
    out[used] = '\0';
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            snprintf(reason, cap, "waitpid: errno %d", errno);
            return 1;
        }
    }

    /* 첫 줄을 사유로 쓴다 */
    {
        size_t n = strcspn(out, "\n");
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(reason, out, n);
        reason[n] = '\0';
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) {
            return 0;
        }
        if (code == TEST_EXIT_SKIP) {
            return 2;
        }
        if (reason[0] == '\0') {
            snprintf(reason, cap, "exit %d", code);
        }
        return 1;
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        char sigmsg[64];
        snprintf(sigmsg, sizeof(sigmsg), "%s",
                 sig == SIGALRM ? "timeout (5s)" : (sig == SIGSEGV ? "SIGSEGV" : "signal"));
        if (sig != SIGALRM && sig != SIGSEGV) {
            snprintf(sigmsg, sizeof(sigmsg), "signal %d", sig);
        }
        if (reason[0] == '\0') {
            snprintf(reason, cap, "%s", sigmsg);
        } else {
            size_t l = strlen(reason);
            snprintf(reason + l, cap - l, " [%s]", sigmsg);
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *pending = NULL;
    const char *only = NULL;
    int list = 0;
    int verbose = 0;
    size_t n;
    test_entry_t *t;
    int npass = 0, npend = 0, nfail = 0, nunexp = 0, nskip = 0, total = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pending") == 0 && i + 1 < argc) {
            pending = argv[++i];
        } else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else if (strcmp(argv[i], "--list") == 0) {
            list = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else {
            fprintf(stderr, "usage: %s [--pending FILE] [--only PREFIX] [--list] [-v]\n", argv[0]);
            return 2;
        }
    }

    register_core_tests();
    register_common_tests();
    register_downstream_tests();
    register_router_tests();
    register_handler_tests();
    register_upstream_tests();
    register_engine_tests();

    n = testlib_count();
    t = testlib_entries();
    qsort(t, n, sizeof(*t), cmp_entry);
    load_pending(pending);

    for (size_t i = 0; i < n; i++) {
        char reason[512];
        const char *label;
        int r;
        int pend;

        if (only != NULL && strncmp(t[i].id, only, strlen(only)) != 0) {
            continue;
        }
        if (list) {
            printf("%s  %s\n", t[i].id, t[i].name);
            continue;
        }
        total++;
        pend = is_pending(t[i].id);
        r = run_one(&t[i], reason, sizeof(reason));
        if (r == 2) {
            label = "SKIP";
            nskip++;
        } else if (r == 0) {
            label = pend ? "UNEXPECTED PASS" : "PASS";
            if (pend) {
                nunexp++;
            } else {
                npass++;
            }
        } else {
            label = pend ? "PENDING" : "FAIL";
            if (pend) {
                npend++;
            } else {
                nfail++;
            }
        }
        if (r == 2) {
            printf("%-17s%s  %s  (%s)\n", label, t[i].id, t[i].name, reason);
        } else if (verbose && r == 1 && reason[0] != '\0') {
            printf("%-17s%s  %s  (%s)\n", label, t[i].id, t[i].name, reason);
        } else {
            printf("%-17s%s  %s\n", label, t[i].id, t[i].name);
        }
        fflush(stdout);
    }
    if (list) {
        return 0;
    }
    printf("---\n%d total: %d pass, %d pending, %d fail, %d unexpected, %d skip\n", total, npass,
           npend, nfail, nunexp, nskip);
    return (nfail > 0 || nunexp > 0) ? 1 : 0;
}
