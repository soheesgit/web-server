/* src/handler/proxy.c
 *
 * 프록시 판단. 명세: handler.h
 */
#include "handler/handler.h"

#include "common/http_headers.h"
#include "common/http_framing.h"
#include "core/platform.h"
#include "handler/plan.h"
#include "upstream/request_writer.h"
#include "upstream/upstream.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

handler_result_t proxy_begin(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                             proxy_job_t *out_job, response_plan_t *out_plan)
{
    upstream_rewrite_t rw;
    backend_t *backend;
    size_t max_size;
    if (req == NULL || rt == NULL || ctx == NULL || out_job == NULL || out_plan == NULL ||
        rt->upstream == NULL || ctx->cfg == NULL || ctx->lim == NULL) {
        if (out_plan != NULL)
            (void)plan_error(out_plan, 500);
        return HANDLER_DONE;
    }
    memset(out_job, 0, sizeof(*out_job));
    backend = upstream_pick(rt->upstream, NULL, ctx->now_ms);
    if (backend == NULL) {
        (void)plan_error(out_plan, 503);
        return HANDLER_DONE;
    }
    out_job->group = rt->upstream;
    out_job->backend = backend;
    out_job->method = req->method;
    out_job->attempts = 1;
    out_job->max_attempts = (int)upstream_backend_count(rt->upstream);
    out_job->client_minor = req->minor;
    out_job->client_keep_alive = req->keep_alive;
    if (ctx->lim->max_head > SIZE_MAX - 65536u ||
        ctx->lim->max_body > (uint64_t)(SIZE_MAX - ctx->lim->max_head - 65536u)) {
        (void)plan_error(out_plan, 500);
        return HANDLER_DONE;
    }
    max_size = ctx->lim->max_head + (size_t)ctx->lim->max_body + 65536u;
    if (buf_init(&out_job->request, 4096, max_size) != 0) {
        (void)plan_error(out_plan, 500);
        return HANDLER_DONE;
    }
    rw.client_ip = ctx->client_ip;
    rw.upstream_host = str_from_cstr(backend_hostport(backend));
    rw.keep_host = rt->keep_host;
    rw.strip_prefix = (str_t){NULL, 0};
    if (rt->strip_prefix) {
        size_t n = rt->prefix_len;
        while (n > 1 && rt->prefix[n - 1] == '/')
            n--;
        rw.strip_prefix = (str_t){rt->prefix, n};
    }
    if (request_write_upstream(req, &rw, &out_job->request) != 0) {
        proxy_job_cleanup(out_job);
        (void)plan_error(out_plan, 500);
        return HANDLER_DONE;
    }
    return HANDLER_NEED_UPSTREAM;
}

proxy_next_t proxy_on_fail(proxy_job_t *job, proxy_fail_t why, response_plan_t *out_plan)
{
    int64_t now = platform_now_ms();
    int can_retry = 0, status = 502;
    backend_t *failed;
    if (job == NULL || out_plan == NULL)
        return PROXY_GIVE_UP;
    failed = job->backend;
    upstream_report(failed, false, now);
    if (why == PFAIL_TIMEOUT) {
        status = 504;
    } else if (why == PFAIL_CONNECT) {
        can_retry = 1;
    } else if (why == PFAIL_SEND && !job->sent_any) {
        can_retry = 1;
    } else if (why == PFAIL_SEND && (job->method == HTTP_GET || job->method == HTTP_HEAD)) {
        can_retry = 1;
    }
    if (can_retry && job->attempts < job->max_attempts) {
        backend_t *next = upstream_pick(job->group, failed, now);
        if (next != NULL) {
            job->backend = next;
            job->attempts++;
            job->sent_any = false;
            return PROXY_RETRY;
        }
    }
    (void)plan_error(out_plan, status);
    return PROXY_GIVE_UP;
}

int proxy_on_head(proxy_job_t *job, struct upstream_conn *c, const http_response_t *resp,
                  response_plan_t *out_plan)
{
    size_t i;
    http_framing_t framing;
    if (job == NULL || c == NULL || resp == NULL || out_plan == NULL) {
        errno = EINVAL;
        return -1;
    }
    plan_cleanup(out_plan);
    out_plan->head.status = resp->status;
    out_plan->head.close = !job->client_keep_alive;
    framing = resp->framing;
    if (job->method == HTTP_HEAD || resp->status == 204 || resp->status == 304 ||
        (resp->status >= 100 && resp->status < 200)) {
        framing = (http_framing_t){FRAMING_NONE, 0};
    } else if (framing.kind == FRAMING_CHUNKED && job->client_minor == 0) {
        framing.kind = FRAMING_CLOSE;
    }
    out_plan->head.framing = framing;
    if (framing.kind == FRAMING_CLOSE)
        out_plan->head.close = true;
    if (out_plan->head.fields.max == 0 && buf_init(&out_plan->head.fields, 256, 64u * 1024u) != 0)
        goto fail;
    for (i = 0; i < resp->headers.n; i++) {
        const http_header_t *h = &resp->headers.v[i];
        if (http_is_hop_by_hop(h->name, &resp->headers) || str_eq_ci(h->name, "Date") ||
            str_eq_ci(h->name, "Server") || str_eq_ci(h->name, "Content-Length"))
            continue;
        if (buf_appendf(&out_plan->head.fields, "%.*s: %.*s\r\n", (int)h->name.len, h->name.ptr,
                        (int)h->value.len, h->value.ptr) != 0)
            goto fail;
    }
    out_plan->kind = BODY_UPSTREAM;
    out_plan->body.upstream.conn = c;
    http_body_tracker_init(&out_plan->body.upstream.tracker, resp->framing);
    upstream_report(c->backend, true, platform_now_ms());
    return 0;
fail:
    upstream_release(c, false);
    out_plan->body.upstream.conn = NULL;
    (void)plan_error(out_plan, 502);
    return -1;
}

void proxy_job_cleanup(proxy_job_t *job)
{
    int saved = errno;
    if (job == NULL)
        return;
    buf_cleanup(&job->request);
    job->group = NULL;
    job->backend = NULL;
    errno = saved;
}
