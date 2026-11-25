/* 설정 오류는 1, 기동·엔진 오류는 2, 정상 종료는 0. */
#include "app/app.h"

#include <stdio.h>

#include "core/config.h"
#include "core/log.h"
#include "core/platform.h"

static volatile sig_atomic_t stopping;

static void request_stop(int signum)
{
    (void)signum;
    stopping = 1;
}

int main(int argc, char **argv)
{
    config_t cfg;
    char err[512] = "";
    int rc = config_load(argc, argv, &cfg, err, sizeof(err));
    if (rc == 1) {
        config_print_usage(argv[0]);
        return 0;
    }
    if (rc < 0) {
        fprintf(stderr, "httpd: %s\n", err);
        return 1;
    }
    if (log_open(cfg.access_log, cfg.error_log, LOG_INFO) != 0) {
        fprintf(stderr, "httpd: cannot open logs\n");
        return 2;
    }
    platform_ignore_sigpipe();
    (void)platform_raise_nofile(65536, NULL);
    struct sigaction act = {0};
    act.sa_handler = request_stop;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGINT, &act, NULL) != 0 || sigaction(SIGTERM, &act, NULL) != 0) {
        fprintf(stderr, "httpd: cannot install signal handlers\n");
        log_close();
        return 2;
    }
    app_t *app = app_new(&cfg, err, sizeof(err));
    if (app == NULL) {
        fprintf(stderr, "httpd: %s\n", err);
        log_close();
        return 2;
    }
    rc = app_run(app, &stopping);
    app_free(app);
    log_close();
    return rc == 0 ? 0 : 2;
}
