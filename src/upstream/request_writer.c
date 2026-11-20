/* src/upstream/request_writer.c
 *
 * 업스트림 요청 직렬화. 명세: request_writer.h
 */
#include "upstream/request_writer.h"
#include "core/stub.h"

int request_write_upstream(const http_request_t *req, const upstream_rewrite_t *rw, buf_t *out)
{
    UNUSED(req);
    UNUSED(rw);
    UNUSED(out);
    /* TODO(upstream): 구현 — U-US-01, U-PX-01, E-PX-02, E-PX-03, E-PX-04 */
    STUB_RETURN(-1);
}
