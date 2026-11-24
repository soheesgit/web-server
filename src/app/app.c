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

/* v2: conn이 기다리겠다는 것에 맞춰 클라이언트 fd 감시를 바꾼다. CONN_CLOSE면 -1.
 * WAIT_FD 동안은 conn이 추가 fd를 감시하므로 클라이언트 fd는 쉬게 둔다. */
static int rewatch(const conn_env_t *env, conn_t *c, conn_status_t st)
{
    unsigned ev = st == CONN_WANT_READ ? EV_READ : st == CONN_WANT_WRITE ? EV_WRITE : 0u;

    if (st == CONN_CLOSE) {
        return -1;
    }
    return engine_watch(env->engine, conn_fd(c), ev, c) == 0 ? 0 : -1;
}

static int cb_event(void *ctx, void *obj, int fd, unsigned events, int64_t now_ms)
{
    return rewatch(ctx, obj, conn_on_event(obj, fd, events, now_ms));
}

static int cb_timeout(void *ctx, void *obj, int64_t now_ms)
{
    return rewatch(ctx, obj, conn_on_timeout(obj, now_ms));
}

static int64_t cb_deadline(void *ctx, void *obj)
{
    (void)ctx;
    return conn_deadline_ms(obj);
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
    cb_accept, cb_serve, cb_event, cb_timeout, cb_deadline, cb_close, cb_busy,
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
        (void)snprintf(err, errcap, "engine: cannot create (out of memory or bad workers)");
        goto fail;
    }
    if (kind == ENGINE_EVENT_LOOP) {
        a->env.engine = a->engine; /* conn이 추가 fd를 등록할 엔진 */
    }
    return a;

fail:
    app_free(a);
    return NULL;
}

int app_run(app_t *a, volatile sig_atomic_t *stop)
{
    int rc;

    if (a->cfg.engine == CFG_ENGINE_LOOP) {
        log_msg(LOG_INFO, "listening on %s:%u (event loop)", a->cfg.listen_host,
                (unsigned)a->cfg.port);
    } else {
        log_msg(LOG_INFO, "listening on %s:%u (thread pool, %d workers)", a->cfg.listen_host,
                (unsigned)a->cfg.port, a->cfg.workers);
    }
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
