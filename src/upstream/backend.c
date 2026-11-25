/* 레지스트리, 백엔드 상태와 유휴 연결 정리 스레드. */
#include "upstream/internal_state.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "core/platform.h"

#define UPSTREAM_REAP_MS 30000
#define UPSTREAM_RECOVERY_MAX 8u

struct upstream_registry {
    upstream_group_t *groups;
    size_t count;
    int wake[2];
    pthread_t reaper;
    bool started, metrics;
};

static int resolve_backend(backend_t *b, upstream_group_t *g, const backend_conf_t *cfg, char *err,
                           size_t errcap)
{
    if (inet_pton(AF_INET, cfg->host, &b->address.sin_addr) == 1) {
        b->address.sin_family = AF_INET;
        b->address.sin_port = htons(cfg->port);
        b->group = g;
        (void)snprintf(b->hostport, sizeof(b->hostport), "%s:%u", cfg->host, (unsigned)cfg->port);
        return 0;
    }
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *result;
    char port[6];
    (void)snprintf(port, sizeof(port), "%u", (unsigned)cfg->port);
    int status = getaddrinfo(cfg->host, port, &hints, &result);
    if (status != 0) {
        if (errcap > 0) {
            (void)snprintf(err, errcap, "upstream %s: cannot resolve %s: %s", g->name, cfg->host,
                           gai_strerror(status));
        }
        errno = EINVAL;
        return -1;
    }
    memcpy(&b->address, result->ai_addr, sizeof(b->address));
    freeaddrinfo(result);
    b->group = g;
    (void)snprintf(b->hostport, sizeof(b->hostport), "%s:%s", cfg->host, port);
    return 0;
}

static void expire_group(upstream_group_t *g, int64_t now)
{
    pool_entry_t *expired = NULL;
    pthread_mutex_lock(&g->lock);
    for (size_t i = 0; i < g->count; i++) {
        backend_t *b = &g->backends[i];
        pool_entry_t **link = &b->idle;
        while (*link != NULL) {
            pool_entry_t *p = *link;
            if (now - p->idle_since < UPSTREAM_IDLE_MS) {
                link = &p->next;
                continue;
            }
            *link = p->next;
            b->idle_count--;
            b->expired++;
            p->next = expired;
            expired = p;
        }
    }
    pthread_mutex_unlock(&g->lock);
    upstream_discard_idle(expired);
}

static void *reap(void *arg)
{
    upstream_registry_t *reg = arg;
    int64_t next = platform_now_ms() + UPSTREAM_REAP_MS;
    struct pollfd ready = {reg->wake[0], POLLIN, 0};
    for (;;) {
        int64_t now = platform_now_ms();
        int timeout = now < next ? (int)(next - now) : 0;
        int rc = poll(&ready, 1, timeout);
        if (rc > 0 || (rc < 0 && errno != EINTR)) {
            return NULL;
        }
        now = platform_now_ms();
        if (now >= next) {
            for (size_t i = 0; i < reg->count; i++) {
                expire_group(&reg->groups[i], now);
            }
            next = now + UPSTREAM_REAP_MS;
        }
    }
}

static int start_reaper(upstream_registry_t *reg)
{
    if (pipe(reg->wake) != 0) {
        return -1;
    }
    if (platform_set_cloexec(reg->wake[0]) != 0 || platform_set_cloexec(reg->wake[1]) != 0) {
        return -1;
    }
    int error = pthread_create(&reg->reaper, NULL, reap, reg);
    if (error != 0) {
        errno = error;
        return -1;
    }
    reg->started = true;
    return 0;
}

