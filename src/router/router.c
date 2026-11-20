/* src/router/router.c
 *
 * 라우팅. 명세: router.h
 */
#include "router/router.h"
#include "core/stub.h"

#include <stddef.h>

router_t *router_new(const config_t *cfg, router_upstream_lookup_fn lookup, void *lookup_ctx,
                     char *err, size_t errcap)
{
    UNUSED(cfg);
    UNUSED(lookup);
    UNUSED(lookup_ctx);
    UNUSED(err);
    UNUSED(errcap);
    /* TODO(router): 구현 — U-RT-02, U-RT-03, E-CF-02 */
    STUB_RETURN(NULL);
}

void router_free(router_t *r)
{
    UNUSED(r);
    /* TODO(router): 구현 — U-RT-02 */
}

const route_t *router_match(const router_t *r, str_t host, const char *path)
{
    UNUSED(r);
    UNUSED(host);
    UNUSED(path);
    /* TODO(router): 구현 — U-RT-02, U-RT-03, E-PX-15, E-ST-01 */
    STUB_RETURN(NULL);
}
