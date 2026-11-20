/* src/core/config.c
 *
 * 기본값·설정 파일·CLI → config_t. 명세: config.h
 *
 * config_defaults와 config_print_usage는 빌드·테스트 하네스를 위해 initial commit에서 구현했다
 * (작업 지시문 7.3절 예외). config_load는 스텁이다.
 */
#include "core/config.h"
#include "core/stub.h"

#include <stddef.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void config_defaults(config_t *out_cfg)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int workers = 4;

    if (ncpu >= 1) {
        workers = ncpu > 512 ? 1024 : (int)ncpu * 2;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    (void)snprintf(out_cfg->listen_host, sizeof(out_cfg->listen_host), "%s", "0.0.0.0");
    out_cfg->port = 8080;
    out_cfg->backlog = 128;
    (void)snprintf(out_cfg->root, sizeof(out_cfg->root), "%s", "./www");
    out_cfg->engine = CFG_ENGINE_POOL;
    out_cfg->workers = workers;
    out_cfg->queue_len = 1024;
    out_cfg->max_head = 8192;
    out_cfg->max_uri = 2048;
    out_cfg->max_headers = 64;
    out_cfg->max_body = 10u * 1024u * 1024u;
    out_cfg->header_timeout_ms = 10000;
    out_cfg->body_timeout_ms = 30000;
    out_cfg->keepalive_timeout_ms = 5000;
    out_cfg->max_keepalive_requests = 100;
    out_cfg->cgi_timeout_ms = 10000;
    out_cfg->cgi_max_head = 8192;
    out_cfg->upstream_connect_timeout_ms = 2000;
    out_cfg->upstream_response_timeout_ms = 15000;
    out_cfg->backend_fail_threshold = 3;
    out_cfg->backend_cooldown_ms = 10000;
    out_cfg->pool_max_idle = 8;
    out_cfg->shutdown_grace_ms = 5000;
    out_cfg->access_log[0] = '\0';
    out_cfg->error_log[0] = '\0';
    out_cfg->n_upstreams = 0;
    out_cfg->n_routes = 0;
}

int config_load(int argc, char **argv, config_t *out_cfg, char *err, size_t errcap)
{
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(out_cfg);
    UNUSED(err);
    UNUSED(errcap);
    /* TODO(core): 구현 — U-CORE-17, U-CORE-21, E-CF-02, E-CF-04 */
    STUB_RETURN(-1);
}

void config_print_usage(const char *prog)
{
    if (prog == NULL) {
        prog = "httpd";
    }
    printf("Usage: %s [-p port] [-r root] [-c file] [-w workers] [-e pool|loop] [-h]\n", prog);
    printf("  -p <port>     listen port (1-65535, default 8080)\n");
    printf("  -r <root>     static file root directory (default ./www)\n");
    printf("  -c <file>     configuration file (grammar: src/core/config.h)\n");
    printf("  -w <workers>  worker threads for the pool engine (default: CPU cores x 2)\n");
    printf("  -e pool|loop  concurrency engine (default pool)\n");
    printf("  -h, --help    show this help and exit\n");
}