upstream_registry_t *upstream_registry_new(const config_t *cfg, char *err, size_t errcap)
{
    const char *policy = getenv("HTTPD_PROXY_LB");
    const char *recovery = getenv("HTTPD_PROXY_RECOVERY");
    up_policy_t choice = UP_RR;
    if (policy != NULL && strcmp(policy, "rr") != 0) {
        if (strcmp(policy, "least_requests") == 0) {
            choice = UP_LEAST;
        } else if (strcmp(policy, "p2c") == 0) {
            choice = UP_P2C;
        } else {
            if (errcap > 0) {
                (void)snprintf(err, errcap, "invalid HTTPD_PROXY_LB");
            }
            errno = EINVAL;
            return NULL;
        }
    }
    if (recovery != NULL && strcmp(recovery, "ramp") != 0 && strcmp(recovery, "single") != 0) {
        if (errcap > 0) {
            (void)snprintf(err, errcap, "invalid HTTPD_PROXY_RECOVERY");
        }
        errno = EINVAL;
        return NULL;
    }

    upstream_registry_t *reg = calloc(1, sizeof(*reg));
    if (reg == NULL) {
        goto system_error;
    }
    reg->wake[0] = -1;
    reg->wake[1] = -1;
    reg->metrics = getenv("HTTPD_PROXY_METRICS") != NULL;
    bool forward = false; /* FORWARD route가 있으면 목적지 그룹을 하나 더 둔다 (D-046) */
    for (size_t i = 0; i < cfg->n_routes; i++) {
        forward = forward || cfg->routes[i].kind == ROUTE_FORWARD;
    }
    if (cfg->n_upstreams == 0 && !forward) {
        return reg;
    }
    reg->groups = calloc(cfg->n_upstreams + (forward ? 1 : 0), sizeof(*reg->groups));
    if (reg->groups == NULL) {
        goto system_error;
    }
    for (size_t i = 0; i < cfg->n_upstreams + (forward ? 1 : 0); i++) {
        upstream_group_t *g = &reg->groups[i];
        /* 포워드 그룹은 백엔드 없이 시작하고 upstream_forward_backend가 채운다 */
        static const upstream_conf_t forward_conf = {.name = CFG_FORWARD_UPSTREAM};
        const upstream_conf_t *conf = i < cfg->n_upstreams ? &cfg->upstreams[i] : &forward_conf;
        int error = pthread_mutex_init(&g->lock, NULL);
        if (error != 0) {
            errno = error;
            goto system_error;
        }
        reg->count++;
        memcpy(g->name, conf->name, sizeof(g->name));
        g->count = conf->n_backends;
        g->max_idle = (size_t)cfg->pool_max_idle;
        g->fail_threshold = (unsigned)cfg->backend_fail_threshold;
        g->cooldown_ms = cfg->backend_cooldown_ms;
        g->policy = choice;
        g->rng = UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)platform_now_ms() ^ i;
        if (g->rng == 0) {
            g->rng = 1;
        }
        g->ramp = recovery == NULL || strcmp(recovery, "ramp") == 0;
        g->picker = conf->picker;
        g->bound_pct = conf->bound_pct;
        for (size_t j = 0; j < g->count; j++) {
            if (resolve_backend(&g->backends[j], g, &conf->backends[j], err, errcap) != 0) {
                upstream_registry_free(reg);
                return NULL;
            }
        }
    }
    if (start_reaper(reg) != 0) {
        goto system_error;
    }
    return reg;

system_error:
    if (errcap > 0) {
        (void)snprintf(err, errcap, "upstream registry: initialization failed (errno %d)", errno);
    }
    upstream_registry_free(reg);
    return NULL;
}

void upstream_registry_free(upstream_registry_t *reg)
{
    if (reg == NULL) {
        return;
    }
    int saved = errno;
    if (reg->wake[1] >= 0) {
        close(reg->wake[1]); /* POLLHUP로 정리 스레드를 깨운다. */
    }
    if (reg->started) {
        pthread_join(reg->reaper, NULL);
    }
    if (reg->wake[0] >= 0) {
        close(reg->wake[0]);
    }
    for (size_t i = 0; i < reg->count; i++) {
        upstream_group_t *g = &reg->groups[i];
        for (size_t j = 0; j < g->count; j++) {
            backend_t *b = &g->backends[j];
            if (reg->metrics && reg->started) {
                log_msg(LOG_INFO,
                        "proxy_metrics group=%s backend=%s assigned=%" PRIu64
                        " peak_active=%zu active=%zu sockets=%" PRIu64 " reused=%" PRIu64
                        " expired=%" PRIu64 " cooldowns=%" PRIu64 " recoveries=%" PRIu64
                        " detect_to_healthy_ms=%" PRIu64,
                        g->name, b->hostport, b->assigned, b->peak_active, b->active, b->sockets,
                        b->reused, b->expired, b->cooldowns, b->recoveries,
                        b->detect_to_healthy_ms);
            }
            upstream_discard_idle(b->idle);
        }
        pthread_mutex_destroy(&g->lock);
    }
    free(reg->groups);
    free(reg);
    errno = saved;
}

