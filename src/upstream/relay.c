/* src/upstream/relay.c
 *
 * 프록시 I/O 실행. 명세: relay.h
 */
#include "upstream/relay.h"

#include "core/platform.h"
#include "upstream/response_parser.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>

enum { RELAY_EV_READ = 1u, RELAY_EV_WRITE = 2u, RELAY_EV_ERROR = 4u };

typedef enum { R_CONNECT, R_SEND, R_HEAD, R_OK, R_FAIL } relay_step_t;

struct relay {
    proxy_job_t *job;
    http_limits_t lim;
    upstream_conn_t *conn;
    http_response_t resp;
    proxy_fail_t fail;
    relay_status_t result;
    relay_step_t step;
    int64_t deadline;
    int connect_timeout;
    int response_timeout;
    size_t sent;
    bool retried_reused;
    bool transferred;
};

static void relay_fail(relay_t *r, proxy_fail_t why)
{
    if (r->conn != NULL) {
        upstream_release(r->conn, false);
        r->conn = NULL;
    }
    r->fail = why;
    r->step = R_FAIL;
    r->result = RELAY_FAIL;
}

static int relay_reopen_reused(relay_t *r)
{
    backend_t *backend = r->job->backend;
    upstream_release(r->conn, false);
    r->conn = NULL;
    r->job->backend = backend;
    r->job->sent_any = false;
    r->sent = 0;
    r->retried_reused = true;
    if (upstream_acquire(r->job->group, backend, &r->conn) != 0)
        return -1;
    r->step = r->conn->connecting ? R_CONNECT : R_SEND;
    r->deadline = platform_now_ms() + r->connect_timeout;
    return 0;
}

relay_t *relay_start(proxy_job_t *job, const http_limits_t *lim, int64_t now_ms,
                     int connect_timeout_ms, int response_timeout_ms)
{
    relay_t *r;
    if (job == NULL || lim == NULL) {
        errno = EINVAL;
        return NULL;
    }
    r = calloc(1, sizeof(*r));
    if (r == NULL)
        return NULL;
    r->job = job;
    r->lim = *lim;
    r->connect_timeout = connect_timeout_ms;
    r->response_timeout = response_timeout_ms;
    r->step = R_CONNECT;
    r->result = RELAY_WANT_WRITE;
    r->fail = PFAIL_CONNECT;
    r->deadline = now_ms + connect_timeout_ms;
    if (upstream_acquire(job->group, job->backend, &r->conn) != 0) {
        r->step = R_FAIL;
        r->result = RELAY_FAIL;
    } else if (!r->conn->connecting) {
        r->step = R_SEND;
    }
    return r;
}

