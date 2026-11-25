/* src/router/router.c
 *
 * 라우팅. 명세: router.h
 */
#include "router/router.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct router {
    route_t routes[CFG_MAX_ROUTES];
    size_t n;
};

static void set_err(char *err, size_t errcap, const char *prefix, const char *name)
{
    if (errcap > 0) {
        (void)snprintf(err, errcap, "route %s: unknown upstream '%s'", prefix, name);
    }
}

router_t *router_new(const config_t *cfg, router_upstream_lookup_fn lookup, void *lookup_ctx,
                     char *err, size_t errcap)
{
    router_t *r = calloc(1, sizeof(*r));

    if (r == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    if (cfg->n_routes == 0) {
        r->routes[0].kind = ROUTE_STATIC;
        r->routes[0].prefix = "/";
        r->routes[0].prefix_len = 1;
        r->routes[0].root = cfg->root;
        r->n = 1;
        return r;
    }
    for (size_t i = 0; i < cfg->n_routes; i++) {
        const route_conf_t *rc = &cfg->routes[i];
        route_t *rt = &r->routes[i];

        rt->kind = rc->kind;
        rt->prefix = rc->prefix;
        rt->prefix_len = strlen(rc->prefix);
        rt->host = rc->host[0] != '\0' ? rc->host : NULL;
        rt->strip_prefix = rc->strip_prefix;
        rt->keep_host = rc->keep_host;
        if (rc->kind == ROUTE_PROXY || rc->kind == ROUTE_FORWARD) {
            rt->upstream = lookup != NULL ? lookup(lookup_ctx, rc->upstream) : NULL;
            if (rt->upstream == NULL) {
                set_err(err, errcap, rc->prefix, rc->upstream);
                free(r);
                errno = EINVAL;
                return NULL;
            }
        } else {
            rt->root = rc->root;
        }
    }
    r->n = cfg->n_routes;
    return r;
}

void router_free(router_t *r)
{
    int e = errno;

    free(r);
    errno = e;
}

/* Host에서 포트를 뗀다. IPv6 리터럴이면 ']' 뒤의 ':'만 포트 구분자다. */
static str_t strip_port(str_t host)
{
    size_t from = 0;

    if (host.len > 0 && host.ptr[0] == '[') {
        const char *rb = memchr(host.ptr, ']', host.len);
        if (rb == NULL) {
            return host;
        }
        from = (size_t)(rb - host.ptr) + 1;
    }
    for (size_t i = from; i < host.len; i++) {
        if (host.ptr[i] == ':') {
            host.len = i;
            break;
        }
    }
    return host;
}

static bool prefix_matches(const route_t *rt, const char *path)
{
    if (strncmp(path, rt->prefix, rt->prefix_len) != 0) {
        return false;
    }
    return rt->prefix[rt->prefix_len - 1] == '/' || path[rt->prefix_len] == '\0' ||
           path[rt->prefix_len] == '/';
}

const route_t *router_match(const router_t *r, str_t host, const char *path)
{
    const route_t *by_host = NULL;
    const route_t *any_host = NULL;

    host = strip_port(host);
    for (size_t i = 0; i < r->n; i++) {
        const route_t *rt = &r->routes[i];
        const route_t **best;

        if (!prefix_matches(rt, path)) {
            continue;
        }
        if (rt->host == NULL) {
            best = &any_host;
        } else if (host.len > 0 && str_eq_ci(host, rt->host)) {
            best = &by_host;
        } else {
            continue;
        }
        /* 길이가 같으면 먼저 나온 규칙을 유지한다 */
        if (*best == NULL || rt->prefix_len > (*best)->prefix_len) {
            *best = rt;
        }
    }
    return by_host != NULL ? by_host : any_host;
}
