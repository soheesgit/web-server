/* 클라이언트 HTTP 요청 머리 해석. */
#include "downstream/request_parser.h"

#include <string.h>

#include "common/http_framing.h"
#include "common/http_headers.h"
#include "downstream/uri.h"

/* 요청 줄이 끝나기를 기다리는 중에도 선행 빈 줄을 제외하고 길이 한도를 확인한다. */
static bool unfinished_line_too_long(const char *p, size_t len, size_t max_uri)
{
    size_t off = 0;
    while (off < len) {
        if (p[off] == '\n') {
            ++off;
        } else if (len - off > 1 && p[off] == '\r' && p[off + 1] == '\n') {
            off += 2;
        } else {
            break;
        }
    }
    size_t remaining = len - off;
    return remaining > 16 && remaining - 16 > max_uri && memchr(p + off, '\n', remaining) == NULL;
}

static bool split_start_line(str_t line, str_t *method, str_t *target, str_t *version)
{
    const char *first = memchr(line.ptr, ' ', line.len);
    if (first == NULL || first == line.ptr || memchr(line.ptr, '\t', line.len) != NULL) {
        return false;
    }

    const char *end = line.ptr + line.len;
    const char *next = memchr(first + 1, ' ', (size_t)(end - first - 1));
    if (next == NULL || next == first + 1 || next + 1 == end ||
        memchr(next + 1, ' ', (size_t)(end - next - 1)) != NULL) {
        return false;
    }
    *method = (str_t){line.ptr, (size_t)(first - line.ptr)};
    *target = (str_t){first + 1, (size_t)(next - first - 1)};
    *version = (str_t){next + 1, (size_t)(end - next - 1)};
    return true;
}

static int decode_method(str_t token, http_method_t *out)
{
    static const struct {
        const char *token;
        http_method_t method;
    } methods[] = {{"GET", HTTP_GET}, {"HEAD", HTTP_HEAD}, {"POST", HTTP_POST}};

    if (!http_token_valid(token)) {
        return 400;
    }
    for (size_t i = 0; i < sizeof(methods) / sizeof(*methods); ++i) {
        if (str_eq(token, methods[i].token)) {
            *out = methods[i].method;
            return 0;
        }
    }
    return 501;
}

static int decode_version(str_t version, int *out)
{
    if (version.len < 8 || memcmp(version.ptr, "HTTP/", 5) != 0) {
        return 400;
    }

    /* 버전 표기가 잘못된 경우(400)와 지원하지 않는 버전인 경우(505)를 구분한다. */
    bool dot = false;
    for (size_t i = 5; i < version.len; ++i) {
        char c = version.ptr[i];
        if (c == '.' && !dot && i > 5 && i < version.len - 1) {
            dot = true;
        } else if (c < '0' || c > '9') {
            return 400;
        }
    }
    if (!dot) {
        return 400;
    }
    if (version.len != 8 || version.ptr[5] != '1' || version.ptr[7] > '1') {
        return 505;
    }
    *out = version.ptr[7] - '0';
    return 0;
}

static bool looks_like_authority(str_t target)
{
    return memchr(target.ptr, ':', target.len) != NULL &&
           memchr(target.ptr, '/', target.len) == NULL &&
           memchr(target.ptr, '?', target.len) == NULL &&
           memchr(target.ptr, '#', target.len) == NULL;
}

static int decode_target(http_request_t *req)
{
    str_t target = req->target;
    str_t path_query = target;

    for (size_t i = 0; i < target.len; ++i) {
        unsigned char c = (unsigned char)target.ptr[i];
        if (c <= 0x20 || c == 0x7f) {
            return 400;
        }
    }

    if (target.ptr[0] != '/') {
        if (target.len >= 7 && str_eq_ci((str_t){target.ptr, 7}, "http://")) {
            if (uri_parse_absolute(target, &req->host, &path_query) < 0) {
                return 400;
            }
        } else {
            return str_eq(target, "*") || looks_like_authority(target) ? 501 : 400;
        }
    }

    /* 업스트림에 전달할 원본 경로와 라우터가 사용할 디코딩된 경로를 따로 보관한다. */
    if (uri_split(path_query, &req->raw_path, &req->query) < 0 ||
        uri_decode_path(req->raw_path, req->path, sizeof(req->path), NULL) < 0 ||
        path_normalize(req->path) < 0) {
        return 400;
    }
    return 0;
}

static int read_start_line(str_t line, size_t max_uri, http_request_t *req)
{
    str_t version;
    if (!split_start_line(line, &req->method_raw, &req->target, &version)) {
        return 400;
    }

    int status = decode_method(req->method_raw, &req->method);
    if (status != 0) {
        return status;
    }
    status = decode_version(version, &req->minor);
    if (status != 0) {
        return status;
    }
    if (req->target.len > max_uri) {
        return 414;
    }
    return decode_target(req);
}

static int read_request_fields(const char *p, size_t len, const http_limits_t *lim,
                               http_request_t *req)
{
    int status;
    if (http_parse_headers(p, len, lim->max_headers, &req->headers, &status) < 0) {
        return status;
    }
    /* 절대 URI의 호스트는 요청 줄에서 이미 정했다. 경로 형식에서만 Host를 읽는다. */
    if (req->target.ptr[0] == '/') {
        if (req->minor == 1 && http_header_count(&req->headers, "Host") != 1) {
            return 400;
        }
        const http_header_t *host = http_header_get(&req->headers, "Host");
        if (host != NULL) {
            req->host = host->value;
        }
    }

    if (http_resolve_framing(&req->headers, false, 0, req->method, &req->framing, &status) < 0) {
        return status;
    }
    /* 공통 파서는 chunked를 인식하지만 요청 처리는 아직 D-003의 501 정책을 따른다. */
    if (req->framing.kind == FRAMING_CHUNKED) {
        return 501;
    }
    if (req->framing.kind == FRAMING_LENGTH && req->framing.length > lim->max_body) {
        return 413;
    }
    return 0;
}

http_parse_result_t request_parse_head(const char *p, size_t len, const http_limits_t *lim,
                                       http_request_t *out_req, int *out_status)
{
    size_t skip, total;
    http_parse_result_t result =
        http_find_head_end(p, len, lim->max_head, &skip, &total, out_status);
    if (result != HTTP_PARSE_DONE) {
        if (unfinished_line_too_long(p, len, lim->max_uri)) {
            *out_status = 414;
            return HTTP_PARSE_ERROR;
        }
        return result;
    }

    const char *start = p + skip;
    const char *lf = memchr(start, '\n', total - skip);
    size_t line_len = (size_t)(lf - start);
    if (line_len != 0 && start[line_len - 1] == '\r') {
        --line_len;
    }
    /* 검증을 끝낸 뒤 결과를 넘긴다. 범위 필드들은 원본 수신 버퍼를 빌린다. */
    http_request_t req = {0};
    int status = read_start_line((str_t){start, line_len}, lim->max_uri, &req);
    if (status == 0) {
        size_t blank_len = p[total - 2] == '\r' ? 2 : 1;
        const char *fields = lf + 1;
        status = read_request_fields(fields, (size_t)(p + total - blank_len - fields), lim, &req);
    }
    if (status != 0) {
        *out_status = status;
        return HTTP_PARSE_ERROR;
    }

    /* 여기서 본문을 읽지 않는다. 호출자가 이 위치부터 이어서 처리한다. */
    req.head_len = total;
    req.keep_alive = http_keep_alive(req.minor, &req.headers);
    *out_req = req;
    return HTTP_PARSE_DONE;
}
