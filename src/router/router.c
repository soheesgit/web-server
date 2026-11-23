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
    size_t n_routes;
    route_t *routes;
};

router_t *router_new(const config_t *cfg, router_upstream_lookup_fn lookup, void *lookup_ctx,
                     char *err, size_t errcap)
{
    router_t *r;
    size_t i, n;
    if (cfg == NULL) {
        errno = EINVAL;
        return NULL;
    }
    n = cfg->n_routes == 0 ? 1 : cfg->n_routes;
    r = calloc(1, sizeof(*r));
    if (r == NULL) return NULL;
    r->routes = calloc(n, sizeof(*r->routes));
    if (r->routes == NULL) {
        free(r);
        return NULL;
    }
    r->n_routes = n;
    if (cfg->n_routes == 0) {
        r->routes[0] = (route_t){.kind = ROUTE_STATIC,
                                 .prefix = "/",
                                 .prefix_len = 1,
                                 .host = NULL,
                                 .root = cfg->root};
        return r;
    }
    for (i = 0; i < cfg->n_routes; i++) {
        const route_conf_t *src = &cfg->routes[i];
        route_t *dst = &r->routes[i];
        dst->kind = src->kind;
        dst->prefix = src->prefix;
        dst->prefix_len = strlen(src->prefix);
        dst->host = src->host[0] == '\0' ? NULL : src->host;
        dst->root = src->root[0] == '\0' ? NULL : src->root;
        dst->strip_prefix = src->strip_prefix;
        dst->keep_host = src->keep_host;
        if (src->kind == ROUTE_PROXY) {
            dst->upstream = lookup == NULL ? NULL : lookup(lookup_ctx, src->upstream);
            if (dst->upstream == NULL) {
                if (err != NULL && errcap > 0) {
                    (void)snprintf(err, errcap, "route %s: unknown upstream '%s'", src->prefix,
                                   src->upstream);
                }
                free(r->routes);
                free(r);
                errno = EINVAL;
                return NULL;
            }
        }
    }
    return r;
}

void router_free(router_t *r)
{
    int saved = errno;
    if (r != NULL) {
        free(r->routes);
        free(r);
    }
    errno = saved;
}

static str_t host_without_port(str_t host)
{
    size_t i;
    if (host.ptr == NULL) return host;
    if (host.len > 0 && host.ptr[0] == '[') {
        for (i = 1; i < host.len; i++) {
            if (host.ptr[i] == ']') {
                return (str_t){host.ptr, i + 1};
            }
        }
    }
    for (i = 0; i < host.len; i++) {
        if (host.ptr[i] == ':') return (str_t){host.ptr, i};
    }
    return host;
}

static int prefix_matches(const route_t *r, const char *path)
{
    size_t n;
    if (path == NULL) return 0;
    n = strlen(path);
    if (n < r->prefix_len || memcmp(path, r->prefix, r->prefix_len) != 0) return 0;
    if (r->prefix_len == 0 || r->prefix[r->prefix_len - 1] == '/') return 1;
    return n == r->prefix_len || path[r->prefix_len] == '/';
}

const route_t *router_match(const router_t *r, str_t host, const char *path)
{
    const route_t *best_host = NULL, *best_any = NULL;
    str_t clean_host = host_without_port(host);
    size_t i;
    if (r == NULL || path == NULL) return NULL;
    for (i = 0; i < r->n_routes; i++) {
        const route_t *candidate = &r->routes[i];
        if (!prefix_matches(candidate, path)) continue;
        if (candidate->host != NULL) {
            if (!str_eq_ci(clean_host, candidate->host)) continue;
            if (best_host == NULL || candidate->prefix_len > best_host->prefix_len)
                best_host = candidate;
        } else if (best_any == NULL || candidate->prefix_len > best_any->prefix_len) {
            best_any = candidate;
        }
    }
    return best_host != NULL ? best_host : best_any;
}
