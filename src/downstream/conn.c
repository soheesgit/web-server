/* 클라이언트 연결의 blocking 요청 처리. */
#include "downstream/conn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/http_framing.h"
#include "common/http_headers.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/stub.h"
#include "downstream/request_parser.h"
#include "downstream/response_writer.h"
#include "handler/handler.h"
#include "handler/plan.h"
#include "upstream/relay.h"

enum { CONN_IO_CHUNK = 16384, CONN_WRITE_CAP = 128 * 1024 };

struct conn {
    int fd;
    const conn_env_t *env;
    char ip[INET6_ADDRSTRLEN];
    uint16_t port;
    int64_t deadline;
    bool finished;
    bool watch_peer_input;
    bool upstream_reusable;
    buf_t input;
    buf_t output;
    response_plan_t plan;
    proxy_job_t job;
};

static int64_t after_ms(int64_t now, int ms)
{
    if (now < 0 || ms <= 0) {
        return now;
    }
    return now <= INT64_MAX - ms ? now + ms : INT64_MAX;
}

static int64_t earlier(int64_t a, int64_t b)
{
    if (a < 0) {
        return b;
    }
    return b >= 0 && b < a ? b : a;
}

static int poll_until(struct pollfd *fds, nfds_t n, int64_t deadline)
{
    for (;;) {
        int timeout = -1;
        if (deadline >= 0) {
            int64_t now = platform_now_ms();
            if (now < 0) {
                return -1;
            }
            if (now >= deadline) {
                errno = ETIMEDOUT;
                return -1;
            }
            int64_t left = deadline - now;
            timeout = left > INT_MAX ? INT_MAX : (int)left;
        }
        int ready = poll(fds, n, timeout);
        if (ready > 0) {
            return 0;
        }
        /* EINTR과 플랫폼의 poll 대기 상한 모두 남은 시각으로 다시 계산한다. */
        if (ready < 0 && errno != EINTR) {
            return -1;
        }
    }
}

static int wait_socket(int fd, short events, int64_t deadline)
{
    struct pollfd watched = {.fd = fd, .events = events};
    return poll_until(&watched, 1, deadline);
}

static bool would_wait(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static void set_address(conn_t *c, const struct sockaddr_storage *peer)
{
    const void *address = NULL;
    if (peer != NULL && peer->ss_family == AF_INET) {
        address = &((const struct sockaddr_in *)peer)->sin_addr;
    } else if (peer != NULL && peer->ss_family == AF_INET6) {
        address = &((const struct sockaddr_in6 *)peer)->sin6_addr;
    }
    if (address == NULL || inet_ntop(peer->ss_family, address, c->ip, sizeof(c->ip)) == NULL) {
        memcpy(c->ip, "-", 2);
    }

    struct sockaddr_storage local;
    socklen_t length = sizeof(local);
    if (getsockname(c->fd, (struct sockaddr *)&local, &length) == 0 && local.ss_family == AF_INET &&
        length >= sizeof(struct sockaddr_in)) {
        c->port = ntohs(((const struct sockaddr_in *)&local)->sin_port);
    } else {
        c->port = c->env->cfg->port;
    }
}

conn_t *conn_new(int fd, const struct sockaddr_storage *peer, const conn_env_t *env, int64_t now_ms)
{
    const http_limits_t *lim = env->lim;
    if (lim->max_head == SIZE_MAX || lim->max_body > SIZE_MAX - lim->max_head - 1) {
        errno = EOVERFLOW;
        return NULL;
    }

    conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->fd = fd;
    c->env = env;
    c->deadline = after_ms(now_ms, env->cfg->header_timeout_ms);
    plan_init(&c->plan);
    if (buf_init(&c->input, 0, lim->max_head + (size_t)lim->max_body + 1) < 0 ||
        buf_init(&c->output, 0, CONN_WRITE_CAP) < 0) {
        goto failed;
    }

    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        goto failed;
    }
    if (!env->nonblocking) {
        const struct timeval period = {.tv_usec = 100000};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &period, sizeof(period)) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &period, sizeof(period)) < 0) {
            goto failed;
        }
    }
    int mode = env->nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
    if (mode != flags && fcntl(fd, F_SETFL, mode) < 0) {
        goto failed;
    }
    set_address(c, peer);
    return c;

