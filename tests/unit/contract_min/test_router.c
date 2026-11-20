/* tests/unit/contract_min/test_router.c
 *
 * router/ 계약 최소셋. 기대값의 원래 표: 기획서 10.1절. ID 목록: src/router/README.md
 */
#include "unit/testlib.h"

#include <stdio.h>
#include <string.h>

#include "core/config.h"
#include "fakes/fake_handler.h"
#include "router/router.h"

static long g_group_a, g_group_b; /* 가짜 upstream 그룹 (주소만 쓴다) */

static struct upstream_group *lookup(void *ctx, const char *name)
{
    (void)ctx;
    if (strcmp(name, "a") == 0) {
        return (struct upstream_group *)(void *)&g_group_a;
    }
    if (strcmp(name, "b") == 0) {
        return (struct upstream_group *)(void *)&g_group_b;
    }
    return NULL;
}

static void add_upstream(config_t *cfg, const char *name, uint16_t port)
{
    upstream_conf_t *u = &cfg->upstreams[cfg->n_upstreams++];
    (void)snprintf(u->name, sizeof(u->name), "%s", name);
    (void)snprintf(u->backends[0].host, sizeof(u->backends[0].host), "%s", "127.0.0.1");
    u->backends[0].port = port;
    u->n_backends = 1;
}

static void add_route(config_t *cfg, route_kind_t kind, const char *prefix, const char *upstream)
{
    route_conf_t *r = &cfg->routes[cfg->n_routes++];
    r->kind = kind;
    (void)snprintf(r->prefix, sizeof(r->prefix), "%s", prefix);
    if (kind == ROUTE_PROXY) {
        (void)snprintf(r->upstream, sizeof(r->upstream), "%s", upstream);
    } else {
        (void)snprintf(r->root, sizeof(r->root), "%s", cfg->root);
    }
}

static void t_u_rt_02(void)
{
    config_t *cfg = fake_config();
    router_t *r;
    const route_t *m;
    char err[256] = "";
    str_t host = {"x", 1};

    CHECK(cfg != NULL);
    add_upstream(cfg, "a", 18001);
    add_upstream(cfg, "b", 18002);
    add_route(cfg, ROUTE_PROXY, "/api/", "a");
    add_route(cfg, ROUTE_PROXY, "/api/v2/", "b");
    r = router_new(cfg, lookup, NULL, err, sizeof(err));
    CHECK(r != NULL);

    m = router_match(r, host, "/api/v2/x");
    CHECK(m != NULL);
    CHECK_EQ_INT(m->kind, ROUTE_PROXY);
    CHECK(m->upstream == (struct upstream_group *)(void *)&g_group_b);
    m = router_match(r, host, "/api/x");
    CHECK(m != NULL);
    CHECK(m->upstream == (struct upstream_group *)(void *)&g_group_a);
    router_free(r);
}

static void t_u_rt_03(void)
{
    config_t *cfg = fake_config();
    router_t *r;
    char err[256] = "";
    str_t host = {"x", 1};

    CHECK(cfg != NULL);
    add_route(cfg, ROUTE_STATIC, "/api", NULL);
    r = router_new(cfg, NULL, NULL, err, sizeof(err));
    CHECK(r != NULL);
    CHECK(router_match(r, host, "/api") != NULL);
    CHECK(router_match(r, host, "/api/x") != NULL);
    CHECK(router_match(r, host, "/apix") == NULL);
    router_free(r);
}

void register_router_tests(void)
{
    test_register("U-RT-02", "router_match: longest prefix wins", t_u_rt_02);
    test_register("U-RT-03", "router_match: prefix boundary /api vs /apix", t_u_rt_03);
}
