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
    size_t count;
    route_t routes[];
};

router_t *router_new(const config_t *cfg, router_upstream_lookup_fn lookup, void *lookup_ctx,
                     char *err, size_t errcap)
{
    size_t count = cfg->n_routes == 0 ? 1 : cfg->n_routes;
    router_t *r = calloc(1, sizeof(*r) + count * sizeof(r->routes[0]));

    if (r == NULL) {
        if (errcap != 0) {
            (void)snprintf(err, errcap, "router: out of memory");
        }
        errno = ENOMEM;
        return NULL;
    }

    r->count = count;
    if (cfg->n_routes == 0) {
        r->routes[0] =
            (route_t){.kind = ROUTE_STATIC, .prefix = "/", .prefix_len = 1, .root = cfg->root};
        return r;
    }

    for (size_t i = 0; i < count; i++) {
        const route_conf_t *conf = &cfg->routes[i];
        route_t *rt = &r->routes[i];

        rt->kind = conf->kind;
        rt->prefix = conf->prefix;
        rt->prefix_len = strlen(conf->prefix);
        rt->host = conf->host[0] == '\0' ? NULL : conf->host;
        if (conf->kind == ROUTE_PROXY) {
            rt->strip_prefix = conf->strip_prefix;
            rt->keep_host = conf->keep_host;
            if (lookup != NULL) {
                rt->upstream = lookup(lookup_ctx, conf->upstream);
            }
            if (rt->upstream == NULL) {
                if (errcap != 0) {
                    (void)snprintf(err, errcap, "route %s: unknown upstream '%s'", conf->prefix,
                                   conf->upstream);
                }
                router_free(r);
                errno = EINVAL;
                return NULL;
            }
        } else {
            rt->root = conf->root;
        }
    }
    return r;
}

void router_free(router_t *r)
{
    int saved = errno;
    free(r);
    errno = saved;
}

static str_t host_without_port(str_t host)
{
    const char *port;

    if (host.len == 0) {
        return host;
    }
    if (host.ptr[0] == '[') {
        const char *end = memchr(host.ptr, ']', host.len);
        if (end != NULL && (size_t)(end - host.ptr + 1) < host.len && end[1] == ':') {
            host.len = (size_t)(end - host.ptr + 1);
        }
        return host;
    }
    port = memchr(host.ptr, ':', host.len);
    if (port != NULL) {
        host.len = (size_t)(port - host.ptr);
    }
    return host;
}

static bool match_prefix(const route_t *rt, const char *path, size_t path_len)
{
    size_t n = rt->prefix_len;

    return n != 0 && n <= path_len && memcmp(path, rt->prefix, n) == 0 &&
           (n == path_len || rt->prefix[n - 1] == '/' || path[n] == '/');
}

const route_t *router_match(const router_t *r, str_t host, const char *path)
{
    const route_t *matched_host = NULL;
    const route_t *matched_any = NULL;
    size_t path_len = strlen(path);

    host = host_without_port(host);
    for (size_t i = 0; i < r->count; i++) {
        const route_t *rt = &r->routes[i];

        if (!match_prefix(rt, path, path_len)) {
            continue;
        }
        if (rt->host == NULL) {
            if (matched_any == NULL || rt->prefix_len > matched_any->prefix_len) {
                matched_any = rt;
            }
        } else if ((matched_host == NULL || rt->prefix_len > matched_host->prefix_len) &&
                   host.len != 0 && str_eq_ci(host, rt->host)) {
            matched_host = rt;
        }
    }
    return matched_host != NULL ? matched_host : matched_any;
}
