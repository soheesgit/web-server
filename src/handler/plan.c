/* src/handler/plan.c
 *
 * 응답 계획과 처리기 분기. 명세: plan.h, handler.h
 */
#include "handler/plan.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/http_headers.h"
#include "common/http_status.h"
#include "handler/handler.h"
#include "upstream/upstream.h"

void plan_init(response_plan_t *p)
{
    memset(p, 0, sizeof(*p));
    /* BODY_NONE일 때는 자원을 소유하지 않는다. fd와 마감 값만 비워 둔다. */
    p->body.pipe.out_fd = -1;
    p->body.pipe.in_fd = -1;
    p->body.pipe.deadline_ms = -1;
}

int plan_set_mem(response_plan_t *p, int status, const char *content_type, const void *body,
                 size_t n)
{
    response_plan_t next;
    int result = -1;

    plan_init(&next);
    if (status < 100 || status > 599) {
        errno = EINVAL;
    } else {
        next.kind = BODY_MEM;
        next.body.mem.data = (buf_t){0};
        next.head.status = status;
        next.head.framing = (http_framing_t){FRAMING_LENGTH, n};
        if (buf_init(&next.body.mem.data, n, n == 0 ? 1 : n) == 0 &&
            buf_append(&next.body.mem.data, body, n) == 0 &&
            (content_type == NULL || plan_add_header(&next, "Content-Type", content_type) == 0)) {
            result = 0;
        }
    }

    plan_cleanup(p);
    if (result == 0) {
        *p = next;
    } else {
        plan_cleanup(&next);
    }
    return result;
}

int plan_error(response_plan_t *p, int status)
{
    bool close_after = p->head.close;

    plan_cleanup(p);
    p->head.status = status;
    if (status < 400 || status > 599) {
        errno = EINVAL;
        return -1;
    }

    p->kind = BODY_MEM;
    p->body.mem.data = (buf_t){0};
    if (buf_init(&p->body.mem.data, 256, 64 * 1024) < 0 ||
        http_error_body(status, &p->body.mem.data) < 0 ||
        plan_add_header(p, "Content-Type", "text/html; charset=utf-8") < 0) {
        plan_cleanup(p);
        p->head.status = status;
        return -1;
    }
    p->head.close = close_after;
    p->head.framing = (http_framing_t){FRAMING_LENGTH, p->body.mem.data.len};
    return 0;
}

int plan_add_header(response_plan_t *p, const char *name, const char *value)
{
    str_t field = str_from_cstr(name);
    buf_t *fields = &p->head.fields;

    if (!http_token_valid(field) || strpbrk(value, "\r\n") != NULL || str_eq_ci(field, "Date") ||
        str_eq_ci(field, "Server") || str_eq_ci(field, "Content-Length") ||
        str_eq_ci(field, "Connection") || str_eq_ci(field, "Transfer-Encoding")) {
        errno = EINVAL;
        return -1;
    }
    if (fields->max != 0) {
        return buf_appendf(fields, "%s: %s\r\n", name, value);
    }

    buf_t first;
    if (buf_init(&first, 256, 64 * 1024) < 0) {
        return -1;
    }
    if (buf_appendf(&first, "%s: %s\r\n", name, value) < 0) {
        buf_cleanup(&first);
        return -1;
    }
    *fields = first;
    return 0;
}

static void reap_child(pid_t pid)
{
    pid_t got;

    do {
        got = waitpid(pid, NULL, WNOHANG);
    } while (got < 0 && errno == EINTR);
    if (got == 0) {
        (void)kill(pid, SIGKILL);
        do {
            got = waitpid(pid, NULL, 0);
        } while (got < 0 && errno == EINTR);
    }
}

void plan_cleanup(response_plan_t *p)
{
    int saved = errno;

    if (p == NULL) {
        return;
    }
    switch (p->kind) {
    case BODY_MEM:
        buf_cleanup(&p->body.mem.data);
        break;
    case BODY_FILE:
        if (p->body.file.fd >= 0) {
            (void)close(p->body.file.fd);
        }
        break;
    case BODY_PIPE:
        if (p->body.pipe.in_fd >= 0) {
            (void)close(p->body.pipe.in_fd);
        }
        if (p->body.pipe.out_fd >= 0) {
            (void)close(p->body.pipe.out_fd);
        }
        if (p->body.pipe.pid > 0) {
            reap_child(p->body.pipe.pid);
        }
        buf_cleanup(&p->body.pipe.in_data);
        break;
    case BODY_UPSTREAM:
        if (p->body.upstream.conn != NULL) {
            upstream_release(p->body.upstream.conn, false);
        }
        break;
    case BODY_NONE:
        break;
    }
    buf_cleanup(&p->head.fields);
    plan_init(p);
    errno = saved;
}

handler_result_t handler_dispatch(const http_request_t *req, const route_t *rt,
                                  const handler_ctx_t *ctx, proxy_job_t *out_job,
                                  response_plan_t *out_plan)
{
    switch (rt->kind) {
    case ROUTE_STATIC:
        return static_handle(req, rt, ctx, out_plan);
    case ROUTE_CGI:
        return cgi_handle(req, rt, ctx, out_plan);
    case ROUTE_PROXY:
        return proxy_begin(req, rt, ctx, out_job, out_plan);
    }
    (void)plan_error(out_plan, 500);
    return HANDLER_DONE;
}
