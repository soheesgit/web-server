/* src/upstream/lb.c
 *
 * 백엔드 선택. 명세: upstream.h
 */
#include "upstream/upstream.h"
#include "core/stub.h"

#include <stddef.h>

backend_t *upstream_pick(upstream_group_t *g, const backend_t *exclude, int64_t now_ms)
{
    UNUSED(g);
    UNUSED(exclude);
    UNUSED(now_ms);
    /* TODO(upstream): 구현 — U-US-17, E-PX-01, E-PX-08, E-PX-09 */
    STUB_RETURN(NULL);
}