failed:
    c->fd = -1; /* 실패하면 호출자가 소켓을 계속 소유한다. */
    conn_free(c);
    return NULL;
}

void conn_free(conn_t *c)
{
    if (c == NULL) {
        return;
    }
    int saved = errno;
    plan_cleanup(&c->plan);
    proxy_job_cleanup(&c->job);
    buf_cleanup(&c->input);
    buf_cleanup(&c->output);
    if (c->fd >= 0) {
        (void)close(c->fd);
    }
    free(c);
    errno = saved;
}

static ssize_t receive_client(conn_t *c)
{
    for (;;) {
        if (wait_socket(c->fd, POLLIN, c->deadline) < 0) {
            return -1;
        }
        ssize_t n = buf_read_fd(&c->input, c->fd);
        if (n >= 0 || !would_wait()) {
            return n;
        }
    }
}

/* 0은 요청 완료, -1은 응답 없이 종료, 양수는 보낼 오류 상태다. */
static int read_request(conn_t *c, int64_t head_deadline, http_request_t *req)
{
    const http_limits_t *lim = c->env->lim;
    int status;
    http_parse_result_t parsed;

    do {
        parsed = request_parse_head(c->input.data, c->input.len, lim, req, &status);
        if (parsed == HTTP_PARSE_ERROR) {
            return status;
        }
        if (parsed == HTTP_PARSE_INCOMPLETE) {
            ssize_t n = receive_client(c);
            if (n < 0) {
                return errno == ETIMEDOUT && c->input.len != 0 ? 408 : -1;
            }
            if (n == 0) {
                return c->input.len != 0 ? 400 : -1;
            }
            c->deadline = head_deadline;
        }
    } while (parsed == HTTP_PARSE_INCOMPLETE);

    if (req->framing.kind != FRAMING_LENGTH || req->framing.length == 0) {
        return 0;
    }

    size_t length = (size_t)req->framing.length;
    size_t total = req->head_len + length;
    if (c->input.len < total) {
        int64_t now = platform_now_ms();
        if (now < 0) {
            return -1;
        }
        c->deadline = after_ms(now, c->env->cfg->body_timeout_ms);
        do {
            ssize_t n = receive_client(c);
            if (n < 0) {
                return errno == ETIMEDOUT ? 408 : -1;
            }
            if (n == 0) {
                return 400;
            }
        } while (c->input.len < total);
        /* 버퍼 재할당 뒤 빌리는 뷰를 다시 만든다. */
        if (request_parse_head(c->input.data, c->input.len, lim, req, &status) != HTTP_PARSE_DONE) {
            return 400;
        }
    }
    req->body = (str_t){c->input.data + req->head_len, length};
    return 0;
}

static int send_bytes(conn_t *c, const void *data, size_t len, int64_t hard_deadline,
                      uint64_t *sent)
{
    const char *p = data;
    int stall = c->env->cfg->body_timeout_ms;
    int64_t now = platform_now_ms();
    if (len != 0 && now < 0) {
        return -1;
    }
    c->deadline = earlier(after_ms(now, stall), hard_deadline);

    while (len != 0) {
        if (wait_socket(c->fd, POLLOUT, c->deadline) < 0) {
            return -1;
        }
        size_t batch = len < CONN_IO_CHUNK ? len : CONN_IO_CHUNK;
        ssize_t n = platform_send(c->fd, p, batch);
        if (n > 0) {
            if (sent != NULL) {
                *sent += (uint64_t)n;
            }
            p += n;
            len -= (size_t)n;
            if (len != 0) {
                now = platform_now_ms();
                if (now < 0) {
                    return -1;
                }
                c->deadline = earlier(after_ms(now, stall), hard_deadline);
            }
        } else if (n == 0 || !would_wait()) {
            return -1;
        }
    }
    return 0;
}

