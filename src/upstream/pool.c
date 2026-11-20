/* src/upstream/pool.c
 *
 * 연결 획득·반납. 명세: upstream.h
 */
#include "upstream/upstream.h"
#include "core/stub.h"

int upstream_acquire(upstream_group_t *g, backend_t *b, upstream_conn_t **out_c)
{
    UNUSED(g);
    UNUSED(b);
    UNUSED(out_c);
    /* TODO(upstream): 구현 — U-US-24, E-PX-14 */
    STUB_RETURN(-1);
}

int upstream_connect_result(upstream_conn_t *c)
{
    UNUSED(c);
    /* TODO(upstream): 구현 — U-US-24 */
    STUB_RETURN(-1);
}

void upstream_release(upstream_conn_t *c, bool reusable)
{
    UNUSED(c);
    UNUSED(reusable);
    /* TODO(upstream): 구현 — U-US-24, E-PX-14, E-PX-17 */
    STUB_VOID();
}
