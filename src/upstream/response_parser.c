/* src/upstream/response_parser.c
 *
 * 업스트림 응답 머리 파서. 명세: response_parser.h
 */
#include "upstream/response_parser.h"

#include "common/http_framing.h"
#include "common/http_headers.h"

#include <errno.h>
#include <string.h>

static size_t line_end(const char *p, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (p[i] == '\n')
            return i;
    return len;
}

http_parse_result_t response_parse_head(const char *p, size_t len, http_method_t req_method,
                                        const http_limits_t *lim, http_response_t *out_resp,
                                        int *out_status)
{
    size_t skip, head_len, end, header_start, header_len;
    int status = 502;
    http_parse_result_t pr;
    http_response_t resp;
    uint64_t code;
    str_t status_line;
    if (lim == NULL || out_resp == NULL || out_status == NULL || (p == NULL && len != 0)) {
        if (out_status != NULL)
            *out_status = 502;
        errno = EINVAL;
        return HTTP_PARSE_ERROR;
    }
    pr = http_find_head_end(p, len, lim->max_head, &skip, &head_len, &status);
    if (pr == HTTP_PARSE_INCOMPLETE)
        return pr;
    if (pr != HTTP_PARSE_DONE) {
        *out_status = 502;
        return HTTP_PARSE_ERROR;
    }
    if (skip > len || head_len > len - skip) {
        *out_status = 502;
        return HTTP_PARSE_ERROR;
    }
    status_line = (str_t){p + skip, head_len - skip};
    end = line_end(status_line.ptr, status_line.len);
    if (end == status_line.len) {
        *out_status = 502;
        return HTTP_PARSE_ERROR;
    }
    if (end > 0 && status_line.ptr[end - 1] == '\r')
        end--;
    if (end < 12 || memcmp(status_line.ptr, "HTTP/1.", 7) != 0 || status_line.ptr[7] < '0' ||
        status_line.ptr[7] > '9' || status_line.ptr[8] != ' ' || status_line.ptr[9] < '0' ||
        status_line.ptr[9] > '9' || status_line.ptr[10] < '0' || status_line.ptr[10] > '9' ||
        status_line.ptr[11] < '0' || status_line.ptr[11] > '9' ||
        (end > 12 && status_line.ptr[12] != ' ')) {
        *out_status = 502;
        return HTTP_PARSE_ERROR;
    }
    code = (uint64_t)(status_line.ptr[9] - '0') * 100 + (uint64_t)(status_line.ptr[10] - '0') * 10 +
           (uint64_t)(status_line.ptr[11] - '0');
    if (code < 100 || code > 599) {
        *out_status = 502;
        return HTTP_PARSE_ERROR;
    }
    memset(&resp, 0, sizeof(resp));
    resp.status = (int)code;
    resp.minor = status_line.ptr[7] - '0';
    if (end > 13)
        resp.reason = (str_t){status_line.ptr + 13, end - 13};
    header_start = skip + line_end(p + skip, head_len - skip) + 1;
    if (header_start > head_len) {
        *out_status = 502;
        return HTTP_PARSE_ERROR;
    }
    header_len = head_len - header_start;
    if (header_len >= 2 && p[head_len - 2] == '\r' && p[head_len - 1] == '\n')
        header_len -= 2;
    else if (header_len >= 1 && p[head_len - 1] == '\n')
        header_len -= 1;
    if (http_parse_headers(p + header_start, header_len, lim->max_headers, &resp.headers,
                           &status) != 0) {
        *out_status = 502;
        return HTTP_PARSE_ERROR;
    }
    if (http_resolve_framing(&resp.headers, true, resp.status, req_method, &resp.framing,
                             &status) != 0) {
        *out_status = 502;
        return HTTP_PARSE_ERROR;
    }
    resp.keep_alive =
        http_keep_alive(resp.minor, &resp.headers) && resp.framing.kind != FRAMING_CLOSE;
    resp.head_len = head_len;
    *out_resp = resp;
    return HTTP_PARSE_DONE;
}
