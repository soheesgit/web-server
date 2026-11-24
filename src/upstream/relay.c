/* 업스트림 시도 한 번의 connect, send, 응답 머리 수신. */
#include "upstream/relay.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>

#include "core/platform.h"
#include "upstream/internal_state.h"
#include "upstream/response_parser.h"

typedef enum { CONNECTING, SENDING, READING, FINISHED, FAILED } relay_phase_t;

struct relay {
    proxy_job_t *job;
    const http_limits_t *lim;
    upstream_conn_t *conn;
    http_response_t resp;
    relay_phase_t phase;
    proxy_fail_t why;
    int connect_ms, response_ms;
    int64_t deadline;
    size_t offset;
};

static relay_status_t fail(relay_t *r, proxy_fail_t why, bool endpoint_failure)
{
    if (endpoint_failure && r->conn != NULL) {
        upstream_report(r->conn->backend, false, platform_now_ms());
    }
    upstream_release(r->conn, false);
    r->conn = NULL;
    r->why = why;
    r->phase = FAILED;
    return RELAY_FAIL;
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
    r->connect_ms = connect_timeout_ms;
    r->response_ms = response_timeout_ms;
    r->deadline = now_ms + connect_timeout_ms;
    /* 직렬화 전인 요청은 먼저 대상을 예약한다. 이미 직렬화된 요청은 기존 대상을 유지한다. */
    bool deferred = job->request.len == 0;
    backend_t *selected = NULL;
    if (upstream_checkout(job->group, deferred ? NULL : job->backend,
                          deferred ? job->backend : NULL, &selected, &r->conn) != 0) {
        r->phase = FAILED;
        r->why = PFAIL_CONNECT;
    } else {
        r->phase = r->conn->connecting ? CONNECTING : SENDING;
    }
    if (deferred) {
        job->backend = selected;
    }
    return r;
}

static bool retryable_io(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static relay_status_t read_head(relay_t *r)
{
    buf_t *b = &r->conn->rbuf;
    int ignored;
    for (;;) {
        if (platform_now_ms() >= r->deadline) {
            return fail(r, PFAIL_TIMEOUT, true);
        }
        http_parse_result_t result =
            response_parse_head(b->data, b->len, r->job->method, r->lim, &r->resp, &ignored);
        if (result == HTTP_PARSE_ERROR || (result == HTTP_PARSE_DONE && r->resp.status == 101)) {
            return fail(r, PFAIL_BAD_RESPONSE, true);
        }
        if (result == HTTP_PARSE_DONE) {
            if (r->resp.status >= 200) {
                r->phase = FINISHED;
                return RELAY_OK;
            }
            buf_consume(b, r->resp.head_len);
            continue;
        }
        ssize_t n = buf_read_fd(b, r->conn->fd);
        if (n <= 0) {
            if (n < 0 && retryable_io()) {
                return RELAY_WANT_READ;
            }
            return fail(r, PFAIL_BAD_RESPONSE, n == 0 || errno != ENOMEM);
        }
    }
}

relay_status_t relay_step(relay_t *r, unsigned events, int64_t now_ms)
{
    if (r->phase == FAILED) {
        return RELAY_FAIL;
    }
    if (r->phase == FINISHED) {
        return RELAY_OK;
    }
    if (now_ms >= r->deadline) {
        return fail(r, r->phase == CONNECTING ? PFAIL_CONNECT : PFAIL_TIMEOUT, true);
    }
    if (r->phase == CONNECTING) {
        if (events == 0) {
            return RELAY_WANT_WRITE;
        }
        if (upstream_connect_result(r->conn) != 0) {
            return fail(r, PFAIL_CONNECT, true);
        }
        r->phase = SENDING;
        r->deadline = now_ms + r->response_ms;
    }

    if (r->phase == SENDING) {
        const buf_t *request = &r->job->request;
        if (request->len == 0) {
            return fail(r, PFAIL_SEND, false);
        }
        while (r->offset < request->len) {
            if (platform_now_ms() >= r->deadline) {
                return fail(r, PFAIL_TIMEOUT, true);
            }
            ssize_t n =
                platform_send(r->conn->fd, request->data + r->offset, request->len - r->offset);
            if (n > 0) {
                r->job->sent_any = true;
                r->offset += (size_t)n;
                continue;
            }
            if (n < 0 && retryable_io()) {
                return RELAY_WANT_WRITE;
            }
            /* 바이트를 한 번이라도 보냈다면 TCP EOF로 요청 실행 여부를 알 수 없다. */
            if (r->conn->reused && r->offset == 0) {
                if (upstream_reconnect(r->conn) != 0) {
                    return fail(r, PFAIL_CONNECT,
                                errno != EMFILE && errno != ENFILE && errno != ENOMEM &&
                                    errno != ENOBUFS);
                }
                r->deadline = platform_now_ms() + r->connect_ms;
                r->phase = r->conn->connecting ? CONNECTING : SENDING;
                return RELAY_WANT_WRITE;
            }
            return fail(r, PFAIL_SEND, true);
        }
        r->phase = READING;
        r->deadline = platform_now_ms() + r->response_ms;
    }
    return read_head(r);
}

int relay_fd(const relay_t *r)
{
    return r->conn == NULL ? -1 : r->conn->fd;
}

int64_t relay_deadline_ms(const relay_t *r)
{
    return r->phase == FINISHED || r->phase == FAILED ? -1 : r->deadline;
}

void relay_take(relay_t *r, upstream_conn_t **out_conn, http_response_t *out_resp,
                proxy_fail_t *out_fail)
{
    *out_conn = NULL;
    if (r->phase == FINISHED) {
        *out_conn = r->conn;
        *out_resp = r->resp;
        r->conn = NULL;
    } else {
        *out_fail = r->phase == FAILED ? r->why : PFAIL_CONNECT;
    }
}

void relay_free(relay_t *r)
{
    if (r != NULL) {
        upstream_release(r->conn, false);
        free(r);
    }
}

relay_status_t relay_exchange_blocking(proxy_job_t *job, const http_limits_t *lim,
                                       int connect_timeout_ms, int response_timeout_ms,
                                       upstream_conn_t **out_conn, http_response_t *out_resp,
                                       proxy_fail_t *out_fail)
{
    *out_conn = NULL;
    *out_fail = PFAIL_CONNECT;
    relay_t *r = relay_start(job, lim, platform_now_ms(), connect_timeout_ms, response_timeout_ms);
    if (r == NULL) {
        return RELAY_FAIL;
    }
    unsigned events = 0;
    relay_status_t result;
    for (;;) {
        int64_t now = platform_now_ms();
        result = relay_step(r, events, now);
        if (result != RELAY_WANT_READ && result != RELAY_WANT_WRITE) {
            break;
        }
        int64_t wait = relay_deadline_ms(r) - now;
        struct pollfd pfd = {relay_fd(r), result == RELAY_WANT_READ ? POLLIN : POLLOUT, 0};
        int rc = poll(&pfd, 1, wait <= 0 ? 0 : wait > INT_MAX ? INT_MAX : (int)wait);
        if (rc < 0 && errno != EINTR) {
            result = fail(r, PFAIL_BAD_RESPONSE, false);
            break;
        }
        events = rc > 0 ? 1u : 0u;
    }
    relay_take(r, out_conn, out_resp, out_fail);
    relay_free(r);
    return result;
}