static int send_file(conn_t *c, uint64_t *sent)
{
    int stall = c->env->cfg->body_timeout_ms;
    off_t position = c->plan.body.file.off;
    off_t left = c->plan.body.file.len;
    int64_t now = platform_now_ms();
    if (now < 0 || position < 0) {
        return -1;
    }
    c->deadline = after_ms(now, stall);

    while (left > 0) {
        if (wait_socket(c->fd, POLLOUT, c->deadline) < 0) {
            return -1;
        }
        off_t before = position;
        size_t count = left < CONN_IO_CHUNK ? (size_t)left : CONN_IO_CHUNK;
        ssize_t result = platform_sendfile(c->fd, c->plan.body.file.fd, &position, count);
        off_t progress = position - before;
        if (progress > 0) {
            left -= progress;
            *sent += (uint64_t)progress;
            now = platform_now_ms();
            if (now < 0) {
                return -1;
            }
            c->deadline = after_ms(now, stall);
        }
        if (result == 0 || (result < 0 && !would_wait())) {
            return -1;
        }
    }
    return 0;
}

/* 읽기를 마친 클라이언트가 끊어졌는지 확인한다. 다음 요청은 소비하지 않는다. */
static bool peer_gone(conn_t *c, short events)
{
    if (events & (POLLERR | POLLNVAL | POLLHUP)) {
        return true;
    }
    if (events & POLLIN) {
        char byte;
        ssize_t n = recv(c->fd, &byte, sizeof(byte), MSG_PEEK);
        if (n > 0) {
            c->watch_peer_input = false;
        }
        return n == 0 || (n < 0 && !would_wait());
    }
    return false;
}

static struct pollfd watch_client(const conn_t *c)
{
    return (struct pollfd){.fd = c->fd, .events = c->watch_peer_input ? POLLIN : 0};
}

static void finish_cgi_input(response_plan_t *plan)
{
    if (plan->body.pipe.in_fd >= 0) {
        (void)close(plan->body.pipe.in_fd);
        plan->body.pipe.in_fd = -1;
    }
}

static int write_cgi_input(response_plan_t *plan)
{
    buf_t *input = &plan->body.pipe.in_data;
    size_t limit = input->len < CONN_IO_CHUNK ? input->len : CONN_IO_CHUNK;
    ssize_t n = write(plan->body.pipe.in_fd, input->data, limit);
    if (n > 0) {
        buf_consume(input, (size_t)n);
        if (input->len == 0) {
            finish_cgi_input(plan);
        }
    } else if (n < 0 && errno == EPIPE) {
        finish_cgi_input(plan);
    } else if (n == 0 || !would_wait()) {
        return -1;
    }
    return 0;
}

static ssize_t read_cgi_output(conn_t *c, void *out, size_t size)
{
    response_plan_t *plan = &c->plan;
    c->deadline = plan->body.pipe.deadline_ms;

    for (;;) {
        struct pollfd fds[] = {watch_client(c),
                               {.fd = plan->body.pipe.out_fd, .events = POLLIN},
                               {.fd = plan->body.pipe.in_fd, .events = POLLOUT}};
        if (poll_until(fds, 3, c->deadline) < 0) {
            return -1;
        }
        if (peer_gone(c, fds[0].revents)) {
            errno = ECONNRESET;
            return -1;
        }
        if (fds[2].revents != 0 && write_cgi_input(plan) < 0) {
            return -1;
        }
        if (fds[1].revents != 0) {
            ssize_t n = read(plan->body.pipe.out_fd, out, size);
            if (n >= 0 || !would_wait()) {
                return n;
            }
        }
    }
}

