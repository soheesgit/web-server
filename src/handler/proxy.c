/* 프록시 작업 직렬화, 재시도 판정과 응답 계획. */
#include "handler/handler.h"

#include <errno.h>
#include <string.h>

#include "common/http_framing.h"
#include "common/http_headers.h"
#include "core/platform.h"
#include "handler/internal_cache.h"
#include "handler/plan.h"
#include "upstream/request_writer.h"
#include "upstream/upstream.h"

int proxy_cache_begin(const http_request_t *req, const config_t *cfg, proxy_job_t *job,
                      response_plan_t *plan)
{
    char key[2200];
    size_t key_len;
    bool bypass, store;
    if (cfg->cache_max_bytes <= 0 || (req->method != HTTP_GET && req->method != HTTP_POST)) {
        return 0;
    }
    key_len = cache_make_key(req->target, key, sizeof(key));
    if (key_len == 0) {
        return 0;
    }
    if (req->method == HTTP_POST) {
        cache_invalidate(key, key_len);
        return 0;
    }
    cache_request_policy(req, &bypass, &store);
    if (!bypass && cache_serve(key, key_len, req->keep_alive, plan) == 0) {
        return 1;
    }
    if (store) {
        job->fill = cache_fill_new(key, key_len, (size_t)cfg->cache_entry_max,
                                   (size_t)cfg->cache_max_bytes);
    }
    return 0;
}

handler_result_t proxy_begin(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                             proxy_job_t *out_job, response_plan_t *out_plan)
{
    *out_job = (proxy_job_t){0};
    /* AI 라우팅: picker 줄이 있는 그룹은 X-Session-Id를 고정 키로 고른다 (D-045).
     * 그 밖의 그룹과 키 없는 요청은 upstream_pick과 같다 */
    const http_header_t *sid = http_header_get(&req->headers, "X-Session-Id");
    backend_t *b;
    if (rt->kind == ROUTE_FORWARD) {
        /* 포워드(D-046)는 목적지가 URI에 적힌 absolute-form 요청만 받는다 */
        if (req->target.len == 0 || req->target.ptr[0] == '/') {
            (void)plan_error(out_plan, 400);
            return HANDLER_DONE;
        }
        /* 캐시 적중이면 이름 해석(블로킹)과 연결을 건너뛴다 (D-047) */
        if (proxy_cache_begin(req, ctx->cfg, out_job, out_plan)) {
            return HANDLER_DONE;
        }
        b = upstream_forward_backend(rt->upstream, req->host);
        if (b == NULL) {
            (void)plan_error(out_plan, 502);
            return HANDLER_DONE;
        }
    } else {
        b = upstream_pick_key(rt->upstream, sid != NULL ? sid->value.ptr : NULL,
                              sid != NULL ? sid->value.len : 0, NULL, ctx->now_ms);
        if (b == NULL) {
            (void)plan_error(out_plan, 503);
            return HANDLER_DONE;
        }
    }
    out_job->group = rt->upstream;
    out_job->backend = b;
    out_job->method = req->method;
    out_job->attempts = 1;
    /* 포워드는 목적지가 하나라 다른 백엔드로 재시도하지 않는다 */
    out_job->max_attempts =
        rt->kind == ROUTE_FORWARD ? 1 : (int)upstream_backend_count(rt->upstream);
    out_job->client_minor = req->minor;
    out_job->client_keep_alive = req->keep_alive;
    if (sid != NULL) {
        out_job->affinity_key = sid->value;
    }

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
    if (job->fill != NULL) {
        (void)cache_fill_arm(job->fill, resp, &out_plan->head.fields);
        if (buf_appendf(&out_plan->head.fields, "X-Cache: MISS\r\n") != 0) {
            goto fail;
        }
        if (resp->framing.kind == FRAMING_LENGTH && resp->framing.length == 0) {
            cache_fill_commit(job->fill); /* 본문이 없어 전달 경로가 done을 부르지 않는다 */
        }
    }
    out_plan->kind = BODY_UPSTREAM;
    out_plan->body.upstream.conn = c;
    http_body_tracker_init(&out_plan->body.upstream.tracker, resp->framing);
    return 0;

fail:
    upstream_release(c, false);
    (void)plan_error(out_plan, 502);
    return -1;
}

void proxy_body_tee(proxy_job_t *job, const char *p, size_t n)
{
    if (job != NULL && job->fill != NULL) {
        cache_fill_data(job->fill, p, n);
    }
}

void proxy_body_done(proxy_job_t *job)
{
    if (job != NULL && job->fill != NULL) {
        cache_fill_commit(job->fill);
    }
}

void proxy_job_cleanup(proxy_job_t *job)
{
    if (job != NULL) {
        cache_fill_free(job->fill);
        buf_cleanup(&job->request);
        *job = (proxy_job_t){0};
    }
}
