/* src/app/main.c
 *
 * httpd 진입점. 디렉토리 책임과 관련 테스트: src/app/README.md
 *
 * 최종 동작 (구현 목표):
 *   1. config_load(argc, argv, &cfg, err). 1(--help)이면 config_print_usage 후 종료 코드 0.
 *      -1이면 stderr에 "httpd: <err>" 후 종료 코드 1.
 *   2. log_open(cfg.access_log, cfg.error_log, LOG_INFO). 실패하면 stderr에 쓰고 종료 코드 2.
 *   3. platform_ignore_sigpipe(). platform_raise_nofile(65536, NULL) (실패해도 계속).
 *   4. SIGINT·SIGTERM 핸들러: volatile sig_atomic_t 플래그만 1로 (sigaction, SA_RESTART 없음).
 *   5. app_new(&cfg, err). 실패하면 stderr에 "httpd: <err>"(예: "httpd: bind: Address already in
 *      use") 후 종료 코드 2.
 *   6. app_run(app, &stop). 7. app_free(app). 8. log_close().
 *   종료 코드: 0 정상(시그널 종료 포함), 1 설정 오류(--help는 0), 2 기동 실패, app_run 오류도 2.
 *
 * initial commit 동작 (작업 지시문 7.3절 예외): -h/--help면 사용법을 출력하고 0으로 종료한다.
 * 그 밖에는 config_load를 부르고(스텁이므로 항상 실패) stderr에 "httpd: not implemented"를 쓰고
 * 2로 종료한다.
 */
#include "app/app.h"
#include "core/config.h"
#include "core/log.h"
#include "core/platform.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t stop_requested;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static const char *error_text(int code, char *buf, size_t cap)
{
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    return strerror_r(code, buf, cap);
#else
    if (strerror_r(code, buf, cap) != 0) (void)snprintf(buf, cap, "error %d", code);
    return buf;
#endif
}

int main(int argc, char **argv)
{
    config_t cfg;
    char err[512];
    char sysmsg[128];
    app_t *app;
    struct sigaction sa;
    int loaded, rc;
    err[0] = '\0';
    loaded = config_load(argc, argv, &cfg, err, sizeof(err));
    if (loaded == 1) {
        config_print_usage(argv[0]);
        return 0;
    }
    if (loaded != 0) {
        fprintf(stderr, "httpd: %s\n", err[0] == '\0' ? "invalid configuration" : err);
        return 1;
    }
    if (log_open(cfg.access_log, cfg.error_log, LOG_INFO) != 0) {
        fprintf(stderr, "httpd: log_open: %s\n", error_text(errno, sysmsg, sizeof(sysmsg)));
        return 2;
    }
    platform_ignore_sigpipe();
    (void)platform_raise_nofile(65536, NULL);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        fprintf(stderr, "httpd: sigaction: %s\n", error_text(errno, sysmsg, sizeof(sysmsg)));
        log_close();
        return 2;
    }
    app = app_new(&cfg, err, sizeof(err));
    if (app == NULL) {
        fprintf(stderr, "httpd: %s\n", err[0] == '\0' ? error_text(errno, sysmsg, sizeof(sysmsg)) : err);
        log_close();
        return 2;
    }
    rc = app_run(app, &stop_requested);
    if (rc != 0) fprintf(stderr, "httpd: app_run: %s\n", error_text(errno, sysmsg, sizeof(sysmsg)));
    app_free(app);
    log_close();
    return rc == 0 ? 0 : 2;
}
