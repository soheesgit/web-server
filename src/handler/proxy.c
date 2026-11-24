/* src/handler/proxy.c
 *
 * 프록시 판단. 명세: handler.h
 */
#include "handler/handler.h"

#include "common/http_framing.h"
#include "common/http_headers.h"
#include "core/platform.h"
#include "handler/plan.h"
#include "upstream/request_writer.h"
#include "upstream/upstream.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* 직렬화한 요청에서 프록시가 덧붙이는 헤더(Host, X-Forwarded-*)를 위한 여유 */
#define PROXY_REQ_SLACK 4096

handler_result_t proxy_begin(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                             proxy_job_t *out_job, response_plan_t *out_plan)
{
    upstream_group_t *g = rt->upstream;
    backend_t *b = upstream_pick(g, NULL, ctx->now_ms);
    upstream_rewrite_t rw;
    size_t max = ctx->lim->max_head + (size_t)ctx->lim->max_body + PROXY_REQ_SLACK;

    if (b == NULL) {
        (void)plan_error(out_plan, 503);
        return HANDLER_DONE;
    }
    out_job->group = g;
    out_job->backend = b;
    out_job->method = req->method;
    out_job->attempts = 1;
    out_job->max_attempts = (int)upstream_backend_count(g);
    out_job->sent_any = false;
    out_job->client_minor = req->minor;
    out_job->client_keep_alive = req->keep_alive;

    memset(&rw, 0, sizeof(rw));
    rw.client_ip = ctx->client_ip;
    rw.upstream_host = str_from_cstr(backend_hostport(b));
    rw.keep_host = rt->keep_host;
    if (rt->strip_prefix) {
        /* "/api/"는 끝 '/'를 남겨 "/api/users" → "/users" */
        rw.strip_prefix.ptr = rt->prefix;
        rw.strip_prefix.len = rt->prefix_len;
        if (rw.strip_prefix.len > 0 && rt->prefix[rt->prefix_len - 1] == '/') {
            rw.strip_prefix.len--;
        }
    }
    if (buf_init(&out_job->request, req->head_len + req->body.len + PROXY_REQ_SLACK, max) != 0 ||
        request_write_upstream(req, &rw, &out_job->request) != 0) {
        buf_cleanup(&out_job->request);
        (void)plan_error(out_plan, 500);
        return HANDLER_DONE;
    }
    return HANDLER_NEED_UPSTREAM;
}

/* 재시도할 다른 백엔드로 바꾼다. 없으면 false. */
static bool switch_backend(proxy_job_t *job, int64_t now_ms)
{
    backend_t *next;

    if (job->attempts >= job->max_attempts) {
        return false;
    }
    next = upstream_pick(job->group, job->backend, now_ms);
    if (next == NULL) {
        return false;
    }
    job->backend = next;
    job->attempts++;
    job->sent_any = false;
    return true;
}

proxy_next_t proxy_on_fail(proxy_job_t *job, proxy_fail_t why, response_plan_t *out_plan)
{
    int64_t now_ms = platform_now_ms();
    bool retryable;
    int status = 502;

    upstream_report(job->backend, false, now_ms);
    switch (why) {
    case PFAIL_CONNECT:
        retryable = true;
        break;
    case PFAIL_SEND:
        /* 보내기 시작한 요청은 멱등 메서드만 다시 보낸다 */
        retryable = !job->sent_any || job->method == HTTP_GET || job->method == HTTP_HEAD;
        break;
    case PFAIL_TIMEOUT:
        retryable = false; /* D-004 */
        status = 504;
        break;
    case PFAIL_BAD_RESPONSE:
    default:
        retryable = false;
        break;
    }
    if (retryable && switch_backend(job, now_ms)) {
        return PROXY_RETRY;
    }
    (void)plan_error(out_plan, status);
    return PROXY_GIVE_UP;
}

/* 업스트림 헤더 한 줄을 계획에 복사한다 (str_t → NUL 종료 사본). */
static int copy_field(response_plan_t *p, const http_header_t *h)
{
    char *s = malloc(h->name.len + h->value.len + 2);
    int rc;

    if (s == NULL) {
        return -1;
    }
    memcpy(s, h->name.ptr, h->name.len);
    s[h->name.len] = '\0';
    if (h->value.len > 0) {
        memcpy(s + h->name.len + 1, h->value.ptr, h->value.len);
    }
    s[h->name.len + 1 + h->value.len] = '\0';
    rc = plan_add_header(p, s, s + h->name.len + 1);
    free(s);
    return rc;
}

/* 프록시가 다시 만드는 헤더 (D-015). */
static bool regenerated(str_t name)
{
    return str_eq_ci(name, "Date") || str_eq_ci(name, "Server") ||
           str_eq_ci(name, "Content-Length");
}

static http_framing_t client_framing(const proxy_job_t *job, http_framing_t up)
{
    http_framing_t f = up;

    if (job->method == HTTP_HEAD || up.kind == FRAMING_NONE) {
        f.kind = FRAMING_NONE;
        f.length = 0;
    } else if (up.kind == FRAMING_CHUNKED && job->client_minor == 0) {
        f.kind = FRAMING_CLOSE;
    }
    return f;
}

int proxy_on_head(proxy_job_t *job, struct upstream_conn *c, const http_response_t *resp,
                  response_plan_t *out_plan)
{
    out_plan->head.status = resp->status;
    for (size_t i = 0; i < resp->headers.n; i++) {
        const http_header_t *h = &resp->headers.v[i];

        if (http_is_hop_by_hop(h->name, &resp->headers) || regenerated(h->name)) {
            continue;
        }
        if (copy_field(out_plan, h) != 0) {
            upstream_release(c, false);
            (void)plan_error(out_plan, 502);
            errno = ENOMEM;
            return -1;
        }
    }
    out_plan->head.framing = client_framing(job, resp->framing);
    if (out_plan->head.framing.kind == FRAMING_CLOSE || !job->client_keep_alive) {
        out_plan->head.close = true;
    }
    out_plan->kind = BODY_UPSTREAM;
    out_plan->body.upstream.conn = c;
    http_body_tracker_init(&out_plan->body.upstream.tracker, resp->framing);
    upstream_report(c->backend, true, platform_now_ms());
    return 0;
}

void proxy_job_cleanup(proxy_job_t *job)
{
    if (job == NULL) {
        return;
    }
    buf_cleanup(&job->request);
    job->backend = NULL;
    job->group = NULL;
}
