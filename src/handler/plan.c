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
    /*
     * 본문 멤버들은 같은 union 저장 공간을 쓴다. BODY_NONE에는 소유한 자원이 없으므로
     * 여기서는 fd와 마감의 초기값을 기록하고, 실제 본문 멤버는 종류가 정해질 때 채운다(D-039).
     */
    p->body.pipe.out_fd = -1;
    p->body.pipe.in_fd = -1;
    p->body.pipe.deadline_ms = -1;
}

int plan_set_mem(response_plan_t *p, int status, const char *content_type, const void *body,
                 size_t n)
{
    response_plan_t next;
    int result = -1;

    /* body가 기존 계획의 버퍼를 가리킬 수 있어 사본을 만든 뒤 기존 계획을 정리한다. */
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
    /* 응답을 에러 계획으로 바꿔도 이미 내려진 연결 종료 결정은 이어받는다. */
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

    /*
     * 이름과 값이 헤더 한 줄 안에 머물도록 검사한다. 날짜·길이·연결·서버 식별 헤더의
     * 직렬화는 응답 writer가 담당하므로 여기서 추가하면 중복될 수 있다.
     */
    if (!http_token_valid(field) || strpbrk(value, "\r\n") != NULL || str_eq_ci(field, "Date") ||
        str_eq_ci(field, "Server") || str_eq_ci(field, "Content-Length") ||
        str_eq_ci(field, "Connection") || str_eq_ci(field, "Transfer-Encoding")) {
        errno = EINVAL;
        return -1;
    }
    if (fields->max != 0) {
        return buf_appendf(fields, "%s: %s\r\n", name, value);
    }

    /* 첫 줄까지 성공한 뒤 버퍼를 넘겨, 실패하면 처음의 빈 fields 상태를 유지한다. */
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

    /*
     * 이미 끝난 자식은 즉시 회수한다. 아직 실행 중인 자식만 종료한 뒤 기다리며,
     * 신호가 waitpid를 중단하면 재시도해 좀비가 남지 않게 한다.
     */
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
    /* kind로 선택된 본문 멤버만 자원을 소유한다. 응답 헤더 버퍼는 본문과 따로 정리한다. */
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
        /* 중계가 끝난 연결은 호출자가 이미 반납한다. 남은 연결은 재사용 없이 반납한다. */
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
    /* 라우터가 고른 종류를 처리기에 넘긴다. 파일과 CGI의 세부 처리는 각 처리기가 맡는다. */
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
