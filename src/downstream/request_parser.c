/* src/downstream/request_parser.c
 *
 * 요청 머리 파서. 명세: request_parser.h
 */
#include "downstream/request_parser.h"

#include "common/http_framing.h"
#include "common/http_headers.h"
#include "downstream/uri.h"

#include <stdbool.h>
#include <string.h>

typedef enum { TARGET_ORIGIN, TARGET_ABSOLUTE } target_form_t;

static http_parse_result_t fail(int *out_status, int status)
{
    *out_status = status;
    return HTTP_PARSE_ERROR;
}

static bool is_digits(const char *p, size_t n)
{
    if (n == 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
    }
    return true;
}

/* 머리 한도를 넘기기 전에 있든, 같은 read에 있든 대상 토큰 길이를 확인한다. */
static bool request_line_too_long(const char *p, size_t len, size_t max_uri)
{
    size_t i = 0;
    while (i < len) {
        if (p[i] == '\n') {
            i++;
        } else if (p[i] == '\r' && i + 1 < len && p[i + 1] == '\n') {
            i += 2;
        } else {
            break;
        }
    }
    if (i == len) {
        return false;
    }
    const char *start = p + i;
    const char *lf = memchr(start, '\n', len - i);
    const char *end = lf == NULL ? p + len : lf;
    const char *space = memchr(start, ' ', (size_t)(end - start));
    if (space == NULL || !http_token_valid((str_t){start, (size_t)(space - start)})) {
        return false;
    }
    start = space + 1;
    space = memchr(start, ' ', (size_t)(end - start));
    return (size_t)((space == NULL ? end : space) - start) > max_uri;
}

/* "메서드 SP 대상 SP 버전"을 셋으로 나눈다. 정확히 SP 두 개로 나뉜 비지 않은 세 토큰만 허용. */
static int split_request_line(str_t line, str_t out_tok[3])
{
    size_t start = 0;
    int n = 0;

    if (memchr(line.ptr, '\t', line.len) != NULL) {
        return -1;
    }
    for (size_t i = 0; i <= line.len; i++) {
        if (i < line.len && line.ptr[i] != ' ') {
            continue;
        }
        if (i == start || n == 3) {
            return -1;
        }
        out_tok[n].ptr = line.ptr + start;
        out_tok[n].len = i - start;
        n++;
        start = i + 1;
    }
    return n == 3 ? 0 : -1;
}

/* 버전 토큰을 검사한다. 0 성공(*out_minor), 그 밖에는 상태 코드(400·505). */
static int parse_version(str_t v, int *out_minor)
{
    const char *dot;
    size_t major_len;

    if (v.len < 5 || memcmp(v.ptr, "HTTP/", 5) != 0) {
        return 400;
    }
    dot = memchr(v.ptr + 5, '.', v.len - 5);
    if (dot == NULL) {
        return 400;
    }
    major_len = (size_t)(dot - (v.ptr + 5));
    if (!is_digits(v.ptr + 5, major_len) || !is_digits(dot + 1, v.len - 5 - major_len - 1)) {
        return 400;
    }
    if (str_eq(v, "HTTP/1.1")) {
        *out_minor = 1;
        return 0;
    }
    if (str_eq(v, "HTTP/1.0")) {
        *out_minor = 0;
        return 0;
    }
    return 505;
}

static http_method_t method_of(str_t m)
{
    if (str_eq(m, "GET")) {
        return HTTP_GET;
    }
    if (str_eq(m, "HEAD")) {
        return HTTP_HEAD;
    }
    if (str_eq(m, "POST")) {
        return HTTP_POST;
    }
    return HTTP_OTHER;
}

/* authority-form: '/'·'?'·'#' 없이 ':'를 포함하는 대상 ("a.com:443"). */
static bool is_authority_form(str_t t)
{
    return memchr(t.ptr, '/', t.len) == NULL && memchr(t.ptr, '?', t.len) == NULL &&
           memchr(t.ptr, '#', t.len) == NULL && memchr(t.ptr, ':', t.len) != NULL;
}

