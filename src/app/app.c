/* src/app/app.c
 *
 * 서버 조립. 명세: app.h
 */
#include "app/app.h"

#include "common/http_status.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/platform.h"
#include "downstream/conn.h"
#include "downstream/listener.h"
#include "engine/engine.h"
#include "router/router.h"
#include "upstream/upstream.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct app {
    config_t cfg;
    http_limits_t lim;
    upstream_registry_t *reg;
    router_t *router;
    int lfd;
    conn_env_t env;
    engine_t *engine;
};

static void *cb_accept(void *ctx, int fd, const struct sockaddr_storage *peer, int64_t now_ms)
{
    return conn_new(fd, peer, ctx, now_ms);
}

static void cb_serve(void *ctx, void *obj)
{
    (void)ctx;
    conn_serve_blocking(obj);
}

static void cb_close(void *ctx, void *obj)
{
    (void)ctx;
    conn_free(obj);
}

/* D-005: 작업 큐가 가득이면 503 + Connection: close 후 닫는다. */
static void cb_busy(void *ctx, int fd)
{
    buf_t body;
    char head[256];
    int n;

    (void)ctx;
    if (buf_init(&body, 512, 4096) == 0 && http_error_body(503, &body) == 0) {
        n = snprintf(head, sizeof(head),
                     "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/html; "
                     "charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     body.len);
        if (n > 0 && (size_t)n < sizeof(head) && platform_send(fd, head, (size_t)n) == n) {
            (void)platform_send(fd, body.data, body.len);
        }
    }
    buf_cleanup(&body);
    close(fd);
}

static const engine_callbacks_t CONN_CALLBACKS = {
    cb_accept, cb_serve, NULL, NULL, NULL, cb_close, cb_busy,
};

app_t *app_new(const config_t *cfg, char *err, size_t errcap)
{
    app_t *a = calloc(1, sizeof(*a));
    engine_kind_t kind;

    if (a == NULL) {
        (void)snprintf(err, errcap, "out of memory");
        return NULL;
    }
    a->cfg = *cfg;
    a->lfd = -1;
    a->lim.max_head = a->cfg.max_head;
    a->lim.max_uri = a->cfg.max_uri;
    a->lim.max_headers = a->cfg.max_headers;
    a->lim.max_body = a->cfg.max_body;

    a->reg = upstream_registry_new(&a->cfg, err, errcap);
    if (a->reg == NULL) {
        goto fail;
    }
    a->router = router_new(&a->cfg, upstream_lookup_cb, a->reg, err, errcap);
    if (a->router == NULL) {
        goto fail;
    }
    a->lfd = listener_open(a->cfg.listen_host, a->cfg.port, a->cfg.backlog, err, errcap);
    if (a->lfd < 0) {
        goto fail;
    }
    kind = a->cfg.engine == CFG_ENGINE_LOOP ? ENGINE_EVENT_LOOP : ENGINE_THREAD_POOL;
    a->env.cfg = &a->cfg;
    a->env.lim = &a->lim;
    a->env.router = a->router;
    a->env.engine = NULL;
    a->env.nonblocking = kind == ENGINE_EVENT_LOOP;
    a->engine = engine_new(kind, a->cfg.workers, a->cfg.queue_len, &CONN_CALLBACKS, &a->env);
    if (a->engine == NULL) {
        (void)snprintf(err, errcap, "engine: %s",
                       errno == ENOSYS ? "event loop is not implemented yet"
                                       : "cannot create (out of memory or bad workers)");
        goto fail;
    }
    return a;

fail:
    app_free(a);
    return NULL;
}

int app_run(app_t *a, volatile sig_atomic_t *stop)
{
    int rc;

    log_msg(LOG_INFO, "listening on %s:%u (%s, %d workers)", a->cfg.listen_host,
            (unsigned)a->cfg.port, a->cfg.engine == CFG_ENGINE_LOOP ? "loop" : "pool",
            a->cfg.workers);
    rc = engine_run(a->engine, a->lfd, stop, a->cfg.shutdown_grace_ms);
    log_msg(LOG_INFO, "stopped (%s)", rc == 0 ? "ok" : "engine error");
    return rc;
}

void app_free(app_t *a)
{
    int e = errno;

    if (a == NULL) {
        return;
    }
    engine_free(a->engine);
    if (a->lfd >= 0) {
        listener_close(a->lfd);
    }
    router_free(a->router);
    upstream_registry_free(a->reg);
    free(a);
    errno = e;
}
