/* src/handler/static.c
 *
 * 정적 파일 처리기. 명세: handler.h
 */
#include "handler/handler.h"
#include "core/stub.h"

handler_result_t static_handle(const http_request_t *req, const route_t *rt,
                               const handler_ctx_t *ctx, response_plan_t *out_plan)
{
    UNUSED(req);
    UNUSED(rt);
    UNUSED(ctx);
    UNUSED(out_plan);
    /* TODO(handler): 구현 — U-HD-09, U-HD-11, U-HD-14, E-ST-01~12 */
    STUB_RETURN(HANDLER_DONE);
}