/* 완료 0, 실행 중 1, 비정상 종료 -1. */
static int check_cgi_exit(response_plan_t *plan)
{
    if (plan->body.pipe.pid <= 0) {
        return 0;
    }
    int status;
    pid_t child;
    do {
        child = waitpid(plan->body.pipe.pid, &status, WNOHANG);
    } while (child < 0 && errno == EINTR);
    if (child == 0) {
        return 1;
    }
    if (child > 0 || errno == ECHILD) {
        plan->body.pipe.pid = 0;
    }
    return child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int cgi_failure(void)
{
    if (errno == ECONNRESET) {
        return -1;
    }
    return errno == ETIMEDOUT ? 504 : 502;
}

static int prepare_cgi(conn_t *c, int minor, buf_t *buffered)
{
    response_plan_t *plan = &c->plan;
    if (plan->body.pipe.in_data.len == 0) {
        finish_cgi_input(plan);
    }
    if (platform_set_nonblock(plan->body.pipe.out_fd) < 0 ||
        (plan->body.pipe.in_fd >= 0 && platform_set_nonblock(plan->body.pipe.in_fd) < 0)) {
        return 502;
    }
    if (!plan->head_pending) {
        return 0;
    }

    size_t max_head = c->env->cfg->cgi_max_head;
    if (max_head > SIZE_MAX - CONN_IO_CHUNK ||
        buf_init(buffered, 0, max_head + CONN_IO_CHUNK) < 0) {
        return 502;
    }
    http_head_out_t head = {0};
    for (;;) {
        char bytes[CONN_IO_CHUNK];
        ssize_t n = read_cgi_output(c, bytes, sizeof(bytes));
        if (n <= 0 || buf_append(buffered, bytes, (size_t)n) < 0) {
            int status = n < 0 ? cgi_failure() : 502;
            buf_cleanup(&head.fields);
            return status;
        }
        size_t used;
        int status;
        http_parse_result_t parsed =
            cgi_parse_head(buffered->data, buffered->len, max_head, &head, &used, &status);
        if (parsed == HTTP_PARSE_DONE && check_cgi_exit(plan) >= 0) {
            head.close = head.close || plan->head.close;
            if (head.framing.kind == FRAMING_NONE && head.status >= 200 && head.status != 204 &&
                head.status != 304) {
                head.framing.kind = minor == 0 ? FRAMING_CLOSE : FRAMING_CHUNKED;
            }
            buf_cleanup(&plan->head.fields);
            plan->head = head;
            plan->head_pending = false;
            buf_consume(buffered, used);
            return 0;
        }
        if (parsed != HTTP_PARSE_INCOMPLETE || buffered->len >= max_head) {
            buf_cleanup(&head.fields);
            return 502;
        }
    }
}

static int finish_cgi_output(conn_t *c)
{
    finish_cgi_input(&c->plan);
    int64_t deadline = c->plan.body.pipe.deadline_ms;
    int status;
    while ((status = check_cgi_exit(&c->plan)) > 0) {
        int64_t now = platform_now_ms();
        if (now < 0 || (deadline >= 0 && now >= deadline)) {
            return -1;
        }
        struct pollfd peer = watch_client(c);
        if (poll_until(&peer, 1, earlier(deadline, after_ms(now, 20))) == 0) {
            if (peer_gone(c, peer.revents)) {
                return -1;
            }
        } else if (errno != ETIMEDOUT) {
            return -1;
        }
    }
    return status;
}

static int send_cgi(conn_t *c, const buf_t *buffered, uint64_t *sent)
{
    http_framing_t framing = c->plan.head.framing;
    int64_t deadline = c->plan.body.pipe.deadline_ms;
    char bytes[CONN_IO_CHUNK];
    const char *data = buffered->data;
    size_t size = buffered->len;

    for (;;) {
        if (framing.kind == FRAMING_LENGTH && (uint64_t)size > framing.length - *sent) {
            return -1;
        }
        if (size != 0 && framing.kind == FRAMING_CHUNKED) {
            buf_reset(&c->output);
            if (response_write_chunk(data, size, &c->output) < 0 ||
                send_bytes(c, c->output.data, c->output.len, deadline, NULL) < 0) {
                return -1;
            }
            *sent += size;
        } else if (send_bytes(c, data, size, deadline, sent) < 0) {
            return -1;
        }
        ssize_t n = read_cgi_output(c, bytes, sizeof(bytes));
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            break;
        }
        data = bytes;
        size = (size_t)n;
    }

    if (finish_cgi_output(c) < 0 || (framing.kind == FRAMING_LENGTH && framing.length != *sent)) {
        return -1;
    }
    if (framing.kind == FRAMING_CHUNKED) {
        buf_reset(&c->output);
        if (response_write_last_chunk(&c->output) < 0) {
            return -1;
        }
        return send_bytes(c, c->output.data, c->output.len, deadline, NULL);
    }
    return 0;
}

