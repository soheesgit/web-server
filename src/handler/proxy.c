/* src/handler/proxy.c
 *
 * 프록시 판단. 명세: handler.h
 */
#include "handler/handler.h"

#include "common/http_headers.h"
#include "common/http_framing.h"
#include "core/platform.h"
#include "handler/internal_cache.h"
#include "handler/plan.h"
#include "upstream/request_writer.h"
#include "upstream/upstream.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

/* 역할: 포워드 요청의 캐시를 본다 (D-042). GET은 적중이면 계획을 만들고 1을 돌려준다. 적중이 아니면
 * 0이고 저장할 만한 요청에는 job->fill을 준비해 둔다. POST는 같은 URL의 항목을 지운다. */
static int forward_cache_begin(const http_request_t *req, const handler_ctx_t *ctx,
                               proxy_job_t *job, response_plan_t *plan)
{
    char key[2200];
    size_t key_len;
    bool bypass, store;
    if (ctx->cfg->cache_max_bytes <= 0 || (req->method != HTTP_GET && req->method != HTTP_POST))
        return 0;
    key_len = cache_make_key(req->target, key, sizeof(key));
    if (key_len == 0)
        return 0;
    if (req->method == HTTP_POST) {
        cache_invalidate(key, key_len);
        return 0;
    }
    cache_request_policy(req, &bypass, &store);
    if (!bypass && cache_serve(key, key_len, req->keep_alive, plan) == 0)
        return 1;
    if (store)
        job->fill = cache_fill_new(key, key_len, (size_t)ctx->cfg->cache_entry_max,
                                   (size_t)ctx->cfg->cache_max_bytes);
    return 0;
}

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
    if (rt->kind == ROUTE_FORWARD) {
        /* 포워드 프록시는 목적지가 URI에 적힌 absolute-form 요청만 받는다 (D-041). */
        static const char scheme[] = "http://";
        if (req->target.len < sizeof(scheme) - 1 ||
            !str_eq_ci((str_t){req->target.ptr, sizeof(scheme) - 1}, scheme)) {
            (void)plan_error(out_plan, 400);
            return HANDLER_DONE;
        }
        /* 캐시 적중이면 이름 해석(블로킹)과 연결을 건너뛴다. */
        if (forward_cache_begin(req, ctx, out_job, out_plan))
            return HANDLER_DONE;
        backend = upstream_forward_backend(rt->upstream, req->host);
        if (backend == NULL) {
            (void)plan_error(out_plan, 502);
            return HANDLER_DONE;
        }
    } else {
        backend = upstream_pick(rt->upstream, NULL, ctx->now_ms);
        if (backend == NULL) {
            (void)plan_error(out_plan, 503);
            return HANDLER_DONE;
        }
    }
    out_job->group = rt->upstream;
    out_job->backend = backend;
    out_job->method = req->method;
    out_job->attempts = 1;
    /* 포워드는 목적지가 하나라 다른 백엔드로 재시도하지 않는다. */
    out_job->max_attempts =
        rt->kind == ROUTE_FORWARD ? 1 : (int)upstream_backend_count(rt->upstream);
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
    if (job->fill != NULL) {
        (void)cache_fill_arm(job->fill, resp, &out_plan->head.fields);
        if (buf_appendf(&out_plan->head.fields, "X-Cache: MISS\r\n") != 0)
            goto fail;
        if (resp->framing.kind == FRAMING_LENGTH && resp->framing.length == 0)
            cache_fill_commit(job->fill); /* 본문이 없어 전달 경로가 DONE을 부르지 않는다 */
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

void proxy_body_tee(proxy_job_t *job, const char *p, size_t n)
{
    if (job != NULL && job->fill != NULL)
        cache_fill_data(job->fill, p, n);
}

void proxy_body_done(proxy_job_t *job)
{
    if (job != NULL && job->fill != NULL)
        cache_fill_commit(job->fill);
}

void proxy_job_cleanup(proxy_job_t *job)
{
    int saved = errno;
    if (job == NULL)
        return;
    cache_fill_free(job->fill);
    job->fill = NULL;
    buf_cleanup(&job->request);
    job->group = NULL;
    job->backend = NULL;
    errno = saved;
}
