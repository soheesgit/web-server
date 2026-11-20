/* src/handler/cgi.c
 *
 * CGI 처리기. 명세: handler.h
 */
#include "handler/handler.h"
#include "core/stub.h"

#include <stddef.h>

handler_result_t cgi_handle(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                            response_plan_t *out_plan)
{
    UNUSED(req);
    UNUSED(rt);
    UNUSED(ctx);
    UNUSED(out_plan);
    /* TODO(handler): 구현 — U-HD-21, E-CG-01~10 */
    STUB_RETURN(HANDLER_DONE);
}

http_parse_result_t cgi_parse_head(const char *p, size_t len, size_t max_head,
                                   http_head_out_t *out_head, size_t *out_used, int *out_status)
{
    UNUSED(p);
    UNUSED(len);
    UNUSED(max_head);
    UNUSED(out_head);
    UNUSED(out_used);
    UNUSED(out_status);
    /* TODO(handler): 구현 — U-HD-26, E-CG-04, E-CG-05 */
    if (out_status != NULL) {
        *out_status = 500;
    }
    STUB_RETURN(HTTP_PARSE_ERROR);
}
