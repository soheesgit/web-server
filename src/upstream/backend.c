/* src/upstream/backend.c
 *
 * 레지스트리·그룹·백엔드 상태. 명세: upstream.h
 */
#include "upstream/upstream.h"

#include "core/log.h"
#include "upstream/internal_upstream.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void close_idle(struct backend *b)
{
    for (size_t i = 0; i < b->n_idle; i++) {
        close(b->idle[i]->fd);
        buf_cleanup(&b->idle[i]->rbuf);
        free(b->idle[i]);
    }
    b->n_idle = 0;
    free(b->idle);
    b->idle = NULL;
}

static void group_cleanup(struct upstream_group *g)
{
    if (g->backends != NULL) {
        for (size_t i = 0; i < g->n_backends; i++) {
            close_idle(&g->backends[i]);
        }
        free(g->backends);
        pthread_mutex_destroy(&g->mu);
    }
}

static int resolve(const backend_conf_t *bc, struct sockaddr_in *out, char *err, size_t errcap,
                   const char *group)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(bc->host, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        if (errcap > 0) {
            (void)snprintf(err, errcap, "upstream %s: cannot resolve %s: %s", group, bc->host,
                           gai_strerror(rc));
        }
        return -1;
    }
    memcpy(out, res->ai_addr, sizeof(*out));
    out->sin_port = htons(bc->port);
    freeaddrinfo(res);
    return 0;
}

static int group_init(struct upstream_group *g, struct upstream_registry *reg,
                      const upstream_conf_t *uc, char *err, size_t errcap)
{
    g->reg = reg;
    g->picker = uc->picker;
    g->bound_pct = uc->bound_pct;
    (void)snprintf(g->name, sizeof(g->name), "%s", uc->name);
    g->backends = calloc(uc->n_backends, sizeof(*g->backends));
    if (g->backends == NULL || pthread_mutex_init(&g->mu, NULL) != 0) {
        free(g->backends);
        g->backends = NULL;
        errno = ENOMEM;
        return -1;
    }
    g->n_backends = uc->n_backends;
    for (size_t i = 0; i < uc->n_backends; i++) {
        struct backend *b = &g->backends[i];
        const backend_conf_t *bc = &uc->backends[i];

        b->group = g;
        (void)snprintf(b->hostport, sizeof(b->hostport), "%s:%u", bc->host, (unsigned)bc->port);
        b->name_hash = upstream_hash(b->hostport, strlen(b->hostport));
        if (reg->max_idle > 0) {
            b->idle = calloc(reg->max_idle, sizeof(*b->idle));
            if (b->idle == NULL) {
                errno = ENOMEM;
                return -1;
            }
        }
        if (resolve(bc, &b->addr, err, errcap, uc->name) != 0) {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

upstream_registry_t *upstream_registry_new(const config_t *cfg, char *err, size_t errcap)
{
    upstream_registry_t *reg = calloc(1, sizeof(*reg));

    if (reg == NULL) {
        return NULL;
    }
    reg->fail_threshold = cfg->backend_fail_threshold;
    reg->cooldown_ms = cfg->backend_cooldown_ms;
    reg->max_idle = cfg->pool_max_idle > 0 ? (size_t)cfg->pool_max_idle : 0;
    if (cfg->n_upstreams > 0) {
        reg->groups = calloc(cfg->n_upstreams, sizeof(*reg->groups));
        if (reg->groups == NULL) {
            free(reg);
            return NULL;
        }
    }
    for (size_t i = 0; i < cfg->n_upstreams; i++) {
        reg->n_groups = i + 1; /* 실패해도 만든 그룹까지 정리하도록 먼저 센다 */
        if (group_init(&reg->groups[i], reg, &cfg->upstreams[i], err, errcap) != 0) {
            int e = errno;

            upstream_registry_free(reg);
            errno = e;
            return NULL;
        }
    }
    return reg;
}

void upstream_registry_free(upstream_registry_t *reg)
{
    int e = errno;

    if (reg == NULL) {
        return;
    }
    for (size_t i = 0; i < reg->n_groups; i++) {
        group_cleanup(&reg->groups[i]);
    }
    free(reg->groups);
    free(reg);
    errno = e;
}

upstream_group_t *upstream_registry_get(upstream_registry_t *reg, const char *name)
{
    for (size_t i = 0; i < reg->n_groups; i++) {
        if (strcmp(reg->groups[i].name, name) == 0) {
            return &reg->groups[i];
        }
    }
    return NULL;
}

struct upstream_group *upstream_lookup_cb(void *ctx, const char *name)
{
    if (ctx == NULL || name == NULL) {
        return NULL;
    }
    return upstream_registry_get(ctx, name);
}

size_t upstream_backend_count(const upstream_group_t *g)
{
    return g->n_backends;
}

void upstream_report(backend_t *b, bool ok, int64_t now_ms)
{
    struct upstream_group *g;
    bool newly_excluded = false;
    int fails;

    if (b == NULL) {
        return;
    }
    g = b->group;
    pthread_mutex_lock(&g->mu);
    if (ok) {
        b->fails = 0;
    } else {
        b->fails++;
        if (b->fails >= g->reg->fail_threshold) {
            newly_excluded = b->excluded_until_ms <= now_ms;
            b->excluded_until_ms = now_ms + g->reg->cooldown_ms;
        }
    }
    fails = b->fails;
    pthread_mutex_unlock(&g->mu);
    if (newly_excluded) {
        log_msg(LOG_WARN, "upstream %s: backend %s excluded for %d ms after %d failures", g->name,
                b->hostport, g->reg->cooldown_ms, fails);
    }
}

const char *backend_hostport(const backend_t *b)
{
    return b->hostport;
}
