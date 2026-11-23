/* src/upstream/backend.c
 *
 * 레지스트리·그룹·백엔드 상태. 명세: upstream.h
 */
#include "upstream/upstream.h"

#include "core/log.h"
#include "upstream/internal_state.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

upstream_registry_t *upstream_registry_new(const config_t *cfg, char *err, size_t errcap)
{
    upstream_registry_t *reg;
    size_t i, j;
    if (cfg == NULL) { errno = EINVAL; return NULL; }
    reg = calloc(1, sizeof(*reg));
    if (reg == NULL) return NULL;
    reg->n_groups = cfg->n_upstreams;
    reg->connect_timeout_ms = cfg->upstream_connect_timeout_ms;
    reg->response_timeout_ms = cfg->upstream_response_timeout_ms;
    if (reg->n_groups > 0) {
        reg->groups = calloc(reg->n_groups, sizeof(*reg->groups));
        if (reg->groups == NULL) { free(reg); return NULL; }
    }
    for (i = 0; i < reg->n_groups; i++) {
        const upstream_conf_t *src = &cfg->upstreams[i];
        upstream_group_t *g = &reg->groups[i];
        (void)snprintf(g->name, sizeof(g->name), "%s", src->name);
        g->n_backends = src->n_backends;
        if (pthread_mutex_init(&g->lock, NULL) != 0) { errno = ENOMEM; goto fail; }
        g->lock_initialized = true;
        g->backends = calloc(g->n_backends, sizeof(*g->backends));
        if (g->backends == NULL && g->n_backends != 0) goto fail;
        for (j = 0; j < g->n_backends; j++) {
            struct addrinfo hints, *res = NULL;
            char service[8];
            backend_t *b = calloc(1, sizeof(*b));
            int gai;
            if (b == NULL) goto fail;
            g->backends[j] = b;
            (void)snprintf(service, sizeof(service), "%u", (unsigned)src->backends[j].port);
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            gai = getaddrinfo(src->backends[j].host, service, &hints, &res);
            if (gai != 0 || res == NULL) {
                if (err != NULL && errcap > 0)
                    (void)snprintf(err, errcap, "upstream %s: cannot resolve %s: %s", src->name,
                                   src->backends[j].host, gai_strerror(gai));
                if (res != NULL) freeaddrinfo(res);
                errno = EINVAL;
                goto fail;
            }
            memcpy(&b->addr, res->ai_addr, sizeof(b->addr));
            freeaddrinfo(res);
            if (pthread_mutex_init(&b->lock, NULL) != 0) { errno = ENOMEM; goto fail; }
            b->lock_initialized = true;
            b->idle_limit = (size_t)cfg->pool_max_idle;
            b->fail_threshold = cfg->backend_fail_threshold;
            b->cooldown_ms = cfg->backend_cooldown_ms;
            (void)snprintf(b->hostport, sizeof(b->hostport), "%s:%u", src->backends[j].host,
                           (unsigned)src->backends[j].port);
        }
    }
    return reg;
fail:
    upstream_registry_free(reg);
    return NULL;
}

void upstream_registry_free(upstream_registry_t *reg)
{
    int saved = errno;
    size_t i, j;
    if (reg == NULL) return;
    for (i = 0; i < reg->n_groups; i++) {
        upstream_group_t *g = &reg->groups[i];
        for (j = 0; j < g->n_backends; j++) {
            backend_t *b = g->backends == NULL ? NULL : g->backends[j];
            if (b != NULL) {
                idle_node_t *node = b->idle;
                while (node != NULL) {
                    idle_node_t *next = node->next;
                    upstream_release(node->conn, false);
                    free(node);
                    node = next;
                }
                if (b->lock_initialized) (void)pthread_mutex_destroy(&b->lock);
                free(b);
            }
        }
        free(g->backends);
        if (g->lock_initialized) (void)pthread_mutex_destroy(&g->lock);
    }
    free(reg->groups);
    free(reg);
    errno = saved;
}

upstream_group_t *upstream_registry_get(upstream_registry_t *reg, const char *name)
{
    size_t i;
    if (reg == NULL || name == NULL) return NULL;
    for (i = 0; i < reg->n_groups; i++)
        if (strcmp(reg->groups[i].name, name) == 0) return &reg->groups[i];
    return NULL;
}

struct upstream_group *upstream_lookup_cb(void *ctx, const char *name)
{
    return upstream_registry_get((upstream_registry_t *)ctx, name);
}

size_t upstream_backend_count(const upstream_group_t *g)
{
    return g == NULL ? 0 : g->n_backends;
}

void upstream_report(backend_t *b, bool ok, int64_t now_ms)
{
    bool newly_down = false;
    if (b == NULL) return;
    pthread_mutex_lock(&b->lock);
    if (ok) {
        b->failures = 0;
        b->down_until_ms = 0;
    } else {
        b->failures++;
        if (b->failures >= (unsigned)b->fail_threshold) {
            newly_down = b->down_until_ms <= now_ms;
            b->down_until_ms = now_ms + b->cooldown_ms;
        }
    }
    pthread_mutex_unlock(&b->lock);
    if (newly_down) log_msg(LOG_WARN, "upstream backend %s temporarily disabled", b->hostport);
}

const char *backend_hostport(const backend_t *b)
{
    return b == NULL ? "" : b->hostport;
}
