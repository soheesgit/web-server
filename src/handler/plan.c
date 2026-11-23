/* src/handler/plan.c
 *
 * 응답 계획과 처리기 분기. 명세: plan.h, handler.h
 */
#include "handler/plan.h"

#include "common/http_headers.h"
#include "common/http_status.h"
#include "handler/handler.h"
#include "upstream/upstream.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void plan_init(response_plan_t *p)
{
    if (p == NULL)
        return;
    memset(p, 0, sizeof(*p));
    p->kind = BODY_NONE;
    p->body.file.fd = -1;
    p->body.pipe.out_fd = -1;
    p->body.pipe.in_fd = -1;
    p->body.pipe.deadline_ms = -1;
    /* body는 union이라 upstream.conn = NULL을 쓰면 위 fd들이 0(stdin)으로 덮인다 (D-039). */
}

void plan_cleanup(response_plan_t *p)
{
    int saved = errno;
    if (p == NULL)
        return;
    if (p->kind == BODY_MEM) {
        buf_cleanup(&p->body.mem.data);
    } else if (p->kind == BODY_FILE) {
        if (p->body.file.fd >= 0)
            (void)close(p->body.file.fd);
    } else if (p->kind == BODY_PIPE) {
        if (p->body.pipe.out_fd >= 0)
            (void)close(p->body.pipe.out_fd);
        if (p->body.pipe.in_fd >= 0)
            (void)close(p->body.pipe.in_fd);
        buf_cleanup(&p->body.pipe.in_data);
        if (p->body.pipe.pid > 0) {
            int status;
            (void)kill(p->body.pipe.pid, SIGKILL);
            while (waitpid(p->body.pipe.pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    } else if (p->kind == BODY_UPSTREAM && p->body.upstream.conn != NULL) {
        upstream_release(p->body.upstream.conn, false);
    }
    buf_cleanup(&p->head.fields);
    plan_init(p);
    errno = saved;
}

int plan_add_header(response_plan_t *p, const char *name, const char *value)
{
    static const char *const reserved[] = {"Date", "Server", "Content-Length", "Connection",
                                           "Transfer-Encoding"};
    size_t i, old_len;
    str_t ns;
    if (p == NULL || name == NULL || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    ns = str_from_cstr(name);
    if (!http_token_valid(ns) || strchr(name, '\r') != NULL || strchr(name, '\n') != NULL ||
        strchr(value, '\r') != NULL || strchr(value, '\n') != NULL) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (str_eq_ci(ns, reserved[i])) {
            errno = EINVAL;
            return -1;
        }
    }
    if (p->head.fields.max == 0 && buf_init(&p->head.fields, 256, 64u * 1024u) != 0)
        return -1;
    old_len = p->head.fields.len;
    if (buf_appendf(&p->head.fields, "%s: %s\r\n", name, value) != 0) {
        p->head.fields.len = old_len;
        return -1;
    }
    return 0;
}

int plan_set_mem(response_plan_t *p, int status, const char *content_type, const void *body,
                 size_t n)
{
    if (p == NULL || status < 100 || status > 599 || (body == NULL && n != 0)) {
        errno = EINVAL;
        return -1;
    }
    plan_cleanup(p);
    p->head.status = status;
    p->head.framing = (http_framing_t){FRAMING_LENGTH, n};
    p->kind = n == 0 ? BODY_NONE : BODY_MEM;
    if (n > 0) {
        if (buf_init(&p->body.mem.data, n, n) != 0 || buf_append(&p->body.mem.data, body, n) != 0) {
            plan_cleanup(p);
            p->head.status = status;
            return -1;
        }
    }
    if (content_type != NULL && plan_add_header(p, "Content-Type", content_type) != 0) {
        plan_cleanup(p);
        p->head.status = status;
        return -1;
    }
    return 0;
}

int plan_error(response_plan_t *p, int status)
{
    bool close_after;
    buf_t body = {0};
    int rc;
    if (p == NULL || status < 400 || status > 599) {
        errno = EINVAL;
        return -1;
    }
    close_after = p->head.close;
    plan_cleanup(p);
    p->head.close = close_after;
    p->head.status = status;
    if (buf_init(&body, 128, 4096) != 0)
        return -1;
    rc = http_error_body(status, &body);
    if (rc == 0)
        rc = plan_set_mem(p, status, "text/html; charset=utf-8", body.data, body.len);
    buf_cleanup(&body);
    if (rc != 0) {
        plan_cleanup(p);
        p->head.status = status;
        p->head.close = close_after;
        return -1;
    }
    p->head.close = close_after;
    return 0;
}

handler_result_t handler_dispatch(const http_request_t *req, const route_t *rt,
                                  const handler_ctx_t *ctx, proxy_job_t *out_job,
                                  response_plan_t *out_plan)
{
    if (req == NULL || rt == NULL || ctx == NULL || out_plan == NULL) {
        if (out_plan != NULL)
            (void)plan_error(out_plan, 500);
        return HANDLER_DONE;
    }
    switch (rt->kind) {
    case ROUTE_STATIC:
        return static_handle(req, rt, ctx, out_plan);
    case ROUTE_CGI:
        return cgi_handle(req, rt, ctx, out_plan);
    case ROUTE_PROXY:
    case ROUTE_FORWARD:
        if (out_job == NULL) {
            (void)plan_error(out_plan, 500);
            return HANDLER_DONE;
        }
        return proxy_begin(req, rt, ctx, out_job, out_plan);
    }
    (void)plan_error(out_plan, 500);
    return HANDLER_DONE;
}
