/* 프록시 작업 직렬화, 재시도 판정과 응답 계획. */
#include "handler/handler.h"

#include <errno.h>
#include <string.h>

#include "common/http_framing.h"
#include "common/http_headers.h"
#include "core/platform.h"
#include "handler/plan.h"
#include "upstream/request_writer.h"
#include "upstream/upstream.h"

handler_result_t proxy_begin(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                             proxy_job_t *out_job, response_plan_t *out_plan)
{
    *out_job = (proxy_job_t){0};
    backend_t *b = upstream_pick(rt->upstream, NULL, ctx->now_ms);
    if (b == NULL) {
        (void)plan_error(out_plan, 503);
        return HANDLER_DONE;
    }
    out_job->group = rt->upstream;
    out_job->backend = b;
    out_job->method = req->method;
    out_job->attempts = 1;
    out_job->max_attempts = (int)upstream_backend_count(rt->upstream);
    out_job->client_minor = req->minor;
    out_job->client_keep_alive = req->keep_alive;

    upstream_rewrite_t rw = {
        ctx->client_ip, str_from_cstr(backend_hostport(b)), {0}, rt->keep_host};
    if (rt->strip_prefix) {
        rw.strip_prefix = (str_t){rt->prefix, rt->prefix_len};
        if (rw.strip_prefix.len > 0 && rw.strip_prefix.ptr[rw.strip_prefix.len - 1] == '/') {
            rw.strip_prefix.len--;
        }
    }
    if (req->body.len > SIZE_MAX - req->head_len - 1024 ||
        buf_init(&out_job->request, 0, req->head_len + req->body.len + 1024) != 0 ||
        request_write_upstream(req, &rw, &out_job->request) != 0) {
        proxy_job_cleanup(out_job);
        (void)plan_error(out_plan, 500);
        return HANDLER_DONE;
    }
    return HANDLER_NEED_UPSTREAM;
}

proxy_next_t proxy_on_fail(proxy_job_t *job, proxy_fail_t why, response_plan_t *out_plan)
{
    bool retry = why == PFAIL_CONNECT ||
                 (why == PFAIL_SEND &&
                  (!job->sent_any || job->method == HTTP_GET || job->method == HTTP_HEAD));
    if (retry && job->attempts < job->max_attempts) {
        backend_t *b = upstream_pick(job->group, job->backend, platform_now_ms());
        if (b != NULL) {
            job->backend = b;
            job->attempts++;
            job->sent_any = false;
            return PROXY_RETRY;
        }
    }
    (void)plan_error(out_plan, why == PFAIL_TIMEOUT ? 504 : 502);
    return PROXY_GIVE_UP;
}

int proxy_on_head(proxy_job_t *job, upstream_conn_t *c, const http_response_t *resp,
                  response_plan_t *out_plan)
{
    if (buf_init(&out_plan->head.fields, 256, 1u << 20) != 0) {
        goto fail;
    }
    for (size_t i = 0; i < resp->headers.n; i++) {
        const http_header_t *h = &resp->headers.v[i];
        if (http_is_hop_by_hop(h->name, &resp->headers) || str_eq_ci(h->name, "Date") ||
            str_eq_ci(h->name, "Server") || str_eq_ci(h->name, "Content-Length")) {
            continue;
        }
        buf_t *fields = &out_plan->head.fields;
        if (buf_append(fields, h->name.ptr, h->name.len) != 0 || buf_append(fields, ": ", 2) != 0 ||
            buf_append(fields, h->value.ptr, h->value.len) != 0 ||
            buf_append(fields, "\r\n", 2) != 0) {
            goto fail;
        }
    }
    out_plan->head.status = resp->status;
    out_plan->head.framing = resp->framing;
    if (resp->framing.kind == FRAMING_CHUNKED && job->client_minor == 0) {
        out_plan->head.framing.kind = FRAMING_CLOSE;
    }
    if (job->method == HTTP_HEAD) {
        http_framing_t framing;
        int ignored;
        out_plan->head.framing = (http_framing_t){FRAMING_NONE, 0};
        if (http_resolve_framing(&resp->headers, true, 200, HTTP_GET, &framing, &ignored) == 0 &&
            framing.kind == FRAMING_LENGTH) {
            out_plan->head.framing = framing;
        }
    }
    out_plan->head.close = !job->client_keep_alive || out_plan->head.framing.kind == FRAMING_CLOSE;
    out_plan->kind = BODY_UPSTREAM;
    out_plan->body.upstream.conn = c;
    http_body_tracker_init(&out_plan->body.upstream.tracker, resp->framing);
    return 0;

fail:
    upstream_release(c, false);
    (void)plan_error(out_plan, 502);
    return -1;
}

void proxy_job_cleanup(proxy_job_t *job)
{
    if (job != NULL) {
        buf_cleanup(&job->request);
        *job = (proxy_job_t){0};
    }
}
