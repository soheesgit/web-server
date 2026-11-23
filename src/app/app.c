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
        .on_event = NULL,
        .on_timeout = NULL,
        .deadline_ms = NULL,
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
    a->conn_env = (conn_env_t){.cfg = &a->cfg,
                               .lim = &a->limits,
                               .router = a->router,
                               .engine = NULL,
                               .nonblocking = false};
    a->engine = engine_new(cfg->engine == CFG_ENGINE_POOL ? ENGINE_THREAD_POOL : ENGINE_EVENT_LOOP,
                           cfg->workers, cfg->queue_len, &callbacks, a);
    if (a->engine == NULL) {
        if (err != NULL && errcap > 0 && err[0] == '\0')
            (void)snprintf(err, errcap, "engine: %s",
                           errno == ENOTSUP ? "event loop is not enabled" : "cannot create");
        goto fail;
    }
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
