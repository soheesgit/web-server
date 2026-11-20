/* src/common/http_framing.c
 *
 * 본문 길이·경계 추적·keep-alive. 명세: http_framing.h
 */
#include "common/http_framing.h"
#include "core/stub.h"

int http_resolve_framing(const http_headers_t *h, bool is_response, int status,
                         http_method_t req_method, http_framing_t *out_f, int *out_status)
{
    UNUSED(h);
    UNUSED(is_response);
    UNUSED(status);
    UNUSED(req_method);
    UNUSED(out_f);
    UNUSED(out_status);
    /* TODO(common): 구현 — U-COM-18, U-COM-21, U-COM-26, E-ER-08 */
    STUB_RETURN(-1);
}

void http_body_tracker_init(http_body_tracker_t *t, http_framing_t f)
{
    UNUSED(t);
    UNUSED(f);
    /* TODO(common): 구현 — U-COM-29, U-COM-32 */
    STUB_VOID();
}

http_body_result_t http_body_track(http_body_tracker_t *t, const char *p, size_t len,
                                   size_t *out_used)
{
    UNUSED(t);
    UNUSED(p);
    UNUSED(len);
    UNUSED(out_used);
    /* TODO(common): 구현 — U-COM-29, U-COM-32, E-PX-05 */
    STUB_RETURN(BODY_ERROR);
}

bool http_keep_alive(int minor, const http_headers_t *h)
{
    UNUSED(minor);
    UNUSED(h);
    /* TODO(common): 구현 — U-COM-38, E-KA-03, E-KA-04, E-KA-05 */
    STUB_RETURN(false);
}
