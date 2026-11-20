/* src/upstream/backend.c
 *
 * 레지스트리·그룹·백엔드 상태. 명세: upstream.h
 */
#include "upstream/upstream.h"
#include "core/stub.h"

#include <stddef.h>

upstream_registry_t *upstream_registry_new(const config_t *cfg, char *err, size_t errcap)
{
    UNUSED(cfg);
    UNUSED(err);
    UNUSED(errcap);
    /* TODO(upstream): 구현 — U-PX-01, U-US-17, U-US-24, E-PX-* */
    STUB_RETURN(NULL);
}

void upstream_registry_free(upstream_registry_t *reg)
{
    UNUSED(reg);
    /* TODO(upstream): 구현 — U-US-17 */
}

upstream_group_t *upstream_registry_get(upstream_registry_t *reg, const char *name)
{
    UNUSED(reg);
    UNUSED(name);
    /* TODO(upstream): 구현 — U-PX-01, U-US-17 */
    STUB_RETURN(NULL);
}

struct upstream_group *upstream_lookup_cb(void *ctx, const char *name)
{
    UNUSED(ctx);
    UNUSED(name);
    /* TODO(upstream): 구현 — E-PX-* */
    STUB_RETURN(NULL);
}

size_t upstream_backend_count(const upstream_group_t *g)
{
    UNUSED(g);
    /* TODO(upstream): 구현 — U-PX-05 */
    STUB_RETURN(0);
}

void upstream_report(backend_t *b, bool ok, int64_t now_ms)
{
    UNUSED(b);
    UNUSED(ok);
    UNUSED(now_ms);
    /* TODO(upstream): 구현 — E-PX-08, E-PX-09 */
    STUB_VOID();
}

const char *backend_hostport(const backend_t *b)
{
    UNUSED(b);
    /* TODO(upstream): 구현 — U-US-17, U-US-01 */
    STUB_RETURN("");
}