static int prepare_proxy(conn_t *c, int minor)
{
    const config_t *cfg = c->env->cfg;
    for (;;) {
        upstream_conn_t *upstream = NULL;
        http_response_t response;
        proxy_fail_t reason = PFAIL_BAD_RESPONSE;
        relay_status_t result = relay_exchange_blocking(
            &c->job, c->env->lim, cfg->upstream_connect_timeout_ms,
            cfg->upstream_response_timeout_ms, &upstream, &response, &reason);
        if (result != RELAY_OK) {
            if (proxy_on_fail(&c->job, reason, &c->plan) == PROXY_RETRY) {
                continue;
            }
            return 0;
        }

        if (response.framing.kind == FRAMING_CHUNKED) {
            const http_header_t *coding = http_header_get(&response.headers, "Transfer-Encoding");
            if (minor == 0 || coding == NULL || !str_eq_ci(coding->value, "chunked") ||
                http_header_count(&response.headers, "Transfer-Encoding") != 1) {
                upstream_release(upstream, false);
                return 502;
            }
        }

        if (proxy_on_head(&c->job, upstream, &response, &c->plan) < 0) {
            return 502;
        }
        c->upstream_reusable = response.keep_alive;
        buf_consume(&upstream->rbuf, response.head_len);
        return 0;
    }
}

static ssize_t read_upstream(conn_t *c, upstream_conn_t *upstream)
{
    int64_t now = platform_now_ms();
    if (now < 0) {
        return -1;
    }
    c->deadline = after_ms(now, c->env->cfg->upstream_response_timeout_ms);
    for (;;) {
        struct pollfd fds[] = {watch_client(c), {.fd = upstream->fd, .events = POLLIN}};
        if (poll_until(fds, 2, c->deadline) < 0 || peer_gone(c, fds[0].revents)) {
            return -1;
        }
        if (fds[1].revents != 0) {
            ssize_t n = buf_read_fd(&upstream->rbuf, upstream->fd);
            if (n >= 0 || !would_wait()) {
                return n;
            }
        }
    }
}

static int send_upstream(conn_t *c, uint64_t *sent)
{
    upstream_conn_t *upstream = c->plan.body.upstream.conn;
    http_body_tracker_t *tracker = &c->plan.body.upstream.tracker;
    if (upstream == NULL) {
        return -1;
    }
    buf_t *buffer = &upstream->rbuf;

    for (;;) {
        size_t used;
        http_body_result_t tracked = http_body_track(tracker, buffer->data, buffer->len, &used);
        if (tracked == BODY_ERROR || send_bytes(c, buffer->data, used, -1, sent) < 0) {
            return -1;
        }
        if (tracked == BODY_DONE) {
            upstream_release(upstream, c->upstream_reusable && used == buffer->len);
            c->plan.body.upstream.conn = NULL;
            return 0;
        }
        buf_consume(buffer, used);
        ssize_t n = read_upstream(c, upstream);
        if (n <= 0) {
            return n == 0 && tracker->f.kind == FRAMING_CLOSE ? 0 : -1;
        }
    }
}

