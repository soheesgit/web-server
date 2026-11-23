/* src/router/router.c
 *
 * 라우팅. 명세: router.h
 */
#include "router/router.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 규칙 배열을 설정 순서대로 한 번 만든 뒤 요청마다 읽기만 한다.
 * 규칙의 문자열은 config_t를 가리키므로 라우터가 해제될 때까지 설정이 살아 있어야 한다.
 */
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
    /* 명시된 규칙이 없는 설정의 기본 라우트는 모든 경로를 cfg->root에서 찾는다. */
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
        /* 이름 해석은 호출자에게 맡겨 라우터가 업스트림 구현에 의존하지 않게 한다. */
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
    /* [2001:db8::1]:8080의 주소 안에 있는 콜론은 그대로 두고 포트만 뗀다. */
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

    /*
     * 문자 일치에 이어 경로 세그먼트 경계를 확인한다. /api는 /api와 /api/x에,
     * 끝에 슬래시가 붙은 /api/는 /api/x에 일치한다.
     */
    return n != 0 && n <= path_len && memcmp(path, rt->prefix, n) == 0 &&
           (n == path_len || rt->prefix[n - 1] == '/' || path[n] == '/');
}

const route_t *router_match(const router_t *r, str_t host, const char *path)
{
    const route_t *matched_host = NULL;
    const route_t *matched_any = NULL;
    size_t path_len = strlen(path);

    /*
     * Host가 일치하는 후보와 공용 후보의 최장 prefix를 따로 기억한다.
     * Host 전용 후보에 우선권이 있으며, 같은 길이는 먼저 본 설정을 유지한다.
     */
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
