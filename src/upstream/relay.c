/* src/upstream/relay.c
 *
 * 프록시 I/O 실행. 명세: relay.h
 *
 * v1·v2가 같은 상태 머신을 쓴다. relay_exchange_blocking은 relay_step이 원하는 이벤트를
 * poll로 기다리며 돌리는 얇은 래퍼다.
 */
#include "upstream/relay.h"

#include "core/platform.h"
#include "upstream/response_parser.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* engine.h의 EV_* 값 (D-021). upstream은 engine을 include할 수 없어 같은 값을 둔다. */
enum { RELAY_EV_READ = 1u, RELAY_EV_WRITE = 2u, RELAY_EV_ERROR = 4u };

typedef enum {
    ST_CONNECT, /* non-blocking connect 완료 대기 */
    ST_SEND,    /* job->request 송신 */
    ST_HEAD,    /* 응답 머리 수신 */
    ST_DONE     /* OK 또는 FAIL (result) */
} relay_state_t;

struct relay {
    proxy_job_t *job; /* 빌림 */
    const http_limits_t *lim;
    int connect_timeout_ms;
    int response_timeout_ms;
    relay_state_t st;
    relay_status_t result; /* ST_DONE일 때 OK 또는 FAIL */
    proxy_fail_t fail;
    upstream_conn_t *conn; /* 넘기기 전까지 소유 */
    size_t sent;           /* job->request에서 보낸 바이트 */
    bool got_bytes;        /* 이 연결에서 응답 바이트를 하나라도 받았는가 */
    bool redialed;         /* D-011 새 연결을 이미 썼는가 */
    int64_t deadline_ms;   /* 현재 단계 마감. 없으면 -1 */
    http_response_t resp;
    bool taken;
};

static relay_status_t finish(relay_t *r, relay_status_t s, proxy_fail_t why)
{
    r->st = ST_DONE;
    r->result = s;
    r->fail = why;
    r->deadline_ms = -1;
    if (s == RELAY_FAIL && r->conn != NULL) {
        upstream_release(r->conn, false);
        r->conn = NULL;
    }
    return s;
}

/* 연결을 얻고 connect 단계로 간다. 즉시 실패면 false. */
static bool dial(relay_t *r, int64_t now_ms)
{
    if (upstream_acquire(r->job->group, r->job->backend, &r->conn) != 0) {
        r->conn = NULL;
        return false;
    }
    r->sent = 0;
    r->got_bytes = false;
    r->job->sent_any = false;
    r->st = ST_CONNECT;
    r->deadline_ms = now_ms + r->connect_timeout_ms;
    return true;
}

/* D-011: 재사용 연결이 첫 송신·응답 전에 죽었으면 같은 백엔드로 새 연결 1회. */
static bool can_redial(const relay_t *r)
{
    return r->conn->reused && !r->redialed && !r->got_bytes;
}

static relay_status_t redial(relay_t *r, int64_t now_ms)
{
    upstream_release(r->conn, false);
    r->conn = NULL;
    r->redialed = true;
    if (!dial(r, now_ms)) {
        return finish(r, RELAY_FAIL, PFAIL_CONNECT);
    }
    return RELAY_WANT_WRITE;
}

static void enter_send(relay_t *r, int64_t now_ms)
{
    r->st = ST_SEND;
    r->deadline_ms = now_ms + r->response_timeout_ms;
}

static relay_status_t step_connect(relay_t *r, unsigned events, int64_t now_ms)
{
    if (r->conn->connecting) {
        if ((events & (RELAY_EV_WRITE | RELAY_EV_ERROR)) == 0) {
            return now_ms >= r->deadline_ms ? finish(r, RELAY_FAIL, PFAIL_CONNECT)
                                            : RELAY_WANT_WRITE;
        }
        if (upstream_connect_result(r->conn) != 0) {
            return finish(r, RELAY_FAIL, PFAIL_CONNECT);
        }
    }
    enter_send(r, now_ms);
    return RELAY_WANT_WRITE;
}

static relay_status_t step_send(relay_t *r, int64_t now_ms)
{
    const buf_t *req = &r->job->request;

    while (r->sent < req->len) {
        ssize_t n = platform_send(r->conn->fd, req->data + r->sent, req->len - r->sent);

        if (n > 0) {
            r->sent += (size_t)n;
            r->job->sent_any = true;
            r->deadline_ms = now_ms + r->response_timeout_ms; /* 진행하면 연장 */
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return now_ms >= r->deadline_ms ? finish(r, RELAY_FAIL, PFAIL_SEND) : RELAY_WANT_WRITE;
        }
        return can_redial(r) ? redial(r, now_ms) : finish(r, RELAY_FAIL, PFAIL_SEND);
    }
    r->st = ST_HEAD;
    r->deadline_ms = now_ms + r->response_timeout_ms; /* 송신 완료 시각 기준 */
    return RELAY_WANT_READ;
}

/* rbuf 앞에서 머리를 파싱한다. 1xx는 버리고 다시 본다. */
static relay_status_t try_parse(relay_t *r)
{
    for (;;) {
        int st = 0;
        http_parse_result_t pr = response_parse_head(r->conn->rbuf.data, r->conn->rbuf.len,
                                                     r->job->method, r->lim, &r->resp, &st);

        if (pr == HTTP_PARSE_INCOMPLETE) {
            return RELAY_WANT_READ;
        }
        if (pr == HTTP_PARSE_ERROR) {
            return finish(r, RELAY_FAIL, PFAIL_BAD_RESPONSE);
        }
        if (r->resp.status >= 200) {
            return finish(r, RELAY_OK, PFAIL_CONNECT);
        }
        buf_consume(&r->conn->rbuf, r->resp.head_len);
    }
}

