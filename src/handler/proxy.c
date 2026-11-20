/* src/handler/proxy.c
 *
 * 프록시 판단. 명세: handler.h
 */
#include "handler/handler.h"
#include "core/stub.h"

handler_result_t proxy_begin(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                             proxy_job_t *out_job, response_plan_t *out_plan)
{
    UNUSED(req);
    UNUSED(rt);
    UNUSED(ctx);
    UNUSED(out_job);
    UNUSED(out_plan);
    /* TODO(handler): 구현 — U-PX-01, E-PX-01, E-PX-02, E-PX-09 */
    STUB_RETURN(HANDLER_DONE);
}

proxy_next_t proxy_on_fail(proxy_job_t *job, proxy_fail_t why, response_plan_t *out_plan)
{
    UNUSED(job);
    UNUSED(why);
    UNUSED(out_plan);
    /* TODO(handler): 구현 — U-PX-05, U-PX-07, E-PX-08~11, E-PX-13 */
    STUB_RETURN(PROXY_GIVE_UP);
}

int proxy_on_head(proxy_job_t *job, struct upstream_conn *c, const http_response_t *resp,
                  response_plan_t *out_plan)
{
    UNUSED(job);
    UNUSED(c);
    UNUSED(resp);
    UNUSED(out_plan);
    /* TODO(handler): 구현 — U-PX-10, E-PX-03, E-PX-05, E-PX-06, E-PX-16 */
    STUB_RETURN(-1);
}

void proxy_job_cleanup(proxy_job_t *job)
{
    UNUSED(job);
    /* TODO(handler): 구현 — U-PX-01 */
}