/* 대상을 형식별로 나눠 raw_path·query·path(·host)를 채운다. 0 성공, 그 밖에는 상태 코드. */
static int parse_target(str_t target, http_request_t *req, target_form_t *out_form)
{
    str_t rest = target;

    if (target.ptr[0] == '/') {
        *out_form = TARGET_ORIGIN;
    } else if (target.len >= 7 && str_eq_ci((str_t){target.ptr, 7}, "http://")) {
        if (uri_parse_absolute(target, &req->host, &rest) != 0) {
            return 400;
        }
        *out_form = TARGET_ABSOLUTE;
    } else if (str_eq(target, "*") || is_authority_form(target)) {
        return 501;
    } else {
        return 400;
    }
    if (uri_split(rest, &req->raw_path, &req->query) != 0 ||
        uri_decode_path(req->raw_path, req->path, sizeof(req->path), NULL) != 0 ||
        path_normalize(req->path) != 0) {
        return 400;
    }
    return 0;
}

http_parse_result_t request_parse_head(const char *p, size_t len, const http_limits_t *lim,
                                       http_request_t *out_req, int *out_status)
{
    http_parse_result_t r;
    size_t skip = 0;
    size_t head_len = 0;
    const char *line;
    const char *lf;
    size_t line_len;
    size_t hdr_off;
    size_t blank_len;
    str_t tok[3];
    target_form_t form;
    int st = 0;

    /* 1. 머리 끝 */
    r = http_find_head_end(p, len, lim->max_head, &skip, &head_len, &st);
    if (r != HTTP_PARSE_DONE) {
        if (request_line_too_long(p, len, lim->max_uri)) {
            return fail(out_status, 414);
        }
        if (r == HTTP_PARSE_ERROR) {
            return fail(out_status, st);
        }
        return r;
    }

    /* 2. 요청 라인: p + skip부터 첫 LF까지 (CR 하나는 줄 끝에 포함) */
    line = p + skip;
    lf = memchr(line, '\n', head_len - skip);
    line_len = (size_t)(lf - line);
    hdr_off = skip + line_len + 1;
    if (line_len > 0 && line[line_len - 1] == '\r') {
        line_len--;
    }
    if (split_request_line((str_t){line, line_len}, tok) != 0) {
        return fail(out_status, 400);
    }

    /* 3. 메서드 */
    if (!http_token_valid(tok[0])) {
        return fail(out_status, 400);
    }
    out_req->method_raw = tok[0];
    out_req->method = method_of(tok[0]);
    if (out_req->method == HTTP_OTHER) {
        return fail(out_status, 501);
    }

    /* 4. 버전 */
    st = parse_version(tok[2], &out_req->minor);
    if (st != 0) {
        return fail(out_status, st);
    }

    /* 5~7. 대상 */
    out_req->target = tok[1];
    if (tok[1].len > lim->max_uri) {
        return fail(out_status, 414);
    }
    out_req->host.ptr = NULL;
    out_req->host.len = 0;
    st = parse_target(tok[1], out_req, &form);
    if (st != 0) {
        return fail(out_status, st);
    }

    /* 8. 헤더: 요청 라인 다음부터 마지막 빈 줄("\r\n" 또는 "\n") 직전까지 */
    blank_len = (head_len >= 2 && p[head_len - 2] == '\r') ? 2 : 1;
    if (http_parse_headers(p + hdr_off, head_len - blank_len - hdr_off, lim->max_headers,
                           &out_req->headers, &st) != 0) {
        return fail(out_status, st);
    }

    /* 9. Host */
    if (form == TARGET_ORIGIN) {
        const http_header_t *h = http_header_get(&out_req->headers, "Host");
        if (out_req->minor == 1 && http_header_count(&out_req->headers, "Host") != 1) {
            return fail(out_status, 400);
        }
        if (h != NULL) {
            out_req->host = h->value;
        }
    }

    /* 10. 본문 길이 */
    if (http_resolve_framing(&out_req->headers, false, 0, out_req->method, &out_req->framing,
                             &st) != 0) {
        return fail(out_status, 400);
    }
    if (out_req->framing.kind == FRAMING_CHUNKED) {
        return fail(out_status, 501);
    }
    if (out_req->framing.kind == FRAMING_LENGTH && out_req->framing.length > lim->max_body) {
        return fail(out_status, 413);
    }

    /* 11. 연결 유지와 머리 길이 */
    out_req->keep_alive = http_keep_alive(out_req->minor, &out_req->headers);
    out_req->head_len = head_len;
    out_req->body.ptr = NULL;
    out_req->body.len = 0;
    return HTTP_PARSE_DONE;
}
