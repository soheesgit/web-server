/* src/downstream/request_parser.c
 *
 * 요청 머리 파서. 명세: request_parser.h
 */
#include "downstream/request_parser.h"

#include <errno.h>
#include <string.h>

#include "common/http_framing.h"
#include "common/http_headers.h"
#include "downstream/uri.h"

/* scheme 비교용: ASCII 영문자만 소문자로 접는다. */
static unsigned char lower_ascii(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* request-target이 absolute-form의 http:// scheme으로 시작하는지 본다. */
static bool starts_http_scheme(str_t s)
{
    static const char scheme[] = "http://";
    if (s.ptr == NULL || s.len < sizeof(scheme) - 1) {
        return false;
    }
    for (size_t i = 0; i < sizeof(scheme) - 1; i++) {
        if (lower_ascii((unsigned char)s.ptr[i]) != (unsigned char)scheme[i]) {
            return false;
        }
    }
    return true;
}

/* HTTP/<major>.<minor> 모양을 확인한다. 문법이 맞지만 1.0/1.1이 아니면 out_minor=-1이다. */
static bool version_parse(str_t v, int *out_minor)
{
    static const char prefix[] = "HTTP/";
    if (v.ptr == NULL || v.len < sizeof(prefix) - 1) {
        return false;
    }
    if (memcmp(v.ptr, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    if (v.len == 8 && v.ptr[5] >= '0' && v.ptr[5] <= '9' && v.ptr[6] == '.' && v.ptr[7] >= '0' &&
        v.ptr[7] <= '9') {
        if (v.ptr[5] == '1' && (v.ptr[7] == '0' || v.ptr[7] == '1')) {
            *out_minor = (int)(v.ptr[7] - '0');
            return true;
        }
        *out_minor = -1;
        return true;
    }
    return false;
}

/* 요청보다 앞에 붙은 빈 줄(CRLF 또는 LF)의 총 바이트 수를 센다. */
static size_t initial_blank_bytes(const char *p, size_t len)
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
    return i;
}

/* 요청 줄이 아직 끝나지 않았는데 URI 상한을 넘었는지 조기 판정한다. */
static bool first_line_too_long(const char *p, size_t len, size_t max_uri)
{
    size_t start = initial_blank_bytes(p, len);
    size_t method_end = start;
    size_t target_start;
    size_t target_end;

    while (method_end < len && p[method_end] != ' ' && p[method_end] != '\n') {
        method_end++;
    }
    if (method_end == start || method_end == len || p[method_end] != ' ') {
        return false;
    }
    target_start = method_end + 1;
    target_end = target_start;
    while (target_end < len && p[target_end] != ' ' && p[target_end] != '\n') {
        target_end++;
    }
    return target_end - target_start > max_uri;
}

/* 헤더 크기 제한을 넘긴 입력이 정상적인 HTTP 요청 줄로 시작했는지 확인한다. */
static bool has_http_request_line(const char *p, size_t len)
{
    size_t start = initial_blank_bytes(p, len);
    size_t end = start;
    size_t first_space;
    size_t second_space;

    while (end < len && p[end] != '\n') {
        end++;
    }
    if (end == len) {
        return false;
    }
    if (end > start && p[end - 1] == '\r') {
        end--;
    }
    first_space = start;
    while (first_space < end && p[first_space] != ' ') {
        first_space++;
    }
    if (first_space == start || first_space == end) {
        return false;
    }
    second_space = first_space + 1;
    while (second_space < end && p[second_space] != ' ') {
        second_space++;
    }
    return second_space + 5 <= end && memcmp(p + second_space + 1, "HTTP/", 5) == 0;
}

/* 오류 상태를 out_status에 기록하고 파서 결과 ERROR를 돌려준다. */
static int request_error(int status, int *out_status)
{
    if (out_status != NULL) {
        *out_status = status;
    }
    return HTTP_PARSE_ERROR;
}

/* 역할: 요청 머리를 HTTP 규칙으로 검증하고 라우팅/본문 수신에 쓸 요청 구조체를 채운다.
 * 미포함: 요청 본문 바이트 수신은 conn이 담당한다. 이 함수는 머리까지만 읽는다.
 */
http_parse_result_t request_parse_head(const char *p, size_t len, const http_limits_t *lim,
                                       http_request_t *out_req, int *out_status)
{
    http_request_t req;
    http_parse_result_t found;
    size_t skip = 0;
    size_t head_len = 0;
    size_t line_end;
    size_t line_sep;
    size_t delimiter_len;
    size_t headers_start;
    size_t headers_len;
    str_t method;
    str_t target;
    str_t version;
    str_t effective_target;
    str_t authority = {NULL, 0};
    str_t raw_path;
    str_t query;
    int status = 0;
    int minor = -1;

    if (lim == NULL || out_req == NULL || out_status == NULL || (len != 0 && p == NULL)) {
        return (http_parse_result_t)request_error(400, out_status);
    }
    /* 1) 공용 함수로 머리의 끝을 찾는다. 불완전한 입력은 버퍼를 더 받도록 그대로 돌려준다. */
    found = http_find_head_end(p, len, lim->max_head, &skip, &head_len, &status);
    if (found != HTTP_PARSE_DONE) {
        if (first_line_too_long(p, len, lim->max_uri)) {
            return (http_parse_result_t)request_error(414, out_status);
        }
        if (found == HTTP_PARSE_ERROR) {
            /* 정상 요청 줄 없이 상한을 넘긴 잡음은 431이 아니라 400이다. */
            *out_status = has_http_request_line(p, len) ? status : 400;
        }
        return found;
    }

    /* 성공 전까지 임시 req에 채워 오류가 나면 호출자의 출력 객체를 건드리지 않는다. */
    memset(&req, 0, sizeof(req));
    req.minor = -1;
    line_end = skip;
    while (line_end < head_len && p[line_end] != '\n') {
        line_end++;
    }
    if (line_end >= head_len) {
        return (http_parse_result_t)request_error(400, out_status);
    }
    line_sep = line_end > skip && p[line_end - 1] == '\r' ? 2 : 1;
    if (line_sep == 2) {
        line_end--;
    }
    if (line_end <= skip) {
        return (http_parse_result_t)request_error(400, out_status);
    }

    /* 요청 줄을 SP 두 개로 세 토큰(method, target, version)으로 분리한다. */
    size_t first_space = skip;
    while (first_space < line_end && p[first_space] != ' ') {
        first_space++;
    }
    if (first_space == skip || first_space == line_end || first_space + 1 >= line_end ||
        p[first_space + 1] == ' ') {
        return (http_parse_result_t)request_error(400, out_status);
    }
    size_t second_space = first_space + 1;
    while (second_space < line_end && p[second_space] != ' ') {
        second_space++;
    }
    if (second_space == first_space + 1 || second_space == line_end ||
        second_space + 1 >= line_end || p[second_space + 1] == ' ') {
        return (http_parse_result_t)request_error(400, out_status);
    }
    for (size_t i = second_space + 1; i < line_end; i++) {
        if (p[i] == ' ' || p[i] == '\t') {
            return (http_parse_result_t)request_error(400, out_status);
        }
    }
    method.ptr = p + skip;
    method.len = first_space - skip;
    target.ptr = p + first_space + 1;
    target.len = second_space - first_space - 1;
    version.ptr = p + second_space + 1;
    version.len = line_end - second_space - 1;
    /* 2) 메서드 문법과 지원 목록을 구분한다: 잘못된 token은 400, 지원하지 않는 token은 501. */
    if (!http_token_valid(method)) {
        return (http_parse_result_t)request_error(400, out_status);
    }
    if (method.len == 3 && memcmp(method.ptr, "GET", 3) == 0) {
        req.method = HTTP_GET;
    } else if (method.len == 4 && memcmp(method.ptr, "HEAD", 4) == 0) {
        req.method = HTTP_HEAD;
    } else if (method.len == 4 && memcmp(method.ptr, "POST", 4) == 0) {
        req.method = HTTP_POST;
    } else {
        return (http_parse_result_t)request_error(501, out_status);
    }
    req.method_raw = method;
    /* 3) 버전 문법 오류와 문법상 유효하지만 지원하지 않는 버전을 나눈다. */
    if (!version_parse(version, &minor)) {
        return (http_parse_result_t)request_error(400, out_status);
    }
    if (minor < 0) {
        return (http_parse_result_t)request_error(505, out_status);
    }
    req.minor = minor;
    if (target.len > lim->max_uri) {
        return (http_parse_result_t)request_error(414, out_status);
    }
    for (size_t i = 0; i < target.len; i++) {
        unsigned char c = (unsigned char)target.ptr[i];
        if (c <= 0x20 || c == 0x7f) {
            return (http_parse_result_t)request_error(400, out_status);
        }
    }

    /* 4) 대상 형식에 따라 origin-form 경로 또는 absolute-form의 authority/경로를 얻는다. */
    req.target = target;
    effective_target = target;
    if (target.ptr[0] == '/') {
        /* origin-form */
    } else if (starts_http_scheme(target)) {
        if (uri_parse_absolute(target, &authority, &effective_target) != 0) {
            return (http_parse_result_t)request_error(400, out_status);
        }
    } else if ((target.len == 1 && target.ptr[0] == '*') ||
               (memchr(target.ptr, ':', target.len) != NULL &&
                memchr(target.ptr, '/', target.len) == NULL &&
                memchr(target.ptr, '?', target.len) == NULL &&
                memchr(target.ptr, '#', target.len) == NULL)) {
        return (http_parse_result_t)request_error(501, out_status);
    } else {
        return (http_parse_result_t)request_error(400, out_status);
    }
    /* 5) raw 경로와 query를 나눈 뒤 경로만 디코딩하고 정규화한다. */
    if (uri_split(effective_target, &raw_path, &query) != 0 || raw_path.len == 0) {
        return (http_parse_result_t)request_error(400, out_status);
    }
    req.raw_path = raw_path;
    req.query = query;
    if (uri_decode_path(raw_path, req.path, sizeof(req.path), NULL) != 0 ||
        path_normalize(req.path) != 0) {
        return (http_parse_result_t)request_error(400, out_status);
    }

    /* 6) 요청 줄 다음부터 헤더 빈 줄 직전까지만 공용 헤더 파서에 넘긴다. */
    headers_start = skip + (line_end - skip) + line_sep;
    if (head_len >= 4 && memcmp(p + head_len - 4, "\r\n\r\n", 4) == 0) {
        delimiter_len = 2;
    } else if (head_len >= 3 && memcmp(p + head_len - 3, "\n\r\n", 3) == 0) {
        delimiter_len = 2;
    } else if (head_len >= 3 && memcmp(p + head_len - 3, "\r\n\n", 3) == 0) {
        delimiter_len = 1;
    } else if (head_len >= 2 && memcmp(p + head_len - 2, "\n\n", 2) == 0) {
        delimiter_len = 1;
    } else {
        return (http_parse_result_t)request_error(400, out_status);
    }
    if (head_len < headers_start + delimiter_len) {
        return (http_parse_result_t)request_error(400, out_status);
    }
    headers_len = head_len - headers_start - delimiter_len;
    if (http_parse_headers(p + headers_start, headers_len, lim->max_headers, &req.headers,
                           &status) != 0) {
        return (http_parse_result_t)request_error(status, out_status);
    }

    /* 7) absolute-form이면 URI authority가 우선이고, origin-form HTTP/1.1은 Host 하나를 요구한다.
     */
    if (authority.ptr != NULL) {
        req.host = authority;
    } else {
        size_t hosts = http_header_count(&req.headers, "Host");
        const http_header_t *host = http_header_get(&req.headers, "Host");
        if (minor == 1 && hosts != 1) {
            return (http_parse_result_t)request_error(400, out_status);
        }
        if (host != NULL) {
            req.host = host->value;
        }
    }
    /* 8) 본문 경계를 공용 규칙으로 판정한다. 요청 chunked는 이 서버 정책상 501이다. */
    if (http_resolve_framing(&req.headers, false, 0, req.method, &req.framing, &status) != 0) {
        return (http_parse_result_t)request_error(status, out_status);
    }
    if (req.framing.kind == FRAMING_CHUNKED) {
        return (http_parse_result_t)request_error(501, out_status);
    }
    if (req.framing.kind == FRAMING_LENGTH && req.framing.length > lim->max_body) {
        return (http_parse_result_t)request_error(413, out_status);
    }
    /* 파서가 머리 길이와 연결 유지 여부까지 채운 뒤 요청 전체를 호출자에게 넘긴다. */
    req.keep_alive = http_keep_alive(minor, &req.headers);
    req.head_len = head_len;
    *out_req = req;
    return HTTP_PARSE_DONE;
}
