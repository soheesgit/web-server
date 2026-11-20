/* src/upstream/response_parser.c
 *
 * 업스트림 응답 머리 파서. 명세: response_parser.h
 */
#include "upstream/response_parser.h"
#include "core/stub.h"

#include <stddef.h>

http_parse_result_t response_parse_head(const char *p, size_t len, http_method_t req_method,
                                        const http_limits_t *lim, http_response_t *out_resp,
                                        int *out_status)
{
    UNUSED(p);
    UNUSED(len);
    UNUSED(req_method);
    UNUSED(lim);
    UNUSED(out_resp);
    UNUSED(out_status);
    /* TODO(upstream): 구현 — U-US-09, U-US-12, E-PX-11 */
    if (out_status != NULL) {
        *out_status = 500;
    }
    STUB_RETURN(HTTP_PARSE_ERROR);
}