static relay_status_t step_head(relay_t *r, int64_t now_ms)
{
    for (;;) {
        ssize_t n = buf_read_fd(&r->conn->rbuf, r->conn->fd);
        relay_status_t s;

        if (n > 0) {
            r->got_bytes = true;
            s = try_parse(r);
            if (s != RELAY_WANT_READ) {
                return s;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return now_ms >= r->deadline_ms ? finish(r, RELAY_FAIL, PFAIL_TIMEOUT)
                                            : RELAY_WANT_READ;
        }
        /* EOF·리셋. 머리를 다 받기 전이다 */
        return can_redial(r) ? redial(r, now_ms) : finish(r, RELAY_FAIL, PFAIL_BAD_RESPONSE);
    }
}

relay_t *relay_start(proxy_job_t *job, const http_limits_t *lim, int64_t now_ms,
                     int connect_timeout_ms, int response_timeout_ms)
{
    relay_t *r = calloc(1, sizeof(*r));

    if (r == NULL) {
        return NULL;
    }
    r->job = job;
    r->lim = lim;
    r->connect_timeout_ms = connect_timeout_ms;
    r->response_timeout_ms = response_timeout_ms;
    r->deadline_ms = -1;
    if (!dial(r, now_ms)) {
        (void)finish(r, RELAY_FAIL, PFAIL_CONNECT);
    }
    return r;
}

relay_status_t relay_step(relay_t *r, unsigned events, int64_t now_ms)
{
    relay_state_t before;
    relay_status_t s;

    do {
        before = r->st;
        switch (r->st) {
        case ST_CONNECT:
            s = step_connect(r, events, now_ms);
            break;
        case ST_SEND:
            s = step_send(r, now_ms);
            break;
        case ST_HEAD:
            s = step_head(r, now_ms);
            break;
        case ST_DONE:
        default:
            return r->result;
        }
        events = 0; /* 다음 단계는 막힐 때까지 바로 시도 */
    } while (r->st != before && r->st != ST_DONE && !(r->st == ST_CONNECT && r->conn->connecting));
    return s;
}

int relay_fd(const relay_t *r)
{
    return r->conn != NULL ? r->conn->fd : -1;
}

int64_t relay_deadline_ms(const relay_t *r)
{
    return r->deadline_ms;
}

void relay_take(relay_t *r, upstream_conn_t **out_conn, http_response_t *out_resp,
                proxy_fail_t *out_fail)
{
    *out_conn = NULL;
    if (r->st != ST_DONE || r->taken) {
        *out_fail = PFAIL_CONNECT;
        return;
    }
    r->taken = true;
    if (r->result == RELAY_OK) {
        *out_conn = r->conn;
        *out_resp = r->resp;
        r->conn = NULL;
        return;
    }
    *out_fail = r->fail;
}

void relay_free(relay_t *r)
{
    int e = errno;

    if (r == NULL) {
        return;
    }
    upstream_release(r->conn, false);
    free(r);
    errno = e;
}

/* 원하는 이벤트를 마감까지 기다린다. 결과 이벤트 비트, 시간 초과면 0. */
static unsigned wait_fd(int fd, relay_status_t want, int64_t deadline_ms)
{
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = want == RELAY_WANT_READ ? POLLIN : POLLOUT;
    for (;;) {
        int64_t left = deadline_ms < 0 ? -1 : deadline_ms - platform_now_ms();
        int rc;

        if (deadline_ms >= 0 && left <= 0) {
            return 0;
        }
        pfd.revents = 0;
        rc = poll(&pfd, 1, left > 1000000 ? 1000000 : (int)left);
        if (rc > 0) {
            return ((pfd.revents & POLLIN) ? RELAY_EV_READ : 0u) |
                   ((pfd.revents & POLLOUT) ? RELAY_EV_WRITE : 0u) |
                   ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) ? RELAY_EV_ERROR : 0u);
        }
        if (rc == 0) {
            return 0;
        }
        if (errno != EINTR) {
            return RELAY_EV_ERROR; /* relay_step이 I/O를 시도해 실패로 끝낸다 */
        }
    }
}

relay_status_t relay_exchange_blocking(proxy_job_t *job, const http_limits_t *lim,
                                       int connect_timeout_ms, int response_timeout_ms,
                                       upstream_conn_t **out_conn, http_response_t *out_resp,
                                       proxy_fail_t *out_fail)
{
    relay_t *r = relay_start(job, lim, platform_now_ms(), connect_timeout_ms, response_timeout_ms);
    relay_status_t s;
    unsigned ev = 0;

    *out_conn = NULL;
    if (r == NULL) {
        *out_fail = PFAIL_CONNECT;
        return RELAY_FAIL;
    }
    for (;;) {
        s = relay_step(r, ev, platform_now_ms());
        if (s == RELAY_OK || s == RELAY_FAIL) {
            break;
        }
        ev = wait_fd(relay_fd(r), s, relay_deadline_ms(r));
    }
    relay_take(r, out_conn, out_resp, out_fail);
    relay_free(r);
    return s;
}
