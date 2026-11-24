/* src/app/main.c
 *
 * httpd 진입점. 디렉토리 책임과 관련 테스트: src/app/README.md
 *
 * 동작:
 *   1. config_load(argc, argv, &cfg, err). 1(--help)이면 config_print_usage 후 종료 코드 0.
 *      -1이면 stderr에 "httpd: <err>" 후 종료 코드 1.
 *   2. log_open(cfg.access_log, cfg.error_log, LOG_INFO). 실패하면 stderr에 쓰고 종료 코드 2.
 *   3. platform_ignore_sigpipe(). platform_raise_nofile(65536, NULL) (실패해도 계속).
 *   4. SIGINT·SIGTERM 핸들러: volatile sig_atomic_t 플래그만 1로 (sigaction, SA_RESTART 없음).
 *   5. app_new(&cfg, err). 실패하면 stderr에 "httpd: <err>"(예: "httpd: bind: Address already in
 *      use") 후 종료 코드 2.
 *   6. app_run(app, &stop). 7. app_free(app). 8. log_close().
 *   종료 코드: 0 정상(시그널 종료 포함), 1 설정 오류(--help는 0), 2 기동 실패, app_run 오류도 2.
 */
#include "app/app.h"
#include "core/config.h"
#include "core/log.h"
#include "core/platform.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void install_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
}

int main(int argc, char **argv)
{
    static config_t cfg; /* 수 KB라 스택 대신 정적 영역 */
    char err[512];
    app_t *app;
    int rc;

    err[0] = '\0';
    rc = config_load(argc, argv, &cfg, err, sizeof(err));
    if (rc == 1) {
        config_print_usage(argv[0]);
        return 0;
    }
    if (rc != 0) {
        fprintf(stderr, "httpd: %s\n", err);
        return 1;
    }
    if (log_open(cfg.access_log, cfg.error_log, LOG_INFO) != 0) {
        fprintf(stderr, "httpd: cannot open log files\n");
        return 2;
    }
    platform_ignore_sigpipe();
    (void)platform_raise_nofile(65536, NULL);
    install_signals();

    app = app_new(&cfg, err, sizeof(err));
    if (app == NULL) {
        fprintf(stderr, "httpd: %s\n", err);
        log_close();
        return 2;
    }
    rc = app_run(app, &g_stop);
    app_free(app);
    log_close();
    return rc == 0 ? 0 : 2;
}
