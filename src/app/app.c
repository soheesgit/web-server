/* 설정, 레지스트리, 라우터와 연결 엔진 조립. */
#include "app/app.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "core/platform.h"
#include "downstream/conn.h"
#include "downstream/listener.h"
#include "engine/engine.h"
#include "upstream/upstream.h"

struct app {
    config_t cfg;
    http_limits_t limits;
    upstream_registry_t *registry;
    router_t *router;
    int listener;
    conn_env_t env;
    engine_t *engine;
};

static void *accept_conn(void *ctx, int fd, const struct sockaddr_storage *peer, int64_t now)
{
    app_t *a = ctx;
    return conn_new(fd, peer, &a->env, now);
}

static void serve_conn(void *ctx, void *obj)
{
    (void)ctx;
    conn_serve_blocking(obj);
}

static void close_conn(void *ctx, void *obj)
{
    (void)ctx;
    conn_free(obj);
}

static int update_conn(app_t *a, conn_t *c, conn_status_t next)
{
    if (next == CONN_CLOSE) {
        return -1;
    }
    if (next == CONN_WAIT_FD) {
        return 0; /* conn이 추가 fd와 클라이언트 취소 감시를 설정했다. */
    }
    return engine_watch(a->engine, conn_fd(c), next == CONN_WANT_READ ? EV_READ : EV_WRITE, c);
}

static int event_conn(void *ctx, void *obj, int fd, unsigned events, int64_t now)
{
    return update_conn(ctx, obj, conn_on_event(obj, fd, events, now));
}

static int timeout_conn(void *ctx, void *obj, int64_t now)
{
    return update_conn(ctx, obj, conn_on_timeout(obj, now));
}

static int64_t deadline_conn(void *ctx, void *obj)
{
    (void)ctx;
    return conn_deadline_ms(obj);
}

static void reject_busy(void *ctx, int fd)
{
    static const char response[] =
        "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    (void)ctx;
    if (platform_set_nonblock(fd) == 0) {
        size_t sent = 0;
        while (sent < sizeof(response) - 1) {
            ssize_t n = platform_send(fd, response + sent, sizeof(response) - 1 - sent);
            if (n <= 0) {
                break;
            }
            sent += (size_t)n;
        }
    }
    close(fd);
}

app_t *app_new(const config_t *cfg, char *err, size_t errcap)
{
    static const engine_callbacks_t callbacks = {
        .on_accept = accept_conn,
        .serve_blocking = serve_conn,
        .on_event = event_conn,
        .on_timeout = timeout_conn,
        .deadline_ms = deadline_conn,
        .on_close = close_conn,
        .on_busy = reject_busy,
    };
    app_t *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        if (errcap > 0) {
            (void)snprintf(err, errcap, "app: out of memory");
        }
        return NULL;
    }
    a->listener = -1;
    a->cfg = *cfg;
    a->limits = (http_limits_t){cfg->max_head, cfg->max_uri, cfg->max_headers, cfg->max_body};
    a->registry = upstream_registry_new(&a->cfg, err, errcap);
    if (a->registry == NULL) {
        goto fail;
    }
    a->router = router_new(&a->cfg, upstream_lookup_cb, a->registry, err, errcap);
    if (a->router == NULL) {
        goto fail;
    }
    a->listener = listener_open(cfg->listen_host, cfg->port, cfg->backlog, err, errcap);
    if (a->listener < 0) {
        goto fail;
    }
    engine_kind_t kind = cfg->engine == CFG_ENGINE_LOOP ? ENGINE_EVENT_LOOP : ENGINE_THREAD_POOL;
    a->engine = engine_new(kind, cfg->workers, cfg->queue_len, &callbacks, a);
    if (a->engine == NULL) {
        if (errcap > 0) {
            (void)snprintf(err, errcap, "engine: startup failed (errno %d)", errno);
        }
        goto fail;
    }
    a->env = (conn_env_t){
        .cfg = &a->cfg,
        .lim = &a->limits,
        .router = a->router,
        .engine = kind == ENGINE_EVENT_LOOP ? a->engine : NULL,
        .nonblocking = kind == ENGINE_EVENT_LOOP,
    };
    return a;

fail:
    app_free(a);
    return NULL;
}

int app_run(app_t *a, volatile sig_atomic_t *stop)
{
    log_msg(LOG_INFO, "listening on %s:%u", a->cfg.listen_host, (unsigned)a->cfg.port);
    int rc = engine_run(a->engine, a->listener, stop, a->cfg.shutdown_grace_ms);
    int saved = errno;
    log_msg(LOG_INFO, "server stopped");
    errno = saved;
    return rc;
}

void app_free(app_t *a)
{
    int saved = errno;
    if (a != NULL) {
        engine_free(a->engine);
        listener_close(a->listener);
        router_free(a->router);
        upstream_registry_free(a->registry);
        free(a);
    }
    errno = saved;
}
