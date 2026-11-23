/* src/app/app.c
 *
 * 서버 조립. 명세: app.h
 */
#include "app/app.h"

#include "core/log.h"
#include "core/platform.h"
#include "downstream/conn.h"
#include "downstream/listener.h"
#include "engine/engine.h"
#include "handler/plan.h"
#include "router/router.h"
#include "upstream/upstream.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct app {
    config_t cfg;
    http_limits_t limits;
    upstream_registry_t *upstreams;
    router_t *router;
    int listen_fd;
    conn_env_t conn_env;
    engine_t *engine;
};

static void *on_accept(void *ctx, int fd, const struct sockaddr_storage *peer, int64_t now_ms)
{
    app_t *a = ctx;
    return conn_new(fd, peer, &a->conn_env, now_ms);
}

static void serve_blocking(void *ctx, void *obj)
{
    (void)ctx;
    conn_serve_blocking(obj);
}

/* v2: 연결이 다음에 기다릴 것을 엔진에 알린다. 0 계속, -1 닫기. */
static int watch_next(app_t *a, conn_t *c, conn_status_t st)
{
    unsigned events = st == CONN_WANT_READ ? EV_READ : st == CONN_WANT_WRITE ? EV_WRITE : 0;
    if (st == CONN_CLOSE)
        return -1;
    /* CONN_WAIT_FD면 클라이언트 fd는 쉬게 둔다(추가 fd는 conn이 이미 등록했다). */
    return engine_watch(a->engine, conn_fd(c), events, c) == 0 ? 0 : -1;
}

static int on_event(void *ctx, void *obj, int fd, unsigned events, int64_t now_ms)
{
    return watch_next(ctx, obj, conn_on_event(obj, fd, events, now_ms));
}

static int on_timeout(void *ctx, void *obj, int64_t now_ms)
{
    return watch_next(ctx, obj, conn_on_timeout(obj, now_ms));
}

static int64_t deadline_ms(void *ctx, void *obj)
{
    (void)ctx;
    return conn_deadline_ms(obj);
}

static void on_close(void *ctx, void *obj)
{
    (void)ctx;
    conn_free(obj);
}

static void on_busy(void *ctx, int fd)
{
    static const char response[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n"
                                   "Content-Length: 0\r\n\r\n";
    size_t sent = 0;
    (void)ctx;
    while (sent < sizeof(response) - 1) {
        ssize_t n = platform_send(fd, response + sent, sizeof(response) - 1 - sent);
        if (n > 0)
            sent += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    (void)close(fd);
}

app_t *app_new(const config_t *cfg, char *err, size_t errcap)
{
    static const engine_callbacks_t callbacks = {
        .on_accept = on_accept,
        .serve_blocking = serve_blocking,
        .on_event = on_event,
        .on_timeout = on_timeout,
        .deadline_ms = deadline_ms,
        .on_close = on_close,
        .on_busy = on_busy,
    };
    app_t *a;
    if (cfg == NULL) {
        errno = EINVAL;
        return NULL;
    }
    a = calloc(1, sizeof(*a));
    if (a == NULL)
        return NULL;
    a->listen_fd = -1;
    a->cfg = *cfg;
    a->limits = (http_limits_t){cfg->max_head, cfg->max_uri, cfg->max_headers, cfg->max_body};
    a->upstreams = upstream_registry_new(&a->cfg, err, errcap);
    if (a->upstreams == NULL)
        goto fail;
    a->router = router_new(&a->cfg, upstream_lookup_cb, a->upstreams, err, errcap);
    if (a->router == NULL)
        goto fail;
    a->listen_fd = listener_open(a->cfg.listen_host, a->cfg.port, a->cfg.backlog, err, errcap);
    if (a->listen_fd < 0)
        goto fail;
    a->engine = engine_new(cfg->engine == CFG_ENGINE_POOL ? ENGINE_THREAD_POOL : ENGINE_EVENT_LOOP,
                           cfg->workers, cfg->queue_len, &callbacks, a);
    if (a->engine == NULL) {
        if (err != NULL && errcap > 0 && err[0] == '\0')
            (void)snprintf(err, errcap, "engine: cannot create");
        goto fail;
    }
    a->conn_env = (conn_env_t){.cfg = &a->cfg,
                               .lim = &a->limits,
                               .router = a->router,
                               .engine = cfg->engine == CFG_ENGINE_POOL ? NULL : a->engine,
                               .nonblocking = cfg->engine != CFG_ENGINE_POOL};
    return a;
fail:
    app_free(a);
    return NULL;
}

int app_run(app_t *a, volatile sig_atomic_t *stop)
{
    if (a == NULL || stop == NULL) {
        errno = EINVAL;
        return -1;
    }
    log_msg(LOG_INFO, "httpd listening on %s:%u (%s)", a->cfg.listen_host, (unsigned)a->cfg.port,
            a->cfg.engine == CFG_ENGINE_POOL ? "pool" : "loop");
    if (engine_run(a->engine, a->listen_fd, stop, a->cfg.shutdown_grace_ms) != 0)
        return -1;
    log_msg(LOG_INFO, "httpd stopped");
    return 0;
}

void app_free(app_t *a)
{
    int saved = errno;
    if (a != NULL) {
        engine_free(a->engine);
        listener_close(a->listen_fd);
        router_free(a->router);
        upstream_registry_free(a->upstreams);
        free(a);
    }
    errno = saved;
}