upstream_group_t *upstream_registry_get(upstream_registry_t *reg, const char *name)
{
    if (reg != NULL && name != NULL) {
        for (size_t i = 0; i < reg->count; i++) {
            if (strcmp(reg->groups[i].name, name) == 0) {
                return &reg->groups[i];
            }
        }
    }
    return NULL;
}

upstream_group_t *upstream_lookup_cb(void *ctx, const char *name)
{
    return upstream_registry_get(ctx, name);
}

backend_t *upstream_forward_backend(upstream_group_t *g, str_t authority)
{
    backend_conf_t dest = {.port = 80};
    const char *colon = memchr(authority.ptr, ':', authority.len);
    size_t host_len = colon != NULL ? (size_t)(colon - authority.ptr) : authority.len;
    uint64_t port = 80;
    backend_t fresh = {0};
    backend_t *b = NULL;
    char err[128];

    if (host_len == 0 || host_len >= sizeof(dest.host) || authority.ptr[0] == '[' ||
        (colon != NULL &&
         (str_to_u64((str_t){colon + 1, authority.len - host_len - 1}, 65535, &port) != 0 ||
          port == 0))) {
        errno = EINVAL;
        return NULL;
    }
    memcpy(dest.host, authority.ptr, host_len);
    dest.port = (uint16_t)port;
    /* 같은 목적지는 같은 백엔드를 돌려줘 유휴 연결 풀을 함께 쓴다 */
    char key[sizeof(fresh.hostport)];
    (void)snprintf(key, sizeof(key), "%s:%u", dest.host, (unsigned)dest.port);
    pthread_mutex_lock(&g->lock);
    for (size_t i = 0; i < g->count && b == NULL; i++) {
        if (strcmp(g->backends[i].hostport, key) == 0) {
            b = &g->backends[i];
        }
    }
    pthread_mutex_unlock(&g->lock);
    if (b != NULL) {
        return b;
    }
    /* 이름 해석은 lock 밖에서 한다. 그 사이 같은 목적지가 추가됐으면 그것을 쓴다 */
    if (resolve_backend(&fresh, g, &dest, err, sizeof(err)) != 0) {
        errno = EINVAL;
        return NULL;
    }
    (void)snprintf(fresh.hostport, sizeof(fresh.hostport), "%s", key);
    pthread_mutex_lock(&g->lock);
    for (size_t i = 0; i < g->count && b == NULL; i++) {
        if (strcmp(g->backends[i].hostport, key) == 0) {
            b = &g->backends[i];
        }
    }
    if (b == NULL && g->count < CFG_MAX_BACKENDS) {
        b = &g->backends[g->count];
        *b = fresh;
        g->count++;
    }
    pthread_mutex_unlock(&g->lock);
    if (b == NULL) {
        errno = E2BIG;
    }
    return b;
}

size_t upstream_backend_count(const upstream_group_t *g)
{
    return g->count;
}

const char *backend_hostport(const backend_t *b)
{
    return b->hostport;
}

void upstream_report(backend_t *b, bool ok, int64_t now_ms)
{
    if (b == NULL) {
        return;
    }
    upstream_group_t *g = b->group;
    pool_entry_t *discard = NULL;
    bool cooled = false;
    int saved = errno;

    pthread_mutex_lock(&g->lock);
    if (b->health != UP_COOLDOWN) {
        if (ok) {
            b->failures = 0;
            if (b->health == UP_RECOVERING && ++b->recovery_successes >= b->recovery_limit) {
                if (!g->ramp || b->recovery_limit == UPSTREAM_RECOVERY_MAX) {
                    b->health = UP_HEALTHY;
                    b->recoveries++;
                    b->detect_to_healthy_ms = (uint64_t)(now_ms - b->detected_at);
                } else {
                    b->recovery_limit *= 2;
                    b->recovery_successes = 0;
                }
            }
        } else if (b->health == UP_RECOVERING || ++b->failures >= g->fail_threshold) {
            if (b->health == UP_HEALTHY) {
                b->detected_at = now_ms;
            }
            b->health = UP_COOLDOWN;
            b->until = now_ms + g->cooldown_ms;
            b->cooldowns++;
            discard = b->idle;
            b->idle = NULL;
            b->idle_count = 0;
            cooled = true;
        }
    }
    pthread_mutex_unlock(&g->lock);
    upstream_discard_idle(discard);
    if (cooled) {
        log_msg(LOG_WARN, "upstream %s: cooldown", b->hostport);
    }
    errno = saved;
}