static bool body_allowed(http_method_t method, int status)
{
    return method != HTTP_HEAD && status >= 200 && status != 204 && status != 304;
}

static int prepare_response(conn_t *c, const http_request_t *req, int64_t now, buf_t *cgi)
{
    const route_t *route = router_match(c->env->router, req->host, req->path);
    if (route == NULL) {
        return 404;
    }

    handler_ctx_t ctx = {.cfg = c->env->cfg,
                         .lim = c->env->lim,
                         .client_ip = str_from_cstr(c->ip),
                         .server_port = c->port,
                         .now = platform_wall_time(),
                         .now_ms = now};
    if (handler_dispatch(req, route, &ctx, &c->job, &c->plan) == HANDLER_NEED_UPSTREAM) {
        int status = prepare_proxy(c, req->minor);
        if (status != 0) {
            return status;
        }
    }
    if (c->plan.kind == BODY_PIPE) {
        int status = prepare_cgi(c, req->minor, cgi);
        if (status != 0) {
            return status;
        }
    }

    const http_head_out_t *head = &c->plan.head;
    if (head->status < 100 || head->status > 599) {
        return 500;
    }
    if (body_allowed(req->method, head->status) && head->framing.kind == FRAMING_CHUNKED &&
        c->plan.kind != BODY_PIPE && c->plan.kind != BODY_UPSTREAM) {
        return 500;
    }
    if (body_allowed(req->method, head->status) && head->framing.kind == FRAMING_LENGTH) {
        if ((c->plan.kind == BODY_NONE && head->framing.length != 0) ||
            (c->plan.kind == BODY_MEM && head->framing.length != c->plan.body.mem.data.len) ||
            (c->plan.kind == BODY_FILE &&
             (c->plan.body.file.len < 0 ||
              head->framing.length != (uint64_t)c->plan.body.file.len))) {
            return 500;
        }
        if (c->plan.kind == BODY_PIPE && (uint64_t)cgi->len > head->framing.length) {
            return 502;
        }
    }
    return 0;
}

static int send_response(conn_t *c, const http_request_t *req, const buf_t *cgi, uint64_t *sent)
{
    int64_t hard_deadline = c->plan.kind == BODY_PIPE ? c->plan.body.pipe.deadline_ms : -1;
    buf_reset(&c->output);
    if (response_write_head(&c->plan.head, req->minor, platform_wall_time(), &c->output) < 0 ||
        send_bytes(c, c->output.data, c->output.len, hard_deadline, NULL) < 0) {
        return -1;
    }

    if (!body_allowed(req->method, c->plan.head.status)) {
        if (c->plan.kind == BODY_UPSTREAM && c->plan.body.upstream.tracker.f.kind == FRAMING_NONE) {
            return send_upstream(c, sent);
        }
        return 0;
    }

    switch (c->plan.kind) {
    case BODY_NONE:
        return 0;
    case BODY_MEM:
        return send_bytes(c, c->plan.body.mem.data.data, c->plan.body.mem.data.len, -1, sent);
    case BODY_FILE:
        return send_file(c, sent);
    case BODY_PIPE:
        return send_cgi(c, cgi, sent);
    case BODY_UPSTREAM:
        return send_upstream(c, sent);
    }
    return -1;
}

static char *copy_for_log(str_t value)
{
    if (value.len == 0) {
        return NULL;
    }
    char *copy = malloc(value.len + 1);
    if (copy != NULL) {
        memcpy(copy, value.ptr, value.len);
        copy[value.len] = '\0';
    }
    return copy;
}

static void record_access(const conn_t *c, const http_request_t *req, uint64_t sent, int64_t start)
{
    char *method = copy_for_log(req->method_raw);
    char *target = copy_for_log(req->target);
    int64_t now = platform_now_ms();
    log_access(c->ip, method, target, c->plan.head.status, sent, now >= start ? now - start : 0);
    free(target);
    free(method);
}

