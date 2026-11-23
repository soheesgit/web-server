/* src/handler/plan.c
 *
 * 응답 계획과 처리기 분기. 명세: plan.h, handler.h
 */
#include "handler/plan.h"
#include "handler/handler.h"

#include "common/http_status.h"
#include "core/buf.h"
#include "upstream/upstream.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIELDS_INITIAL 256
#define FIELDS_MAX (64 * 1024)
#define ERROR_BODY_MAX 4096

void plan_init(response_plan_t *p)
{
    memset(p, 0, sizeof(*p));
    p->kind = BODY_NONE;
    p->head.framing.kind = FRAMING_NONE;
    /* union이라 upstream.conn(0)과 겹친다. kind가 NONE이므로 아래 값이 남는다 */
    p->body.file.fd = -1;
    p->body.pipe.out_fd = -1;
    p->body.pipe.in_fd = -1;
    p->body.pipe.pid = 0;
    p->body.pipe.deadline_ms = -1;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static void reap(pid_t pid)
{
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
}

void plan_cleanup(response_plan_t *p)
{
    int e = errno;

    if (p == NULL) {
        return;
    }
    switch (p->kind) {
    case BODY_MEM:
        buf_cleanup(&p->body.mem.data);
        break;
    case BODY_FILE:
        close_fd(&p->body.file.fd);
        break;
    case BODY_PIPE:
        close_fd(&p->body.pipe.in_fd);
        close_fd(&p->body.pipe.out_fd);
        if (p->body.pipe.pid > 0) {
            reap(p->body.pipe.pid);
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
    errno = e;
}

/* writer가 붙이는 헤더 */
static bool writer_only(const char *name)
{
    static const char *const names[] = {"Date", "Server", "Content-Length", "Connection",
                                        "Transfer-Encoding"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(name, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

int plan_add_header(response_plan_t *p, const char *name, const char *value)
{
    if (strpbrk(name, "\r\n") != NULL || strpbrk(value, "\r\n") != NULL || writer_only(name)) {
        errno = EINVAL;
        return -1;
    }
    if (p->head.fields.max == 0 && buf_init(&p->head.fields, FIELDS_INITIAL, FIELDS_MAX) != 0) {
        return -1;
    }
    return buf_appendf(&p->head.fields, "%s: %s\r\n", name, value);
}

int plan_set_mem(response_plan_t *p, int status, const char *content_type, const void *body,
                 size_t n)
{
    plan_cleanup(p);
    if (status < 100 || status > 599) {
        errno = EINVAL;
        return -1;
    }
    p->kind = BODY_MEM;
    p->head.status = status;
    p->head.framing.kind = FRAMING_LENGTH;
    p->head.framing.length = n;
    if (buf_init(&p->body.mem.data, n, n > 0 ? n : 1) != 0 ||
        buf_append(&p->body.mem.data, body, n) != 0 ||
        (content_type != NULL && plan_add_header(p, "Content-Type", content_type) != 0)) {
        plan_cleanup(p);
        return -1;
    }
    return 0;
}

int plan_error(response_plan_t *p, int status)
{
    bool close_after = p->head.close;
    int rc = -1;

    plan_cleanup(p);
    if (status < 400 || status > 599) {
        errno = EINVAL;
    } else {
        buf_t body;

        if (buf_init(&body, 256, ERROR_BODY_MAX) == 0 && http_error_body(status, &body) == 0) {
            rc = plan_set_mem(p, status, "text/html; charset=utf-8", body.data, body.len);
        }
        buf_cleanup(&body);
    }
    if (rc != 0) {
        p->head.status = status;
    }
    p->head.close = close_after;
    return rc;
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
