/* src/downstream/request_parser.c
 *
 * 요청 머리 파서. 명세: request_parser.h
 */
#include "downstream/request_parser.h"
#include "core/stub.h"

#include <stddef.h>

http_parse_result_t request_parse_head(const char *p, size_t len, const http_limits_t *lim,
                                       http_request_t *out_req, int *out_status)
{
    UNUSED(p);
    UNUSED(len);
    UNUSED(lim);
    UNUSED(out_req);
    UNUSED(out_status);
    /* TODO(downstream): 구현 — U-DS-01, U-DS-06, U-DS-07, U-DS-12, E-ER-01~09, E-ER-12~14, E-ST-06,
     * E-ST-10 */
    if (out_status != NULL) {
        *out_status = 500;
    }
    STUB_RETURN(HTTP_PARSE_ERROR);
}
