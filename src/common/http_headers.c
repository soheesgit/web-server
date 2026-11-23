/* src/common/http_headers.c
 *
 * 헤더 끝 탐색·파싱·쓰기. 명세: http_headers.h
 */
#include "common/http_headers.h"
#include "core/stub.h"

#include <stddef.h>

http_parse_result_t http_find_head_end(const char *p, size_t len, size_t max_head, size_t *out_skip,
                                       size_t *out_head_len, int *out_status)
{
    UNUSED(p);
    UNUSED(len);
    UNUSED(max_head);
    UNUSED(out_skip);
    UNUSED(out_head_len);
    UNUSED(out_status);
    /* TODO(common): 구현 — U-COM-01, U-COM-02, U-COM-04, E-ER-06, E-ER-14 */
    if (out_status != NULL) {
        *out_status = 500;
    }
    STUB_RETURN(HTTP_PARSE_ERROR);
}

int http_parse_headers(const char *p, size_t len, size_t max_headers, http_headers_t *out_h,
                       int *out_status)
{
    UNUSED(p);
    UNUSED(len);
    UNUSED(max_headers);
    UNUSED(out_h);
    UNUSED(out_status);
    /* TODO(common): 구현 — U-COM-07, U-COM-10, E-ER-13 */
    STUB_RETURN(-1);
}

const http_header_t *http_header_get(const http_headers_t *h, const char *name)
{
    UNUSED(h);
    UNUSED(name);
    /* TODO(common): 구현 — (테스트 없음) */
    STUB_RETURN(NULL);
}

size_t http_header_count(const http_headers_t *h, const char *name)
{
    UNUSED(h);
    UNUSED(name);
    /* TODO(common): 구현 — U-DS-12 */
    STUB_RETURN(0);
}

bool http_token_valid(str_t s)
{
    UNUSED(s);
    /* TODO(common): 구현 — U-DS-07 */
    STUB_RETURN(false);
}

bool http_is_hop_by_hop(str_t name, const http_headers_t *h)
{
    UNUSED(name);
    UNUSED(h);
    /* TODO(common): 구현 — U-COM-40, E-PX-03 */
    STUB_RETURN(false);
}

int http_headers_write(const http_headers_t *h, bool skip_hop_by_hop, buf_t *out)
{
    UNUSED(h);
    UNUSED(skip_hop_by_hop);
    UNUSED(out);
    /* TODO(common): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}
