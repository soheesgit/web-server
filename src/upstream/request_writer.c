/* src/upstream/request_writer.c
 *
 * 업스트림 요청 직렬화. 명세: request_writer.h
 */
#include "upstream/request_writer.h"

#include "common/http_headers.h"

#include <errno.h>
#include <string.h>

static int skip_header(str_t name)
{
    return str_eq_ci(name, "Host") || str_eq_ci(name, "X-Forwarded-For") ||
           str_eq_ci(name, "X-Forwarded-Host") || str_eq_ci(name, "X-Forwarded-Proto");
}

int request_write_upstream(const http_request_t *req, const upstream_rewrite_t *rw, buf_t *out)
{
    size_t old_len, i;
    str_t path;
    int rc = -1;
    if (req == NULL || rw == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    old_len = out->len;
    if (rw->keep_host && (req->host.len == 0 || req->host.ptr == NULL)) {
        errno = EINVAL;
        goto done;
    }
    if (!rw->keep_host && (rw->upstream_host.len == 0 || rw->upstream_host.ptr == NULL)) {
        errno = EINVAL;
        goto done;
    }
    path = req->raw_path;
    if (path.ptr == NULL || path.len == 0)
        path = STR_LIT("/");
    if (rw->strip_prefix.len > 0 && rw->strip_prefix.ptr != NULL &&
        path.len >= rw->strip_prefix.len &&
        memcmp(path.ptr, rw->strip_prefix.ptr, rw->strip_prefix.len) == 0) {
        path.ptr += rw->strip_prefix.len;
        path.len -= rw->strip_prefix.len;
    }
    if (path.len == 0 || path.ptr[0] != '/') {
        if (buf_append(out, "/", 1) != 0)
            goto done;
    }
    if (req->method_raw.ptr == NULL ||
        buf_append(out, req->method_raw.ptr, req->method_raw.len) != 0 ||
        buf_append(out, " ", 1) != 0 || buf_append(out, path.ptr, path.len) != 0)
        goto done;
    if (req->query.len > 0 &&
        (buf_append(out, "?", 1) != 0 || buf_append(out, req->query.ptr, req->query.len) != 0))
        goto done;
    if (buf_append(out, " HTTP/1.1\r\nHost: ", sizeof(" HTTP/1.1\r\nHost: ") - 1) != 0)
        goto done;
    if (rw->keep_host) {
        if (buf_append(out, req->host.ptr, req->host.len) != 0)
            goto done;
    } else if (buf_append(out, rw->upstream_host.ptr, rw->upstream_host.len) != 0)
        goto done;
    if (buf_append(out, "\r\n", 2) != 0)
        goto done;
    for (i = 0; i < req->headers.n; i++) {
        const http_header_t *h = &req->headers.v[i];
        if (!http_is_hop_by_hop(h->name, &req->headers) && !skip_header(h->name)) {
            if (buf_appendf(out, "%.*s: %.*s\r\n", (int)h->name.len, h->name.ptr, (int)h->value.len,
                            h->value.ptr) != 0)
                goto done;
        }
    }
    if (buf_append(out, "X-Forwarded-For: ", sizeof("X-Forwarded-For: ") - 1) != 0)
        goto done;
    {
        int first = 1;
        for (i = 0; i < req->headers.n; i++) {
            const http_header_t *h = &req->headers.v[i];
            if (str_eq_ci(h->name, "X-Forwarded-For")) {
                if ((!first && buf_append(out, ", ", 2) != 0) ||
                    buf_append(out, h->value.ptr, h->value.len) != 0)
                    goto done;
                first = 0;
            }
        }
        if (!first && buf_append(out, ", ", 2) != 0)
            goto done;
        if (rw->client_ip.len > 0 && rw->client_ip.ptr != NULL) {
            if (buf_append(out, rw->client_ip.ptr, rw->client_ip.len) != 0)
                goto done;
        } else if (buf_append(out, "-", 1) != 0)
            goto done;
    }
    if (buf_append(out, "\r\nX-Forwarded-Host: ", sizeof("\r\nX-Forwarded-Host: ") - 1) != 0 ||
        buf_append(out, req->host.ptr, req->host.len) != 0 ||
        buf_append(out, "\r\nX-Forwarded-Proto: http\r\n\r\n",
                   sizeof("\r\nX-Forwarded-Proto: http\r\n\r\n") - 1) != 0 ||
        buf_append(out, req->body.ptr, req->body.len) != 0)
        goto done;
    rc = 0;
done:
    if (rc != 0)
        out->len = old_len;
    return rc;
}
