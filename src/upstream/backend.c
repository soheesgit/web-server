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

/* 백엔드 하나를 만든다 (이름 해석 포함). 실패하면 NULL, 해석 실패면 *out_gai에 getaddrinfo 값. */
static backend_t *backend_create(const char *host, uint16_t port, size_t idle_limit,
                                 int fail_threshold, int cooldown_ms, int *out_gai)
{
    struct addrinfo hints, *res = NULL;
    char service[8];
    backend_t *b;
    int gai;
    (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    gai = getaddrinfo(host, service, &hints, &res);
    *out_gai = gai;
    if (gai != 0 || res == NULL) {
        if (res != NULL)
            freeaddrinfo(res);
        errno = EINVAL;
        return NULL;
    }
    b = calloc(1, sizeof(*b));
    if (b == NULL) {
        freeaddrinfo(res);
        return NULL;
    }
    memcpy(&b->addr, res->ai_addr, sizeof(b->addr));
    freeaddrinfo(res);
    if (pthread_mutex_init(&b->lock, NULL) != 0) {
        free(b);
        errno = ENOMEM;
        return NULL;
    }
    b->lock_initialized = true;
    b->idle_limit = idle_limit;
    b->fail_threshold = fail_threshold;
    b->cooldown_ms = cooldown_ms;
    (void)snprintf(b->hostport, sizeof(b->hostport), "%s:%u", host, (unsigned)port);
    return b;
}

upstream_registry_t *upstream_registry_new(const config_t *cfg, char *err, size_t errcap)
{
    upstream_registry_t *reg;
    size_t i, j;
    if (cfg == NULL) {
        errno = EINVAL;
        return NULL;
    }
    reg = calloc(1, sizeof(*reg));
    if (reg == NULL)
        return NULL;
    (void)snprintf(reg->forward.name, sizeof(reg->forward.name), "%s", CFG_FORWARD_UPSTREAM);
    reg->forward.fw_idle_limit = (size_t)cfg->pool_max_idle;
    reg->forward.fw_fail_threshold = cfg->backend_fail_threshold;
    reg->forward.fw_cooldown_ms = cfg->backend_cooldown_ms;
    if (pthread_mutex_init(&reg->forward.lock, NULL) != 0) {
        free(reg);
        errno = ENOMEM;
        return NULL;
    }
    reg->forward.lock_initialized = true;
    reg->n_groups = cfg->n_upstreams;
    reg->connect_timeout_ms = cfg->upstream_connect_timeout_ms;
    reg->response_timeout_ms = cfg->upstream_response_timeout_ms;
    if (reg->n_groups > 0) {
        reg->groups = calloc(reg->n_groups, sizeof(*reg->groups));
        if (reg->groups == NULL) {
            free(reg);
            return NULL;
        }
    }
    for (i = 0; i < reg->n_groups; i++) {
        const upstream_conf_t *src = &cfg->upstreams[i];
        upstream_group_t *g = &reg->groups[i];
        (void)snprintf(g->name, sizeof(g->name), "%s", src->name);
        g->n_backends = src->n_backends;
        if (pthread_mutex_init(&g->lock, NULL) != 0) {
            errno = ENOMEM;
            goto fail;
        }
        g->lock_initialized = true;
        g->backends = calloc(g->n_backends, sizeof(*g->backends));
        if (g->backends == NULL && g->n_backends != 0)
            goto fail;
        for (j = 0; j < g->n_backends; j++) {
            int gai = 0;
            g->backends[j] = backend_create(src->backends[j].host, src->backends[j].port,
                                            (size_t)cfg->pool_max_idle, cfg->backend_fail_threshold,
                                            cfg->backend_cooldown_ms, &gai);
            if (g->backends[j] == NULL) {
                if (gai != 0 && err != NULL && errcap > 0)
                    (void)snprintf(err, errcap, "upstream %s: cannot resolve %s: %s", src->name,
                                   src->backends[j].host, gai_strerror(gai));
                goto fail;
            }
        }
    }
    return reg;
fail:
    upstream_registry_free(reg);
    return NULL;
}

/* 그룹의 백엔드와 유휴 연결을 모두 닫고 해제한다. */
static void group_free(upstream_group_t *g)
{
    for (size_t j = 0; j < g->n_backends; j++) {
        backend_t *b = g->backends == NULL ? NULL : g->backends[j];
        if (b != NULL) {
            idle_node_t *node = b->idle;
            while (node != NULL) {
                idle_node_t *next = node->next;
                upstream_release(node->conn, false);
                free(node);
                node = next;
            }
            if (b->lock_initialized)
                (void)pthread_mutex_destroy(&b->lock);
            free(b);
        }
    }
    free(g->backends);
    if (g->lock_initialized)
        (void)pthread_mutex_destroy(&g->lock);
}

void upstream_registry_free(upstream_registry_t *reg)
{
    int saved = errno;
    size_t i;
    if (reg == NULL)
        return;
    for (i = 0; i < reg->n_groups; i++)
        group_free(&reg->groups[i]);
    group_free(&reg->forward);
    free(reg->groups);
    free(reg);
    errno = saved;
}

backend_t *upstream_forward_backend(upstream_group_t *g, str_t authority)
{
    enum { FORWARD_MAX = 256 };
    char host[CFG_HOST_MAX], hostport[CFG_HOST_MAX + 8];
    size_t hl = 0, i;
    unsigned long port = 80;
    backend_t *b = NULL;
    if (g == NULL || authority.ptr == NULL || authority.len == 0) {
        errno = EINVAL;
        return NULL;
    }
    /* host[:port]. IPv6 리터럴("[::1]")은 받지 않는다 (AF_INET만 쓴다). */
    while (hl < authority.len && authority.ptr[hl] != ':')
        hl++;
    if (hl == 0 || hl >= sizeof(host) || authority.ptr[0] == '[') {
        errno = EINVAL;
        return NULL;
    }
    if (hl < authority.len) {
        port = 0;
        for (i = hl + 1; i < authority.len; i++) {
            unsigned char c = (unsigned char)authority.ptr[i];
            if (c < '0' || c > '9' || port > 65535)
                break;
            port = port * 10 + (c - '0');
        }
        if (i != authority.len || i == hl + 1 || port == 0 || port > 65535) {
            errno = EINVAL;
            return NULL;
        }
    }
    memcpy(host, authority.ptr, hl);
    host[hl] = '\0';
    (void)snprintf(hostport, sizeof(hostport), "%s:%lu", host, port);
    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->n_backends && b == NULL; i++) {
        if (strcmp(g->backends[i]->hostport, hostport) == 0)
            b = g->backends[i];
    }
    if (b == NULL && g->n_backends >= FORWARD_MAX) {
        errno = E2BIG;
    } else if (b == NULL) {
        /* shortcut: 이름 해석이 블로킹이라 v2 루프가 그동안 멈춘다, 느린 DNS가 문제면 비동기
         * 해석으로 바꾼다 */
        int gai = 0;
        backend_t **grown = realloc(g->backends, (g->n_backends + 1) * sizeof(*grown));
        if (grown != NULL) {
            g->backends = grown;
            b = backend_create(host, (uint16_t)port, g->fw_idle_limit, g->fw_fail_threshold,
                               g->fw_cooldown_ms, &gai);
            if (b != NULL)
                g->backends[g->n_backends++] = b;
        }
    }
    pthread_mutex_unlock(&g->lock);
    return b;
}

upstream_group_t *upstream_registry_get(upstream_registry_t *reg, const char *name)
{
    size_t i;
    if (reg == NULL || name == NULL)
        return NULL;
    for (i = 0; i < reg->n_groups; i++)
        if (strcmp(reg->groups[i].name, name) == 0)
            return &reg->groups[i];
    return NULL;
}

struct upstream_group *upstream_lookup_cb(void *ctx, const char *name)
{
    upstream_registry_t *reg = ctx;
    if (reg != NULL && name != NULL && strcmp(name, CFG_FORWARD_UPSTREAM) == 0)
        return &reg->forward;
    return upstream_registry_get(reg, name);
}

size_t upstream_backend_count(const upstream_group_t *g)
{
    return g == NULL ? 0 : g->n_backends;
}

void upstream_report(backend_t *b, bool ok, int64_t now_ms)
{
    bool newly_down = false;
    if (b == NULL)
        return;
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
    if (newly_down)
        log_msg(LOG_WARN, "upstream backend %s temporarily disabled", b->hostport);
}

const char *backend_hostport(const backend_t *b)
{
    return b == NULL ? "" : b->hostport;
}