relay_status_t relay_step(relay_t *r, unsigned events, int64_t now_ms)
{
    if (r == NULL)
        return RELAY_FAIL;
    if (r->step == R_OK || r->step == R_FAIL)
        return r->result;
    if (now_ms >= r->deadline) {
        relay_fail(r, r->step == R_CONNECT ? PFAIL_CONNECT
                                           : (r->step == R_SEND ? PFAIL_SEND : PFAIL_TIMEOUT));
        return r->result;
    }
    if (r->step == R_CONNECT) {
        int rc;
        if (r->conn == NULL) {
            relay_fail(r, PFAIL_CONNECT);
            return r->result;
        }
        if (r->conn->connecting && events == 0)
            return RELAY_WANT_WRITE;
        if (r->conn->connecting && (events & (RELAY_EV_WRITE | RELAY_EV_ERROR)) == 0)
            return RELAY_WANT_WRITE;
        rc = upstream_connect_result(r->conn);
        if (rc != 0) {
            relay_fail(r, PFAIL_CONNECT);
            return r->result;
        }
        r->step = R_SEND;
    }
    if (r->step == R_SEND) {
        while (r->sent < r->job->request.len) {
            ssize_t n = platform_send(r->conn->fd, r->job->request.data + r->sent,
                                      r->job->request.len - r->sent);
            if (n > 0) {
                r->sent += (size_t)n;
                r->job->sent_any = true;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return RELAY_WANT_WRITE;
            if (r->conn->reused && !r->retried_reused && r->sent == 0 &&
                relay_reopen_reused(r) == 0)
                return RELAY_WANT_WRITE;
            relay_fail(r, PFAIL_SEND);
            return r->result;
        }
        r->step = R_HEAD;
        r->deadline = now_ms + r->response_timeout;
    }
    if (r->step == R_HEAD) {
        int status = 502;
        http_parse_result_t pr = response_parse_head(r->conn->rbuf.data, r->conn->rbuf.len,
                                                     r->job->method, &r->lim, &r->resp, &status);
        if (pr == HTTP_PARSE_DONE) {
            if (r->resp.status >= 100 && r->resp.status < 200) {
                buf_consume(&r->conn->rbuf, r->resp.head_len);
                r->step = R_HEAD;
                r->deadline = now_ms + r->response_timeout;
            } else {
                r->step = R_OK;
                r->result = RELAY_OK;
                return r->result;
            }
        } else if (pr == HTTP_PARSE_ERROR) {
            relay_fail(r, PFAIL_BAD_RESPONSE);
            return r->result;
        }
        if (events == 0 || (events & (RELAY_EV_READ | RELAY_EV_ERROR)) == 0)
            return RELAY_WANT_READ;
        {
            ssize_t n = buf_read_fd(&r->conn->rbuf, r->conn->fd);
            if (n > 0)
                return relay_step(r, RELAY_EV_READ, platform_now_ms());
            if (n == 0) {
                /* D-036: 요청을 이미 다 보낸 뒤라 백엔드가 처리했을 수 있다. 멱등한 메서드만 다시
                 * 보낸다. */
                if (r->conn->reused && !r->retried_reused && r->conn->rbuf.len == 0 &&
                    (r->job->method == HTTP_GET || r->job->method == HTTP_HEAD) &&
                    relay_reopen_reused(r) == 0)
                    return RELAY_WANT_WRITE;
                relay_fail(r, PFAIL_BAD_RESPONSE);
                return r->result;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return RELAY_WANT_READ;
            relay_fail(r, PFAIL_BAD_RESPONSE);
            return r->result;
        }
    }
    return r->result;
}

int relay_fd(const relay_t *r)
{
    return r == NULL || r->conn == NULL ? -1 : r->conn->fd;
}

int64_t relay_deadline_ms(const relay_t *r)
{
    return r == NULL || r->step == R_OK || r->step == R_FAIL ? -1 : r->deadline;
}

void relay_take(relay_t *r, upstream_conn_t **out_conn, http_response_t *out_resp,
                proxy_fail_t *out_fail)
{
    if (out_conn != NULL)
        *out_conn = NULL;
    if (out_fail != NULL)
        *out_fail = PFAIL_CONNECT;
    if (r == NULL || out_conn == NULL || out_resp == NULL || out_fail == NULL)
        return;
    if (r->step == R_OK) {
        *out_conn = r->conn;
        *out_resp = r->resp;
        r->conn = NULL;
        r->transferred = true;
        *out_fail = PFAIL_CONNECT;
    } else if (r->step == R_FAIL) {
        *out_fail = r->fail;
    }
}

void relay_free(relay_t *r)
{
    int saved = errno;
    if (r != NULL) {
        if (r->conn != NULL)
            upstream_release(r->conn, false);
        free(r);
    }
    errno = saved;
}

relay_status_t relay_exchange_blocking(proxy_job_t *job, const http_limits_t *lim,
                                       int connect_timeout_ms, int response_timeout_ms,
                                       upstream_conn_t **out_conn, http_response_t *out_resp,
                                       proxy_fail_t *out_fail)
{
    relay_t *r;
    relay_status_t st;
    if (out_conn == NULL || out_resp == NULL || out_fail == NULL) {
        errno = EINVAL;
        return RELAY_FAIL;
    }
    *out_conn = NULL;
    r = relay_start(job, lim, platform_now_ms(), connect_timeout_ms, response_timeout_ms);
    if (r == NULL) {
        *out_fail = PFAIL_CONNECT;
        return RELAY_FAIL;
    }
    for (;;) {
        struct pollfd pfd;
        int64_t now = platform_now_ms(), remain;
        st = relay_step(r, 0, now);
        if (st == RELAY_OK || st == RELAY_FAIL)
            break;
        remain = relay_deadline_ms(r) - now;
        if (remain < 0)
            remain = 0;
        pfd.fd = relay_fd(r);
        pfd.events = st == RELAY_WANT_WRITE ? POLLOUT : POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, remain > INT_MAX ? INT_MAX : (int)remain) < 0 && errno != EINTR) {
            relay_fail(r, r->step == R_CONNECT
                              ? PFAIL_CONNECT
                              : (r->step == R_SEND ? PFAIL_SEND : PFAIL_BAD_RESPONSE));
            st = RELAY_FAIL;
            break;
        }
        if (pfd.revents == 0) {
            st = relay_step(r, 0, platform_now_ms());
            if (st == RELAY_FAIL)
                break;
        } else {
            unsigned ev = 0;
            if (pfd.revents & (POLLIN | POLLHUP))
                ev |= RELAY_EV_READ;
            if (pfd.revents & POLLOUT)
                ev |= RELAY_EV_WRITE;
            if (pfd.revents & (POLLERR | POLLNVAL))
                ev |= RELAY_EV_ERROR;
            st = relay_step(r, ev, platform_now_ms());
            if (st == RELAY_OK || st == RELAY_FAIL)
                break;
        }
    }
    relay_take(r, out_conn, out_resp, out_fail);
    relay_free(r);
    return st;
}
