/* src/upstream/response_parser.c
 *
 * 업스트림 응답 머리 파서. 명세: response_parser.h
 */
#include "upstream/response_parser.h"

#include <string.h>

#include "common/http_framing.h"
#include "common/http_headers.h"

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool parse_status_line(const char *p, size_t len, http_response_t *out)
{
    /* 필수 부분은 고정 위치까지 확인하고, 선택 사유 문구는 길이와 함께 빌린다. */
    if (len < 12 || memcmp(p, "HTTP/1.", 7) != 0 || !is_digit(p[7]) || p[8] != ' ' || p[9] < '1' ||
        p[9] > '5' || !is_digit(p[10]) || !is_digit(p[11]) || (len > 12 && p[12] != ' ')) {
        return false;
    }
    out->minor = p[7] - '0';
    out->status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
    if (len > 12) {
        out->reason = (str_t){p + 13, len - 13};
        for (size_t i = 13; i < len; i++) {
            unsigned char c = (unsigned char)p[i];
            if ((c < 0x20 && c != '\t') || c == 0x7F) {
                return false;
            }
        }
    }
    return true;
}

http_parse_result_t response_parse_head(const char *p, size_t len, http_method_t req_method,
                                        const http_limits_t *lim, http_response_t *out_resp,
                                        int *out_status)
{
    size_t skip, head_len;
    int status;
    http_response_t resp = {0};
    http_parse_result_t result =
        http_find_head_end(p, len, lim->max_head, &skip, &head_len, &status);

    /* 수신 중이면 더 기다린다. 완성된 응답의 형식 오류와 한도 초과는 502로 처리한다. */
    if (result == HTTP_PARSE_INCOMPLETE) {
        return result;
    }
    if (result == HTTP_PARSE_DONE) {
        const char *line = p + skip;
        const char *lf = memchr(line, '\n', head_len - skip);
        size_t line_len = (size_t)(lf - line);
        size_t headers_off = skip + line_len + 1;
        size_t blank_len = p[head_len - 2] == '\r' ? 2 : 1;

        if (line_len > 0 && line[line_len - 1] == '\r') {
            line_len--;
        }
        /* 헤더 원본을 파싱해 본문 방식을 정한다. 헤더 필터링은 전달하는 쪽에서 한다. */
        if (parse_status_line(line, line_len, &resp) &&
            http_parse_headers(p + headers_off, head_len - blank_len - headers_off,
                               lim->max_headers, &resp.headers, &status) == 0 &&
            http_resolve_framing(&resp.headers, true, resp.status, req_method, &resp.framing,
                                 &status) == 0) {
            /* 연결을 닫아 본문의 끝을 알리는 응답은 업스트림 연결을 재사용할 수 없다. */
            resp.keep_alive =
                resp.framing.kind != FRAMING_CLOSE && http_keep_alive(resp.minor, &resp.headers);
            resp.head_len = head_len;
            *out_resp = resp;
            return HTTP_PARSE_DONE;
        }
    }
    *out_status = 502;
    return HTTP_PARSE_ERROR;
}
