/* 응답 머리 파싱과 업스트림에서 중계할 수 있는 프레이밍 검사. */
#include "upstream/response_parser.h"

#include <string.h>

#include "common/http_framing.h"
#include "common/http_headers.h"

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

static int status_line(str_t line, http_response_t *out)
{
    const char *p = line.ptr;
    if (line.len < 12 || memcmp(p, "HTTP/1.", 7) != 0 || !digit(p[7]) || p[8] != ' ' ||
        p[9] < '1' || p[9] > '5' || !digit(p[10]) || !digit(p[11]) ||
        (line.len > 12 && p[12] != ' ')) {
        return -1;
    }
    out->minor = p[7] - '0';
    out->status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
    out->reason = line.len > 12 ? (str_t){p + 13, line.len - 13} : (str_t){p + 12, 0};
    for (size_t i = 0; i < out->reason.len; i++) {
        unsigned char c = (unsigned char)out->reason.ptr[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            return -1;
        }
    }
    return 0;
}

http_parse_result_t response_parse_head(const char *p, size_t len, http_method_t req_method,
                                        const http_limits_t *lim, http_response_t *out_resp,
                                        int *out_status)
{
    size_t skip = 0, head_len = 0;
    int ignored = 0;
    http_parse_result_t result =
        http_find_head_end(p, len, lim->max_head, &skip, &head_len, &ignored);
    if (result == HTTP_PARSE_INCOMPLETE) {
        return result;
    }
    if (result == HTTP_PARSE_ERROR) {
        goto bad;
    }
    const char *line = p + skip;
    const char *lf = memchr(line, '\n', head_len - skip);
    size_t n = (size_t)(lf - line);
    size_t offset = (size_t)(lf - p) + 1;
    size_t blank = p[head_len - 2] == '\r' ? 2 : 1;
    if (n > 0 && line[n - 1] == '\r') {
        n--;
    }
    if (status_line((str_t){line, n}, out_resp) != 0 ||
        http_parse_headers(p + offset, head_len - blank - offset, lim->max_headers,
                           &out_resp->headers, &ignored) != 0) {
        goto bad;
    }
    const http_header_t *te = http_header_get(&out_resp->headers, "Transfer-Encoding");
    if (te != NULL && (out_resp->minor == 0 || !str_eq_ci(te->value, "chunked") ||
                       http_header_count(&out_resp->headers, "Transfer-Encoding") != 1)) {
        goto bad;
    }
    /* 본문 없는 응답도 길이 문법과 TE/CL 충돌을 확인한 뒤 경계를 정한다. */
    if (http_resolve_framing(&out_resp->headers, true, 200, HTTP_GET, &out_resp->framing,
                             &ignored) != 0) {
        goto bad;
    }
    if (req_method == HTTP_HEAD || out_resp->status < 200 || out_resp->status == 204 ||
        out_resp->status == 304) {
        out_resp->framing = (http_framing_t){FRAMING_NONE, 0};
    }
    out_resp->head_len = head_len;
    out_resp->keep_alive = out_resp->framing.kind != FRAMING_CLOSE &&
                           http_keep_alive(out_resp->minor, &out_resp->headers);
    return HTTP_PARSE_DONE;

bad:
    *out_status = 502;
    return HTTP_PARSE_ERROR;
}
