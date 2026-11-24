/* 요청 하나를 HTTP/1.1 origin-form으로 직렬화한다. */
#include "upstream/request_writer.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "common/http_headers.h"

static int field(buf_t *out, str_t name, str_t value)
{
    return buf_append(out, name.ptr, name.len) || buf_append(out, ": ", 2) ||
                   buf_append(out, value.ptr, value.len) || buf_append(out, "\r\n", 2)
               ? -1
               : 0;
}

static int request_line(const http_request_t *req, str_t prefix, buf_t *out)
{
    str_t path = req->raw_path;
    if (prefix.len > 0 && path.len >= prefix.len && memcmp(path.ptr, prefix.ptr, prefix.len) == 0) {
        path.ptr += prefix.len;
        path.len -= prefix.len;
    }
    if (buf_append(out, req->method_raw.ptr, req->method_raw.len) != 0 ||
        buf_append(out, " ", 1) != 0 ||
        ((path.len == 0 || path.ptr[0] != '/') && buf_append(out, "/", 1) != 0) ||
        buf_append(out, path.ptr, path.len) != 0) {
        return -1;
    }
    if (req->query.ptr != NULL &&
        (buf_append(out, "?", 1) != 0 || buf_append(out, req->query.ptr, req->query.len) != 0)) {
        return -1;
    }
    return buf_append(out, " HTTP/1.1\r\n", 11);
}

static int forwarded_for(const http_request_t *req, str_t ip, buf_t *out)
{
    str_t name = str_from_cstr("X-Forwarded-For");
    if (buf_append(out, "X-Forwarded-For: ", 17) != 0) {
        return -1;
    }
    if (!http_is_hop_by_hop(name, &req->headers)) {
        for (size_t i = 0; i < req->headers.n; i++) {
            const http_header_t *h = &req->headers.v[i];
            if (str_eq_ci_str(h->name, name) && h->value.len != 0 &&
                (buf_append(out, h->value.ptr, h->value.len) != 0 ||
                 buf_append(out, ", ", 2) != 0)) {
                return -1;
            }
        }
    }
    return buf_append(out, ip.ptr, ip.len) || buf_append(out, "\r\n", 2) ? -1 : 0;
}

int request_write_upstream(const http_request_t *req, const upstream_rewrite_t *rw, buf_t *out)
{
    const size_t start = out->len;
    str_t host = rw->keep_host && req->host.len != 0 ? req->host : rw->upstream_host;
    if ((req->framing.kind != FRAMING_NONE && req->framing.kind != FRAMING_LENGTH) ||
        (req->framing.kind == FRAMING_LENGTH && req->framing.length != req->body.len) ||
        (req->framing.kind == FRAMING_NONE && req->body.len != 0)) {
        errno = EINVAL;
        return -1;
    }
    if (request_line(req, rw->strip_prefix, out) != 0 ||
        field(out, str_from_cstr("Host"), host) != 0) {
        goto fail;
    }
    for (size_t i = 0; i < req->headers.n; i++) {
        const http_header_t *h = &req->headers.v[i];
        if (http_is_hop_by_hop(h->name, &req->headers) || str_eq_ci(h->name, "Host") ||
            str_eq_ci(h->name, "Content-Length") || str_eq_ci(h->name, "X-Forwarded-For") ||
            str_eq_ci(h->name, "X-Forwarded-Host") || str_eq_ci(h->name, "X-Forwarded-Proto") ||
            str_eq_ci(h->name, "Expect")) {
            continue;
        }
        if (field(out, h->name, h->value) != 0) {
            goto fail;
        }
    }
    /* 길이는 연결 헤더가 가리킨 이름과 관계없이 검증한 경계를 한 번만 쓴다. */
    if ((req->framing.kind == FRAMING_LENGTH &&
         buf_appendf(out, "Content-Length: %" PRIu64 "\r\n", req->framing.length) != 0) ||
        forwarded_for(req, rw->client_ip, out) != 0 ||
        field(out, str_from_cstr("X-Forwarded-Host"), req->host) != 0 ||
        field(out, str_from_cstr("X-Forwarded-Proto"), str_from_cstr("http")) != 0 ||
        buf_append(out, "\r\n", 2) != 0 || buf_append(out, req->body.ptr, req->body.len) != 0) {
        goto fail;
    }
    return 0;

fail:
    out->len = start;
    return -1;
}