static void drain_after_error(conn_t *c)
{
    int64_t now = platform_now_ms();
    if (now < 0 || shutdown(c->fd, SHUT_WR) < 0) {
        return;
    }
    int64_t deadline = after_ms(now, 1000);
    char discarded[CONN_IO_CHUNK];
    while (wait_socket(c->fd, POLLIN, deadline) == 0) {
        ssize_t n = recv(c->fd, discarded, sizeof(discarded), 0);
        if (n == 0 || (n < 0 && !would_wait())) {
            break;
        }
    }
}

/* 현재 요청을 끝까지 처리하고 소켓을 더 사용할 수 있는지 돌려준다. */
static bool serve_request(conn_t *c, int64_t head_deadline, bool last)
{
    http_request_t req = {0};
    buf_t cgi = {0};
    uint64_t sent = 0;
    int64_t start = platform_now_ms();
    if (start < 0) {
        return false;
    }

    int status = read_request(c, head_deadline, &req);
    bool bad_request = status > 0;
    if (status < 0) {
        return false;
    }
    if (bad_request) {
        req = (http_request_t){.method = HTTP_OTHER, .minor = 1};
    } else {
        c->watch_peer_input = true;
        c->upstream_reusable = false;
        status = prepare_response(c, &req, platform_now_ms(), &cgi);
    }

    bool keep = false;
    if (status >= 0 && (status == 0 || plan_error(&c->plan, status) == 0)) {
        http_head_out_t *head = &c->plan.head;
        head->close =
            head->close || bad_request || last || !req.keep_alive ||
            head->framing.kind == FRAMING_CLOSE ||
            (req.minor == 0 && head->framing.kind == FRAMING_CHUNKED) ||
            (head->framing.kind == FRAMING_NONE && body_allowed(req.method, head->status));
        bool sent_all = send_response(c, &req, &cgi, &sent) == 0;
        record_access(c, &req, sent, start);
        if (sent_all && bad_request) {
            drain_after_error(c);
        }
        keep = sent_all && !head->close;
        if (keep) {
            buf_consume(&c->input, req.head_len + req.body.len);
        }
    }

    buf_cleanup(&cgi);
    proxy_job_cleanup(&c->job);
    plan_cleanup(&c->plan);
    return keep;
}

void conn_serve_blocking(conn_t *c)
{
    if (c->env->nonblocking || c->finished) {
        return;
    }

    const config_t *cfg = c->env->cfg;
    int64_t head_deadline = c->deadline;
    for (uint64_t count = 1;; ++count) {
        bool last =
            cfg->max_keepalive_requests > 0 && count >= (uint64_t)cfg->max_keepalive_requests;
        if (!serve_request(c, head_deadline, last)) {
            break;
        }
        int64_t now = platform_now_ms();
        if (now < 0) {
            break;
        }
        head_deadline = after_ms(now, cfg->header_timeout_ms);
        c->deadline = c->input.len == 0 ? after_ms(now, cfg->keepalive_timeout_ms) : head_deadline;
    }
    c->finished = true;
    c->deadline = -1;
}

conn_status_t conn_on_event(conn_t *c, int fd, unsigned events, int64_t now_ms)
{
    UNUSED(c);
    UNUSED(fd);
    UNUSED(events);
    UNUSED(now_ms);
    /* TODO(downstream): 선택 7단계 이벤트 경로. */
    STUB_RETURN(CONN_CLOSE);
}

conn_status_t conn_on_timeout(conn_t *c, int64_t now_ms)
{
    UNUSED(c);
    UNUSED(now_ms);
    /* TODO(downstream): 선택 7단계 이벤트 경로. */
    STUB_RETURN(CONN_CLOSE);
}

int conn_fd(const conn_t *c)
{
    return c->fd;
}

int64_t conn_deadline_ms(const conn_t *c)
{
    return c->deadline;
}
