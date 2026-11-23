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
#include "core/stub.h"
#include "handler/handler.h"
#include "upstream/upstream.h"

void plan_init(response_plan_t *p)
{
    memset(p, 0, sizeof(*p));
    /* FILE과 PIPE는 첫 fd 위치를 공유한다. body의 유효한 멤버는 kind로 구분한다. */
    p->body.pipe.out_fd = -1;
    p->body.pipe.in_fd = -1;
    p->body.pipe.deadline_ms = -1;
}

int plan_set_mem(response_plan_t *p, int status, const char *content_type, const void *body,
                 size_t n)
{
    /* body가 기존 계획을 가리킬 수도 있으므로 복사를 끝낸 뒤 이전 자원을 정리한다. */
    response_plan_t next;

    plan_init(&next);
    next.kind = BODY_MEM;
    next.body.mem.data = (buf_t){0};
    next.head.status = status;
    next.head.framing = (http_framing_t){FRAMING_LENGTH, n};
    if (status < 100 || status > 599 || (n != 0 && body == NULL)) {
        errno = EINVAL;
    } else if (buf_init(&next.body.mem.data, n, n == 0 ? 1 : n) == 0 &&
               buf_append(&next.body.mem.data, body, n) == 0 &&
               (content_type == NULL ||
                plan_add_header(&next, "Content-Type", content_type) == 0)) {
        plan_cleanup(p);
        *p = next;
        return 0;
    }
    plan_cleanup(&next);
    plan_cleanup(p);
    return -1;
}

int plan_error(response_plan_t *p, int status)
{
    /* 오류 응답으로 교체하더라도 호출자가 미리 정한 연결 종료 정책을 유지한다. */
    bool close_after = p->head.close;

    plan_cleanup(p);
    p->head.status = status;
    if (status < 400 || status > 599) {
        errno = EINVAL;
        return -1;
    }

    p->kind = BODY_MEM;
    p->body.mem.data = (buf_t){0};
    if (buf_init(&p->body.mem.data, 128, 1024) != 0 ||
        http_error_body(status, &p->body.mem.data) != 0 ||
        plan_add_header(p, "Content-Type", "text/html; charset=utf-8") != 0) {
        plan_cleanup(p);
        p->head.status = status;
        return -1;
    }
    p->head.framing = (http_framing_t){FRAMING_LENGTH, p->body.mem.data.len};
    p->head.close = close_after;
    return 0;
}

static bool plan_header_valid(str_t name, const char *value)
{
    /* 응답 직렬화가 생성하는 헤더와 이름이 겹치지 않게 한다. */
    static const char *const reserved[] = {
        "Date", "Server", "Content-Length", "Connection", "Transfer-Encoding",
    };

    if (!http_token_valid(name)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (str_eq_ci(name, reserved[i])) {
            return false;
        }
    }
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        if ((*p < 0x20 && *p != '\t') || *p == 0x7F) {
            return false;
        }
    }
    return true;
}

int plan_add_header(response_plan_t *p, const char *name, const char *value)
{
    buf_t fields = {0};

    if (!plan_header_valid(str_from_cstr(name), value)) {
        errno = EINVAL;
        return -1;
    }
    if (p->head.fields.data != NULL) {
        return buf_appendf(&p->head.fields, "%s: %s\r\n", name, value);
    }

    /* 새 버퍼는 첫 헤더를 추가하는 데 성공한 뒤에만 계획으로 넘긴다. */
    if (buf_init(&fields, 256, 64 * 1024) != 0 ||
        buf_appendf(&fields, "%s: %s\r\n", name, value) != 0) {
        buf_cleanup(&fields);
        return -1;
    }
    p->head.fields = fields;
    return 0;
}

static void reap_child(pid_t pid)
{
    /* 종료한 자식은 바로 회수하고, 아직 살아 있을 때만 종료 시그널을 보낸다. */
    pid_t result;

    do {
        result = waitpid(pid, NULL, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        (void)kill(pid, SIGKILL);
        do {
            result = waitpid(pid, NULL, 0);
        } while (result < 0 && errno == EINTR);
    }
}

void plan_cleanup(response_plan_t *p)
{
    int saved = errno;

    if (p == NULL) {
        return;
    }
    switch (p->kind) {
    case BODY_NONE:
        break;
    case BODY_MEM:
        buf_cleanup(&p->body.mem.data);
        break;
    case BODY_FILE:
        if (p->body.file.fd >= 0) {
            (void)close(p->body.file.fd);
        }
        break;
    case BODY_PIPE:
        if (p->body.pipe.out_fd >= 0) {
            (void)close(p->body.pipe.out_fd);
        }
        if (p->body.pipe.in_fd >= 0 && p->body.pipe.in_fd != p->body.pipe.out_fd) {
            (void)close(p->body.pipe.in_fd);
        }
        if (p->body.pipe.pid > 0) {
            reap_child(p->body.pipe.pid);
        }
        buf_cleanup(&p->body.pipe.in_data);
        break;
    case BODY_UPSTREAM:
        /* 완료한 중계는 호출자가 먼저 반납한다. 계획에 남은 연결은 재사용하지 않는다. */
        if (p->body.upstream.conn != NULL) {
            upstream_release(p->body.upstream.conn, false);
        }
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
    UNUSED(req);
    UNUSED(rt);
    UNUSED(ctx);
    UNUSED(out_job);
    UNUSED(out_plan);
    /* TODO(handler): 구현 — E-ST-*, E-CG-*, E-PX-* */
    STUB_RETURN(HANDLER_DONE);
}
