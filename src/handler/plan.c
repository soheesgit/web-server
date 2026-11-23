/* src/handler/plan.c
 *
 * 응답 계획과 처리기 분기. 명세: plan.h, handler.h
 */
#include "handler/plan.h"
#include "handler/handler.h"
#include "core/stub.h"

void plan_init(response_plan_t *p)
{
    UNUSED(p);
    /* TODO(handler): 구현 — U-HD-02, U-HD-04 */
    STUB_VOID();
}

int plan_set_mem(response_plan_t *p, int status, const char *content_type, const void *body,
                 size_t n)
{
    UNUSED(p);
    UNUSED(status);
    UNUSED(content_type);
    UNUSED(body);
    UNUSED(n);
    /* TODO(handler): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}

int plan_error(response_plan_t *p, int status)
{
    UNUSED(p);
    UNUSED(status);
    /* TODO(handler): 구현 — U-HD-02, E-ST-04 */
    STUB_RETURN(-1);
}

int plan_add_header(response_plan_t *p, const char *name, const char *value)
{
    UNUSED(p);
    UNUSED(name);
    UNUSED(value);
    /* TODO(handler): 구현 — U-HD-11 */
    STUB_RETURN(-1);
}

void plan_cleanup(response_plan_t *p)
{
    UNUSED(p);
    /* TODO(handler): 구현 — U-HD-04, E-CG-07, E-CG-09 */
}

handler_result_t handler_dispatch(const http_request_t *req, const route_t *rt,
                                  const handler_ctx_t *ctx, proxy_job_t *out_job,
                                  response_plan_t *out_plan)
{
    UNUSED(req);
    UNUSED(rt);
    UNUSED(ctx);
    UNUSED(out_job);
    UNUSED(out_plan);
    /* TODO(handler): 구현 — E-ST-*, E-CG-*, E-PX-* */
    STUB_RETURN(HANDLER_DONE);
}
