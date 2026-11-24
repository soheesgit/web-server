/* src/upstream/response_parser.c
 *
 * 업스트림 응답 머리 파서. 명세: response_parser.h
 */
#include "upstream/response_parser.h"

#include "common/http_framing.h"
#include "common/http_headers.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* 업스트림 응답 머리의 오류는 모두 502. */
static http_parse_result_t fail(int *out_status)
{
    *out_status = 502;
    return HTTP_PARSE_ERROR;
}

/* "HTTP/1.<digit> SP <3자리> [SP 사유]". line은 줄 끝(CR LF)을 뺀 바이트. */
static int parse_status_line(const char *line, size_t n, http_response_t *r)
{
    int status;

    if (n < 12 || memcmp(line, "HTTP/1.", 7) != 0 || !is_digit(line[7]) || line[8] != ' ' ||
        !is_digit(line[9]) || !is_digit(line[10]) || !is_digit(line[11])) {
        return -1;
    }
    if (n > 12 && line[12] != ' ') {
        return -1; /* "2000" 같은 4자리 이상 */
    }
    status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    if (status < 100 || status > 599) {
        return -1;
    }
    r->minor = line[7] - '0';
    r->status = status;
    r->reason.ptr = n > 13 ? line + 13 : NULL;
    r->reason.len = n > 13 ? n - 13 : 0;
    return 0;
}

http_parse_result_t response_parse_head(const char *p, size_t len, http_method_t req_method,
                                        const http_limits_t *lim, http_response_t *out_resp,
                                        int *out_status)
{
    http_response_t r;
    size_t skip = 0;
    size_t head_len = 0;
    size_t line_len;
    size_t hdr_off;
    size_t blank_len;
    const char *line;
    const char *lf;
    int st = 0;
    http_parse_result_t res = http_find_head_end(p, len, lim->max_head, &skip, &head_len, &st);

    if (res == HTTP_PARSE_INCOMPLETE) {
        return res;
    }
    if (res == HTTP_PARSE_ERROR) {
        return fail(out_status);
    }
    memset(&r, 0, sizeof(r));

    /* 상태 줄: p + skip부터 첫 LF까지. 줄 끝의 CR 하나는 떼고 본다 */
    line = p + skip;
    lf = memchr(line, '\n', head_len - skip);
    if (lf == NULL) {
        return fail(out_status);
    }
    line_len = (size_t)(lf - line);
    hdr_off = skip + line_len + 1;
    if (line_len > 0 && line[line_len - 1] == '\r') {
        line_len--;
    }
    if (parse_status_line(line, line_len, &r) != 0) {
        return fail(out_status);
    }

    /* 헤더: 상태 줄 다음부터 빈 줄 직전까지 */
    blank_len = (head_len >= 2 && p[head_len - 2] == '\r') ? 2 : 1;
    if (head_len - blank_len > hdr_off &&
        http_parse_headers(p + hdr_off, head_len - blank_len - hdr_off, lim->max_headers,
                           &r.headers, &st) != 0) {
        return fail(out_status);
    }
    if (http_resolve_framing(&r.headers, true, r.status, req_method, &r.framing, &st) != 0) {
        return fail(out_status);
    }
    r.keep_alive = http_keep_alive(r.minor, &r.headers) && r.framing.kind != FRAMING_CLOSE;
    r.head_len = head_len;
    *out_resp = r;
    return HTTP_PARSE_DONE;
}
