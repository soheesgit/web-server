/* src/upstream/request_writer.c
 *
 * 업스트림 요청 직렬화. 명세: request_writer.h
 */
#include "upstream/request_writer.h"

#include "common/http_headers.h"

#include <stddef.h>
#include <string.h>

static int put(buf_t *out, const char *p, size_t n)
{
    return n == 0 ? 0 : buf_append(out, p, n);
}

static int put_cstr(buf_t *out, const char *s)
{
    return put(out, s, strlen(s));
}

static int put_str(buf_t *out, str_t s)
{
    return put(out, s.ptr, s.len);
}

/* 요청 라인의 경로: strip_prefix를 떼고, 비거나 '/'로 시작하지 않으면 '/'를 붙인다. */
static int put_path(buf_t *out, str_t raw, str_t strip)
{
    if (strip.len > 0 && raw.len >= strip.len && memcmp(raw.ptr, strip.ptr, strip.len) == 0) {
        raw.ptr += strip.len;
        raw.len -= strip.len;
        if (raw.len == 0 || raw.ptr[0] != '/') {
            if (put_cstr(out, "/") != 0) {
                return -1;
            }
        }
    }
    return put_str(out, raw);
}

/* 프록시가 다시 쓰는 헤더: 클라이언트 값을 그대로 복사하지 않는다. */
static bool rewritten(str_t name)
{
    return str_eq_ci(name, "Host") || str_eq_ci(name, "X-Forwarded-For") ||
           str_eq_ci(name, "X-Forwarded-Host") || str_eq_ci(name, "X-Forwarded-Proto");
}

static int put_forwarded_for(buf_t *out, const http_request_t *req, str_t client_ip)
{
    if (put_cstr(out, "X-Forwarded-For: ") != 0) {
        return -1;
    }
    for (size_t i = 0; i < req->headers.n; i++) {
        const http_header_t *h = &req->headers.v[i];

        if (str_eq_ci(h->name, "X-Forwarded-For") && h->value.len > 0) {
            if (put_str(out, h->value) != 0 || put_cstr(out, ", ") != 0) {
                return -1;
            }
        }
    }
    return put_str(out, client_ip) != 0 || put_cstr(out, "\r\n") != 0 ? -1 : 0;
}

static int write_all(const http_request_t *req, const upstream_rewrite_t *rw, buf_t *out)
{
    /* 1. 요청 라인 (항상 HTTP/1.1) */
    if (put_str(out, req->method_raw) != 0 || put_cstr(out, " ") != 0 ||
        put_path(out, req->raw_path, rw->strip_prefix) != 0) {
        return -1;
    }
    if (req->query.len > 0 && (put_cstr(out, "?") != 0 || put_str(out, req->query) != 0)) {
        return -1;
    }
    if (put_cstr(out, " HTTP/1.1\r\n") != 0) {
        return -1;
    }

    /* 2. Host */
    if (put_cstr(out, "Host: ") != 0 ||
        put_str(out, rw->keep_host ? req->host : rw->upstream_host) != 0 ||
        put_cstr(out, "\r\n") != 0) {
        return -1;
    }

    /* 3. end-to-end 헤더를 받은 순서대로 */
    for (size_t i = 0; i < req->headers.n; i++) {
        const http_header_t *h = &req->headers.v[i];

        if (http_is_hop_by_hop(h->name, &req->headers) || rewritten(h->name)) {
            continue;
        }
        if (put_str(out, h->name) != 0 || put_cstr(out, ": ") != 0 || put_str(out, h->value) != 0 ||
            put_cstr(out, "\r\n") != 0) {
            return -1;
        }
    }

    /* 4. 전달 헤더. 5. Connection은 넣지 않는다 */
    if (put_forwarded_for(out, req, rw->client_ip) != 0 ||
        put_cstr(out, "X-Forwarded-Host: ") != 0 || put_str(out, req->host) != 0 ||
        put_cstr(out, "\r\nX-Forwarded-Proto: http\r\n") != 0) {
        return -1;
    }

    /* 6. 빈 줄 + 본문 */
    return put_cstr(out, "\r\n") != 0 || put_str(out, req->body) != 0 ? -1 : 0;
}

int request_write_upstream(const http_request_t *req, const upstream_rewrite_t *rw, buf_t *out)
{
    size_t start = out->len;

    if (write_all(req, rw, out) != 0) {
        out->len = start;
        return -1;
    }
    return 0;
}
